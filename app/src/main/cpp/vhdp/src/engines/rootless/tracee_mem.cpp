#include "engines/rootless/tracee_mem.hpp"

#include "common/unique_fd.hpp"

#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace vhdp::rootless {

namespace {

std::size_t page_size() noexcept {
    long p = ::sysconf(_SC_PAGESIZE);
    return p > 0 ? static_cast<std::size_t>(p) : 4096u;
}

// Fallback through /proc/<tid>/mem (works for read-only pages too: the kernel
// uses FOLL_FORCE for a ptrace-attached tracer).
int proc_mem_io(pid_t tid, std::uint64_t addr, void* buf, std::size_t len, bool write) noexcept {
    char path[64];
    int n = std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(tid));
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(path)) {
        return EINVAL;
    }
    UniqueFd fd(::open(path, (write ? O_WRONLY : O_RDONLY) | O_CLOEXEC));
    if (!fd.valid()) {
        return errno == ENOENT ? ESRCH : errno;
    }
    std::size_t done = 0;
    while (done < len) {
        auto off = static_cast<off_t>(addr + done);
        ssize_t r = write
                        ? ::pwrite(fd.get(), static_cast<const char*>(buf) + done, len - done, off)
                        : ::pread(fd.get(), static_cast<char*>(buf) + done, len - done, off);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno == EIO ? EFAULT : errno;
        }
        if (r == 0) {
            return EFAULT;
        }
        done += static_cast<std::size_t>(r);
    }
    return 0;
}

bool overflows(std::uint64_t addr, std::size_t len) noexcept {
    return len > 0 && addr > UINT64_MAX - (len - 1);
}

} // namespace

int read_mem(pid_t tid, std::uint64_t addr, void* buf, std::size_t len) noexcept {
    if (len == 0) {
        return 0;
    }
    if (overflows(addr, len)) {
        return EFAULT;
    }
    iovec local{buf, len};
    iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(addr)), len};
    ssize_t r = ::process_vm_readv(tid, &local, 1, &remote, 1, 0);
    if (r == static_cast<ssize_t>(len)) {
        return 0;
    }
    if (r >= 0) {
        return EFAULT;
    }
    if (errno == ESRCH) {
        return ESRCH;
    }
    return proc_mem_io(tid, addr, buf, len, false);
}

int write_mem(pid_t tid, std::uint64_t addr, const void* buf, std::size_t len) noexcept {
    if (len == 0) {
        return 0;
    }
    if (overflows(addr, len)) {
        return EFAULT;
    }
    iovec local{const_cast<void*>(buf), len};
    iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(addr)), len};
    ssize_t r = ::process_vm_writev(tid, &local, 1, &remote, 1, 0);
    if (r == static_cast<ssize_t>(len)) {
        return 0;
    }
    if (r < 0 && errno == ESRCH) {
        return ESRCH;
    }
    return proc_mem_io(tid, addr, const_cast<void*>(buf), len, true);
}

int read_cstring(pid_t tid, std::uint64_t addr, std::size_t max, std::string& out) {
    out.clear();
    if (addr == 0) {
        return EFAULT;
    }
    const std::size_t page = page_size();
    char chunk[4096];
    while (out.size() < max) {
        // Read up to the end of the current page so an unmapped next page does not
        // fail a string that terminates before it.
        std::size_t to_page_end = page - static_cast<std::size_t>(addr % page);
        std::size_t want = to_page_end;
        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }
        if (want > max - out.size()) {
            want = max - out.size();
        }
        if (int e = read_mem(tid, addr, chunk, want); e != 0) {
            return e;
        }
        const void* nul = std::memchr(chunk, '\0', want);
        if (nul != nullptr) {
            out.append(chunk, static_cast<std::size_t>(static_cast<const char*>(nul) - chunk));
            return 0;
        }
        out.append(chunk, want);
        addr += want;
    }
    return ENAMETOOLONG;
}

} // namespace vhdp::rootless
