#pragma once

#include "cli_args.hpp"

#include <string>

namespace phdp {

inline constexpr int kExitUsage = 2;
inline constexpr int kExitStartFailure = 125;

int run_command(const RunOptions& options);
int print_doctor(const std::string& json_text, bool as_json);
int print_capabilities(const std::string& json_text, bool as_json);
int print_inspect(const std::string& json_text, bool as_json);

// Writes all bytes, retrying on EINTR/partial writes. Returns false on error.
bool write_all(int fd, const char* data, std::size_t len) noexcept;

} // namespace phdp
