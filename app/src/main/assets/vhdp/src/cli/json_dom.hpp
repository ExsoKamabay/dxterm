// Minimal JSON DOM used by phdp to render human-readable reports from the JSON
// documents returned by the C ABI (the CLI never includes libvhdp internals).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace phdp::json {

struct Value {
    enum class Type { null, boolean, number, string, array, object };
    Type type = Type::null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    // Object members as parallel arrays (std::vector permits the incomplete Value).
    std::vector<std::string> keys;
    std::vector<Value> values;

    const Value* get(std::string_view key) const;
    std::string str(std::string_view key, std::string_view fallback = "") const;
    bool flag(std::string_view key, bool fallback = false) const;
    double num(std::string_view key, double fallback = 0) const;
};

std::optional<Value> parse(std::string_view text);

// JSON string literal (with quotes) for `s`.
std::string quote(std::string_view s);

} // namespace phdp::json
