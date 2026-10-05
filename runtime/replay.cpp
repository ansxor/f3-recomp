#include "f3rt/rom.hpp"
#include "f3rt/video.hpp"
#include "f3rt/audio.hpp"
#include "capture_io.hpp"
#include <algorithm>
#include <array>
#include <iostream>

namespace {
int replay_audio(const f3rt::RomSet &roms, const std::filesystem::path &trace,
                 const std::filesystem::path &output) {
    std::ifstream input(trace, std::ios::binary);
    std::array<char, 8> magic{};
    input.read(magic.data(), magic.size());
    if (magic != std::array<char, 8>{'F','3','A','U','D','2',0,0})
        throw std::runtime_error("Invalid F3 audio trace header");
    f3rt::Audio audio;
    audio.load_sample_rom(roms.samples);
    f3rt::WavWriter wav(output, audio.sample_rate());
    std::array<int16_t, 1024> samples{};
    std::array<uint8_t, 16> record{};
    uint64_t now = 0, writes = 0, frames = 0;
    int peak = 0;
    bool ended = false;
    const auto little = [](const uint8_t *p, unsigned bytes) {
        uint64_t value = 0;
        for (unsigned i = 0; i < bytes; ++i) value |= uint64_t(p[i]) << (8 * i);
        return value;
    };
    while (input.read(reinterpret_cast<char *>(record.data()), record.size())) {
        const uint64_t at = little(record.data(), 8);
        if (at < now) throw std::runtime_error("Audio trace time moved backwards");
        while (now < at) {
            const auto cycles = uint32_t(std::min<uint64_t>(at - now, 16000));
            audio.advance(cycles);
            now += cycles;
            const size_t count = audio.render(samples.data(), samples.size() / 2);
            wav.append(std::span(samples).first(count * 2));
            frames += count;
            for (size_t i = 0; i < count * 2; ++i) peak = std::max(peak, std::abs(int(samples[i])));
        }
        const auto address = uint32_t(little(record.data() + 8, 4));
        const auto data = uint16_t(little(record.data() + 12, 2));
        const auto mask = uint16_t(little(record.data() + 14, 2));
        if (address == 0xffffffff) { ended = true; break; }
        if (address == 0xfffffffe) { audio.reset_board(); continue; }
        if (mask == 0xffff) audio.write16(address, data);
        else {
            if (mask & 0xff00) audio.write8(address, uint8_t(data >> 8));
            if (mask & 0x00ff) audio.write8(address + 1, uint8_t(data));
        }
        ++writes;
    }
    if (!ended) throw std::runtime_error("Truncated audio trace: missing end timestamp");
    std::cout << "audio_writes=" << writes << " frames=" << frames
              << " sample_rate=" << audio.sample_rate() << " peak=" << peak << '\n';
    return 0;
}
}

int main(int argc, char **argv) try {
    std::filesystem::path romdir, captures, output, audio_trace;
    for (int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if ((arg=="--rom-dir" || arg=="--captures" || arg=="--output" || arg=="--audio-trace") && i+1<argc) {
            const std::filesystem::path value=argv[++i];
            if(arg=="--rom-dir") romdir=value;
            if(arg=="--captures") captures=value;
            if(arg=="--output") output=value;
            if(arg=="--audio-trace") audio_trace=value;
        } else throw std::runtime_error("Usage: f3rt-replay --rom-dir DIR (--captures DIR | --audio-trace FILE) --output PATH");
    }
    if(romdir.empty() || output.empty() || (captures.empty() == audio_trace.empty()))
        throw std::runtime_error("--rom-dir, --output and exactly one of --captures/--audio-trace are required");
    auto roms=f3rt::RomSet::load(romdir);
    if (!audio_trace.empty()) return replay_audio(roms, audio_trace, output);
    f3rt::Video video;
    if(!video.load_roms(roms.sprites,roms.sprites_hi,roms.tiles,roms.tiles_hi,roms.video)) throw std::runtime_error("Video ROM decode failed");
    std::vector<std::filesystem::path> frames;
    if(std::filesystem::exists(captures/"graphics.bin")) frames.push_back(captures);
    else for(const auto &entry:std::filesystem::directory_iterator(captures))
        if(entry.is_directory() && std::filesystem::exists(entry.path()/"graphics.bin")) frames.push_back(entry.path());
    std::sort(frames.begin(),frames.end());
    if(frames.empty()) throw std::runtime_error("No capture frames found");
    std::array<uint8_t,0x8000> palette{};
    std::array<uint8_t,0x40000> graphics{};
    std::array<uint8_t,0x20> control{};
    std::array<uint8_t,0x10000> sprites{};
    std::array<uint32_t,320*232> pixels{};
    std::array<uint8_t,320*232*4> reference{};
    size_t total_bad=0;
    for(const auto &frame:frames) {
        f3rt::read_exact(frame/"palette.bin",palette);
        f3rt::read_exact(frame/"graphics.bin",graphics);
        f3rt::read_exact(frame/"control.bin",control);
        f3rt::read_exact(frame/"spriteram_active.bin",sprites);
        f3rt::read_exact(frame/"reference.argb",reference);
        video.reset();
        video.set_active_spriteram(sprites);
        video.render_frame(palette,graphics,control,pixels);
        const auto dir=output/frame.filename();
        std::filesystem::create_directories(dir);
        f3rt::write_argb(dir/"rendered.argb",pixels);
        f3rt::write_bmp(dir/"rendered.bmp",pixels);
        size_t bad=0;
        unsigned maximum=0;
        uint64_t error=0;
        for(size_t i=0;i<pixels.size();++i) {
            bool differs=false;
            for(unsigned c=0;c<3;++c) {
                const unsigned d=unsigned(std::abs(int((pixels[i]>>(8*c))&255)-int(reference[4*i+c])));
                maximum=std::max(maximum,d);error+=d;differs|=d!=0;
            }
            bad+=differs;
        }
        total_bad+=bad;
        std::cout<<frame.filename().string()<<": "<<bad<<"/"<<pixels.size()<<" mismatched pixels, max_channel_error="<<maximum
                 <<", mean_channel_error="<<double(error)/(pixels.size()*3)<<'\n';
    }
    std::cout<<"frames="<<frames.size()<<" total_mismatched_pixels="<<total_bad<<'\n';
    return total_bad ? 1 : 0;
} catch(const std::exception &e) { std::cerr<<e.what()<<'\n';return 2; }
