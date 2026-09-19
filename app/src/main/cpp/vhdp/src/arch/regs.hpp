// Per-architecture tracee register access for syscall stops.
//
// Invariants: a RegsAccess is used only by the tracer thread, only while the
// tracee is in a ptrace stop. Changes are staged and written by commit().
#pragma once

#include <sys/types.h>

#include <array>
#include <cstdint>

namespace vhdp::arch {

struct SyscallView {
    long nr = -1;
    std::array<std::uint64_t, 6> args{};
    long ret = 0;
    std::uint64_t sp = 0;
    std::uint64_t ip = 0;
    bool native_abi = true; // false for compat (i386/x32 on x86_64, AArch32 on arm64)
};

class RegsAccess {
public:
    explicit RegsAccess(pid_t tid) noexcept;

    // Reads registers. Returns 0 or errno (ESRCH when the tracee vanished).
    int fetch() noexcept;
    const SyscallView& view() const noexcept { return view_; }

    void set_arg(int index, std::uint64_t value) noexcept;
    void set_return(long value) noexcept;
    // Replaces the syscall number (-1 skips execution at syscall-enter).
    void set_nr(long nr) noexcept;
    // At the signal-delivery-stop of a syscall a seccomp filter trapped (and rolled back): makes
    // the tracee execute its syscall instruction again, as syscall `nr` with the current
    // arguments, when it resumes.
    void restart_as(long nr) noexcept;
    int commit() noexcept;

private:
    pid_t tid_;
    SyscallView view_;
    bool dirty_regs_ = false;
    bool dirty_nr_ = false;
#if defined(__x86_64__)
    std::array<std::uint64_t, 27> raw_{}; // struct user_regs_struct
#elif defined(__aarch64__)
    std::array<std::uint64_t, 34> raw_{}; // struct user_pt_regs
#else
    std::array<std::uint64_t, 1> raw_{};
#endif
};

// Bytes below the stack pointer the ABI allows leaf functions to use.
std::uint64_t stack_red_zone() noexcept;
// Stack alignment required at a call boundary.
std::uint64_t stack_alignment() noexcept;
// AUDIT_ARCH_* value of the native syscall ABI (for seccomp filters).
std::uint32_t native_audit_arch() noexcept;
const char* arch_name() noexcept;

} // namespace vhdp::arch
