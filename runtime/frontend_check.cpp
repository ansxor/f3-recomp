#include "frontend_settings.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
void key(f3rt::InputMapper &input,SDL_Scancode scan,bool down,bool suppressed=false){
    SDL_Event event{};
    event.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;
    event.key.scancode=scan;
    input.process_event(event,suppressed);
}
bool neutral(const f3rt::InputMapper &input){
    for(unsigned p=0;p<f3rt::local_player_count;++p)if(input.word(p))return false;
    return true;
}
struct ConfigFile {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("f3rt-frontend-check-"+std::to_string(SDL_GetTicksNS())+".cfg");
    ~ConfigFile(){std::error_code error;std::filesystem::remove(path,error);}
    void write(const std::string &text)const{
        std::ofstream out(path,std::ios::trunc);out<<text;out.close();
        require(bool(out),"Write temporary settings fixture");
    }
};
void defaults_and_release(){
    f3rt::FrontendSettings settings;
    f3rt::InputMapper input(settings);
    const std::array<SDL_Scancode,f3rt::local_control_count> p1={
        SDL_SCANCODE_UP,SDL_SCANCODE_DOWN,SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_Z,SDL_SCANCODE_X,SDL_SCANCODE_C,SDL_SCANCODE_1,SDL_SCANCODE_5,
        SDL_SCANCODE_F3,SDL_SCANCODE_F2,SDL_SCANCODE_A,SDL_SCANCODE_S,SDL_SCANCODE_D};
    for(unsigned c=0;c<p1.size();++c){
        key(input,p1[c],true);
        require(input.word(0)==(1u<<c),"P1 control indices and extra-button defaults");
        for(unsigned p=1;p<f3rt::local_player_count;++p)
            require(input.word(p)==0,"Other local profiles default to independent controls");
        key(input,p1[c],false);
    }
    const std::array<SDL_Scancode,f3rt::local_player_count> starts={SDL_SCANCODE_1,SDL_SCANCODE_2,SDL_SCANCODE_3,SDL_SCANCODE_4};
    const std::array<SDL_Scancode,f3rt::local_player_count> coins={SDL_SCANCODE_5,SDL_SCANCODE_6,SDL_SCANCODE_7,SDL_SCANCODE_8};
    for(unsigned p=0;p<f3rt::local_player_count;++p){
        require(settings.profiles[p].device_slot==int(p),"Default gamepad ordinals follow all local players");
        key(input,starts[p],true);key(input,coins[p],true);
        for(unsigned other=0;other<f3rt::local_player_count;++other)
            require(input.word(other)==(other==p?0x180:0),"Each local start/coin pair targets only its player");
        key(input,starts[p],false);key(input,coins[p],false);
    }
    key(input,SDL_SCANCODE_Z,true);
    input.release();
    key(input,SDL_SCANCODE_Z,true);
    require(neutral(input),"Held input must remain suppressed after menu closure");
    key(input,SDL_SCANCODE_Z,false);key(input,SDL_SCANCODE_Z,true);
    require(input.word(0)==0x10,"Physical release rearms gameplay input");
    key(input,SDL_SCANCODE_Z,false);
    for(unsigned c=11;c<p1.size();++c){
        key(input,p1[c],true,true);key(input,p1[c],true);
        require(neutral(input),"Suppressed extra buttons stay blocked until physical release");
        key(input,p1[c],false);key(input,p1[c],true);
        require(input.word(0)==(1u<<c),"Physical release rearms each extra button");
        input.release();key(input,p1[c],true);
        require(neutral(input),"Menu closure blocks each held extra button");
        key(input,p1[c],false);
    }
    key(input,SDL_SCANCODE_A,true);key(input,SDL_SCANCODE_S,true);key(input,SDL_SCANCODE_D,true);
    require(input.word(0)==0x3800,"All three extra buttons may be held together");
    SDL_Event focus{};focus.type=SDL_EVENT_WINDOW_FOCUS_LOST;
    input.process_event(focus,false);
    require(neutral(input),"Focus loss releases every extra button");
    for(unsigned c=11;c<p1.size();++c)key(input,p1[c],false);
    require(input.word(f3rt::local_player_count)==0,"Out-of-range player word is neutral");
}
void capture_and_remap(){
    f3rt::FrontendSettings settings;
    f3rt::InputMapper input(settings);
    input.start_capture(1,4,true);
    input.release(); // F1 closure, focus loss, or device removal.
    key(input,SDL_SCANCODE_Z,true);
    require(!input.capturing() && input.word(0)==0x10,"Closing gamepad capture must not disable keyboard gameplay");
    unsigned profile=0,control=0;
    f3rt::InputBinding binding;
    require(!input.take_binding(profile,control,binding),"Cancelled capture must not swallow a later gameplay key");
    key(input,SDL_SCANCODE_Z,false);
    input.start_capture(1,4,false);key(input,SDL_SCANCODE_V,true,true);
    require(input.take_binding(profile,control,binding) && profile==1 && control==4,"P2 binding capture identifies its own control");
    settings.profiles[profile].controls[control]=binding;input.configure(settings);
    require(neutral(input),"Capture key must not leak into any local player");
    key(input,SDL_SCANCODE_V,false);key(input,SDL_SCANCODE_V,true);
    require(input.word(1)==0x10 && input.word(0)==0,"P2 remap must remain independent of P1");
    key(input,SDL_SCANCODE_V,false);
    const std::array<SDL_Scancode,2> extra_keys={SDL_SCANCODE_G,SDL_SCANCODE_H};
    for(unsigned p=2;p<f3rt::local_player_count;++p){
        const unsigned c=p==2?11:13;
        input.start_capture(p,c,true);input.release();
        key(input,SDL_SCANCODE_D,true);
        require(!input.capturing() && input.word(0)==0x2000,"Cancelling P3/P4 extra-button pad capture restores gameplay");
        require(!input.take_binding(profile,control,binding),"Cancelled extra-button capture yields no binding");
        key(input,SDL_SCANCODE_D,false);
        input.start_capture(p,c,false);key(input,extra_keys[p-2],true,true);
        require(input.take_binding(profile,control,binding) && profile==p && control==c,"P3/P4 capture preserves player and appended control indices");
        settings.profiles[profile].controls[control]=binding;input.configure(settings);
        require(neutral(input),"Captured extra-button key remains blocked for all profiles");
        key(input,extra_keys[p-2],false);key(input,extra_keys[p-2],true);
        for(unsigned other=0;other<f3rt::local_player_count;++other)
            require(input.word(other)==(other==p?1u<<c:0),"P3/P4 remaps are independent of every other player");
        key(input,extra_keys[p-2],false);
    }
    input.start_capture(3,12,false);key(input,SDL_SCANCODE_ESCAPE,true,true);
    require(!input.capturing() && !input.take_binding(profile,control,binding) && neutral(input),"Escape cancels an appended-control capture without leaking gameplay input");
    key(input,SDL_SCANCODE_ESCAPE,false);
    input.start_capture(f3rt::local_player_count,13,false);
    input.start_capture(3,f3rt::local_control_count,false);
    require(!input.capturing(),"Capture rejects players or controls outside the shared local counts");
    key(input,SDL_SCANCODE_G,true);key(input,SDL_SCANCODE_H,true);
    SDL_Event focus{};focus.type=SDL_EVENT_WINDOW_FOCUS_LOST;input.process_event(focus,false);
    require(neutral(input),"Focus loss releases independent P3/P4 remapped extra buttons");
}
void legacy_settings(){
    ConfigFile file;
    const std::array<SDL_Scancode,11> old_keys={SDL_SCANCODE_I,SDL_SCANCODE_K,SDL_SCANCODE_J,SDL_SCANCODE_L,
        SDL_SCANCODE_Q,SDL_SCANCODE_W,SDL_SCANCODE_E,SDL_SCANCODE_R,SDL_SCANCODE_T,SDL_SCANCODE_Y,SDL_SCANCODE_U};
    std::ostringstream old;
    old<<"device0=2\ndevice1=3\n";
    for(unsigned c=0;c<old_keys.size();++c)old<<"binding0."<<c<<'='<<int(old_keys[c])<<" -1 -1 1\n";
    old<<"binding1.7="<<int(SDL_SCANCODE_N)<<" -1 -1 1\nbinding1.8="<<int(SDL_SCANCODE_M)<<" -1 -1 1\n";
    file.write(old.str());
    f3rt::FrontendSettings settings;std::string error;
    require(f3rt::load_frontend_settings(file.path.string(),settings,error),"Load an old two-player, eleven-control settings file");
    f3rt::InputMapper input(settings);
    for(unsigned c=0;c<old_keys.size();++c){
        key(input,old_keys[c],true);
        require(input.word(0)==(1u<<c),"Old persisted control indices retain their original gameplay meaning");
        key(input,old_keys[c],false);
    }
    key(input,SDL_SCANCODE_N,true);key(input,SDL_SCANCODE_M,true);
    require(input.word(1)==0x180,"Old P2 start/coin bindings retain indices seven and eight");
    key(input,SDL_SCANCODE_N,false);key(input,SDL_SCANCODE_M,false);
    key(input,SDL_SCANCODE_A,true);key(input,SDL_SCANCODE_S,true);key(input,SDL_SCANCODE_D,true);
    require(input.word(0)==0x3800,"Missing appended settings leave extra-button defaults intact");
    key(input,SDL_SCANCODE_A,false);key(input,SDL_SCANCODE_S,false);key(input,SDL_SCANCODE_D,false);
    key(input,SDL_SCANCODE_3,true);key(input,SDL_SCANCODE_7,true);
    key(input,SDL_SCANCODE_4,true);key(input,SDL_SCANCODE_8,true);
    require(input.word(2)==0x180 && input.word(3)==0x180 && input.word(0)==0 && input.word(1)==0,
        "Old settings preserve default additional-player start/coin inputs");
}
void settings_roundtrip_and_validation(){
    ConfigFile file;
    f3rt::FrontendSettings settings;
    settings.profiles[2].device_slot=5;settings.profiles[3].device_slot=7;
    settings.profiles[2].controls[0].key=SDL_SCANCODE_H;
    settings.profiles[2].controls[11]={SDL_SCANCODE_V,SDL_GAMEPAD_BUTTON_NORTH,-1,1};
    settings.profiles[2].controls[12].key=SDL_SCANCODE_F;
    settings.profiles[2].controls[13].key=SDL_SCANCODE_G;
    settings.profiles[3].controls[1].key=SDL_SCANCODE_J;
    settings.profiles[3].controls[11].key=SDL_SCANCODE_B;
    settings.profiles[3].controls[12]={SDL_SCANCODE_N,-1,SDL_GAMEPAD_AXIS_RIGHTX,-1};
    settings.profiles[3].controls[13].key=SDL_SCANCODE_M;
    std::string error;
    require(f3rt::save_frontend_settings(file.path.string(),settings,error),"Save four profiles with appended button bindings");
    f3rt::FrontendSettings loaded;
    require(f3rt::load_frontend_settings(file.path.string(),loaded,error),"Reload four-player settings");
    for(unsigned p=0;p<f3rt::local_player_count;++p){
        require(loaded.profiles[p].device_slot==settings.profiles[p].device_slot,"Roundtrip preserves each player's gamepad ordinal");
        for(unsigned c=0;c<f3rt::local_control_count;++c){
            const auto &actual=loaded.profiles[p].controls[c],&expected=settings.profiles[p].controls[c];
            require(actual.key==expected.key && actual.button==expected.button && actual.axis==expected.axis && actual.direction==expected.direction,
                "Roundtrip preserves keyboard, button and signed-axis bindings for every local control");
        }
    }
    f3rt::InputMapper input(loaded);
    for(unsigned p=2;p<f3rt::local_player_count;++p)for(unsigned c=0;c<f3rt::local_control_count;++c){
        const auto scan=loaded.profiles[p].controls[c].key;if(scan==SDL_SCANCODE_UNKNOWN)continue;
        key(input,scan,true);
        for(unsigned other=0;other<f3rt::local_player_count;++other)
            require(input.word(other)==(other==p?1u<<c:0),"Reloaded P3/P4 remaps produce only the intended player's input bits");
        key(input,scan,false);
    }
    const std::array<std::string,8> invalid={
        "device"+std::to_string(f3rt::local_player_count)+"=0\n","device3=16\n",
        "binding"+std::to_string(f3rt::local_player_count)+".0=0 -1 -1 1\n",
        "binding3."+std::to_string(f3rt::local_control_count)+"=0 -1 -1 1\n",
        "binding3.13="+std::to_string(SDL_SCANCODE_F1)+" -1 -1 1\n",
        "binding3.13=0 "+std::to_string(SDL_GAMEPAD_BUTTON_COUNT)+" -1 1\n",
        "binding3.13=0 -1 "+std::to_string(SDL_GAMEPAD_AXIS_COUNT)+" 1\n",
        "binding3.13=0 -1 -1 0\n"};
    for(const auto &text:invalid){
        file.write(text);
        require(!f3rt::load_frontend_settings(file.path.string(),loaded,error),"Reject invalid device or extra-player binding without applying it");
        input.configure(loaded);key(input,SDL_SCANCODE_M,true);
        require(input.word(3)==0x2000 && input.word(0)==0 && input.word(1)==0 && input.word(2)==0,
            "Rejected settings preserve the previous playable P4 mapping");
        key(input,SDL_SCANCODE_M,false);
    }
    auto invalid_settings=loaded;
    invalid_settings.profiles[3].controls[13].key=SDL_SCANCODE_F12;
    require(!f3rt::save_frontend_settings(file.path.string(),invalid_settings,error),"Save validates additional-player appended bindings");
}
}
int main() try {
    require(SDL_Init(SDL_INIT_EVENTS),SDL_GetError());
    defaults_and_release();capture_and_remap();legacy_settings();settings_roundtrip_and_validation();
    SDL_Quit();
    std::cout<<"PASS four-player defaults/remapping, extra-button capture/release, legacy indices and settings roundtrip/validation\n";
    return 0;
} catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';SDL_Quit();return 1;}
