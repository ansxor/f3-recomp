#include "renderer/gpu/user_spirv.hpp"
#include "f3rt_video_cxx/video.h"
#include "rust/cxx.h"
#include <stdexcept>

namespace f3rt {
void validate_user_spirv(std::span<const uint32_t> words) {
    try {
        rust_video::validate_user_spirv({words.data(), words.size()});
    } catch (const rust::Error &error) {
        throw std::runtime_error(error.what());
    }
}

LoadedUserShader load_user_shader(const std::filesystem::path &path, bool metal) {
    try {
        const auto shader = rust_video::load_user_shader(rust::Str(path.string()), metal);
        return {{shader.code.begin(), shader.code.end()}, shader.size};
    } catch (const rust::Error &error) {
        throw std::runtime_error(error.what());
    }
}
}
