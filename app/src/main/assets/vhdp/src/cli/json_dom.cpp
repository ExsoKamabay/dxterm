#include "json_dom.hpp"

#include <cstdlib>

namespace phdp::json {

const Value* Value::get(std::string_view key) const {
    if (type != Type::object) {
        return nullptr;
    }
    for (std::size_t i = 0; i < keys.size() && i < values.size(); ++i) {
        if (keys[i] == key) {
            return &values[i];
        }
    }
    return nullptr;
}

std::string Value::str(std::string_view key, std::string_view fallback) const {
    const Value* v = get(key);
    return v != nullptr && v->type == Type::string ? v->string : std::string(fallback);
}

bool Value::flag(std::string_view key, bool fallback) const {
    const Value* v = get(key);
    return v != nullptr && v->type == Type::boolean ? v->boolean : fallback;
}

double Value::num(std::string_view key, double fallback) const {
    const Value* v = get(key);
    return v != nullptr && v->type == Type::number ? v->number : fallback;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}

    std::optional<Value> run() {
        Value v;
        ws();
        if (!value(v, 0)) {
            return std::nullopt;
        }
        ws();
        if (pos_ != t_.size()) {
            return std::nullopt;
        }
        return v;
    }

private:
    bool value(Value& out, int depth) {
        if (depth > 128 || pos_ >= t_.size()) {
            return false;
        }
        char c = t_[pos_];
        if (c == '{') {
            return object(out, depth + 1);
        }
        if (c == '[') {
            return array(out, depth + 1);
        }
        if (c == '"') {
            out.type = Value::Type::string;
            return string(out.string);
        }
        if (literal("true")) {
            out.type = Value::Type::boolean;
            out.boolean = true;
            return true;
        }
        if (literal("false")) {
            out.type = Value::Type::boolean;
            return true;
        }
        if (literal("null")) {
            out.type = Value::Type::null;
            return true;
        }
        return number(out);
    }

    bool object(Value& out, int depth) {
        out.type = Value::Type::object;
        ++pos_;
        ws();
        if (peek('}')) {
            ++pos_;
            return true;
        }
        for (;;) {
            ws();
            std::string key;
            if (!peek('"') || !string(key)) {
                return false;
            }
            ws();
            if (!peek(':')) {
                return false;
            }
            ++pos_;
            ws();
            Value v;
            if (!value(v, depth)) {
                return false;
            }
            out.keys.push_back(std::move(key));
            out.values.push_back(std::move(v));
            ws();
            if (peek(',')) {
                ++pos_;
                continue;
            }
            if (peek('}')) {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool array(Value& out, int depth) {
        out.type = Value::Type::array;
        ++pos_;
        ws();
        if (peek(']')) {
            ++pos_;
            return true;
        }
        for (;;) {
            ws();
            Value v;
            if (!value(v, depth)) {
                return false;
            }
            out.array.push_back(std::move(v));
            ws();
            if (peek(',')) {
                ++pos_;
                continue;
            }
            if (peek(']')) {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    static void put_utf8(std::string& s, unsigned cp) {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(unsigned& out) {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ >= t_.size()) {
                return false;
            }
            char c = t_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool string(std::string& out) {
        ++pos_;
        while (pos_ < t_.size()) {
            char c = t_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= t_.size()) {
                return false;
            }
            char e = t_[pos_++];
            switch (e) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < t_.size() && t_[pos_] == '\\' &&
                        t_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned lo = 0;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) {
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put_utf8(out, cp);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool number(Value& out) {
        std::size_t start = pos_;
        while (pos_ < t_.size() &&
               (t_[pos_] == '-' || t_[pos_] == '+' || t_[pos_] == '.' || t_[pos_] == 'e' ||
                t_[pos_] == 'E' || (t_[pos_] >= '0' && t_[pos_] <= '9'))) {
            ++pos_;
        }
        if (pos_ == start) {
            return false;
        }
        std::string tmp(t_.substr(start, pos_ - start));
        char* end = nullptr;
        out.type = Value::Type::number;
        out.number = std::strtod(tmp.c_str(), &end);
        return end == tmp.c_str() + tmp.size();
    }

    bool literal(std::string_view l) {
        if (t_.substr(pos_, l.size()) == l) {
            pos_ += l.size();
            return true;
        }
        return false;
    }
    bool peek(char c) const { return pos_ < t_.size() && t_[pos_] == c; }
    void ws() {
        while (pos_ < t_.size() &&
               (t_[pos_] == ' ' || t_[pos_] == '\n' || t_[pos_] == '\r' || t_[pos_] == '\t')) {
            ++pos_;
        }
    }

    std::string_view t_;
    std::size_t pos_ = 0;
};

} // namespace

std::optional<Value> parse(std::string_view text) {
    return Parser(text).run();
}

std::string quote(std::string_view s) {
    static constexpr const char* hex = "0123456789abcdef";
    std::string out = "\"";
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
    return out;
}

} // namespace phdp::json
