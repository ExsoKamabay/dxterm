#include "platform/linux/host_probe.hpp"

#include "common/unique_fd.hpp"
#include "linux_abi/errno_table.h"
#include "platform/linux/probe_child.h"
#include "platform/linux/proc.hpp"
#include "platform/linux/ptrace_defs.hpp"

#include <fcntl.h>
#include <linux/magic.h>
#include <linux/seccomp.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace vhdp::platform {

namespace {

std::string errno_text(int e) {
    return std::string(vhdp_errno_name(e)) + " (" + vhdp_errno_description(e) + ")";
}

// Waits for a child created by a probe; returns the wait status or -1.
int reap(pid_t pid) {
    int status = 0;
    for (;;) {
        pid_t r = ::waitpid(pid, &status, __WALL);
        if (r == pid) {
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                return status;
            }
            continue;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
}

} // namespace

PtraceProbe probe_ptrace_child() {
    PtraceProbe out;
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        out.err = errno;
        out.detail = "pipe2 failed: " + errno_text(out.err);
        return out;
    }
    UniqueFd rd(fds[0]);
    UniqueFd wr(fds[1]);
    int ready[2];
    if (::pipe2(ready, O_CLOEXEC) != 0) {
        out.err = errno;
        out.detail = "pipe2 failed: " + errno_text(out.err);
        return out;
    }
    UniqueFd ready_rd(ready[0]);
    UniqueFd ready_wr(ready[1]);
    long pid = vhdp_probe_spawn_waiter(rd.get(), ready_wr.get());
    if (pid < 0) {
        out.err = static_cast<int>(-pid);
        out.detail = "clone failed: " + errno_text(out.err);
        return out;
    }
    auto child = static_cast<pid_t>(pid);
    // Attach only once the child has made itself traceable (see probe_child.h).
    ready_wr.reset();
    char byte = 0;
    while (::read(ready_rd.get(), &byte, 1) < 0 && errno == EINTR) {
    }
    if (ptrace_call(kPtraceSeize, child, 0, kOptExitKill | kOptTraceSysGood) != 0) {
        out.err = errno;
        out.detail = "PTRACE_SEIZE of a child failed: " + errno_text(out.err);
        ::kill(child, SIGKILL);
        (void)reap(child);
        return out;
    }
    out.ok = true;
    out.detail = "PTRACE_SEIZE of a child succeeded";
    if (ptrace_call(kPtraceInterrupt, child, 0, 0) == 0) {
        int status = 0;
        pid_t r = -1;
        do {
            r = ::waitpid(child, &status, __WALL);
        } while (r < 0 && errno == EINTR);
        if (r == child && WIFSTOPPED(status)) {
            PtraceSyscallInfo info{};
            long n = ptrace_call(kPtraceGetSyscallInfo, child, sizeof(info),
                                 reinterpret_cast<std::uintptr_t>(&info));
            out.get_syscall_info = n > 0;
        }
    }
    ::kill(child, SIGKILL);
    (void)reap(child);
    return out;
}

SeccompProbe probe_seccomp_filter() {
    SeccompProbe out;
    // Everything is tried in a disposable child. Issuing seccomp(2) here -- as the
    // SECCOMP_GET_ACTION_AVAIL query once did -- kills the embedding process with SIGSYS under
    // a host policy that does not allow that syscall, which an Android app's policy does not.
    long pid = vhdp_probe_spawn_seccomp();
    if (pid < 0) {
        out.err = static_cast<int>(-pid);
        out.detail = "clone failed: " + errno_text(out.err);
        return out;
    }
    int status = reap(static_cast<pid_t>(pid));
    if (status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        out.ok = true;
        // SECCOMP_RET_TRACE shipped with filter mode itself (Linux 3.5), so a kernel that
        // installs a filter supports it.
        out.ret_trace_available = true;
        out.detail = "seccomp filter installation under no_new_privs succeeded";
    } else if (status >= 0 && WIFEXITED(status)) {
        out.err = WEXITSTATUS(status) - 1;
        out.detail = "seccomp filter installation failed: " + errno_text(out.err);
    } else {
        out.detail = "seccomp probe child did not exit normally";
    }
    return out;
}

NamespaceProbe probe_user_namespaces() {
    NamespaceProbe out;

    // The probe unshares a mount namespace and mounts a tmpfs in it. That is only
    // contained when the kernel really performs it. A ptrace supervisor such as
    // PRoot answers unshare() and mount() with a fake success and records the
    // mountpoint in its own table, so the probe would both report capabilities the
    // process does not have and leave the supervisor believing something is mounted
    // where nothing is. Report instead of probing.
    if (long tracer = tracer_pid(); tracer > 0) {
        out.detail = "skipped: this process is traced by PID " + std::to_string(tracer) +
                     " (a ptrace supervisor such as PRoot), which emulates unshare(2) and "
                     "mount(2); the result would describe the supervisor, not the kernel";
        return out;
    }

    // A directory of our own to mount on, removed once the child is gone: never a
    // shared path such as /tmp. Several candidates because there is no one path
    // that exists everywhere: TMPDIR when set, /tmp on ordinary Linux, and
    // /data/local/tmp on Android, which has no /tmp at all.
    const char* tmpdir_env = std::getenv("TMPDIR");
    const std::string leaf = "/.vhdp-nsprobe-" + std::to_string(::getpid());
    std::string mountpoint;
    std::string mkdir_error;
    for (const std::string& base :
         {std::string(tmpdir_env != nullptr ? tmpdir_env : ""), std::string("/tmp"),
          std::string("/data/local/tmp")}) {
        if (base.empty()) {
            continue;
        }
        std::string candidate = base + leaf;
        if (::mkdir(candidate.c_str(), 0700) == 0) {
            mountpoint = candidate;
            break;
        }
        if (mkdir_error.empty()) {
            mkdir_error = "mkdir " + candidate + ": " + errno_text(errno);
        }
    }
    if (mountpoint.empty()) {
        out.detail = "no probe mountpoint (set TMPDIR to a writable directory): " + mkdir_error;
        return out;
    }

    long pid = vhdp_probe_spawn_userns(static_cast<std::uint32_t>(::getuid()),
                                       static_cast<std::uint32_t>(::getgid()),
                                       mountpoint.c_str());
    if (pid < 0) {
        ::rmdir(mountpoint.c_str());
        out.detail = "clone failed: " + errno_text(static_cast<int>(-pid));
        return out;
    }
    int status = reap(static_cast<pid_t>(pid));
    ::rmdir(mountpoint.c_str()); // empty again: the tmpfs lived in the child's namespace
    if (status >= 0 && WIFEXITED(status)) {
        out.ran = true;
        out.bits = WEXITSTATUS(status);
        out.detail = "disposable user+mount namespace probe completed";
    } else {
        out.detail = "namespace probe child did not exit normally";
    }
    return out;
}

KvmProbe probe_kvm() {
    KvmProbe out;
    struct stat st{};
    if (::stat("/dev/kvm", &st) != 0) {
        out.err = errno;
        return out;
    }
    out.present = S_ISCHR(st.st_mode);
    UniqueFd fd(::open("/dev/kvm", O_RDWR | O_CLOEXEC));
    if (!fd.valid()) {
        out.err = errno;
        return out;
    }
    out.accessible = true;
    constexpr unsigned long kKvmGetApiVersion = 0xAE00; // _IO(KVMIO, 0x00)
    int v = ::ioctl(fd.get(), kKvmGetApiVersion, 0);
    out.api_version = v;
    if (v < 0) {
        out.err = errno;
    }
    return out;
}

std::string kernel_release() {
    utsname u{};
    return ::uname(&u) == 0 ? std::string(u.release) : std::string();
}

std::string kernel_machine() {
    utsname u{};
    return ::uname(&u) == 0 ? std::string(u.machine) : std::string();
}

long page_size_sysconf() {
    return ::sysconf(_SC_PAGESIZE);
}

unsigned long page_size_auxv() {
    return ::getauxval(AT_PAGESZ);
}

Openat2Probe probe_openat2(bool allow_child) {
    Openat2Probe out;
#ifndef SYS_openat2
    (void)allow_child;
    out.probed = true;
    out.detail = "openat2 unavailable (not defined by this build's headers)";
    return out;
#else
    std::string mode = seccomp_status();
    if (mode == "0") {
        errno = 0;
        long r = ::syscall(SYS_openat2, -1, nullptr, nullptr, 0);
        out.probed = true;
        out.available = !(r < 0 && errno == ENOSYS);
        out.detail = out.available ? "openat2 available" : "openat2 unavailable (ENOSYS)";
        return out;
    }
    if (!allow_child) {
        out.detail = "not probed: this process runs under a seccomp filter (Seccomp: " + mode +
                     "), which can kill it with SIGSYS for syscalls outside its allowlist; "
                     "active probes test openat2 in a disposable child";
        return out;
    }
    long pid = vhdp_probe_spawn_openat2();
    if (pid < 0) {
        out.detail = "not probed: clone failed: " + errno_text(static_cast<int>(-pid));
        return out;
    }
    int status = reap(static_cast<pid_t>(pid));
    if (status >= 0 && WIFEXITED(status)) {
        out.probed = true;
        out.available = WEXITSTATUS(status) == VHDP_OPENAT2_PROBE_AVAILABLE;
        out.detail = out.available ? "openat2 available (probed in a disposable child)"
                                   : "openat2 unavailable (ENOSYS, probed in a disposable child)";
    } else if (status >= 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS) {
        out.probed = true;
        out.detail =
            "openat2 blocked by this process's seccomp filter (probe child killed by SIGSYS)";
    } else {
        out.detail = "not probed: openat2 probe child did not exit normally";
    }
    return out;
#endif
}

std::optional<int> yama_ptrace_scope() {
    auto v = read_small_file("/proc/sys/kernel/yama/ptrace_scope", 64);
    if (!v.is_ok() || v.value().empty()) {
        return std::nullopt;
    }
    return v.value()[0] - '0';
}

std::string selinux_mode() {
    auto v = read_small_file("/sys/fs/selinux/enforce", 16);
    if (!v.is_ok() || v.value().empty()) {
        return "disabled/absent";
    }
    return v.value()[0] == '1' ? "enforcing" : "permissive";
}

std::string selinux_context() {
    auto v = read_small_file("/proc/self/attr/current", 512);
    if (!v.is_ok()) {
        return {};
    }
    std::string s = v.value();
    while (!s.empty() && (s.back() == '\0' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}

std::string seccomp_status() {
    return proc_status_field(0, "Seccomp").value_or("unknown");
}

bool cgroup2_available() {
    struct statfs fs{};
    return ::statfs("/sys/fs/cgroup", &fs) == 0 &&
           static_cast<unsigned long>(fs.f_type) == CGROUP2_SUPER_MAGIC;
}

std::string cgroup_self_path() {
    auto v = read_small_file("/proc/self/cgroup", 4096);
    if (!v.is_ok()) {
        return {};
    }
    const std::string& s = v.value();
    std::size_t p = s.find("0::");
    if (p == std::string::npos) {
        return {};
    }
    std::size_t e = s.find('\n', p);
    return s.substr(p + 3, e == std::string::npos ? std::string::npos : e - p - 3);
}

bool tun_accessible() {
    return ::access("/dev/net/tun", R_OK | W_OK) == 0;
}

bool is_android_host() {
#if defined(__ANDROID__)
    return true;
#else
    return ::access("/system/build.prop", F_OK) == 0;
#endif
}

std::optional<int> android_sdk_level() {
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.build.version.sdk", value) > 0) {
        int sdk = 0;
        for (char* p = value; *p >= '0' && *p <= '9'; ++p) {
            sdk = sdk * 10 + (*p - '0');
        }
        return sdk;
    }
#endif
    return std::nullopt;
}

bool android_app_context() {
    if (selinux_context().find(":untrusted_app") != std::string::npos) {
        return true;
    }
    // Without SELinux labels (a container such as WayDroid, a permissive build) the domain says
    // nothing, but an application uid on an Android host is still an app sandbox with the same
    // storage rules. AID_USER_OFFSET 100000, AID_APP_START..AID_APP_END 10000..19999.
    if (!is_android_host()) {
        return false;
    }
    uid_t app_id = ::getuid() % 100000;
    return app_id >= 10000 && app_id <= 19999;
}

namespace {

// Path of the file mapped at `addr` in this process, from /proc/self/maps; "" if none.
// Used instead of dladdr() so the answer is the same for libvhdp.so and for a static
// executable that libvhdp was linked into (where dladdr is a stub).
std::string mapped_file_containing(std::uintptr_t addr) {
    std::FILE* maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) {
        return {};
    }
    std::string found;
    char line[4096];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long lo = 0;
        unsigned long long hi = 0;
        int path_at = -1;
        if (std::sscanf(line, "%llx-%llx %*s %*s %*s %*s %n", &lo, &hi, &path_at) < 2 ||
            path_at < 0) {
            continue;
        }
        if (addr < lo || addr >= hi) {
            continue;
        }
        std::string path(line + path_at);
        while (!path.empty() && (path.back() == '\n' || path.back() == ' ')) {
            path.pop_back();
        }
        if (!path.empty() && path.front() == '/') {
            found = path;
        }
        break;
    }
    std::fclose(maps);
    return found;
}

void anchor_for_module_lookup() {}

} // namespace

std::string find_userland_loader(bool* explicit_env) {
    if (explicit_env != nullptr) {
        *explicit_env = false;
    }
    if (const char* env = std::getenv("VHDP_LOADER"); env != nullptr && env[0] != '\0') {
        if (explicit_env != nullptr) {
            *explicit_env = true;
        }
        // Configured explicitly: use it or nothing, never silently pick another loader.
        return ::access(env, X_OK) == 0 ? std::string(env) : std::string();
    }
    std::string module = mapped_file_containing(
        static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(&anchor_for_module_lookup)));
    if (module.empty()) {
        auto exe = read_link("/proc/self/exe");
        if (!exe.is_ok()) {
            return {};
        }
        module = exe.value();
    }
    std::size_t slash = module.rfind('/');
    if (slash == std::string::npos) {
        return {};
    }
    std::string candidate = module.substr(0, slash + 1) + kUserlandLoaderName;
    return ::access(candidate.c_str(), X_OK) == 0 ? candidate : std::string();
}

ExecMappingProbe probe_exec_mapping(const std::string& dir) {
    ExecMappingProbe out;
    if (dir.empty() || ::access(dir.c_str(), W_OK | X_OK) != 0) {
        out.detail = "not probed: no writable directory" + (dir.empty() ? "" : " (" + dir + ")");
        return out;
    }
    std::string name = dir + "/.vhdp-xmap-XXXXXX";
    int fd = ::mkstemp(name.data());
    if (fd < 0) {
        out.detail = "not probed: cannot create a file in " + dir + ": " + errno_text(errno);
        return out;
    }
    long ps = ::sysconf(_SC_PAGESIZE);
    std::size_t len = ps > 0 ? static_cast<std::size_t>(ps) : 4096;
    std::string zeros(len, '\0');
    bool written = ::write(fd, zeros.data(), len) == static_cast<ssize_t>(len);
    ::unlink(name.c_str());
    if (!written) {
        out.detail = "not probed: cannot write a probe file in " + dir;
        ::close(fd);
        return out;
    }
    out.probed = true;
    void* m = ::mmap(nullptr, len, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) {
        out.err = errno;
        out.detail = "mapping a file from " + dir + " PROT_EXEC is refused: " + errno_text(out.err);
    } else {
        ::munmap(m, len);
        out.allowed = true;
        out.detail = "files from " + dir + " can be mapped PROT_EXEC";
    }
    ::close(fd);
    return out;
}

long tracer_pid() {
    std::optional<std::string> v = proc_status_field(0, "TracerPid");
    if (!v.has_value()) {
        return 0;
    }
    char* end = nullptr;
    long pid = std::strtol(v->c_str(), &end, 10);
    return pid > 0 ? pid : 0;
}

std::optional<bool> mount_noexec(const std::string& dir) {
    struct statvfs vfs{};
    if (::statvfs(dir.c_str(), &vfs) != 0) {
        return std::nullopt;
    }
    return (vfs.f_flag & ST_NOEXEC) != 0;
}

std::string filesystem_type(const std::string& dir) {
    struct statfs fs{};
    if (::statfs(dir.c_str(), &fs) != 0) {
        return "unknown";
    }
    switch (static_cast<unsigned long>(fs.f_type)) {
        case EXT4_SUPER_MAGIC:
            return "ext2/3/4";
        case TMPFS_MAGIC:
            return "tmpfs";
        case BTRFS_SUPER_MAGIC:
            return "btrfs";
        case F2FS_SUPER_MAGIC:
            return "f2fs";
        case 0x65735546UL:
            return "fuse";
        case 0x5346414FUL:
            return "sdcardfs";
        case OVERLAYFS_SUPER_MAGIC:
            return "overlayfs";
        case 0x58465342UL:
            return "xfs";
        case 0x2011BAB0UL:
            return "exfat";
        case 0x4d44UL:
            return "vfat";
        default:
            return "other";
    }
}

} // namespace vhdp::platform
