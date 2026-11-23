#pragma once
#include "f3rt/game_video.hpp"
#include "renderer/scale.hpp"
#include "video_shaders.hpp"
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>

namespace f3rt {
class DisplayPacer;
class GpuContext;

[[noreturn]] void gpu_fail(const char *operation);
template <class T> T *gpu_checked(T *value, const char *operation) {
    if (!value) gpu_fail(operation);
    return value;
}

struct GpuSurface {
    SDL_GPUTexture *texture = nullptr;
    uint32_t width = 0, height = 0;
    explicit operator bool() const noexcept { return texture != nullptr; }
};

// Move-only owner of one SDL GPU object. SDL defers the actual destruction until queued commands
// finish using it, so releasing never stalls the GPU; the device must outlive every handle.
template <class T, void (*Release)(SDL_GPUDevice *, T *)> class GpuHandle {
public:
    GpuHandle() = default;
    GpuHandle(SDL_GPUDevice *device, T *handle) noexcept : m_device(device), m_handle(handle) {}
    GpuHandle(GpuHandle &&other) noexcept
        : m_device(other.m_device), m_handle(std::exchange(other.m_handle, nullptr)) {}
    GpuHandle &operator=(GpuHandle &&other) noexcept {
        if (this != &other) {
            reset();
            m_device = other.m_device;
            m_handle = std::exchange(other.m_handle, nullptr);
        }
        return *this;
    }
    GpuHandle(const GpuHandle &) = delete;
    GpuHandle &operator=(const GpuHandle &) = delete;
    ~GpuHandle() { reset(); }
    void reset() noexcept {
        if (m_handle) Release(m_device, m_handle);
        m_handle = nullptr;
    }
    T *get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != nullptr; }

protected:
    SDL_GPUDevice *m_device = nullptr;

private:
    T *m_handle = nullptr;
};

using GpuBuffer = GpuHandle<SDL_GPUBuffer, SDL_ReleaseGPUBuffer>;
using GpuPipeline = GpuHandle<SDL_GPUGraphicsPipeline, SDL_ReleaseGPUGraphicsPipeline>;
using GpuSampler = GpuHandle<SDL_GPUSampler, SDL_ReleaseGPUSampler>;

class GpuTexture : public GpuHandle<SDL_GPUTexture, SDL_ReleaseGPUTexture> {
public:
    GpuTexture() = default;
    GpuTexture(SDL_GPUDevice *device, SDL_GPUTexture *texture, SDL_GPUTextureFormat format,
               uint32_t width, uint32_t height) noexcept
        : GpuHandle(device, texture), m_format(format), m_width(width), m_height(height) {}
    GpuSurface surface() const noexcept { return {get(), m_width, m_height}; }
    // Keeps the current texture when it already has this format and size; otherwise replaces it.
    void ensure(const GpuContext &ctx, SDL_GPUTextureFormat format, uint32_t width, uint32_t height);

private:
    SDL_GPUTextureFormat m_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    uint32_t m_width = 0, m_height = 0;
};

class GpuMapping {
public:
    GpuMapping(SDL_GPUDevice *device, SDL_GPUTransferBuffer *transfer, void *data) noexcept
        : m_device(device), m_transfer(transfer), m_data(data) {}
    GpuMapping(const GpuMapping &) = delete;
    GpuMapping &operator=(const GpuMapping &) = delete;
    ~GpuMapping() { SDL_UnmapGPUTransferBuffer(m_device, m_transfer); }
    void *data() const noexcept { return m_data; }
    std::span<uint32_t> words(size_t count) const noexcept {
        return {static_cast<uint32_t *>(m_data), count};
    }

private:
    SDL_GPUDevice *m_device;
    SDL_GPUTransferBuffer *m_transfer;
    void *m_data;
};

class GpuTransfer : public GpuHandle<SDL_GPUTransferBuffer, SDL_ReleaseGPUTransferBuffer> {
public:
    GpuTransfer() = default;
    GpuTransfer(SDL_GPUDevice *device, SDL_GPUTransferBuffer *transfer, uint32_t size) noexcept
        : GpuHandle(device, transfer), m_size(size) {}
    uint32_t size() const noexcept { return m_size; }
    // `cycle` lets SDL hand out fresh storage when the previous contents are still in flight.
    GpuMapping map(bool cycle, const char *operation) const;

private:
    uint32_t m_size = 0;
};

// Optional host overlay invoked on the window composition target (the swapchain directly, or the
// offscreen capture texture during window capture) after the game blit.
using GpuOverlay = void (*)(void *, SDL_GPUCommandBuffer *, SDL_GPUTexture *composition_target,
                            Uint32, Uint32);

enum class PresentAcquire : uint8_t { DrawableReady, HeadlessProceed, SkipFrame };

// Live motion alpha is the elapsed fraction of the native frame period, sampled after acquisition.
struct MotionLiveClock {
    std::chrono::steady_clock::time_point frame_start;
    std::chrono::nanoseconds period{};
};
// monostate: static (canonical); float: fixed alpha; MotionLiveClock: timed live alpha.
using PresentTiming = std::variant<std::monostate, float, MotionLiveClock>;

struct PresentRequest {
    // Block for the swapchain and any display tick. False skips the frame when the GPU is busy:
    // nothing is uploaded or rendered, and no fence is waited on.
    bool wait = false;
    PresentTiming timing{};
    // Empty: one full-window blit. Otherwise the left half shows this (already oriented) surface
    // and the right half the presented game image.
    GpuSurface left_pane{};
    // One-shot PNG of the full-window composition submitted to the drawable, including overlay.
    // Null adds no readback, allocation or fence wait.
    const char *capture_path = nullptr;

    std::optional<float> sample_alpha() const;
};

class GpuCommand;

class GpuContext {
public:
    // SDL video must be initialized. Window may be null for offscreen use.
    GpuContext(SDL_Window *window, bool vsync);
    ~GpuContext();
    GpuContext(const GpuContext &) = delete;
    GpuContext &operator=(const GpuContext &) = delete;

    GpuCommand begin_command() const;
    GpuBuffer create_buffer(uint32_t bytes) const;
    GpuTexture create_texture(SDL_GPUTextureFormat format, uint32_t width, uint32_t height) const;
    GpuPipeline create_pipeline(const video_shaders::Shader &vertex,
                                const video_shaders::Shader &fragment,
                                SDL_GPUTextureFormat format) const;
    GpuTransfer create_transfer(uint32_t bytes, SDL_GPUTransferBufferUsage usage) const;
    void wait_idle() const noexcept { SDL_WaitForGPUIdle(m_device); }
    bool is_metal() const noexcept;
    const char *driver() const noexcept { return SDL_GetGPUDeviceDriver(m_device); }
    SDL_GPUDevice *device() const noexcept { return m_device; }
    SDL_Window *window() const noexcept { return m_window; }
    SDL_GPUSampler *sampler() const noexcept { return m_sampler.get(); }
    bool vsync() const noexcept { return m_vsync; }

private:
    void destroy() noexcept;
    SDL_GPUShader *create_shader(const video_shaders::Shader &source, SDL_GPUShaderStage stage) const;
    SDL_GPUDevice *m_device = nullptr;
    SDL_Window *m_window = nullptr;
    bool m_claimed = false;
    bool m_vsync = true;
    GpuSampler m_sampler;
};

// One GPU command buffer. Destruction submits it when a swapchain texture was acquired (SDL
// requires that) and cancels it otherwise.
class GpuCommand {
public:
    explicit GpuCommand(SDL_GPUDevice *device);
    GpuCommand(GpuCommand &&other) noexcept;
    GpuCommand(const GpuCommand &) = delete;
    GpuCommand &operator=(const GpuCommand &) = delete;
    GpuCommand &operator=(GpuCommand &&) = delete;
    ~GpuCommand();

    PresentAcquire acquire_for_present(const GpuContext &ctx, DisplayPacer &pacer,
                                       const PresentRequest &request);

    SDL_GPUCommandBuffer *raw() const noexcept { return m_cmd; }
    SDL_GPUCopyPass *begin_copy_pass(const char *operation);
    SDL_GPURenderPass *begin_render_pass(SDL_GPUTexture *target, bool load = false);
    static void upload_buffer(SDL_GPUCopyPass *pass, const GpuTransfer &source,
                              const GpuBuffer &destination, uint32_t offset, uint32_t size,
                              bool cycle);
    // Full-surface nearest copy; `destination` must be at least as large as `source`.
    void blit_copy(GpuSurface source, GpuSurface destination);
    // Blits `surface` (and `left_pane`, when set) onto the acquired swapchain, runs the overlay,
    // submits, and optionally saves the composed window to `capture_path`. Returns whether a
    // swapchain texture was submitted. `rotated`: the surfaces are portrait.
    bool present(const GpuContext &ctx, GpuSurface surface, GpuSurface left_pane,
                 VideoScaleMode scale_mode, GameVideoOptions options, bool linear, bool rotated,
                 GpuOverlay overlay, void *overlay_userdata, const char *capture_path);
    void submit();
    // Submits and blocks until the GPU finished this command.
    void submit_and_wait();

private:
    SDL_GPUDevice *m_device;
    SDL_GPUCommandBuffer *m_cmd;
    GpuSurface m_swapchain{};
    bool m_acquired = false;
};

class ReadbackBuffer {
public:
    void reset() noexcept { m_download.reset(); }
    // Consumes `cmd`, copies `surface` into `output` (ARGB8888, width*height pixels) and waits.
    void finish(GpuCommand &cmd, GpuSurface surface, const GpuContext &ctx,
                std::span<uint32_t> output);

private:
    GpuTransfer m_download;
};
}
