#pragma once
#include <SDL3/SDL.h>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace f3rt {
// Optional host overlay invoked on the window composition target (the swapchain directly, or the
// offscreen capture texture during window capture) after the game blit.
using GpuOverlay = void (*)(void *, SDL_GPUCommandBuffer *, SDL_GPUTexture *composition_target,
                            Uint32, Uint32);

// --- C ABI shared with crates/f3rt-video/src/gpu/driver.rs ------------------
// Field order and types must match `GpuDriverBlit`, `GpuDriverTextureSamplerBinding` and
// `GpuDriverSwapchain` exactly.

struct GpuDriverBlit {
    SDL_GPUTexture *source;
    uint32_t source_x, source_y, source_width, source_height;
    SDL_GPUTexture *destination;
    uint32_t x, y, width, height;
    uint32_t load_op;
    uint32_t filter;
    float clear_color[4];
};

struct GpuDriverTextureSamplerBinding {
    SDL_GPUTexture *texture;
    SDL_GPUSampler *sampler;
};

struct GpuDriverSwapchain {
    SDL_GPUTexture *texture;
    Uint32 width;
    Uint32 height;
};

// The shim owns the SDL window association and the host overlay; Rust owns object lifetimes.
struct GpuDriverShim {
    SDL_Window *window = nullptr;
    GpuOverlay overlay = nullptr;
    void *overlay_userdata = nullptr;
};

struct GpuDriverVtable {
    void *(*create_device)(void *self);
    const char *(*last_error)(void *self);
    bool (*claim_window)(void *self, void *device);
    bool (*set_swapchain_mode)(void *self, void *device, bool vsync);
    void (*release_window)(void *self, void *device);
    void (*destroy_device)(void *self, void *device);
    void (*wait_idle)(void *self, void *device);
    bool (*is_metal)(void *self, void *device);
    const char *(*driver_name)(void *self, void *device);
    double (*display_hz)(void *self);
    uint32_t (*swapchain_format)(void *self, void *device);
    void *(*create_sampler)(void *self, void *device);
    void *(*create_buffer)(void *self, void *device, uint32_t bytes);
    void *(*create_texture)(void *self, void *device, uint32_t format, uint32_t width,
                            uint32_t height);
    void *(*create_transfer)(void *self, void *device, uint32_t bytes, uint32_t usage);
    void *(*create_pipeline)(void *self, void *device, uint32_t vertex, uint32_t fragment,
                             uint32_t format);
    void *(*create_user_pipeline)(void *self, void *device, const uint8_t *code, size_t size,
                                  const char *entry, uint32_t samplers, uint32_t buffers,
                                  uint32_t uniforms, uint32_t format);
    void (*release_buffer)(void *self, void *device, void *buffer);
    void (*release_texture)(void *self, void *device, void *texture);
    void (*release_transfer)(void *self, void *device, void *transfer);
    void (*release_sampler)(void *self, void *device, void *sampler);
    void (*release_pipeline)(void *self, void *device, void *pipeline);
    void *(*map_transfer)(void *self, void *device, void *transfer, bool cycle);
    void (*unmap_transfer)(void *self, void *device, void *transfer);
    void *(*acquire_command)(void *self, void *device);
    bool (*submit_command)(void *command);
    bool (*cancel_command)(void *command);
    int32_t (*submit_and_wait)(void *self, void *device, void *command);
    int32_t (*acquire_swapchain)(void *self, void *command, bool wait,
                                 GpuDriverSwapchain *out);
    void *(*begin_copy_pass)(void *command);
    void (*end_copy_pass)(void *pass);
    void *(*begin_render_pass)(void *command, void *texture, bool load);
    void (*end_render_pass)(void *pass);
    void (*upload_buffer)(void *pass, void *source, uint32_t source_offset, void *destination,
                          uint32_t size, bool cycle);
    void (*upload_texture)(void *pass, void *source, uint32_t source_offset, uint32_t width,
                           uint32_t height, void *destination, bool cycle);
    void (*download_texture)(void *pass, void *source, uint32_t width, uint32_t height,
                             void *destination);
    void (*push_vertex_uniforms)(void *command, uint32_t slot, const void *data, size_t size);
    void (*push_fragment_uniforms)(void *command, uint32_t slot, const void *data, size_t size);
    void (*bind_pipeline)(void *pass, void *pipeline);
    void (*bind_vertex_storage_buffers)(void *pass, uint32_t first_slot, void *const *buffers,
                                        size_t count);
    void (*bind_fragment_storage_buffers)(void *pass, uint32_t first_slot, void *const *buffers,
                                          size_t count);
    void (*bind_fragment_samplers)(void *pass, uint32_t first_slot,
                                   const GpuDriverTextureSamplerBinding *bindings, size_t count);
    void (*set_scissor)(void *pass, int32_t x, int32_t y, int32_t width, int32_t height);
    void (*draw)(void *pass, uint32_t vertices, uint32_t instances);
    void (*blit)(void *command, const GpuDriverBlit *blit);
    void (*invoke_overlay)(void *self, void *command, void *target, uint32_t width,
                           uint32_t height);
    int32_t (*save_surface)(void *self, const char *path, const uint32_t *pixels, uint32_t width,
                            uint32_t height, uint32_t pixel_format, bool png);
    bool (*set_frames_in_flight)(void *self, void *device, uint32_t count);
    void *(*create_pacer)(void *self);
    void (*destroy_pacer)(void *self, void *pacer);
    bool (*wait_pacer)(void *self, void *pacer);
    bool (*poll_pacer)(void *self, void *pacer);
    double (*pacer_callback_hz)(void *self, void *pacer);
    double (*pacer_requested_hz)(void *self, void *pacer);
    uint64_t (*monotonic_ns)(void *self);
    uint64_t (*ticks_ns)(void *self);
};

static_assert(sizeof(GpuDriverVtable) == 56 * sizeof(void *),
              "GpuDriverVtable must stay in sync with crates/f3rt-video/src/gpu/driver.rs");
static_assert(sizeof(GpuDriverBlit) == 72, "GpuDriverBlit layout must match Rust");
static_assert(sizeof(GpuDriverTextureSamplerBinding) == 2 * sizeof(void *));
static_assert(sizeof(GpuDriverSwapchain) == 16);

/// The static table; the renderer passes this pointer into Rust at `GpuVideo` creation.
const GpuDriverVtable *sdl_driver_vtable();

/// Creates the shim for `window` (may be null for offscreen use).
std::unique_ptr<GpuDriverShim> create_sdl_driver(SDL_Window *window);
} // namespace f3rt
