#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "eeprom.hpp"
#include "interpreter.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char *why) { if(!ok)throw std::runtime_error(why); }
f3rt::RomSet fixture() {
    f3rt::RomSet r;
    r.main.resize(0x200000);r.sprites.resize(0x400000);r.sprites_hi.resize(0x200000);
    r.tiles.resize(0x400000);r.tiles_hi.resize(0x200000);r.sound.resize(0x80000);r.samples.resize(0x1000000);
    r.main[1]=0x41;r.main[2]=0xff;r.main[3]=0xf0; // SSP 41fff0
    r.main[6]=1; // PC 100
    r.main[0x100]=0x70;r.main[0x101]=0x2a; // moveq #42,d0
    r.main[0x102]=0x60;r.main[0x103]=0xfe; // bra self
    r.samples[0x2468a]=0x45;r.samples[0x2468b]=0x67; // OTIS word 0x12345, above old truncated mask
    return r;
}
void send_bit(f3rt::Eeprom &e,bool bit,uint64_t now) { uint8_t pins=0x10|(bit?4:0);e.pins(pins,now);e.pins(pins|8,now); }
void command(f3rt::Eeprom &e,unsigned word,uint64_t now) { e.pins(0,now);for(int bit=8;bit>=0;--bit)send_bit(e,(word>>bit)&1,now); }
void serial_write(f3rt::Eeprom &e,unsigned address,uint16_t value,uint64_t now) {
    command(e,0x140|address,now);for(int bit=15;bit>=0;--bit)send_bit(e,(value>>bit)&1,now);e.pins(0,now);
}
uint16_t read_word(f3rt::Eeprom &e,uint64_t now) { uint16_t value=0;for(int i=0;i<16;++i) { send_bit(e,false,now);value=uint16_t((value<<1)|e.output(now)); }return value; }
void native(f3_cpu *cpu) { cpu->d[0]=99;cpu->pc+=2;cpu->cycles+=4; }
void check_audio_mixer() {
    f3rt::Audio audio;
    const std::array<uint8_t,4> rom{0x40,0,0x40,0};
    audio.load_sample_rom(rom);
    audio.write16(0x20001e,0x20);
    for (unsigned reg=1;reg<=6;++reg) audio.write16(0x200000+reg*2,0x4000);
    audio.write16(0x20001e,0);
    audio.write16(0x200010,0xff00);audio.write16(0x200012,0xff00);
    audio.write16(0x200000,0x0c00); // Constant sample, all poles lowpass, auxiliary pair.
    std::array<int16_t,2> pcm{};
    const auto sample = [&] {
        audio.advance(538);
        require(audio.render(pcm.data(),1)==1,"One complete audio sample is available");
    };
    sample();
    // DC 0x4000, OTIS volume 15.5, /2^19, board 0.18, two 100/32
    // gain stages, auxiliary route 0.5, and signed PCM scale 32768.
    require(pcm[0]==13950 && pcm[1]==13950,"Board gain and signed PCM normalization");
    audio.write8(0x340000,2);audio.write8(0x340002,0);
    sample();
    require(pcm[0]==0 && pcm[1]==13950,"Left volume mute preserves the right channel");
    audio.write8(0x340000,7);audio.write8(0x340002,0x33);
    sample();
    require(pcm[0]==0 && pcm[1]==3487,"Minus-six-dB control applies both baseline gain stages");
    audio.set_gain_model(f3rt::Audio::GainModel::SingleStage);
    sample();
    require(pcm[0]==0 && pcm[1]==715,"Single-stage gain is distinct and preserves channel mute");
    audio.set_reset(false);audio.set_reset(true);
    sample();
    require(pcm[0]==0 && pcm[1]==715,"CPU-line reset preserves attenuation and playing OTIS voices");
    audio.reset_board();
    sample();
    require(pcm[0]==1428 && pcm[1]==1428,"Board reset restores volume without resetting OTIS voices");
}
}
int main() try {
    check_audio_mixer();
    f3rt::Audio clock_audio;
    std::array<int16_t, 128> clock_samples{};
    uint64_t sample_count=0;
    for (unsigned i=0;i<10000;++i) {
        clock_audio.advance(16000);
        sample_count+=clock_audio.render(clock_samples.data(),clock_samples.size()/2);
    }
    require(sample_count==uint64_t(clock_audio.sample_rate())*10,
            "Ten seconds of audio match the advertised stream rate without clock drift");
    clock_audio.advance(638);
    clock_audio.reset_board();
    clock_audio.advance(438);
    require(clock_audio.render(clock_samples.data(),clock_samples.size()/2)==2,
            "Board reset preserves queued audio and fractional sample-clock phase");
    auto m=std::make_unique<f3rt::Machine>(fixture());
    require(m->cpu.cycles==4 && m->cpu.pc==0x100 && m->cpu.d[0]==0,
            "Cold reset charges four cycles without executing the first opcode");
    m->write32(0x400001,0x12345678);
    require(m->read32(0x420001)==0x12345678,"BE misaligned work RAM mirror");
    m->write32(0x41fffe,0xaabbccdd);
    require(m->read16(0x400000)==0xccdd && m->read16(0x41fffe)==0xaabb,"Work RAM wrap across mirror");
    m->write8(0x100,0xff);require(m->read16(0x100)==0x702a,"ROM is read-only");
    m->write8(0xc00010,0x75);require(m->audio->read16(0x140020)==0x75ff,"DPRAM sound high-byte lane");
    m->audio->write16(0x140020,0xaabb);require(m->read8(0xc00010)==0xaa,"DPRAM reverse lane");
    m->audio->write16(0x20001e,0); // Voice 0, register page
    m->audio->write16(0x200014,0x0246);m->audio->write16(0x200016,0x8a00);
    m->audio->write16(0x20001e,0x20); // Stopped voice sample-ROM readback
    require(m->audio->read16(0x20000c)==0x4567,"OTIS stopped voice reads full 20-bit sample address");
    m->audio->write16(0x600,0xa55a);m->audio->set_reset(false);
    m->audio->set_reset(true);m->audio->set_reset(false);
    require(m->audio->read16(0x600)==0xa55a,"Sound CPU RESET preserves board work RAM");
    m->write8(0x4a0004,0x04);m->write8(0x4a0004,0x04);require(m->coin_count[0]==1,"Coin counter rising-edge only");
    m->set_input(0,0x1000,true);require(!(m->read32(0x4a0000)&0x1000),"Active-low start input");
    f3rt::Eeprom e;
    uint64_t now=100;
    serial_write(e,63,0x1234,now);require(e.words[63]==0xffff,"EEPROM write disabled at power-on");
    command(e,0x130,now);e.pins(0,now); // EWEN
    serial_write(e,63,0x1234,now);
    require(e.output(now),"Deselected EEPROM DO is pulled high while programming");
    e.pins(0x10,now);
    require(!e.output(now),"Raising CS exposes EEPROM programming busy");
    serial_write(e,0,0xdead,now);
    require(e.words[0]==0xffff,"EEPROM ignores new commands while programming");
    e.pins(0x10,now);
    now+=27999;require(!e.output(now),"EEPROM write remains busy before 1750us deadline");
    ++now;require(e.output(now),"EEPROM write finishes without another clock edge");
    serial_write(e,0,0xabcd,now);now+=28000;
    command(e,0x1bf,now);require(!e.output(now),"EEPROM read dummy bit");
    require(read_word(e,now)==0x1234 && read_word(e,now)==0xabcd,"EEPROM sequential read wraps 63 to 0");
    e.pins(0,now);command(e,0x100,now);e.pins(0,now);serial_write(e,0,0x4321,now);
    e.pins(0x10,now);
    require(e.words[0]==0xabcd && e.output(now),"EEPROM EWDS protects contents without becoming busy");
    command(e,0x130,now);command(e,0x1ff,now); // EWEN; erase word 63
    e.pins(0,now);e.pins(0x10,now);
    now+=15999;require(!e.output(now),"EEPROM erase remains busy before 1000us deadline");
    ++now;require(e.output(now) && e.words[63]==0xffff,"EEPROM single-word erase completes");
    command(e,0x120,now);e.pins(0,now);e.pins(0x10,now); // ERAL
    now+=127999;require(!e.output(now),"EEPROM erase-all remains busy before 8000us deadline");
    ++now;require(e.output(now) && e.words[0]==0xffff,"EEPROM erase-all completes");
    command(e,0x110,now); // WRAL
    for(int bit=15;bit>=0;--bit)send_bit(e,(0x5a5a>>bit)&1,now);
    e.pins(0,now);e.pins(0x10,now);
    now+=127999;require(!e.output(now),"EEPROM write-all remains busy before 8000us deadline");
    ++now;require(e.output(now) && e.words[0]==0x5a5a && e.words[63]==0x5a5a,"EEPROM write-all completes");
    e.reset();e.pins(0x10,0);
    require(e.output(0) && e.words[0]==0x5a5a,"Power reset clears serial timing but preserves EEPROM contents");
    auto &cpu=m->cpu;
    cpu.usp=0x400800;cpu.a[7]=0x401000;cpu.sr=0x2000;
    f3_set_sr(&cpu,0);require(cpu.a[7]==0x400800 && cpu.ssp==0x401000,"Supervisor to user stack switch");
    m->write32(0x400000+26*4,0x400300);cpu.vbr=0x400000;cpu.pc=0x100;cpu.stopped=1;
    m->pending_irqs=1<<2;require(f3_boundary(&cpu)!=0,"IRQ redirects boundary");
    require(cpu.pc==0x400300 && !cpu.stopped && (cpu.sr&0x2700)==0x2200,"IRQ releases STOP and raises mask");
    require(cpu.a[7]==0x400ff8 && m->read32(cpu.a[7]+2)==0x100 && m->read16(cpu.a[7]+6)==104,"68020 interrupt frame");
    m->write32(cpu.vbr+5*4,0x400310);m->write16(0x400310,0x4e73); // RTE handler
    cpu.pc=0x100;cpu.sr=0x2700;
    const auto exception_cycles=cpu.cycles;const auto exception_sp=cpu.a[7];
    f3_exception(&cpu,5,0x102);
    require(cpu.cycles-exception_cycles==38 && m->read16(cpu.a[7]+6)==0x2014 &&
            m->read32(cpu.a[7]+8)==0x100,"Divide-by-zero full charge and format-2 instruction PC");
    require(f3_fallback(&cpu) && cpu.pc==0x102 && cpu.a[7]==exception_sp && cpu.sr==0x2700,
            "Real RTE restores the format-2 resume PC and stack");
    const f3_block blocks[]={{0x100,native}};
    require(f3_register_blocks(&cpu,blocks,1)==1,"Valid block table");
    cpu.pc=0x100;cpu.sr=0x2700;require(f3_dispatch(&cpu) && cpu.d[0]==99,"Native dispatch executes matching block");
    cpu.pc=0x100;cpu.sr=0x2700;require(f3_fallback(&cpu) && cpu.d[0]==42 && cpu.pc==0x102,"Fallback executes exactly one real instruction");
    cpu.d[0]=0xdeadbeef;cpu.sr=0x2015;
    const auto reset_cycles=cpu.cycles;
    m->interpreter->reset_main();
    require(cpu.d[0]==0xdeadbeef && cpu.sr==0x2715 && cpu.pc==0x100,
            "Reset preserves canonical native D/CCR, not stale fallback context");
    require(cpu.cycles-reset_cycles==4,"Warm reset charges its four-cycle latency exactly once");
    m->allow_main_fallback=false;const auto fallback_count=m->fallback_instructions;
    bool rejected=false;
    try { f3_fallback(&cpu); } catch(const std::runtime_error &e) { rejected=std::string(e.what()).find("0x100")!=std::string::npos; }
    require(rejected && cpu.pc==0x100 && cpu.d[0]==0xdeadbeef && m->fallback_instructions==fallback_count,
            "Native-only fallback rejection reports PC without executing or counting an instruction");
    const f3_block bad[]={{0x100,native},{0x100,native}};
    require(!f3_register_blocks(&cpu,bad,2),"Duplicate PCs rejected");
    m->audio->set_reset(true);
    m->audio->write8(0x280019,0x66);
    m->audio->write8(0x260001,0x12);m->audio->write8(0x260003,0x34);
    m->audio->write8(0x260005,0x56);m->audio->write8(0x260141,0);
    m->audio->set_reset(false);m->audio->set_reset(true);
    m->audio->write8(0x260101,0);
    require(m->audio->read8(0x260001)==0x12 && m->audio->read8(0x280019)==0x66,
            "CPU-line reset preserves DSP registers and DUART configuration");
    m->audio->write32(0,0xabcdef12);
    cpu.sr=0x2700;cpu.cycles=3ull*f3rt::Machine::main_clock;
    require(m->boundary()!=0 && m->audio->is_reset(),"Watchdog holds the sound CPU in reset");
    require(m->audio->read8(0x280019)==0x0f,"Watchdog restores the DUART interrupt vector");
    require(m->audio->read16(0x600)==0xa55a,"Whole-board reset preserves sound work RAM");
    require(m->audio->read32(0)==0,"Whole-board reset reloads boot vectors from sound ROM");
    m->audio->write8(0x260101,0);
    require(m->audio->read8(0x260001)==0 && m->audio->read8(0x260003)==0 &&
            m->audio->read8(0x260005)==0,"Watchdog clears DSP general-purpose registers");
    std::cout<<"PASS memory/lanes, input/coin, EEPROM protocol, IRQ/stack, native dispatch and real interpreter\n";
    return 0;
} catch(const std::exception &e) { std::cerr<<"FAIL "<<e.what()<<'\n';return 1; }
