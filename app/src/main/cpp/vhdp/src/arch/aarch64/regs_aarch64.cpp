// AArch64 register access via PTRACE_GETREGSET/SETREGSET (NT_PRSTATUS) and
// NT_ARM_SYSTEM_CALL for changing the syscall number.
//
// ABI notes: x0 is both the first argument and the return value; at a
// syscall-enter stop x0 still holds argument 0. Only x0 is clobbered by a
// syscall, so restoring x1..x5 after a rewrite is sufficient. x8 carries the
// syscall number at entry but the kernel reads the number from its own saved
// copy, hence NT_ARM_SYSTEM_CALL is required to change it.
#include "arch/regs.hpp"

#include <elf.h>
#include <linux/audit.h>
#include <sys/ptrace.h>
#include <sys/uio.h>

#include <cerrno>
#include <cstring>

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef NT_ARM_SYSTEM_CALL
#define NT_ARM_SYSTEM_CALL 0x404
#endif

namespace vhdp::arch {

namespace {
constexpr std::size_t kRegsSize = 34 * sizeof(std::uint64_t); // regs[31], sp, pc, pstate
constexpr std::size_t kSp = 31;
constexpr std::size_t kPc = 32;
constexpr std::size_t kPstate = 33;
constexpr std::uint64_t kPsrMode32Bit = 0x10;
} // namespace

RegsAccess::RegsAccess(pid_t tid) noexcept : tid_(tid) {}

int RegsAccess::fetch() noexcept {
    iovec iov{raw_.data(), kRegsSize};
    if (::ptrace(PTRACE_GETREGSET, tid_, reinterpret_cast<void*>(NT_PRSTATUS), &iov) != 0) {
        return errno;
    }
    view_.native_abi = iov.iov_len == kRegsSize && (raw_[kPstate] & kPsrMode32Bit) == 0;
    int nr = -1;
    iovec nr_iov{&nr, sizeof(nr)};
    if (::ptrace(PTRACE_GETREGSET, tid_, reinterpret_cast<void*>(NT_ARM_SYSTEM_CALL), &nr_iov) !=
        0) {
        nr = static_cast<int>(raw_[8]);
    }
    view_.nr = nr;
    for (std::size_t i = 0; i < 6; ++i) {
        view_.args[i] = raw_[i];
    }
    view_.ret = static_cast<long>(raw_[0]);
    view_.sp = raw_[kSp];
    view_.ip = raw_[kPc];
    dirty_regs_ = false;
    dirty_nr_ = false;
    return 0;
}

void RegsAccess::set_arg(int index, std::uint64_t value) noexcept {
    if (index < 0 || index > 5) {
        return;
    }
    view_.args[static_cast<std::size_t>(index)] = value;
    raw_[static_cast<std::size_t>(index)] = value;
    dirty_regs_ = true;
}

void RegsAccess::set_return(long value) noexcept {
    view_.ret = value;
    raw_[0] = static_cast<std::uint64_t>(value);
    dirty_regs_ = true;
}

void RegsAccess::set_nr(long nr) noexcept {
    view_.nr = nr;
    dirty_nr_ = true;
}

void RegsAccess::restart_as(long nr) noexcept {
    constexpr std::uint64_t kSvcInsnSize = 4;
    raw_[kPc] -= kSvcInsnSize;
    view_.ip = raw_[kPc];
    raw_[8] = static_cast<std::uint64_t>(nr); // x8: the number svc passes
    dirty_regs_ = true;
    view_.nr = -1; // no pending syscall, so signal delivery does not restart one
    dirty_nr_ = true;
}

int RegsAccess::commit() noexcept {
    if (dirty_regs_) {
        iovec iov{raw_.data(), kRegsSize};
        if (::ptrace(PTRACE_SETREGSET, tid_, reinterpret_cast<void*>(NT_PRSTATUS), &iov) != 0) {
            return errno;
        }
        dirty_regs_ = false;
    }
    if (dirty_nr_) {
        int nr = static_cast<int>(view_.nr);
        iovec iov{&nr, sizeof(nr)};
        if (::ptrace(PTRACE_SETREGSET, tid_, reinterpret_cast<void*>(NT_ARM_SYSTEM_CALL), &iov) !=
            0) {
            return errno;
        }
        dirty_nr_ = false;
    }
    return 0;
}

std::uint64_t stack_red_zone() noexcept {
    return 0;
}

std::uint64_t stack_alignment() noexcept {
    return 16;
}

std::uint32_t native_audit_arch() noexcept {
    return AUDIT_ARCH_AARCH64;
}

const char* arch_name() noexcept {
    return "aarch64";
}

} // namespace vhdp::arch
