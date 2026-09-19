#include "common/json.hpp"

#include <cstddef>

namespace vhdp {

namespace {

constexpr const char* kHex = "0123456789abcdef";

// Returns the length of the valid UTF-8 sequence starting at s[i], or 0.
std::size_t utf8_seq_len(std::string_view s, std::size_t i) noexcept {
    auto b = static_cast<unsigned char>(s[i]);
    std::size_t n = 0;
    std::uint32_t cp = 0;
    if (b < 0x80) {
        return 1;
    }
    if ((b & 0xE0) == 0xC0) {
        n = 2;
        cp = b & 0x1Fu;
    } else if ((b & 0xF0) == 0xE0) {
        n = 3;
        cp = b & 0x0Fu;
    } else if ((b & 0xF8) == 0xF0) {
        n = 4;
        cp = b & 0x07u;
    } else {
        return 0;
    }
    if (i + n > s.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < n; ++k) {
        auto c = static_cast<unsigned char>(s[i + k]);
        if ((c & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (c & 0x3Fu);
    }
    // Reject overlong encodings, surrogates and out-of-range code points.
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return n;
}

} // namespace

void json_escape_into(std::string& out, std::string_view s) {
    out.push_back('"');
    std::size_t i = 0;
    while (i < s.size()) {
        auto c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':
                out += "\\\"";
                ++i;
                continue;
            case '\\':
                out += "\\\\";
                ++i;
                continue;
            case '\n':
                out += "\\n";
                ++i;
                continue;
            case '\r':
                out += "\\r";
                ++i;
                continue;
            case '\t':
                out += "\\t";
                ++i;
                continue;
            case '\b':
                out += "\\b";
                ++i;
                continue;
            case '\f':
                out += "\\f";
                ++i;
                continue;
            default:
                break;
        }
        if (c < 0x20) {
            out += "\\u00";
            out.push_back(kHex[(c >> 4) & 0xF]);
            out.push_back(kHex[c & 0xF]);
            ++i;
            continue;
        }
        std::size_t n = utf8_seq_len(s, i);
        if (n == 0) {
            out += "\\ufffd";
            ++i;
            continue;
        }
        out.append(s.substr(i, n));
        i += n;
    }
    out.push_back('"');
}

std::string json_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    json_escape_into(out, s);
    return out;
}

void JsonWriter::before_value() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (!first_.empty()) {
        if (!first_.back()) {
            out_.push_back(',');
        }
        first_.back() = false;
    }
}

JsonWriter& JsonWriter::begin_object() {
    before_value();
    out_.push_back('{');
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_object() {
    out_.push_back('}');
    if (!first_.empty()) {
        first_.pop_back();
    }
    return *this;
}

JsonWriter& JsonWriter::begin_array() {
    before_value();
    out_.push_back('[');
    first_.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::end_array() {
    out_.push_back(']');
    if (!first_.empty()) {
        first_.pop_back();
    }
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view k) {
    before_value();
    json_escape_into(out_, k);
    out_.push_back(':');
    after_key_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view v) {
    before_value();
    json_escape_into(out_, v);
    return *this;
}

JsonWriter& JsonWriter::value(std::int64_t v) {
    before_value();
    out_ += std::to_string(v);
    return *this;
}

JsonWriter& JsonWriter::value(std::uint64_t v) {
    before_value();
    out_ += std::to_string(v);
    return *this;
}

JsonWriter& JsonWriter::value(bool v) {
    before_value();
    out_ += v ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::null() {
    before_value();
    out_ += "null";
    return *this;
}

JsonWriter& JsonWriter::raw(std::string_view json) {
    before_value();
    out_.append(json);
    return *this;
}

namespace {

class Validator {
public:
    explicit Validator(std::string_view t) : t_(t) {}

    bool run() {
        ws();
        if (!value(0)) {
            return false;
        }
        ws();
        return pos_ == t_.size();
    }

private:
    bool value(int depth) {
        if (depth > 256 || pos_ >= t_.size()) {
            return false;
        }
        char c = t_[pos_];
        if (c == '{') {
            return object(depth + 1);
        }
        if (c == '[') {
            return array(depth + 1);
        }
        if (c == '"') {
            return string();
        }
        if (c == 't') {
            return lit("true");
        }
        if (c == 'f') {
            return lit("false");
        }
        if (c == 'n') {
            return lit("null");
        }
        return number();
    }

    bool object(int depth) {
        ++pos_;
        ws();
        if (peek('}')) {
            ++pos_;
            return true;
        }
        for (;;) {
            ws();
            if (!peek('"') || !string()) {
                return false;
            }
            ws();
            if (!peek(':')) {
                return false;
            }
            ++pos_;
            ws();
            if (!value(depth)) {
                return false;
            }
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

    bool array(int depth) {
        ++pos_;
        ws();
        if (peek(']')) {
            ++pos_;
            return true;
        }
        for (;;) {
            ws();
            if (!value(depth)) {
                return false;
            }
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

    bool string() {
        ++pos_;
        while (pos_ < t_.size()) {
            auto c = static_cast<unsigned char>(t_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) {
                return false;
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= t_.size()) {
                    return false;
                }
                char e = t_[pos_];
                if (e == 'u') {
                    for (int k = 0; k < 4; ++k) {
                        ++pos_;
                        if (pos_ >= t_.size() || !is_hex(t_[pos_])) {
                            return false;
                        }
                    }
                    ++pos_;
                    continue;
                }
                if (e != '"' && e != '\\' && e != '/' && e != 'b' && e != 'f' && e != 'n' &&
                    e != 'r' && e != 't') {
                    return false;
                }
                ++pos_;
                continue;
            }
            std::size_t n = utf8_seq_len(t_, pos_);
            if (n == 0) {
                return false;
            }
            pos_ += n;
        }
        return false;
    }

    bool number() {
        std::size_t start = pos_;
        if (peek('-')) {
            ++pos_;
        }
        if (peek('0')) {
            ++pos_;
        } else if (digit()) {
            while (digit()) {
            }
        } else {
            return false;
        }
        if (peek('.')) {
            ++pos_;
            if (!digit()) {
                return false;
            }
            while (digit()) {
            }
        }
        if (peek('e') || peek('E')) {
            ++pos_;
            if (peek('+') || peek('-')) {
                ++pos_;
            }
            if (!digit()) {
                return false;
            }
            while (digit()) {
            }
        }
        return pos_ > start;
    }

    bool digit() {
        if (pos_ < t_.size() && t_[pos_] >= '0' && t_[pos_] <= '9') {
            ++pos_;
            return true;
        }
        return false;
    }
    static bool is_hex(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    bool lit(std::string_view l) {
        if (t_.substr(pos_, l.size()) != l) {
            return false;
        }
        pos_ += l.size();
        return true;
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

bool json_validate(std::string_view text) noexcept {
    Validator v(text);
    return v.run();
}

} // namespace vhdp
