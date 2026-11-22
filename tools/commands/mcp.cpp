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
#include "frontend/state.hpp"
#include "renderer/game/scene.hpp"
#include "sprite_walk.hpp"

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
#include <concepts>
#include <condition_variable>
#include <deque>
#include <limits>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <pthread.h>
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

// Strict unsigned integer text: decimal, or hex with a 0x/0X/$ prefix. No sign, whitespace, separators or
// trailing characters, and the value must fit `max` (the destination width) exactly.
uint64_t parse_unsigned_text(std::string_view text, uint64_t max, std::string_view key) {
    const std::string name = "'" + std::string(key) + "'";
    std::string_view digits = text;
    unsigned base = 10;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        base = 16;
        digits.remove_prefix(2);
    } else if (digits.starts_with('$')) {
        base = 16;
        digits.remove_prefix(1);
    }
    if (digits.empty()) throw std::runtime_error(name + " is not an unsigned number: \"" + std::string(text) + "\"");
    uint64_t value = 0;
    for (const char c : digits) {
        unsigned digit;
        if (c >= '0' && c <= '9') digit = unsigned(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') digit = unsigned(c - 'a') + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') digit = unsigned(c - 'A') + 10;
        else throw std::runtime_error(name + " is not an unsigned number: \"" + std::string(text) + "\"");
        if (digit > max || value > (max - digit) / base)
            throw std::runtime_error(name + " is out of range (maximum " + std::to_string(max) + "): \"" +
                                     std::string(text) + "\"");
        value = value * base + digit;
    }
    return value;
}

// Typed access to a tool's `arguments`; every getter throws std::runtime_error naming the key. Numbers are
// unsigned and range-checked against the destination type T before they are narrowed: JSON integers or
// strict decimal / 0x / $ hex strings; negatives, floats, signs and trailing junk are rejected.
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

    template <std::unsigned_integral T> std::optional<T> num(std::string_view key) const {
        const json *v = find(key);
        if (!v) return std::nullopt;
        return T(number(*v, std::numeric_limits<T>::max(), key));
    }

    template <std::unsigned_integral T> T num_or(std::string_view key, T fallback) const {
        return num<T>(key).value_or(fallback);
    }

    template <std::unsigned_integral T> T require_num(std::string_view key) const {
        const auto v = num<T>(key);
        if (!v) throw std::runtime_error("missing '" + std::string(key) + "'");
        return *v;
    }

    std::optional<bool> boolean(std::string_view key) const {
        const json *v = find(key);
        if (!v) return std::nullopt;
        if (v->is_boolean()) return v->get<bool>();
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

    template <std::unsigned_integral T> std::optional<std::vector<T>> num_array(std::string_view key) const {
        const json *v = find(key);
        if (!v) return std::nullopt;
        if (!v->is_array()) throw std::runtime_error("'" + std::string(key) + "' must be an array");
        std::vector<T> items;
        for (const json &item : *v) items.push_back(T(number(item, std::numeric_limits<T>::max(), key)));
        return items;
    }

    // Rejects keys the tool's schema does not declare, so a typo is an error instead of a silent default.
    void reject_unknown(const json &schema) const {
        if (!m_obj) return;
        const json &props = schema.at("properties");
        for (const auto &[key, value] : m_obj->items())
            if (!props.contains(key)) throw std::runtime_error("unknown argument '" + key + "'");
    }

    // At most one of `keys` may be present.
    void exclusive(std::initializer_list<std::string_view> keys, std::string_view what) const {
        std::string seen;
        for (const std::string_view key : keys) {
            if (!has(key)) continue;
            if (!seen.empty())
                throw std::runtime_error(std::string(what) + ": '" + seen + "' and '" + std::string(key) +
                                         "' are mutually exclusive");
            seen = std::string(key);
        }
    }

    // `key` is not allowed alongside the argument shape `shape`.
    void forbid(std::initializer_list<std::string_view> keys, std::string_view shape) const {
        for (const std::string_view key : keys)
            if (has(key)) throw std::runtime_error("'" + std::string(key) + "' is not valid for " + std::string(shape));
    }

private:
    static uint64_t number(const json &v, uint64_t max, std::string_view key) {
        const std::string name = "'" + std::string(key) + "'";
        if (v.is_number_unsigned()) {
            const uint64_t n = v.get<uint64_t>();
            if (n > max) throw std::runtime_error(name + " is out of range (maximum " + std::to_string(max) + ")");
            return n;
        }
        if (v.is_number_integer()) throw std::runtime_error(name + " must not be negative");
        if (v.is_string()) return parse_unsigned_text(v.get_ref<const std::string &>(), max, key);
        throw std::runtime_error(name + " must be an unsigned integer or a decimal / 0x hex string");
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
        // Everything is validated before the first frame runs or any output file is opened.
        const uint64_t frames = p.num_or<uint64_t>("frames", 1);
        const std::string audio = p.str("audio").value_or("discard");
        const bool diff = p.boolean("video_diff").value_or(false);
        const unsigned mask = layer_mask(p, "video_layer_mask");
        const uint64_t diff_ready_frame = p.num_or<uint64_t>("video_diff_min_frame", VIDEO_DIFF_MIN_FRAME);
        const bool stop_on_halt = p.boolean("stop_on_halt").value_or(true);
        const bool want_crc = p.boolean("crc").value_or(true);
        if (audio != "discard" && audio != "capture" && audio != "wav")
            throw std::runtime_error("audio must be \"discard\", \"capture\" or \"wav\"");
        std::optional<std::string> wav_path;
        if (audio == "wav") wav_path = p.require_str("wav_path");
        else if (p.has("wav_path")) throw std::runtime_error("wav_path needs audio:\"wav\"");
        if (diff && !m_video_diff) throw std::runtime_error("video_diff needs init with video_diff:true");
        std::unique_ptr<WavWriter> wav;
        if (wav_path) {
            const std::filesystem::path path(*wav_path);
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
            wav = std::make_unique<WavWriter>(path, f3rt_audio_sample_rate());
        }

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

        uint64_t run = 0, compared = 0, skipped = 0;
        bool stopped = false;
        std::optional<std::string> video_error;
        while (run < frames) {
            const bool ok = m.run_frame();
            ++run;
            drain();
            if (diff) {
                if (m.frame < diff_ready_frame) {
                    ++skipped; // the FDP oracle is not meaningful during boot (see VIDEO_DIFF_MIN_FRAME)
                } else {
                    ++compared;
                    try {
                        m.game_video->compare_layers(m.frame, mask);
                    } catch (const std::exception &e) {
                        video_error = e.what();
                        stopped = true;
                        break;
                    }
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
            r["video_diff_ready_frame"] = diff_ready_frame;
            r["video_diff_compared"] = compared;
            r["video_diff_skipped_frames"] = skipped;
            r["video_diff_skipped"] = skipped > 0;
            if (video_error) r["video_error"] = *video_error;
        }
        return r;
    }

    json set_input(const Args &p) {
        Machine &m = machine();
        // Parse and validate everything first so a bad argument changes nothing.
        const auto all = p.num_array<uint32_t>("inputs");
        const auto sys = p.num<uint8_t>("system_inputs");
        const bool has_mask = p.has("mask");
        if (!has_mask) p.forbid({"port", "pressed"}, "set_input without 'mask'");
        if (all && all->size() > m.inputs.size())
            throw std::runtime_error("inputs holds at most " + std::to_string(m.inputs.size()) + " ports");
        uint32_t mask = 0;
        uint8_t port = 0;
        bool pressed = true;
        if (has_mask) {
            mask = p.require_num<uint32_t>("mask");
            port = p.num_or<uint8_t>("port", 0);
            pressed = p.boolean("pressed").value_or(true);
            if (port >= m.inputs.size()) throw std::runtime_error("port out of range");
        }
        if (all)
            for (size_t i = 0; i < all->size(); ++i) m.inputs[i] = (*all)[i];
        if (sys) m.system_inputs = *sys;
        if (has_mask) m.set_input(port, mask, pressed);
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
        const bool region_form = p.has("region");
        if (region_form && p.has("addr"))
            throw std::runtime_error("read: 'region' and 'addr' are mutually exclusive");
        if (!region_form && !p.has("addr"))
            throw std::runtime_error("read needs 'addr' (scalar or byte run) or 'region'");
        if (region_form) {
            p.forbid({"size"}, "read with 'region'");
            const Region &region = region_named(*p.str("region"));
            const uint64_t offset = p.num_or<uint32_t>("offset", 0);
            const uint64_t length = p.num_or<uint32_t>("length", uint32_t(region.size - std::min<uint64_t>(offset, region.size)));
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
        p.forbid({"offset"}, "read with 'addr'");
        p.exclusive({"length", "size"}, "read");
        const uint32_t addr = p.require_num<uint32_t>("addr");
        if (p.has("length")) {
            // A byte run through the bus.
            const uint32_t length = p.require_num<uint32_t>("length");
            if (length > RAM_SIZE) throw std::runtime_error("length is too large (maximum " + std::to_string(RAM_SIZE) + ")");
            check_bus_span(addr, length);
            std::vector<uint8_t> bytes(length);
            for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = m.read8(addr + uint32_t(i));
            return {{"addr", addr}, {"length", length}, {"hex", hex_encode(bytes)}};
        }
        const unsigned size = access_size(p);
        check_bus_span(addr, size);
        const uint32_t value = size == 1 ? m.read8(addr) : size == 2 ? m.read16(addr) : m.read32(addr);
        return {{"addr", addr}, {"size", size}, {"value", value}};
    }

    json write(const Args &p) {
        Machine &m = machine();
        const bool region_form = p.has("region");
        if (region_form && p.has("addr"))
            throw std::runtime_error("write: 'region' and 'addr' are mutually exclusive");
        if (!region_form && !p.has("addr"))
            throw std::runtime_error("write needs 'addr' (scalar or hex bytes) or 'region' + 'hex'");
        if (region_form) {
            p.forbid({"size", "value"}, "write with 'region'");
            const Region &region = region_named(*p.str("region"));
            if (!region.bus_base) throw std::runtime_error(std::string(region.name) + " is read only");
            const uint64_t offset = p.num_or<uint32_t>("offset", 0);
            const auto bytes = hex_decode(p.require_str("hex"));
            if (offset > region.size || bytes.size() > region.size - offset)
                throw std::runtime_error("range is outside " + std::string(region.name));
            // Through the bus, so hardware side effects (shared RAM -> sound board, mirrors) apply.
            for (size_t i = 0; i < bytes.size(); ++i) m.write8(region.bus_base + uint32_t(offset + i), bytes[i]);
            return {{"region", std::string(region.name)}, {"offset", offset}, {"length", bytes.size()}};
        }
        p.forbid({"offset"}, "write with 'addr'");
        p.exclusive({"hex", "value"}, "write");
        const uint32_t addr = p.require_num<uint32_t>("addr");
        if (p.has("hex")) {
            p.forbid({"size"}, "write with 'hex'");
            const auto bytes = hex_decode(p.require_str("hex"));
            check_bus_span(addr, bytes.size());
            for (size_t i = 0; i < bytes.size(); ++i) m.write8(addr + uint32_t(i), bytes[i]);
            return {{"addr", addr}, {"length", bytes.size()}};
        }
        if (!p.has("value")) throw std::runtime_error("write needs 'value' (scalar) or 'hex' (bytes)");
        const unsigned size = access_size(p);
        const uint64_t max = size == 4 ? 0xffffffffull : (1ull << (8 * size)) - 1;
        const uint64_t value = p.require_num<uint64_t>("value");
        if (value > max)
            throw std::runtime_error("'value' 0x" + hex_number(value) + " does not fit in " + std::to_string(size) +
                                     " byte(s)");
        check_bus_span(addr, size);
        if (size == 1) m.write8(addr, uint8_t(value));
        else if (size == 2) m.write16(addr, uint16_t(value));
        else m.write32(addr, uint32_t(value));
        return {{"addr", addr}, {"size", size}, {"value", value}};
    }

    // Slots and inline base64 hold the raw machine state of this exact session. `path` files use the shared
    // frontend slot format (ROM CRCs, state size and CRC header) so a file from another ROM set or build is refused.
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
            frontend_state_slot(m, std::filesystem::path(*path), 1, 0, true);
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
        if (path) {
            // Header, size (checked before any allocation) and CRC are verified first; the load rolls back on failure.
            frontend_state_slot(m, std::filesystem::path(*path), 1, 0, false);
        } else if (slot) {
            const auto it = m_slots.find(*slot);
            if (it == m_slots.end()) throw std::runtime_error("no saved state in slot: " + *slot);
            load_blob_atomic(m, it->second);
        } else {
            load_blob_atomic(m, base64_decode(*b64));
        }
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
        const auto start_bank = p.num_or<uint8_t>("start_bank", 0);
        if (start_bank > 1) throw std::runtime_error("start_bank must be 0 or 1");
        const auto limit = p.num_or<uint32_t>("limit", SPRITE_WALK_BUDGET);
        if (limit < 1 || limit > SPRITE_WALK_BUDGET)
            throw std::runtime_error("limit must be 1.." + std::to_string(SPRITE_WALK_BUDGET) +
                                     " (the renderer walks at most " + std::to_string(SPRITE_WALK_BUDGET) +
                                     " entries per frame)");
        const bool start_flip = p.boolean("start_flipscreen").value_or(false);
        const bool drawn_only = p.boolean("drawn_only").value_or(true);
        const std::span<const uint8_t> ram = std::span<const uint8_t>(m.graphics).first(SPRITE_RAM_SIZE);
        SpriteRamState start;
        start.bank = start_bank != 0;
        start.flipscreen = start_flip;
        const SpriteWalkResult walk =
            walk_sprite_list(ram, start, limit, int(geometry::FIRST_LINE), int(geometry::HEIGHT));
        json r = sprites_json(m, walk, start, drawn_only);
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
        const unsigned mask = layer_mask(p, "layer_mask");
        const uint64_t ready_frame = p.num_or<uint64_t>("min_frame", VIDEO_DIFF_MIN_FRAME);
        json r{{"frame", m.frame}, {"layer_mask", mask}, {"ready_frame", ready_frame}};
        if (m.frame < ready_frame) {
            r["ok"] = nullptr;
            r["skipped"] = true;
            return r;
        }
        r["skipped"] = false;
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
        const auto size = p.num_or<uint8_t>("size", 1);
        if (size != 1 && size != 2 && size != 4) throw std::runtime_error("size must be 1, 2 or 4 (or b, w, l)");
        return size;
    }

    // A bus access must not wrap past the 32-bit address space.
    static void check_bus_span(uint32_t addr, uint64_t length) {
        if (uint64_t(addr) + length > 0x100000000ull)
            throw std::runtime_error("0x" + hex_number(addr) + "+0x" + hex_number(length) +
                                     " runs past the 32-bit address space");
    }

    // A layer mask selects PF0..3, SP0..3 and text: bits 0..8, at least one set.
    static unsigned layer_mask(const Args &p, std::string_view key) {
        const auto mask = p.num_or<uint32_t>(key, ALL_LAYERS);
        if (!mask || (mask & ~ALL_LAYERS))
            throw std::runtime_error("'" + std::string(key) + "' must select layers in bits 0.." +
                                     std::to_string(LAYER_COUNT - 1) + " (1.." + std::to_string(ALL_LAYERS) + ")");
        return mask;
    }

    // Loads `blob` or leaves the machine exactly as it was: Machine::load_state overwrites the CPU, RAM and devices
    // in sequence, so a failure part-way is undone from a snapshot taken first.
    static void load_blob_atomic(Machine &m, std::span<const uint8_t> blob) {
        if (blob.size() != m.state_size())
            throw std::runtime_error("state is " + std::to_string(blob.size()) + " bytes but this machine's state is " +
                                     std::to_string(m.state_size()));
        std::vector<uint8_t> previous(m.state_size());
        m.save_state(previous);
        try {
            m.load_state(blob);
        } catch (const std::exception &e) {
            try {
                m.load_state(previous);
            } catch (const std::exception &restore) {
                throw std::runtime_error(std::string("state load failed (") + e.what() +
                                         ") and the rollback failed too (" + restore.what() +
                                         "); the machine is inconsistent, call init");
            }
            throw std::runtime_error(std::string("state load failed, machine unchanged: ") + e.what());
        }
    }

    // The sprite tool's JSON: the renderer-accurate walk (see sprite_walk.hpp).
    static json sprites_json(const Machine &m, const SpriteWalkResult &walk, const SpriteRamState &start,
                             bool drawn_only) {
        json entries = json::array(), banks_read = json::array(), switches = json::array();
        std::set<unsigned> seen_banks;
        json last_used = nullptr;
        for (size_t i = 0; i < walk.entries.size(); ++i) {
            const SpriteWalkEntry &e = walk.entries[i];
            if (seen_banks.insert(e.bank).second) banks_read.push_back(e.bank);
            bool any = false;
            for (int k = 0; k < 7; ++k) any |= e.words[k] != 0;
            if (any) last_used = json::array({e.bank, e.slot});
            if (i + 1 < walk.entries.size() && walk.entries[i + 1].bank != e.bank)
                switches.push_back(json::array({e.bank, e.slot, walk.entries[i + 1].bank}));
            if (drawn_only && !e.drawn) continue;
            json notes = json::array();
            if (e.command) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "command: bank %u, flipscreen %u, extra planes %u, trails %u",
                              unsigned(e.words[5] & 1), (e.words[5] & 0x2000) ? 1u : 0u, (e.words[5] >> 8) & 3u,
                              (e.words[5] & 2) ? 1u : 0u);
                notes.push_back(buf);
            }
            if (e.list_end) {
                notes.push_back("list end (jump to itself)");
            } else if (e.jump >= 0) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "jump to 0x%03x", unsigned(e.jump));
                notes.push_back(buf);
            }
            if (e.culled) notes.push_back("culled: outside the scanout crop");
            json entry{{"slot", e.slot},
                       {"bank", e.bank},
                       {"address", GRAPHICS_BASE + e.bank * SPRITE_BANK_STRIDE + e.slot * 16},
                       {"words", json::array({e.words[0], e.words[1], e.words[2], e.words[3], e.words[4], e.words[5],
                                              e.words[6], e.words[7]})},
                       {"tile", e.tile},
                       {"color", e.color},
                       {"x", double(e.x) / 256},
                       {"y", double(e.y) / 256},
                       {"raw_x", double(e.raw_x) / 256},
                       {"raw_y", double(e.raw_y) / 256},
                       {"scale_x", e.scale_x},
                       {"scale_y", e.scale_y},
                       {"flip_x", e.flip_x},
                       {"flip_y", e.flip_y},
                       {"control", e.control},
                       {"drawn", e.drawn},
                       {"culled", e.culled},
                       {"notes", std::move(notes)}};
            if (m.sprite_writers) entry["writer_pc"] = (*m.sprite_writers)[size_t(e.bank) * 0x800 + e.slot];
            entries.push_back(std::move(entry));
        }
        return {{"entries", std::move(entries)},
                {"summary",
                 {{"start_bank", unsigned(start.bank)},
                  {"start_flipscreen", start.flipscreen},
                  {"end", walk.end_slot >= 0 ? json(walk.end_slot) : json(nullptr)},
                  {"jumps", walk.jumps},
                  {"commands", walk.commands},
                  {"drawn", walk.drawn},
                  {"culled", walk.culled},
                  {"visited", walk.visited},
                  {"banks_read", std::move(banks_read)},
                  {"switches", std::move(switches)},
                  {"last_used", last_used},
                  {"end_state",
                   {{"bank", unsigned(walk.end_state.bank)},
                    {"flipscreen", walk.end_state.flipscreen},
                    {"extra_planes", walk.end_state.extra_planes},
                    {"trails", walk.end_state.trails}}}}}};
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
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(req)},
            {"additionalProperties", false}};
}

// Unsigned integer, or strict decimal / hex text ("0x400000", "$400000"); no sign, whitespace or junk. Each
// argument is also range-checked against the width it feeds (32-bit addresses, 8-bit bytes, ...).
const json ADDRESS_TYPE = {{"type", json::array({"integer", "string"})},
                           {"minimum", 0},
                           {"pattern", "^(0[xX][0-9a-fA-F]+|\\$[0-9a-fA-F]+|[0-9]+)$"},
                           {"description", "An unsigned integer or a hex string such as \"0x400000\" or \"$400000\"."}};

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
    "machine. `init` always replaces the running machine. Numbers are unsigned: integers or strict hex "
    "strings (\"0x401f54\", \"$401f54\"), range-checked (addresses are 32-bit); negatives, signs and trailing "
    "junk are rejected, as are unknown arguments. `regs` and region reads never change `state_crc`; `write` is a "
    "real bus write and does. A saved state holds the CPU, RAM, devices, the six input words and `system_inputs`, "
    "so `load_state` restores held buttons too; only the audio synthesizer's voices are outside it. A failed "
    "`load_state` leaves the machine unchanged.";

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

// The one thread that builds, drives and destroys the machine. The audio ABI wants a single caller thread,
// and building the audio board needs ~2 MiB of stack in release (~8.5 MiB with unoptimized Rust): far more
// than the 512 KiB macOS gives the per-request worker threads mcp-cpp dispatches on.
// It owns the Runtime, which it destroys on its own thread once the queue drains.
class MachineThread {
public:
    static constexpr std::size_t STACK_BYTES = 16u << 20;

    std::optional<Runtime> runtime;

    MachineThread() {
        pthread_attr_t attr;
        if (int rc = pthread_attr_init(&attr)) throw std::system_error(rc, std::generic_category(), "machine thread");
        int rc = pthread_attr_setstacksize(&attr, STACK_BYTES);
        if (!rc) rc = pthread_create(&thread_, &attr, entry, this);
        pthread_attr_destroy(&attr);
        if (rc) throw std::system_error(rc, std::generic_category(), "machine thread");
    }

    ~MachineThread() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        pthread_join(thread_, nullptr);
    }

    MachineThread(const MachineThread &) = delete;
    MachineThread &operator=(const MachineThread &) = delete;

    // Runs `fn` on the machine thread and returns its result (or rethrows its exception) to the caller.
    // The queue owns the task: the future can become ready before the task's call has fully returned.
    template <class F> std::invoke_result_t<F &> run(F &&fn) {
        auto task = std::make_shared<std::packaged_task<std::invoke_result_t<F &>()>>(std::ref(fn));
        auto result = task->get_future();
        {
            std::lock_guard lock(mutex_);
            jobs_.emplace_back([task] { (*task)(); });
        }
        wake_.notify_one();
        return result.get();
    }

private:
    static void *entry(void *self) {
        static_cast<MachineThread *>(self)->loop();
        return nullptr;
    }

    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty()) break;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
        runtime.reset();
    }

    pthread_t thread_{};

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_ = false;
};

// Registers the tools and serves until the client closes the transport.
void serve(Runtime &runtime, MachineThread &machine, int out_fd) {
    mcp::Server server(mcp::Implementation{.name = "f3rt-tool", .version = "1.0.0",
                                           .description = "Taito F3 recompiled runtime machine"});
    server.set_instructions(INSTRUCTIONS);

    // mcp-cpp runs each request on its own worker thread; the machine only ever runs on `machine`.
    auto tool = [&](const char *name, std::string description, json schema, auto fn) {
        server.tool(
            name, schema,
            [&machine, schema, fn](const json &arguments) -> mcp::CallToolResult {
                return machine.run([&]() -> mcp::CallToolResult {
                    try {
                        const Args args(arguments);
                        args.reject_unknown(schema);
                        return fn(args);
                    } catch (const std::exception &e) {
                        return error_result(e.what());
                    }
                });
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
         "Stops early if the CPU halts (unless stop_on_halt is false). With video_diff:true the FDP comparison only "
         "runs once the frame reaches video_diff_min_frame (default 600, where the oracle becomes meaningful); earlier "
         "frames are skipped and reported as video_diff_skipped / video_diff_skipped_frames, never as a failure.",
         schema_object({{"frames", {{"type", "integer"}, {"minimum", 0}, {"default", 1}}},
                        {"audio", {{"type", "string"}, {"enum", json::array({"discard", "capture", "wav"})},
                                   {"description", "discard (default): count only; capture: return PCM as base64 (<= 6 MiB); wav: write wav_path."}}},
                        {"wav_path", {{"type", "string"}, {"description", "Output file for audio:\"wav\"."}}},
                        {"video_diff", {{"type", "boolean"}, {"description", "Compare layers with the FDP oracle after each frame (needs init video_diff:true)."}}},
                        {"video_layer_mask", {{"type", "integer"}, {"minimum", 1}, {"maximum", 511}, {"default", 511},
                                              {"description", "Layers to compare: PF0..3, SP0..3, text (bits 0..8)."}}},
                        {"video_diff_min_frame", {{"type", "integer"}, {"minimum", 0}, {"default", int(VIDEO_DIFF_MIN_FRAME)},
                                                  {"description", "First frame that is compared; earlier frames are skipped."}}},
                        {"stop_on_halt", {{"type", "boolean"}, {"default", true}}},
                        {"crc", {{"type", "boolean"}, {"default", true}, {"description", "Include frame_crc and state_crc."}}}}),
         plain([&](const Args &a) { return runtime.step(a); }));

    tool("set_input",
         "Set controller and system inputs. `mask` with `pressed` presses/releases buttons of `port`; `inputs` sets all "
         "six port words; `system_inputs` is the coin/service/test byte (active low).",
         schema_object({{"port", {{"type", "integer"}, {"minimum", 0}, {"maximum", 5}, {"default", 0},
                                  {"description", "Port for `mask`; only valid together with `mask`."}}},
                        {"mask", ADDRESS_TYPE},
                        {"pressed", {{"type", "boolean"}, {"default", true}, {"description", "Only valid together with `mask`."}}},
                        {"system_inputs", ADDRESS_TYPE},
                        {"inputs", {{"type", "array"}, {"items", ADDRESS_TYPE}, {"maxItems", 6}}}}),
         plain([&](const Args &a) { return runtime.set_input(a); }));

    tool("regs",
         "Read the main CPU registers (pc, sr, d0-d7, a0-a7, usp/isp/msp, vbr, caches). Pending lazy flags are flushed "
         "on a copy, so state_crc is unchanged.",
         schema_object(json::object()), plain([&](const Args &) { return runtime.regs(); }));

    tool("read",
         "Read memory in exactly one shape. Scalar: `addr` + optional `size` (1|2|4 or b|w|l) returns `value`. Byte run: "
         "`addr` + `length` through the bus (no `size`). Region: `region` (ram, graphics, palette, control, shared, "
         "sprite_writers) + optional offset/length returns hex (no `addr`/`size`). `{}` or mixed shapes are rejected. "
         "Addresses are 32-bit and accesses must not wrap past 0xffffffff.",
         schema_object({{"addr", ADDRESS_TYPE},
                        {"size", {{"type", json::array({"integer", "string"})}, {"default", 1}}},
                        {"length", ADDRESS_TYPE},
                        {"region", {{"type", "string"}, {"enum", json::array({"ram", "graphics", "palette", "control", "shared", "sprite_writers"})}}},
                        {"offset", ADDRESS_TYPE}}),
         plain([&](const Args &a) { return runtime.read(a); }));

    tool("write",
         "Write memory through the bus (hardware side effects apply, state_crc changes) in exactly one shape. Scalar: "
         "`addr` + optional `size` + `value` (must fit the size). Bytes: `addr` + `hex` (no size/value). Region: `region` "
         "(ram, graphics, palette, control, shared) + optional `offset` + `hex` (no addr/size/value). `{}` or mixed shapes "
         "are rejected.",
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
         "Checkpoint the machine (CPU, RAM, devices, input words and system_inputs; not the audio synth voices): into an "
         "in-memory `slot`, a `path` file (.f3s: header with ROM CRCs, size and CRC, then the state), and/or inline base64 "
         "(the raw state of this session).",
         schema_object({{"slot", {{"type", "string"}}},
                        {"path", {{"type", "string"}}},
                        {"inline", {{"type", "boolean"}, {"default", false}}}}),
         plain([&](const Args &a) { return runtime.save_state(a); }));

    tool("load_state",
         "Restore a checkpoint from exactly one of `slot`, `path` (.f3s) or `base64`. Input words and system_inputs are "
         "part of the state and are restored with it. The load is atomic: a corrupt, incompatible or wrong-size state is "
         "rejected and the machine is left unchanged.",
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
         "slot and `writer_pc` per entry). Follows the renderer's walker (runtime/renderer/decode.cpp): command entries "
         "switch bank/flipscreen for the entries after them, jumps are followed, and an entry is `drawn` only if its tile "
         "is non-zero and it survives the scanout-crop cull (`culled` marks the rest). `x`/`y` are the coordinates as "
         "drawn (flipscreen applied), `raw_x`/`raw_y` before the mirror; `flip_x`/`flip_y` are inverted under "
         "flipscreen. The walk starts from `start_bank`/`start_flipscreen` (the renderer carries these across frames; "
         "summary.end_state is the state it ends in) and visits at most `limit` <= 1024 entries, the renderer's budget. "
         "Only drawn entries are listed unless drawn_only is false. `raw` adds the 64 KiB sprite RAM as hex.",
         schema_object({{"drawn_only", {{"type", "boolean"}, {"default", true}}},
                        {"start_bank", {{"type", "integer"}, {"enum", json::array({0, 1})}, {"default", 0}}},
                        {"start_flipscreen", {{"type", "boolean"}, {"default", false}}},
                        {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", int(SPRITE_WALK_BUDGET)},
                                   {"default", int(SPRITE_WALK_BUDGET)},
                                   {"description", "Most list entries to visit (the renderer's budget is 1024)."}}},
                        {"raw", {{"type", "boolean"}, {"default", false}}}}),
         plain([&](const Args &a) { return runtime.sprites(a); }));

    tool("video_diff",
         "Compare the game renderer's layers with the FDP oracle for the current frame (needs init video_diff:true). "
         "Below `min_frame` (default 600, where the oracle becomes meaningful) the comparison is skipped and the result "
         "has skipped:true, ok:null.",
         schema_object({{"layer_mask", {{"type", "integer"}, {"minimum", 1}, {"maximum", 511}, {"default", 511}}},
                        {"min_frame", {{"type", "integer"}, {"minimum", 0}, {"default", int(VIDEO_DIFF_MIN_FRAME)}}}}),
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
        // The machine (and its audio/EEPROM leases) is torn down on its own thread before the process exits.
        MachineThread machine;
        machine.run([&] {
            auto &runtime = machine.runtime.emplace(std::move(defaults));
            if (eager && runtime.can_init_from_defaults()) runtime.init_from_defaults();
        });
        serve(*machine.runtime, machine, out_fd);
    }
#if !defined(_WIN32)
    ::close(out_fd);
#endif
    return 0;
} catch (const std::exception &e) {
    std::cerr << "mcp: " << e.what() << '\n';
    return 1;
}

} // namespace f3rt::tool
