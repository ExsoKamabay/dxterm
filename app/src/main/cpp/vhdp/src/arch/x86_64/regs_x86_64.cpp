// x86_64 register access via PTRACE_GETREGS/PTRACE_SETREGS (struct user_regs_struct).
#include "arch/regs.hpp"

#include <asm/unistd.h>
#include <linux/audit.h>
#include <sys/ptrace.h>
#include <sys/user.h>

#include <cerrno>
#include <cstring>

namespace vhdp::arch {

namespace {
constexpr std::uint64_t kX32SyscallBit = 0x40000000ull;
constexpr std::uint64_t kUser64Cs = 0x33;
} // namespace

RegsAccess::RegsAccess(pid_t tid) noexcept : tid_(tid) {}

int RegsAccess::fetch() noexcept {
    user_regs_struct r{};
    if (::ptrace(PTRACE_GETREGS, tid_, nullptr, &r) != 0) {
        return errno;
    }
    std::memcpy(raw_.data(), &r, sizeof(r));
    view_.nr = static_cast<long>(r.orig_rax);
    view_.args = {r.rdi, r.rsi, r.rdx, r.r10, r.r8, r.r9};
    view_.ret = static_cast<long>(r.rax);
    view_.sp = r.rsp;
    view_.ip = r.rip;
    view_.native_abi = r.cs == kUser64Cs && (r.orig_rax & kX32SyscallBit) == 0;
    dirty_regs_ = false;
    dirty_nr_ = false;
    return 0;
}

void RegsAccess::set_arg(int index, std::uint64_t value) noexcept {
    if (index < 0 || index > 5) {
        return;
    }
    view_.args[static_cast<std::size_t>(index)] = value;
    dirty_regs_ = true;
}

void RegsAccess::set_return(long value) noexcept {
    view_.ret = value;
    dirty_regs_ = true;
}

void RegsAccess::set_nr(long nr) noexcept {
    view_.nr = nr;
    dirty_regs_ = true;
}

void RegsAccess::restart_as(long nr) noexcept {
    constexpr std::uint64_t kSyscallInsnSize = 2; // 0f 05
    view_.ip -= kSyscallInsnSize;
    view_.ret = nr; // rax is the number the syscall instruction passes
    view_.nr = -1;  // no pending syscall, so signal delivery does not restart one
    dirty_regs_ = true;
}

int RegsAccess::commit() noexcept {
    if (!dirty_regs_ && !dirty_nr_) {
        return 0;
    }
    user_regs_struct r{};
    std::memcpy(&r, raw_.data(), sizeof(r));
    r.orig_rax = static_cast<std::uint64_t>(view_.nr);
    r.rdi = view_.args[0];
    r.rsi = view_.args[1];
    r.rdx = view_.args[2];
    r.r10 = view_.args[3];
    r.r8 = view_.args[4];
    r.r9 = view_.args[5];
    r.rax = static_cast<std::uint64_t>(view_.ret);
    r.rip = view_.ip;
    if (::ptrace(PTRACE_SETREGS, tid_, nullptr, &r) != 0) {
        return errno;
    }
    std::memcpy(raw_.data(), &r, sizeof(r));
    dirty_regs_ = false;
    dirty_nr_ = false;
    return 0;
}

std::uint64_t stack_red_zone() noexcept {
    return 128;
}

std::uint64_t stack_alignment() noexcept {
    return 16;
}

std::uint32_t native_audit_arch() noexcept {
    return AUDIT_ARCH_X86_64;
}

const char* arch_name() noexcept {
    return "x86_64";
}

} // namespace vhdp::arch
