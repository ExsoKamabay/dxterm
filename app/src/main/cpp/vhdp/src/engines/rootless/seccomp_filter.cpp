#include "engines/rootless/seccomp_filter.hpp"

#include <linux/seccomp.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>

namespace vhdp::rootless {

namespace {

constexpr std::uint32_t kOffNr = 0;
constexpr std::uint32_t kOffArch = 4;
constexpr std::uint32_t kOffArgs = 16; // seccomp_data.args[0], low 32 bits (little-endian)
constexpr std::uint32_t kX32Bit = 0x40000000u;
constexpr std::uint32_t kCloneUntraced = 0x00800000u;

sock_filter stmt(std::uint16_t code, std::uint32_t k) {
    return sock_filter{code, 0, 0, k};
}

sock_filter jump(std::uint16_t code, std::uint32_t k, std::uint8_t jt, std::uint8_t jf) {
    return sock_filter{code, jt, jf, k};
}

std::uint32_t trace_action() {
    return SECCOMP_RET_TRACE | kSeccompTraceMarker;
}

} // namespace

std::vector<sock_filter> build_seccomp_filter(const FilterPolicy& p) {
    std::vector<sock_filter> f;
    f.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, kOffArch));
    f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, p.audit_arch, 1, 0));
    f.push_back(stmt(BPF_RET | BPF_K, trace_action()));
    f.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, kOffNr));
    if (p.check_x32) {
        f.push_back(jump(BPF_JMP | BPF_JGE | BPF_K, kX32Bit, 0, 1));
        f.push_back(stmt(BPF_RET | BPF_K, trace_action()));
    }
    if (p.network_none && p.nr_socket >= 0) {
        f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(p.nr_socket), 0, 4));
        f.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, kOffArgs));
        f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 0, 1));
        f.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
        f.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | static_cast<std::uint32_t>(EACCES)));
    }
    if (p.nr_clone >= 0) {
        f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(p.nr_clone), 0, 4));
        f.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, kOffArgs));
        f.push_back(jump(BPF_JMP | BPF_JSET | BPF_K, kCloneUntraced, 0, 1));
        f.push_back(stmt(BPF_RET | BPF_K, trace_action()));
        f.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    }
    if (p.nr_sendto >= 0) {
        // sendto(fd, buf, len, flags, dest_addr, addrlen): allow when dest_addr == NULL.
        f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(p.nr_sendto), 0, 6));
        f.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, kOffArgs + 4 * 8));
        f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 3));
        f.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, kOffArgs + 4 * 8 + 4));
        f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
        f.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
        f.push_back(stmt(BPF_RET | BPF_K, trace_action()));
    }
    if (p.nr_ioctl >= 0 && !p.ioctl_trace.empty() && p.ioctl_trace.size() < 64) {
        // ioctl(fd, request, arg): trace the listed requests, allow every other one. The kernel
        // reads the request as an unsigned int, so its low 32 bits are the whole of it.
        const auto n = static_cast<std::uint8_t>(p.ioctl_trace.size());
        f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(p.nr_ioctl), 0,
                         static_cast<std::uint8_t>(n + 3)));
        f.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, kOffArgs + 8));
        for (std::uint8_t i = 0; i < n; ++i) {
            // A match jumps over the remaining comparisons and the ALLOW, onto the TRACE.
            f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, p.ioctl_trace[i],
                             static_cast<std::uint8_t>(n - i), 0));
        }
        f.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
        f.push_back(stmt(BPF_RET | BPF_K, trace_action()));
    }

    // Allowed numbers compressed into contiguous ranges [lo, hi].
    std::vector<long> nrs = p.allow;
    std::sort(nrs.begin(), nrs.end());
    nrs.erase(std::unique(nrs.begin(), nrs.end()), nrs.end());
    std::size_t i = 0;
    while (i < nrs.size()) {
        if (nrs[i] < 0) {
            ++i;
            continue;
        }
        long lo = nrs[i];
        long hi = lo;
        while (i + 1 < nrs.size() && nrs[i + 1] == hi + 1) {
            ++i;
            ++hi;
        }
        ++i;
        f.push_back(jump(BPF_JMP | BPF_JGE | BPF_K, static_cast<std::uint32_t>(lo), 0, 2));
        f.push_back(jump(BPF_JMP | BPF_JGT | BPF_K, static_cast<std::uint32_t>(hi), 1, 0));
        f.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    }
    f.push_back(stmt(BPF_RET | BPF_K, trace_action()));
    return f;
}

std::uint32_t evaluate_seccomp_filter(const std::vector<sock_filter>& prog, const FilterInput& in) {
    unsigned char data[64] = {};
    auto put32 = [&](std::size_t off, std::uint32_t v) {
        for (std::size_t k = 0; k < 4; ++k) {
            data[off + k] = static_cast<unsigned char>((v >> (8 * k)) & 0xff);
        }
    };
    put32(kOffNr, static_cast<std::uint32_t>(in.nr));
    put32(kOffArch, in.arch);
    for (std::size_t a = 0; a < 6; ++a) {
        put32(kOffArgs + a * 8, static_cast<std::uint32_t>(in.args[a] & 0xffffffffu));
        put32(kOffArgs + a * 8 + 4, static_cast<std::uint32_t>(in.args[a] >> 32));
    }
    std::uint32_t acc = 0;
    std::size_t pc = 0;
    std::size_t steps = 0;
    while (pc < prog.size() && steps++ < prog.size() + 1) {
        const sock_filter& ins = prog[pc];
        switch (ins.code) {
            case BPF_LD | BPF_W | BPF_ABS:
                if (ins.k > sizeof(data) - 4) {
                    return 0;
                }
                acc = static_cast<std::uint32_t>(data[ins.k]) |
                      (static_cast<std::uint32_t>(data[ins.k + 1]) << 8) |
                      (static_cast<std::uint32_t>(data[ins.k + 2]) << 16) |
                      (static_cast<std::uint32_t>(data[ins.k + 3]) << 24);
                ++pc;
                break;
            case BPF_JMP | BPF_JEQ | BPF_K:
                pc += 1 + (acc == ins.k ? ins.jt : ins.jf);
                break;
            case BPF_JMP | BPF_JGE | BPF_K:
                pc += 1 + (acc >= ins.k ? ins.jt : ins.jf);
                break;
            case BPF_JMP | BPF_JGT | BPF_K:
                pc += 1 + (acc > ins.k ? ins.jt : ins.jf);
                break;
            case BPF_JMP | BPF_JSET | BPF_K:
                pc += 1 + ((acc & ins.k) != 0 ? ins.jt : ins.jf);
                break;
            case BPF_RET | BPF_K:
                return ins.k;
            default:
                return 0;
        }
    }
    return 0;
}

} // namespace vhdp::rootless
