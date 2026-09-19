// Bounds-checked access to tracee memory.
//
// All sizes are bounded by the caller; reads never allocate more than `max`.
// Errors use Linux errno values: EFAULT for unmapped/unreadable addresses,
// ENAMETOOLONG when a string has no terminating NUL within `max` bytes, ESRCH
// when the tracee vanished.
#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace vhdp::rootless {

int read_mem(pid_t tid, std::uint64_t addr, void* buf, std::size_t len) noexcept;
int write_mem(pid_t tid, std::uint64_t addr, const void* buf, std::size_t len) noexcept;
int read_cstring(pid_t tid, std::uint64_t addr, std::size_t max, std::string& out);

} // namespace vhdp::rootless
