#include "renderer/gpu/encode.hpp"
#include <stdexcept>

namespace f3rt {
namespace {

rust_video::GameVideoOptions rust_options(GameVideoOptions options) {
    return {options.scale, options.border};
}

rust_video::SpriteList rust_list(SpriteList list) {
    return list == SpriteList::Canonical ? rust_video::SpriteList::Canonical
                                         : rust_video::SpriteList::Presented;
}

} // namespace

GpuPrepScratch::GpuPrepScratch()
    : m_scratch(rust_video::new_prep_scratch()), motion_state(m_scratch->motion_state()),
      interp_coefficients(m_scratch->interp_coefficients()) {}

GpuPrepScratch::~GpuPrepScratch() = default;

EncodedScene::EncodedScene(const EncodedScene &other)
    : uniforms(other.uniforms), upload_bytes(other.upload_bytes), fallback(other.fallback),
      use_interpolation(other.use_interpolation), has_trails(other.has_trails),
      blend_weighted(other.blend_weighted), trail_storage(other.trail_storage),
      trail_seed(trail_storage.data(), trail_storage.size()),
      motion_stats(other.motion_stats), interp_stats(other.interp_stats) {}

EncodedScene &EncodedScene::operator=(const EncodedScene &other) {
    if (this == &other) return *this;
    uniforms = other.uniforms;
    upload_bytes = other.upload_bytes;
    fallback = other.fallback;
    use_interpolation = other.use_interpolation;
    has_trails = other.has_trails;
    blend_weighted = other.blend_weighted;
    trail_storage = other.trail_storage;
    trail_seed = {trail_storage.data(), trail_storage.size()};
    motion_stats = other.motion_stats;
    interp_stats = other.interp_stats;
    return *this;
}

EncodedScene::EncodedScene(EncodedScene &&other) noexcept
    : uniforms(other.uniforms), upload_bytes(other.upload_bytes), fallback(other.fallback),
      use_interpolation(other.use_interpolation), has_trails(other.has_trails),
      blend_weighted(other.blend_weighted), trail_storage(std::move(other.trail_storage)),
      trail_seed(trail_storage.data(), trail_storage.size()),
      motion_stats(other.motion_stats), interp_stats(other.interp_stats) {}

EncodedScene &EncodedScene::operator=(EncodedScene &&other) noexcept {
    uniforms = other.uniforms;
    upload_bytes = other.upload_bytes;
    fallback = other.fallback;
    use_interpolation = other.use_interpolation;
    has_trails = other.has_trails;
    blend_weighted = other.blend_weighted;
    trail_storage = std::move(other.trail_storage);
    trail_seed = {trail_storage.data(), trail_storage.size()};
    motion_stats = other.motion_stats;
    interp_stats = other.interp_stats;
    return *this;
}

MotionScene ScenePlan::apply_motion(const GpuMotionHistory *history, std::optional<float> alpha,
                                    MotionState &scratch) const noexcept {
    MotionScene result{*this, nullptr, {}};
    const auto opts = rust_options(options);
    if (history && alpha) {
        result.motion_stats = rust_video::apply_motion_history(
            scene.inner(), opts, rust_list(sprite_list), layer_mask, history->inner(), *alpha,
            scratch);
    } else {
        result.motion_stats = rust_video::apply_motion(scene.inner(), opts, rust_list(sprite_list),
                                                       layer_mask, scratch);
    }
    if (result.motion_stats.paired) result.motion = &scratch;
    return result;
}

PreparedScene MotionScene::analyze_interpolation(
    InterpolationConfig config, std::span<const uint64_t> tile_pen_masks,
    InterpolationCoefficients &scratch) const noexcept {
    const auto masks = rust::Slice<const uint64_t>(tile_pen_masks.data(), tile_pen_masks.size());
    const auto opts = rust_options(plan.options);
    rust_video::InterpolationResult result{};
    if (motion) {
        result = rust_video::analyze_interpolation_motion(
            plan.scene.inner(), opts, rust_list(plan.sprite_list), plan.layer_mask, *motion,
            config.mode, config.fields, masks, scratch);
    } else {
        result = rust_video::analyze_interpolation(plan.scene.inner(), opts,
                                                   rust_list(plan.sprite_list), plan.layer_mask,
                                                   config.mode, config.fields, masks, scratch);
    }
    PreparedScene prepared{*this,
                           result.use_interpolation ? &scratch : nullptr,
                           result.stats,
                           result.use_interpolation};
    return prepared;
}

size_t PreparedScene::required_words() const noexcept {
    return rust_video::required_scene_words(motion.plan.scene.inner(), rust_options(motion.plan.options),
                                            use_interpolation);
}

uint32_t PreparedScene::upload_bytes() const noexcept {
    return uint32_t(required_words() * sizeof(uint32_t));
}

EncodedScene PreparedScene::encode_into(std::span<uint32_t> out, uint32_t pf_tile_count,
                                        uint32_t sp_tile_count) const {
    const auto opts = rust_options(motion.plan.options);
    const auto list = rust_list(motion.plan.sprite_list);
    const auto words = rust::Slice<uint32_t>(out.data(), out.size());
    const auto &scene = motion.plan.scene;
    rust::Vec<uint16_t> trail;
    const size_t trail_capacity = scene.trail_len();
    if (trail_capacity) {
        trail.reserve(trail_capacity);
        for (size_t i = 0; i < trail_capacity; ++i) trail.push_back(0);
    }
    auto trail_slice = rust::Slice<uint16_t>(trail.data(), trail.size());
    rust_video::EncodedSceneMeta meta{};
    try {
        if (motion.motion && coefficients) {
            meta = rust_video::encode_prepared_motion_coefficients(
                scene.inner(), opts, list, motion.plan.layer_mask, motion.motion_stats,
                interp_stats, use_interpolation, *motion.motion, *coefficients, words, trail_slice,
                pf_tile_count, sp_tile_count);
        } else if (motion.motion) {
            meta = rust_video::encode_prepared_motion(
                scene.inner(), opts, list, motion.plan.layer_mask, motion.motion_stats,
                interp_stats, use_interpolation, *motion.motion, words, trail_slice, pf_tile_count,
                sp_tile_count);
        } else if (coefficients) {
            meta = rust_video::encode_prepared_coefficients(
                scene.inner(), opts, list, motion.plan.layer_mask, motion.motion_stats,
                interp_stats, use_interpolation, *coefficients, words, trail_slice, pf_tile_count,
                sp_tile_count);
        } else {
            meta = rust_video::encode_prepared(
                scene.inner(), opts, list, motion.plan.layer_mask, motion.motion_stats,
                interp_stats, use_interpolation, words, trail_slice, pf_tile_count,
                sp_tile_count);
        }
    } catch (const rust::Error &error) {
        throw std::invalid_argument(error.what());
    }
    if (meta.trail_seed_len <= trail_capacity) trail.truncate(meta.trail_seed_len);

    EncodedScene encoded;
    encoded.uniforms = meta.uniforms;
    encoded.upload_bytes = meta.upload_bytes;
    encoded.fallback = meta.fallback;
    encoded.use_interpolation = meta.use_interpolation;
    encoded.has_trails = meta.has_trails;
    encoded.blend_weighted = meta.blend_weighted;
    encoded.trail_storage = std::move(trail);
    encoded.trail_seed = {encoded.trail_storage.data(), encoded.trail_storage.size()};
    encoded.motion_stats = meta.motion_stats;
    encoded.interp_stats = meta.interp_stats;
    return encoded;
}

} // namespace f3rt
