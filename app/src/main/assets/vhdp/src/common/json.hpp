#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vhdp {

// Streaming JSON writer producing RFC 8259 text. Strings are escaped; bytes that
// are not valid UTF-8 (e.g. arbitrary host path bytes) are emitted as U+FFFD so
// the output is always valid UTF-8 JSON.
class JsonWriter {
public:
    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(std::string_view k);
    JsonWriter& value(std::string_view v);
    JsonWriter& value(const char* v) { return value(std::string_view(v)); }
    JsonWriter& value(std::int64_t v);
    JsonWriter& value(std::uint64_t v);
    JsonWriter& value(int v) { return value(static_cast<std::int64_t>(v)); }
    JsonWriter& value(unsigned v) { return value(static_cast<std::uint64_t>(v)); }
    JsonWriter& value(bool v);
    JsonWriter& null();
    // Inserts an already-serialised JSON value verbatim (caller guarantees validity).
    JsonWriter& raw(std::string_view json);

    const std::string& str() const noexcept { return out_; }
    std::string take() && { return std::move(out_); }

private:
    void before_value();
    std::string out_;
    std::vector<bool> first_; // per open container: next element is the first
    bool after_key_ = false;
};

void json_escape_into(std::string& out, std::string_view s);
std::string json_quote(std::string_view s);

// Strict validator (RFC 8259, single top-level value, max depth 256).
bool json_validate(std::string_view text) noexcept;

} // namespace vhdp
