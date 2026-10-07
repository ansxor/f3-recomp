#include "renderer/game/compositor.hpp"
#include "renderer/game/lines.hpp"
#include "renderer/game/text.hpp"
#include "renderer/game/tiles.hpp"
#include "renderer/game/clip.hpp"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace f3rt {
namespace {
struct PixelMix {
    uint16_t source, destination;
    uint8_t source_weight, destination_weight;
    uint8_t source_priority, destination_priority, source_mode;
};
void mix(PixelMix &pixel, const SceneLayer &layer, uint16_t color, bool select,
         const std::array<uint8_t, 4> &blend) {
    if (!color || layer.blend_mode == pixel.source_mode) return;
    if (layer.priority > pixel.source_priority) {
        unsigned slot = unsigned(select);
        if (layer.blend_mode == 1 || layer.blend_mode == 2) {
            if (layer.blend_mode == 1) slot += 2;
            if (!blend[slot]) return;
            pixel.source_weight = blend[slot];
        } else {
            if (!blend[slot] && !blend[slot + 2]) return;
            pixel.source_weight = blend[slot + 2];
            pixel.destination_weight = blend[slot];
            pixel.destination_priority = layer.priority;
            pixel.destination = color;
        }
        pixel.source = color;
        pixel.source_mode = layer.blend_mode;
        pixel.source_priority = layer.priority;
    } else if (layer.priority >= pixel.destination_priority) {
        pixel.destination = layer.priority == pixel.destination_priority ? 0 : color;
        pixel.destination_priority = layer.priority;
        pixel.destination_weight = blend[unsigned(select) + (pixel.source_mode == 1 ? 0 : 2)];
    }
}
uint32_t rgb(const PixelMix &pixel, std::span<const uint32_t> palette) {
    if (pixel.source_weight == 8 && pixel.destination_weight == 0)
        return 0xff000000u | palette[pixel.source & 8191];
    if (pixel.source_weight == 0 && pixel.destination_weight == 8)
        return 0xff000000u | palette[pixel.destination & 8191];
    const uint32_t source = palette[pixel.source & 8191];
    const uint32_t destination = palette[pixel.destination & 8191];
    uint32_t color = 0xff000000;
    for (unsigned shift : {0u, 8u, 16u}) {
        const unsigned value = (((source >> shift) & 255) * pixel.source_weight +
                                ((destination >> shift) & 255) * pixel.destination_weight) >> 3;
        color |= std::min(value, 255u) << shift;
    }
    return color;
}
int floor_divide(int value, int divisor) {
    return value >= 0 ? value / divisor : -1 - (-1 - value) / divisor;
}
struct SceneJob {
    const FrameScene &scene;
    const SceneTarget &target;
};

void validate_scene(const SceneJob &job) {
    const auto options = job.target.geometry;
    if (!options.scale || options.scale > GameVideoOptions::max_gpu_scale || options.border > GameVideoOptions::max_border)
        throw std::runtime_error("Game presentation scale/border out of range");
    const size_t sprite_size = options.expanded() ? size_t(options.width()) * options.height() : 432 * 256;
    if (job.target.sprites.size() < sprite_size || job.scene.colors.size() < 8192 ||
        job.target.output.size() < size_t(options.width()) * options.height())
        throw std::runtime_error("Incomplete game scene render buffers");
    // Reject unsupported rows on the caller before publishing any work.
    for (unsigned y = 24; y < 256; ++y)
        if (job.scene.rows[y].bitmap)
            throw std::runtime_error("Game scene bitmap pivot is unsupported");
}

// Scale zero is the fixed native frame; 1..8 retain expanded border geometry.
// Constant divisors avoid per-texel integer division at every supported scale.
template<unsigned Scale>
void compose_rows(const SceneJob &job, unsigned begin, unsigned end) noexcept {
    constexpr bool Expanded = Scale != 0;
    const auto &scene = job.scene;
    const auto &tiles = scene.tiles;
    const auto &text = scene.text;
    const bool flipped = scene.flipped;
    const auto tile_pixels = scene.tile_pixels;
    const auto colors = scene.colors;
    const auto sprites = job.target.sprites;
    const auto output = job.target.output;
    const auto options = job.target.geometry;
    constexpr int scale = Expanded ? int(Scale) : 1;
    const int width = Expanded ? int(options.width()) : 320;
    const int left_edge = Expanded ? 46 - int(options.border) : 46;
    const int right_edge = Expanded ? 366 + int(options.border) : 366;
    constexpr unsigned max_width = Expanded
        ? (320 + GameVideoOptions::max_border * 2) * GameVideoOptions::max_gpu_scale : 320;
    std::array<PixelMix, max_width> pixels;
    for (unsigned y = begin; y < end; ++y) {
        const auto &row = scene.rows[y & 255];
        const auto order = scene_order(row);
        std::array<bool, layer_count> active;
        for (unsigned i = 0; i < layer_count; ++i) {
            const LayerId id{uint8_t(i)};
            active[i] = row.layer(id).enabled && (scene.layer_mask & layer_bit(id)) != 0;
        }
        std::array<ClipRanges, layer_count> clips;
        for (LayerId id : order)
            if (active[unsigned(id)]) clips[unsigned(id)] = clip_ranges(row, row.layer(id), int16_t(left_edge), int16_t(right_edge));
        for (int sub_y = 0; sub_y < scale; ++sub_y) {
            const unsigned output_y = (y - 24) * scale + sub_y;
            std::fill_n(pixels.begin(), width, PixelMix{0, row.background, 0, 8, 0, 0, 255});
            for (LayerId id : order) {
                const auto &state = row.layer(id);
                if (!active[unsigned(id)]) continue;
                const LayerKind layer_kind = kind(id);
                GameTiles::RowSampler tile_sampler;
                if (layer_kind == LayerKind::Playfield) {
                    const auto &pf = row.playfields[sub_index(id)];
                    const int fy = (int(pf.y_fraction) * scale + sub_y * pf.y_step) / scale;
                    const int source_y = pf.source_y + (fy >> 8);
                    tile_sampler = tiles.row_sampler(sub_index(id), source_y, flipped, tile_pixels);
                }
                // Expanded subcolumns often resolve to the same source texel.
                // Cache only within one layer/subrow; palette offsets apply to copies.
                ScenePixel cached_source{};
                int cached_x = 0;
                bool cached_valid = false;
                const auto &ranges = clips[unsigned(id)];
                for (unsigned r = 0; r < ranges.count; ++r) {
                    const int left = (std::max<int>(left_edge, ranges.ranges[r].left) - left_edge) * scale;
                    const int right = (std::min<int>(right_edge, ranges.ranges[r].right) - left_edge) * scale;
                    for (int output_x = left; output_x < right; ++output_x) {
                        auto &pixel = pixels[output_x];
                        if (state.blend_mode == pixel.source_mode) continue;
                        int sample_x = output_x + left_edge * scale;
                        if (state.mosaic && row.mosaic_period > 1) {
                            const int hardware_x = left_edge + output_x / scale;
                            // Validated border bounds keep this phase in [-46, 593].
                            const int phase = hardware_x + 68;
                            const int count = phase < 0 ? phase + 432 : phase >= 432 ? phase - 432 : phase;
                            sample_x = (hardware_x - count % row.mosaic_period) * scale;
                        }
                        ScenePixel source;
                        bool select = state.blend_select;
                        if (layer_kind == LayerKind::Playfield) {
                            const auto &pf = row.playfields[sub_index(id)];
                            // Divide only after combining the native phase and output
                            // subpixel. This samples geometry, not an enlarged RGB frame.
                            const int x = floor_divide(pf.source_x * scale + (sample_x - 46 * scale) * pf.x_step, scale * 256);
                            if constexpr (Expanded) {
                                if (!cached_valid || cached_x != x) {
                                    cached_source = tile_sampler.pixel(x);
                                    cached_x = x;
                                    cached_valid = true;
                                }
                                source = cached_source;
                            } else {
                                source = tile_sampler.pixel(x);
                            }
                            select = (source.flags & 1) != 0;
                            if (!(source.flags & 0x10) || !source.palette) continue;
                            source.palette = uint16_t(source.palette + pf.palette_add);
                        } else if (layer_kind == LayerKind::Sprite) {
                            if constexpr (Expanded) {
                                const int source_x = sample_x - left_edge * scale;
                                if (source_x < 0 || source_x >= width) continue;
                                source.palette = sprites[output_y * width + source_x];
                            } else {
                                if (sample_x < 0 || sample_x >= 432) continue;
                                source.palette = sprites[y * 432 + sample_x];
                            }
                            if (!source.palette || sprite_layer(source.palette) != id) continue;
                        } else {
                            const int x = floor_divide(row.text_x * scale + sample_x - 46 * scale, scale);
                            if constexpr (Expanded) {
                                if (!cached_valid || cached_x != x) {
                                    cached_source = text.pixel(x, row.text_y, flipped);
                                    cached_x = x;
                                    cached_valid = true;
                                }
                                source = cached_source;
                            } else {
                                source = text.pixel(x, row.text_y, flipped);
                            }
                            if (!(source.flags & 0x10)) continue;
                        }
                        mix(pixel, state, source.palette, select, row.blend);
                    }
                }
            }
            for (int x = 0; x < width; ++x) output[output_y * width + x] = rgb(pixels[x], colors);
        }
    }
}

void compose_expanded_rows(const SceneJob &job, unsigned begin, unsigned end) noexcept {
    using Kernel = void (*)(const SceneJob &, unsigned, unsigned) noexcept;
    static constexpr std::array<Kernel, 8> kernels{
        compose_rows<1>, compose_rows<2>, compose_rows<3>, compose_rows<4>,
        compose_rows<5>, compose_rows<6>, compose_rows<7>, compose_rows<8>
    };
    static_assert(kernels.size() == GameVideoOptions::max_gpu_scale);
    kernels[job.target.geometry.scale - 1](job, begin, end);
}

// Three persistent workers plus the caller leave room for emulation/audio on
// a ten-core host. Each participant owns a contiguous native-row interval.
class RowWorkers {
public:
    RowWorkers() {
        const unsigned hardware = std::thread::hardware_concurrency();
        count_ = hardware ? std::min(3u, hardware - 1) : 1;
        try {
            for (unsigned i = 0; i < count_; ++i)
                threads_[i] = std::thread([this, i] { work(i + 1); });
        } catch (...) {
            stop();
            throw;
        }
    }
    ~RowWorkers() { stop(); }
    RowWorkers(const RowWorkers &) = delete;
    RowWorkers &operator=(const RowWorkers &) = delete;

    void compose(const SceneJob &job) {
        // Serialize submissions, not sampling: stack-owned jobs remain alive
        // until every participant has finished, including concurrent callers.
        std::unique_lock submission(submission_mutex_);
        {
            std::lock_guard lock(mutex_);
            job_ = &job;
            remaining_ = count_;
            ++generation_;
        }
        ready_.notify_all();
        compose_part(job, 0);
        std::unique_lock lock(mutex_);
        done_.wait(lock, [this] { return remaining_ == 0; });
        job_ = nullptr;
    }

private:
    void compose_part(const SceneJob &job, unsigned part) const noexcept {
        const unsigned participants = count_ + 1;
        compose_expanded_rows(job, 24 + 232 * part / participants,
                     24 + 232 * (part + 1) / participants);
    }
    void work(unsigned part) {
        uint64_t observed = 0;
        std::unique_lock lock(mutex_);
        for (;;) {
            ready_.wait(lock, [this, observed] { return stopping_ || generation_ != observed; });
            if (stopping_) return;
            observed = generation_;
            const SceneJob *job = job_;
            lock.unlock();
            compose_part(*job, part);
            lock.lock();
            if (--remaining_ == 0) done_.notify_one();
        }
    }
    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto &thread : threads_)
            if (thread.joinable()) thread.join();
    }

    std::array<std::thread, 3> threads_;
    std::mutex submission_mutex_, mutex_;
    std::condition_variable ready_, done_;
    const SceneJob *job_ = nullptr;
    uint64_t generation_ = 0;
    unsigned count_ = 0, remaining_ = 0;
    bool stopping_ = false;
};
} // namespace

void compose_game_scene(const FrameScene &scene, const SceneTarget &target, ComposeMode mode) {
    const SceneJob job{scene, target};
    validate_scene(job);
    const bool expanded = target.geometry.expanded();
    if (mode == ComposeMode::Serial) {
        if (expanded) compose_expanded_rows(job, 24, 256);
        else compose_rows<0>(job, 24, 256);
    } else if (!expanded) {
        // Constant native geometry also removes per-pixel variable division.
        compose_rows<0>(job, 24, 256);
    } else {
        static RowWorkers workers;
        workers.compose(job);
    }
}
} // namespace f3rt
