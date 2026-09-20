// Capability matrix. Single source of truth for `phdp capabilities` and
// docs/CAPABILITIES.md. Every `supported`/`partial` claim for the Linux host
// profile names the CTest test(s) that exercise it.
#include "common/json.hpp"
#include "core/context.hpp"
#include "core/reports.hpp"
#include "hdr/hardware_registry.hpp"
#include "linux_abi/syscall_table.hpp"
#include "vdr/drivers.hpp"
#include "vhdp_version.hpp"

#include "arch/raw_syscall.h"

namespace vhdp::core {

const char* support_status_name(SupportStatus s) noexcept {
    switch (s) {
        case SupportStatus::supported:
            return "supported";
        case SupportStatus::partial:
            return "partial";
        case SupportStatus::unsupported:
            return "unsupported";
        case SupportStatus::untested:
            return "untested";
        case SupportStatus::backend_required:
            return "backend-required";
    }
    return "unknown";
}

namespace {
using S = SupportStatus;
constexpr S kSup = S::supported;
constexpr S kPart = S::partial;
constexpr S kUnsup = S::unsupported;
constexpr S kUnt = S::untested;
constexpr S kBack = S::backend_required;
} // namespace

const std::vector<ProfileEntry>& capability_profiles() {
    static const std::vector<ProfileEntry> k = {
        {"linux-x86_64",
         "Linux x86_64 host, same-architecture guest, rootless engine (development/CI profile; not "
         "an Android claim)",
         kSup, "host test suite (ctest) on Linux x86_64"},
        {"linux-aarch64", "Linux aarch64 host, same-architecture guest, rootless engine", kUnt,
         "compiled for Android arm64 only; no aarch64 Linux test run"},
        {"terminal-unprivileged",
         "Android ARM64 CLI from a terminal app or adb shell, same-architecture guest, when the "
         "ptrace probe passes",
         kUnt, "android-arm64-release artifacts build with the official NDK; device tests NOT RUN"},
        {"android-emulator-x86_64", "Android x86_64 emulator CLI, same-architecture guest", kUnt,
         "android-x86_64-debug artifacts build; emulator tests NOT RUN"},
        {"android-app", "libvhdp inside a third-party Android app (targetSdk >= 29)", kSup,
         "guest programs start through the userland loader (libvhdp-loader.so in "
         "nativeLibraryDir), which maps them PROT_EXEC from app storage; host policy refusals are "
         "answered (seccomp traps, link(2), NETLINK_AUDIT, denied /proc files). Device QA: "
         "arm64 Android 15 phone and x86_64 Android 13 running a Debian rootfs through phdp from "
         "an app, incl. apt/dpkg, job control and procps"},
        {"rooted", "rooted device with real namespaces/chroot/cgroups", kBack,
         "rooted engine not implemented"},
        {"vm", "full guest kernel through a hypervisor (AVF/KVM)", kBack,
         "VM engine not implemented"},
        {"cross-architecture", "guest ABI different from the host (e.g. x86_64 rootfs on arm64)",
         kBack, "no emulator adapter integrated; ABI mismatch is detected and reported"},
    };
    return k;
}

const std::vector<FeatureEntry>& capability_features() {
    static const std::vector<FeatureEntry> k = {
        {"cli.run",
         "phdp run ROOTFS -- CMD and the positional alias; exit status propagation",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-CLI-RUN-EXIT37", "T-CLI-POSITIONAL-EXIT37", "T-CLI-ARGS-ENV-CWD"},
         ""},
        {"cli.parser",
         "subcommand disambiguation, option validation, unsupported options rejected",
         kSup,
         kSup,
         kSup,
         kSup,
         kSup,
         {"T-UNIT-CLI-ARGS"},
         "pure parser, platform independent"},
        {"cli.default_shell",
         "no command runs /bin/sh (fallback /bin/bash, /bin/ash)",
         kSup,
         kUnt,
         kUnsup,
         kBack,
         kBack,
         {"T-PTY-DEFAULT-SHELL"},
         ""},
        {"cli.reports",
         "doctor, inspect, capabilities with --json",
         kSup,
         kUnt,
         kSup,
         kUnt,
         kUnt,
         {"T-CLI-DOCTOR-JSON", "T-CLI-INSPECT-JSON", "T-CLI-CAPABILITIES-JSON"},
         ""},
        {"cli.json_events",
         "lifecycle/diagnostic events as JSON Lines on stderr or --event-fd; guest stdout "
         "untouched",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-CLI-JSON-EVENTS"},
         ""},
        {"lib.c_abi",
         "stable C ABI: versioned structs, opaque handles, export allowlist, no C++ symbol leaks",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-UNIT-CAPI", "T-ABI-SYMBOLS", "T-ABI-HEADERS-C"},
         ""},
        {"lib.cpp_wrapper",
         "header-only RAII C++ wrapper over the C ABI",
         kSup,
         kUnt,
         kUnsup,
         kBack,
         kBack,
         {"T-UNIT-CPP-WRAPPER", "T-ABI-HEADERS-CPP"},
         ""},
        {"lib.embedding_examples",
         "C and C++ examples run the same session as the CLI",
         kSup,
         kUnt,
         kUnsup,
         kBack,
         kBack,
         {"T-EX-C-EMBED", "T-EX-CPP-EMBED"},
         ""},
        {"lib.jni_aar",
         "JNI/Kotlin wrapper and AAR",
         kUnsup,
         kUnsup,
         kPart,
         kUnsup,
         kUnsup,
         {},
         "no AAR is published; an app embeds the C ABI through its own JNI layer and runs "
         "sessions through phdp from nativeLibraryDir"},
        {"exec.static_elf",
         "static same-architecture ELF from the rootfs",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-CLI-RUN-EXIT37"},
         ""},
        {"exec.dynamic_elf",
         "dynamic ELF through the rootfs dynamic loader (loader indirection)",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-EXEC-DYNAMIC-LOADER"},
         "without the userland loader /proc/self/exe names ld.so (glibc --argv0 used when "
         "supported); with it (Android) exe names the program"},
        {"exec.shebang",
         "#! scripts with interpreter lookup inside the rootfs",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-EXEC-SHEBANG"},
         ""},
        {"exec.abi_mismatch",
         "foreign-architecture ELF refused with ENOEXEC and a diagnostic",
         kSup,
         kUnt,
         kUnsup,
         kBack,
         kBack,
         {"T-EXEC-ABI-MISMATCH"},
         ""},
        {"exec.loader_missing",
         "missing PT_INTERP loader refused with ENOENT and a diagnostic",
         kSup,
         kUnt,
         kUnsup,
         kBack,
         kBack,
         {"T-EXEC-LOADER-MISSING"},
         ""},
        {"fs.paths",
         "absolute and relative paths, cwd, chdir/getcwd",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-FS-ABS-REL", "T-UNIT-RESOLVER"},
         ""},
        {"fs.at_family",
         "dirfd-relative *at syscalls (openat, newfstatat, statx, mkdirat, unlinkat, renameat2, "
         "linkat, symlinkat, readlinkat, faccessat2, fchmodat, fchownat, utimensat)",
         kPart,
         kUnt,
         kPart,
         kBack,
         kBack,
         {"T-FS-DIRFD"},
         "openat2 refused with ENOSYS"},
        {"fs.dotdot",
         "'..' never climbs above the guest root",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-FS-DOTDOT", "T-UNIT-RESOLVER"},
         ""},
        {"fs.symlinks",
         "absolute/relative symlinks resolved in guest space; loops give ELOOP",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-FS-SYMLINK-ESCAPE", "T-UNIT-RESOLVER"},
         "resolution/use race (TOCTOU) remains; see SECURITY.md"},
        {"fs.hardlink_rename",
         "link/rename inside a mount; EXDEV across mounts",
         kPart,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-FS-RENAME-LINK"},
         "where the host refuses link(2) (Android app storage) hard links are emulated: shared "
         "object, st_nlink/st_ino, d_type, collapse back to a file at one name"},
        {"fs.magic_links",
         "/proc magic links (--proc host): self remap, outside targets fail closed",
         kPart,
         kUnt,
         kPart,
         kBack,
         kBack,
         {"T-FS-MAGIC-LINK"},
         "host /proc content is visible when --proc host is used; files the host refuses "
         "(Android: stat, uptime, loadavg, version, vmstat, ...) are served by stand-ins, and "
         "/proc/<pid>/status of session processes shows the guest identity"},
        {"fs.binds",
         "explicit binds, read-only by default, :rw opt-in",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-FS-BIND-RO", "T-FS-BIND-RW"},
         "bind targets missing in the rootfs are not listed by readdir"},
        {"fs.read_only_rootfs",
         "--read-only-rootfs returns EROFS for writes",
         kSup,
         kUnt,
         kUnsup,
         kBack,
         kBack,
         {"T-FS-READONLY-ROOTFS"},
         ""},
        {"fs.inherited_fd",
         "only stdio is inherited; descriptors outside the mounts are refused as dirfd",
         kSup,
         kUnt,
         kUnsup,
         kBack,
         kBack,
         {"T-FS-INHERITED-FD"},
         ""},
        {"fs.unix_sockets",
         "AF_UNIX pathname connect/bind/sendto/sendmsg translated",
         kPart,
         kUnt,
         kUnsup,
         kBack,
         kBack,
         {"T-FS-UNIX-SOCKET"},
         "host path must fit 107 bytes; sendmmsg with pathname addresses refused"},
        {"identity.uid_gid",
         "guest-visible uid/gid and stat ownership (--uid/--gid)",
         kPart,
         kUnt,
         kPart,
         kBack,
         kBack,
         {"T-ID-UID-GID"},
         "credentials tracked per process (set*id rules, getgroups/setgroups); chown is a no-op "
         "for uid 0"},
        {"dev.minimal",
         "/dev/null, zero, full, random, urandom, tty, ptmx, pts",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-DEV-MINIMAL"},
         ""},
        {"tty.pty",
         "PTY sessions, Ctrl-C/SIGINT exit 130, window resize",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-PTY-CTRL-C", "T-PTY-RESIZE", "T-LIB-PTY-RESIZE"},
         ""},
        {"tty.job_control",
         "interactive shell job control",
         kPart,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-PTY-JOB-CONTROL"},
         "test drives the fixture shell on a PTY (command list and exit status); stopping and "
         "resuming jobs is not covered by an automated test"},
        {"proc.signals",
         "signal forwarding and 128+N exit status",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-CLI-SIGNAL-FORWARD"},
         "signals to processes outside the session are refused (ESRCH)"},
        {"proc.tree_cleanup",
         "whole process tree killed on exit/cancel; no zombies or leaked fds",
         kSup,
         kUnt,
         kUnsup,
         kBack,
         kBack,
         {"T-LIFE-TREE-CANCEL", "T-LIFE-100-CYCLES"},
         ""},
        {"proc.timeout",
         "--timeout kills the tree and exits 124",
         kSup,
         kUnt,
         kUnsup,
         kBack,
         kBack,
         {"T-CLI-TIMEOUT"},
         ""},
        {"proc.multiprocess",
         "fork/vfork/clone/threads followed; CLONE_UNTRACED stripped; clone3 -> ENOSYS",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-PROC-FANOUT"},
         ""},
        {"syscall.policy",
         "unknown and dangerous syscalls refused with errno + diagnostic",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-SYS-UNSUPPORTED", "T-UNIT-SYSCALL-TABLE"},
         ""},
        {"syscall.seccomp_acceleration",
         "seccomp RET_TRACE accelerator with ptrace fallback",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-UNIT-SECCOMP-FILTER", "T-SYS-PTRACE-ONLY"},
         ""},
        {"net.host",
         "host network pass-through",
         kSup,
         kUnt,
         kSup,
         kBack,
         kBack,
         {"T-NET-HOST-SOCKET"},
         "socket creation tested; no external traffic in tests"},
        {"net.none",
         "--network none refuses non-AF_UNIX sockets",
         kPart,
         kUnt,
         kUnsup,
         kBack,
         kBack,
         {"T-NET-NONE"},
         "abstract AF_UNIX sockets stay reachable"},
        {"net.dns",
         "DNS resolution inside the guest",
         kUnt,
         kUnt,
         kSup,
         kBack,
         kBack,
         {},
         "depends on rootfs resolv.conf; no test"},
        {"guest.kernel_modules",
         "kernel modules / guest kernel drivers",
         kUnsup,
         kUnsup,
         kUnsup,
         kBack,
         kBack,
         {},
         ""},
        {"guest.systemd_cgroups",
         "systemd, full cgroups",
         kUnsup,
         kUnsup,
         kUnsup,
         kBack,
         kBack,
         {},
         ""},
        {"guest.containers_kvm_ebpf",
         "nested containers, KVM, eBPF, netfilter, FUSE",
         kUnsup,
         kUnsup,
         kUnsup,
         kBack,
         kBack,
         {},
         ""},
        {"guest.setuid_privilege",
         "setuid/capabilities granting host privilege",
         kUnsup,
         kUnsup,
         kUnsup,
         kBack,
         kBack,
         {},
         "no_new_privs is always set"},
        {"guest.hardware",
         "USB, raw block devices, GPU passthrough, GUI acceleration",
         kUnsup,
         kUnsup,
         kUnsup,
         kBack,
         kBack,
         {},
         ""},
        {"security.sandbox",
         "isolation for hostile root filesystems",
         kUnsup,
         kUnsup,
         kUnsup,
         kUnt,
         kBack,
         {},
         "rootless path translation is compatibility isolation only; use an isolated VM"},
    };
    return k;
}

std::string build_capabilities_json(Context& ctx) {
    JsonWriter w;
    w.begin_object();
    w.key("vhdp_version").value(VHDP_VERSION_STRING);
    w.key("abi_version").value(1u);
    w.key("build").begin_object();
    w.key("arch").value(VHDP_BUILD_ARCH);
    w.key("platform").value(VHDP_BUILD_PLATFORM);
    w.key("build_type").value(VHDP_BUILD_TYPE);
    w.key("assembly_raw_syscall").value(vhdp_raw_syscall_is_asm() != 0);
    w.key("rooted_engine").value(false);
    w.key("emulator_engine").value(false);
    w.key("vm_engine").value(false);
    w.end_object();

    w.key("status_values").begin_array();
    for (S s : {kSup, kPart, kUnsup, kUnt, kBack}) {
        w.value(support_status_name(s));
    }
    w.end_array();

    w.key("engines").begin_array();
    for (const Engine* e : ctx.engines()) {
        ProbeReport p = e->probe();
        w.begin_object();
        w.key("engine").value(engine_name(e->kind()));
        w.key("available").value(p.available);
        w.key("reason").value(p.reason);
        w.end_object();
    }
    w.end_array();

    w.key("profiles").begin_array();
    for (const auto& p : capability_profiles()) {
        w.begin_object();
        w.key("id").value(p.id).key("status").value(support_status_name(p.status));
        w.key("description").value(p.description).key("evidence").value(p.evidence);
        w.end_object();
    }
    w.end_array();

    w.key("features").begin_array();
    for (const auto& f : capability_features()) {
        w.begin_object();
        w.key("id").value(f.id).key("description").value(f.description);
        w.key("status").begin_object();
        w.key("linux-x86_64").value(support_status_name(f.linux_host));
        w.key("terminal-unprivileged").value(support_status_name(f.terminal_unprivileged));
        w.key("android-app").value(support_status_name(f.android_app));
        w.key("rooted").value(support_status_name(f.rooted));
        w.key("vm").value(support_status_name(f.vm));
        w.end_object();
        w.key("tests").begin_array();
        for (const char* t : f.tests) {
            w.value(t);
        }
        w.end_array();
        w.key("notes").value(f.notes);
        w.end_object();
    }
    w.end_array();

    std::size_t counts[5] = {};
    w.key("syscalls").begin_object();
    for (const auto& s : abi::all_syscalls()) {
        ++counts[static_cast<std::size_t>(s.cls)];
    }
    w.key("arch").value(VHDP_BUILD_ARCH);
    w.key("pass-through").value(static_cast<std::uint64_t>(counts[0]));
    w.key("translated").value(static_cast<std::uint64_t>(counts[1]));
    w.key("emulated").value(static_cast<std::uint64_t>(counts[2]));
    w.key("denied").value(static_cast<std::uint64_t>(counts[3]));
    w.key("unsupported").value(static_cast<std::uint64_t>(counts[4]));
    w.key("unknown_numbers").value("denied with ENOSYS");
    w.end_object();

    w.key("hardware").begin_array();
    for (const auto& h : hdr::hardware_registry()) {
        w.begin_object();
        w.key("id").value(h.id).key("provision").value(hdr::provision_name(h.provision));
        w.key("provided_by")
            .value(h.provided_by)
            .key("summary")
            .value(h.summary)
            .key("notes")
            .value(h.notes);
        w.end_object();
    }
    w.end_array();

    w.key("drivers").begin_array();
    for (const auto& d : vdr::driver_catalogue()) {
        w.begin_object();
        w.key("id")
            .value(d.id)
            .key("summary")
            .value(d.summary)
            .key("permissions")
            .value(d.permissions);
        w.key("policy").value(d.policy).key("cleanup").value(d.cleanup);
        w.end_object();
    }
    w.end_array();

    w.key("isolation")
        .value("compatibility isolation only; not a security sandbox for hostile root filesystems");
    w.end_object();
    return std::move(w).take();
}

} // namespace vhdp::core
