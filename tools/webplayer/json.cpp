#include "json.hpp"

#include <cstdlib>

namespace f3rt::webplayer::json {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Value parse_document() {
        Value v = parse_value();
        skip();
        if (at_ != text_.size()) fail("trailing data");
        return v;
    }

private:
    [[noreturn]] void fail(const char *what) const {
        throw std::runtime_error(std::string("JSON parse error at byte ") + std::to_string(at_) + ": " + what);
    }
    void skip() {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\n' || text_[at_] == '\r' || text_[at_] == '\t'))
            ++at_;
    }
    char peek() {
        skip();
        if (at_ >= text_.size()) fail("unexpected end");
        return text_[at_];
    }
    void expect(char c) {
        if (peek() != c) fail("unexpected character");
        ++at_;
    }
    bool literal(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) return false;
        at_ += word.size();
        return true;
    }

    Value parse_value() {
        Value v;
        const char c = peek();
        if (c == '{') {
            ++at_;
            v.type_ = Value::Type::Object;
            if (peek() == '}') {
                ++at_;
                return v;
            }
            for (;;) {
                if (peek() != '"') fail("object key expected");
                std::string name = parse_string();
                expect(':');
                v.object_.emplace_back(std::move(name), parse_value());
                if (peek() == ',') {
                    ++at_;
                    continue;
                }
                expect('}');
                return v;
            }
        }
        if (c == '[') {
            ++at_;
            v.type_ = Value::Type::Array;
            if (peek() == ']') {
                ++at_;
                return v;
            }
            for (;;) {
                v.array_.push_back(parse_value());
                if (peek() == ',') {
                    ++at_;
                    continue;
                }
                expect(']');
                return v;
            }
        }
        if (c == '"') {
            v.type_ = Value::Type::String;
            v.string_ = parse_string();
            return v;
        }
        if (literal("true")) {
            v.type_ = Value::Type::Bool;
            v.bool_ = true;
            return v;
        }
        if (literal("false")) {
            v.type_ = Value::Type::Bool;
            return v;
        }
        if (literal("null")) return v;
        v.type_ = Value::Type::Number;
        const size_t start = at_;
        while (at_ < text_.size() && std::string_view("+-0123456789.eE").find(text_[at_]) != std::string_view::npos)
            ++at_;
        if (at_ == start) fail("value expected");
        const auto r = std::from_chars(text_.data() + start, text_.data() + at_, v.number_);
        if (r.ec != std::errc() || r.ptr != text_.data() + at_) fail("bad number");
        return v;
    }

    std::string parse_string() {
        expect('"');
        std::string s;
        for (;;) {
            if (at_ >= text_.size()) fail("unterminated string");
            const char c = text_[at_++];
            if (c == '"') return s;
            if (c != '\\') {
                s += c;
                continue;
            }
            if (at_ >= text_.size()) fail("unterminated escape");
            const char e = text_[at_++];
            switch (e) {
            case '"': s += '"'; break;
            case '\\': s += '\\'; break;
            case '/': s += '/'; break;
            case 'n': s += '\n'; break;
            case 'r': s += '\r'; break;
            case 't': s += '\t'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'u': {
                if (at_ + 4 > text_.size()) fail("short \\u escape");
                unsigned code = 0;
                const auto r = std::from_chars(text_.data() + at_, text_.data() + at_ + 4, code, 16);
                if (r.ec != std::errc() || r.ptr != text_.data() + at_ + 4) fail("bad \\u escape");
                at_ += 4;
                if (code >= 0x80) fail("non-ASCII \\u escape is not supported");
                s += char(code);
                break;
            }
            default: fail("bad escape");
            }
        }
    }

    std::string_view text_;
    size_t at_ = 0;
};

Value Value::parse(std::string_view text) { return Parser(text).parse_document(); }

void Value::write(Writer &w) const {
    switch (type_) {
    case Type::Null: w.null(); break;
    case Type::Bool: w.value(bool_); break;
    case Type::Number: w.value(number_); break;
    case Type::String: w.value(string_); break;
    case Type::Array: {
        bool containers = false;
        for (const Value &v : array_) containers = containers || v.type_ == Type::Object || v.type_ == Type::Array;
        w.begin_array(containers);
        for (const Value &v : array_) v.write(w);
        w.end_array();
        break;
    }
    case Type::Object:
        w.begin_object();
        for (const auto &[k, v] : object_) {
            w.key(k);
            v.write(w);
        }
        w.end_object();
        break;
    }
}

std::string Value::dump(bool pretty) const {
    Writer w(pretty);
    write(w);
    return w.take();
}

} // namespace f3rt::webplayer::json
