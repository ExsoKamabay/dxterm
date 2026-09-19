// seccomp-bpf acceleration for the rootless engine.
//
// The filter is an *accelerator*, never the policy of record: every syscall it
// does not explicitly allow returns SECCOMP_RET_TRACE and is decided by the
// ptrace supervisor using the syscall table. Unknown numbers, foreign
// architectures and x32 syscalls are always traced (and then denied).
#pragma once

#include <linux/filter.h>

#include <cstdint>
#include <vector>

namespace vhdp::rootless {

// SECCOMP_RET_DATA marker so the supervisor can tell its own trace events from
// RET_TRACE results of filters installed by the guest.
inline constexpr std::uint16_t kSeccompTraceMarker = 0x7648;

struct FilterPolicy {
    std::uint32_t audit_arch = 0;
    bool check_x32 = false;    // x86_64: trace syscalls with __X32_SYSCALL_BIT
    std::vector<long> allow;   // numbers executed without a stop
    bool network_none = false; // socket(): allow AF_UNIX only, EACCES otherwise
    long nr_socket = -1;
    long nr_clone = -1;  // trace clone() only when CLONE_UNTRACED is set
    long nr_sendto = -1; // trace sendto() only with a destination address
    long nr_ioctl = -1;  // with ioctl_trace: trace ioctl() only for these requests
    std::vector<std::uint32_t> ioctl_trace;
};

std::vector<sock_filter> build_seccomp_filter(const FilterPolicy& policy);

struct FilterInput {
    int nr = 0;
    std::uint32_t arch = 0;
    std::uint64_t args[6] = {};
};

// Reference interpreter for the classic-BPF subset emitted by build_seccomp_filter.
// Returns the SECCOMP_RET_* action value, or 0 (KILL) for malformed programs.
std::uint32_t evaluate_seccomp_filter(const std::vector<sock_filter>& prog, const FilterInput& in);

} // namespace vhdp::rootless
