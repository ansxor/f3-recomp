#include "f3rt/rom.hpp"
#include "f3rt/video.hpp"
#include "capture_io.hpp"
#include <algorithm>
#include <array>
#include <iostream>

int main(int argc, char **argv) try {
    std::filesystem::path romdir, captures, output;
    for (int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if ((arg=="--rom-dir" || arg=="--captures" || arg=="--output") && i+1<argc) {
            const std::filesystem::path value=argv[++i];
            if(arg=="--rom-dir") romdir=value;
            if(arg=="--captures") captures=value;
            if(arg=="--output") output=value;
        } else throw std::runtime_error("Usage: f3rt-replay --rom-dir DIR --captures DIR --output DIR");
    }
    if(romdir.empty() || captures.empty() || output.empty()) throw std::runtime_error("--rom-dir, --captures and --output are required");
    auto roms=f3rt::RomSet::load(romdir);
    f3rt::Video video;
    if(!video.load_roms(roms.sprites,roms.sprites_hi,roms.tiles,roms.tiles_hi)) throw std::runtime_error("Video ROM decode failed");
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
