#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_video.hpp"
#include "f3rt/netplay_session.hpp"
#include "frontend_ui.hpp"
#include "frontend_state.hpp"
#include "interpreter.hpp"
#include "sound_trace.hpp"
#include "capture_io.hpp"
#include "video_scale.hpp"
#include "gpu_interp.hpp"
#include "block_profile.hpp"
#ifdef F3RT_GPU
#include "gpu_video.hpp"
#include "f3rt/video.hpp"
#endif
#include <SDL3/SDL.h>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
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
    std::unique_ptr<f3rt::InputMapper> input;
    std::unique_ptr<f3rt::FrontendUi> ui;
#ifdef F3RT_GPU
    std::unique_ptr<f3rt::GpuVideo> gpu;
#endif
    ~Sdl() {
        ui.reset();input.reset();
#ifdef F3RT_GPU
        gpu.reset();
#endif
        SDL_DestroyAudioStream(audio);SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    }
};
void check(bool result) { if(!result) throw std::runtime_error(SDL_GetError()); }
void parse_scale(const std::string &scale, f3rt::VideoScaleMode &mode, f3rt::GameVideoOptions &options) {
    if(scale=="auto" || scale=="auto-integer") {
        mode=scale=="auto"?f3rt::VideoScaleMode::Auto:f3rt::VideoScaleMode::AutoInteger;
        options.scale=1;
    } else {
        size_t consumed=0;
        unsigned long numeric=0;
        try { numeric=std::stoul(scale,&consumed); }
        catch(const std::exception &) { throw std::runtime_error("Video scale must be 1..4, auto or auto-integer"); }
        if(consumed!=scale.size() || !numeric || numeric>f3rt::GameVideoOptions::max_scale)
            throw std::runtime_error("Video scale must be 1..4, auto or auto-integer");
        options.scale=unsigned(numeric);
        mode=f3rt::VideoScaleMode::Fixed;
    }
}
enum Preference : uint32_t {
    VideoMode=1u<<0, Backend=1u<<1, Scale=1u<<2, Border=1u<<3, Filter=1u<<4,
    Interpolation=1u<<5, Fields=1u<<6, SoundDriver=1u<<7, Volume=1u<<8,
    Server=1u<<9, Room=1u<<10, Slot=1u<<11, Delay=1u<<12,
    PostprocessMode=1u<<13, UserShader=1u<<14
};
}
int main(int argc,char **argv) try {
    std::filesystem::path romdir,dumpdir,eeprom,wav_path,fallback_report,surface;
    std::filesystem::path sound_trace_path,profile_path,config_path;
    std::string set="landmakrj";
    std::string video_mode="fdp";
    std::string sound_driver="oracle";
    f3rt::GameVideoOptions video_options;
    f3rt::VideoScaleMode video_scale_mode=f3rt::VideoScaleMode::Fixed;
    std::string video_filter="nearest";
    std::string video_backend="cpu";
    std::string video_interp="off";
    std::string video_interp_fields="geometry";
    std::string video_scale="1";
    std::string postprocess="off",user_shader;
    float volume=1;
    uint32_t cli_preferences=0;
    f3rt::netplay::TransportOptions net_options;
    bool net_option_seen=false,net_role_seen=false;
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
        else if(arg=="--sound-driver") { sound_driver=value();cli_preferences|=SoundDriver; }
        else if(arg=="--config")config_path=value();
        else if(arg=="--volume") {
            const std::string text=value();size_t consumed=0;
            volume=std::stof(text,&consumed)/100;
            if(consumed!=text.size() || !std::isfinite(volume) || volume<0 || volume>1)
                throw std::runtime_error("--volume must be 0..100");
            cli_preferences|=Volume;
        }
        else if(arg=="--fallback-report")fallback_report=value();
        else if(arg=="--surface")surface=value();
        else if(arg=="--video") { video_mode=value();cli_preferences|=VideoMode; }
        else if(arg=="--video-scale") { video_scale=value();cli_preferences|=Scale; }
        else if(arg=="--video-border") {
            const auto border=std::stoul(value());
            if(border>f3rt::GameVideoOptions::max_border)throw std::runtime_error("--video-border must be 0..160");
            video_options.border=unsigned(border);cli_preferences|=Border;
        }
        else if(arg=="--video-filter") { video_filter=value();cli_preferences|=Filter; }
        else if(arg=="--video-backend") { video_backend=value();cli_preferences|=Backend; }
        else if(arg=="--video-interp") { video_interp=value();cli_preferences|=Interpolation; }
        else if(arg=="--video-interp-fields") { video_interp_fields=value();cli_preferences|=Fields; }
        else if(arg=="--postprocess") { postprocess=value();cli_preferences|=PostprocessMode; }
        else if(arg=="--user-shader") { user_shader=value();cli_preferences|=UserShader; }
        else if(arg=="--netplay-server") { net_options.server=value();net_option_seen=true;cli_preferences|=Server; }
        else if(arg=="--netplay-room") { net_options.room=value();net_option_seen=true;cli_preferences|=Room; }
        else if(arg=="--netplay-host" || arg=="--netplay-join") {
            if(net_role_seen)throw std::runtime_error("Choose one of --netplay-host or --netplay-join");
            net_options.host=arg=="--netplay-host";net_role_seen=net_option_seen=true;
        }
        else if(arg=="--netplay-player") {
            const auto player=std::stoul(value());
            if(player<1 || player>2)throw std::runtime_error("--netplay-player must be 1 or 2");
            net_options.player=unsigned(player);net_option_seen=true;cli_preferences|=Slot;
        }
        else if(arg=="--netplay-delay") {
            const auto delay=std::stoul(value());
            if(delay>8)throw std::runtime_error("--netplay-delay must be 0..8");
            net_options.delay=unsigned(delay);net_option_seen=true;cli_preferences|=Delay;
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
                     <<"  [--config FILE] [--volume 0..100] (user preferences load first; CLI overrides)\n"
                     <<"  [--profile-out FILE] (instrumented build: merged entry counts, atomic flush every 30s and at exit)\n"
                     <<"  [--sound-trace FILE] [--sound-driver oracle|native] (default native in landmakr; oracle in f3rt-run)\n"
                     <<"  [--video fdp|game|compare] (game data requires strict native landmakrj)\n"
                     <<"  [--video-scale 1..4|auto|auto-integer] [--video-border 0..160] [--video-filter nearest|linear]\n"
                     <<"  [--video-backend cpu|gpu] (presentation only; headless/captures retain CPU pixels)\n"
                     <<"  [--video-interp off|linear|fit] (opt-in GPU line sampling; default off)\n"
                     <<"  [--video-interp-fields none|geometry|palette|geometry,palette] (default geometry; native alpha stays discrete)\n"
                     <<"  [--postprocess off|crt|user] [--user-shader FILE.metal|FILE.spv] (GPU only, default off)\n"
                     <<"  Presentation options require game/compare; defaults: scale 1, border 0, nearest.\n"
                     <<"  Auto scales follow window pixels (GPU only); auto-integer uses nearest filtering.\n"
                     <<"  [--netplay-host|--netplay-join --netplay-server HOST:PORT --netplay-room CODE]\n"
                     <<"  [--netplay-player 1|2 --netplay-delay 0..8] (host delay; independent local histories)\n"
                     <<"  Netplay starts at 2P selection using a host snapshot; match end/disconnect returns to solo.\n"
                     <<"Arrows: move; Z/X/C: buttons; 1/2: start; 5/6: coin; F3: service; F2: test.\n"
                     <<"F1: menu (pauses solo); F12: screenshot; Escape: close menu or quit.\n"
                     <<"F11 or Alt+Enter: toggle fullscreen. Menu includes persisted keyboard/gamepad remapping.\n";
            return 0;
        } else throw std::runtime_error("Unknown argument: "+arg);
    }
    if(romdir.empty() || !dump_every)throw std::runtime_error("--rom-dir required; --dump-every must be positive");
    if(headless && !frames)throw std::runtime_error("Headless execution requires --frames");
    f3rt::FrontendSettings settings;
#if defined(F3RT_SOUND_GENERATED) && defined(F3RT_LANDMAKR)
    settings.audio_backend=f3rt::AudioBackend::Native;
#endif
#ifdef F3RT_LANDMAKR
    if(translated && !allow_fallback)settings.video_mode="game";
#endif
    if(config_path.empty())config_path=f3rt::default_config_path();
    std::string settings_error;
    if(!config_path.empty() && !f3rt::load_frontend_settings(config_path.string(),settings,settings_error))
        std::cerr<<"f3rt: ignoring config "<<config_path<<": "<<settings_error<<'\n';
    auto preference=[&](auto &option,auto &saved,Preference field) {
        if(cli_preferences&field)saved=option;else option=saved;
    };
    preference(video_mode,settings.video_mode,VideoMode);
    preference(video_backend,settings.video_backend,Backend);
    preference(video_scale,settings.video_scale,Scale);
    preference(video_options.border,settings.border,Border);
    preference(video_filter,settings.filter,Filter);
    preference(video_interp,settings.interpolation,Interpolation);
    preference(video_interp_fields,settings.interpolation_fields,Fields);
    preference(postprocess,settings.postprocess,PostprocessMode);
    preference(user_shader,settings.user_shader,UserShader);
    preference(volume,settings.volume,Volume);
    preference(net_options.server,settings.server,Server);
    preference(net_options.room,settings.room,Room);
    preference(net_options.player,settings.requested_slot,Slot);
    preference(net_options.delay,settings.delay,Delay);
    if(cli_preferences&SoundDriver)
        settings.audio_backend=sound_driver=="native"?f3rt::AudioBackend::Native:
            sound_driver=="hle"?f3rt::AudioBackend::Hle:f3rt::AudioBackend::Oracle;
    else sound_driver=f3rt::audio_backend_name(settings.audio_backend);
    parse_scale(video_scale,video_scale_mode,video_options);
    if(sound_driver!="oracle" && sound_driver!="native")
        throw std::runtime_error("--sound-driver must be oracle or native");
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
    if(video_backend!="cpu" && video_backend!="gpu")throw std::runtime_error("--video-backend must be cpu or gpu");
    bool automatic_scale=video_scale_mode!=f3rt::VideoScaleMode::Fixed;
    if(automatic_scale && video_backend!="gpu")throw std::runtime_error("--video-scale auto/auto-integer requires --video-backend gpu");
    if(video_backend=="gpu" && video_mode=="fdp")throw std::runtime_error("GPU presentation requires --video game or compare");
    if(video_interp!="off" && video_interp!="linear" && video_interp!="fit")
        throw std::runtime_error("--video-interp must be off, linear or fit");
    if(video_interp!="off" && video_backend!="gpu")throw std::runtime_error("--video-interp requires --video-backend gpu");
    if(postprocess!="off" && postprocess!="crt" && postprocess!="user")
        throw std::runtime_error("--postprocess must be off, crt or user");
    if(postprocess!="off" && video_backend!="gpu")
        std::cerr<<"f3rt: post-processing requires GPU backend; preference is inactive on CPU\n";
    const auto interpolation_fields=f3rt::parse_interpolation_fields(video_interp_fields);
    if(!interpolation_fields)throw std::runtime_error("--video-interp-fields must be none, geometry, palette or geometry,palette");
#ifndef F3RT_GPU
    if(video_backend=="gpu" && !headless)throw std::runtime_error("GPU presentation requires F3RT_GPU build support");
#endif
    const bool netplay=net_option_seen;
    if(netplay && (net_options.server.empty() || net_options.room.empty()))
        throw std::runtime_error("Netplay requires --netplay-server and --netplay-room");
    if(netplay && !net_role_seen)throw std::runtime_error("Netplay requires --netplay-host or --netplay-join");
    if(netplay && (!translated || allow_fallback || !sound_trace_path.empty()))
        throw std::runtime_error("Netplay requires strict-native main execution without sound tracing");
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
        m.use_native_sound(f3_sound_blocks,f3_sound_block_count,
                           {f3_sound_excluded_ranges,f3_sound_excluded_count});
#else
        throw std::runtime_error("Native sound requires a generated sound program (F3_ROM_DIR)");
#endif
    }
    m.allow_main_fallback=allow_fallback;
    if(video_mode!="fdp")
        m.game_video=std::make_unique<f3rt::GameVideo>(m,video_mode=="game"?f3rt::GameVideoMode::Game:f3rt::GameVideoMode::Compare,video_options);
    const auto snapshot_video_options=video_options;
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
    int pixel_width=0,pixel_height=0;
    uint32_t audio_rate=m.audio->sample_rate();
    if(!headless) {
        check(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD|(sound?SDL_INIT_AUDIO:0)));
#ifdef F3RT_GPU
        if(video_backend=="gpu") {
            const SDL_WindowFlags flags=SDL_WINDOW_RESIZABLE|(automatic_scale?SDL_WINDOW_HIGH_PIXEL_DENSITY:0);
            sdl.window=SDL_CreateWindow(("f3rt — "+set).c_str(),int((320+video_options.border*2)*3),696,flags);
            check(sdl.window!=nullptr);
            m.game_video->enable_gpu_presentation();
            if(automatic_scale) {
                int pixel_width=0,pixel_height=0;
                check(SDL_GetWindowSizeInPixels(sdl.window,&pixel_width,&pixel_height));
                video_options.scale=f3rt::video_scale_for_window(video_scale_mode,unsigned(std::max(pixel_width,0)),
                                                               unsigned(std::max(pixel_height,0)),video_options.border);
            }
            m.game_video->set_gpu_scale(video_options.scale);
            sdl.gpu=std::make_unique<f3rt::GpuVideo>(sdl.window,video_options,m.video->playfield_tiles(),
                                                   m.video->sprite_tiles(),video_filter=="linear",throttle,
                                                   video_interp=="fit"?f3rt::VideoInterpolation::Fit:
                                                   video_interp=="linear"?f3rt::VideoInterpolation::Linear:f3rt::VideoInterpolation::Off,
                                                   *interpolation_fields);
            sdl.gpu->set_scale_mode(video_scale_mode);
            if(postprocess!="off")sdl.gpu->set_postprocess(
                postprocess=="crt"?f3rt::Postprocess::Crt:f3rt::Postprocess::User,user_shader);
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
            check(SDL_SetAudioStreamGain(sdl.audio,volume));
        }
        check(SDL_GetWindowSizeInPixels(sdl.window,&pixel_width,&pixel_height));
        sdl.input=std::make_unique<f3rt::InputMapper>(settings);
        SDL_GPUDevice *ui_device=nullptr;
#ifdef F3RT_GPU
        if(sdl.gpu)ui_device=sdl.gpu->device();
#endif
        sdl.ui=std::make_unique<f3rt::FrontendUi>(sdl.window,sdl.renderer,ui_device,settings,*sdl.input);
        std::cout<<"window_open video_driver="<<SDL_GetCurrentVideoDriver()<<" backend="<<video_backend
                 <<" video="<<video_mode<<" internal="<<video_options.width()<<'x'<<video_options.height()
                 <<" pixels="<<pixel_width<<'x'<<pixel_height<<" scale="<<video_options.scale
                 <<" filter="<<video_filter<<" interp="<<video_interp
                 <<" interp_fields="<<f3rt::interpolation_fields_name(*interpolation_fields)<<'\n';
    }
    std::unique_ptr<f3rt::WavWriter> wav;
    if(!wav_path.empty())wav=std::make_unique<f3rt::WavWriter>(wav_path,audio_rate);
    std::array<int16_t,8192> samples{};
    uint64_t audio_frames=0,nonzero_samples=0;
    int audio_peak=0;
    bool quit=false;
    std::unique_ptr<f3rt::netplay::Session> session;
    std::optional<f3rt::netplay::Identity> identity;
    auto machine_identity=[&]() -> const f3rt::netplay::Identity & {
        if(!identity)identity=f3rt::netplay::machine_identity(m);
        return *identity;
    };
    f3rt::FrontendUiState ui_state;
#ifdef F3RT_GPU
    ui_state.gpu_available=true;
    if(sdl.gpu)ui_state.active_postprocess=postprocess;
#endif
    ui_state.message=settings_error;
    uint64_t executed_frames=0;
    bool screenshot_pending=false,surface_saved=false,surface_valid=false;
    int exit_code=0;
    const bool finite_netplay=netplay && frames;
    auto next_net_step=std::chrono::steady_clock::now();
    auto next_status=next_net_step;
    auto next_frame=std::chrono::steady_clock::now();
    constexpr int max_audio_queue_ms=50,max_catchup_ms=50;
    uint64_t audio_queue_drops=0,clock_resyncs=0,audio_queue_sum=0,audio_queue_samples=0,audio_queue_max=0;
    const auto start=std::chrono::steady_clock::now();
#ifdef F3RT_GPU
    int observed_pixel_width=pixel_width,observed_pixel_height=pixel_height;
    bool scale_pending=false;
    auto scale_pending_since=start,scale_last_resize=start;
    uint64_t scale_changes=0;
#endif
    auto connect=[&](f3rt::netplay::TransportOptions options) {
        if(session)throw std::runtime_error("Disconnect the current session first");
        session=std::make_unique<f3rt::netplay::Session>(m,options,machine_identity());
        if(finite_netplay)session->set_frame_limit(uint32_t(frames));
        if(sdl.ui)sdl.ui->set_open(false);
        if(sdl.input)sdl.input->release();
        ui_state.message.clear();ui_state.desync=false;
        next_net_step=std::chrono::steady_clock::now();
        next_status=next_net_step;
    };
    if(netplay)connect(net_options);
    while(!quit && (netplay || !frames || executed_frames<frames)) {
        profile.tick();
        bool refresh=false;
        if(!headless) {
            SDL_Event event;
            while(SDL_PollEvent(&event)) {
                if(event.type==SDL_EVENT_QUIT)quit=true;
                const bool was_open=sdl.ui->open();
                const bool consumed=sdl.ui->process_event(event);
                sdl.input->process_event(event,consumed || sdl.ui->open());
                refresh|=was_open!=sdl.ui->open();
                if(event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                    if(!consumed && event.key.scancode==SDL_SCANCODE_ESCAPE)quit=true;
                    const bool fullscreen_key=event.key.scancode==SDL_SCANCODE_F11 ||
                        (event.key.scancode==SDL_SCANCODE_RETURN && (event.key.mod&SDL_KMOD_ALT));
                    if(fullscreen_key) {
                        check(SDL_SetWindowFullscreen(sdl.window,!(SDL_GetWindowFlags(sdl.window)&SDL_WINDOW_FULLSCREEN)));
                        sdl.input->release();refresh=true;
                    }
                }
                if(event.type==SDL_EVENT_WINDOW_EXPOSED || event.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    refresh=true;
            }
            ui_state.connected=bool(session);
            ui_state.transferring=session && (session->phase()==f3rt::netplay::Session::Phase::Sending ||
                session->phase()==f3rt::netplay::Session::Phase::Accepting ||
                session->phase()==f3rt::netplay::Session::Phase::WaitingSnapshot);
            if(session) {
                ui_state.rtt_ms=session->rtt_ms();ui_state.frame_advantage=session->frame_advantage();
                ui_state.rollback_depth=session->rollback()?session->rollback()->last_rollback_depth():0;
                ui_state.transfer_progress=float(session->transfer_progress());
            } else if(sdl.ui->open())ui_state.status="Solo paused at frame "+std::to_string(m.frame);
            sdl.ui->draw(ui_state);
            for(const auto action:sdl.ui->take_actions())try {
                using Action=f3rt::UiActionKind;
                switch(action.kind) {
                case Action::Host: case Action::Join: {
                    f3rt::netplay::TransportOptions options;
                    options.server=settings.server;options.room=settings.room;
                    options.player=settings.requested_slot;options.delay=settings.delay;
                    options.host=action.kind==Action::Host;
                    connect(options);break;
                }
                case Action::Disconnect:
                    if(session)session->disconnect();
                    sdl.input->release();break;
                case Action::Screenshot:screenshot_pending=true;break;
                case Action::SaveState: case Action::LoadState: {
                    if(session || !translated || allow_fallback || m.sound_trace)
                        throw std::runtime_error("Slots require offline strict-native execution without sound tracing");
                    if(config_path.empty())throw std::runtime_error("No user config path; supply --config FILE");
                    const auto path=std::filesystem::absolute(config_path).parent_path()/"states"/
                        (set+"-"+std::to_string(action.slot)+".f3state");
                    const bool save=action.kind==Action::SaveState;
                    f3rt::frontend_state_slot(m,machine_identity(),path,
                        snapshot_video_options.scale,snapshot_video_options.border,save);
                    if(!save) {
                        sdl.input->release();refresh=true;
                        if(sdl.audio)check(SDL_ClearAudioStream(sdl.audio));
                        next_frame=std::chrono::steady_clock::now();
                    }
                    ui_state.message=std::string(save?"Saved ":"Loaded ")+path.string();break;
                }
                case Action::ApplySettings:
                    sdl.input->configure(settings);
                    if(sdl.audio)check(SDL_SetAudioStreamGain(sdl.audio,settings.volume));
                    video_filter=settings.filter;
#ifdef F3RT_GPU
                    if(sdl.gpu) {
                        auto requested=video_options;
                        parse_scale(settings.video_scale,video_scale_mode,requested);
                        automatic_scale=video_scale_mode!=f3rt::VideoScaleMode::Fixed;
                        if(automatic_scale) {
                            check(SDL_GetWindowSizeInPixels(sdl.window,&pixel_width,&pixel_height));
                            requested.scale=f3rt::video_scale_for_window(video_scale_mode,
                                unsigned(std::max(pixel_width,0)),unsigned(std::max(pixel_height,0)),video_options.border);
                        }
                        if(requested.scale!=video_options.scale) {
                            m.game_video->set_gpu_scale(requested.scale);sdl.gpu->set_scale(requested.scale);
                            video_options.scale=requested.scale;++scale_changes;
                        }
                        sdl.gpu->set_scale_mode(video_scale_mode);
                        sdl.gpu->set_linear(video_filter=="linear");
                    } else
#endif
                    check(SDL_SetTextureScaleMode(sdl.texture,video_filter=="linear"?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST));
                    refresh=true;break;
                case Action::ApplyPostprocess:
#ifdef F3RT_GPU
                    if(sdl.gpu) {
                        const auto preset=settings.postprocess=="off"?f3rt::Postprocess::Off:
                            settings.postprocess=="crt"?f3rt::Postprocess::Crt:f3rt::Postprocess::User;
                        sdl.gpu->set_postprocess(preset,settings.user_shader);
                        ui_state.active_postprocess=settings.postprocess;
                        ui_state.message="Post-processing applied: "+settings.postprocess;
                        refresh=true;break;
                    }
#endif
                    throw std::runtime_error("Post-processing requires GPU backend");
                case Action::SavePreferences:
                    if(settings.video_mode=="fdp" && (settings.video_scale!="1" || settings.border ||
                       settings.filter!="nearest" || settings.video_backend=="gpu"))
                        throw std::runtime_error("FDP preferences require CPU, scale 1, border 0 and nearest filtering");
                    if(settings.video_backend=="cpu" && (settings.video_scale.starts_with("auto") || settings.interpolation!="off"))
                        throw std::runtime_error("Auto scale and interpolation preferences require GPU");
                    if(!f3rt::save_frontend_settings(config_path.string(),settings,ui_state.message))
                        throw std::runtime_error(ui_state.message);
                    ui_state.message="Preferences saved to "+config_path.string();break;
                }
            } catch(const std::exception &error) { ui_state.message=error.what(); }
        }
        if(quit)break;
        bool advanced=false;
        std::array<f3rt::netplay::InputWord,2> local{};
        if(sdl.input && !sdl.ui->open())local={sdl.input->word(0),sdl.input->word(1)};
        if(session) {
            const auto previous_rollbacks=session->rollback()?session->rollback()->rollback_count():0;
            const bool was_synchronized=session->synchronized();
            session->pump();
            if(!was_synchronized && session->synchronized())
                std::cout<<"netplay_ready player="<<session->slot()+1<<" delay="<<session->delay()
                         <<" origin="<<session->rollback()->origin_frame()<<'\n';
            refresh|=session->rollback() && session->rollback()->rollback_count()!=previous_rollbacks;
            if(session->take_discontinuity()) {
                if(sdl.input)sdl.input->release();
                local={};refresh=true;
                if(sdl.audio && (session->phase()==f3rt::netplay::Session::Phase::Accepting ||
                    (session->rollback() && session->rollback()->frame()==0)))
                    check(SDL_ClearAudioStream(sdl.audio));
            }
            const auto now=std::chrono::steady_clock::now();
            const int lead_limit=throttle?2+int(session->rtt_ms()*f3rt::Machine::pixel_clock/
                (2000.0*f3rt::Machine::frame_pixels)+0.999):16;
            if((!throttle || now>=next_net_step) && session->frame_advantage()<=lead_limit) {
                advanced=session->advance(local);
                if(advanced)next_net_step=std::max(next_net_step,now)+std::chrono::nanoseconds(
                    uint64_t(1e9*f3rt::Machine::frame_pixels/f3rt::Machine::pixel_clock));
            }
            if(now>=next_status) {
                ui_state.status=session->status();
                if(sdl.window)check(SDL_SetWindowTitle(sdl.window,("Land Maker — "+ui_state.status+
                    " ping="+std::to_string(int(session->rtt_ms()))+"ms rollback="+
                    std::to_string(session->rollback()?session->rollback()->last_rollback_depth():0)).c_str()));
                next_status=now+std::chrono::milliseconds(250);
            }
        } else if(!sdl.ui || !sdl.ui->open()) {
            f3rt::netplay::apply_inputs(m,local);
            if(!m.run_frame(translated))throw std::runtime_error("CPU halted at "+std::to_string(m.cpu.pc));
            advanced=true;
        }
        executed_frames+=advanced;
        if(advanced && !dumpdir.empty() && m.frame>=dump_start && (m.frame-dump_start)%dump_every==0)f3rt::dump_machine(m,dumpdir);
        size_t count;
        while((count=session?session->render_audio(samples.data(),samples.size()/2):
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
        const bool capture_final=!surface_saved && !surface.empty() && frames &&
            (finite_netplay ? session && session->synchronized() &&
                session->result()!=f3rt::netplay::Session::Result::None : executed_frames==frames);
        const bool draw_menu=sdl.ui && sdl.ui->open();
        if(!headless && (advanced || refresh || draw_menu || screenshot_pending || capture_final)) {
            std::filesystem::path screenshot;
            if(screenshot_pending) {
                const auto stamp=std::chrono::system_clock::now().time_since_epoch().count();
                screenshot=std::filesystem::absolute(config_path).parent_path()/"screenshots"/
                    (set+"-"+std::to_string(m.frame)+"-"+std::to_string(stamp)+".png");
                std::filesystem::create_directories(screenshot.parent_path());
            }
#ifdef F3RT_GPU
            if(sdl.gpu) {
                if(automatic_scale) {
                    // Poll physical pixels, not logical resize events: display-density/fullscreen changes count too.
                    // Debounce geometry until 100ms quiet, but never defer a live drag more than 250ms.
                    check(SDL_GetWindowSizeInPixels(sdl.window,&pixel_width,&pixel_height));
                    const auto now=std::chrono::steady_clock::now();
                    if(pixel_width!=observed_pixel_width || pixel_height!=observed_pixel_height) {
                        observed_pixel_width=pixel_width;observed_pixel_height=pixel_height;
                        if(!scale_pending)scale_pending_since=now;
                        scale_pending=true;scale_last_resize=now;
                    }
                    if(scale_pending && (now-scale_last_resize>=std::chrono::milliseconds(100) ||
                                         now-scale_pending_since>=std::chrono::milliseconds(250))) {
                        scale_pending=false;
                        const unsigned target=f3rt::video_scale_for_window(video_scale_mode,
                            unsigned(std::max(pixel_width,0)),unsigned(std::max(pixel_height,0)),video_options.border);
                        const auto change_start=std::chrono::steady_clock::now();
                        const unsigned previous=video_options.scale;
                        if(target!=previous) {
                            // Native audio is already enqueued above; neither host API waits for GPU idle.
                            m.game_video->set_gpu_scale(target);
                            sdl.gpu->set_scale(target);
                            video_options.scale=target;++scale_changes;
                        }
                        const double change_ms=std::chrono::duration<double,std::milli>(
                            std::chrono::steady_clock::now()-change_start).count();
                        std::cout<<"video_scale pixels="<<pixel_width<<'x'<<pixel_height
                                 <<" previous="<<previous<<" scale="<<target<<" changes="<<scale_changes
                                 <<" change_ms="<<change_ms<<" debounce_ms="
                                 <<std::chrono::duration<double,std::milli>(now-scale_pending_since).count()<<'\n';
                    }
                }
                sdl.gpu->set_overlay(draw_menu ? +[](void *user,SDL_GPUCommandBuffer *command,
                    SDL_GPUTexture *texture,Uint32 width,Uint32 height) {
                        static_cast<f3rt::FrontendUi *>(user)->render_gpu(command,texture,width,height);
                    } : nullptr,sdl.ui.get());
                sdl.gpu->draw(m.game_video->gpu_scene());
                if(capture_final)sdl.gpu->save_surface(surface.string().c_str());
                if(screenshot_pending)sdl.gpu->save_surface(screenshot.string().c_str());
            } else
#endif
            {
                const auto pixels=m.game_video?m.game_video->presentation():std::span<const uint32_t>(m.pixels);
                if(advanced || refresh || !surface_valid)
                    check(SDL_UpdateTexture(sdl.texture,nullptr,pixels.data(),int(video_options.width()*4)));
                check(SDL_RenderClear(sdl.renderer));check(SDL_RenderTexture(sdl.renderer,sdl.texture,nullptr,nullptr));
                if(capture_final || screenshot_pending) {
                    SDL_Surface *shot=SDL_RenderReadPixels(sdl.renderer,nullptr);
                    check(shot!=nullptr);
                    const bool saved=(!capture_final || SDL_SaveBMP(shot,surface.string().c_str())) &&
                        (!screenshot_pending || SDL_SavePNG(shot,screenshot.string().c_str()));
                    SDL_DestroySurface(shot);check(saved);
                }
                sdl.ui->render_cpu();
                check(SDL_RenderPresent(sdl.renderer));
            }
            surface_valid=true;
            if(capture_final)surface_saved=true;
            if(screenshot_pending) { ui_state.message="Screenshot saved to "+screenshot.string();screenshot_pending=false; }
            if(throttle && !session && advanced) {
                // Pace against a rolling deadline. After a stall longer than one catch-up window, resync to now instead of
                // running flat out to make up lost time (that burst is what delayed audio by the length of the stall).
                const auto frame_time=std::chrono::nanoseconds(uint64_t(1e9*f3rt::Machine::frame_pixels/f3rt::Machine::pixel_clock));
                next_frame+=frame_time;
                const auto now=std::chrono::steady_clock::now();
                if(now-next_frame>std::chrono::milliseconds(max_catchup_ms)) { next_frame=now;++clock_resyncs; }
                std::this_thread::sleep_until(next_frame);
            }
        }
        if(session && !session->connected()) {
            ui_state.message=session->status();
            ui_state.desync=ui_state.message.find("DESYNC")!=std::string::npos;
            if(const auto *rollback=session->rollback())
                std::cout<<"netplay_confirmed="<<rollback->confirmed_frame()<<" origin="<<rollback->origin_frame()
                         <<" state_crc="<<m.sync_state_crc()<<" rollbacks="<<rollback->rollback_count()
                         <<" max_rollback_depth="<<rollback->maximum_rollback_depth()<<'\n';
            std::cout<<"netplay_local reason="<<ui_state.message<<'\n';
            if(session->result()==f3rt::netplay::Session::Result::Error) {
                std::cerr<<"netplay: "<<ui_state.message<<'\n';
                if(sdl.ui)sdl.ui->set_open(true);
                if(finite_netplay)exit_code=1;
            }
            session.reset();
            ui_state.rtt_ms=0;ui_state.frame_advantage=0;ui_state.rollback_depth=0;
            ui_state.transfer_progress=0;
            if(sdl.input)sdl.input->release();
            next_frame=std::chrono::steady_clock::now();
            if(sdl.window)check(SDL_SetWindowTitle(sdl.window,("f3rt — "+set).c_str()));
            if(finite_netplay)quit=true;
        }
        if(!advanced) {
            if(!session)next_frame=std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(session?1:8));
        }
    }
    if(session && session->connected())session->disconnect("Local frontend closed");
    if(!eeprom.empty())m.save_eeprom(eeprom);
    if(!fallback_report.empty()) {
        std::ofstream report(fallback_report);
        report<<"pc\tcount\n";
        for(size_t i=0;i<m.fallback_hits.size();++i)if(m.fallback_hits[i])report<<"0x"<<std::hex<<i*2<<std::dec<<'\t'<<m.fallback_hits[i]<<'\n';
        if(!report)throw std::runtime_error("Fallback report write failed");
    }
    if(m.game_video)m.game_video->report(std::cout);
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
    return exit_code;
} catch(const std::exception &e) { std::cerr<<"f3rt: "<<e.what()<<'\n';return 1; }
