// Stand-ins for /proc files a host refuses to its processes.
//
// --proc host projects the host's /proc. An Android app's SELinux domain may not read most global
// files there (/proc/stat, uptime, loadavg, version, vmstat, ...), and procps, pstree, uptime
// and vmstat fail on the first one they need. For those files the supervisor answers an open
// with content built from sources the same process may use -- CLOCK_BOOTTIME, sysinfo(2),
// uname(2), cpuidle residency in sysfs, /proc/meminfo and /proc/self/mounts -- in the layout the
// kernel uses. Values the host does not expose are reported as zero rather than invented.
//
// A second stand-in keeps the guest identity consistent: /proc/<pid>/status of a session process
// shows the emulated credentials, as getuid() and stat() of /proc/<pid> already do, so that tools
// matching processes by uid (ps without arguments, pgrep -u, top) see the guest's processes.
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vhdp::rootless {

// Content for a global /proc file ("uptime", "sys/kernel/hostname", ...; the path below the
// proc mount without the leading slash), or nullopt when there is no stand-in for it.
std::optional<std::string> synth_proc_global(std::string_view rel, pid_t last_pid);
bool has_proc_stand_in(std::string_view rel) noexcept;

struct StatusIds {
    std::uint32_t uid[4]; // real, effective, saved, filesystem
    std::uint32_t gid[4];
    std::vector<std::uint32_t> groups;
};

// The kernel's status text with its Uid:, Gid: and Groups: lines replaced.
std::string rewrite_status_ids(const std::string& status, const StatusIds& ids);

} // namespace vhdp::rootless
