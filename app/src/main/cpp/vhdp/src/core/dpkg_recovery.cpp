// dpkg/apt recovery PLAN, decided natively by VHDP.
//
// This is the "fix dpkg state after installation" capability. VHDP owns the decision and the
// recovery recipe (the shell script below); it inspects the rootfs's package-manager state and
// reports what is wrong and what to run. It does NOT execute anything here: this entry point is
// pure inspection, callable from an app process without starting a session -- so the caller runs
// the returned script through a fake-root session on whichever backend runs the rootfs (VHDP's
// own rootless engine, or proot as the fallback). The orchestration/logic stays in VHDP.
//
// Root cause repaired (same as before): with a non-root login (dracos), /var/lib/dpkg ends up
// owned by uid 1000, and a sudo-acquired root in a fake-root guest does not inherit the
// launch-time DAC bypass, so dpkg cannot write its own state. The script normalises system-tree ownership to
// root:root inside a single fake-root pass (the only context where chown succeeds), clears stale
// locks, repairs the status DB, and finishes the interrupted transaction.
#include "core/reports.hpp"

#include "common/json.hpp"

#include <sys/stat.h>

#include <climits>
#include <cstdlib>
#include <string>

namespace vhdp::core {
namespace {

bool is_dir(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool file_nonempty(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

bool exists(const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0;
}

// POSIX sh, run as fake-root (uid 0) inside the guest by the proot backend. Every step tolerates
// absent files and never aborts the whole script on a single failure; rc reflects the REAL dpkg
// outcome so the caller only records success when the transaction actually completed.
constexpr const char* kRecoveryScript = R"DRACSH(set -u
log() { echo "[drac-recovery] $*"; }
log "start"

# 1) Ownership: the system tree must be root-owned so apt/dpkg (root via sudo) can write it.
#    User homes are intentionally skipped so the non-root session keeps owning its files.
for d in /etc /usr /var /bin /sbin /lib /lib32 /lib64 /libx32 /opt /run /srv /boot; do
    [ -e "$d" ] && chown -R 0:0 "$d" 2>/dev/null || true
done

# 2) Clear stale package-manager locks left by an interrupted run.
rm -f /var/lib/dpkg/lock /var/lib/dpkg/lock-frontend \
      /var/lib/apt/lists/lock /var/cache/apt/archives/lock 2>/dev/null || true

# 3) dpkg status DB integrity: restore from a good copy when empty/corrupt.
DB=/var/lib/dpkg
S="$DB/status"
if [ ! -s "$S" ]; then
    log "status missing/empty; attempting restore"
    for b in "$DB/status-old" /var/backups/dpkg.status.0 "$DB/status-new"; do
        if [ -s "$b" ]; then cp -a "$b" "$S" && log "restored from $b" && break; fi
    done
fi
[ -s "$S" ] && cp -a "$S" "$DB/status.drac-bak" 2>/dev/null || true   # rollback point

# 4) Finish the interrupted transaction; roll the status DB back if it fails.
rc=0
if command -v dpkg >/dev/null 2>&1; then
    if dpkg --configure -a; then
        log "dpkg --configure -a OK"
    else
        log "dpkg --configure -a FAILED -> rolling back status"
        [ -s "$DB/status.drac-bak" ] && cp -a "$DB/status.drac-bak" "$S"
        rc=1
    fi
fi
log "done (rc=$rc)"
exit "$rc"
)DRACSH";

} // namespace

std::string build_dpkg_plan_json(const std::string& rootfs_in) {
    JsonWriter w;
    w.begin_object();
    w.key("input").value(rootfs_in);

    char canon[PATH_MAX];
    struct stat st{};
    if (rootfs_in.empty() || ::realpath(rootfs_in.c_str(), canon) == nullptr ||
        ::stat(canon, &st) != 0 || !S_ISDIR(st.st_mode)) {
        w.key("rootfs").null();
        w.key("status").value("error");
        w.key("needs_recovery").value(false);
        w.key("message").value("rootfs path does not exist or is not a directory");
        w.end_object();
        return std::move(w).take();
    }

    const std::string root(canon);
    w.key("rootfs").value(root);

    const bool is_dpkg = is_dir(root + "/var/lib/dpkg");
    w.key("is_dpkg_rootfs").value(is_dpkg);

    // Observability: report what VHDP saw. The decision itself is "any dpkg rootfs gets one
    // recovery pass" (the ownership normalisation is the point and is idempotent), matching the
    // established behaviour; the script itself handles the finer status/lock repair.
    w.key("status_db").begin_object();
    w.key("present").value(file_nonempty(root + "/var/lib/dpkg/status"));
    w.end_object();

    w.key("locks").begin_array();
    for (const char* lk : {"/var/lib/dpkg/lock", "/var/lib/dpkg/lock-frontend",
                           "/var/lib/apt/lists/lock", "/var/cache/apt/archives/lock"}) {
        if (exists(root + lk)) {
            w.value(lk);
        }
    }
    w.end_array();

    w.key("needs_recovery").value(is_dpkg);
    w.key("script").value(is_dpkg ? kRecoveryScript : "");
    w.key("status").value("ok");
    w.end_object();
    return std::move(w).take();
}

} // namespace vhdp::core
