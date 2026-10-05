#include "frontend_ui.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#ifdef F3RT_GPU
#include "imgui_impl_sdlgpu3.h"
#endif
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace f3rt {
namespace {
bool combo(const char *label,std::string &value,std::initializer_list<const char*> choices){bool changed=false;if(ImGui::BeginCombo(label,value.c_str())){for(auto c:choices)if(ImGui::Selectable(c,value==c)){value=c;changed=true;}ImGui::EndCombo();}return changed;}
}
struct FrontendUi::Impl {
    SDL_Window *window;SDL_Renderer *renderer;SDL_GPUDevice *gpu;FrontendSettings &settings;InputMapper &input;
    ImGuiContext *context=nullptr;bool opened=false,draw_ready=false;unsigned save_slot=0;std::vector<UiAction> actions;
    Impl(SDL_Window *w,SDL_Renderer *r,SDL_GPUDevice *g,FrontendSettings &s,InputMapper &i):window(w),renderer(r),gpu(g),settings(s),input(i){}
    void activate(){ImGui::SetCurrentContext(context);}
    void initialize(){
        if(context)return;
        IMGUI_CHECKVERSION();context=ImGui::CreateContext();activate();ImGui::GetIO().IniFilename=nullptr;ImGui::StyleColorsDark();
        bool ok=false;
        // UI uses window coordinates, not the game's SDL logical presentation.
        if(renderer)ok=ImGui_ImplSDL3_InitForOther(window) && ImGui_ImplSDLRenderer3_Init(renderer);
#ifdef F3RT_GPU
        else if(gpu){ImGui_ImplSDLGPU3_InitInfo info;info.Device=gpu;info.ColorTargetFormat=SDL_GetGPUSwapchainTextureFormat(gpu,window);ok=ImGui_ImplSDL3_InitForSDLGPU(window) && ImGui_ImplSDLGPU3_Init(&info);}
#endif
        if(!ok)throw std::runtime_error("Dear ImGui renderer initialization failed");
    }
    ~Impl(){if(!context)return;activate();if(renderer)ImGui_ImplSDLRenderer3_Shutdown();
#ifdef F3RT_GPU
        else if(gpu){SDL_WaitForGPUIdle(gpu);ImGui_ImplSDLGPU3_Shutdown();}
#endif
        ImGui_ImplSDL3_Shutdown();ImGui::DestroyContext(context);
    }
    void action(UiActionKind k,unsigned slot=0){actions.push_back({k,slot});}
};
FrontendUi::FrontendUi(SDL_Window *w,SDL_Renderer *r,SDL_GPUDevice *g,FrontendSettings &s,InputMapper &i):impl_(std::make_unique<Impl>(w,r,g,s,i)){}
FrontendUi::~FrontendUi()=default;
bool FrontendUi::open()const{return impl_->opened;}
void FrontendUi::set_open(bool opened){auto &p=*impl_;if(p.opened==opened)return;if(opened)p.initialize();p.opened=opened;p.draw_ready=false;p.input.release();}
bool FrontendUi::process_event(const SDL_Event &e){
    auto &p=*impl_;if(p.context){p.activate();ImGui_ImplSDL3_ProcessEvent(&e);}
    if(e.type==SDL_EVENT_KEY_DOWN && !e.key.repeat){
        if(e.key.scancode==SDL_SCANCODE_F1){set_open(!p.opened);return true;}
        if(e.key.scancode==SDL_SCANCODE_F12){p.action(UiActionKind::Screenshot);return true;}
        if(e.key.scancode==SDL_SCANCODE_ESCAPE && p.opened){if(!p.input.capturing()){p.opened=false;p.draw_ready=false;p.input.release();}return true;}
    }
    return p.opened && (e.type==SDL_EVENT_KEY_DOWN || e.type==SDL_EVENT_KEY_UP || e.type==SDL_EVENT_TEXT_INPUT || e.type==SDL_EVENT_MOUSE_BUTTON_DOWN || e.type==SDL_EVENT_MOUSE_BUTTON_UP || e.type==SDL_EVENT_MOUSE_MOTION || e.type==SDL_EVENT_MOUSE_WHEEL || e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type==SDL_EVENT_GAMEPAD_BUTTON_UP || e.type==SDL_EVENT_GAMEPAD_AXIS_MOTION);
}
std::vector<UiAction> FrontendUi::take_actions(){std::vector<UiAction> result;result.swap(impl_->actions);return result;}
void FrontendUi::draw(const FrontendUiState &state){
    auto &p=*impl_;p.draw_ready=false;if(!p.opened)return;p.activate();
    if(p.renderer)ImGui_ImplSDLRenderer3_NewFrame();
#ifdef F3RT_GPU
    else if(p.gpu)ImGui_ImplSDLGPU3_NewFrame();
#endif
    ImGui_ImplSDL3_NewFrame();ImGui::NewFrame();
    auto &s=p.settings;bool changed=false;
    unsigned profile,control;InputBinding binding;if(p.input.take_binding(profile,control,binding)){s.profiles[profile].controls[control]=binding;p.input.configure(s);changed=true;}
    ImGui::SetNextWindowSize(ImVec2(650,560),ImGuiCond_FirstUseEver);
    bool visible=p.opened;
    if(ImGui::Begin("F3 runtime - F1 menu",&visible)){
        if(!state.message.empty())ImGui::TextWrapped("%s",state.message.c_str());
        if(ImGui::BeginTabBar("Panels")){
            if(ImGui::BeginTabItem("Netplay")){
                char server[256],room[65];std::snprintf(server,sizeof server,"%s",s.server.c_str());std::snprintf(room,sizeof room,"%s",s.room.c_str());
                ImGui::BeginDisabled(state.connected||state.transferring);
                if(ImGui::InputText("Server host:port",server,sizeof server)){s.server=server;changed=true;}
                if(ImGui::InputText("Room",room,sizeof room)){s.room=room;changed=true;}
                int slot=int(s.requested_slot),delay=int(s.delay);if(ImGui::Combo("Player slot",&slot,"Automatic\0Player 1\0Player 2\0")){s.requested_slot=slot;changed=true;}if(ImGui::SliderInt("Host input delay",&delay,0,8)){s.delay=delay;changed=true;}
                ImGui::TextWrapped("Host/Join marks you ready. Host prepares versus using normal coin/start inputs; the guest adopts that state. Network input uses local P1 controls.");
                ImGui::BeginDisabled(s.server.empty()||s.room.empty());if(ImGui::Button("Host"))p.action(UiActionKind::Host);ImGui::SameLine();if(ImGui::Button("Join"))p.action(UiActionKind::Join);ImGui::EndDisabled();ImGui::EndDisabled();
                ImGui::TextWrapped("Status: %s",state.status.c_str());ImGui::Text("Ping %.1f ms | rollback depth %u | advantage %+d",state.rtt_ms,state.rollback_depth,state.frame_advantage);
                if(state.transferring)ImGui::ProgressBar(std::clamp(state.transfer_progress,0.0f,1.0f));if(state.desync)ImGui::TextColored(ImVec4(1,.3f,.2f,1),"DESYNC: synchronization checksums disagree");
                ImGui::BeginDisabled(!state.connected&&!state.transferring);if(ImGui::Button("Disconnect"))p.action(UiActionKind::Disconnect);ImGui::EndDisabled();ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Video")){
                changed|=combo("Video model (restart)",s.video_mode,{"game","fdp","compare"});
                changed|=combo("Renderer (restart)",s.video_backend,{"cpu","gpu"});if(!state.gpu_available)ImGui::TextWrapped("GPU renderer unavailable in this build; selecting GPU requires a GPU-enabled build on restart.");
                changed|=combo("Scale",s.video_scale,{"1","2","3","4","auto","auto-integer"});int border=int(s.border);if(ImGui::SliderInt("Widescreen border (restart)",&border,0,160)){s.border=border;changed=true;}
                changed|=combo("Filtering",s.filter,{"nearest","linear"});changed|=combo("Interpolation (restart)",s.interpolation,{"off","linear","fit"});changed|=combo("Interpolation fields (restart)",s.interpolation_fields,{"none","geometry","palette","geometry,palette"});
                ImGui::TextWrapped("Scale and filtering apply live on GPU. CPU scale, backend, model, border and interpolation apply on restart. Auto scale/interpolation require GPU; FDP requires native scale/border and nearest filtering. Settings are preferences; CLI flags override them on launch.");ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Shaders")){
                if(!p.gpu)ImGui::TextUnformatted("Requires GPU backend (Video renderer setting; restart).");
                ImGui::BeginDisabled(!p.gpu);
                if(combo("Post-process",s.postprocess,{"off","crt","user"}) && s.postprocess!="user")
                    p.action(UiActionKind::ApplyPostprocess);
                if(s.postprocess=="user"){
                    char path[1001];std::snprintf(path,sizeof path,"%s",s.user_shader.c_str());
                    if(ImGui::InputText("Shader file",path,sizeof path))s.user_shader=path;
                    ImGui::TextWrapped("Metal: .metal, entry f3_postprocess. Vulkan: .spv, entry main. One source texture/sampler and float4 dimensions/scale/time uniform; see README.");
                    ImGui::BeginDisabled(s.user_shader.empty());
                    if(ImGui::Button("Load / reload shader"))p.action(UiActionKind::ApplyPostprocess);
                    ImGui::EndDisabled();
                }
                ImGui::EndDisabled();
                ImGui::Text("Active: %s",state.active_postprocess.c_str());
                ImGui::TextWrapped("Default off. Post-processing affects GPU presentation/screenshots only, never native pixels, simulation, netplay checksums or the menu. A failed load retains the last valid shader.");
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Audio")){
                changed|=ImGui::SliderFloat("Host output volume",&s.volume,0,1,"%.2f");
                if(ImGui::BeginCombo("Sound backend (restart)",audio_backend_name(s.audio_backend))) {
                    for(auto backend:{AudioBackend::Native,AudioBackend::Oracle,AudioBackend::Hle}) {
                        ImGui::BeginDisabled(!audio_backend_available(backend));
                        if(ImGui::Selectable(audio_backend_name(backend),s.audio_backend==backend)){s.audio_backend=backend;changed=true;}
                        ImGui::EndDisabled();
                    }
                    ImGui::EndCombo();
                }
                ImGui::TextWrapped("Native/oracle audio follows confirmed frames. HLE is approximate; its music does not rewind with snapshots or rollback.");
                ImGui::TextWrapped("Volume changes host SDL output gain only. Emulated PCM, WAV captures and confirmed netplay audio remain unchanged.");ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Save states")){
                int slot=int(p.save_slot);if(ImGui::SliderInt("Slot",&slot,0,9))p.save_slot=slot;
                ImGui::BeginDisabled(state.connected||state.transferring);if(ImGui::Button("Save"))p.action(UiActionKind::SaveState,p.save_slot);ImGui::SameLine();if(ImGui::Button("Load"))p.action(UiActionKind::LoadState,p.save_slot);ImGui::EndDisabled();if(state.connected||state.transferring)ImGui::TextUnformatted("Save/load unavailable during network sessions or transfer.");
                if(ImGui::Button("Screenshot (F12)"))p.action(UiActionKind::Screenshot);ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Input")){
                const std::array<const char*,local_control_count> names={"Up","Down","Left","Right","Button 1","Button 2","Button 3","Start","Coin","Service","Test","Button 4","Button 5","Button 6"};
                for(unsigned player=0;player<s.profiles.size();++player){ImGui::PushID(int(player));char label[48];std::snprintf(label,sizeof label,"Local P%u%s",player+1,player?"":" / network controls");ImGui::SeparatorText(label);int slot=s.profiles[player].device_slot;if(ImGui::SliderInt("Gamepad device ordinal",&slot,0,15)){s.profiles[player].device_slot=slot;p.input.configure(s);changed=true;}
                    for(unsigned c=0;c<local_control_count;++c){ImGui::PushID(int(c));const auto &b=s.profiles[player].controls[c];ImGui::Text("%s: %s | button %d axis %d (%+d)",names[c],b.key==SDL_SCANCODE_UNKNOWN?"unbound":SDL_GetScancodeName(b.key),b.button,b.axis,b.direction);ImGui::SameLine();if(ImGui::SmallButton("Key"))p.input.start_capture(player,c,false);ImGui::SameLine();if(ImGui::SmallButton("Pad"))p.input.start_capture(player,c,true);ImGui::SameLine();if(ImGui::SmallButton("Clear")){s.profiles[player].controls[c]={};p.input.configure(s);changed=true;}ImGui::PopID();}ImGui::PopID();}
                if(p.input.capturing())ImGui::TextUnformatted("Press a key or gamepad control on the selected device. Escape cancels.");ImGui::TextWrapped("F1/F11/F12 and Alt+Enter are reserved. Device ordinal follows connected SDL gamepads; hotplug releases all controls. No gamepad bindings are added by default. Players 3/4 and buttons 4/5/6 are offline only; Left/Right also drive native Arkanoid/Puchi Car dial counters.");ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();if(ImGui::Button("Save preferences"))p.action(UiActionKind::SavePreferences);ImGui::SameLine();ImGui::TextUnformatted("Only this button writes the config file.");
    }
    ImGui::End();if(!visible){p.opened=false;p.input.release();}if(changed)p.action(UiActionKind::ApplySettings);ImGui::Render();p.draw_ready=p.opened;
}
void FrontendUi::render_cpu(){auto &p=*impl_;if(!p.draw_ready||!p.renderer)return;p.activate();SDL_Renderer *r=p.renderer;int w=0,h=0;SDL_RendererLogicalPresentation mode;SDL_GetRenderLogicalPresentation(r,&w,&h,&mode);SDL_Rect viewport;SDL_GetRenderViewport(r,&viewport);float sx=1,sy=1;SDL_GetRenderScale(r,&sx,&sy);SDL_Rect clip;bool clipping=SDL_RenderClipEnabled(r);SDL_GetRenderClipRect(r,&clip);
    SDL_SetRenderLogicalPresentation(r,0,0,SDL_LOGICAL_PRESENTATION_DISABLED);SDL_SetRenderViewport(r,nullptr);SDL_SetRenderScale(r,1,1);SDL_SetRenderClipRect(r,nullptr);ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),r);
    SDL_SetRenderLogicalPresentation(r,w,h,mode);SDL_SetRenderViewport(r,&viewport);SDL_SetRenderScale(r,sx,sy);SDL_SetRenderClipRect(r,clipping?&clip:nullptr);
}
void FrontendUi::render_gpu(SDL_GPUCommandBuffer *command,SDL_GPUTexture *target,Uint32,Uint32){
#ifdef F3RT_GPU
    auto &p=*impl_;if(!p.draw_ready||!p.gpu)return;p.activate();auto *data=ImGui::GetDrawData();ImGui_ImplSDLGPU3_PrepareDrawData(data,command);SDL_GPUColorTargetInfo info{};info.texture=target;info.load_op=SDL_GPU_LOADOP_LOAD;info.store_op=SDL_GPU_STOREOP_STORE;auto *pass=SDL_BeginGPURenderPass(command,&info,1,nullptr);if(!pass)throw std::runtime_error(SDL_GetError());ImGui_ImplSDLGPU3_RenderDrawData(data,command,pass);SDL_EndGPURenderPass(pass);
#else
    (void)command;(void)target;
#endif
}
}
