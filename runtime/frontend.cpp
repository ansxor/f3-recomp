#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_video.hpp"
#include "interpreter.hpp"
#include "capture_io.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#ifdef F3RT_GENERATED
#include "program.h"
#endif

namespace {
struct Sdl {
    SDL_Window *window=nullptr;
    SDL_Renderer *renderer=nullptr;
    SDL_Texture *texture=nullptr;
    SDL_AudioStream *audio=nullptr;
    ~Sdl() { SDL_DestroyAudioStream(audio);SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit(); }
};
void check(bool result) { if(!result) throw std::runtime_error(SDL_GetError()); }
void key(f3rt::Machine &m, SDL_Scancode code, bool pressed) {
    switch(code) {
    case SDL_SCANCODE_UP:m.set_input(1,1,pressed);break;
    case SDL_SCANCODE_DOWN:m.set_input(1,2,pressed);break;
    case SDL_SCANCODE_LEFT:m.set_input(1,4,pressed);break;
    case SDL_SCANCODE_RIGHT:m.set_input(1,8,pressed);break;
    case SDL_SCANCODE_Z:m.set_input(0,1,pressed);break;
    case SDL_SCANCODE_X:m.set_input(0,2,pressed);break;
    case SDL_SCANCODE_C:m.set_input(0,4,pressed);break;
    case SDL_SCANCODE_1:m.set_input(0,0x1000,pressed);break;
    case SDL_SCANCODE_2:m.set_input(0,0x2000,pressed);break;
    case SDL_SCANCODE_F1:m.set_input(0,0x200,pressed);break;
    case SDL_SCANCODE_F2:if(pressed)m.system_inputs&=~2u;else m.system_inputs|=2;break;
    case SDL_SCANCODE_5:if(pressed)m.system_inputs&=~0x10u;else m.system_inputs|=0x10;break;
    case SDL_SCANCODE_6:if(pressed)m.system_inputs&=~0x20u;else m.system_inputs|=0x20;break;
    default:break;
    }
}
}
int main(int argc,char **argv) try {
    std::filesystem::path romdir,dumpdir,eeprom,wav_path,fallback_report,surface;
    std::string set="landmakrj";
    std::string video_mode="fdp";
    f3rt::GameVideoOptions video_options;
    std::string video_filter="nearest";
    uint64_t frames=0,dump_start=1,dump_every=1;
    bool headless=false,sound=true,translated=false,throttle=true;
#ifdef F3RT_LANDMAKR
    romdir=F3RT_DEFAULT_ROM_DIR;
    translated=true;
    bool allow_fallback=false;
#else
    bool allow_fallback=true;
#endif
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        auto value=[&]() -> const char * { if(i+1>=argc)throw std::runtime_error("Missing value for "+arg);return argv[++i]; };
        if(arg=="--rom-dir")romdir=value();
        else if(arg=="--set")set=value();
        else if(arg=="--frames")frames=std::stoull(value());
        else if(arg=="--dump-dir")dumpdir=value();
        else if(arg=="--dump-start")dump_start=std::stoull(value());
        else if(arg=="--dump-every")dump_every=std::stoull(value());
        else if(arg=="--eeprom")eeprom=value();
        else if(arg=="--wav")wav_path=value();
        else if(arg=="--fallback-report")fallback_report=value();
        else if(arg=="--surface")surface=value();
        else if(arg=="--video")video_mode=value();
        else if(arg=="--video-scale") {
            const auto scale=std::stoul(value());
            if(!scale || scale>f3rt::GameVideoOptions::max_scale)throw std::runtime_error("--video-scale must be 1..4");
            video_options.scale=unsigned(scale);
        }
        else if(arg=="--video-border") {
            const auto border=std::stoul(value());
            if(border>f3rt::GameVideoOptions::max_border)throw std::runtime_error("--video-border must be 0..160");
            video_options.border=unsigned(border);
        }
        else if(arg=="--video-filter")video_filter=value();
        else if(arg=="--headless")headless=true;
        else if(arg=="--no-audio")sound=false;
        else if(arg=="--translated")translated=true;
        else if(arg=="--allow-fallback")allow_fallback=true;
        else if(arg=="--unthrottled")throttle=false;
        else if(arg=="--help") {
            std::cout<<argv[0]<<" [--rom-dir DIR] [--set landmakrj|landmakr] [--frames N] [--headless] [--no-audio]\n"
                     <<"  [--translated] [--allow-fallback (diagnostic only)] [--unthrottled] [--eeprom FILE] [--wav FILE] [--surface BMP]\n"
                     <<"  [--dump-dir DIR --dump-start N --dump-every N] [--fallback-report TSV]\n"
                     <<"  [--video fdp|game|compare] (game data requires strict native landmakrj)\n"
                     <<"  [--video-scale 1..4] [--video-border 0..160] [--video-filter nearest|linear]\n"
                     <<"  Presentation options require game/compare; defaults: scale 1, border 0, nearest.\n"
                     <<"Arrows: move; Z/X/C: buttons; 1/2: start; 5/6: coin; F1: service; F2: test; Escape: quit.\n";
            return 0;
        } else throw std::runtime_error("Unknown argument: "+arg);
    }
    if(romdir.empty() || !dump_every)throw std::runtime_error("--rom-dir required; --dump-every must be positive");
    if(headless && !frames)throw std::runtime_error("Headless execution requires --frames");
#ifdef F3RT_LANDMAKR
    if(set!="landmakrj")throw std::runtime_error("This generated executable requires landmakrj");
#endif
    if(video_mode!="fdp" && video_mode!="game" && video_mode!="compare")
        throw std::runtime_error("--video must be fdp, game or compare");
    if(video_mode!="fdp" && (set!="landmakrj" || !translated || allow_fallback))
        throw std::runtime_error("Game-data video requires strict native landmakrj");
    if(video_filter!="nearest" && video_filter!="linear")throw std::runtime_error("--video-filter must be nearest or linear");
    if(video_mode=="fdp" && (video_options.expanded() || video_filter!="nearest"))
        throw std::runtime_error("Presentation enhancements require --video game or compare");
    auto machine=std::make_unique<f3rt::Machine>(f3rt::RomSet::load(romdir,set));
    auto &m=*machine;
    m.allow_main_fallback=allow_fallback;
    if(video_mode!="fdp")
        m.game_video=std::make_unique<f3rt::GameVideo>(m,video_mode=="game"?f3rt::GameVideoMode::Game:f3rt::GameVideoMode::Compare,video_options);
    if(!eeprom.empty())m.load_eeprom(eeprom);
    if(!fallback_report.empty())m.fallback_hits.resize(0x800000);
    if(translated) {
#ifdef F3RT_GENERATED
        if(!f3_generated_register(&m.cpu))throw std::runtime_error("Generated block registration failed");
#else
        throw std::runtime_error("This binary was built without F3_GENERATED_DIR");
#endif
    }
    Sdl sdl;
    uint32_t audio_rate=m.audio->sample_rate();
    if(!headless) {
        check(SDL_Init(SDL_INIT_VIDEO|(sound?SDL_INIT_AUDIO:0)));
        check(SDL_CreateWindowAndRenderer(("f3rt — "+set).c_str(),int((320+video_options.border*2)*3),696,SDL_WINDOW_RESIZABLE,&sdl.window,&sdl.renderer));
        check(SDL_SetRenderLogicalPresentation(sdl.renderer,int(video_options.width()),int(video_options.height()),SDL_LOGICAL_PRESENTATION_LETTERBOX));
        sdl.texture=SDL_CreateTexture(sdl.renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,int(video_options.width()),int(video_options.height()));
        check(sdl.texture!=nullptr);
        check(SDL_SetTextureScaleMode(sdl.texture,video_filter=="linear"?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST));
        if(sound) {
            SDL_AudioSpec spec{SDL_AUDIO_S16,2,int(audio_rate)};
            sdl.audio=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr);
            check(sdl.audio!=nullptr);check(SDL_ResumeAudioStreamDevice(sdl.audio));
        }
        std::cout<<"window_open video_driver="<<SDL_GetCurrentVideoDriver()<<" renderer="<<SDL_GetRendererName(sdl.renderer)
                 <<" video="<<video_mode<<" internal="<<video_options.width()<<'x'<<video_options.height()<<" filter="<<video_filter<<'\n';
    }
    std::unique_ptr<f3rt::WavWriter> wav;
    if(!wav_path.empty())wav=std::make_unique<f3rt::WavWriter>(wav_path,audio_rate);
    std::array<int16_t,8192> samples{};
    uint64_t audio_frames=0,nonzero_samples=0;
    int audio_peak=0;
    bool quit=false;
    const auto start=std::chrono::steady_clock::now();
    while(!quit && (!frames || m.frame<frames)) {
        if(!headless) {
            SDL_Event event;
            while(SDL_PollEvent(&event)) {
                if(event.type==SDL_EVENT_QUIT)quit=true;
                if(event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP) {
                    if(event.key.scancode==SDL_SCANCODE_ESCAPE)quit=true;
                    if(!event.key.repeat)key(m,event.key.scancode,event.type==SDL_EVENT_KEY_DOWN);
                }
                if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST) { m.inputs.fill(0xffffffff);m.system_inputs=0xff; }
            }
        }
        if(!m.run_frame(translated))throw std::runtime_error("CPU halted at "+std::to_string(m.cpu.pc));
        if(!dumpdir.empty() && m.frame>=dump_start && (m.frame-dump_start)%dump_every==0)f3rt::dump_machine(m,dumpdir);
        size_t count;
        while((count=m.audio->render(samples.data(),samples.size()/2))!=0) {
            audio_frames+=count;
            for(size_t i=0;i<count*2;++i) { audio_peak=std::max(audio_peak,std::abs(int(samples[i])));nonzero_samples+=samples[i]!=0; }
            if(wav)wav->append(std::span(samples.data(),count*2));
            if(sdl.audio)check(SDL_PutAudioStreamData(sdl.audio,samples.data(),int(count*4)));
        }
        if(!headless) {
            const auto pixels=m.game_video?m.game_video->presentation():std::span<const uint32_t>(m.pixels);
            check(SDL_UpdateTexture(sdl.texture,nullptr,pixels.data(),int(video_options.width()*4)));
            check(SDL_RenderClear(sdl.renderer));check(SDL_RenderTexture(sdl.renderer,sdl.texture,nullptr,nullptr));
            if(!surface.empty() && frames && m.frame==frames) {
                SDL_Surface *shot=SDL_RenderReadPixels(sdl.renderer,nullptr);
                check(shot!=nullptr);const bool saved=SDL_SaveBMP(shot,surface.string().c_str());SDL_DestroySurface(shot);check(saved);
            }
            check(SDL_RenderPresent(sdl.renderer));
            if(throttle)std::this_thread::sleep_until(start+std::chrono::nanoseconds(uint64_t(double(m.frame)*1e9*f3rt::Machine::frame_pixels/f3rt::Machine::pixel_clock)));
        }
    }
    if(!eeprom.empty())m.save_eeprom(eeprom);
    if(!fallback_report.empty()) {
        std::ofstream report(fallback_report);
        report<<"pc\tcount\n";
        for(size_t i=0;i<m.fallback_hits.size();++i)if(m.fallback_hits[i])report<<"0x"<<std::hex<<i*2<<std::dec<<'\t'<<m.fallback_hits[i]<<'\n';
        if(!report)throw std::runtime_error("Fallback report write failed");
    }
    if(m.game_video)m.game_video->report(std::cout);
    std::cout<<"set="<<set<<" frames="<<m.frame<<" pc=0x"<<std::hex<<m.cpu.pc<<" sound_pc=0x"<<m.interpreter->sound_pc()
             <<" frame_crc=0x"<<f3rt::crc32(reinterpret_cast<const uint8_t *>(m.pixels.data()),m.pixels.size()*4)<<std::dec
             <<" cycles="<<m.cpu.cycles<<" native_blocks="<<m.native_blocks<<" fallback_instructions="<<m.fallback_instructions
             <<" audio_frames="<<audio_frames<<" audio_peak="<<audio_peak<<" nonzero_samples="<<nonzero_samples<<'\n';
    return 0;
} catch(const std::exception &e) { std::cerr<<"f3rt: "<<e.what()<<'\n';return 1; }
