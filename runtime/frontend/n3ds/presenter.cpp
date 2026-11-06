#include "presenter.hpp"
#include "frontend/frame_slots.hpp"
#include "imgui_citro3d.h"
#include "imgui_ctru.h"
#include "log.hpp"
#include "menu.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>

namespace f3rt::n3ds {
namespace {
constexpr std::array<const char *, f3rt::VIDEO_STAGE_COUNT> STAGE_NAMES{
    "guest", "decode", "scene", "oracle", "capture", "latch"};

double cpi(const CpuSample &sample) {
    return sample.instructions ? double(sample.cycles) / double(sample.instructions) : 0.0;
}

std::string chunk_split(const PerfStats &perf) {
    std::string split;
    for (unsigned i = 0; i <= f3rt::compose_helper_count(); ++i) {
        if (i) split += '/';
        split += std::to_string(perf.chunks[i].load(std::memory_order_relaxed));
    }
    return split;
}

constexpr unsigned TEXTURE_WIDTH = 512, TEXTURE_HEIGHT = 256;
constexpr unsigned SCREEN_WIDTH = 400, SCREEN_HEIGHT = 240;
void *render_target(unsigned width, unsigned height) {
    return C3D_RenderTargetCreate(int(width), int(height), GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
}
} // namespace

Presenter::Presenter(Shared &shared, Settings &settings, PerfStats &perf)
    : m_shared(shared), m_settings(settings), m_perf(perf),
      m_report_since(std::chrono::steady_clock::now()) {}
Presenter::~Presenter() { shutdown(); }
bool Presenter::init() {
    C3D_Init(4 * C3D_DEFAULT_CMDBUF_SIZE);
    m_top = render_target(SCREEN_HEIGHT, SCREEN_WIDTH);
    m_bottom = render_target(SCREEN_HEIGHT, 320);
    m_texture = std::malloc(sizeof(C3D_Tex));
    std::array<uint32_t *, f3rt::FrameSlots::SLOT_COUNT> buffers{};
    for (size_t i = 0; i < m_staging.size(); ++i) {
        buffers[i] = static_cast<uint32_t *>(
            linearAlloc(size_t(TEXTURE_WIDTH) * TEXTURE_HEIGHT * sizeof(uint32_t)));
        m_staging[i] = buffers[i];
    }
    if (!m_top || !m_bottom || !m_texture || !buffers[0]) {
        log_line("n3ds: presenter allocation failed");
        return false;
    }
    for (auto *buffer : m_staging)
        if (!buffer) return false;
    m_shared.present.attach_buffers(buffers, TEXTURE_WIDTH);
    C3D_Tex *texture = static_cast<C3D_Tex *>(m_texture);
    std::memset(texture, 0, sizeof(C3D_Tex));
    if (!C3D_TexInitVRAM(texture, TEXTURE_WIDTH, TEXTURE_HEIGHT, GPU_RGBA8)) {
        log_line("n3ds: game texture allocation failed");
        return false;
    }
    m_texture_address = texture->data;
    for (auto *buffer : m_staging)
        std::memset(buffer, 0, size_t(TEXTURE_WIDTH) * TEXTURE_HEIGHT * sizeof(uint32_t));
    // Without an output binding citro3d never transfers the targets to the LCD panels when the
    // frame ends, which leaves both screens black.
    constexpr GX_TRANSFER_FORMAT format = GX_TRANSFER_FMT_RGBA8;
    const u32 transfer_flags = GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                               GX_TRANSFER_IN_FORMAT(format) |
                               GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_RenderTargetSetOutput(static_cast<C3D_RenderTarget *>(m_top), GFX_TOP, GFX_LEFT,
                              transfer_flags);
    C3D_RenderTargetSetOutput(static_cast<C3D_RenderTarget *>(m_bottom), GFX_BOTTOM, GFX_LEFT,
                              transfer_flags);
    return true;
}
bool Presenter::create_texture() {
    auto *texture = static_cast<C3D_Tex *>(m_texture);
    m_game_texture = imgui::citro3d::importTexture(texture);
    if (!m_game_texture) {
        log_line("n3ds: cannot import the game texture");
        return false;
    }
    m_game_texture_id = reinterpret_cast<void *>(m_game_texture->TexID);
    return true;
}
void Presenter::shutdown() {
    for (auto *&buffer : m_staging) {
        if (buffer) linearFree(buffer);
        buffer = nullptr;
    }
    if (m_texture) {
        std::free(m_texture);
        m_texture = nullptr;
    }
    if (m_bottom) C3D_RenderTargetDelete(static_cast<C3D_RenderTarget *>(m_bottom));
    if (m_top) C3D_RenderTargetDelete(static_cast<C3D_RenderTarget *>(m_top));
    m_bottom = m_top = nullptr;
}
void Presenter::frame(Menu &menu, const std::string &error, const std::string &audio_note) {
    const auto frame_begin = std::chrono::steady_clock::now();
    {
        bool fresh = false;
        const f3rt::FrameSnapshot *slot = m_shared.present.take(fresh);
        const unsigned width = 320 + m_settings.border * 2;
        if (slot && slot->rgba && slot->stride >= width) {
            static unsigned traced = 0;
            if (traced < 4) {
                ++traced;
                unsigned nonzero = 0;
                for (unsigned y = 0; y < 232; ++y)
                    for (unsigned x = 0; x < width; ++x)
                        nonzero += (slot->rgba[size_t(y) * slot->stride + x] & 0xffffff00u) != 0;
                log_line("n3ds: present trace frame=%llu px=%u nonzero=%u",
                         static_cast<unsigned long long>(slot->frame), width * 232, nonzero);
            }
            if (fresh) {
                m_transfer_buffer = slot->rgba;
                m_transfer_pending = true;
            }
        }
    }
    // The vendored ctru backend only refreshes input and timing; the frame itself starts here.
    imgui::ctru::newFrame();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, 480);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    ImGui::NewFrame();
    ImDrawList *background = ImGui::GetBackgroundDrawList();
    const ImGuiPlatformIO &platform = ImGui::GetPlatformIO();
    background->AddCallback(platform.DrawCallback_SetSamplerNearest, nullptr);
    m_perf.cpu_counters.store(m_settings.perf_overlay, std::memory_order_relaxed);
    if (m_settings.border) {
        background->AddImage(reinterpret_cast<ImTextureID>(m_game_texture_id), ImVec2(0, 4),
                             ImVec2(400, 236), ImVec2(0, 0),
                             ImVec2(400.f / TEXTURE_WIDTH, 232.f / TEXTURE_HEIGHT));
    } else {
        background->AddImage(reinterpret_cast<ImTextureID>(m_game_texture_id), ImVec2(40, 4),
                             ImVec2(360, 236), ImVec2(0, 0),
                             ImVec2(320.f / TEXTURE_WIDTH, 232.f / TEXTURE_HEIGHT));
    }
    background->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    Menu::Status status;
    status.emu_fps = m_emu_fps;
    status.comp_fps = m_comp_fps;
    status.comp_ms = double(m_perf.compose_us.load(std::memory_order_relaxed)) / 1000.0;
    status.audio_ms = double(m_perf.audio_queue_frames.load(std::memory_order_relaxed)) *
                      1000.0 / 48000.0;
    status.drops = m_perf.drops.load(std::memory_order_relaxed);
    menu.draw(status, error, audio_note);
    ImGui::Render();
    if (const ImDrawData *draw = ImGui::GetDrawData()) {
        static unsigned traced = 0;
        if (traced < 3) {
            ++traced;
            log_line("n3ds: draw lists=%d vtx=%d idx=%d cmd=%d size=%.0fx%.0f", draw->CmdListsCount,
                     draw->TotalVtxCount, draw->TotalIdxCount, draw->TotalVtxCount ? 1 : 0,
                     double(draw->DisplaySize.x), double(draw->DisplaySize.y));
        }
    } else {
        static bool once = false;
        if (!once) {
            once = true;
            log_line("n3ds: draw data missing");
        }
    }

    const bool transfer = m_transfer_pending;
    m_transfer_pending = false;
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    if (transfer)
        C3D_SyncDisplayTransfer(reinterpret_cast<u32 *>(m_transfer_buffer),
                                GX_BUFFER_DIM(TEXTURE_WIDTH, TEXTURE_HEIGHT),
                                static_cast<u32 *>(m_texture_address), GX_BUFFER_DIM(TEXTURE_WIDTH, TEXTURE_HEIGHT),
                                GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) |
                                    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    C3D_RenderTargetClear(static_cast<C3D_RenderTarget *>(m_top), C3D_CLEAR_ALL, 0x000000, 0);
    C3D_RenderTargetClear(static_cast<C3D_RenderTarget *>(m_bottom), C3D_CLEAR_ALL, 0x000000, 0);
    imgui::citro3d::render(static_cast<C3D_RenderTarget *>(m_top), nullptr,
                           static_cast<C3D_RenderTarget *>(m_bottom));
    C3D_FrameEnd(0);
    {
        const uint64_t frame_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - frame_begin)
                                               .count());
        const uint64_t previous = m_perf.present_us.load(std::memory_order_relaxed);
        const int64_t value =
            previous ? int64_t(previous) + (int64_t(frame_us) - int64_t(previous)) / 16
                     : int64_t(frame_us);
        m_perf.present_us.store(uint64_t(value), std::memory_order_relaxed);
    }
    m_perf.presented.fetch_add(1, std::memory_order_relaxed);
    const uint64_t emulated = m_perf.emulated.load(std::memory_order_relaxed);
    const uint64_t composed = m_perf.composed.load(std::memory_order_relaxed);
    const uint64_t presented = m_perf.presented.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - m_report_since).count();
    if (seconds >= 1.0) {
        m_emu_fps = double(emulated - m_last_emulated) / seconds;
        m_comp_fps = double(composed - m_last_composed) / seconds;
        m_present_fps = double(presented - m_last_presented) / seconds;
        m_last_emulated = emulated;
        m_last_composed = composed;
        m_last_presented = presented;
        m_report_since = now;
        report_rates();
    }
}
void Presenter::report_rates() {
    static auto last_log = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (last_log.time_since_epoch().count() && now - last_log < std::chrono::seconds(5)) return;
    last_log = now;
    const double comp_ms = double(m_perf.compose_us.load(std::memory_order_relaxed)) / 1000.0;
    const double comp_max_ms = double(m_perf.compose_max_us.load(std::memory_order_relaxed)) / 1000.0;
    log_line("n3ds: perf emu_fps=%.1f emu_ms=%.2f comp_fps=%.1f comp_ms=%.2f comp_max_ms=%.2f "
             "present_ms=%.2f present_fps=%.1f audio_ms=%.1f drops=%llu "
             "special_px=%u chunks=%s",
             m_emu_fps, double(m_perf.emu_us.load(std::memory_order_relaxed)) / 1000.0, m_comp_fps,
             comp_ms, comp_max_ms,
             double(m_perf.present_us.load(std::memory_order_relaxed)) / 1000.0, m_present_fps,
             double(m_perf.audio_queue_frames.load(std::memory_order_relaxed)) * 1000.0 / 29762.0,
             static_cast<unsigned long long>(m_perf.drops.load(std::memory_order_relaxed)),
             m_perf.special_px.load(std::memory_order_relaxed), chunk_split(m_perf).c_str());
    const CpuSample composer = m_perf.composer_cpu.load();
    if (composer.cycles)
        log_line("n3ds: cpu composer insn=%llu cycles=%llu cpi=%.2f dmiss=%llu",
                 static_cast<unsigned long long>(composer.instructions),
                 static_cast<unsigned long long>(composer.cycles), cpi(composer),
                 static_cast<unsigned long long>(composer.data_misses));
    CpuSample frame;
    std::array<CpuSample, f3rt::VIDEO_STAGE_COUNT> stages;
    for (size_t i = 0; i < stages.size(); ++i) frame += stages[i] = m_perf.emulation_cpu[i].load();
    if (frame.cycles) {
        std::string split;
        for (size_t i = 0; i < stages.size(); ++i) {
            char part[96];
            std::snprintf(part, sizeof(part), " %s=%llu/%llu/%llu", STAGE_NAMES[i],
                          static_cast<unsigned long long>(stages[i].instructions),
                          static_cast<unsigned long long>(stages[i].cycles),
                          static_cast<unsigned long long>(stages[i].data_misses));
            split += part;
        }
        log_line("n3ds: cpu emulation insn=%llu cycles=%llu cpi=%.2f dmiss=%llu stages(insn/cycles/"
                 "dmiss):%s",
                 static_cast<unsigned long long>(frame.instructions),
                 static_cast<unsigned long long>(frame.cycles), cpi(frame),
                 static_cast<unsigned long long>(frame.data_misses), split.c_str());
    }
    m_perf.compose_max_us.store(0, std::memory_order_relaxed);
}
} // namespace f3rt::n3ds
