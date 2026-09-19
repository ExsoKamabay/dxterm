#include "common/strings.hpp"

#include <limits>

namespace vhdp {

std::optional<std::uint64_t> parse_u64(std::string_view s) noexcept {
    if (s.empty() || s.size() > 20) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        auto d = static_cast<std::uint64_t>(c - '0');
        if (v > (std::numeric_limits<std::uint64_t>::max() - d) / 10) {
            return std::nullopt;
        }
        v = v * 10 + d;
    }
    return v;
}

std::optional<std::uint32_t> parse_u32(std::string_view s) noexcept {
    auto v = parse_u64(s);
    if (!v || *v > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*v);
}

std::optional<std::int32_t> parse_i32(std::string_view s) noexcept {
    bool neg = false;
    if (!s.empty() && s.front() == '-') {
        neg = true;
        s.remove_prefix(1);
    }
    auto v = parse_u64(s);
    if (!v) {
        return std::nullopt;
    }
    auto limit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + (neg ? 1u : 0u);
    if (*v > limit) {
        return std::nullopt;
    }
    if (neg) {
        return static_cast<std::int32_t>(-static_cast<std::int64_t>(*v));
    }
    return static_cast<std::int32_t>(*v);
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;) {
        std::size_t p = s.find(sep, start);
        if (p == std::string_view::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
}

bool contains_nul(std::string_view s) noexcept {
    return s.find('\0') != std::string_view::npos;
}

} // namespace vhdp
