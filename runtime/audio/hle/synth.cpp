#include "audio/hle/synth.hpp"
#include "audio/hle/voice_kernel.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace f3rt::hle {
namespace {
constexpr uint32_t native_rate = 15238090u / (16u * 32u);
constexpr double rate_ratio = double(native_rate) / sample_rate;
constexpr unsigned volume_ramp = sample_rate * 12 / 1000;
constexpr unsigned filter_ramp = sample_rate * 3 / 1000;
int16_t s16(uint32_t x) { return int16_t(uint16_t(x)); }
int8_t s8(uint32_t x) { return int8_t(uint8_t(x)); }
uint16_t word(const uint8_t *p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
uint32_t longword(const uint8_t *p) { return (uint32_t(word(p)) << 16) | word(p + 2); }
uint16_t parameter_word(const Channel &c, unsigned i) { return word(c.parameters.data() + i); }
int32_t wrap_product(int64_t x) { return int32_t(uint32_t(x)); }
uint16_t volume_target(int value, int threshold) {
    value = s16(value - threshold);
    return value <= 0 ? 0x0ff0 : uint16_t(std::min(0xfff0, value * 48 + 0xff0));
}
struct Split {
    int16_t tuning = 0;
    uint32_t accumulator = 0, start = 0, end = 0;
    uint8_t cutoff = 0, mode = 0;
    static Split read(const uint8_t *p) {
        return {s16(word(p)), longword(p+2), longword(p+6), longword(p+10), p[5], p[9]};
    }
};
struct Envelope {
    uint32_t position = 0, target = 0;
    int32_t slope = 0;
    std::array<uint16_t,4> levels{};
    std::array<uint8_t,4> rates{};
    uint8_t flags = 0, stage = 0;
    bool intermediate = false, done = false;
    int16_t level() const { return s16(position >> 16); }
    bool advance() {
        position += uint32_t(slope);
        return int32_t(position - target) >= 0;
    }
};
struct Filter {
    std::array<float,4> state{}, previous{};
    double a1 = 1, a2 = 1, target1 = 1, target2 = 1;
    double high_pole = 1, target_high_pole = 1;
    unsigned ramp = 0;
    uint8_t mode = 3;
    uint16_t last_k1 = 0, last_k2 = 0;
    void targets(uint16_t k1, uint16_t k2, bool initial) {
        if (!initial && k1 == last_k1 && k2 == last_k2) return;
        last_k1 = k1; last_k2 = k2;
        // Preserve native pole positions in continuous time, not device
        // register/clock execution. Quantization masks come from ROM's format.
        target1 = 1 - std::pow(1 - double(k1 & 0xfff0) / 65536, rate_ratio);
        target2 = 1 - std::pow(1 - double(k2 & 0xfff0) / 65536, rate_ratio);
        // Native highpass pole is (1+k/65536)/2. Converting its
        // pole rather than cutoff-Hz guesses keeps low-k differences.
        target_high_pole = std::pow((1 + double(k2 & 0xfff0) / 65536) * 0.5, rate_ratio);
        if (initial) { a1 = target1; a2 = target2; high_pole = target_high_pole; ramp = 0; }
        else ramp = filter_ramp;
    }
    void advance() {
        if (ramp) {
            a1 += (target1 - a1) / ramp;
            a2 += (target2 - a2) / ramp;
            high_pole += (target_high_pole - high_pole) / ramp;
            --ramp;
        }
    }
};
}

struct Synth::Impl {
    struct Bank { uint8_t tag = 0, count = 0; uint32_t offset = 0, probe = 0; };
    struct ProgramData {
        const uint8_t *base = nullptr;
        std::array<int8_t,17> sample_banks{};
        uint8_t enabled = 0;
    };
    // Volume ramp gain, stepped linearly while the ramp stays inside one gain-table interval.
    struct GainSegment { double value = 0, slope = 0; unsigned frames = 0; };
    struct Voice {
        Note owner{};
        const uint8_t *patch = nullptr;
        Split split{};
        std::array<Envelope,3> envelope{};
        Filter filter{};
        double position = 0, start = 0, end = 0, increment = 0;
        double left = 0, right = 0, target_left = 0, target_right = 0;
        float left_gain = 0, right_gain = 0;
        bool gain_dirty = true;
        uint64_t serial = 0;
        uint16_t frequency = 0, left_volume = 0, right_volume = 0, k1 = 0, k2 = 0;
        int16_t lfo = 0, lfo_depth = 0, lfo_target = 0, lfo_step = 0;
        uint16_t lfo_phase = 0, random_countdown = 0, random_period = 0;
        int16_t random = 0, previous_random = 0, pitch = 0, glide = 0, glide_target = 0, glide_destination = 0;
        uint8_t layer = 0, selector = 14, pair = 0, priority = 0, allocation = 0;
        uint8_t effective_key = 0, weight = 127, pressure = 0;
        unsigned gain_frames = 0;
        double step_left = 0, step_right = 0;
        GainSegment left_segment{}, right_segment{};
        bool active = false, released = false, direct = false, reverse = false;
        bool loop = false, bidirectional = false, boundary = false, sample_finished = false, hardware_valid = false;
    };
    struct Pending { Note note{}; const uint8_t *program = nullptr; const uint8_t *patch = nullptr;
        uint64_t tick = 0; uint8_t layer = 0; bool active = false, release_trigger = false; };
    std::span<const uint8_t> rom;
    std::span<const uint16_t> samples;
    std::vector<uint8_t> metadata;
    std::array<Bank,16> banks{};
    std::array<Voice,32> voices{};
    std::array<Pending,96> pending{};
    std::array<ProgramData,256> programs{};
    std::array<uint8_t,32> next{}, voice_prev{};
    std::array<uint16_t,128> rates{};
    std::array<uint16_t,256> phase_rates{};
    std::array<uint16_t,2048> filters{};
    std::array<uint16_t,768> pitches{};
    std::array<uint8_t,128> attenuation{}, pan{};
    std::array<uint8_t,512> velocity_curves{};
    std::array<float,256> gain{};
    std::array<uint8_t,12> budgets{};
    std::function<void(const VoiceEvent &)> observer;
    struct BufferedEvent { size_t frame = 0; unsigned voice = 0; VoiceEvent event; };
    std::vector<BufferedEvent> buffered_events;
    std::vector<uint64_t> frame_ticks;
    bool buffering_events = false;
    size_t current_frame = 0;
    unsigned current_voice = 0;
    uint64_t tick = 0, timer = timer_ticks, serial = 0;
    uint32_t tick_remainder = 0;
    uint16_t noise = 0;
    uint16_t noise_history = 0;
    uint8_t cursor = 0, budget_slot = 0, bend_range = 0;
    uint32_t reset_start = 0, reset_end = 0;
    alignas(64) float input[voice_block_frames * voice_lane_stride]{};
    alignas(64) float a1[voice_block_frames * voice_lane_stride]{};
    alignas(64) float a2[voice_block_frames * voice_lane_stride]{};
    alignas(64) float high_pole[voice_block_frames * voice_lane_stride]{};
    alignas(64) float gain_left[voice_block_frames * voice_lane_stride]{};
    alignas(64) float gain_right[voice_block_frames * voice_lane_stride]{};
    alignas(64) float out_left[voice_block_frames * voice_lane_stride]{};
    alignas(64) float out_right[voice_block_frames * voice_lane_stride]{};
    alignas(64) float state[4 * 32]{};
    alignas(64) float previous[4 * 32]{};
    alignas(64) uint8_t mode[32]{};
    alignas(64) uint8_t lane_voice[32]{};
    alignas(64) uint8_t lane_pair[32]{};

    Impl(std::span<const uint8_t> r, std::span<const uint16_t> s) : rom(r), samples(s) {
        if (r.size() != 0x80000 || s.size() != 0x800000)
            throw std::runtime_error("Land Maker HLE requires mapped sound and sample ROM regions");
        metadata.reserve(0x10000);
        frame_ticks.reserve(1024);
        buffered_events.reserve(32);
        decode_banks();
        // c15fb4..c15fde heap header overlaps bankc8 sample0. Decode the boot
        // operands rather than assuming sample metadata remains ROM-identical.
        uint32_t heap_base=rw(0xc15fb6);
        uint32_t heap_limit=longword(rom_pointer(0xc15fba,4))+longword(rom_pointer(0xc15fc0,4));
        uint32_t heap_size=heap_limit-heap_base-longword(rom_pointer(0xc15fcc,4));
        uint32_t heap_next=longword(rom_pointer(0xc15fd8,4));
        for(unsigned i=0;i<4;++i){
            metadata.at(heap_base-0x6e9c+i)=uint8_t(heap_size>>(24-i*8));
            metadata.at(heap_base-0x6e9c+4+i)=uint8_t(heap_next>>(24-i*8));
        }
        cache_programs();
        for (unsigned i=0;i<rates.size();++i) rates[i]=rw(0xc089dc+2*i);
        for (unsigned i=0;i<phase_rates.size();++i) phase_rates[i]=rw(0xc08aa4+2*i);
        for (unsigned i=0;i<filters.size();++i) filters[i]=rw(0xc08e6c+2*i);
        for (unsigned i=0;i<pitches.size();++i) pitches[i]=rw(0xc09e6c+2*i);
        bend_range=uint8_t(rw(0xc085f2)); // ROM startup address/value pair cf64,0002.
        reset_start=((uint32_t(rw(0xc17b60))<<16)|rw(0xc17b62))&0x1fffffe0;
        reset_end=((uint32_t(rw(0xc17b64))<<16)|rw(0xc17b66))&0x1fffffe0;
        for (unsigned i=0;i<128;++i) { attenuation[i]=rb(0xc08bec+i); pan[i]=rb(0xc08b6c+i); }
        for (unsigned i=0;i<512;++i) velocity_curves[i]=rb(0xc08c6c+i);
        for (unsigned i=0;i<12;++i) budgets[i]=rb(0xc1a56e + i);
        for (unsigned i=0;i<256;++i) {
            gain[i]=float((((i&15)|16)<<11)>>(16-(i>>4)))/2048;
        }
        reset(0);
    }
    uint8_t rb(uint32_t a) const {
        if (a<0xc00000 || a>=0xc80000) throw std::runtime_error("HLE immutable ROM address out of range");
        return rom[a-0xc00000];
    }
    uint16_t rw(uint32_t a) const { return uint16_t((uint16_t(rb(a))<<8)|rb(a+1)); }
    const uint8_t *rom_pointer(uint32_t a, size_t bytes) const {
        if (a<0xc00000 || bytes>rom.size() || a-0xc00000>rom.size()-bytes)
            throw std::runtime_error("HLE descriptor exceeds sound ROM");
        return rom.data()+a-0xc00000;
    }
    void append_word(uint16_t w) { metadata.push_back(uint8_t(w>>8)); metadata.push_back(uint8_t(w)); }
    void decode_banks() {
        // c0b4a4..c0b610: immutable PCM metadata, including expanded flag word.
        for (unsigned b=0;b<16;++b) {
            unsigned at=0, base=b*0x80000;
            auto sample_word=[&]() {
                if (at>=0x80000) throw std::runtime_error("HLE sample metadata exceeds half-bank");
                return samples[base+at++];
            };
            uint16_t flag=sample_word(); bool packed=(flag&0x8000)!=0;
            auto data_word=[&]() {
                if (!packed) return sample_word();
                uint16_t hi=sample_word(); return uint16_t((hi&0xff00)|(sample_word()>>8));
            };
            Bank &bank=banks[b]; bank.offset=uint32_t(metadata.size()); append_word(flag);
            unsigned last=0; bool valid=true;
            for (;;) {
                uint16_t offset=data_word(); append_word(offset);
                if (!offset) break;
                unsigned limit=packed?offset*2+4:offset+8;
                if (limit<=at*2 || limit%14) { valid=false; break; }
                last=packed?offset*2:offset;
                if (packed) { metadata[metadata.size()-2]=uint8_t((offset+1)>>8); metadata.back()=uint8_t(offset+1); }
                ++bank.count;
                for (unsigned j=0;j<6;++j) append_word(data_word());
            }
            bank.probe=(at*2<<8)|((b&1)<<28);
            if (!valid || !last) { bank.count=0; continue; }
            while (at*2<last) append_word(data_word());
            if (at*2!=last) throw std::runtime_error("HLE split metadata is not aligned");
            for (;;) {
                size_t begin=metadata.size();
                for (unsigned j=0;j<7;++j) append_word(data_word());
                if (metadata[begin+5]==127) break;
            }
            uint16_t tag=samples[base+0x7ffff]; bank.tag=uint8_t(packed?tag>>8:tag);
        }
    }
    void cache_programs() {
        for(unsigned id=0;id<programs.size();++id){
            unsigned index=id,column=0;uint32_t base=0xc00134;
            if(index>=80){base+=0x3fc0;index-=80;if(index>=10){index-=10;++column;}}
            if(index>=40){index-=40;++column;}
            ProgramData &data=programs[id];data.base=rom_pointer(base+index*0x198+column,0x198);
            data.enabled=data.base[0x196]>>5;data.sample_banks.fill(-1);
            auto find_bank=[&](uint8_t tag){
                int8_t result=-2;
                for(unsigned b=0;b<banks.size();++b)if(banks[b].tag==tag)result=int8_t(b);
                return result;
            };
            if(!data.enabled){
                const uint8_t *p=data.base;uint32_t mask=0;
                for(unsigned i=0;i<4;++i)mask=(mask<<8)|p[0x154+2*i];
                if(p[0x15c]=='X'&&p[0x15e]=='E')
                    for(unsigned i=0;i<17;++i)if(mask&(1u<<i))data.sample_banks[i]=find_bank(p[0x160]);
            }else{
                for(unsigned layer=0;layer<3;++layer){
                    const uint8_t *p=data.base+layer*0x76;unsigned marker=0,tag=0;
                    for(unsigned i=0;i<16;i+=2){if(p[i]&128)marker|=1u<<i;if(p[20+i]&128)tag|=1u<<(i/2);}
                    if(marker==0x5515)data.sample_banks[layer]=find_bank(uint8_t(tag));
                }
            }
        }
    }
    const uint8_t *program(uint16_t id) const {
        if(id==0xffff)return nullptr;
        if(id&0x8000)throw std::runtime_error("HLE program selects unpopulated sound ROM bank");
        return programs[id&255].base;
    }
    Split split_for(const uint8_t *patch,unsigned layer,bool direct,int pitch,uint8_t &selector,uint16_t program_id) const {
        unsigned id=(direct?patch[4]:patch[0x6c])&127;
        int bank=programs[program_id&255].sample_banks[layer];
        if(bank==-2)throw std::runtime_error("HLE instrument names missing sample bank");
        const uint8_t *data=nullptr; size_t available=0;
        if(bank>=0){
            const Bank &b=banks[unsigned(bank)];selector=uint8_t(bank);
            if(id>=b.count)throw std::runtime_error("HLE sample ID exceeds external directory");
            size_t offset=b.offset+size_t(int16_t(word(metadata.data()+b.offset+2+id*14)));
            if(offset+14>metadata.size())throw std::runtime_error("HLE sample offset exceeds metadata");
            data=metadata.data()+offset;available=metadata.size()-offset;
        } else {
            selector=14;
            if(id>120)throw std::runtime_error("HLE ROM split ID exceeds table");
            uint32_t address=uint32_t(0xc1ae76+int16_t(rw(0xc1c1e0+id*2)));
            data=rom_pointer(address,14);available=rom.size()-size_t(data-rom.data());
        }
        unsigned key=uint8_t(pitch>>8);
        for(size_t i=0;i+14<=available;i+=14){Split result=Split::read(data+i);if(key<=result.cutoff)return result;}
        throw std::runtime_error("HLE sample splits do not cover key");
    }
    uint16_t frequency(int pitch) const {
        pitch=std::clamp(int(s16(pitch)),-0x4800,0x5400);unsigned shifts=0;uint32_t result;
        if(pitch<0){while(pitch<0){pitch+=0xc00;++shifts;}result=pitches[unsigned(pitch>>2)]>>shifts;}
        else {while(pitch>=0xc00){pitch-=0xc00;++shifts;}result=uint32_t(pitches[unsigned(pitch>>2)])<<shifts;}
        return uint16_t(result)&0xfffe;
    }
    uint16_t rate(int r) const {return rates[unsigned(std::clamp(r,0,99))];}
    void set_glide(Voice &v,int16_t destination,unsigned rate_index) {
        v.glide_destination=destination;
        if(!rate_index){v.glide=0;v.glide_target=destination;return;}
        int difference=s16(destination-v.glide_target);
        v.glide=s16((int64_t(difference)*s16(rw(0xc089dc+2*rate_index))*2)>>16);
        if(!v.glide)v.glide_target=destination;
    }
    void advance_glide(Voice &v) {
        if(!v.glide)return;
        int current=s16(v.glide_target+v.glide);
        if((v.glide>0&&current>=v.glide_destination)||(v.glide<0&&current<=v.glide_destination)){
            current=v.glide_destination;v.glide=0;
        }
        v.glide_target=s16(current);
    }
    void segment(Envelope &e,unsigned target,int r) {
        int difference=s16(target-uint16_t(e.level()));
        if(difference){e.slope=wrap_product(int64_t(difference)*s16(rate(r))*2);e.position&=0xffff0000;}
        else {e.slope=-s16(rate(r));e.position=(e.position&0xffff0000)|0x7fff;}
        e.target=uint32_t(uint16_t(target^(e.slope<=0?0x8000:0)))<<16;
    }
    void initialize_envelope(Envelope &e,const uint8_t *p,const Voice &v,bool amplitude) {
        e={};e.flags=p[16];
        unsigned index=((p[16]&0x30)<<3)+v.owner.velocity;
        int gain_scale=int(p[18]>>4)*8+int(p[18]>>5);
        int scale=s16(0x4000-(128-velocity_curves[index])*gain_scale)>>6;
        for(unsigned i=0;i<4;++i)e.levels[i]=uint16_t((p[i*4]&127)*scale);
        int nibble=s8((p[16]&15)<<4)>>4;
        int key_scale=-(s16((int(v.effective_key)-64)*nibble*18)>>6);
        int vel_rate=(p[18]&15)*9*v.owner.velocity>>7;
        e.rates[0]=uint8_t(std::clamp(int(p[2]&127)-vel_rate,0,99));
        e.rates[1]=uint8_t(std::clamp(int(p[6]&127)+key_scale,0,99));
        e.rates[2]=uint8_t(std::clamp(int(p[10]&127)+key_scale,0,99));
        e.rates[3]=uint8_t(p[14]&127);
        e.position=uint32_t(e.levels[0])<<16;
        if(amplitude&&s16(e.levels[1]-e.levels[0])>0x3000&&e.rates[0]>20){
            e.intermediate=true;segment(e,uint16_t(e.levels[1]-0x1400),e.rates[0]-10);
        }else segment(e,e.levels[1],e.rates[0]);
    }
    bool update_envelope(Voice &v,unsigned which) {
        Envelope &e=v.envelope[which];if(e.done)return false;if(!e.advance())return false;
        uint16_t level=uint16_t(e.target>>16);if(e.slope<=0)level^=0x8000;
        e.position=uint32_t(level)<<16;
        if(v.direct){if(v.released){e.done=true;return true;}e.slope=0;e.target=e.position;e.done=true;return false;}
        if(e.stage==4){e.done=true;return which==2;}
        if(e.intermediate){e.intermediate=false;segment(e,e.levels[1],e.rates[0]);return false;}
        ++e.stage;
        if(e.stage==1)segment(e,e.levels[2],e.rates[1]);
        else if(e.stage==2)segment(e,e.levels[3],e.rates[2]);
        else if(e.flags&0x40){e.stage=4;segment(e,0,e.rates[3]);}
        else if(e.flags&0x80){
            e.stage=0;segment(e,e.levels[1],e.rates[0]);
            if(which==2&&(v.patch[0x6c]&127)<0x54){
                v.position=v.reverse?v.end:accumulator_start(v);
                if(v.sample_finished){v.sample_finished=false;emit(v,VoiceEvent::Kind::Start,tick);}
            }
        }else{e.done=true;return which==2&&e.level()==0;}
        return false;
    }
    int16_t source(const Voice &v,unsigned selector) const {
        switch(selector&15){
        case 0:return v.lfo;case 1:return v.envelope[0].level();case 2:return v.envelope[1].level();
        case 3:return v.envelope[2].level();case 4:return v.random;case 5:return v.previous_random;
        case 6:return s16(v.owner.velocity*257);case 7:return s16((int(v.effective_key)-60)*256);
        case 8:return s16(parameter_word(v.owner.channel,0x1a));case 9:return s16(parameter_word(v.owner.channel,0x12));
        case 10:return s16(parameter_word(v.owner.channel,0x0a));case 11:return s16(parameter_word(v.owner.channel,0x10));
        case 12:return s16(parameter_word(v.owner.channel,0x0c));case 13:return s16(v.pressure*256);
        case 14:return 0x7fff;case 15:return 0;
        }
        return 0;
    }
    double accumulator_start(const Voice &v) const {
        uint32_t address=(v.split.accumulator|((v.selector&1)<<28))&0x1fffff00;
        return double((v.selector>>1)<<20)+double(address)/512;
    }
    void update_lfo(Voice &v) {
        const uint8_t *p=v.patch;
        v.lfo_depth=s16(v.lfo_depth+v.lfo_step);
        if(uint16_t(v.lfo_depth)>uint16_t(v.lfo_target)){v.lfo_step=0;v.lfo_depth=v.lfo_target;}
        unsigned phase_index=p[0x68];
        v.lfo_phase=uint16_t(v.lfo_phase+phase_rates[phase_index]);
        unsigned wave=((p[0x6a]&15)<<8)|(v.lfo_phase>>8);
        int modulation=source(v,p[0x64]&15);
        int depth=std::clamp(int(v.lfo_depth)+modulation,0,32767);
        v.lfo=s16((depth*int(s8(rb(0xc0a73c+wave))))>>7);
    }
    void update_random(Voice &v) {
        if(--v.random_countdown)return;
        v.random_countdown=v.random_period;
        noise_history=uint16_t(noise_history+noise);noise=uint16_t(noise+noise_history);
        v.random=s16(noise); // previous_random retains the onset value.
    }
    uint16_t coefficient(int value,unsigned bank=0) const {
        unsigned index=unsigned(std::clamp(value,0,1023))+bank;
        return uint16_t(filters[index]<<4)&0xfff0;
    }
    void parameters(Voice &v,bool initial,uint64_t at,bool notify=true) {
        const uint8_t *p=v.patch;const Channel &c=v.owner.channel;
        int common=attenuation[c.parameters[0x20]&127]+attenuation[c.sequence_volume&127];
        if(c.parameters[0x14])common+=uint8_t(c.parameters[0x14]+127);
        int pan_position;
        if(v.direct){
            int curve=(p[0x12]>>6)*128+v.owner.velocity;
            int sensitivity=(p[8]>>4)*8+(p[8]>>5);
            int scale=s16(0x4000-(128-velocity_curves[unsigned(curve)])*sensitivity)>>6;
            if(initial){
                Envelope &e=v.envelope[2];e.position=(uint32_t(uint16_t((p[0x10]&127)*scale))<<16)|0x7fff;
                e.slope=-s16(rw(0xc089dc+2*(p[0]&127)));e.target=(uint32_t(uint16_t((e.position>>16)^0x8000)))<<16;
            }
            pan_position=c.parameters[0x26]==128?((p[0x12]&15)^8)<<3:(s8(c.parameters[0x26])>>1)+64;
            int level=(v.envelope[2].level()>>5)+((p[0x10]&128)?0xa8:0);
            v.left_volume=volume_target((common+pan[unsigned(127-pan_position)])*4+level,0xfa0);
            v.right_volume=volume_target((common+pan[unsigned(pan_position)])*4+level,0xfa0);
            int filter_index=p[0xe]*4+((p[0xe]&15)*v.owner.velocity>>9);
            v.k1=v.k2=coefficient(filter_index);
        }else{
            int amp_source=source(v,p[0x60]&15),base;
            if((p[0x60]&15)==15)base=254;
            else {
                int x=(p[0x60]&15)==0?(amp_source>>1)+0x4000:std::max(0,amp_source);
                int factor=s8(p[0x62]);int value=(x*factor)>>15;if(factor>=0)value-=factor;base=(value+127)*2;
            }
            base+=attenuation[p[0x5e]&127]+((p[0x5e]&128)?43:0)+common+attenuation[v.weight&127];
            int pan_value=s8(c.parameters[0x26]);if(pan_value<=-127)pan_value=s8(p[0x4e]&0xf0);
            pan_position=(pan_value>>1)+64;
            int env=v.envelope[2].level()>>5;
            v.left_volume=volume_target((base+pan[unsigned(127-pan_position)])*4+env,0x1b64);
            v.right_volume=volume_target((base+pan[unsigned(pan_position)])*4+env,0x1b64);
            int filter_env=v.envelope[1].level();
            int key=int(v.effective_key)-60;
            int first=(filter_env*s8(p[0x4c])+source(v,p[0x4e]&15)*s8(p[0x50]))>>12;
            first+=(key*s8(p[0x4a]))>>3;first+=(p[0x48]&127)*8;
            int second=(filter_env*s8(p[0x56])+((p[0x52]&128)?source(v,p[0x4e]&15):0)*s8(p[0x50]))>>12;
            second+=(key*s8(p[0x54]))>>3;second+=(p[0x52]&127)*8;
            v.k1=coefficient(first);v.k2=coefficient(second,(p[0x44]&128)?0:1024);
            int pitch_env=s16(v.envelope[0].level()-((p[0xc]&127)<<8));
            int64_t pitch_mod=int64_t(pitch_env)*s8(p[0x40])+int64_t(v.lfo)*s8(p[0x42]);
            int depth=s8(p[0x46]);int multiplier=rw(0xc0a46c+unsigned(std::abs(depth))*2);
            if(depth<0)multiplier=-s16(multiplier);
            pitch_mod+=(int64_t(source(v,p[0x44]&15))*multiplier)>>4;
            int bend=s16(parameter_word(c,0xa)-0x4000);
            pitch_mod>>=4;pitch_mod+=int64_t(bend)*bend_range;pitch_mod>>=6;
            v.pitch=s16(v.glide_target+v.split.tuning+int(pitch_mod));
            if(v.split.mode&128){
                int amount=(int(s8(p[0x70]))*source(v,p[0x6e]&15))>>7;
                amount=std::clamp(amount+(p[0x72]<<8),0,32767);
                int delta=s16((int64_t(amount)*s16(v.split.accumulator>>16))>>16);
                int span=int16_t(uint16_t((v.split.end-v.split.start)>>4));
                uint32_t offset=uint32_t(uint32_t(uint16_t(span))*frequency(delta))>>6;
                uint32_t start=(v.split.end-offset)|uint32_t((v.selector&1)<<28);
                v.start=double((v.selector>>1)<<20)+double(start&0x1fffffe0)/512;
                v.pitch=s16(v.pitch+delta);
            }
            v.frequency=frequency(v.pitch);
        }
        if(v.direct)v.frequency=frequency(v.pitch+v.split.tuning);
        v.increment=double(v.frequency)*native_rate/(1024.0*sample_rate);
        unsigned choice=c.parameters[0x31];
        if(choice>=3){if(choice==5)choice=3;else choice=(v.direct?p[0x12]>>4:p[0x60]>>4)&3;}
        v.pair=uint8_t((choice+1)&3);
        v.target_left=v.left_volume;v.target_right=v.right_volume;
        if(initial){v.left=v.target_left;v.right=v.target_right;v.gain_frames=0;}
        else v.gain_frames=(v.left==v.target_left&&v.right==v.target_right)?0:volume_ramp;
        if(v.gain_frames){v.step_left=(v.target_left-v.left)/v.gain_frames;v.step_right=(v.target_right-v.right)/v.gain_frames;}
        v.left_segment.frames=v.right_segment.frames=0;
        v.gain_dirty=true;
        v.filter.targets(v.k1,v.k2,initial);
        if(!initial&&notify)emit(v,VoiceEvent::Kind::Parameters,at);
    }
    void emit(const Voice &v,VoiceEvent::Kind kind,uint64_t at,const std::array<uint16_t,4> *onset=nullptr) {
        if(!observer)return;
        VoiceEvent e{};e.kind=kind;e.instance=v.owner.instance;e.tick=at;e.sequence=v.owner.sequence;
        e.track=v.owner.track;e.key=v.owner.key;e.layer=v.layer;e.output_pair=v.pair;
        e.sample_start=uint32_t(v.start);e.sample_end=uint32_t(v.end);e.frequency=v.frequency;
        e.left_volume=v.left_volume;e.right_volume=v.right_volume;e.k1=v.k1;e.k2=v.k2;
        if(onset){e.left_volume=(*onset)[0];e.right_volume=(*onset)[1];e.k1=(*onset)[2];e.k2=(*onset)[3];}
        e.loop=v.loop;e.reverse=v.reverse;
        if(buffering_events)buffered_events.push_back({current_frame,current_voice,e});
        else observer(e);
    }
    void stop(Voice &v,uint64_t at){if(v.active&&!v.sample_finished)emit(v,VoiceEvent::Kind::Stop,at);v.active=false;}
    void end_sample(Voice &v,uint64_t at) {
        if(v.allocation){emit(v,VoiceEvent::Kind::Stop,at);v.sample_finished=true;}
        else stop(v,at);
    }
    unsigned allocate(const Note &n,unsigned layer,unsigned priority,unsigned mode) {
        if(mode>=2){
            Voice *found=nullptr;
            for(Voice &v:voices)if(v.active&&!v.released&&!v.direct&&v.owner.sequence==n.sequence&&v.owner.track==n.track&&v.layer==layer)
                if(!found||v.serial<found->serial)found=&v;
            if(found)return unsigned(found-voices.data());
        }
        if(mode==1){
            for(Voice &v:voices)if(v.active&&!v.released&&!v.direct&&v.owner.sequence==n.sequence&&v.owner.track==n.track&&v.layer==layer){
                release(v,n.tick);break;
            }
        }
        for(unsigned i=32;i--;)if(!voices[i].active)return i;
        for(unsigned klass:std::array<unsigned,3>{0,64,128}){
            if(klass==64&&priority==0)break;if(klass==128&&priority<=64)break;
            for(bool released:std::array<bool,2>{true,false}){
                Voice *old=nullptr;for(Voice &v:voices)if(v.released==released&&(klass==128||v.priority==klass))
                    if(!old||v.serial<old->serial)old=&v;
                if(old){unsigned index=unsigned(old-voices.data());stop(*old,n.tick);return index;}
            }
        }
        return 32;
    }
    void move_before_cursor(unsigned i) {
        unsigned a=voice_prev[i],b=next[i];next[a]=uint8_t(b);voice_prev[b]=uint8_t(a);
        if(cursor==i)cursor=uint8_t(b);
        a=voice_prev[cursor];next[a]=uint8_t(i);voice_prev[i]=uint8_t(a);next[i]=cursor;voice_prev[cursor]=uint8_t(i);
    }
    int key_weight(const uint8_t *p,int key) const {
        int amount=s8(p[0x58]),lo=p[0x5a],hi=p[0x5c];
        if(amount==-128)return key<lo||key>hi?-1:127;
        if(amount<0){if(key<=lo)return 127;if(key>=hi)return 127+amount;
            return hi<=lo?127:127+amount+(-amount)*(hi-key)/(hi-lo);}
        if(key>=hi)return 127;if(key<=lo)return 127-amount;
        return hi<=lo?127:127-amount+amount*(key-lo)/(hi-lo);
    }
    void begin(const Note &n,const uint8_t *prog,const uint8_t *patch,unsigned layer,bool direct) {
        int kernel=uint8_t(n.kernel_key+n.channel.parameters[0x35]);
        int weight=direct?127:key_weight(patch,kernel);if(weight<0)return;
        if(!direct){int threshold=s8(patch[0x6e]);int boundary=(std::abs(threshold&~15)*9/8)&0x7c;
            if(threshold<0?n.velocity>=boundary:n.velocity<boundary)return;}
        unsigned mode=direct?0:(patch[0x44]>>4)&3;
        unsigned priority=direct?64:patch[0x60]&0xc0;
        int prior_pitch=0;bool prior_found=false;
        if(mode==1)for(const Voice &old:voices)
            if(old.active&&old.owner.sequence==n.sequence&&old.owner.track==n.track&&old.layer==layer){
                prior_pitch=old.glide_target;prior_found=true;break;
            }
        unsigned index=allocate(n,layer,priority,mode);if(index==32)return;
        Voice &v=voices[index];bool legato=v.active&&mode>=2&&!direct;
        double old_board=double((v.selector>>1)<<20);
        double old_start=v.start-old_board,old_end=v.end-old_board;
        bool old_hardware=v.hardware_valid;
        if(legato){
            uint8_t velocity=v.owner.velocity;
            v.owner=n;v.owner.velocity=velocity;v.released=false;
            int16_t destination=s16(((patch[0x48]&128)?kernel:60)*256+s8(patch[0x3e])*2+(patch[0x3c]<<8));
            set_glide(v,destination,prog[0x17e]);
            parameters(v,false,n.tick);return;
        }
        stop(v,n.tick);v=Voice{};v.owner=n;v.patch=patch;v.layer=uint8_t(layer);
        v.direct=direct;v.priority=uint8_t(priority);v.allocation=uint8_t(mode);v.effective_key=uint8_t(std::clamp(kernel,21,108));
        v.weight=uint8_t(weight);v.serial=++serial;
        if(direct){
            int key=(patch[2]&128)?uint8_t(kernel-patch[10]+60):60;
            key=s8(key+s8(n.channel.parameters[0x35])+s8(patch[6]));
            v.pitch=s16(std::clamp(key*256,0,0x7f00)|((patch[8]&15)<<4));
        }else{
            v.glide_target=s16(((patch[0x48]&128)?kernel:60)*256+s8(patch[0x3e])*2+(patch[0x3c]<<8));
            v.pitch=std::clamp(int(v.glide_target),0,0x7f00);
            if(prog[0x17e]&&(mode==2||prior_found)){
                int16_t destination=v.glide_target;
                v.glide_target=prior_found?s16(prior_pitch):s16(uint8_t(n.channel.parameters[0x22]+patch[0x3c])*256);
                set_glide(v,destination,prog[0x17e]);
            }
            for(unsigned i=0;i<3;++i)initialize_envelope(v.envelope[i],patch+i*20,v,i==2);
            v.lfo_target=int16_t((patch[0x66]&127)<<8);
            v.lfo_phase=(patch[0x66]&128)?0:noise;
            v.lfo_step=int16_t((uint32_t(v.lfo_target)*rw(0xc089dc+(patch[0x6a]&0xf0)))>>16);
            v.filter.mode=uint8_t((patch[0x44]>>6)&3);
            unsigned period=(patch[0x64]&0xf0)>>1;
            period=(period|(period>>4));v.random_period=v.random_countdown=uint16_t((-int(period))&127);
            noise_history=uint16_t(noise_history+noise);noise=uint16_t(noise+noise_history);
            v.random=v.previous_random=s16(noise);
        }
        v.split=split_for(patch,layer,direct,v.pitch,v.selector,n.channel.program);
        unsigned id=(direct?patch[4]:patch[0x6c])&127;
        if(direct&&id==0x78)throw std::runtime_error("ROM direct sample78 enters infinite branch c1968e");
        if(!direct&&id>=0x78)throw std::runtime_error("HLE concatenated sample program is outside supplied active Land Maker patches");
        uint32_t bit=uint32_t(v.selector&1)<<28;unsigned board=(v.selector>>1)<<20;
        uint8_t control=(!direct&&id>=0x6f)?0x0b:rb(0xc0a734+v.split.mode);
        v.reverse=((direct?patch[4]:patch[0x6c])&128)&&id<0x54;
        if(v.reverse)control=0x43;
        v.loop=(control&8)!=0;v.bidirectional=(control&16)!=0;
        uint32_t start=v.reverse?v.split.accumulator:v.split.start;
        uint32_t phase=v.reverse?v.split.end:v.split.accumulator;
        if(id>=0x54&&id<0x6f)phase&=0xfffffe00;
        if(!direct)phase&=0xffffff00;
        v.start=double(board)+double((start|bit)&0x1fffffe0)/512;
        v.end=double(board)+double((v.split.end|bit)&0x1fffffe0)/512;
        v.position=double(board)+double((phase|bit)&0x1fffffff)/512;
        if(!direct&&id<0x54){
            int amount=s8(patch[0x70])*n.velocity+(patch[0x72]<<7);
            if(amount>0){amount=std::min(32767,amount*2);int length=s16((v.split.end-v.split.accumulator)>>10);
                int32_t offset=wrap_product(int64_t(length)*amount)>>5;
                v.position+=double(v.reverse?-offset:offset)/512;
                v.position=std::clamp(v.position,v.start,v.end);
            }
        }
        if(!direct&&id>=0x6f){
            // c17e8c loads ACC from this bank's probe. c17fbe leaves START/END
            // unchanged, including when the physical slot was previously free.
            v.boundary=true;v.loop=true;
            v.start=double(board)+(old_hardware?old_start:double(reset_start)/512);
            v.end=double(board)+(old_hardware?old_end:double(reset_end)/512);
            v.position=double(board)+double(banks[v.selector].probe&0x1fffffff)/512;
        }
        v.active=v.hardware_valid=true;move_before_cursor(index);
        if(!direct)update_lfo(v);
        parameters(v,true,n.tick);
        std::array<uint16_t,4> onset{v.left_volume,v.right_volume,v.k1,v.k2};
        if(!direct){
            bool ended=false;for(unsigned i=0;i<3;++i)ended|=update_envelope(v,i);
            if(ended){v.active=false;return;}
            update_lfo(v);update_random(v);advance_glide(v);
            parameters(v,false,n.tick,false); // c181da calls c18372 before key-on.
        }
        emit(v,VoiceEvent::Kind::Start,n.tick,&onset);
        if(!direct)emit(v,VoiceEvent::Kind::Parameters,n.tick);
    }
    void note_on(const Note &submitted) {
        Note n=submitted;n.velocity&=127; // c16faa clears the velocity flag bit.
        const uint8_t *prog=program(n.channel.program);if(!prog)return;
        unsigned enabled=programs[n.channel.program&255].enabled;
        if(!enabled){int key=s8(uint8_t(n.kernel_key+n.channel.parameters[0x35]));
            for(unsigned i=0;i<17;++i){const uint8_t *p=prog+i*20;if(key>=s8(p[10])&&key<=s8(p[12])){begin(n,prog,p,i,true);return;}}
            return;
        }
        if((n.channel.parameters[0x32]&0x30)==0x30)return;
        if(n.channel.parameters[0x1c])enabled=n.channel.parameters[0x1c];
        for(unsigned layer=3;layer--;){if(!(enabled&(1u<<layer)))continue;
            const uint8_t *p=prog+layer*0x76;
            if(p[0x74]){
                auto slot=std::find_if(pending.begin(),pending.end(),[](const Pending &x){return !x.active;});
                if(slot==pending.end())throw std::runtime_error("HLE delayed-note pool exhausted");
                *slot={n,prog,p,n.tick+uint64_t(p[0x74])*timer_ticks,uint8_t(layer),true,p[0x74]==0xfb};
            }else begin(n,prog,p,layer,false);
        }
    }
    void release(Voice &v,uint64_t at) {
        if(v.released)return;v.released=true;emit(v,VoiceEvent::Kind::Release,at);
        if(v.direct){
            if(v.patch[0]&128)return;
            Envelope &e=v.envelope[2];e.done=false;e.position&=0xffff0000;
            e.slope=wrap_product(-int64_t(0x7f00)*s16(rw(0xc089dc+2*(v.patch[2]&127)))*2);
            e.target=e.slope<=0?0x80000000:0;e.stage=4;
        }else{
            for(Envelope &e:v.envelope){if(e.flags&64)continue;e.done=false;e.stage=4;segment(e,0,e.rates[3]);}
            if(!(v.envelope[2].flags&64)&&v.envelope[2].level()==0){stop(v,at);return;}
            if(v.boundary){uint32_t value=v.split.end;unsigned board=(v.selector>>1)<<20;
                if(value&0x80000000){v.start=double(board)+double(value&0x1fffffe0)/512;v.position=v.end;v.reverse=true;}
                else{v.end=double(board)+double(value&0x1fffffe0)/512;v.position=v.start;}
                v.loop=false;
            }
        }
    }
    void note_off(uint64_t instance,uint64_t at) {
        for(Pending &p:pending)if(p.active&&p.note.instance==instance){
            if(p.release_trigger){Note n=p.note;n.tick=at;begin(n,p.program,p.patch,p.layer,false);}
            p.active=false;
        }
        for(Voice &v:voices)if(v.active&&v.owner.instance==instance)release(v,at);
    }
    void channel_update(uint8_t sequence,uint8_t track,const Channel &c,uint64_t at) {
        for(Pending &p:pending)if(p.active&&p.note.sequence==sequence&&p.note.track==track)p.note.channel=c;
        for(Voice &v:voices)if(v.active&&v.owner.sequence==sequence&&v.owner.track==track){v.owner.channel=c;parameters(v,false,at);}
    }
    void reset(uint64_t at) {
        for(Voice &v:voices){stop(v,at);v=Voice{};}
        for(Pending &p:pending)p.active=false;
        for(unsigned i=0;i<32;++i){next[i]=uint8_t((i+1)%32);voice_prev[i]=uint8_t((i+31)%32);}
        tick=at;tick_remainder=0;timer=(at/timer_ticks+1)*timer_ticks;cursor=0;budget_slot=0;
        noise=rw(0xc16f04);noise_history=rw(0xc16f0a);
        serial=0;buffered_events.clear();buffering_events=false;
    }
    void service(uint64_t at) {
        for(Pending &p:pending)if(p.active&&!p.release_trigger&&p.tick<=at){Note n=p.note;n.tick=p.tick;p.active=false;begin(n,p.program,p.patch,p.layer,false);}
        unsigned count=budgets[budget_slot];budget_slot=uint8_t((budget_slot+1)%12);
        while(count--){unsigned index=cursor;cursor=next[cursor];Voice &v=voices[index];if(!v.active)continue;
            bool ended=false;
            if(v.direct)ended=update_envelope(v,2);
            else{for(unsigned i=0;i<3;++i)ended|=update_envelope(v,i);update_lfo(v);update_random(v);}
            if(ended){stop(v,at);continue;}
            advance_glide(v);
            parameters(v,false,at);
        }
    }
    double sample(size_t at) const {
        if(at>=samples.size())return 0;
        return int16_t(samples[at]);
    }
    float interpolated_gain(double encoded) const {
        double index=std::clamp(encoded/256.0,0.0,255.0);unsigned i=unsigned(index);
        return float(gain[i]+(gain[std::min(i+1,255u)]-gain[i])*(index-i));
    }
    // gain[] is linear between adjacent entries and ramps move the encoded
    // volume by a constant step, so a running sum tracks interpolated_gain (to
    // double rounding) until the ramp crosses into the next table interval.
    float ramp_gain(double encoded,double step,GainSegment &s) const {
        if(s.frames){--s.frames;s.value+=s.slope;return float(s.value);}
        const float exact=interpolated_gain(encoded);
        const double index=encoded/256.0;
        if(index>0&&index<255){
            const unsigned i=unsigned(index);const double per_frame=step/256.0,delta=double(gain[i+1])-gain[i];
            s.value=gain[i]+delta*(index-i);s.slope=delta*per_frame;
            const double room=per_frame>0?(double(i+1)-index)/per_frame:per_frame<0?(index-double(i))/-per_frame:1e9;
            s.frames=room>2?unsigned(std::min(room,1e6))-1:0;
        }
        return exact;
    }
    void render(float *output,size_t frames) {
        if(!output&&frames)throw std::invalid_argument("Null HLE output buffer");
        if(!frames)return;
        if(frame_ticks.size()<frames)frame_ticks.resize(frames);
        uint64_t cur_tick=tick;uint32_t cur_rem=tick_remainder;
        for(size_t f=0;f<frames;++f){
            frame_ticks[f]=cur_tick;
            cur_rem+=main_clock;cur_tick+=cur_rem/sample_rate;cur_rem%=sample_rate;
        }
        const double samples_limit=double(samples.size());
        size_t frame_start=0;
        while(frame_start<frames){
            if(timer<=frame_ticks[frame_start]){
                tick=frame_ticks[frame_start];
                while(timer<=tick){service(timer);timer+=timer_ticks;}
            }
            size_t frame_end=frame_start+1;
            while(frame_end<frames&&frame_ticks[frame_end]<timer)++frame_end;
            std::fill_n(output+frame_start*8,(frame_end-frame_start)*8,0.f);
            if(observer){buffering_events=true;buffered_events.clear();}
            for(size_t chunk_start=frame_start;chunk_start<frame_end;chunk_start+=voice_block_frames){
                const size_t chunk_frames=std::min(voice_block_frames,frame_end-chunk_start);
                size_t count=0;
                for(unsigned vi=0;vi<32;++vi){
                    Voice &v=voices[vi];if(!v.active)continue;
                    const size_t lane=count++;
                    lane_voice[lane]=uint8_t(vi);
                    lane_pair[lane]=v.pair;
                    mode[lane]=v.filter.mode;
                    for(unsigned s=0;s<4;++s){
                        state[s*voice_lane_stride+lane]=v.filter.state[s];
                        previous[s*voice_lane_stride+lane]=v.filter.previous[s];
                    }
                    current_voice=vi;
                    const double length=v.end-v.start,increment=v.increment;
                    auto fill_remaining=[&](size_t start_row){
                        for(size_t rem=start_row;rem<chunk_frames;++rem){
                            const size_t idx=rem*voice_lane_stride+lane;
                            input[idx]=0.f;gain_left[idx]=0.f;gain_right[idx]=0.f;
                            a1[idx]=float(v.filter.a1);a2[idx]=float(v.filter.a2);high_pole[idx]=float(v.filter.high_pole);
                        }
                    };
                    size_t r=0;
                    while(r<chunk_frames){
                        size_t n=0;
                        const bool ramping=v.gain_frames>0;
                        if(!v.sample_finished&&v.filter.ramp==0&&
                           (!ramping||(v.left_segment.frames>0&&v.right_segment.frames>0&&v.gain_frames>1))){
                            bool pos_ok=false;
                            double pos_bound=0;
                            if(!v.reverse){
                                if(v.position>=0&&v.position<samples_limit-1&&v.end<samples_limit-1){
                                    if(increment>0){
                                        pos_bound=std::floor((v.end-v.position)/increment)-1.0;
                                        pos_ok=(pos_bound>=1.0);
                                    }else pos_ok=true;
                                }
                            }else{
                                if(v.start>=0&&v.position>=0&&v.position<samples_limit-1){
                                    if(increment>0){
                                        pos_bound=std::floor((v.position-v.start)/increment)-1.0;
                                        pos_ok=(pos_bound>=1.0);
                                    }else pos_ok=true;
                                }
                            }
                            if(pos_ok){
                                n=chunk_frames-r;
                                if(ramping){
                                    n=std::min(n,size_t(v.left_segment.frames));
                                    n=std::min(n,size_t(v.right_segment.frames));
                                    n=std::min(n,size_t(v.gain_frames-1));
                                }
                                if(increment>0&&pos_bound<double(n))n=size_t(pos_bound);
                            }
                        }
                        const bool slow=n<1;
                        if(slow){
                            const size_t frame=chunk_start+r;
                            current_frame=frame;
                            const uint64_t frame_tick=frame_ticks[frame];
                            if(v.gain_frames){
                                v.left+=v.step_left;v.right+=v.step_right;
                                if(!--v.gain_frames){v.left=v.target_left;v.right=v.target_right;v.gain_dirty=true;}
                                else{v.left_gain=ramp_gain(v.left,v.step_left,v.left_segment);v.right_gain=ramp_gain(v.right,v.step_right,v.right_segment);v.gain_dirty=false;}
                            }
                            if(v.sample_finished){
                                const size_t idx=r*voice_lane_stride+lane;
                                input[idx]=0.f;gain_left[idx]=0.f;gain_right[idx]=0.f;
                                a1[idx]=float(v.filter.a1);a2[idx]=float(v.filter.a2);high_pole[idx]=float(v.filter.high_pole);
                                ++r;
                                continue;
                            }
                            if(v.position<0||v.position>=samples_limit){stop(v,frame_tick);fill_remaining(r);break;}
                            const size_t index=size_t(v.position);
                            const double fraction=v.position-index;
                            const double first=sample(index);
                            const size_t idx=r*voice_lane_stride+lane;
                            input[idx]=float(first+(sample(index+1)-first)*fraction);
                            v.filter.advance();
                            a1[idx]=float(v.filter.a1);
                            a2[idx]=float(v.filter.a2);
                            high_pole[idx]=float(v.filter.high_pole);
                            if(v.gain_dirty){
                                v.left_gain=interpolated_gain(v.left);
                                v.right_gain=interpolated_gain(v.right);
                                v.gain_dirty=false;
                            }
                            const float scale=1.f/524288.f;
                            gain_left[idx]=v.left_gain*scale;
                            gain_right[idx]=v.right_gain*scale;
                            v.position+=v.reverse?-increment:increment;
                            if(!v.reverse&&v.position>v.end){
                                if(!v.loop||length<=0){end_sample(v,frame_tick);fill_remaining(r+1);break;}
                                if(v.bidirectional){v.position=v.end-(v.position-v.end);v.reverse=true;}
                                else v.position=v.start+std::fmod(v.position-v.end,length);
                            }else if(v.reverse&&v.position<v.start){
                                if(!v.loop||length<=0){end_sample(v,frame_tick);fill_remaining(r+1);break;}
                                if(v.bidirectional){v.position=v.start+(v.start-v.position);v.reverse=false;}
                                else v.position=v.end-std::fmod(v.start-v.position,length);
                            }
                            ++r;
                        }else{
                            const float a1c=float(v.filter.a1);
                            const float a2c=float(v.filter.a2);
                            const float hpc=float(v.filter.high_pole);
                            double pos=v.position;
                            const double inc=v.reverse?-increment:increment;
                            if(ramping){
                                double v_left=v.left,v_right=v.right;
                                double seg_l_val=v.left_segment.value;
                                const double seg_l_slope=v.left_segment.slope;
                                double seg_r_val=v.right_segment.value;
                                const double seg_r_slope=v.right_segment.slope;
                                const double step_l=v.step_left,step_r=v.step_right;
                                const float scale=1.f/524288.f;
                                for(size_t k=0;k<n;++k){
                                    const size_t idx=(r+k)*voice_lane_stride+lane;
                                    const size_t i=size_t(pos);
                                    const double frac=pos-double(i);
                                    const double a=int16_t(samples[i]),b=int16_t(samples[i+1]);
                                    input[idx]=float(a+(b-a)*frac);
                                    pos+=inc;
                                    a1[idx]=a1c;a2[idx]=a2c;high_pole[idx]=hpc;
                                    v_left+=step_l;v_right+=step_r;
                                    seg_l_val+=seg_l_slope;seg_r_val+=seg_r_slope;
                                    gain_left[idx]=float(seg_l_val)*scale;
                                    gain_right[idx]=float(seg_r_val)*scale;
                                }
                                v.left=v_left;v.right=v_right;
                                v.left_segment.frames-=unsigned(n);
                                v.left_segment.value=seg_l_val;
                                v.right_segment.frames-=unsigned(n);
                                v.right_segment.value=seg_r_val;
                                v.gain_frames-=unsigned(n);
                                v.left_gain=float(seg_l_val);
                                v.right_gain=float(seg_r_val);
                                v.gain_dirty=false;
                            }else{
                                if(v.gain_dirty){
                                    v.left_gain=interpolated_gain(v.left);
                                    v.right_gain=interpolated_gain(v.right);
                                    v.gain_dirty=false;
                                }
                                const float gl=v.left_gain*(1.f/524288.f);
                                const float gr=v.right_gain*(1.f/524288.f);
                                for(size_t k=0;k<n;++k){
                                    const size_t idx=(r+k)*voice_lane_stride+lane;
                                    const size_t i=size_t(pos);
                                    const double frac=pos-double(i);
                                    const double a=int16_t(samples[i]),b=int16_t(samples[i+1]);
                                    input[idx]=float(a+(b-a)*frac);
                                    pos+=inc;
                                    a1[idx]=a1c;a2[idx]=a2c;high_pole[idx]=hpc;
                                    gain_left[idx]=gl;
                                    gain_right[idx]=gr;
                                }
                            }
                            v.position=pos;
                            current_frame=chunk_start+r+n-1;
                            r+=n;
                        }
                    }
                }
                const size_t lanes=count==0?0:((count+voice_lane_multiple-1)/voice_lane_multiple)*voice_lane_multiple;
                for(size_t lane=count;lane<lanes;++lane){
                    mode[lane]=3;
                    for(unsigned s=0;s<4;++s){
                        state[s*voice_lane_stride+lane]=0.f;
                        previous[s*voice_lane_stride+lane]=0.f;
                    }
                    for(size_t r=0;r<chunk_frames;++r){
                        const size_t idx=r*voice_lane_stride+lane;
                        input[idx]=0.f;gain_left[idx]=0.f;gain_right[idx]=0.f;
                        a1[idx]=0.f;a2[idx]=0.f;high_pole[idx]=0.f;
                    }
                }
                if(count>0){
                    const VoiceBlock block{
                        .frames=chunk_frames,
                        .lanes=lanes,
                        .input=input,
                        .a1=a1,
                        .a2=a2,
                        .high_pole=high_pole,
                        .gain_left=gain_left,
                        .gain_right=gain_right,
                        .mode=mode,
                        .state=state,
                        .previous=previous,
                        .out_left=out_left,
                        .out_right=out_right,
                    };
                    render_voice_block(block);
                    for(size_t lane=0;lane<count;++lane){
                        if(lane_pair[lane]==0)continue;
                        const unsigned pair_left=unsigned(lane_pair[lane])*2;
                        const unsigned pair_right=pair_left+1;
                        for(size_t r=0;r<chunk_frames;++r){
                            float *out=output+(chunk_start+r)*8;
                            out[pair_left]+=out_left[r*voice_lane_stride+lane];
                            out[pair_right]+=out_right[r*voice_lane_stride+lane];
                        }
                    }
                    for(size_t lane=0;lane<count;++lane){
                        Voice &v=voices[lane_voice[lane]];
                        for(unsigned s=0;s<4;++s){
                            v.filter.state[s]=state[s*voice_lane_stride+lane];
                            v.filter.previous[s]=previous[s*voice_lane_stride+lane];
                        }
                    }
                }
            }
            if(observer){
                buffering_events=false;
                if(!buffered_events.empty()){
                    std::stable_sort(buffered_events.begin(),buffered_events.end(),[](const BufferedEvent &a,const BufferedEvent &b){
                        return a.frame!=b.frame?a.frame<b.frame:a.voice<b.voice;
                    });
                    for(const auto &be:buffered_events)observer(be.event);
                    buffered_events.clear();
                }
            }
            float *sub_out=output+frame_start*8;
            const size_t sub_floats=(frame_end-frame_start)*8;
            for(size_t i=0;i<sub_floats;++i)sub_out[i]=std::clamp(sub_out[i],-1.f,1.f-1.f/524288.f);
            frame_start=frame_end;
        }
        tick=cur_tick;tick_remainder=cur_rem;
    }
};

Synth::Synth(std::span<const uint8_t> sound,std::span<const uint16_t> samples)
    : impl_(std::make_unique<Impl>(sound,samples)) {}
Synth::~Synth()=default;
void Synth::note_on(const Note &n){impl_->note_on(n);}
void Synth::note_off(uint64_t instance,uint64_t tick){impl_->note_off(instance,tick);}
void Synth::channel_update(uint8_t sequence,uint8_t track,const Channel &c,uint64_t tick){impl_->channel_update(sequence,track,c,tick);}
void Synth::reset(uint64_t tick){impl_->reset(tick);}
void Synth::render(float *output,size_t frames){impl_->render(output,frames);}
void Synth::set_observer(std::function<void(const VoiceEvent &)> observer){impl_->observer=std::move(observer);}
}
