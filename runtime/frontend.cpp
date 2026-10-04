#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_video.hpp"
#include "f3rt/netplay.hpp"
#include "interpreter.hpp"
#include "sound_trace.hpp"
#include "capture_io.hpp"
#include "block_profile.hpp"
#ifdef F3RT_GPU
#include "gpu_video.hpp"
#include "f3rt/video.hpp"
#endif
#include <SDL3/SDL.h>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#ifdef F3RT_GENERATED
#include "program.h"
#endif
#ifdef F3RT_SOUND_GENERATED
#include "sound_program.h"
#endif

namespace {
struct Sdl {
    SDL_Window *window=nullptr;
    SDL_Renderer *renderer=nullptr;
    SDL_Texture *texture=nullptr;
    SDL_AudioStream *audio=nullptr;
#ifdef F3RT_GPU
    std::unique_ptr<f3rt::GpuVideo> gpu;
#endif
    ~Sdl() {
#ifdef F3RT_GPU
        gpu.reset();
#endif
        SDL_DestroyAudioStream(audio);SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    }
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
void netplay_key(f3rt::netplay::InputWord &word, SDL_Scancode code, bool pressed) {
    unsigned bit;
    switch (code) {
    case SDL_SCANCODE_UP: bit=0;break;
    case SDL_SCANCODE_DOWN: bit=1;break;
    case SDL_SCANCODE_LEFT: bit=2;break;
    case SDL_SCANCODE_RIGHT: bit=3;break;
    case SDL_SCANCODE_Z: bit=4;break;
    case SDL_SCANCODE_X: bit=5;break;
    case SDL_SCANCODE_C: bit=6;break;
    case SDL_SCANCODE_1: case SDL_SCANCODE_2: bit=7;break;
    case SDL_SCANCODE_5: case SDL_SCANCODE_6: bit=8;break;
    case SDL_SCANCODE_F1: bit=9;break;
    case SDL_SCANCODE_F2: bit=10;break;
    default: return;
    }
    if (pressed) word |= uint16_t(1u<<bit); else word &= uint16_t(~(1u<<bit));
}
}
int main(int argc,char **argv) try {
    std::filesystem::path romdir,dumpdir,eeprom,wav_path,fallback_report,surface;
    std::filesystem::path sound_trace_path,profile_path;
    std::string set="landmakrj";
    std::string video_mode="fdp";
    bool video_explicit=false;
    std::string sound_driver="oracle";
    bool sound_explicit=false;
    f3rt::GameVideoOptions video_options;
    std::string video_filter="nearest";
    std::string video_backend="cpu";
    std::string video_interp="off";
    f3rt::netplay::TransportOptions net_options;
    bool net_option_seen=false;
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
        else if(arg=="--sound-trace")sound_trace_path=value();
        else if(arg=="--profile-out")profile_path=value();
        else if(arg=="--sound-driver") { sound_driver=value();sound_explicit=true; }
        else if(arg=="--fallback-report")fallback_report=value();
        else if(arg=="--surface")surface=value();
        else if(arg=="--video") { video_mode=value();video_explicit=true; }
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
        else if(arg=="--video-backend")video_backend=value();
        else if(arg=="--video-interp")video_interp=value();
        else if(arg=="--netplay-server") { net_options.server=value();net_option_seen=true; }
        else if(arg=="--netplay-room") { net_options.room=value();net_option_seen=true; }
        else if(arg=="--netplay-player") {
            const auto player=std::stoul(value());
            if(player<1 || player>2)throw std::runtime_error("--netplay-player must be 1 or 2");
            net_options.player=unsigned(player);net_option_seen=true;
        }
        else if(arg=="--netplay-delay") {
            const auto delay=std::stoul(value());
            if(delay>8)throw std::runtime_error("--netplay-delay must be 0..8");
            net_options.delay=unsigned(delay);net_option_seen=true;
        }
        else if(arg=="--headless")headless=true;
        else if(arg=="--no-audio")sound=false;
        else if(arg=="--translated")translated=true;
        else if(arg=="--allow-fallback")allow_fallback=true;
        else if(arg=="--unthrottled")throttle=false;
        else if(arg=="--help") {
            std::cout<<argv[0]<<" [--rom-dir DIR] [--set landmakrj|landmakr] [--frames N] [--headless] [--no-audio]\n"
                     <<"  [--translated] [--allow-fallback (diagnostic only)] [--unthrottled] [--eeprom FILE] [--wav FILE] [--surface BMP]\n"
                     <<"  [--dump-dir DIR --dump-start N --dump-every N] [--fallback-report TSV]\n"
                     <<"  [--profile-out FILE] (instrumented build: merged entry counts, atomic flush every 30s and at exit)\n"
                     <<"  [--sound-trace FILE] [--sound-driver oracle|native] (default native in landmakr; oracle in f3rt-run)\n"
                     <<"  [--video fdp|game|compare] (game data requires strict native landmakrj)\n"
                     <<"  [--video-scale 1..4] [--video-border 0..160] [--video-filter nearest|linear]\n"
                     <<"  [--video-backend cpu|gpu] (presentation only; headless/captures retain CPU pixels)\n"
                     <<"  [--video-interp off|linear|fit] (opt-in GPU PF2 water sampling; default off)\n"
                     <<"  Presentation options require game/compare; defaults: scale 1, border 0, nearest.\n"
                     <<"  [--netplay-server HOST:PORT --netplay-room CODE --netplay-player 1|2 --netplay-delay 0..8]\n"
                     <<"  Netplay: strict native game video/sound, factory-reset EEPROM, no local-only inputs.\n"
                     <<"Arrows: move; Z/X/C: buttons; 1/2: start; 5/6: coin; F1: service; F2: test; Escape: quit.\n";
            return 0;
        } else throw std::runtime_error("Unknown argument: "+arg);
    }
    if(romdir.empty() || !dump_every)throw std::runtime_error("--rom-dir required; --dump-every must be positive");
    if(headless && !frames)throw std::runtime_error("Headless execution requires --frames");
#ifdef F3RT_SOUND_GENERATED
    // Builds that generated the sound program default to the recompiled driver;
    // --sound-driver oracle selects the interpreted reference.
    if(!sound_explicit)sound_driver="native";
#endif
    if(sound_driver!="oracle" && sound_driver!="native")
        throw std::runtime_error("--sound-driver must be oracle or native");
#ifdef F3RT_LANDMAKR
    if(set!="landmakrj")throw std::runtime_error("This generated executable requires landmakrj");
#endif
#ifdef F3RT_LANDMAKR
    // The strict-native Land Maker executable renders from game data by default;
    // diagnostic fallback execution has no producer hooks, so it keeps the FDP renderer.
    if(!video_explicit && translated && !allow_fallback)video_mode="game";
#endif
    if(video_mode!="fdp" && video_mode!="game" && video_mode!="compare")
        throw std::runtime_error("--video must be fdp, game or compare");
    if(video_mode!="fdp" && (set!="landmakrj" || !translated || allow_fallback))
        throw std::runtime_error("Game-data video requires strict native landmakrj");
    if(video_filter!="nearest" && video_filter!="linear")throw std::runtime_error("--video-filter must be nearest or linear");
    if(video_mode=="fdp" && (video_options.expanded() || video_filter!="nearest"))
        throw std::runtime_error("Presentation enhancements require --video game or compare");
    if(video_backend!="cpu" && video_backend!="gpu")throw std::runtime_error("--video-backend must be cpu or gpu");
    if(video_backend=="gpu" && video_mode=="fdp")throw std::runtime_error("GPU presentation requires --video game or compare");
    if(video_interp!="off" && video_interp!="linear" && video_interp!="fit")
        throw std::runtime_error("--video-interp must be off, linear or fit");
    if(video_interp!="off" && video_backend!="gpu")throw std::runtime_error("--video-interp requires --video-backend gpu");
#ifndef F3RT_GPU
    if(video_backend=="gpu" && !headless)throw std::runtime_error("GPU presentation requires F3RT_GPU build support");
#endif
    const bool netplay=net_option_seen;
    if(netplay && (net_options.server.empty() || net_options.room.empty()))
        throw std::runtime_error("Netplay requires --netplay-server and --netplay-room");
    if(netplay && (!translated || allow_fallback || video_mode!="game" || video_options.expanded() ||
                   sound_driver!="native" || !eeprom.empty() || !sound_trace_path.empty() || !fallback_report.empty()))
        throw std::runtime_error("Netplay requires strict-native game video/native sound at scale 1, border 0; EEPROM persistence and diagnostic traces are disabled");
#ifdef F3_PROFILE_SLIM_ENABLED
    if(allow_fallback || !translated || sound_driver!="native")
        throw std::runtime_error("Profile-slim requires strict native main and sound CPUs; no interpreter fallback");
#endif
#ifdef F3_PROFILE_INSTRUMENT
    if(!profile_path.empty() && (!translated || allow_fallback || sound_driver!="native"))
        throw std::runtime_error("Profiling requires strict native main and sound CPUs");
#endif
    if(netplay && frames>=UINT32_MAX-1024u)throw std::runtime_error("Netplay frame limit exceeds protocol range");
    auto machine=std::make_unique<f3rt::Machine>(f3rt::RomSet::load(romdir,set));
    auto &m=*machine;
    f3rt::BlockProfileSession profile(m.roms,profile_path);
    if(!sound_trace_path.empty())m.sound_trace=std::make_unique<f3rt::SoundTrace>(sound_trace_path);
    if(sound_driver=="native") {
#ifdef F3RT_SOUND_GENERATED
        m.use_native_sound(f3_sound_blocks,f3_sound_block_count);
#else
        throw std::runtime_error("Native sound requires a generated sound program (F3_ROM_DIR)");
#endif
    }
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
#ifdef F3RT_GPU
        if(video_backend=="gpu") {
            sdl.window=SDL_CreateWindow(("f3rt — "+set).c_str(),int((320+video_options.border*2)*3),696,SDL_WINDOW_RESIZABLE);
            check(sdl.window!=nullptr);
            sdl.gpu=std::make_unique<f3rt::GpuVideo>(sdl.window,video_options,m.video->playfield_tiles(),
                                                   m.video->sprite_tiles(),video_filter=="linear",throttle,
                                                   video_interp=="fit"?f3rt::VideoInterpolation::Fit:
                                                   video_interp=="linear"?f3rt::VideoInterpolation::Linear:f3rt::VideoInterpolation::Off);
            m.game_video->enable_gpu_presentation();
        } else
#endif
        {
            check(SDL_CreateWindowAndRenderer(("f3rt — "+set).c_str(),int((320+video_options.border*2)*3),696,SDL_WINDOW_RESIZABLE,&sdl.window,&sdl.renderer));
            check(SDL_SetRenderLogicalPresentation(sdl.renderer,int(video_options.width()),int(video_options.height()),SDL_LOGICAL_PRESENTATION_LETTERBOX));
            sdl.texture=SDL_CreateTexture(sdl.renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,int(video_options.width()),int(video_options.height()));
            check(sdl.texture!=nullptr);
            check(SDL_SetTextureScaleMode(sdl.texture,video_filter=="linear"?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST));
        }
        if(sound) {
            SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES,"512");
            SDL_AudioSpec spec{SDL_AUDIO_S16,2,int(audio_rate)};
            sdl.audio=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr);
            check(sdl.audio!=nullptr);check(SDL_ResumeAudioStreamDevice(sdl.audio));
        }
        std::cout<<"window_open video_driver="<<SDL_GetCurrentVideoDriver()<<" backend="<<video_backend
                 <<" video="<<video_mode<<" internal="<<video_options.width()<<'x'<<video_options.height()
                 <<" filter="<<video_filter<<" interp="<<video_interp<<'\n';
    }
    std::unique_ptr<f3rt::WavWriter> wav;
    if(!wav_path.empty())wav=std::make_unique<f3rt::WavWriter>(wav_path,audio_rate);
    std::array<int16_t,8192> samples{};
    uint64_t audio_frames=0,nonzero_samples=0;
    int audio_peak=0;
    bool quit=false;
    std::unique_ptr<f3rt::netplay::Transport> transport;
    std::unique_ptr<f3rt::netplay::Rollback> rollback;
    f3rt::netplay::InputWord local_word=0;
    if(netplay)transport=std::make_unique<f3rt::netplay::Transport>(net_options,f3rt::netplay::machine_identity(m,net_options.delay));
    bool finish_sent=false;
    auto next_net_step=std::chrono::steady_clock::now();
    auto next_status=next_net_step;
    auto next_frame=std::chrono::steady_clock::now();
    constexpr int max_audio_queue_ms=50,max_catchup_ms=50;
    uint64_t audio_queue_drops=0,clock_resyncs=0,audio_queue_sum=0,audio_queue_samples=0,audio_queue_max=0;
    const auto start=std::chrono::steady_clock::now();
    while(!quit && ((!frames || m.frame<frames) || (transport && !transport->finished()))) {
        profile.tick();
        if(!headless) {
            SDL_Event event;
            while(SDL_PollEvent(&event)) {
                if(event.type==SDL_EVENT_QUIT)quit=true;
                if(event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_KEY_UP) {
                    if(event.key.scancode==SDL_SCANCODE_ESCAPE)quit=true;
                    if(!event.key.repeat) {
                        if(netplay)netplay_key(local_word,event.key.scancode,event.type==SDL_EVENT_KEY_DOWN);
                        else key(m,event.key.scancode,event.type==SDL_EVENT_KEY_DOWN);
                    }
                }
                if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST) {
                    if(netplay)local_word=0;
                    else { m.inputs.fill(0xffffffff);m.system_inputs=0xff; }
                }
            }
        }
        bool advanced=false;
        if(transport) {
            try {
                transport->pump(uint32_t(m.frame),rollback?rollback->confirmed_frame():0);
                if(transport->ready() && !rollback) {
                    rollback=std::make_unique<f3rt::netplay::Rollback>(m,transport->slot(),net_options.delay);
                    std::cout<<"netplay_ready player="<<transport->slot()+1<<" delay="<<net_options.delay<<'\n';
                    next_net_step=std::chrono::steady_clock::now();
                }
                if(rollback) {
                    f3rt::netplay::Input input;
                    while(transport->receive(input))rollback->receive(input);
                    f3rt::netplay::Checksum checksum;
                    while(transport->receive_checksum(checksum))rollback->receive_checksum(checksum);
                    const auto previous_rollbacks=rollback->rollback_count();
                    rollback->synchronize();
                    advanced=rollback->rollback_count()!=previous_rollbacks;
                    const auto now=std::chrono::steady_clock::now();
                    // The peer's advertised frame is already one transit old.
                    // Allow that age plus two frames before yielding to a slower peer.
                    const int lead_limit=throttle?2+int(transport->rtt_ms()*f3rt::Machine::pixel_clock/
                        (2000.0*f3rt::Machine::frame_pixels)+0.999):16;
                    if((!frames || m.frame<frames) && (!throttle || now>=next_net_step) && transport->frame_advantage()<=lead_limit) {
                        if(rollback->needs_local_input())transport->submit(rollback->local_input(local_word));
                        const bool stepped=rollback->advance();
                        advanced|=stepped;
                        if(stepped)next_net_step=std::max(next_net_step,now)+std::chrono::nanoseconds(
                            uint64_t(1e9*f3rt::Machine::frame_pixels/f3rt::Machine::pixel_clock));
                    }
                    while(rollback->receive_checksum_to_send(checksum))transport->checksum(checksum);
                    if(frames && m.frame==frames && rollback->confirmed_frame()==frames && !finish_sent) {
                        transport->finish(uint32_t(frames),m.state_crc());finish_sent=true;
                    }
                }
                const auto now=std::chrono::steady_clock::now();
                if(now>=next_status) {
                    const std::string status=transport->status()+" ping="+std::to_string(int(transport->rtt_ms()))+
                        "ms rollback="+std::to_string(rollback?rollback->last_rollback_depth():0);
                    if(sdl.window)check(SDL_SetWindowTitle(sdl.window,("Land Maker — "+status).c_str()));
                    next_status=now+std::chrono::milliseconds(250);
                }
            } catch(const std::exception &error) {
                if(sdl.window)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Netplay stopped",error.what(),sdl.window);
                throw;
            }
        } else {
            if(!m.run_frame(translated))throw std::runtime_error("CPU halted at "+std::to_string(m.cpu.pc));
            advanced=true;
        }
        if(advanced && !dumpdir.empty() && m.frame>=dump_start && (m.frame-dump_start)%dump_every==0)f3rt::dump_machine(m,dumpdir);
        size_t count;
        while((count=rollback?rollback->render_audio(samples.data(),samples.size()/2):
               m.audio->render(samples.data(),samples.size()/2))!=0) {
            audio_frames+=count;
            for(size_t i=0;i<count*2;++i) { audio_peak=std::max(audio_peak,std::abs(int(samples[i])));nonzero_samples+=samples[i]!=0; }
            if(wav)wav->append(std::span(samples.data(),count*2));
            if(sdl.audio) {
                // A stalled window (tab-out, fullscreen switch) must not leave seconds of audio queued ahead of the picture.
                // Past the latency cap, drop the stale backlog so new sound plays now.
                if(throttle && SDL_GetAudioStreamQueued(sdl.audio)>int(audio_rate*4*max_audio_queue_ms/1000)) {
                    check(SDL_ClearAudioStream(sdl.audio));++audio_queue_drops;
                }
                check(SDL_PutAudioStreamData(sdl.audio,samples.data(),int(count*4)));
                const uint64_t queued=uint64_t(SDL_GetAudioStreamQueued(sdl.audio));
                audio_queue_sum+=queued;++audio_queue_samples;audio_queue_max=std::max(audio_queue_max,queued);
            }
        }
        if(!headless && advanced) {
#ifdef F3RT_GPU
            if(sdl.gpu) {
                sdl.gpu->draw(m.game_video->gpu_scene());
                if(!surface.empty() && frames && m.frame==frames)sdl.gpu->save_surface(surface.string().c_str());
            } else
#endif
            {
                const auto pixels=m.game_video?m.game_video->presentation():std::span<const uint32_t>(m.pixels);
                check(SDL_UpdateTexture(sdl.texture,nullptr,pixels.data(),int(video_options.width()*4)));
                check(SDL_RenderClear(sdl.renderer));check(SDL_RenderTexture(sdl.renderer,sdl.texture,nullptr,nullptr));
                if(!surface.empty() && frames && m.frame==frames) {
                    SDL_Surface *shot=SDL_RenderReadPixels(sdl.renderer,nullptr);
                    check(shot!=nullptr);const bool saved=SDL_SaveBMP(shot,surface.string().c_str());SDL_DestroySurface(shot);check(saved);
                }
                check(SDL_RenderPresent(sdl.renderer));
            }
            if(throttle && !netplay) {
                // Pace against a rolling deadline. After a stall longer than one catch-up window, resync to now instead of
                // running flat out to make up lost time (that burst is what delayed audio by the length of the stall).
                const auto frame_time=std::chrono::nanoseconds(uint64_t(1e9*f3rt::Machine::frame_pixels/f3rt::Machine::pixel_clock));
                next_frame+=frame_time;
                const auto now=std::chrono::steady_clock::now();
                if(now-next_frame>std::chrono::milliseconds(max_catchup_ms)) { next_frame=now;++clock_resyncs; }
                std::this_thread::sleep_until(next_frame);
            }
        }
        if(transport && !advanced)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if(!eeprom.empty())m.save_eeprom(eeprom);
    if(!fallback_report.empty()) {
        std::ofstream report(fallback_report);
        report<<"pc\tcount\n";
        for(size_t i=0;i<m.fallback_hits.size();++i)if(m.fallback_hits[i])report<<"0x"<<std::hex<<i*2<<std::dec<<'\t'<<m.fallback_hits[i]<<'\n';
        if(!report)throw std::runtime_error("Fallback report write failed");
    }
    if(m.game_video)m.game_video->report(std::cout);
    if(rollback)std::cout<<"netplay_confirmed="<<rollback->confirmed_frame()<<" state_crc="<<m.state_crc()
        <<" rollbacks="<<rollback->rollback_count()<<" max_rollback_depth="<<rollback->maximum_rollback_depth()<<'\n';
    if(m.sound_trace)m.sound_trace->finish(m);
    profile.flush();
    std::cout<<"set="<<set<<" frames="<<m.frame<<" pc=0x"<<std::hex<<m.cpu.pc<<" sound_pc=0x"<<m.sound_pc()
             <<" sound_driver="<<sound_driver
             <<" frame_crc=0x"<<f3rt::crc32(reinterpret_cast<const uint8_t *>(m.pixels.data()),m.pixels.size()*4)<<std::dec
             <<" cycles="<<m.cpu.cycles<<" native_blocks="<<m.native_blocks<<" fallback_instructions="<<m.fallback_instructions
             <<" audio_frames="<<audio_frames<<" audio_peak="<<audio_peak<<" nonzero_samples="<<nonzero_samples<<'\n';
    if(audio_queue_samples)std::cerr<<"f3rt: pacing clock_resyncs="<<clock_resyncs<<" audio_queue_drops="<<audio_queue_drops
        <<" queued_ms mean="<<1000.0*double(audio_queue_sum)/double(audio_queue_samples)/(4.0*audio_rate)
        <<" max="<<1000.0*double(audio_queue_max)/(4.0*audio_rate)<<'\n';
    return 0;
} catch(const std::exception &e) { std::cerr<<"f3rt: "<<e.what()<<'\n';return 1; }
