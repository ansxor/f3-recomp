#pragma once

// Minimal JSON for the web-player bundle: a streaming writer with byte-for-byte deterministic output (fixed
// member order, shortest round-trip numbers, no locale) and a small parser for reading it back (games.json merge,
// tests). Not a general-purpose library.

#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace f3rt::webplayer::json {

class Writer {
public:
    // pretty: objects and `multiline` arrays put one member/element per line (two-space indent); everything else
    // stays on one line.
    explicit Writer(bool pretty = false) : pretty_(pretty) {}

    Writer &begin_object() { return open('{', true); }
    Writer &end_object() { return close('}'); }
    Writer &begin_array(bool multiline = false) { return open('[', multiline); }
    Writer &end_array() { return close(']'); }

    Writer &key(std::string_view name) {
        prefix();
        string(name);
        out_ += pretty_ ? ": " : ":";
        after_key_ = true;
        return *this;
    }

    Writer &value(std::string_view s) {
        prefix();
        string(s);
        return *this;
    }
    Writer &value(const char *s) { return value(std::string_view(s)); }
    Writer &value(const std::string &s) { return value(std::string_view(s)); }
    Writer &value(bool b) {
        prefix();
        out_ += b ? "true" : "false";
        return *this;
    }
    Writer &null() {
        prefix();
        out_ += "null";
        return *this;
    }
    template <class Int, std::enable_if_t<std::is_integral_v<Int> && !std::is_same_v<Int, bool>, int> = 0>
    Writer &value(Int v) {
        prefix();
        number(v);
        return *this;
    }
    Writer &value(double v) {
        if (!std::isfinite(v)) throw std::runtime_error("JSON cannot hold a non-finite number");
        prefix();
        number(v);
        return *this;
    }
    Writer &value(float v) {
        if (!std::isfinite(v)) throw std::runtime_error("JSON cannot hold a non-finite number");
        prefix();
        number(v);
        return *this;
    }

    template <class T> Writer &member(std::string_view name, const T &v) { return key(name).value(v); }
    Writer &member_null(std::string_view name) { return key(name).null(); }

    // Already valid JSON text, inserted as one value.
    Writer &raw(std::string_view json) {
        prefix();
        out_ += json;
        return *this;
    }

    const std::string &str() const { return out_; }
    std::string take() {
        if (!stack_.empty()) throw std::logic_error("JSON document not closed");
        return std::move(out_);
    }

private:
    struct Level {
        char closer;
        bool multiline;
        bool first = true;
    };

    Writer &open(char c, bool multiline) {
        prefix();
        out_ += c;
        stack_.push_back({c == '{' ? '}' : ']', multiline});
        return *this;
    }
    Writer &close(char c) {
        if (stack_.empty() || stack_.back().closer != c) throw std::logic_error("JSON nesting mismatch");
        const Level level = stack_.back();
        stack_.pop_back();
        if (pretty_ && level.multiline && !level.first) newline();
        out_ += c;
        return *this;
    }
    void newline() {
        out_ += '\n';
        out_.append(stack_.size() * 2, ' ');
    }
    // Before every member name and every value that is not a member's value.
    void prefix() {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        if (stack_.empty()) return;
        Level &level = stack_.back();
        if (!level.first) out_ += ',';
        if (pretty_ && level.multiline) newline();
        else if (pretty_ && !level.first) out_ += ' ';
        level.first = false;
    }
    template <class T> void number(T v) {
        char buf[40];
        const auto r = std::to_chars(buf, buf + sizeof buf, v);
        out_.append(buf, r.ptr);
    }
    void string(std::string_view s) {
        out_ += '"';
        for (const unsigned char c : s) {
            switch (c) {
            case '"': out_ += "\\\""; break;
            case '\\': out_ += "\\\\"; break;
            case '\n': out_ += "\\n"; break;
            case '\r': out_ += "\\r"; break;
            case '\t': out_ += "\\t"; break;
            default:
                if (c < 0x20) {
                    constexpr char HEX[] = "0123456789abcdef";
                    out_ += "\\u00";
                    out_ += HEX[c >> 4];
                    out_ += HEX[c & 15];
                } else {
                    out_ += char(c);
                }
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<Level> stack_;
    bool pretty_;
    bool after_key_ = false;
};

// ---- reading -----------------------------------------------------------------------------------------------------

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>; // source order kept

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }

    bool as_bool() const { expect(Type::Bool); return bool_; }
    double as_number() const { expect(Type::Number); return number_; }
    int64_t as_int() const { return int64_t(as_number()); }
    const std::string &as_string() const { expect(Type::String); return string_; }
    const Array &as_array() const { expect(Type::Array); return array_; }
    const Object &as_object() const { expect(Type::Object); return object_; }

    // Object member or nullptr.
    const Value *find(std::string_view name) const {
        if (type_ != Type::Object) return nullptr;
        for (const auto &[k, v] : object_)
            if (k == name) return &v;
        return nullptr;
    }
    const Value &at(std::string_view name) const {
        const Value *v = find(name);
        if (!v) throw std::runtime_error("JSON member missing: " + std::string(name));
        return *v;
    }

    // Throws std::runtime_error on malformed input or trailing data.
    static Value parse(std::string_view text);

    // Re-serializes with the Writer; arrays of objects/arrays are multiline when pretty.
    std::string dump(bool pretty = false) const;
    void write(Writer &w) const;

private:
    friend class Parser;
    void expect(Type t) const {
        if (type_ != t) throw std::runtime_error("JSON type mismatch");
    }
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    Array array_;
    Object object_;
};

} // namespace f3rt::webplayer::json
