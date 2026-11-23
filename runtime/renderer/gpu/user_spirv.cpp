#include "renderer/gpu/user_spirv.hpp"
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace f3rt {
void validate_user_spirv(std::span<const uint32_t> words) {
    using namespace spirv;
    auto invalid = [] {
        throw std::runtime_error("User SPIR-V: invalid module or postprocess ABI");
    };
    if (words.size() < 5 || words[0] != MAGIC || words[4] != 0 || words[3] == 0 ||
        words[3] > MAX_USER_SHADER_BYTES / 4)
        invalid();
    struct Id {
        uint32_t opcode = 0;
        uint32_t first_operand = 0;
        uint32_t second_operand = 0;
        uint32_t word_count = 0;
        uint32_t set = UINT32_MAX, binding = UINT32_MAX;
        bool builtin = false, block = false, zero_offset = false;
    };
    std::vector<Id> ids(words[3]);
    auto id = [&](uint32_t n) -> Id & {
        if (!n || n >= ids.size()) invalid();
        return ids[n];
    };
    bool entry = false;
    for (size_t pos = 5; pos < words.size();) {
        const auto *p = words.data() + pos;
        const uint32_t count = p[0] >> 16, op = p[0] & 0xffff;
        if (!count || count > words.size() - pos) invalid();
        auto need = [&](uint32_t n) {
            if (count < n) invalid();
        };
        if (op == OP_ENTRY_POINT) {
            need(5);
            if (entry || p[1] != EXEC_MODEL_FRAGMENT || p[3] != ENTRY_MAIN_WORD || p[4] != 0)
                invalid();
            id(p[2]);
            entry = true;
        } else if (op == OP_DECORATE) {
            need(3);
            auto &v = id(p[1]);
            if (p[2] == DECORATION_BINDING || p[2] == DECORATION_DESCRIPTOR_SET) {
                need(4);
                (p[2] == DECORATION_BINDING ? v.binding : v.set) = p[3];
            } else if (p[2] == DECORATION_BUILTIN) v.builtin = true;
            else if (p[2] == DECORATION_BLOCK) v.block = true;
        } else if (op == OP_MEMBER_DECORATE) {
            need(4);
            if (p[3] == DECORATION_OFFSET) {
                need(5);
                if (p[2] == 0 && p[4] == 0) id(p[1]).zero_offset = true;
            }
        } else if (op == OP_TYPE_FLOAT || op == OP_TYPE_VECTOR || op == OP_TYPE_IMAGE ||
                   op == OP_TYPE_SAMPLED_IMAGE || op == OP_TYPE_STRUCT || op == OP_TYPE_POINTER ||
                   op == OP_VARIABLE) {
            const bool unary_type =
                op == OP_TYPE_FLOAT || op == OP_TYPE_SAMPLED_IMAGE || op == OP_TYPE_STRUCT;
            need(unary_type ? 3 : 4);
            auto &v = id(op == OP_VARIABLE ? p[2] : p[1]);
            v.opcode = op;
            v.first_operand = op == OP_VARIABLE ? p[1] : p[2];
            v.second_operand = (count > 3) ? p[3] : 0;
            v.word_count = count;
            if (op == OP_TYPE_IMAGE) {
                need(9);
                const bool non_arrayed_2d_sampled =
                    p[3] == DIM_2D && p[4] == 0 && p[5] == 0 && p[6] == 0 &&
                    p[7] == IMAGE_SAMPLED_YES;
                if (!non_arrayed_2d_sampled) invalid();
            }
        }
        pos += count;
    }
    if (!entry) invalid();
    unsigned samplers = 0, uniforms = 0;
    for (const auto &v : ids) {
        if (v.opcode != OP_VARIABLE) continue;
        const auto &pointer = id(v.first_operand);
        if (pointer.opcode != OP_TYPE_POINTER || pointer.first_operand != v.second_operand)
            invalid();
        const auto &type = id(pointer.second_operand);
        if (v.second_operand == STORAGE_UNIFORM_CONSTANT) {
            if (v.set != 2 || v.binding != 0 || type.opcode != OP_TYPE_SAMPLED_IMAGE) invalid();
            const auto &image = id(type.first_operand);
            const auto &sampled_type = id(image.first_operand);
            if (image.opcode != OP_TYPE_IMAGE || sampled_type.opcode != OP_TYPE_FLOAT ||
                sampled_type.first_operand != 32)
                invalid();
            ++samplers;
        } else if (v.second_operand == STORAGE_UNIFORM) {
            if (v.set != 3 || v.binding != 0 || type.opcode != OP_TYPE_STRUCT ||
                type.word_count != 3 || !type.block || !type.zero_offset)
                invalid();
            const auto &vector = id(type.first_operand);
            const auto &component = id(vector.first_operand);
            if (vector.opcode != OP_TYPE_VECTOR || vector.second_operand != 4 ||
                component.opcode != OP_TYPE_FLOAT || component.first_operand != 32)
                invalid();
            ++uniforms;
        } else if (v.second_operand == STORAGE_INPUT) {
            if (!v.builtin) invalid();
        } else if (v.second_operand != STORAGE_OUTPUT && v.second_operand != STORAGE_PRIVATE &&
                   v.second_operand != STORAGE_FUNCTION) {
            invalid();
        }
    }
    if (samplers != 1 || uniforms != 1) invalid();
}

LoadedUserShader load_user_shader(const std::filesystem::path &path, bool metal) {
    if (path.extension() != (metal ? ".metal" : ".spv"))
        throw std::runtime_error(metal ? "Metal user shader requires a .metal file"
                                       : "Vulkan user shader requires a .spv file");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open user shader: " + path.string());
    const auto end = file.tellg();
    if (end <= 0 || end > std::streamoff(MAX_USER_SHADER_BYTES))
        throw std::runtime_error("User shader must be 1 byte to 1 MiB");
    LoadedUserShader shader;
    shader.size = size_t(end);
    shader.code.assign((shader.size + 4) / 4, 0);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(shader.code.data()), std::streamsize(shader.size)))
        throw std::runtime_error("Cannot read user shader: " + path.string());
    if (metal) {
        if (std::memchr(shader.code.data(), 0, shader.size))
            throw std::runtime_error("User Metal shader contains an embedded NUL");
        return shader;
    }
    if (shader.size % 4) throw std::runtime_error("User SPIR-V size must be a multiple of four");
    shader.code.resize(shader.size / 4);
    validate_user_spirv(shader.code);
    return shader;
}
}
