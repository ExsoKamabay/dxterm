// Guest system/device/arch PROJECTION plan, decided natively by VHDP (feature 4).
//
// The terminal's stability and compatibility depend on what hardware/system view the guest sees.
// VHDP owns that decision here: it reports the host architecture/kernel/page-size it can read from
// Android ("take data from Android"), and decides which real host trees (/dev, /proc, /sys) to
// project into the guest, checking each for availability. The backend that runs the session (the
// rootless engine, or proot as the fallback) then APPLIES the plan as binds. This is the seam
// where VHDP can substitute synthesized data for a tree Android restricts (the rootless engine
// already does that for global /proc, see engines/rootless/proc_synth.*) or present an edited
// virtual view -- the backend only applies whatever binds VHDP hands it, so that customization
// needs no change below this layer.
//
// Pure inspection (no engine/exec), so it runs in an app process.
#include "core/reports.hpp"

#include "common/json.hpp"
#include "platform/linux/host_probe.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace vhdp::core {
namespace {

// A host tree is projectable when it exists as a directory this process can SEARCH.
//
// Search (X_OK), deliberately not read (R_OK). On Android, SELinux refuses an untrusted_app
// permission to LIST /dev and /sys while still allowing it to traverse them and open the nodes
// inside -- /dev/null, /dev/zero, /dev/urandom, /dev/ptmx. Testing R_OK therefore reported both
// trees unavailable on every real device (they passed only on a permissive emulator), the backend
// dropped their binds, and the guest was left with no device nodes at all: writing to /dev/null
// silently created a regular file inside the rootfs instead. What the backend needs to know is
// whether the guest will be able to reach through the directory, which is exactly X_OK.
bool projectable(const char* p, std::string& why) {
    struct stat st{};
    if (::stat(p, &st) != 0) {
        why = std::string("stat: ") + std::strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        why = "not a directory";
        return false;
    }
    if (::access(p, X_OK) != 0) {
        why = std::string("not searchable: ") + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace

std::string build_projection_json() {
    JsonWriter w;
    w.begin_object();

    // Architecture / device data VHDP reads from the host and presents to the guest.
    w.key("host").begin_object();
    w.key("arch").value(platform::kernel_machine());
    w.key("kernel").value(platform::kernel_release());
    w.key("page_size").value(static_cast<std::int64_t>(platform::page_size_sysconf()));
    w.end_object();

    // System projection: the real host trees to graft into the guest. `available` lets the backend
    // skip one the OS hides; `ro` is false because proot binds these read-write by default.
    w.key("system_binds").begin_array();
    for (const char* d : {"/dev", "/proc", "/sys"}) {
        std::string why;
        bool ok = projectable(d, why);
        w.begin_object()
            .key("host").value(d)
            .key("guest").value(d)
            .key("available").value(ok)
            .key("ro").value(false);
        if (!ok) {
            // Says WHY the guest will be missing this tree. Without it a dropped bind is
            // invisible until something inside the guest fails for an unrelated-looking reason.
            w.key("unavailable_because").value(why);
        }
        w.end_object();
    }
    w.end_array();

    w.key("status").value("ok");
    w.end_object();
    return std::move(w).take();
}

} // namespace vhdp::core
