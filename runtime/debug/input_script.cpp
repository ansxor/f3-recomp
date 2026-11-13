#include "debug/input_script.hpp"
#include "f3rt/memory_map.hpp"
#include "f3rt/machine.hpp"

#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace f3rt {
namespace {
constexpr std::array<std::string_view, LOCAL_CONTROL_COUNT> CONTROL_NAMES = {
    "up",    "down", "left",    "right", "b1", "b2", "b3",
    "start", "coin", "service", "test",  "b4", "b5", "b6"};

uint64_t number(std::string_view text, const std::string &where) {
    uint64_t value = 0;
    const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    if (hex) text.remove_prefix(2);
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value, hex ? 16 : 10);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty())
        throw std::runtime_error(where + ": expected a number, got '" + std::string(text) + "'");
    return value;
}

std::vector<unsigned> controls(std::string_view text, const std::string &where) {
    std::vector<unsigned> result;
    while (!text.empty()) {
        const size_t plus = text.find('+');
        const std::string_view name = text.substr(0, plus);
        unsigned index = 0;
        while (index < LOCAL_CONTROL_COUNT && CONTROL_NAMES[index] != name) ++index;
        if (index == LOCAL_CONTROL_COUNT)
            throw std::runtime_error(
                where + ": unknown control '" + std::string(name) +
                "' (up down left right b1 b2 b3 b4 b5 b6 start coin service test)");
        result.push_back(index);
        text = plus == std::string_view::npos ? std::string_view{} : text.substr(plus + 1);
    }
    return result;
}
} // namespace

InputScript InputScript::parse(std::string_view text, const std::string &source) {
    InputScript script;
    std::istringstream lines{std::string(text)};
    std::string raw;
    for (unsigned line_number = 1; std::getline(lines, raw); ++line_number) {
        const std::string where = source + ":" + std::to_string(line_number);
        std::istringstream words(raw.substr(0, raw.find('#')));
        std::vector<std::string> tokens;
        for (std::string token; words >> token;) tokens.push_back(token);
        if (tokens.empty()) continue;
        Rule rule;
        const std::string_view frames = tokens[0];
        const size_t split = frames.find_first_of("-+");
        rule.begin = number(frames.substr(0, split), where);
        if (split == std::string_view::npos) rule.end = rule.begin;
        else if (frames.substr(split) == "-end") rule.end = UINT64_MAX;
        else if (frames[split] == '-') rule.end = number(frames.substr(split + 1), where);
        else {
            const uint64_t count = number(frames.substr(split + 1), where);
            if (!count) throw std::runtime_error(where + ": +COUNT must be positive");
            rule.end = rule.begin + count - 1;
        }
        if (!rule.begin || rule.end < rule.begin)
            throw std::runtime_error(where + ": frames are 1-based and END must not precede FRAME");
        size_t next = 1;
        if (next < tokens.size() && tokens[next].size() == 2 && tokens[next][0] == 'p') {
            const char digit = tokens[next][1];
            if (digit < '1' || digit > char('0' + LOCAL_PLAYER_COUNT))
                throw std::runtime_error(where + ": player must be p1..p" +
                                         std::to_string(LOCAL_PLAYER_COUNT));
            rule.player = unsigned(digit - '1');
            ++next;
        }
        if (next >= tokens.size())
            throw std::runtime_error(where + ": missing controls, 'mash' or 'poke'");
        if (tokens[next] == "poke") {
            if (next != 1) throw std::runtime_error(where + ": poke takes no player");
            if (next + 2 != tokens.size())
                throw std::runtime_error(where + ": expected 'poke ADDR.b|.w|.l=VALUE'");
            const std::string_view spec = tokens[next + 1];
            const size_t dot = spec.find('.'), equals = spec.find('=');
            if (dot == std::string_view::npos || equals != dot + 2)
                throw std::runtime_error(
                    where + ": expected 'poke ADDR.b|.w|.l=VALUE', e.g. poke 0x401f54.w=10");
            const char size = spec[dot + 1];
            rule.poke_size = size == 'b' ? 1 : size == 'w' ? 2 : size == 'l' ? 4 : 0;
            if (!rule.poke_size)
                throw std::runtime_error(where + ": poke size must be .b, .w or .l");
            const uint64_t address = number(spec.substr(0, dot), where);
            const uint64_t value = number(spec.substr(equals + 1), where);
            if (address < RAM_BASE || address + rule.poke_size > RAM_PHYSICAL_END)
                throw std::runtime_error(
                    where + ": poke address must be main RAM 0x400000..0x41ffff "
                            "(a5 globals: f3a xref a5-0xNNNN prints the absolute address)");
            if (rule.poke_size < 4 && value >> (8 * rule.poke_size))
                throw std::runtime_error(where + ": poke value does not fit ." +
                                         std::string(1, size));
            if (value > UINT32_MAX)
                throw std::runtime_error(where + ": poke value does not fit .l");
            rule.poke_address = uint32_t(address);
            rule.poke_value = uint32_t(value);
        } else if (tokens[next] == "mash") {
            rule.mash = true;
            for (unsigned key = 0; key < 7; ++key) rule.keys.push_back(key);
            for (++next; next < tokens.size(); ++next) {
                const std::string_view option = tokens[next];
                const size_t equals = option.find('=');
                const std::string_view key = option.substr(0, equals);
                const std::string_view value =
                    equals == std::string_view::npos ? "" : option.substr(equals + 1);
                if (key == "seed") rule.seed = number(value, where);
                else if (key == "period") rule.period = unsigned(number(value, where));
                else if (key == "keys") rule.keys = controls(value, where);
                else
                    throw std::runtime_error(where + ": unknown mash option '" +
                                             std::string(option) + "' (seed=N period=N keys=A+B)");
            }
            if (!rule.period || rule.keys.empty())
                throw std::runtime_error(where +
                                         ": mash needs a positive period and at least one key");
            rule.rng = rule.seed;
            rule.next = rule.begin;
        } else {
            if (next + 1 != tokens.size())
                throw std::runtime_error(where + ": join controls with '+', e.g. right+b1");
            for (const unsigned control : controls(tokens[next], where))
                rule.hold |= LocalInputWord(1u << control);
        }
        script.m_rules.push_back(std::move(rule));
    }
    return script;
}

InputScript InputScript::load(const std::string &path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot read input script " + path);
    std::ostringstream text;
    text << file.rdbuf();
    return parse(text.str(), path);
}

std::array<LocalInputWord, LOCAL_PLAYER_COUNT> InputScript::words(uint64_t frame) {
    std::array<LocalInputWord, LOCAL_PLAYER_COUNT> result{};
    for (Rule &rule : m_rules) {
        if (frame < rule.begin || frame > rule.end) continue;
        if (!rule.mash) {
            result[rule.player] |= rule.hold;
            continue;
        }
        if (frame < rule.next) {
            rule.rng = rule.seed;
            rule.held = 0;
            rule.next = rule.begin;
        }
        // Step on begin, begin+period, ... up to `frame`, like GameplaySchedule's f % period == 0 steps.
        uint64_t step =
            rule.begin + (rule.next - rule.begin + rule.period - 1) / rule.period * rule.period;
        for (; step <= frame; step += rule.period) {
            rule.rng = rule.rng * 6364136223846793005ULL + 1442695040888963407ULL;
            const unsigned key = rule.keys[(rule.rng >> 33) % rule.keys.size()];
            if ((rule.rng >> 20) & 1) rule.held |= LocalInputWord(1u << key);
            else rule.held &= LocalInputWord(~(1u << key));
        }
        rule.next = frame + 1;
        result[rule.player] |= rule.held;
    }
    return result;
}

void InputScript::poke(Machine &machine, uint64_t frame) const {
    for (const Rule &rule : m_rules) {
        if (!rule.poke_size || frame < rule.begin || frame > rule.end) continue;
        for (unsigned i = 0; i < rule.poke_size; ++i)
            machine.ram[rule.poke_address - RAM_BASE + i] =
                uint8_t(rule.poke_value >> (8 * (rule.poke_size - 1 - i)));
    }
}
bool InputScript::has_pokes() const {
    for (const auto &rule : m_rules) {
        if (rule.poke_size > 0) return true;
    }
    return false;
}
} // namespace f3rt
