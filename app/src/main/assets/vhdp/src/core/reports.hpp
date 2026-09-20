// JSON reports: doctor (host probes), inspect (rootfs), capabilities (matrix).
#pragma once

#include <string>
#include <vector>

namespace vhdp::core {

class Context;

enum class SupportStatus { supported, partial, unsupported, untested, backend_required };
const char* support_status_name(SupportStatus s) noexcept;

struct ProfileEntry {
    const char* id;
    const char* description;
    SupportStatus status;
    const char* evidence;
};

struct FeatureEntry {
    const char* id;
    const char* description;
    SupportStatus linux_host;            // profile linux-x86_64 (where the test suite runs)
    SupportStatus terminal_unprivileged; // Android terminal app or adb shell, same-arch
    SupportStatus android_app;
    SupportStatus rooted;
    SupportStatus vm;
    std::vector<const char*> tests; // CTest names; empty when no automated test exists
    const char* notes;
};

const std::vector<ProfileEntry>& capability_profiles();
const std::vector<FeatureEntry>& capability_features();

std::string build_doctor_json(Context& ctx, bool active_probes);
std::string build_capabilities_json(Context& ctx);
std::string build_inspect_json(const std::string& rootfs);

// First-boot configuration of an already-extracted rootfs: bind mountpoints, DNS/hosts,
// a login profile, and a normal 'dracos' user with passwordless sudo. Pure filesystem work
// (no engine/ptrace), so it runs even in an Android app process. Idempotent: nothing that
// already exists is overwritten (resolv.conf is the one deliberate exception). Returns a JSON
// report {input, rootfs, status, actions[]}.
std::string build_configure_json(const std::string& rootfs);

// Decide dpkg/apt recovery for a rootfs: inspect the package-manager state and return a JSON plan
// {input, rootfs, is_dpkg_rootfs, status_db, locks[], needs_recovery, script}. VHDP owns the
// decision and the recovery script; the caller runs `script` in a session with guest uid 0
// (running dpkg needs to exec guest binaries, which this inspection call never does).
std::string build_dpkg_plan_json(const std::string& rootfs);

// Decide the guest system/device/arch projection: the host arch/kernel/page-size VHDP reads from
// the device, plus which host trees (/dev, /proc, /sys) to project into the guest. The caller
// applies the plan's binds. Returns JSON {host, system_binds[], status}.
std::string build_projection_json();

} // namespace vhdp::core
