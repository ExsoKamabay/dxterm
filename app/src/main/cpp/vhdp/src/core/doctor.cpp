// `phdp doctor`: host probes. Read-only inspection first; active probes run only
// in disposable children (ptrace, seccomp, user/mount namespaces) and are skipped
// with VHDP_DOCTOR_NO_ACTIVE_PROBES. No privileged probe exists in this build.
#include "arch/regs.hpp"
#include "common/json.hpp"
#include "core/context.hpp"
#include "core/reports.hpp"
#include "linux_abi/elf.h"
#include "linux_abi/errno_table.h"
#include "platform/linux/host_probe.hpp"
#include "platform/linux/probe_child.h"
#include "vhdp_version.hpp"

#include <cstdlib>
#include <unistd.h>

namespace vhdp::core {

namespace {

struct Check {
    std::string id;
    std::string status; // pass | warn | fail | info | skip
    std::string summary;
};

std::string errno_text(int e) {
    return std::string(vhdp_errno_name(e)) + " (" + vhdp_errno_description(e) + ")";
}

} // namespace

std::string build_doctor_json(Context& ctx, bool active) {
    std::vector<Check> checks;
    auto add = [&](std::string id, std::string status, std::string summary) {
        checks.push_back({std::move(id), std::move(status), std::move(summary)});
    };

    // --- read-only inspection ---
    std::string machine = platform::kernel_machine();
    std::string compiled = arch::arch_name();
    bool abi_match = (machine == compiled) || (machine == "arm64" && compiled == "aarch64");
    add("abi.host", abi_match ? "pass" : "fail",
        "kernel machine " + machine + ", libvhdp compiled for " + compiled +
            " (guest ELF machine " + vhdp_elf_machine_name(vhdp_elf_host_machine()) + ")");
    long ps = platform::page_size_sysconf();
    unsigned long aux = platform::page_size_auxv();
    add("memory.page_size", "info",
        "sysconf " + std::to_string(ps) + " bytes, AT_PAGESZ " + std::to_string(aux) + " bytes");
    add("kernel.release", "info", platform::kernel_release());
    auto yama = platform::yama_ptrace_scope();
    add("ptrace.yama_scope", !yama ? "info" : (*yama >= 2 ? "fail" : "pass"),
        yama ? "kernel.yama.ptrace_scope=" + std::to_string(*yama) : "Yama not present");
    std::string se = platform::selinux_mode();
    std::string sectx = platform::selinux_context();
    add("selinux.mode", "info", se + (sectx.empty() ? "" : ", context " + sectx));
    add("seccomp.self", "info",
        "Seccomp field of /proc/self/status: " + platform::seccomp_status());
    bool app = platform::android_app_context();
    const char* tmpdir_env = std::getenv("TMPDIR");
    std::string tmpdir = tmpdir_env != nullptr ? tmpdir_env : "/tmp";
    auto noexec_tmp = platform::mount_noexec(tmpdir);
    std::string storage;
    std::string storage_status = "pass";
    if (!app) {
        storage = "not running in an Android app domain: guest programs are exec'd directly";
    } else {
        // execve() from writable app storage is blocked for targetSdk >= 29. That is only fatal
        // when there is no other way to start a guest program, so assess the way VHDP uses.
        storage = "android app SELinux domain: execve() from writable app storage is blocked "
                  "(targetSdk >= 29)";
        std::string loader = platform::find_userland_loader();
        if (loader.empty()) {
            storage_status = "fail";
            storage += std::string("; no userland loader (") + platform::kUserlandLoaderName +
                       ") next to libvhdp, so guest programs cannot be started";
        } else {
            storage += "; guest programs start through the userland loader " + loader +
                       ", which maps them PROT_EXEC instead";
            platform::ExecMappingProbe xm = platform::probe_exec_mapping(tmpdir);
            if (xm.probed && !xm.allowed) {
                storage_status = "fail";
            }
            storage += "; " + xm.detail;
        }
    }
    if (noexec_tmp) {
        storage += std::string("; TMPDIR mount is ") + (*noexec_tmp ? "noexec" : "exec-capable");
    }
    add("exec.storage_policy", storage_status, storage);
    // Probed in a child only when active probes are allowed and this process is seccomp-filtered.
    platform::Openat2Probe openat2 = platform::probe_openat2(active);
    add("openat2", !openat2.probed ? "skip" : (openat2.available ? "pass" : "info"),
        openat2.probed && !openat2.available ? openat2.detail + "; component walk fallback"
                                             : openat2.detail);
    add("cgroup.v2", "info",
        platform::cgroup2_available()
            ? "cgroup2 mounted at /sys/fs/cgroup, self " + platform::cgroup_self_path()
            : "cgroup2 not mounted at /sys/fs/cgroup");
    add("net.tun", "info",
        platform::tun_accessible() ? "/dev/net/tun readable+writable"
                                   : "/dev/net/tun not accessible");
    platform::KvmProbe kvm = platform::probe_kvm();
    add("kvm.device", "info",
        !kvm.present
            ? "/dev/kvm absent"
            : (kvm.accessible
                   ? "/dev/kvm accessible, KVM_GET_API_VERSION=" + std::to_string(kvm.api_version)
                   : "/dev/kvm present but not accessible: " + errno_text(kvm.err)));
    bool android = platform::is_android_host();
    auto sdk = platform::android_sdk_level();
    add("android.host", "info",
        android ? "Android host" + (sdk ? ", SDK " + std::to_string(*sdk) : std::string())
                : "not an Android host");

    // --- active probes in disposable children ---
    bool ptrace_ok = false;
    if (active) {
        platform::PtraceProbe pt = platform::probe_ptrace_child();
        ptrace_ok = pt.ok;
        add("ptrace.child", pt.ok ? "pass" : "fail", pt.detail);
        add("ptrace.get_syscall_info", pt.get_syscall_info ? "pass" : "warn",
            pt.get_syscall_info
                ? "PTRACE_GET_SYSCALL_INFO available"
                : "PTRACE_GET_SYSCALL_INFO unavailable (enter/exit tracked manually)");
        platform::SeccompProbe sc = platform::probe_seccomp_filter();
        add("seccomp.filter", sc.ok ? "pass" : "warn",
            sc.detail + (sc.ok ? "" : "; rootless falls back to PTRACE_SYSCALL"));
        platform::NamespaceProbe ns = platform::probe_user_namespaces();
        if (!ns.ran) {
            add("namespace.user", "info", ns.detail);
        } else {
            auto bit = [&](int b) { return (ns.bits & b) != 0; };
            add("namespace.user", "info",
                bit(VHDP_NS_PROBE_USERNS)
                    ? "unprivileged user namespace creation works (disposable child)"
                    : "unprivileged user namespaces unavailable");
            add("namespace.mount", "info",
                bit(VHDP_NS_PROBE_MOUNTNS) ? "private mount namespace works"
                                           : "mount namespace unavailable");
            add("mount.tmpfs", "info",
                bit(VHDP_NS_PROBE_TMPFS) ? "tmpfs mount inside the namespace works"
                                         : "tmpfs mount failed");
            add("mount.devpts", "info",
                bit(VHDP_NS_PROBE_DEVPTS) ? "devpts newinstance mount works"
                                          : "devpts mount failed");
            add("mount.proc", "info",
                bit(VHDP_NS_PROBE_PROC) ? "proc mount in a new PID namespace works"
                                        : "proc mount failed");
        }
    } else {
        add("ptrace.child", "skip", "active probes disabled");
        add("seccomp.filter", "skip", "active probes disabled");
        add("namespace.user", "skip", "active probes disabled");
    }

    JsonWriter w;
    w.begin_object();
    w.key("vhdp_version").value(VHDP_VERSION_STRING);
    w.key("active_probes").value(active);
    w.key("checks").begin_array();
    for (const auto& c : checks) {
        w.begin_object()
            .key("id")
            .value(c.id)
            .key("status")
            .value(c.status)
            .key("summary")
            .value(c.summary)
            .end_object();
    }
    w.end_array();
    w.key("engines").begin_array();
    for (const Engine* e : ctx.engines()) {
        w.begin_object();
        w.key("engine").value(engine_name(e->kind()));
        if (e->kind() == EngineKind::rootless && !active) {
            w.key("available").null();
            w.key("reason").value("not probed (active probes disabled)");
        } else {
            ProbeReport p = e->probe();
            w.key("available").value(p.available);
            w.key("reason").value(p.reason);
        }
        w.end_object();
    }
    w.end_array();
    std::string profile;
    if (app) {
        profile = "android-app";
    } else if (android) {
        profile = "terminal-unprivileged";
    } else {
        profile = std::string("linux-") + compiled;
    }
    w.key("detected_profile").value(profile);
    // Inside an app the engine needs the userland loader and PROT_EXEC mappings of app storage,
    // which exec.storage_policy assesses.
    w.key("rootless_ready")
        .value(active ? (ptrace_ok && abi_match && storage_status != "fail") : false);
    w.end_object();
    return std::move(w).take();
}

} // namespace vhdp::core
