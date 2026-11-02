#include "audio_ndsp.hpp"
#include "composer.hpp"
#include "input.hpp"
#include "log.hpp"
#include "menu.hpp"
#include "perf.hpp"
#include "presenter.hpp"
#include "session.hpp"
#include "settings.hpp"
#include <3ds.h>
#include <imgui.h>
#include <imgui_citro3d.h>
#include <imgui_ctru.h>
#include <filesystem>
#include <memory>
#include <string>

extern "C" {
// Azahar grants a 3dsx 96 MiB; the DSP buffers and decoded assets live in the linear heap.
u32 __ctru_linear_heap_size = 6 * 1024 * 1024;
u32 __stacksize__ = 0x40000;
}

extern "C" void (*f3rt_pthread_stall)(const void *lock, unsigned long long waited_us);

namespace {
using namespace f3rt::n3ds;

aptHookCookie g_hook;
Shared g_shared;
Session *g_session = nullptr;

void apt_hook(APT_HookType type, void *) {
    if (!g_session) return;
    switch (type) {
    case APTHOOK_ONSUSPEND:
        g_session->post(Command::SystemPause);
        break;
    case APTHOOK_ONRESTORE:
    case APTHOOK_ONWAKEUP:
        g_session->post(Command::SystemResume);
        break;
    case APTHOOK_ONEXIT:
        g_shared.quit.store(true);
        break;
    default:
        break;
    }
}
} // namespace

int main() {
    osSetSpeedupEnable(true);
    APT_SetAppCpuTimeLimit(80);
    romfsInit();
    gfxInit(GSP_BGR8_OES, GSP_BGR8_OES, false);
    gfxSet3D(false);
    consoleDebugInit(debugDevice_SVC);

    const std::string set = F3RT_DEFAULT_SET;
    const std::string data_dir = "/3ds/f3-recomp/" + set;
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    log_open((data_dir + "/f3rt.log").c_str());
    log_line("n3ds: start set=%s data=%s", set.c_str(), data_dir.c_str());
    f3rt_pthread_stall = [](const void *lock, unsigned long long waited_us) {
        log_line("n3ds: mutex stall lock=%p us=%llu", lock, waited_us);
    };

    Settings settings = default_settings();
    load_settings(data_dir + "/settings.cfg", settings);
    log_line("n3ds: stage settings");

    std::unique_ptr<NdspAudioOutput> audio;
    std::string audio_note;
    log_line("n3ds: stage dsp");
    const bool dsp = R_SUCCEEDED(ndspInit());
    if (dsp) audio = std::make_unique<NdspAudioOutput>();
    if (!audio || !audio->valid()) {
        audio.reset();
        audio_note = "Audio off: sdmc:/3ds/dspfirm.cdc missing";
        log_line("n3ds: %s", audio_note.c_str());
    }

    PerfStats perf;
    Presenter presenter(g_shared, settings, perf);
    log_line("n3ds: stage presenter");
    bool present_ok = presenter.init();
    log_line("n3ds: stage context");
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    imgui::ctru::init();
    // Touch only: the gamepad navigation path would move focus away from the tapped widget.
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    ImGui::GetStyle().TouchExtraPadding = ImVec2(4, 4);
    log_line("n3ds: stage ctru");
    imgui::citro3d::init();
    log_line("n3ds: stage texture");
    present_ok = presenter.create_texture() && present_ok;
    log_line("n3ds: threads emulation=2 audio=0 composer=1 helper_cores=0 count=%u",
             f3rt::compose_helper_count());
    log_line("n3ds: memory app_free_kib=%u",
             unsigned(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024));

    auto session = std::make_unique<Session>(g_shared, settings, set, data_dir, audio.get(), perf);
    g_session = session.get();
    auto composer = std::make_unique<Composer>(g_shared, *session, settings, perf);
    auto menu = std::make_unique<Menu>(g_shared, *session, settings, data_dir + "/settings.cfg",
                                       perf);
    aptHook(&g_hook, apt_hook, nullptr);
    composer->start();
    session->start();

    while (aptMainLoop() && !g_shared.quit.load() && present_ok) {
        hidScanInput();
        session->input.store(menu->open() ? 0
                                          : map_input(hidKeysHeld(), settings.keys));
        presenter.frame(*menu, session->error(), audio_note);
        if (menu->take_restart()) {
            // A border change reconfigures the capture geometry: rebuild the game threads.
            composer->stop();
            session->stop();
            g_session = nullptr;
            session = std::make_unique<Session>(g_shared, settings, set, data_dir, audio.get(),
                                                perf);
            g_session = session.get();
            composer = std::make_unique<Composer>(g_shared, *session, settings, perf);
            menu = std::make_unique<Menu>(g_shared, *session, settings,
                                          data_dir + "/settings.cfg", perf);
            composer->start();
            session->start();
        }
    }

    g_session = nullptr;
    aptUnhook(&g_hook);
    composer->stop();
    session->stop();
    audio.reset();
    imgui::citro3d::exit();
    presenter.shutdown();
    ImGui::DestroyContext();
    if (dsp) ndspExit();
    C3D_Fini();
    gfxExit();
    romfsExit();
    return present_ok ? 0 : 1;
}
