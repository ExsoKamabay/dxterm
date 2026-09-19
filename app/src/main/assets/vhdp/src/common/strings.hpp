#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vhdp {

// Decimal parsing with overflow detection; rejects sign, whitespace and empty input.
std::optional<std::uint64_t> parse_u64(std::string_view s) noexcept;
std::optional<std::uint32_t> parse_u32(std::string_view s) noexcept;
std::optional<std::int32_t> parse_i32(std::string_view s) noexcept;

std::vector<std::string_view> split(std::string_view s, char sep);
bool contains_nul(std::string_view s) noexcept;

} // namespace vhdp
