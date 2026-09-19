// Host capability probes used by `doctor`, engine selection and inspect.
// Read-only inspection functions never modify the host. Active probes run in
// disposable children (see probe_child.h) and clean up after themselves.
#pragma once

#include <optional>
#include <string>

namespace vhdp::platform {

struct PtraceProbe {
    bool ok = false;
    int err = 0;
    bool get_syscall_info = false;
    std::string detail;
};

struct SeccompProbe {
    bool ok = false;
    int err = 0;
    bool ret_trace_available = false;
    std::string detail;
};

struct NamespaceProbe {
    bool ran = false;
    int bits = 0; // VHDP_NS_PROBE_*
    std::string detail;
};

struct Openat2Probe {
    bool probed = false; // false: not probed, detail says why
    bool available = false;
    std::string detail;
};

struct KvmProbe {
    bool present = false;
    bool accessible = false;
    int api_version = -1;
    int err = 0;
};

PtraceProbe probe_ptrace_child();
SeccompProbe probe_seccomp_filter();
NamespaceProbe probe_user_namespaces();
KvmProbe probe_kvm();

std::string kernel_release();
std::string kernel_machine();
long page_size_sysconf();
unsigned long page_size_auxv();
// openat2 availability. A process under a seccomp filter (every Android app process,
// Termux included) can be killed with SIGSYS for a syscall outside the filter's
// allowlist instead of getting ENOSYS, so there the syscall is never issued in-process:
// with allow_child it is tried in a disposable child, otherwise it is not probed.
Openat2Probe probe_openat2(bool allow_child);
std::optional<int> yama_ptrace_scope();
std::string selinux_mode();    // "enforcing", "permissive", "disabled/absent"
std::string selinux_context(); // /proc/self/attr/current or ""
std::string seccomp_status();  // /proc/self/status "Seccomp" field
bool cgroup2_available();
std::string cgroup_self_path();
bool tun_accessible();
bool is_android_host();
std::optional<int> android_sdk_level();
bool android_app_context(); // untrusted_app* SELinux domain, or an app uid on an Android host

// PID of the process tracing this one (TracerPid in /proc/self/status), or 0 when
// untraced. Non-zero means a ptrace supervisor -- PRoot, strace, gdb -- is between
// this process and the kernel, so its syscalls may be emulated rather than real.
long tracer_pid();
// File name of the userland loader shipped next to libvhdp / phdp.
inline constexpr const char* kUserlandLoaderName = "libvhdp-loader.so";

// Where the userland loader is, or "" when there is none: $VHDP_LOADER when it names an
// executable file, otherwise kUserlandLoaderName in the directory of the module that
// contains libvhdp (the shared library, or the executable libvhdp was linked into).
// On Android both live in nativeLibraryDir, the one place an app may execve() from.
// `explicit_env` reports whether the answer came from $VHDP_LOADER.
std::string find_userland_loader(bool* explicit_env = nullptr);

struct ExecMappingProbe {
    bool probed = false;  // false: no writable directory to probe in (detail says why)
    bool allowed = false; // a regular file there could be mapped PROT_EXEC
    int err = 0;
    std::string detail;
};

// Whether this process may map a file from writable storage PROT_EXEC -- the permission
// the userland loader relies on. Probed in `dir` with a short-lived temporary file.
ExecMappingProbe probe_exec_mapping(const std::string& dir);

// statvfs ST_NOEXEC of dir; nullopt if statvfs fails.
std::optional<bool> mount_noexec(const std::string& dir);
std::string filesystem_type(const std::string& dir);

} // namespace vhdp::platform
