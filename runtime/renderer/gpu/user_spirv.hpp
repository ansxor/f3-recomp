#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace f3rt {
inline constexpr size_t MAX_USER_SHADER_BYTES = size_t(1024) * 1024;

namespace spirv {
inline constexpr uint32_t MAGIC = 0x07230203u;
inline constexpr uint32_t VERSION_1_0 = 0x00010000u;
inline constexpr uint32_t ENTRY_MAIN_WORD = 0x6e69616du;

inline constexpr uint32_t OP_MEMORY_MODEL = 14u;
inline constexpr uint32_t OP_ENTRY_POINT = 15u;
inline constexpr uint32_t OP_EXECUTION_MODE = 16u;
inline constexpr uint32_t OP_CAPABILITY = 17u;
inline constexpr uint32_t OP_TYPE_VOID = 19u;
inline constexpr uint32_t OP_TYPE_FLOAT = 22u;
inline constexpr uint32_t OP_TYPE_VECTOR = 23u;
inline constexpr uint32_t OP_TYPE_IMAGE = 25u;
inline constexpr uint32_t OP_TYPE_SAMPLED_IMAGE = 27u;
inline constexpr uint32_t OP_TYPE_STRUCT = 30u;
inline constexpr uint32_t OP_TYPE_POINTER = 32u;
inline constexpr uint32_t OP_TYPE_FUNCTION = 33u;
inline constexpr uint32_t OP_FUNCTION = 54u;
inline constexpr uint32_t OP_FUNCTION_END = 56u;
inline constexpr uint32_t OP_VARIABLE = 59u;
inline constexpr uint32_t OP_DECORATE = 71u;
inline constexpr uint32_t OP_MEMBER_DECORATE = 72u;
inline constexpr uint32_t OP_LABEL = 248u;
inline constexpr uint32_t OP_RETURN = 253u;

inline constexpr uint32_t CAPABILITY_SHADER = 1u;
inline constexpr uint32_t ADDRESSING_LOGICAL = 0u;
inline constexpr uint32_t MEMORY_MODEL_GLSL450 = 1u;
inline constexpr uint32_t EXEC_MODEL_VERTEX = 0u;
inline constexpr uint32_t EXEC_MODEL_FRAGMENT = 4u;
inline constexpr uint32_t EXEC_MODE_ORIGIN_UPPER_LEFT = 7u;

inline constexpr uint32_t DECORATION_BLOCK = 2u;
inline constexpr uint32_t DECORATION_BUILTIN = 11u;
inline constexpr uint32_t DECORATION_LOCATION = 30u;
inline constexpr uint32_t DECORATION_BINDING = 33u;
inline constexpr uint32_t DECORATION_DESCRIPTOR_SET = 34u;
inline constexpr uint32_t DECORATION_OFFSET = 35u;

inline constexpr uint32_t STORAGE_UNIFORM_CONSTANT = 0u;
inline constexpr uint32_t STORAGE_INPUT = 1u;
inline constexpr uint32_t STORAGE_UNIFORM = 2u;
inline constexpr uint32_t STORAGE_OUTPUT = 3u;
inline constexpr uint32_t STORAGE_PRIVATE = 6u;
inline constexpr uint32_t STORAGE_FUNCTION = 7u;

inline constexpr uint32_t DIM_2D = 1u;
inline constexpr uint32_t DIM_3D = 2u;
inline constexpr uint32_t IMAGE_SAMPLED_YES = 1u;
} // namespace spirv

// A user postprocess shader read from disk. `code` is 4-byte aligned storage: SPIR-V words for
// Vulkan, or the MSL text plus one trailing zero byte for Metal. `size` is the payload in bytes.
struct LoadedUserShader {
    std::vector<uint32_t> code;
    size_t size = 0;
};

// Validates the fixed descriptor/interface ABI before user SPIR-V reaches the driver. Pipeline
// creation performs the remaining shader validation. Throws std::runtime_error.
void validate_user_spirv(std::span<const uint32_t> words);

// Reads and checks a user shader file: extension (.metal / .spv), size (1 byte..1 MiB), no
// embedded NUL for Metal, 4-byte multiple plus validate_user_spirv for Vulkan.
LoadedUserShader load_user_shader(const std::filesystem::path &path, bool metal);
}
