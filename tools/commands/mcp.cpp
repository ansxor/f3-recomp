// f3rt-tool mcp: a Model Context Protocol server (stdio transport, Neumann-Labs/mcp-cpp) over one
// process-local Machine. Tools: init shutdown reset step set_input regs read write crc audio_stats
// save_state load_state screenshot sprites video_diff.
//
// Stdout carries MCP frames only: fd 1 is duplicated for the transport and then pointed at stderr, so
// stray prints from the runtime can never corrupt the framing.
#include "commands.hpp"
#include "../runner/runner.hpp"
#include "capture_io.hpp"
#include "f3rt/abi/audio.h"
#include "f3rt/game_video.hpp"
#include "f3rt/video.hpp"

#include <mcp/mcp.hpp>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace f3rt::tool {

namespace {

using json = nlohmann::json;

// ---- encodings -------------------------------------------------------------------------------------

constexpr char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::span<const uint8_t> in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const uint32_t v = uint32_t(in[i]) << 16 | uint32_t(in[i + 1]) << 8 | in[i + 2];
        out += B64[v >> 18];
        out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63];
        out += B64[v & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t v = uint32_t(in[i]) << 16;
        out += B64[v >> 18];
        out += B64[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t v = uint32_t(in[i]) << 16 | uint32_t(in[i + 1]) << 8;
        out += B64[v >> 18];
        out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::vector<uint8_t> base64_decode(std::string_view in) {
    std::array<int8_t, 256> table{};
    table.fill(-1);
    for (int i = 0; i < 64; ++i) table[uint8_t(B64[i])] = int8_t(i);
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = table[uint8_t(c)];
        if (v < 0) throw std::runtime_error("invalid base64 character");
        acc = acc << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
        }
    }
    return out;
}

std::string hex_encode(std::span<const uint8_t> in) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(in.size() * 2);
    for (const uint8_t b : in) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

std::vector<uint8_t> hex_decode(std::string_view in) {
    std::string clean;
    for (const char c : in)
        if (c != ' ' && c != '\n' && c != '\t' && c != '\r' && c != '_') clean += c;
    if (clean.size() >= 2 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X'))
        clean.erase(0, 2);
    if (clean.size() % 2) throw std::runtime_error("hex string has an odd number of digits");
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out(clean.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const int hi = nibble(clean[2 * i]), lo = nibble(clean[2 * i + 1]);
        if (hi < 0 || lo < 0) throw std::runtime_error("invalid hex digit");
        out[i] = uint8_t(hi << 4 | lo);
    }
    return out;
}

std::string hex_number(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string crc_text(uint32_t crc) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", crc);
    return buf;
}

// 8-bit RGB PNG of ARGB pixels; the frame is opaque.
std::vector<uint8_t> encode_png(std::span<const uint32_t> pixels, uint32_t width, uint32_t height) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (1 + size_t(width) * 3));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); // filter: none
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t p = pixels[size_t(y) * width + x];
            raw.push_back(uint8_t(p >> 16));
            raw.push_back(uint8_t(p >> 8));
            raw.push_back(uint8_t(p));
        }
    }
    uLongf packed_size = compressBound(uLong(raw.size()));
    std::vector<uint8_t> packed(packed_size);
    if (compress2(packed.data(), &packed_size, raw.data(), uLong(raw.size()), 6) != Z_OK)
        throw std::runtime_error("PNG compression failed");
    packed.resize(packed_size);

    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    auto be32 = [](std::vector<uint8_t> &v, uint32_t x) {
        for (int shift = 24; shift >= 0; shift -= 8) v.push_back(uint8_t(x >> shift));
    };
    auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        be32(png, uint32_t(data.size()));
        std::vector<uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        png.insert(png.end(), body.begin(), body.end());
        be32(png, crc32(body.data(), body.size()));
    };
    std::vector<uint8_t> ihdr;
    be32(ihdr, width);
    be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB, deflate, no filter bank, no interlace
    chunk("IHDR", ihdr);
    chunk("IDAT", packed);
    chunk("IEND", {});
    return png;
}

// ---- arguments -------------------------------------------------------------------------------------

// Typed access to a tool's `arguments`; every getter throws std::runtime_error naming the key.
class Args {
public:
    explicit Args(const json &j) : m_obj(j.is_object() ? &j : nullptr) {
        if (!j.is_null() && !j.is_object()) throw std::runtime_error("arguments must be an object");
    }

    const json *find(std::string_view key) const {
        if (!m_obj) return nullptr;
        const auto it = m_obj->find(std::string(key));
        if (it == m_obj->end() || it->is_null()) return nullptr;
        return &*it;
    }

    bool has(std::string_view key) const { return find(key) != nullptr; }

    // Integers, or hex/decimal strings such as "0x400000" and "$400000".
    std::optional<uint64_t> u64(std::string_view key) const {
        const json *v = find(key);
        if (!v) return std::nullopt;
        if (v->is_number_unsigned()) return v->get<uint64_t>();
        if (v->is_number_integer()) {
            const int64_t n = v->get<int64_t>();
            if (n < 0) throw std::runtime_error("'" + std::string(key) + "' must not be negative");
            return uint64_t(n);
        }
        if (v->is_string()) return parse_number(v->get<std::string>(), key);
        throw std::runtime_error("'" + std::string(key) + "' must be an integer or a hex string");
    }

    uint64_t u64_or(std::string_view key, uint64_t fallback) const { return u64(key).value_or(fallback); }

    uint64_t require_u64(std::string_view key) const {
        const auto v = u64(key);
        if (!v) throw std::runtime_error("missing '" + std::string(key) + "'");
        return *v;
    }

    std::optional<bool> boolean(std::string_view key) const {
        const json *v = find(key);
        if (!v) return std::nullopt;
        if (v->is_boolean()) return v->get<bool>();
        if (v->is_number_unsigned() && v->get<uint64_t>() <= 1) return v->get<uint64_t>() == 1;
        throw std::runtime_error("'" + std::string(key) + "' must be a boolean");
    }

    std::optional<std::string> str(std::string_view key) const {
        const json *v = find(key);
        if (!v) return std::nullopt;
        if (!v->is_string()) throw std::runtime_error("'" + std::string(key) + "' must be a string");
        return v->get<std::string>();
    }

    std::string require_str(std::string_view key) const {
        const auto v = str(key);
        if (!v) throw std::runtime_error("missing '" + std::string(key) + "'");
        return *v;
    }

    std::optional<std::vector<uint64_t>> u64_array(std::string_view key) const {
        const json *v = find(key);
        if (!v) return std::nullopt;
        if (!v->is_array()) throw std::runtime_error("'" + std::string(key) + "' must be an array");
        std::vector<uint64_t> items;
        for (const json &item : *v) {
            if (item.is_number_unsigned()) items.push_back(item.get<uint64_t>());
            else if (item.is_string()) items.push_back(parse_number(item.get<std::string>(), key));
            else throw std::runtime_error("'" + std::string(key) + "' must hold unsigned integers");
        }
        return items;
    }

    // Rejects keys the tool's schema does not declare, so a typo is an error instead of a silent default.
    void reject_unknown(const json &schema) const {
        if (!m_obj) return;
        const json &props = schema.at("properties");
        for (const auto &[key, value] : m_obj->items())
            if (!props.contains(key)) throw std::runtime_error("unknown argument '" + key + "'");
    }

private:
    static uint64_t parse_number(std::string s, std::string_view key) {
        if (!s.empty() && s[0] == '$') s = "0x" + s.substr(1);
        char *end = nullptr;
        errno = 0;
        const unsigned long long v = std::strtoull(s.c_str(), &end, 0);
        if (s.empty() || *end != '\0' || errno != 0)
            throw std::runtime_error("'" + std::string(key) + "' is not a number: " + s);
        return v;
    }

    const json *m_obj;
};

// ---- the machine ---------------------------------------------------------------------------------

struct Defaults {
    std::filesystem::path rom_dir;
    std::string set = F3RT_DEFAULT_SET;
    bool game_video = true;
    bool sprite_writers = false;
    std::filesystem::path eeprom;
};

struct Region {
    std::string_view name;
    size_t size;
    uint32_t bus_base; // 0 = read only
};

constexpr std::array<Region, 6> REGIONS{{
    {"ram", RAM_SIZE, RAM_BASE},
    {"graphics", GRAPHICS_RAM_SIZE, GRAPHICS_BASE},
    {"palette", PALETTE_RAM_SIZE, PALETTE_BASE},
    {"control", 0x20, 0x660000},
    {"shared", 0x800, 0xc00000},
    {"sprite_writers", 0x1000 * 4, 0},
}};

constexpr size_t MAX_CAPTURED_PCM_BYTES = 6u << 20;

// One process-local machine. The audio board and the EEPROM are process-wide leases, so the old
// machine is always destroyed before a new one is built.
class Runtime {
public:
    explicit Runtime(Defaults defaults) : m_defaults(std::move(defaults)) {}

    // Destroys the machine before the oracle its scanout hook may borrow.
    ~Runtime() { destroy(); }

    bool can_init_from_defaults() const { return !m_defaults.rom_dir.empty(); }
    json init_from_defaults() { return init(Args(json::object())); }

    json init(const Args &p) {
        const std::filesystem::path rom_dir = p.str("rom_dir").value_or(m_defaults.rom_dir.string());
        const std::string set = p.str("set").value_or(m_defaults.set);
        const bool game_video = p.boolean("game_video").value_or(m_defaults.game_video);
        const bool sprite_writers = p.boolean("sprite_writers").value_or(m_defaults.sprite_writers);
        const bool video_diff = p.boolean("video_diff").value_or(false);
        const std::filesystem::path eeprom = p.str("eeprom").value_or(m_defaults.eeprom.string());
        if (rom_dir.empty())
            throw std::runtime_error(
                "no ROM directory: pass rom_dir, --rom-dir, or build with F3RT_DEFAULT_ROM_DIR");

        destroy();
        auto machine = runner::create_strict_machine(rom_dir, set, game_video && !video_diff);
        if (sprite_writers)
            machine->sprite_writers = std::make_unique<std::array<uint32_t, 0x1000>>();
        if (video_diff) {
            machine->game_video = std::make_unique<GameVideo>(*machine, GameVideoMode::Diagnostic);
        } else if (!game_video) {
            m_oracle = std::make_unique<Video>(machine->graphics_assets, machine->roms.video);
            Machine *raw = machine.get();
            Video *oracle = m_oracle.get();
            machine->inspect_scanout = [raw, oracle](std::span<uint32_t> pixels) {
                oracle->render_frame(raw->palette, raw->graphics, raw->control, pixels);
            };
        }
        if (!eeprom.empty()) machine->load_eeprom(eeprom);
        m_machine = std::move(machine);
        m_set = set;
        m_sprite_writers = sprite_writers;
        m_video_diff = video_diff;

        Machine &m = *m_machine;
        return {{"set", m_set},
                {"frame", m.frame},
                {"state_size", m.state_size()},
                {"sample_rate", f3rt_audio_sample_rate()},
                {"width", unsigned(geometry::NATIVE_WIDTH)},
                {"height", unsigned(geometry::HEIGHT)},
                {"game_video", game_video || video_diff},
                {"video_diff", video_diff},
                {"sprite_writers", sprite_writers}};
    }

    json shutdown() {
        const bool was_running = m_machine != nullptr;
        destroy();
        return {{"shutdown", true}, {"was_initialized", was_running}};
    }

    json reset() {
        Machine &m = machine();
        m.reset();
        return {{"frame", m.frame}, {"pc", m.cpu.pc}, {"state_crc", crc_text(m.state_crc())}};
    }

    json step(const Args &p) {
        Machine &m = machine();
        const uint64_t frames = p.u64_or("frames", 1);
        const std::string audio = p.str("audio").value_or("discard");
        const bool diff = p.boolean("video_diff").value_or(false);
        const unsigned mask = unsigned(p.u64_or("video_layer_mask", 511));
        const bool stop_on_halt = p.boolean("stop_on_halt").value_or(true);
        const bool want_crc = p.boolean("crc").value_or(true);
        std::optional<std::string> wav_path;
        std::unique_ptr<WavWriter> wav;
        if (audio == "wav") {
            wav_path = p.require_str("wav_path");
            const std::filesystem::path path(*wav_path);
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
            wav = std::make_unique<WavWriter>(path, f3rt_audio_sample_rate());
        } else if (audio != "discard" && audio != "capture") {
            throw std::runtime_error("audio must be \"discard\", \"capture\" or \"wav\"");
        }
        if (diff && !m_video_diff) throw std::runtime_error("video_diff needs init with video_diff:true");

        std::vector<int16_t> captured;
        std::array<int16_t, 8192> block{};
        uint64_t audio_frames = 0, nonzero = 0;
        int peak = 0;
        auto drain = [&] {
            while (const size_t count = f3rt_audio_render(block.data(), block.size() / 2)) {
                audio_frames += count;
                const std::span<const int16_t> samples(block.data(), count * 2);
                for (const int16_t s : samples) {
                    peak = std::max(peak, std::abs(int(s)));
                    nonzero += s != 0;
                }
                if (wav) wav->append(samples);
                if (audio == "capture") {
                    captured.insert(captured.end(), samples.begin(), samples.end());
                    if (captured.size() * 2 > MAX_CAPTURED_PCM_BYTES)
                        throw std::runtime_error(
                            "captured audio exceeds 6 MiB; use audio:\"wav\" with wav_path for long runs");
                }
            }
        };

        uint64_t run = 0;
        bool stopped = false;
        std::optional<std::string> video_error;
        while (run < frames) {
            const bool ok = m.run_frame();
            ++run;
            drain();
            if (diff) {
                try {
                    m.game_video->compare_layers(m.frame, mask);
                } catch (const std::exception &e) {
                    video_error = e.what();
                    stopped = true;
                    break;
                }
            }
            if ((!ok || m.cpu.halted) && stop_on_halt) {
                stopped = true;
                break;
            }
        }
        wav.reset();

        json r{{"frames_run", run},
               {"frame", m.frame},
               {"pc", m.cpu.pc},
               {"steps", m.cpu.steps},
               {"native_blocks", m.native_blocks},
               {"halted", bool(m.cpu.halted)},
               {"cpu_stopped", bool(m.cpu.stopped)},
               {"stopped", stopped},
               {"audio_frames", audio_frames},
               {"audio_peak", peak},
               {"nonzero_samples", nonzero}};
        if (want_crc) {
            r["frame_crc"] = crc_text(frame_crc(m));
            r["state_crc"] = crc_text(m.state_crc());
        }
        if (wav_path) r["wav_path"] = *wav_path;
        if (audio == "capture") {
            std::vector<uint8_t> bytes(captured.size() * 2);
            for (size_t i = 0; i < captured.size(); ++i) {
                bytes[2 * i] = uint8_t(uint16_t(captured[i]));
                bytes[2 * i + 1] = uint8_t(uint16_t(captured[i]) >> 8);
            }
            r["audio_pcm_base64"] = base64_encode(bytes);
        }
        if (diff) {
            r["video_diff_ok"] = !video_error;
            if (video_error) r["video_error"] = *video_error;
        }
        return r;
    }

    json set_input(const Args &p) {
        Machine &m = machine();
        if (const auto all = p.u64_array("inputs")) {
            if (all->size() > m.inputs.size())
                throw std::runtime_error("inputs holds at most " + std::to_string(m.inputs.size()) + " ports");
            for (size_t i = 0; i < all->size(); ++i) m.inputs[i] = uint32_t((*all)[i]);
        }
        if (const auto sys = p.u64("system_inputs")) m.system_inputs = uint8_t(*sys);
        if (p.has("mask")) {
            const uint64_t mask = p.require_u64("mask");
            const uint64_t port = p.u64_or("port", 0);
            const bool pressed = p.boolean("pressed").value_or(true);
            if (port >= m.inputs.size()) throw std::runtime_error("port out of range");
            m.set_input(unsigned(port), uint32_t(mask), pressed);
        }
        json inputs = json::array();
        for (const uint32_t v : m.inputs) inputs.push_back(v);
        return {{"inputs", std::move(inputs)}, {"system_inputs", unsigned(m.system_inputs)}};
    }

    json regs() {
        Machine &m = machine();
        // Pending lazy condition codes are materialised on a copy: flushing the live CPU would change state_crc.
        f3_cpu c = m.cpu;
        f3_cc_flush(&c);
        json d = json::array(), a = json::array();
        for (int i = 0; i < 8; ++i) {
            d.push_back(c.d[i]);
            a.push_back(c.a[i]);
        }
        return {{"pc", c.pc},      {"sr", unsigned(c.sr)}, {"d", std::move(d)},  {"a", std::move(a)},
                {"usp", c.usp},    {"isp", c.ssp},         {"msp", c.msp},       {"vbr", c.vbr},
                {"sfc", c.sfc},    {"dfc", c.dfc},         {"cacr", c.cacr},     {"caar", c.caar},
                {"halted", bool(c.halted)}, {"stopped", bool(c.stopped)},        {"steps", c.steps},
                {"frame", m.frame}, {"native_blocks", m.native_blocks}};
    }

    json crc() {
        Machine &m = machine();
        return {{"frame", m.frame}, {"frame_crc", crc_text(frame_crc(m))}, {"state_crc", crc_text(m.state_crc())}};
    }

    json read(const Args &p) {
        Machine &m = machine();
        if (const auto region_name = p.str("region")) {
            const Region &region = region_named(*region_name);
            const uint64_t offset = p.u64_or("offset", 0);
            const uint64_t length = p.u64_or("length", region.size - std::min<uint64_t>(offset, region.size));
            if (offset > region.size || length > region.size - offset)
                throw std::runtime_error("range 0x" + hex_number(offset) + "+0x" + hex_number(length) +
                                         " is outside " + std::string(region.name) + " (0x" +
                                         hex_number(region.size) + " bytes)");
            const auto bytes = region_bytes(m, region);
            return {{"region", std::string(region.name)},
                    {"offset", offset},
                    {"length", length},
                    {"hex", hex_encode(std::span(bytes).subspan(offset, length))}};
        }
        const uint32_t addr = uint32_t(p.require_u64("addr"));
        if (p.has("length")) {
            // A byte run through the bus.
            const uint64_t length = p.require_u64("length");
            if (length > RAM_SIZE) throw std::runtime_error("length is too large");
            std::vector<uint8_t> bytes(length);
            for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = m.read8(addr + uint32_t(i));
            return {{"addr", addr}, {"length", length}, {"hex", hex_encode(bytes)}};
        }
        const unsigned size = access_size(p);
        const uint32_t value = size == 1 ? m.read8(addr) : size == 2 ? m.read16(addr) : m.read32(addr);
        return {{"addr", addr}, {"size", size}, {"value", value}};
    }

    json write(const Args &p) {
        Machine &m = machine();
        if (const auto region_name = p.str("region")) {
            const Region &region = region_named(*region_name);
            if (!region.bus_base) throw std::runtime_error(std::string(region.name) + " is read only");
            const uint64_t offset = p.u64_or("offset", 0);
            const auto bytes = hex_decode(p.require_str("hex"));
            if (offset > region.size || bytes.size() > region.size - offset)
                throw std::runtime_error("range is outside " + std::string(region.name));
            // Through the bus, so hardware side effects (shared RAM -> sound board, mirrors) apply.
            for (size_t i = 0; i < bytes.size(); ++i) m.write8(region.bus_base + uint32_t(offset + i), bytes[i]);
            return {{"region", std::string(region.name)}, {"offset", offset}, {"length", bytes.size()}};
        }
        const uint32_t addr = uint32_t(p.require_u64("addr"));
        if (p.has("hex")) {
            const auto bytes = hex_decode(p.require_str("hex"));
            for (size_t i = 0; i < bytes.size(); ++i) m.write8(addr + uint32_t(i), bytes[i]);
            return {{"addr", addr}, {"length", bytes.size()}};
        }
        const unsigned size = access_size(p);
        const uint64_t value = p.require_u64("value");
        if (size == 1) m.write8(addr, uint8_t(value));
        else if (size == 2) m.write16(addr, uint16_t(value));
        else m.write32(addr, uint32_t(value));
        return {{"addr", addr}, {"size", size}, {"value", value & (size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1)}};
    }

    json save_state(const Args &p) {
        Machine &m = machine();
        const auto slot = p.str("slot");
        const auto path = p.str("path");
        const bool inline_state = p.boolean("inline").value_or(false);
        if (!slot && !path && !inline_state)
            throw std::runtime_error("save_state needs a slot, a path, or inline:true");
        std::vector<uint8_t> blob(m.state_size());
        m.save_state(blob);
        json r{{"size", blob.size()}, {"state_crc", crc_text(crc32(blob.data(), blob.size()))}};
        if (path) {
            const std::filesystem::path file(*path);
            if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
            write_bytes(file, blob);
            r["path"] = *path;
        }
        if (inline_state) r["base64"] = base64_encode(blob);
        if (slot) {
            r["slot"] = *slot;
            m_slots[*slot] = std::move(blob);
        }
        return r;
    }

    json load_state(const Args &p) {
        Machine &m = machine();
        const auto slot = p.str("slot");
        const auto path = p.str("path");
        const auto b64 = p.str("base64");
        if (int(slot.has_value()) + int(path.has_value()) + int(b64.has_value()) != 1)
            throw std::runtime_error("load_state needs exactly one of slot, path, base64");
        std::vector<uint8_t> blob;
        if (slot) {
            const auto it = m_slots.find(*slot);
            if (it == m_slots.end()) throw std::runtime_error("no saved state in slot: " + *slot);
            blob = it->second;
        } else if (path) {
            std::ifstream f(*path, std::ios::binary | std::ios::ate);
            if (!f) throw std::runtime_error("cannot read state file: " + *path);
            blob.resize(size_t(f.tellg()));
            f.seekg(0);
            if (!f.read(reinterpret_cast<char *>(blob.data()), std::streamsize(blob.size())))
                throw std::runtime_error("state file read failed: " + *path);
        } else {
            blob = base64_decode(*b64);
        }
        if (blob.size() != m.state_size())
            throw std::runtime_error("state is " + std::to_string(blob.size()) + " bytes but this machine's state is " +
                                     std::to_string(m.state_size()));
        m.load_state(blob);
        return {{"frame", m.frame}, {"pc", m.cpu.pc}, {"state_crc", crc_text(m.state_crc())}};
    }

    // PNG bytes of the current native frame, plus the metadata object. `path` (optional) also writes a file:
    // BMP for a .bmp extension, PNG otherwise.
    std::pair<std::vector<uint8_t>, json> screenshot(const Args &p) {
        Machine &m = machine();
        const auto &px = m.native_pixels();
        const unsigned width = unsigned(geometry::NATIVE_WIDTH);
        const unsigned height = unsigned(px.size() / geometry::NATIVE_WIDTH);
        auto png = encode_png(px, width, height);
        json r{{"width", width}, {"height", height}, {"frame", m.frame}, {"frame_crc", crc_text(frame_crc(m))},
               {"mime_type", "image/png"}, {"png_bytes", png.size()}};
        if (const auto path = p.str("path")) {
            const std::filesystem::path file(*path);
            if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
            if (file.extension() == ".bmp") write_bmp(file, px);
            else write_bytes(file, png);
            r["path"] = *path;
        }
        return {std::move(png), std::move(r)};
    }

    json sprites(const Args &p) {
        Machine &m = machine();
        const unsigned start_bank = unsigned(p.u64_or("start_bank", 0));
        if (start_bank > 1) throw std::runtime_error("start_bank must be 0 or 1");
        const uint64_t limit = p.u64_or("limit", 0x400);
        const bool drawn_only = p.boolean("drawn_only").value_or(true);
        const std::span<const uint8_t> ram = std::span<const uint8_t>(m.graphics).first(SPRITE_RAM_SIZE);
        json r = decode_sprites(m, ram, start_bank, limit, drawn_only);
        r["frame"] = m.frame;
        r["address"] = GRAPHICS_BASE;
        if (m.sprite_writers) {
            json writers = json::array();
            for (const uint32_t pc : *m.sprite_writers) writers.push_back(pc);
            r["writers"] = std::move(writers);
        } else {
            r["writers"] = nullptr;
        }
        if (p.boolean("raw").value_or(false)) {
            r["length"] = SPRITE_RAM_SIZE;
            r["hex"] = hex_encode(ram);
        }
        return r;
    }

    json video_diff(const Args &p) {
        Machine &m = machine();
        if (!m_video_diff) throw std::runtime_error("video_diff needs init with video_diff:true (FDP oracle)");
        const unsigned mask = unsigned(p.u64_or("layer_mask", 511));
        json r{{"frame", m.frame}, {"layer_mask", mask}};
        try {
            m.game_video->compare_layers(m.frame, mask);
            r["ok"] = true;
        } catch (const std::exception &e) {
            r["ok"] = false;
            r["error"] = e.what();
        }
        return r;
    }

    json audio_stats() {
        machine();
        const f3rt_audio_stats s = f3rt_audio_performance_stats();
        return {{"commands", s.commands},
                {"rendered_frames", s.rendered_frames},
                {"voices_started", s.voices_started},
                {"render_ns", s.render_ns},
                {"voices_active", s.voices_active},
                {"sample_rate", f3rt_audio_sample_rate()}};
    }

private:
    Machine &machine() {
        if (!m_machine) throw std::runtime_error("not initialized: call init first");
        return *m_machine;
    }

    void destroy() {
        m_machine.reset(); // releases the process-wide audio board and EEPROM
        m_oracle.reset();
        m_slots.clear();
        m_set.clear();
        m_video_diff = m_sprite_writers = false;
    }

    static uint32_t frame_crc(const Machine &m) {
        const auto &px = m.native_pixels();
        return crc32(reinterpret_cast<const uint8_t *>(px.data()), px.size() * 4);
    }

    static const Region &region_named(const std::string &name) {
        for (const Region &region : REGIONS)
            if (region.name == name) return region;
        throw std::runtime_error("unknown region: " + name);
    }

    static std::vector<uint8_t> region_bytes(Machine &m, const Region &region) {
        if (region.name == "ram") return {m.ram.begin(), m.ram.end()};
        if (region.name == "graphics") return {m.graphics.begin(), m.graphics.end()};
        if (region.name == "palette") return {m.palette.begin(), m.palette.end()};
        if (region.name == "control") return {m.control.begin(), m.control.end()};
        if (region.name == "shared") return {m.shared.begin(), m.shared.end()};
        if (!m.sprite_writers) throw std::runtime_error("sprite_writers is off: init with sprite_writers:true");
        std::vector<uint8_t> out;
        out.reserve(region.size);
        for (const uint32_t pc : *m.sprite_writers)
            for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(pc >> shift));
        return out;
    }

    // 1, 2 or 4 bytes; also "b", "w", "l".
    static unsigned access_size(const Args &p) {
        if (const json *v = p.find("size"); v && v->is_string()) {
            const std::string s = v->get<std::string>();
            if (s == "b" || s == "byte") return 1;
            if (s == "w" || s == "word") return 2;
            if (s == "l" || s == "long") return 4;
        }
        const uint64_t size = p.u64_or("size", 1);
        if (size != 1 && size != 2 && size != 4) throw std::runtime_error("size must be 1, 2 or 4 (or b, w, l)");
        return unsigned(size);
    }

    // Walks the sprite list the way the chip does (runtime/renderer/decode.cpp decode_sprite_list). Two banks of
    // 0x400 16-byte entries (0x0000 / 0x8000); a command entry (word 3 bit 15) switches banks; word 6 bit 15 jumps
    // to entry w6 & 0x3ff, and a jump to itself ends the list. Words: w0 tile (bit 16 = w5 bit 0), w1 zoom
    // (x low byte, y high byte), w2/w3 x/y (12-bit signed, scroll mode in w2 bits 12-15), w4 high byte sprite
    // control, w4 low byte colour.
    static json decode_sprites(const Machine &m, std::span<const uint8_t> ram, unsigned start_bank, uint64_t limit,
                               bool drawn_only) {
        struct Axis {
            int blockScale = 0x100;
            int64_t pos = 0, blockPos = 0;
            int global = 0, subglobal = 0;
            static int signed12(unsigned v) { return (v & 0x800) ? int(v & 0xfff) - 0x1000 : int(v & 0xfff); }
            void update(unsigned scroll, unsigned posw, bool multi, unsigned blockCtrl, unsigned zoom) {
                int value = signed12(posw);
                if (scroll & 1) subglobal = value;
                if (scroll & 2) global = value;
                if (!(scroll & 8)) {
                    value += global;
                    if (!(scroll & 4)) value += subglobal;
                }
                if (blockCtrl == 0 && !multi) {
                    blockPos = int64_t(value) * 256;
                    blockScale = 0x100 - int(zoom);
                }
                if (blockCtrl == 0 || blockCtrl == 2) pos = blockPos;
                else if (blockCtrl == 3) pos += int64_t(blockScale) * 16;
            }
        };
        Axis x, y;
        unsigned color = 0, bank = start_bank;
        bool multi = false;
        json entries = json::array();
        json banks_read = json::array(), switches = json::array();
        std::set<unsigned> seen_banks;
        uint64_t jumps = 0, commands = 0, drawn_count = 0, visited = 0;
        json end = nullptr, last_used = nullptr;
        unsigned offs = 0;
        while (offs < 0x400 && visited < limit) {
            ++visited;
            const size_t base = size_t(bank) * 0x8000 + size_t(offs) * 16;
            if (base + 16 > ram.size()) break;
            unsigned w[8];
            for (int i = 0; i < 8; ++i) w[i] = unsigned(ram[base + 2 * i]) << 8 | ram[base + 2 * i + 1];
            json notes = json::array();
            const unsigned this_bank = bank;
            if (seen_banks.insert(bank).second) banks_read.push_back(bank);
            bool any = false;
            for (int i = 0; i < 7; ++i) any |= w[i] != 0;
            if (any) last_used = json::array({bank, offs});
            if (w[3] & 0x8000) {
                ++commands;
                if ((w[5] & 1) != bank) switches.push_back(json::array({bank, offs, w[5] & 1}));
                bank = w[5] & 1;
                char buf[96];
                std::snprintf(buf, sizeof buf, "command: bank %u, flipscreen %u, extra planes %u, trails %u", bank,
                              (w[5] & 0x2000) ? 1u : 0u, (w[5] >> 8) & 3, (w[5] & 2) ? 1u : 0u);
                notes.push_back(buf);
            }
            const unsigned slot = offs;
            bool at_end = false;
            if (w[6] & 0x8000) {
                const unsigned target = w[6] & 0x3ff;
                if (target == offs) {
                    notes.push_back("list end (jump to itself)");
                    at_end = true;
                } else {
                    ++jumps;
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "jump to 0x%03x", target);
                    notes.push_back(buf);
                    offs = target - 1;
                }
            }
            const unsigned control = w[4] >> 8, scroll = (w[2] >> 12) & 0xf;
            if (!at_end) {
                if (!(control & 4)) color = w[4] & 0xff;
                x.update(scroll, w[2], multi, (control >> 6) & 3, w[1] & 0xff);
                y.update(scroll, w[3], multi, (control >> 4) & 3, w[1] >> 8);
                multi = (control & 8) != 0;
            }
            const unsigned tile = w[0] | ((w[5] & 1) << 16);
            const bool drawn = tile != 0 && !at_end;
            if (drawn) ++drawn_count;
            if (!drawn_only || drawn) {
                json entry{{"slot", slot},
                           {"bank", this_bank},
                           {"address", GRAPHICS_BASE + this_bank * 0x8000 + slot * 16},
                           {"words", json::array({w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]})},
                           {"tile", tile},
                           {"color", color},
                           {"x", double(x.pos) / 256},
                           {"y", double(y.pos) / 256},
                           {"scale_x", x.blockScale},
                           {"scale_y", y.blockScale},
                           {"control", control},
                           {"drawn", drawn},
                           {"notes", std::move(notes)}};
                if (m.sprite_writers) entry["writer_pc"] = (*m.sprite_writers)[base >> 4];
                entries.push_back(std::move(entry));
            }
            if (at_end) {
                end = slot;
                break;
            }
            ++offs;
        }
        return {{"entries", std::move(entries)},
                {"summary",
                 {{"start_bank", start_bank},
                  {"end", end},
                  {"jumps", jumps},
                  {"commands", commands},
                  {"drawn", drawn_count},
                  {"visited", visited},
                  {"banks_read", std::move(banks_read)},
                  {"switches", std::move(switches)},
                  {"last_used", last_used}}}};
    }

    Defaults m_defaults;
    std::unique_ptr<Video> m_oracle; // the machine's scanout hook borrows it; destroy() releases the machine first
    std::unique_ptr<Machine> m_machine;
    std::unordered_map<std::string, std::vector<uint8_t>> m_slots;
    std::string m_set;
    bool m_sprite_writers = false;
    bool m_video_diff = false;
};

// ---- MCP plumbing --------------------------------------------------------------------------------

json schema_object(json properties, std::initializer_list<const char *> required = {}) {
    json req = json::array();
    for (const char *name : required) req.push_back(name);
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(req)}};
}

const json ADDRESS_TYPE = {{"type", json::array({"integer", "string"})},
                           {"description", "An integer or a hex string such as \"0x400000\" or \"$400000\"."}};

mcp::CallToolResult text_result(const json &structured) {
    mcp::CallToolResult r;
    r.content.push_back(mcp::TextContent{.text = structured.dump(-1, ' ', false, json::error_handler_t::replace)});
    r.structured_content = structured;
    return r;
}

mcp::CallToolResult error_result(const std::string &message) {
    mcp::CallToolResult r;
    r.content.push_back(mcp::TextContent{.text = message});
    r.is_error = true;
    return r;
}

constexpr const char *INSTRUCTIONS =
    "One Taito F3 machine (68EC020 main CPU, recompiled game) that you can step, inspect and poke. "
    "Call `init` first (unless the server was started with --rom-dir and without --no-init), then "
    "`step` frames, `set_input`, `regs`, `read`, `write`, `screenshot` (returns a PNG image), `sprites`, "
    "`save_state`/`load_state` to checkpoint and replay. `reset` restarts the game; `shutdown` frees the "
    "machine. `init` always replaces the running machine. Addresses accept integers or hex strings "
    "(\"0x401f54\", \"$401f54\"). `regs` and region reads never change `state_crc`; `write` is a real bus "
    "write and does. Inputs are not part of a saved state: reset them yourself after `load_state`.";

void print_help() {
    std::cout
        << "Usage: f3rt-tool mcp [options]\n\n"
        << "Model Context Protocol server (stdio) over one process-local Machine.\n"
        << "MCP frames go to stdout; diagnostics and stray runtime prints go to stderr. EOF on stdin exits 0.\n\n"
        << "Options (defaults for the `init` tool; `init` arguments override them):\n"
        << "  --rom-dir DIR        ROM directory (defaults to F3RT_DEFAULT_ROM_DIR)\n"
        << "  --set SET            ROM set name (default: " F3RT_DEFAULT_SET ")\n"
        << "  --game-video 0|1     Game renderer (1, default) or FDP-oracle scanout (0)\n"
        << "  --sprite-writers 0|1 Record per-sprite-slot writer PCs (default 0)\n"
        << "  --eeprom PATH        EEPROM image loaded at init (missing file = factory contents)\n"
        << "  --no-init            Start uninitialised and wait for an `init` tool call\n"
        << "  --help, -h           Show this help message\n\n"
        << "Unless --no-init is given and a ROM directory is known (--rom-dir or F3RT_DEFAULT_ROM_DIR), the\n"
        << "machine is initialised once before serving; a failure there exits 1.\n\n"
        << "Tools: init shutdown reset step set_input regs read write crc audio_stats save_state load_state\n"
        << "       screenshot sprites video_diff   (run `tools/list` for the schemas)\n";
}

bool parse_01(const std::string &flag, const std::string &text) {
    if (text == "1" || text == "true" || text == "on") return true;
    if (text == "0" || text == "false" || text == "off") return false;
    throw std::runtime_error(flag + " expects 0 or 1");
}

// Registers the tools and serves until the client closes the transport.
void serve(Runtime &runtime, int out_fd) {
    mcp::Server server(mcp::Implementation{.name = "f3rt-tool", .version = "1.0.0",
                                           .description = "Taito F3 recompiled runtime machine"});
    server.set_instructions(INSTRUCTIONS);

    // The machine is single-threaded; mcp-cpp runs each request on its own worker thread.
    std::mutex machine_mutex;
    auto tool = [&](const char *name, std::string description, json schema, auto fn) {
        server.tool(
            name, schema,
            [&machine_mutex, schema, fn](const json &arguments) -> mcp::CallToolResult {
                std::lock_guard lock(machine_mutex);
                try {
                    const Args args(arguments);
                    args.reject_unknown(schema);
                    return fn(args);
                } catch (const std::exception &e) {
                    return error_result(e.what());
                }
            },
            std::nullopt, std::move(description));
    };
    auto plain = [](auto fn) { return [fn](const Args &a) { return text_result(fn(a)); }; };

    tool("init",
         "Build (or rebuild) the machine from a ROM set and boot it to frame 0. Replaces any running machine and "
         "clears the saved-state slots. All arguments default to the server's startup options.",
         schema_object({{"rom_dir", {{"type", "string"}, {"description", "Directory holding the ROM set files."}}},
                        {"set", {{"type", "string"}, {"description", "ROM set name."}}},
                        {"game_video", {{"type", "boolean"}, {"description", "true: game renderer (default); false: FDP-oracle scanout."}}},
                        {"sprite_writers", {{"type", "boolean"}, {"description", "Record the PC that last wrote each sprite RAM slot."}}},
                        {"video_diff", {{"type", "boolean"}, {"description", "Also build the FDP oracle so `step` and `video_diff` can compare layers."}}},
                        {"eeprom", {{"type", "string"}, {"description", "EEPROM image to load; a missing file means factory contents."}}}}),
         plain([&](const Args &a) { return runtime.init(a); }));

    tool("shutdown", "Destroy the machine and release the audio board and EEPROM. `init` builds a new one.",
         schema_object(json::object()), plain([&](const Args &) { return runtime.shutdown(); }));

    tool("reset", "Reset the CPU and devices (the audio clock keeps running). Returns frame, pc and state_crc.",
         schema_object(json::object()), plain([&](const Args &) { return runtime.reset(); }));

    tool("step",
         "Run N emulated frames. Returns frame/pc/step counters, frame_crc and state_crc, and audio counters. "
         "Stops early if the CPU halts (unless stop_on_halt is false).",
         schema_object({{"frames", {{"type", "integer"}, {"minimum", 0}, {"default", 1}}},
                        {"audio", {{"type", "string"}, {"enum", json::array({"discard", "capture", "wav"})},
                                   {"description", "discard (default): count only; capture: return PCM as base64 (<= 6 MiB); wav: write wav_path."}}},
                        {"wav_path", {{"type", "string"}, {"description", "Output file for audio:\"wav\"."}}},
                        {"video_diff", {{"type", "boolean"}, {"description", "Compare layers with the FDP oracle after each frame (needs init video_diff:true)."}}},
                        {"video_layer_mask", {{"type", "integer"}, {"default", 511}}},
                        {"stop_on_halt", {{"type", "boolean"}, {"default", true}}},
                        {"crc", {{"type", "boolean"}, {"default", true}, {"description", "Include frame_crc and state_crc."}}}}),
         plain([&](const Args &a) { return runtime.step(a); }));

    tool("set_input",
         "Set controller and system inputs. `mask` with `pressed` presses/releases buttons of `port`; `inputs` sets all "
         "six port words; `system_inputs` is the coin/service/test byte (active low).",
         schema_object({{"port", {{"type", "integer"}, {"default", 0}}},
                        {"mask", ADDRESS_TYPE},
                        {"pressed", {{"type", "boolean"}, {"default", true}}},
                        {"system_inputs", ADDRESS_TYPE},
                        {"inputs", {{"type", "array"}, {"items", ADDRESS_TYPE}, {"maxItems", 6}}}}),
         plain([&](const Args &a) { return runtime.set_input(a); }));

    tool("regs",
         "Read the main CPU registers (pc, sr, d0-d7, a0-a7, usp/isp/msp, vbr, caches). Pending lazy flags are flushed "
         "on a copy, so state_crc is unchanged.",
         schema_object(json::object()), plain([&](const Args &) { return runtime.regs(); }));

    tool("read",
         "Read memory. Scalar: addr + size (1|2|4 or b|w|l) returns `value`. Byte run: addr + length through the bus. "
         "Region: region (ram, graphics, palette, control, shared, sprite_writers) + optional offset/length returns hex.",
         schema_object({{"addr", ADDRESS_TYPE},
                        {"size", {{"type", json::array({"integer", "string"})}, {"default", 1}}},
                        {"length", ADDRESS_TYPE},
                        {"region", {{"type", "string"}, {"enum", json::array({"ram", "graphics", "palette", "control", "shared", "sprite_writers"})}}},
                        {"offset", ADDRESS_TYPE}}),
         plain([&](const Args &a) { return runtime.read(a); }));

    tool("write",
         "Write memory through the bus (hardware side effects apply, state_crc changes). Scalar: addr + size + value. "
         "Bytes: addr + hex, or region (ram, graphics, palette, control, shared) + offset + hex.",
         schema_object({{"addr", ADDRESS_TYPE},
                        {"size", {{"type", json::array({"integer", "string"})}, {"default", 1}}},
                        {"value", ADDRESS_TYPE},
                        {"hex", {{"type", "string"}, {"description", "Bytes as hex digits."}}},
                        {"region", {{"type", "string"}, {"enum", json::array({"ram", "graphics", "palette", "control", "shared"})}}},
                        {"offset", ADDRESS_TYPE}}),
         plain([&](const Args &a) { return runtime.write(a); }));

    tool("crc", "CRC32 of the current frame (frame_crc) and of the full machine state (state_crc).",
         schema_object(json::object()), plain([&](const Args &) { return runtime.crc(); }));

    tool("audio_stats", "Audio board performance counters and the output sample rate.",
         schema_object(json::object()), plain([&](const Args &) { return runtime.audio_stats(); }));

    tool("save_state",
         "Checkpoint the machine: into an in-memory `slot`, a `path` file (.f3s), and/or inline base64.",
         schema_object({{"slot", {{"type", "string"}}},
                        {"path", {{"type", "string"}}},
                        {"inline", {{"type", "boolean"}, {"default", false}}}}),
         plain([&](const Args &a) { return runtime.save_state(a); }));

    tool("load_state",
         "Restore a checkpoint from exactly one of `slot`, `path` or `base64`. Inputs are not part of a state.",
         schema_object({{"slot", {{"type", "string"}}}, {"path", {{"type", "string"}}}, {"base64", {{"type", "string"}}}}),
         plain([&](const Args &a) { return runtime.load_state(a); }));

    tool("screenshot",
         "Capture the current native frame. Returns an image/png content block plus metadata (width, height, frame_crc). "
         "`path` also writes a file (.bmp for BMP, anything else PNG).",
         schema_object({{"path", {{"type", "string"}}}}),
         [&](const Args &a) {
             auto [png, meta] = runtime.screenshot(a);
             mcp::CallToolResult r;
             r.content.push_back(mcp::ImageContent{.data = base64_encode(png), .mime_type = "image/png"});
             r.content.push_back(mcp::TextContent{.text = meta.dump()});
             r.structured_content = std::move(meta);
             return r;
         });

    tool("sprites",
         "Decode the sprite list the way the chip walks it: entries (tile, colour, x/y, scale, control, notes), a summary "
         "(commands, jumps, bank switches, end slot) and, when init used sprite_writers, writer PCs (`writers` per 16-byte "
         "slot and `writer_pc` per entry). Only drawn entries are listed unless drawn_only is false. `raw` adds the "
         "64 KiB sprite RAM as hex.",
         schema_object({{"drawn_only", {{"type", "boolean"}, {"default", true}}},
                        {"start_bank", {{"type", "integer"}, {"enum", json::array({0, 1})}, {"default", 0}}},
                        {"limit", {{"type", "integer"}, {"default", 1024}, {"description", "Most list entries to visit."}}},
                        {"raw", {{"type", "boolean"}, {"default", false}}}}),
         plain([&](const Args &a) { return runtime.sprites(a); }));

    tool("video_diff",
         "Compare the game renderer's layers with the FDP oracle for the current frame (needs init video_diff:true; "
         "meaningful from about frame 600).",
         schema_object({{"layer_mask", {{"type", "integer"}, {"default", 511}}}}),
         plain([&](const Args &a) { return runtime.video_diff(a); }));

    mcp::StdioTransport::Options options;
    options.read_fd = STDIN_FILENO;
    options.write_fd = out_fd;
    options.owns_fds = false;
    server.run(std::make_unique<mcp::StdioTransport>(options));
}

} // namespace

int cmd_mcp(int argc, char **argv) try {
    Defaults defaults;
#ifdef F3RT_DEFAULT_ROM_DIR
    defaults.rom_dir = F3RT_DEFAULT_ROM_DIR;
#endif
    bool no_init = false;
    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for argument: " + arg);
            return argv[++i];
        };
        if (arg == "--rom-dir") defaults.rom_dir = value();
        else if (arg == "--set") defaults.set = value();
        else if (arg == "--game-video") defaults.game_video = parse_01(arg, value());
        else if (arg == "--sprite-writers") defaults.sprite_writers = parse_01(arg, value());
        else if (arg == "--eeprom") defaults.eeprom = value();
        else if (arg == "--no-init") no_init = true;
        else if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    // Stdout is reserved for MCP frames: the transport gets a private duplicate of fd 1, and fd 1 itself now
    // points at stderr, so anything the runtime prints can never corrupt the stream.
    int out_fd = STDOUT_FILENO;
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
    std::cout.flush();
    std::fflush(stdout);
    out_fd = ::dup(STDOUT_FILENO);
    if (out_fd < 0) throw std::runtime_error("cannot duplicate stdout");
    ::dup2(STDERR_FILENO, STDOUT_FILENO);
#endif

    const bool eager = !no_init;
    {
        Runtime runtime(std::move(defaults));
        if (eager && runtime.can_init_from_defaults()) runtime.init_from_defaults();
        serve(runtime, out_fd);
    } // the machine (and its audio/EEPROM leases) is torn down before the process exits
#if !defined(_WIN32)
    ::close(out_fd);
#endif
    return 0;
} catch (const std::exception &e) {
    std::cerr << "mcp: " << e.what() << '\n';
    return 1;
}

} // namespace f3rt::tool
