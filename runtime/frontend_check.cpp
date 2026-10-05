#include "frontend_settings.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
void key(f3rt::InputMapper &input,SDL_Scancode scan,bool down,bool suppressed=false){
    SDL_Event event{};
    event.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;
    event.key.scancode=scan;
    input.process_event(event,suppressed);
}
}
int main() try {
    require(SDL_Init(SDL_INIT_EVENTS),SDL_GetError());
    {
        f3rt::FrontendSettings settings;
        f3rt::InputMapper input(settings);
        key(input,SDL_SCANCODE_Z,true);
        require(input.word(0)==0x10,"P1 action input");
        input.release();
        key(input,SDL_SCANCODE_Z,true);
        require(input.word(0)==0,"Held input must remain suppressed after menu closure");
        key(input,SDL_SCANCODE_Z,false);
        key(input,SDL_SCANCODE_Z,true);
        require(input.word(0)==0x10,"Physical release rearms gameplay input");
        input.start_capture(1,4,true);
        input.release(); // F1 closure, focus loss, or device removal.
        key(input,SDL_SCANCODE_Z,false);
        key(input,SDL_SCANCODE_Z,true);
        require(!input.capturing() && input.word(0)==0x10,"Closing gamepad capture must not disable keyboard gameplay");
        unsigned profile=0,control=0;
        f3rt::InputBinding binding;
        require(!input.take_binding(profile,control,binding),"Cancelled capture must not swallow a later gameplay key");
        input.start_capture(1,4,false);
        key(input,SDL_SCANCODE_V,true,true);
        require(input.take_binding(profile,control,binding) && profile==1 && control==4,"P2 binding capture identifies its own control");
        settings.profiles[profile].controls[control]=binding;
        input.configure(settings);
        require(input.word(0)==0 && input.word(1)==0,"Capture key must not leak into either player");
        key(input,SDL_SCANCODE_V,false);
        key(input,SDL_SCANCODE_V,true);
        require(input.word(1)==0x10 && input.word(0)==0,"P2 remap must remain independent of P1");
        SDL_Event focus{};focus.type=SDL_EVENT_WINDOW_FOCUS_LOST;
        input.process_event(focus,false);
        require(input.word(1)==0,"Focus loss releases held gameplay input");
    }
    SDL_Quit();
    std::cout<<"PASS capture cancellation, held-input release and independent P1/P2 remapping\n";
    return 0;
} catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';SDL_Quit();return 1;}
