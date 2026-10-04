#include "game_compositor.hpp"
#include "game_lines.hpp"
#include "game_text.hpp"
#include "game_tiles.hpp"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace f3rt {
namespace {
struct ClipRanges {
    std::array<SceneClip, 16> ranges{};
    unsigned count = 1;
};
ClipRanges clip_ranges(const SceneRow &row, const SceneLayer &layer, int16_t left, int16_t right) {
    ClipRanges result;
    result.ranges[0] = {left, right};
    unsigned normal = layer.clip_enabled & ~layer.clip_inverted;
    unsigned inverted = layer.clip_enabled & layer.clip_inverted;
    if (!layer.clip_inverse) std::swap(normal, inverted);
    for (unsigned plane = 0; plane < 4; ++plane) {
        const auto clip = row.clips[plane];
        if (normal & (1u << plane)) {
            unsigned count = 0;
            for (unsigned i = 0; i < result.count; ++i) {
                const auto range = result.ranges[i];
                if (clip.left <= clip.right && range.right >= clip.left && range.left <= clip.right)
                    result.ranges[count++] = {std::max(range.left, clip.left), std::min(range.right, clip.right)};
            }
            result.count = count;
        } else if ((inverted & (1u << plane)) && clip.left <= clip.right) {
            ClipRanges next;
            next.count = 0;
            for (unsigned i = 0; i < result.count * 2; ++i) {
                SceneClip candidate = i < result.count ? SceneClip{left, clip.left} : SceneClip{clip.right, right};
                bool keep = true;
                for (unsigned j = 0; j < result.count; ++j) {
                    const auto range = result.ranges[j];
                    // Preserve the observed oracle's inverted-window combining rule.
                    candidate.left = std::max(range.left, candidate.left);
                    candidate.right = std::max(range.left, candidate.right);
                    if (candidate.left >= candidate.right) { keep = false; break; }
                }
                if (keep) next.ranges[next.count++] = candidate;
            }
            result = next;
        }
    }
    return result;
}
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
    const GameTiles &tiles;
    const GameText &text;
    const GameLines &lines;
    std::span<const uint16_t> sprites;
    bool flipped;
    std::span<const uint8_t> tile_pixels;
    std::span<const uint32_t> colors;
    std::span<uint32_t> output;
    GameVideoOptions options;
};

void validate_scene(const SceneJob &job) {
    const auto options = job.options;
    if (!options.scale || options.scale > GameVideoOptions::max_scale || options.border > GameVideoOptions::max_border)
        throw std::runtime_error("Game presentation scale/border out of range");
    const size_t sprite_size = options.expanded() ? size_t(options.width()) * options.height() : 432 * 256;
    if (job.sprites.size() < sprite_size || job.colors.size() < 8192 ||
        job.output.size() < size_t(options.width()) * options.height())
        throw std::runtime_error("Incomplete game scene render buffers");
    // Reject unsupported rows on the caller before publishing any work.
    for (unsigned y = 24; y < 256; ++y)
        if (job.lines.row(y).bitmap)
            throw std::runtime_error("Game scene bitmap pivot is unsupported");
}

void compose_rows(const SceneJob &job, unsigned begin, unsigned end) noexcept {
    const auto &tiles = job.tiles;
    const auto &text = job.text;
    const auto &lines = job.lines;
    const auto sprites = job.sprites;
    const bool flipped = job.flipped;
    const auto tile_pixels = job.tile_pixels;
    const auto colors = job.colors;
    const auto output = job.output;
    const auto options = job.options;
    const int scale = int(options.scale), width = int(options.width());
    const int left_edge = 46 - int(options.border), right_edge = 366 + int(options.border);
    constexpr unsigned max_width = (320 + GameVideoOptions::max_border * 2) * GameVideoOptions::max_scale;
    std::array<PixelMix, max_width> pixels;
    for (unsigned y = begin; y < end; ++y) {
        const auto &row = lines.row(y);
        const auto layer = [&row](unsigned index) -> const SceneLayer & {
            return index < 4 ? row.playfields[index].layer : index < 8 ? row.sprites[index - 4] : row.text;
        };
        std::array<unsigned, 9> order{8, 4, 0, 7, 3, 6, 2, 5, 1};
        for (unsigned i = 1; i < order.size(); ++i) {
            const unsigned item = order[i];
            unsigned j = i;
            while (j && layer(order[j - 1]).priority < layer(item).priority) {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = item;
        }
        std::array<ClipRanges, 9> clips;
        for (unsigned index : order)
            if (layer(index).enabled) clips[index] = clip_ranges(row, layer(index), int16_t(left_edge), int16_t(right_edge));
        for (int sub_y = 0; sub_y < scale; ++sub_y) {
            const unsigned output_y = (y - 24) * scale + sub_y;
            std::fill_n(pixels.begin(), width, PixelMix{0, row.background, 0, 8, 0, 0, 255});
            for (unsigned index : order) {
                const auto &state = layer(index);
                if (!state.enabled) continue;
                const auto &ranges = clips[index];
                for (unsigned r = 0; r < ranges.count; ++r) {
                    const int left = (std::max<int>(left_edge, ranges.ranges[r].left) - left_edge) * scale;
                    const int right = (std::min<int>(right_edge, ranges.ranges[r].right) - left_edge) * scale;
                    for (int output_x = left; output_x < right; ++output_x) {
                        auto &pixel = pixels[output_x];
                        if (state.blend_mode == pixel.source_mode) continue;
                        int sample_x = output_x + left_edge * scale;
                        if (state.mosaic && row.mosaic_period > 1) {
                            const int hardware_x = left_edge + output_x / scale;
                            int count = hardware_x + 68;
                            count = (count % 432 + 432) % 432;
                            sample_x = (hardware_x - count % row.mosaic_period) * scale;
                        }
                        ScenePixel source;
                        bool select = state.blend_select;
                        if (index < 4) {
                            const auto &pf = row.playfields[index];
                            // Divide only after combining the native phase and output
                            // subpixel. This samples geometry, not an enlarged RGB frame.
                            const int x = floor_divide(pf.source_x * scale + (sample_x - 46 * scale) * pf.x_step, scale * 256);
                            const int fy = (int(pf.y_fraction) * scale + sub_y * pf.y_step) / scale;
                            source = tiles.playfield_pixel(index, x, pf.source_y + (fy >> 8), flipped, tile_pixels);
                            select = (source.flags & 1) != 0;
                            if (!(source.flags & 0x10) || !source.palette) continue;
                            source.palette = uint16_t(source.palette + pf.palette_add);
                        } else if (index < 8) {
                            if (options.expanded()) {
                                const int source_x = sample_x - left_edge * scale;
                                if (source_x < 0 || source_x >= width) continue;
                                source.palette = sprites[output_y * width + source_x];
                            } else {
                                if (sample_x < 0 || sample_x >= 432) continue;
                                source.palette = sprites[y * 432 + sample_x];
                            }
                            if (!source.palette || ((source.palette >> 10) & 3) != index - 4) continue;
                        } else {
                            const int x = floor_divide(row.text_x * scale + sample_x - 46 * scale, scale);
                            source = text.pixel(x, row.text_y, flipped);
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
        compose_rows(job, 24 + 232 * part / participants,
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

void compose_game_scene(const GameTiles &tiles, const GameText &text, const GameLines &lines,
                        std::span<const uint16_t> sprites, bool flipped,
                        std::span<const uint8_t> tile_pixels, std::span<const uint32_t> colors,
                        std::span<uint32_t> output, GameVideoOptions options) {
    const SceneJob job{tiles, text, lines, sprites, flipped, tile_pixels, colors, output, options};
    validate_scene(job);
    if (!options.expanded()) {
        // Dispatch is not worthwhile for the strict-native 320x232 frame.
        compose_rows(job, 24, 256);
    } else {
        static RowWorkers workers;
        workers.compose(job);
    }
}

void compose_game_scene_serial(const GameTiles &tiles, const GameText &text, const GameLines &lines,
                               std::span<const uint16_t> sprites, bool flipped,
                               std::span<const uint8_t> tile_pixels, std::span<const uint32_t> colors,
                               std::span<uint32_t> output, GameVideoOptions options) {
    const SceneJob job{tiles, text, lines, sprites, flipped, tile_pixels, colors, output, options};
    validate_scene(job);
    compose_rows(job, 24, 256);
}
} // namespace f3rt
