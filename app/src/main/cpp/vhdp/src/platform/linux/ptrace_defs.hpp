// ptrace constants and a libc-independent call wrapper.
//
// glibc declares ptrace requests as an enum (no macros) while bionic uses the UAPI
// macros, so the values are defined here once from the kernel UAPI
// (include/uapi/linux/ptrace.h). All calls go through the raw syscall, which has
// identical semantics for the requests used here (no PEEK* requests are used).
#pragma once

#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>

namespace vhdp::platform {

inline constexpr long kPtraceGetEventMsg = 0x4201;
inline constexpr long kPtraceGetSigInfo = 0x4202;
inline constexpr long kPtraceSeize = 0x4206;
inline constexpr long kPtraceInterrupt = 0x4207;
inline constexpr long kPtraceListen = 0x4208;
inline constexpr long kPtraceGetSyscallInfo = 0x420e;
inline constexpr long kPtraceCont = 7;
inline constexpr long kPtraceSyscall = 24;

inline constexpr unsigned long kOptTraceSysGood = 0x01;
inline constexpr unsigned long kOptTraceFork = 0x02;
inline constexpr unsigned long kOptTraceVfork = 0x04;
inline constexpr unsigned long kOptTraceClone = 0x08;
inline constexpr unsigned long kOptTraceExec = 0x10;
inline constexpr unsigned long kOptTraceSeccomp = 0x80;
inline constexpr unsigned long kOptExitKill = 0x100000;

inline constexpr int kEventFork = 1;
inline constexpr int kEventVfork = 2;
inline constexpr int kEventClone = 3;
inline constexpr int kEventExec = 4;
inline constexpr int kEventSeccomp = 7;
inline constexpr int kEventStop = 128;

inline constexpr std::uint8_t kSyscallInfoNone = 0;
inline constexpr std::uint8_t kSyscallInfoEntry = 1;
inline constexpr std::uint8_t kSyscallInfoExit = 2;
inline constexpr std::uint8_t kSyscallInfoSeccomp = 3;

// Mirror of struct ptrace_syscall_info (UAPI, Linux >= 5.3).
struct PtraceSyscallEntry {
    std::uint64_t nr;
    std::uint64_t args[6];
};
struct PtraceSyscallExit {
    std::int64_t rval;
    std::uint8_t is_error;
};
struct PtraceSyscallSeccomp {
    std::uint64_t nr;
    std::uint64_t args[6];
    std::uint32_t ret_data;
};
union PtraceSyscallData {
    PtraceSyscallEntry entry;
    PtraceSyscallExit exit;
    PtraceSyscallSeccomp seccomp;
};
struct PtraceSyscallInfo {
    std::uint8_t op;
    std::uint8_t pad[3];
    std::uint32_t arch;
    std::uint64_t instruction_pointer;
    std::uint64_t stack_pointer;
    PtraceSyscallData data;
};

inline long ptrace_call(long request, pid_t pid, std::uintptr_t addr,
                        std::uintptr_t data) noexcept {
    return ::syscall(SYS_ptrace, request, static_cast<long>(pid), addr, data);
}

} // namespace vhdp::platform
