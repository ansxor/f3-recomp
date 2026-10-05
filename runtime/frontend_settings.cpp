#include "frontend_settings.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace f3rt {
bool audio_backend_available(AudioBackend backend) {
    if (backend == AudioBackend::Oracle) return true;
#ifdef F3RT_SOUND_GENERATED
    if (backend == AudioBackend::Native) return true;
#endif
    return false; // HLE registration belongs to the audio backend, not a fake UI implementation.
}
const char *audio_backend_name(AudioBackend backend) {
    switch (backend) {
    case AudioBackend::Oracle: return "oracle";
    case AudioBackend::Native: return "native";
    case AudioBackend::Hle: return "hle";
    }
    return "unknown";
}
FrontendSettings::FrontendSettings() {
    const SDL_Scancode p1[] = {SDL_SCANCODE_UP,SDL_SCANCODE_DOWN,SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT,SDL_SCANCODE_Z,SDL_SCANCODE_X,SDL_SCANCODE_C,SDL_SCANCODE_1,SDL_SCANCODE_5,SDL_SCANCODE_F3,SDL_SCANCODE_F2};
    for(unsigned i=0;i<control_count;++i)profiles[0].controls[i].key=p1[i];
    profiles[1].device_slot=1;
    profiles[1].controls[7].key=SDL_SCANCODE_2;
    profiles[1].controls[8].key=SDL_SCANCODE_6;
}
std::string default_config_path() {
    char *p=SDL_GetPrefPath("f3-recomp","f3rt");
    if(!p)return {};
    std::string result=std::string(p)+"settings.cfg";SDL_free(p);return result;
}
namespace {
bool reserved(SDL_Scancode k) { return k==SDL_SCANCODE_F1 || k==SDL_SCANCODE_F11 || k==SDL_SCANCODE_F12; }
bool choice(const std::string &s,std::initializer_list<const char*> values) { for(auto v:values)if(s==v)return true;return false; }
bool valid(const FrontendSettings &s) {
    if(!audio_backend_available(s.audio_backend))return false;
    if(!choice(s.video_mode,{"game","fdp","compare"}) || !choice(s.video_backend,{"cpu","gpu"}) || !choice(s.video_scale,{"1","2","3","4","auto","auto-integer"}) || s.border>160 || !choice(s.filter,{"nearest","linear"}) || !choice(s.interpolation,{"off","linear","fit"}) || !choice(s.interpolation_fields,{"none","geometry","palette","geometry,palette"}) || !std::isfinite(s.volume) || s.volume<0 || s.volume>1 || s.requested_slot>2 || s.delay>8 || s.server.size()>255 || s.room.size()>64 || s.server.find_first_of("\r\n")!=std::string::npos || s.room.find_first_of("\r\n")!=std::string::npos)return false;
    if(!choice(s.postprocess,{"off","crt","user"}) || s.user_shader.size()>1000 || s.user_shader.find_first_of("\r\n")!=std::string::npos)return false;
    for(const auto &p:s.profiles) {
        if(p.device_slot<0 || p.device_slot>15)return false;
        for(const auto &b:p.controls)if(b.key<0 || b.key>=SDL_SCANCODE_COUNT || reserved(b.key) || b.button < -1 || b.button>=SDL_GAMEPAD_BUTTON_COUNT || b.axis < -1 || b.axis>=SDL_GAMEPAD_AXIS_COUNT || (b.direction!=1 && b.direction!=-1))return false;
    }
    return true;
}
bool integer(const std::string &s,int &v) { auto r=std::from_chars(s.data(),s.data()+s.size(),v);return r.ec==std::errc{} && r.ptr==s.data()+s.size(); }
}
bool load_frontend_settings(const std::string &path,FrontendSettings &settings,std::string &error) {
    error.clear();std::error_code ec;
    if(!std::filesystem::exists(path,ec)) { if(ec)error=ec.message();return !ec; }
    if(std::filesystem::file_size(path,ec)>32768 || ec) { error="Config exceeds 32 KiB or is unreadable";return false; }
    std::ifstream file(path);if(!file){error="Cannot read config";return false;}
    auto s=settings;std::string line;unsigned count=0;
    while(std::getline(file,line)) {
        if(++count>512 || line.size()>1024){error="Config limits exceeded";return false;}
        if(line.empty() || line[0]=='#')continue;
        auto split=line.find('=');if(split==std::string::npos){error="Invalid config line";return false;}
        const auto k=line.substr(0,split),v=line.substr(split+1);int n=0;
        if(k=="video")s.video_mode=v;else if(k=="backend")s.video_backend=v;else if(k=="scale")s.video_scale=v;
        else if(k=="filter")s.filter=v;else if(k=="interpolation")s.interpolation=v;else if(k=="fields")s.interpolation_fields=v;
        else if(k=="postprocess")s.postprocess=v;else if(k=="user_shader")s.user_shader=v;
        else if(k=="server")s.server=v;else if(k=="room")s.room=v;
        else if(k=="audio") {if(v=="native")s.audio_backend=AudioBackend::Native;else if(v=="oracle")s.audio_backend=AudioBackend::Oracle;else if(v=="hle")s.audio_backend=AudioBackend::Hle;else {error="Unknown audio backend";return false;}}
        else if(k=="volume") { std::istringstream in(v);in>>s.volume;if(!in || in.peek()!=EOF){error="Invalid volume";return false;} }
        else if(k=="border" || k=="slot" || k=="delay") {if(!integer(v,n) || n<0){error="Invalid numeric setting";return false;}if(k=="border")s.border=n;else if(k=="slot")s.requested_slot=n;else s.delay=n;}
        else if(k.size()==7 && k.substr(0,6)=="device" && (k[6]=='0'||k[6]=='1')) {if(!integer(v,n)){error="Invalid device slot";return false;}s.profiles[k[6]-'0'].device_slot=n;}
        else if(k.starts_with("binding")) {
            unsigned p=0,c=0;char dot=0;std::istringstream name(k.substr(7));name>>p>>dot>>c;
            InputBinding b;int key;std::istringstream in(v);in>>key>>b.button>>b.axis>>b.direction;
            if(!name || dot!='.' || name.peek()!=EOF || p>1 || c>=control_count || !in || in.peek()!=EOF){error="Invalid binding";return false;}
            b.key=SDL_Scancode(key);s.profiles[p].controls[c]=b;
        } else {error="Unknown config setting: "+k;return false;}
    }
    if(file.bad() || !valid(s)){error="Invalid config values";return false;}
    settings=std::move(s);return true;
}
bool save_frontend_settings(const std::string &path,const FrontendSettings &s,std::string &error) {
    error.clear();if(path.empty() || !valid(s)){error="Invalid config path or values";return false;}
    std::error_code ec;auto target=std::filesystem::path(path);if(!target.parent_path().empty())std::filesystem::create_directories(target.parent_path(),ec);
    if(ec){error=ec.message();return false;}
    // Same-directory temporary file keeps replacement atomic.
    std::string tmp=path+".tmp."+std::to_string(SDL_GetTicksNS());
    std::ofstream out(tmp,std::ios::binary|std::ios::trunc);if(!out){error="Cannot create config temporary file";return false;}
    out<<"# f3rt settings, Dear ImGui frontend\nvideo="<<s.video_mode<<"\nbackend="<<s.video_backend<<"\nscale="<<s.video_scale<<"\nborder="<<s.border<<"\nfilter="<<s.filter<<"\ninterpolation="<<s.interpolation<<"\nfields="<<s.interpolation_fields<<"\naudio="<<audio_backend_name(s.audio_backend)<<"\nvolume="<<s.volume<<"\nserver="<<s.server<<"\nroom="<<s.room<<"\nslot="<<s.requested_slot<<"\ndelay="<<s.delay<<'\n';
    out<<"postprocess="<<s.postprocess<<"\nuser_shader="<<s.user_shader<<'\n';
    for(unsigned p=0;p<2;++p) {out<<"device"<<p<<'='<<s.profiles[p].device_slot<<'\n';for(unsigned c=0;c<control_count;++c){const auto &b=s.profiles[p].controls[c];out<<"binding"<<p<<'.'<<c<<'='<<int(b.key)<<' '<<b.button<<' '<<b.axis<<' '<<b.direction<<'\n';}}
    out.close();if(!out){error="Cannot write config";std::filesystem::remove(tmp,ec);return false;}
    std::filesystem::rename(tmp,target,ec);if(ec){error=ec.message();std::filesystem::remove(tmp,ec);return false;}return true;
}
InputMapper::InputMapper(const FrontendSettings &s):profiles_(s.profiles) {
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);int count=0;auto *ids=SDL_GetGamepads(&count);for(int i=0;i<count;++i)add_pad(ids[i]);SDL_free(ids);
}
InputMapper::~InputMapper(){for(unsigned i=0;i<pad_count_;++i)SDL_CloseGamepad(pads_[i].handle);SDL_QuitSubSystem(SDL_INIT_GAMEPAD);}
void InputMapper::add_pad(SDL_JoystickID id){for(unsigned i=0;i<pad_count_;++i)if(pads_[i].id==id)return;if(pad_count_==pads_.size())return;auto *pad=SDL_OpenGamepad(id);if(pad)pads_[pad_count_++]={id,pad};}
void InputMapper::configure(const FrontendSettings &s){release();profiles_=s.profiles;}
void InputMapper::release(){capture_=false;for(unsigned i=0;i<keys_.size();++i)blocked_keys_[i]=blocked_keys_[i]||keys_[i];for(unsigned i=0;i<pad_count_;++i){auto &p=pads_[i];for(unsigned j=0;j<p.buttons.size();++j)p.blocked_buttons[j]=p.blocked_buttons[j]||p.buttons[j];for(unsigned j=0;j<p.axes.size();++j)p.blocked_axes[j]=p.blocked_axes[j]||std::abs(int(p.axes[j]))>8000;}}
void InputMapper::start_capture(unsigned p,unsigned c,bool pad){if(p>1 || c>=control_count)return;release();capture_profile_=p;capture_control_=c;capture_pad_=pad;capture_=true;captured_=false;}
bool InputMapper::capturing()const{return capture_;}
bool InputMapper::take_binding(unsigned &p,unsigned &c,InputBinding &b){if(!captured_)return false;p=capture_profile_;c=capture_control_;b=captured_binding_;captured_=false;return true;}
void InputMapper::process_event(const SDL_Event &e,bool suppress){
    auto capture=[&](InputBinding b){captured_binding_=b;capture_=false;captured_=true;release();};
    if(e.type==SDL_EVENT_WINDOW_FOCUS_LOST)release();
    if(e.type==SDL_EVENT_GAMEPAD_ADDED)add_pad(e.gdevice.which);
    if(e.type==SDL_EVENT_GAMEPAD_REMOVED){for(unsigned i=0;i<pad_count_;++i)if(pads_[i].id==e.gdevice.which){release();SDL_CloseGamepad(pads_[i].handle);for(unsigned j=i+1;j<pad_count_;++j)pads_[j-1]=pads_[j];pads_[--pad_count_]={};break;}}
    if(e.type==SDL_EVENT_KEY_DOWN || e.type==SDL_EVENT_KEY_UP){auto k=e.key.scancode;if(k<=0 || k>=SDL_SCANCODE_COUNT)return;bool down=e.type==SDL_EVENT_KEY_DOWN;keys_[k]=down;if(!down)blocked_keys_[k]=false;else if(suppress)blocked_keys_[k]=true;
        if(capture_ && down && k==SDL_SCANCODE_ESCAPE)capture_=false;
        else if(capture_ && !capture_pad_ && down && !e.key.repeat && !reserved(k) &&
                !(k==SDL_SCANCODE_RETURN && (e.key.mod&SDL_KMOD_ALT))) {
            InputBinding b=profiles_[capture_profile_].controls[capture_control_];b.key=k;capture(b);
        }
    }
    if(e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type==SDL_EVENT_GAMEPAD_BUTTON_UP){for(unsigned i=0;i<pad_count_;++i)if(pads_[i].id==e.gbutton.which && e.gbutton.button<SDL_GAMEPAD_BUTTON_COUNT){auto &p=pads_[i];auto b=e.gbutton.button;bool down=e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN;p.buttons[b]=down;if(!down)p.blocked_buttons[b]=false;else if(suppress)p.blocked_buttons[b]=true;if(capture_ && capture_pad_ && down && int(i)==profiles_[capture_profile_].device_slot){auto binding=profiles_[capture_profile_].controls[capture_control_];binding.button=b;binding.axis=-1;capture(binding);}}}
    if(e.type==SDL_EVENT_GAMEPAD_AXIS_MOTION){for(unsigned i=0;i<pad_count_;++i)if(pads_[i].id==e.gaxis.which && e.gaxis.axis<SDL_GAMEPAD_AXIS_COUNT){auto &p=pads_[i];auto a=e.gaxis.axis;p.axes[a]=e.gaxis.value;if(std::abs(int(e.gaxis.value))<8000)p.blocked_axes[a]=false;else if(suppress)p.blocked_axes[a]=true;if(capture_ && capture_pad_ && std::abs(int(e.gaxis.value))>20000 && int(i)==profiles_[capture_profile_].device_slot){auto b=profiles_[capture_profile_].controls[capture_control_];b.button=-1;b.axis=a;b.direction=e.gaxis.value>0?1:-1;capture(b);}}}
}
uint16_t InputMapper::word(unsigned profile)const{
    if(profile>1 || capture_)return 0;uint16_t word=0;const auto &p=profiles_[profile];
    if(keys_[SDL_SCANCODE_RETURN] && (keys_[SDL_SCANCODE_LALT]||keys_[SDL_SCANCODE_RALT]))return 0;
    for(unsigned i=0;i<control_count;++i){const auto &b=p.controls[i];bool held=b.key!=SDL_SCANCODE_UNKNOWN && keys_[b.key] && !blocked_keys_[b.key];if(b.key==SDL_SCANCODE_RETURN && (keys_[SDL_SCANCODE_LALT]||keys_[SDL_SCANCODE_RALT]))held=false;
        if(p.device_slot<int(pad_count_)){const auto &pad=pads_[p.device_slot];if(b.button>=0)held|=pad.buttons[b.button]&&!pad.blocked_buttons[b.button];if(b.axis>=0)held|=!pad.blocked_axes[b.axis] && int(pad.axes[b.axis])*b.direction>16000;}
        if(held)word|=uint16_t(1u<<i);
    }return word;
}
}
