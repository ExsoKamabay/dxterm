#include "linux_abi/syscall_table.hpp"

#include <asm/unistd.h>

#include <array>

namespace vhdp::abi {

namespace {

#define VHDP_SYSCALL(name, cls, handler) {__NR_##name, #name, SyscallClass::cls, Handler::handler},
constexpr SyscallInfo kTable[] = {
#include "linux_abi/syscalls.def"
};
#undef VHDP_SYSCALL

constexpr std::size_t kMaxNr = 1024;

constexpr std::array<std::int16_t, kMaxNr> build_index() {
    std::array<std::int16_t, kMaxNr> idx{};
    for (auto& v : idx) {
        v = -1;
    }
    for (std::size_t i = 0; i < std::size(kTable); ++i) {
        auto nr = static_cast<std::size_t>(kTable[i].nr);
        if (nr < kMaxNr) {
            idx[nr] = static_cast<std::int16_t>(i);
        }
    }
    return idx;
}

constinit const std::array<std::int16_t, kMaxNr> kIndex = build_index();

} // namespace

const SyscallInfo* lookup_syscall(long nr) noexcept {
    if (nr < 0 || static_cast<unsigned long>(nr) >= kMaxNr) {
        return nullptr;
    }
    std::int16_t i = kIndex[static_cast<std::size_t>(nr)];
    return i < 0 ? nullptr : &kTable[static_cast<std::size_t>(i)];
}

std::span<const SyscallInfo> all_syscalls() noexcept {
    return {kTable, std::size(kTable)};
}

const char* class_name(SyscallClass c) noexcept {
    switch (c) {
        case SyscallClass::pass_through:
            return "pass-through";
        case SyscallClass::translated:
            return "translated";
        case SyscallClass::emulated:
            return "emulated";
        case SyscallClass::denied:
            return "denied";
        case SyscallClass::unsupported:
            return "unsupported";
    }
    return "unknown";
}

long lookup_nr(std::string_view name) noexcept {
    for (const auto& e : kTable) {
        if (name == e.name) {
            return e.nr;
        }
    }
    return -1;
}

} // namespace vhdp::abi
