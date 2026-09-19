// Lexical helpers for guest paths. These never touch the filesystem; symlink-aware
// resolution lives in resolver.hpp.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vhdp::vfs {

inline constexpr std::size_t kPathMax = 4096; // including the terminating NUL

bool is_absolute(std::string_view p) noexcept;

// Components of p, skipping empty components and "."; ".." is kept.
std::vector<std::string_view> components(std::string_view p);

// True when p contains a ".." component.
bool has_dotdot(std::string_view p);

// Lexically normalises an absolute path: collapses "//" and "." and applies ".."
// (clamped at "/"). Only valid when symlinks are irrelevant (e.g. config input
// already rejected for "..").
std::string normalize_lexical(std::string_view abs);

// base (absolute) + rel. If rel is absolute it is returned as-is. No normalisation.
std::string join(std::string_view base, std::string_view rel);

// "/a/b" is below "/a" and "/a/b"; not below "/ab". prefix "/" matches everything.
bool is_below(std::string_view path, std::string_view prefix) noexcept;

// Suffix of path after prefix (starting with '/' or empty). Requires is_below.
std::string_view suffix_after(std::string_view path, std::string_view prefix) noexcept;

} // namespace vhdp::vfs
