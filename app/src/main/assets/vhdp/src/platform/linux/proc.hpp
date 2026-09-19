// Small, bounded helpers for reading procfs/sysfs and host files.
#pragma once

#include "common/status.hpp"

#include <sys/types.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace vhdp::platform {

// Reads at most max_bytes from path. Fails for unreadable files.
Result<std::string> read_small_file(const std::string& path, std::size_t max_bytes = 64 * 1024);

// readlink(2) into a string (bounded by PATH_MAX).
Result<std::string> read_link(const std::string& path);

// Value of "Key:\tvalue" from /proc/<pid>/status ("self" when pid == 0).
std::optional<std::string> proc_status_field(pid_t pid, const std::string& key);

// Number of entries in /proc/self/fd (excluding the directory fd used to count).
int count_open_fds();

// Direct children of `parent` found by scanning /proc/*/stat (PPid field).
std::vector<pid_t> child_pids(pid_t parent);

// Process state letter from /proc/<pid>/stat, or '\0' when the process is gone.
char process_state(pid_t pid);

// Trims trailing whitespace/newlines.
std::string trim_right(std::string s);

} // namespace vhdp::platform
