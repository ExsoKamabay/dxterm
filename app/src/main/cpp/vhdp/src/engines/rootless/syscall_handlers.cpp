// Syscall handlers of the rootless supervisor.
//
// Invariants for every handler:
//  - Guest pointers are only dereferenced through read_mem/read_cstring with
//    explicit bounds; failures map to the errno the kernel would return.
//  - A guest path never reaches the kernel untranslated: it is resolved in guest
//    space (vfs::resolve) and replaced by the host path written below the tracee
//    stack pointer. Rewritten arguments are restored at syscall exit (or right
//    away when the call is refused).
//  - Anything that cannot be translated fails closed with an errno and a
//    diagnostic event.
#include "engines/rootless/supervisor.hpp"

#include "common/json.hpp"
#include "common/unique_fd.hpp"
#include "engines/rootless/proc_synth.hpp"
#include "engines/rootless/termios2.hpp"
#include "engines/rootless/tracee_mem.hpp"
#include "linux_abi/errno_table.h"
#include "platform/linux/proc.hpp"
#include "vfs/guest_path.hpp"
#include "vfs/link_marker.hpp"

#include <asm/unistd.h>
#include <dirent.h>
#include <linux/netlink.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>

namespace vhdp::rootless {

using abi::Handler;
using core::EventKind;
using core::Severity;

namespace {

// Thread group whose program a /proc magic "exe" link names: "/proc/<n>/exe",
// "/proc/<n>/task/<t>/exe" or "/proc/self/exe". 0 when the path is not an exe link.
pid_t proc_exe_owner(std::string_view guest, pid_t self_tgid) {
    auto parts = vfs::components(guest);
    if (parts.size() < 3 || parts.front() != "proc" || parts.back() != "exe") {
        return 0;
    }
    if (parts.size() != 3 && !(parts.size() == 5 && parts[2] == "task")) {
        return 0;
    }
    if (parts[1] == "self") {
        return self_tgid;
    }
    pid_t n = 0;
    for (char c : parts[1]) {
        if (c < '0' || c > '9' || n > 400000000) {
            return 0;
        }
        n = n * 10 + (c - '0');
    }
    return n;
}

constexpr long kAtFdcwd = -100;
constexpr std::string_view kSelinuxFs = "/sys/fs/selinux";
constexpr std::uint64_t kAtSymlinkNofollow = 0x100;
constexpr std::uint64_t kAtSymlinkFollow = 0x400;
constexpr std::uint64_t kAtEmptyPath = 0x1000;
constexpr std::uint64_t kCloneUntraced = 0x00800000;
constexpr std::uint64_t kInDontFollow = 0x02000000;
constexpr std::uint64_t kFanMarkDontFollow = 0x4;
constexpr std::uint64_t kAtRemoveDir = 0x200;
constexpr std::uint64_t kSeccompModeFilter = 2;    // PR_SET_SECCOMP mode
constexpr std::uint64_t kSeccompSetModeFilter = 1; // seccomp(2) operation
constexpr std::uint64_t kRenameNoReplace = 1;
constexpr std::uint64_t kRenameExchange = 2;
constexpr std::size_t kMaxDirentsScan = std::size_t{1} << 20;
constexpr std::size_t kMaxExecArgs = 4096;
constexpr std::size_t kMaxArgStrlen = 131072; // MAX_ARG_STRLEN
constexpr std::size_t kMaxExecBytes = std::size_t{512} * 1024;
constexpr std::size_t kMsghdrSize = 56;
constexpr std::size_t kMmsghdrSize = 64;
constexpr std::size_t kDeletedSuffixLen = 10; // " (deleted)"

// st_gid and stx_gid follow the uid fields directly.
#if defined(__x86_64__)
constexpr std::size_t kStatUidOff = 28;
#else
constexpr std::size_t kStatUidOff = 24;
#endif
constexpr std::size_t kStatxUidOff = 20;
constexpr std::size_t kStatxNlinkOff = 16;
constexpr std::size_t kStatxInoOff = 32;
constexpr std::size_t kStatxDevMajorOff = 136;
constexpr std::size_t kStatxSize = 256;
#if defined(__x86_64__)
constexpr std::size_t kStatSize = 144; // struct stat of the x86_64 newfstatat ABI
#else
constexpr std::size_t kStatSize = 128; // generic struct stat (aarch64)
#endif
constexpr std::size_t kXattrMax = 65536; // XATTR_SIZE_MAX / XATTR_LIST_MAX
constexpr std::uint64_t kAtNoAutomount = 0x800;
constexpr std::uint64_t kAtStatxSyncType = 0x6000;
constexpr std::uint64_t kAtEaccess = 0x200;
constexpr std::uint64_t kStatxReserved = 0x80000000u;

// Argument checks the kernel makes before it looks the path up, so that a call with both a bad
// argument and a bad path fails with the same errno as on the host.
long early_stat_error(bool statx, std::uint64_t flags, std::uint64_t mask) {
    std::uint64_t allowed = kAtSymlinkNofollow | kAtNoAutomount | kAtEmptyPath;
    if (statx) {
        allowed |= kAtStatxSyncType;
        if ((flags & kAtStatxSyncType) == kAtStatxSyncType || (mask & kStatxReserved) != 0) {
            return -EINVAL;
        }
    }
    return (flags & ~allowed) != 0 ? -EINVAL : 0;
}

// "/proc/<pid>/root" -> "/proc/*/root": the part of a /proc path that says which link it is.
std::string proc_link_kind(std::string_view guest) {
    std::string out;
    for (auto c : vfs::components(guest)) {
        bool number = !c.empty() && c.find_first_not_of("0123456789") == std::string_view::npos;
        out.push_back('/');
        out.append(number ? std::string_view("*") : c);
    }
    return out;
}

bool ends_with_deleted(const std::string& s) {
    return s.size() > kDeletedSuffixLen &&
           s.compare(s.size() - kDeletedSuffixLen, kDeletedSuffixLen, " (deleted)") == 0;
}

PathSpec open_spec(int path_arg, int dirfd_arg, std::uint64_t flags) {
    PathSpec s;
    s.path_arg = path_arg;
    s.dirfd_arg = dirfd_arg;
    bool creat = (flags & O_CREAT) != 0;
    bool excl = (flags & O_EXCL) != 0;
    s.follow = (flags & O_NOFOLLOW) == 0 && !(creat && excl);
    int acc = static_cast<int>(flags & O_ACCMODE);
    bool tmpfile = (flags & O_TMPFILE) == O_TMPFILE;
    s.erofs_if_exists = acc != O_RDONLY || (flags & O_TRUNC) != 0 || tmpfile;
    s.erofs_if_missing = creat;
    return s;
}

PathSpec spec(int path_arg, int dirfd_arg, bool follow, bool erofs_if_exists = false,
              bool erofs_if_missing = false) {
    PathSpec s;
    s.path_arg = path_arg;
    s.dirfd_arg = dirfd_arg;
    s.follow = follow;
    s.erofs_if_exists = erofs_if_exists;
    s.erofs_if_missing = erofs_if_missing;
    return s;
}

const char* denial_reason(const abi::SyscallInfo& info) {
    std::string_view n = info.name;
    if (n == "ptrace") {
        return "nested ptrace is not supported: the rootless engine is already the tracer";
    }
    if (n.rfind("io_uring", 0) == 0) {
        return "io_uring would execute path operations without syscall translation";
    }
    if (n == "clone3") {
        return "clone3 is refused so the C library falls back to clone(2), whose flags can be "
               "checked";
    }
    if (n == "openat2") {
        return "openat2 resolution flags are not translated yet; callers fall back to openat";
    }
    if (n == "chroot") {
        return "chroot is not emulated by the rootless engine";
    }
    if (n == "unshare" || n == "setns") {
        return "namespace changes would invalidate path translation";
    }
    if (n == "mount" || n == "umount2" || n == "pivot_root" || n == "open_tree" ||
        n == "move_mount" || n == "fsopen" || n == "fsconfig" || n == "fsmount" || n == "fspick" ||
        n == "mount_setattr") {
        return "mount operations require a rooted or VM backend";
    }
    if (n == "open_by_handle_at" || n == "name_to_handle_at") {
        return "file handles bypass path translation";
    }
    if (n == "pidfd_getfd") {
        return "copying descriptors out of other processes is refused";
    }
    if (info.cls == abi::SyscallClass::unsupported) {
        return "not supported by the rootless engine";
    }
    return "refused by the rootless syscall policy";
}

} // namespace

pid_t Supervisor::tgid_of(Tracee& t) {
    if (t.tgid > 0) {
        return t.tgid;
    }
    if (auto v = platform::proc_status_field(t.tid, "Tgid")) {
        long tg = std::strtol(v->c_str(), nullptr, 10);
        if (tg > 0) {
            t.tgid = static_cast<pid_t>(tg);
            return t.tgid;
        }
    }
    return t.tid;
}

vfs::ResolveOptions Supervisor::resolve_options(Tracee& t, bool follow) {
    vfs::ResolveOptions o;
    o.follow_final = follow;
    // Any session on a host that refuses link(2) may meet emulated links -- another session on
    // the same rootfs can create them at any time -- but only once a store exists. Until then,
    // and on hosts that allow link(2), nobody pays a readlink per symlink looked up without
    // following (that costs about a tenth of a symlink-heavy listing).
    o.see_through_links = links_visible();
    o.dir_cache = &dir_cache_;
    if (cfg_->cfg.proc == core::ProcMode::host) {
        o.proc_tgid = tgid_of(t);
        o.proc_tid = t.tid;
    }
    return o;
}

int Supervisor::guest_cwd(Tracee& t, std::string& out) {
    auto link = platform::read_link("/proc/" + std::to_string(t.tid) + "/cwd");
    if (!link.is_ok()) {
        return ESRCH;
    }
    const std::string& host = link.value();
    if (ends_with_deleted(host)) {
        return ENOENT;
    }
    auto guest = table_.to_guest(host);
    if (!guest) {
        if (!first_report("path.cwd_outside", host)) {
            return EACCES;
        }
        emit(EventKind::diagnostic, Severity::warning, "path.cwd_outside",
             "guest working directory is outside every session mount; path lookups relative to it "
             "are refused",
             t.tid, EACCES);
        return EACCES;
    }
    out = std::move(*guest);
    return 0;
}

int Supervisor::guest_fd_path(Tracee& t, int fd, std::string& out, bool& is_path) {
    is_path = false;
    if (fd < 0) {
        return EBADF;
    }
    auto link = platform::read_link("/proc/" + std::to_string(t.tid) + "/fd/" + std::to_string(fd));
    if (!link.is_ok()) {
        return EBADF;
    }
    const std::string& host = link.value();
    if (host.empty() || host.front() != '/') {
        return 0; // pipe:, socket:, anon_inode: ...
    }
    is_path = true;
    if (ends_with_deleted(host)) {
        return ENOENT;
    }
    auto guest = table_.to_guest(host);
    if (!guest) {
        if (!first_report("path.fd_outside", host)) {
            return EACCES;
        }
        emit(EventKind::diagnostic, Severity::warning, "path.fd_outside",
             "descriptor " + std::to_string(fd) +
                 " refers to a host path outside the session mounts; refused",
             t.tid, EACCES);
        return EACCES;
    }
    out = std::move(*guest);
    return 0;
}

void Supervisor::rewrite_arg(Tracee& t, arch::RegsAccess& regs, int index, std::uint64_t value) {
    std::uint32_t bit = 1u << index;
    if ((t.pending.restore_mask & bit) == 0) {
        t.pending.orig_args[static_cast<std::size_t>(index)] =
            regs.view().args[static_cast<std::size_t>(index)];
        t.pending.restore_mask |= bit;
    }
    regs.set_arg(index, value);
    t.pending.active = true;
}

SyscallAction Supervisor::deny(Tracee& t, const abi::SyscallInfo* info, long nr, int err,
                               const char* name, const std::string& why, Severity sev) {
    auto& count = denials_[{nr, err}];
    if (count++ == 0 && sink_.wants(sev)) {
        JsonWriter w;
        w.begin_object();
        w.key("syscall").value(info != nullptr ? info->name : "unknown");
        w.key("nr").value(static_cast<std::int64_t>(nr));
        w.key("errno").value(vhdp_errno_name(err));
        w.key("class").value(info != nullptr ? abi::class_name(info->cls) : "unknown");
        w.end_object();
        std::string msg = std::string(info != nullptr ? info->name : "syscall") + " -> " +
                          vhdp_errno_name(err) + ": " + why;
        emit(EventKind::diagnostic, sev, name, msg, t.tid, err, w.str());
    }
    return {true, -static_cast<long>(err)};
}

int Supervisor::translate_path(Tracee& t, arch::RegsAccess& regs, StackWriter& sw,
                               const PathSpec& s, TranslatedPath& out) {
    out = TranslatedPath{};
    const auto& args = regs.view().args;
    std::uint64_t addr = args[static_cast<std::size_t>(s.path_arg)];
    if (addr == 0) {
        return s.null_ok ? 0 : EFAULT;
    }
    std::string path;
    if (int e = read_cstring(t.tid, addr, vfs::kPathMax, path); e != 0) {
        return e;
    }
    if (path.empty()) {
        return s.empty_ok ? 0 : ENOENT;
    }
    std::string full;
    if (vfs::is_absolute(path)) {
        full = path;
    } else {
        std::string base;
        long dirfd =
            s.dirfd_arg >= 0
                ? static_cast<long>(static_cast<int>(args[static_cast<std::size_t>(s.dirfd_arg)]))
                : kAtFdcwd;
        if (dirfd == kAtFdcwd) {
            if (int e = guest_cwd(t, base); e != 0) {
                return e;
            }
        } else {
            bool is_path = false;
            if (int e = guest_fd_path(t, static_cast<int>(dirfd), base, is_path); e != 0) {
                return e;
            }
            if (!is_path) {
                return ENOTDIR;
            }
        }
        full = vfs::join(base, path);
    }
    if (selinuxfs_hidden_ && vfs::is_below(full, kSelinuxFs)) {
        // The host has SELinux but keeps its interface to itself (an Android app domain), so the
        // guest is told there is none: libselinux verifies the mount by stat'ing it, fails, and
        // every program built on it behaves as on a kernel without SELinux. Left visible, dpkg
        // 1.23.6+ aborts every package install with "cannot open security status notification
        // channel", and nothing in the guest could have used the interface anyway.
        return ENOENT;
    }
    vfs::ResolveOptions ro = resolve_options(t, s.follow);
    ro.see_through_links = ro.see_through_links && !s.link_name;
    if (int e = vfs::resolve(table_, full, ro, out.r); e != 0) {
        if (e == EACCES && cfg_->cfg.proc == core::ProcMode::host && vfs::is_below(full, "/proc") &&
            first_report("path.magic_link_denied", proc_link_kind(full))) {
            emit(EventKind::diagnostic, Severity::warning, "path.magic_link_denied",
                 "/proc magic link target is outside the session mounts: " + full, t.tid, EACCES);
        }
        return e;
    }
    out.present = true;
    out.original = std::move(path);
    // A /proc magic link to a non-path object (pipe, socket, anon inode) names something the
    // guest already holds; whether it may be written is the kernel's decision for that object,
    // not a property of the read-only /proc projection it was reached through. Without this,
    // `echo x > /dev/stdout` on a pipe failed with EROFS.
    if (out.r.mount != nullptr && out.r.mount->read_only && !out.r.proc_magic) {
        if ((s.erofs_if_exists && out.r.exists) || (s.erofs_if_missing && !out.r.exists)) {
            return EROFS;
        }
    }
    return s.resolve_only ? 0 : apply_translation(t, regs, sw, s, out);
}

int Supervisor::apply_translation(Tracee& t, arch::RegsAccess& regs, StackWriter& sw,
                                  const PathSpec& s, const TranslatedPath& tp) {
    if (!tp.present || tp.r.host == tp.original) {
        return 0;
    }
    std::uint64_t new_addr = 0;
    if (int e = sw.push_string(tp.r.host, new_addr); e != 0) {
        if (e == ENOMEM) {
            emit(EventKind::diagnostic, Severity::warning, "path.scratch_unavailable",
                 "no writable stack space below the tracee stack pointer for a translated path",
                 t.tid, ENOMEM);
        }
        return e;
    }
    rewrite_arg(t, regs, s.path_arg, new_addr);
    return 0;
}

// Read-only queries on a translated path (stat, access, readlink, xattr reads) are run by the
// supervisor: it has the guest's host credentials and security context, so the same kernel call
// gives the same answer, and the guest's syscall is skipped. That is one stop instead of two and
// no argument register to restore afterwards. /proc keeps the kernel path: its links are
// remapped when the call returns.
bool Supervisor::links_visible() {
    return links_.has_objects() || (links_enabled_ && links_.stores_present(table_));
}

bool Supervisor::host_queryable(const TranslatedPath& tp) {
    return tp.present && tp.r.mount != nullptr && tp.r.mount->kind != vfs::MountKind::proc &&
           !tp.r.proc_magic;
}

long Supervisor::host_stat(Tracee& t, const std::string& host, std::uint64_t buf, bool statx,
                           std::uint64_t flags, std::uint64_t mask) {
    unsigned char kbuf[kStatxSize] = {};
    long r = statx ? ::syscall(__NR_statx, AT_FDCWD, host.c_str(), static_cast<int>(flags),
                               static_cast<unsigned int>(mask), kbuf)
                   : ::syscall(__NR_newfstatat, AT_FDCWD, host.c_str(), kbuf,
                               static_cast<int>(flags));
    if (r != 0) {
        return -static_cast<long>(errno);
    }
    present_stat(kbuf, statx);
    if (write_mem(t.tid, buf, kbuf, statx ? kStatxSize : kStatSize) != 0) {
        return -EFAULT;
    }
    return 0;
}

long Supervisor::host_readlink(Tracee& t, const std::string& host, std::uint64_t buf,
                               std::uint64_t size) {
    char target[vfs::kPathMax];
    std::size_t want = size < sizeof(target) ? static_cast<std::size_t>(size) : sizeof(target);
    ssize_t n = ::readlink(host.c_str(), target, want);
    if (n < 0) {
        return -static_cast<long>(errno);
    }
    if (n > 0 && write_mem(t.tid, buf, target, static_cast<std::size_t>(n)) != 0) {
        return -EFAULT;
    }
    return static_cast<long>(n);
}

long Supervisor::host_xattr(Tracee& t, const abi::SyscallInfo& info, const std::string& host,
                            arch::RegsAccess& regs) {
    const auto& a = regs.view().args;
    bool list = false;
    bool follow = info.handler == Handler::xattr_get;
#ifdef __NR_listxattr
    list = info.nr == __NR_listxattr;
#endif
#ifdef __NR_llistxattr
    list = list || info.nr == __NR_llistxattr;
#endif
    std::uint64_t out = list ? a[1] : a[2];
    std::size_t size = static_cast<std::size_t>(list ? a[2] : a[3]);
    if (size > kXattrMax) {
        size = kXattrMax; // the kernel caps the request the same way
    }
    if (xattr_buf_.size() < size) {
        xattr_buf_.resize(size);
    }
    char* data = size > 0 ? xattr_buf_.data() : nullptr;
    ssize_t n = 0;
    if (list) {
        n = follow ? ::listxattr(host.c_str(), data, size) : ::llistxattr(host.c_str(), data, size);
    } else {
        std::string name;
        if (int e = read_cstring(t.tid, a[1], 256, name); e != 0) {
            return -static_cast<long>(e == ENAMETOOLONG ? ERANGE : e);
        }
        n = follow ? ::getxattr(host.c_str(), name.c_str(), data, size)
                   : ::lgetxattr(host.c_str(), name.c_str(), data, size);
    }
    if (n < 0) {
        return -static_cast<long>(errno);
    }
    if (size > 0 && n > 0 && write_mem(t.tid, out, data, static_cast<std::size_t>(n)) != 0) {
        return -EFAULT;
    }
    return static_cast<long>(n);
}

SyscallAction Supervisor::handle_fd_meta(Tracee& t, arch::RegsAccess&, const abi::SyscallInfo&,
                                         int fd, bool chown) {
    bool fake_root = cfg_->cfg.uid.has_value() && *cfg_->cfg.uid == 0;
    std::string guest;
    bool is_path = false;
    int e = guest_fd_path(t, fd, guest, is_path);
    if (e == EBADF) {
        return {}; // the kernel reports EBADF itself
    }
    if (e != 0) {
        return {true, -static_cast<long>(e)};
    }
    if (is_path) {
        const vfs::Mount* m = table_.mount_for_guest(guest);
        if (m != nullptr && m->read_only) {
            return {true, -EROFS};
        }
    }
    if (chown && fake_root && guest_is_root(t)) {
        return {true, 0}; // emulated: ownership changes are accepted but not applied
    }
    return {};
}

bool Supervisor::guest_is_root(Tracee& t) {
    const core::SessionConfig& c = cfg_->cfg;
    return c.uid.has_value() && *c.uid == 0 && creds_of(t).euid == 0;
}

// Identity syscalls for a session that presents a guest identity. Semantics follow the kernel's
// credential rules, with "privileged" meaning an effective uid of 0 (in a real user namespace the
// capabilities CAP_SETUID/CAP_SETGID are dropped when the effective uid leaves 0).
SyscallAction Supervisor::handle_identity(Tracee& t, arch::RegsAccess& regs,
                                          const abi::SyscallInfo& info) {
    const auto& a = regs.view().args;
    Creds& cr = creds_of(t);
    constexpr std::uint32_t kKeep = 0xffffffffu; // (uid_t)-1: leave unchanged
    auto id = [&](int i) { return static_cast<std::uint32_t>(a[static_cast<std::size_t>(i)]); };
    auto in3 = [](std::uint32_t v, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return v == x || v == y || v == z;
    };
    bool priv = cr.euid == 0;
    long nr = info.nr;

    auto write_three = [&](std::uint32_t r, std::uint32_t e, std::uint32_t sv) -> SyscallAction {
        std::uint32_t v[3] = {r, e, sv};
        for (std::size_t i = 0; i < 3; ++i) {
            if (write_mem(t.tid, a[i], &v[i], sizeof(std::uint32_t)) != 0) {
                return {true, -EFAULT};
            }
        }
        return {true, 0};
    };

    switch (info.handler) {
        case Handler::getuid:
            return {true, static_cast<long>(cr.ruid)};
        case Handler::geteuid:
            return {true, static_cast<long>(cr.euid)};
        case Handler::getgid:
            return {true, static_cast<long>(cr.rgid)};
        case Handler::getegid:
            return {true, static_cast<long>(cr.egid)};
        case Handler::getresuid:
            return write_three(cr.ruid, cr.euid, cr.suid);
        case Handler::getresgid:
            return write_three(cr.rgid, cr.egid, cr.sgid);
        default:
            break;
    }

#ifdef __NR_getgroups
    if (nr == __NR_getgroups) {
        auto size = static_cast<std::int64_t>(static_cast<std::int32_t>(a[0]));
        auto n = static_cast<std::int64_t>(cr.groups.size());
        if (size < 0) {
            return {true, -EINVAL};
        }
        if (size == 0) {
            return {true, static_cast<long>(n)};
        }
        if (size < n) {
            return {true, -EINVAL};
        }
        if (n > 0 && write_mem(t.tid, a[1], cr.groups.data(),
                               cr.groups.size() * sizeof(std::uint32_t)) != 0) {
            return {true, -EFAULT};
        }
        return {true, static_cast<long>(n)};
    }
#endif
#ifdef __NR_setgroups
    if (nr == __NR_setgroups) {
        if (!priv) {
            return {true, -EPERM};
        }
        auto n = static_cast<std::int64_t>(static_cast<std::int32_t>(a[0]));
        if (n < 0 || n > 65536) {
            return {true, -EINVAL};
        }
        std::vector<std::uint32_t> list(static_cast<std::size_t>(n));
        if (n > 0 && read_mem(t.tid, a[1], list.data(), list.size() * sizeof(std::uint32_t)) != 0) {
            return {true, -EFAULT};
        }
        cr.groups = std::move(list);
        return {true, 0};
    }
#endif
    if (nr == __NR_setuid) {
        std::uint32_t u = id(0);
        if (priv) {
            cr.ruid = cr.euid = cr.suid = cr.fsuid = u;
        } else if (u == cr.ruid || u == cr.suid) {
            cr.euid = cr.fsuid = u;
        } else {
            return {true, -EPERM};
        }
        return {true, 0};
    }
    if (nr == __NR_setgid) {
        std::uint32_t g = id(0);
        if (priv) {
            cr.rgid = cr.egid = cr.sgid = cr.fsgid = g;
        } else if (g == cr.rgid || g == cr.sgid) {
            cr.egid = cr.fsgid = g;
        } else {
            return {true, -EPERM};
        }
        return {true, 0};
    }
    if (nr == __NR_setreuid || nr == __NR_setregid) {
        bool user = nr == __NR_setreuid;
        std::uint32_t& rr = user ? cr.ruid : cr.rgid;
        std::uint32_t& ee = user ? cr.euid : cr.egid;
        std::uint32_t& ss = user ? cr.suid : cr.sgid;
        std::uint32_t& fs = user ? cr.fsuid : cr.fsgid;
        std::uint32_t r = id(0);
        std::uint32_t e = id(1);
        if (!priv && ((r != kKeep && r != rr && r != ee) ||
                      (e != kKeep && !in3(e, rr, ee, ss)))) {
            return {true, -EPERM};
        }
        std::uint32_t old_r = rr;
        if (r != kKeep) {
            rr = r;
        }
        if (e != kKeep) {
            ee = e;
        }
        if (r != kKeep || (e != kKeep && e != old_r)) {
            ss = ee;
        }
        fs = ee;
        return {true, 0};
    }
    if (nr == __NR_setresuid || nr == __NR_setresgid) {
        bool user = nr == __NR_setresuid;
        std::uint32_t& rr = user ? cr.ruid : cr.rgid;
        std::uint32_t& ee = user ? cr.euid : cr.egid;
        std::uint32_t& ss = user ? cr.suid : cr.sgid;
        std::uint32_t& fs = user ? cr.fsuid : cr.fsgid;
        std::uint32_t v[3] = {id(0), id(1), id(2)};
        if (!priv) {
            for (std::uint32_t x : v) {
                if (x != kKeep && !in3(x, rr, ee, ss)) {
                    return {true, -EPERM};
                }
            }
        }
        if (v[0] != kKeep) {
            rr = v[0];
        }
        if (v[1] != kKeep) {
            ee = v[1];
        }
        if (v[2] != kKeep) {
            ss = v[2];
        }
        fs = ee;
        return {true, 0};
    }
    if (nr == __NR_setfsuid || nr == __NR_setfsgid) {
        bool user = nr == __NR_setfsuid;
        std::uint32_t& fs = user ? cr.fsuid : cr.fsgid;
        std::uint32_t old = fs;
        std::uint32_t f = id(0);
        bool ok = user ? in3(f, cr.ruid, cr.euid, cr.suid) || f == cr.fsuid
                       : in3(f, cr.rgid, cr.egid, cr.sgid) || f == cr.fsgid;
        if (f != kKeep && (priv || ok)) {
            fs = f;
        }
        return {true, static_cast<long>(old)}; // always the previous value, never an error
    }
    return {}; // not an identity syscall this handler knows: let the kernel decide
}

SyscallAction Supervisor::handle_signal_target(Tracee& t, const abi::SyscallInfo& info,
                                               arch::RegsAccess& regs) {
    const auto& a = regs.view().args;
    auto known = [&](long p) { return p > 0 && tracees_.count(static_cast<pid_t>(p)) != 0; };
    auto outside = [&](long p) {
        return deny(t, &info, info.nr, ESRCH, "signal.outside_session",
                    "target " + std::to_string(p) + " is not a process of this session",
                    Severity::debug);
    };
    switch (info.handler) {
        case Handler::kill: {
            long pid = static_cast<long>(static_cast<int>(a[0]));
            int sig = static_cast<int>(a[1]);
            if (pid > 0) {
                return known(pid) ? SyscallAction{} : outside(pid);
            }
            if (pid == 0) {
                return {};
            }
            if (pid == -1) {
                // Emulated: "every process the caller may signal" is limited to the session.
                pid_t self = tgid_of(t);
                bool sent = false;
                std::vector<pid_t> tids;
                tids.reserve(tracees_.size());
                for (auto& [tid, tr] : tracees_) {
                    tids.push_back(tid);
                }
                for (pid_t tid : tids) {
                    Tracee& other = tracees_[tid];
                    pid_t tg = tgid_of(other);
                    if (tg == self || tg != tid) {
                        continue;
                    }
                    if (sig == 0 || ::kill(tg, sig) == 0) {
                        sent = true;
                    }
                }
                return {true, sent ? 0 : -ESRCH};
            }
            pid_t pgid = static_cast<pid_t>(-pid);
            for (auto& [tid, tr] : tracees_) {
                if (::getpgid(tid) == pgid) {
                    return {};
                }
            }
            return outside(pid);
        }
        case Handler::tkill:
        case Handler::sigqueue:
        case Handler::pidfd_open: {
            long pid = static_cast<long>(static_cast<int>(a[0]));
            return known(pid) ? SyscallAction{} : outside(pid);
        }
        case Handler::tgkill:
        case Handler::tgsigqueue: {
            long tid = static_cast<long>(static_cast<int>(a[1]));
            return known(tid) ? SyscallAction{} : outside(tid);
        }
        default:
            return {};
    }
}

SyscallAction Supervisor::handle_sockaddr(Tracee& t, arch::RegsAccess& regs, int addr_arg,
                                          int len_arg, bool is_bind) {
    const auto& a = regs.view().args;
    std::uint64_t addr = a[static_cast<std::size_t>(addr_arg)];
    std::uint64_t len = a[static_cast<std::size_t>(len_arg)] & 0xffffffffu;
    if (addr == 0 || len <= offsetof(sockaddr_un, sun_path) || len > sizeof(sockaddr_un)) {
        return {}; // not a pathname AF_UNIX address, or invalid: let the kernel decide
    }
    sockaddr_un sun{};
    if (read_mem(t.tid, addr, &sun, static_cast<std::size_t>(len)) != 0) {
        return {};
    }
    if (sun.sun_family != AF_UNIX || sun.sun_path[0] == '\0') {
        return {}; // other families and abstract sockets are not filesystem paths
    }
    std::size_t max_path = static_cast<std::size_t>(len) - offsetof(sockaddr_un, sun_path);
    std::string path(sun.sun_path, ::strnlen(sun.sun_path, max_path));
    std::string full = path;
    if (!vfs::is_absolute(path)) {
        std::string cwd;
        if (int e = guest_cwd(t, cwd); e != 0) {
            return {true, -static_cast<long>(e)};
        }
        full = vfs::join(cwd, path);
    }
    vfs::Resolved r;
    if (int e = vfs::resolve(table_, full, resolve_options(t, !is_bind), r); e != 0) {
        return {true, -static_cast<long>(e)};
    }
    if (is_bind && !r.exists && r.mount != nullptr && r.mount->read_only) {
        return {true, -EROFS};
    }
    if (r.host == path) {
        return {};
    }
    if (r.host.size() + 1 > sizeof(sun.sun_path)) {
        return deny(t, abi::lookup_syscall(t.pending.nr), t.pending.nr, ENAMETOOLONG,
                    "unix_socket.path_too_long",
                    "translated AF_UNIX path exceeds 107 bytes: " + r.host);
    }
    sockaddr_un out{};
    out.sun_family = AF_UNIX;
    std::memcpy(out.sun_path, r.host.c_str(), r.host.size() + 1);
    std::size_t out_len = offsetof(sockaddr_un, sun_path) + r.host.size() + 1;
    StackWriter sw(t.tid, regs.view().sp);
    std::uint64_t new_addr = 0;
    if (int e = sw.push(&out, out_len, new_addr); e != 0) {
        return {true, -static_cast<long>(e)};
    }
    rewrite_arg(t, regs, addr_arg, new_addr);
    rewrite_arg(t, regs, len_arg, out_len);
    return {};
}

SyscallAction Supervisor::handle_sendmsg(Tracee& t, arch::RegsAccess& regs, bool multiple) {
    const auto& a = regs.view().args;
    auto unix_pathname = [&](std::uint64_t name, std::uint32_t namelen) {
        if (name == 0 || namelen <= offsetof(sockaddr_un, sun_path)) {
            return false;
        }
        char head[3] = {};
        if (read_mem(t.tid, name, head, sizeof(head)) != 0) {
            return false;
        }
        sa_family_t fam = 0;
        std::memcpy(&fam, head, sizeof(fam));
        return fam == AF_UNIX && head[2] != '\0';
    };
    if (multiple) {
        std::uint64_t vec = a[1];
        std::uint64_t vlen = a[2] & 0xffffffffu;
        if (vlen > 1024) {
            vlen = 1024;
        }
        for (std::uint64_t i = 0; i < vlen; ++i) {
            unsigned char hdr[16];
            if (read_mem(t.tid, vec + i * kMmsghdrSize, hdr, sizeof(hdr)) != 0) {
                return {};
            }
            std::uint64_t name = 0;
            std::uint32_t namelen = 0;
            std::memcpy(&name, hdr, 8);
            std::memcpy(&namelen, hdr + 8, 4);
            if (unix_pathname(name, namelen)) {
                return deny(t, abi::lookup_syscall(t.pending.nr), t.pending.nr, EOPNOTSUPP,
                            "unix_socket.sendmmsg",
                            "sendmmsg to AF_UNIX pathname addresses is not translated");
            }
        }
        return {};
    }
    unsigned char msg[kMsghdrSize];
    if (a[1] == 0 || read_mem(t.tid, a[1], msg, sizeof(msg)) != 0) {
        return {};
    }
    std::uint64_t name = 0;
    std::uint32_t namelen = 0;
    std::memcpy(&name, msg, 8);
    std::memcpy(&namelen, msg + 8, 4);
    if (!unix_pathname(name, namelen) || namelen > sizeof(sockaddr_un)) {
        return {};
    }
    sockaddr_un sun{};
    if (read_mem(t.tid, name, &sun, namelen) != 0) {
        return {};
    }
    std::string path(sun.sun_path,
                     ::strnlen(sun.sun_path, namelen - offsetof(sockaddr_un, sun_path)));
    std::string full = path;
    if (!vfs::is_absolute(path)) {
        std::string cwd;
        if (int e = guest_cwd(t, cwd); e != 0) {
            return {true, -static_cast<long>(e)};
        }
        full = vfs::join(cwd, path);
    }
    vfs::Resolved r;
    if (int e = vfs::resolve(table_, full, resolve_options(t, true), r); e != 0) {
        return {true, -static_cast<long>(e)};
    }
    if (r.host == path) {
        return {};
    }
    if (r.host.size() + 1 > sizeof(sun.sun_path)) {
        return deny(t, abi::lookup_syscall(t.pending.nr), t.pending.nr, ENAMETOOLONG,
                    "unix_socket.path_too_long",
                    "translated AF_UNIX path exceeds 107 bytes: " + r.host);
    }
    sockaddr_un out{};
    out.sun_family = AF_UNIX;
    std::memcpy(out.sun_path, r.host.c_str(), r.host.size() + 1);
    auto out_len = static_cast<std::uint32_t>(offsetof(sockaddr_un, sun_path) + r.host.size() + 1);
    StackWriter sw(t.tid, regs.view().sp);
    std::uint64_t new_name = 0;
    std::uint64_t new_msg = 0;
    if (int e = sw.push(&out, out_len, new_name); e != 0) {
        return {true, -static_cast<long>(e)};
    }
    std::memcpy(msg, &new_name, 8);
    std::memcpy(msg + 8, &out_len, 4);
    if (int e = sw.push(msg, sizeof(msg), new_msg); e != 0) {
        return {true, -static_cast<long>(e)};
    }
    rewrite_arg(t, regs, 1, new_msg);
    return {};
}

SyscallAction Supervisor::handle_exec(Tracee& t, arch::RegsAccess& regs, bool at_variant) {
    const auto& a = regs.view().args;
    const abi::SyscallInfo* info = abi::lookup_syscall(t.pending.nr);
    int path_arg = at_variant ? 1 : 0;
    int argv_arg = at_variant ? 2 : 1;
    std::uint64_t flags = at_variant ? a[4] : 0;

    std::string path;
    if (int e = read_cstring(t.tid, a[static_cast<std::size_t>(path_arg)], vfs::kPathMax, path);
        e != 0) {
        return {true, -static_cast<long>(e)};
    }
    std::string cwd;
    int cwd_err = guest_cwd(t, cwd);
    if (cwd_err != 0) {
        // A removed (or unmapped) working directory only matters to relative names: the kernel
        // runs an absolute path regardless, and scripts routinely delete their own directory.
        cwd = "/";
    }
    std::string guest_path;
    if (at_variant && path.empty()) {
        if ((flags & kAtEmptyPath) == 0) {
            return {true, -ENOENT};
        }
        bool is_path = false;
        if (int e = guest_fd_path(t, static_cast<int>(a[0]), guest_path, is_path); e != 0) {
            return {true, -static_cast<long>(e)};
        }
        if (!is_path) {
            return deny(t, info, t.pending.nr, EACCES, "exec.fd_not_path",
                        "executing a descriptor that is not a filesystem path (e.g. memfd) is not "
                        "supported");
        }
    } else if (path.empty()) {
        return {true, -ENOENT};
    } else if (vfs::is_absolute(path)) {
        guest_path = path;
    } else if (at_variant && static_cast<long>(static_cast<int>(a[0])) != kAtFdcwd) {
        std::string base;
        bool is_path = false;
        if (int e = guest_fd_path(t, static_cast<int>(a[0]), base, is_path); e != 0) {
            return {true, -static_cast<long>(e)};
        }
        if (!is_path) {
            return {true, -ENOTDIR};
        }
        guest_path = vfs::join(base, path);
    } else if (cwd_err != 0) {
        return {true, -static_cast<long>(cwd_err)};
    } else {
        guest_path = vfs::join(cwd, path);
    }
    if (at_variant && (flags & kAtSymlinkNofollow) != 0) {
        vfs::Resolved r;
        if (int e = vfs::resolve(table_, guest_path, resolve_options(t, false), r); e != 0) {
            return {true, -static_cast<long>(e)};
        }
        if (r.final_is_symlink) {
            return {true, -ELOOP};
        }
    }

    std::vector<std::string> argv;
    std::uint64_t argvp = a[static_cast<std::size_t>(argv_arg)];
    std::size_t total = 0;
    if (argvp != 0) {
        std::size_t i = 0;
        for (; i < kMaxExecArgs; ++i) {
            std::uint64_t ptr = 0;
            if (read_mem(t.tid, argvp + i * sizeof(std::uint64_t), &ptr, sizeof(ptr)) != 0) {
                return {true, -EFAULT};
            }
            if (ptr == 0) {
                break;
            }
            std::string s;
            int e = read_cstring(t.tid, ptr, kMaxArgStrlen, s);
            if (e != 0) {
                return {true, -static_cast<long>(e == ENAMETOOLONG ? E2BIG : e)};
            }
            total += s.size() + 1;
            if (total > kMaxExecBytes) {
                return deny(t, info, t.pending.nr, E2BIG, "exec.args_too_large",
                            "argument vector larger than the translation scratch limit (512 KiB)");
            }
            argv.push_back(std::move(s));
        }
        if (i == kMaxExecArgs) {
            return {true, -E2BIG};
        }
    }

    ExecPlan plan;
    ExecDiag diag;
    int e = exec_.plan(guest_path, argv, cwd, resolve_options(t, true), plan, diag);
    if (e != 0) {
        if (!diag.name.empty()) {
            emit(EventKind::diagnostic, Severity::warning, diag.name, diag.message, t.tid, e);
        }
        return {true, -static_cast<long>(e)};
    }
    if (plan.argv.empty()) {
        plan.argv.push_back(plan.guest_program);
    }
    std::string load_request;
    if (loader_mode()) {
        // The kernel execs the loader with the argv the program itself should see; the loader
        // maps the program and its dynamic linker from the request handed over at the exec stop.
        load_request = make_load_request(plan, guest_path);
        if (load_request.empty()) {
            return {true, -ENAMETOOLONG};
        }
        plan.host_exec = opts_.loader_path;
        if (!plan.program_argv.empty()) {
            plan.argv = plan.program_argv;
        }
    }

    StackWriter sw(t.tid, regs.view().sp);
    std::vector<std::uint64_t> ptrs;
    ptrs.reserve(plan.argv.size() + 1);
    for (const auto& s : plan.argv) {
        std::uint64_t p = 0;
        if (int we = sw.push_string(s, p); we != 0) {
            return deny(
                t, info, t.pending.nr, ENOMEM, "exec.scratch_unavailable",
                "no stack space below the tracee stack pointer for the translated argument vector");
        }
        ptrs.push_back(p);
    }
    ptrs.push_back(0);
    std::uint64_t exec_addr = 0;
    std::uint64_t argv_addr = 0;
    if (sw.push_string(plan.host_exec, exec_addr) != 0 ||
        sw.push(ptrs.data(), ptrs.size() * sizeof(std::uint64_t), argv_addr) != 0) {
        return deny(
            t, info, t.pending.nr, ENOMEM, "exec.scratch_unavailable",
            "no stack space below the tracee stack pointer for the translated argument vector");
    }
    if (at_variant) {
        rewrite_arg(t, regs, 0, static_cast<std::uint64_t>(kAtFdcwd));
        rewrite_arg(t, regs, 1, exec_addr);
        rewrite_arg(t, regs, 2, argv_addr);
        rewrite_arg(t, regs, 4, 0);
    } else {
        rewrite_arg(t, regs, 0, exec_addr);
        rewrite_arg(t, regs, 1, argv_addr);
    }
    if (sink_.wants(Severity::debug)) {
        JsonWriter w;
        w.begin_object()
            .key("program")
            .value(plan.guest_program)
            .key("via_loader")
            .value(plan.via_loader);
        w.key("loader")
            .value(plan.guest_loader)
            .key("shebang_depth")
            .value(plan.shebang_depth)
            .end_object();
        emit(EventKind::trace, Severity::debug, "exec.translated", "execve " + plan.guest_program,
             t.tid, 0, w.str());
    }
    if (loader_mode()) {
        t.load_request = std::move(load_request);
        t.load_exe = plan.guest_program;
    }
    return {};
}

void Supervisor::fixup_exit(Tracee& t, arch::RegsAccess& regs) {
    PendingSyscall& p = t.pending;
    long ret = regs.view().ret;
    switch (p.fixup) {
        case Fixup::none:
            break;
        case Fixup::emulated_return:
            regs.set_return(p.emulated);
            break;
        case Fixup::getcwd: {
            if (ret <= 0) {
                break;
            }
            std::size_t n = static_cast<std::size_t>(ret) < p.size
                                ? static_cast<std::size_t>(ret)
                                : static_cast<std::size_t>(p.size);
            std::string host(n, '\0');
            if (n == 0 || read_mem(t.tid, p.buf, host.data(), n) != 0) {
                break;
            }
            while (!host.empty() && host.back() == '\0') {
                host.pop_back();
            }
            auto guest = table_.to_guest(host);
            if (!guest) {
                regs.set_return(-ENOENT);
            } else if (guest->size() + 1 > p.size) {
                regs.set_return(-ERANGE);
            } else if (write_mem(t.tid, p.buf, guest->c_str(), guest->size() + 1) == 0) {
                regs.set_return(static_cast<long>(guest->size() + 1));
            }
            break;
        }
        case Fixup::readlink_proc: {
            if (ret <= 0 || p.size == 0) {
                break;
            }
            // The kernel truncated the host target to the guest's buffer, and a host path is longer
            // than the guest path it maps to: read the link in full, then truncate the guest path.
            std::string target;
            if (auto full = platform::read_link(p.link_host); full.is_ok()) {
                target = full.value();
            } else {
                auto n = static_cast<std::size_t>(ret);
                target.assign(n, '\0');
                if (read_mem(t.tid, p.buf, target.data(), n) != 0) {
                    break;
                }
            }
            if (target.empty() || target.front() != '/') {
                break; // "pipe:[..]" and friends are returned unchanged
            }
            std::string suffix;
            if (ends_with_deleted(target)) {
                suffix = target.substr(target.size() - kDeletedSuffixLen);
                target.resize(target.size() - kDeletedSuffixLen);
            }
            std::optional<std::string> guest;
            // The kernel truncates to the caller's buffer (readlink(1) starts with 64 bytes and
            // grows on a full buffer), so a prefix of the loader path identifies it too.
            if (p.exe_tgid != 0 && target.size() <= opts_.loader_path.size() &&
                opts_.loader_path.compare(0, target.size(), target) == 0) {
                // Loader mode: the kernel names the loader; the guest ran a program of its own.
                if (auto it = exe_of_.find(p.exe_tgid); it != exe_of_.end()) {
                    guest = it->second;
                }
            }
            if (!guest) {
                guest = table_.to_guest(target);
            }
            if (!guest) {
                regs.set_return(-EACCES);
                if (!first_report("path.magic_link_hidden", target)) {
                    break;
                }
                emit(EventKind::diagnostic, Severity::warning, "path.magic_link_hidden",
                     "readlink of a /proc magic link pointing outside the session mounts refused",
                     t.tid, EACCES);
                break;
            }
            std::string value = *guest + suffix;
            std::size_t wn =
                value.size() < p.size ? value.size() : static_cast<std::size_t>(p.size);
            if (write_mem(t.tid, p.buf, value.data(), wn) == 0) {
                regs.set_return(static_cast<long>(wn));
            }
            break;
        }
        case Fixup::stat:
        case Fixup::statx: {
            if (ret != 0 || p.buf == 0) {
                break;
            }
            bool statx = p.fixup == Fixup::statx;
            std::size_t size = statx ? kStatxSize : kStatSize;
            unsigned char kbuf[kStatxSize];
            if (read_mem(t.tid, p.buf, kbuf, size) != 0) {
                break;
            }
            unsigned char before[kStatxSize];
            std::memcpy(before, kbuf, size);
            present_stat(kbuf, statx);
            if (std::memcmp(before, kbuf, size) != 0) {
                (void)write_mem(t.tid, p.buf, kbuf, size);
            }
            break;
        }
        case Fixup::rename_links: {
            if (ret != 0 || p.link_mount == nullptr) {
                break;
            }
            if (p.link_exchange) {
                if (!p.link_src.empty()) {
                    links_.name_moved(*p.link_mount, p.link_src, p.link_from, p.link_to);
                }
                if (!p.link_dst.empty()) {
                    links_.name_moved(*p.link_mount, p.link_dst, p.link_to, p.link_from);
                }
                break;
            }
            if (!p.link_dst.empty()) {
                links_.name_dropped(*p.link_mount, p.link_dst, p.link_to);
            }
            if (!p.link_src.empty()) {
                links_.name_moved(*p.link_mount, p.link_src, p.link_from, p.link_to);
            }
            break;
        }
        case Fixup::dirents:
            present_dirents(t, ret);
            break;
        case Fixup::termios2_get: {
            // TCGETS filled the termios part; add the two speeds TCGETS2 reports after it.
            if (ret != 0) {
                break;
            }
            std::uint32_t cflag = 0;
            if (read_mem(t.tid, p.buf + termios2::cflag_offset(), &cflag, sizeof(cflag)) != 0) {
                break;
            }
            const std::uint32_t speeds[2] = {termios2::input_speed(cflag),
                                             termios2::output_speed(cflag)};
            (void)write_mem(t.tid, p.buf + termios2::ispeed_offset(), speeds, sizeof(speeds));
            break;
        }
    }
}

// A read-only open of a /proc file the host refuses to its processes, or of the status of a
// session process under an emulated identity: the stand-in content (proc_synth.hpp) is written
// to a fresh file in a private scratch directory, which the guest opens instead. The file's name
// is removed once the open returns.
bool Supervisor::proc_stand_in(Tracee& t, const TranslatedPath& tp, std::string& host_out) {
    const core::SessionConfig& c = cfg_->cfg;
    std::string_view rel = vfs::suffix_after(tp.r.guest, tp.r.mount->guest);
    if (rel.empty() || rel.front() != '/') {
        return false;
    }
    rel.remove_prefix(1);
    std::optional<std::string> content;
    auto parts = vfs::components(rel);
    auto number = [](std::string_view s) -> pid_t {
        if (s.empty() || s.size() > 9 || s.find_first_not_of("0123456789") != std::string_view::npos) {
            return 0;
        }
        return static_cast<pid_t>(std::strtol(std::string(s).c_str(), nullptr, 10));
    };
    bool status = (parts.size() == 2 && parts[1] == "status") ||
                  (parts.size() == 4 && parts[1] == "task" && parts[3] == "status");
    if (status) {
        pid_t tg = number(parts[0]);
        bool identity = c.uid.has_value() || c.gid.has_value();
        if (!identity || tg <= 0 || tracees_.count(tg) == 0) {
            return false;
        }
        std::string real;
        {
            UniqueFd fd(::open(tp.r.host.c_str(), O_RDONLY | O_CLOEXEC));
            if (!fd.valid()) {
                return false;
            }
            char buf[4096];
            ssize_t n = 0;
            while ((n = ::read(fd.get(), buf, sizeof(buf))) > 0) {
                real.append(buf, static_cast<std::size_t>(n));
            }
        }
        Creds& cr = creds_of(tracees_[tg]);
        StatusIds ids{{cr.ruid, cr.euid, cr.suid, cr.fsuid}, {cr.rgid, cr.egid, cr.sgid, cr.fsgid},
                      cr.groups};
        content = rewrite_status_ids(real, ids);
    } else {
        if (!has_proc_stand_in(rel)) {
            return false;
        }
        const char* force = std::getenv("VHDP_PROC_STAND_IN"); // "always": exercise on any host
        bool forced = force != nullptr && std::string_view(force) == "always";
        if (!forced && (::access(tp.r.host.c_str(), R_OK) == 0 || errno != EACCES)) {
            return false; // readable (or absent) on this host: the real file is used
        }
        content = synth_proc_global(rel, last_pid_);
    }
    if (!content) {
        return false;
    }
    if (proc_scratch_.empty()) {
        // Private to this supervisor: a fresh 0700 directory nobody else can have prepared, and
        // one that another session in the same process does not remove from under it.
        const char* tmp = std::getenv("TMPDIR");
        std::string dir = std::string(tmp != nullptr && tmp[0] == '/' ? tmp : "/tmp") +
                          "/.vhdp-proc.XXXXXX";
        if (::mkdtemp(dir.data()) == nullptr) {
            return false;
        }
        proc_scratch_ = std::move(dir);
    }
    std::string path = proc_scratch_ + "/" + std::to_string(++proc_scratch_seq_);
    UniqueFd out(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444));
    if (!out.valid()) {
        return false;
    }
    std::size_t done = 0;
    while (done < content->size()) {
        ssize_t n = ::write(out.get(), content->data() + done, content->size() - done);
        if (n <= 0) {
            ::unlink(path.c_str());
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    (void)t;
    host_out = std::move(path);
    return true;
}

// The guest identity (host-owned files appear owned by --uid/--gid) and, for the object of an
// emulated hard link, its recorded link count.
void Supervisor::present_stat(unsigned char* b, bool statx) {
    const core::SessionConfig& c = cfg_->cfg;
    std::size_t uid_off = statx ? kStatxUidOff : kStatUidOff;
    std::uint32_t ids[2] = {0, 0};
    std::memcpy(ids, b + uid_off, sizeof(ids));
    if (c.uid && ids[0] == host_uid_) {
        ids[0] = *c.uid;
    }
    if (c.gid && ids[1] == host_gid_) {
        ids[1] = *c.gid;
    }
    std::memcpy(b + uid_off, ids, sizeof(ids));
    if (!links_.has_objects()) {
        return;
    }
    if (statx) {
        std::uint64_t ino = 0;
        std::uint32_t dev[2] = {0, 0};
        std::memcpy(&ino, b + kStatxInoOff, sizeof(ino));
        std::memcpy(dev, b + kStatxDevMajorOff, sizeof(dev));
        if (auto count = links_.count_for(dev[0], dev[1], ino)) {
            std::uint32_t n = *count;
            std::memcpy(b + kStatxNlinkOff, &n, sizeof(n));
        }
        return;
    }
    std::uint64_t head[2] = {0, 0}; // st_dev, st_ino
    std::memcpy(head, b, sizeof(head));
    if (auto count = links_.count_for(major(head[0]), minor(head[0]), head[1])) {
#if defined(__x86_64__)
        std::uint64_t n = *count; // st_nlink follows st_ino
        std::memcpy(b + 16, &n, sizeof(n));
#else
        std::uint32_t n = *count; // generic layout: st_mode, then st_nlink
        std::memcpy(b + 20, &n, sizeof(n));
#endif
    }
}

// getdents64 records of emulated hard links carry DT_LNK from the host; they are regular files
// to the guest, and tools that trust d_type (find -type f) must see them so.
void Supervisor::present_dirents(Tracee& t, long ret) {
    PendingSyscall& p = t.pending;
    if (ret <= 0 || static_cast<std::uint64_t>(ret) > p.size ||
        static_cast<std::size_t>(ret) > kMaxDirentsScan) {
        return;
    }
    auto n = static_cast<std::size_t>(ret);
    std::vector<char> buf(n);
    if (read_mem(t.tid, p.buf, buf.data(), n) != 0) {
        return;
    }
    const std::string fd_dir = "/proc/" + std::to_string(t.tid) + "/fd/" + std::to_string(p.fd);
    std::string store; // host store directory of the listed directory's mount, once needed
    bool store_known = false;
    constexpr std::size_t kReclenOff = 16;
    constexpr std::size_t kTypeOff = 18;
    constexpr std::size_t kNameOff = 19;
    std::size_t off = 0;
    while (off + kNameOff < n) {
        std::uint16_t reclen = 0;
        std::memcpy(&reclen, buf.data() + off + kReclenOff, sizeof(reclen));
        if (reclen <= kNameOff || off + reclen > n) {
            break;
        }
        if (static_cast<unsigned char>(buf[off + kTypeOff]) == DT_LNK) {
            const char* name = buf.data() + off + kNameOff;
            std::size_t len = ::strnlen(name, reclen - kNameOff);
            char target[vfs::kPathMax];
            ssize_t tl = len < reclen - kNameOff
                             ? ::readlink((fd_dir + "/" + std::string(name, len)).c_str(), target,
                                          sizeof(target))
                             : -1;
            std::string_view id = tl > 0 && static_cast<std::size_t>(tl) < sizeof(target)
                                      ? vfs::link_target_id({target, static_cast<std::size_t>(tl)})
                                      : std::string_view{};
            if (!id.empty() && !store_known) {
                store_known = true;
                auto dir = platform::read_link(fd_dir);
                if (dir.is_ok()) {
                    if (auto guest = table_.to_guest(dir.value())) {
                        if (const vfs::Mount* m = table_.mount_for_guest(*guest)) {
                            store = vfs::link_store_path(m->host);
                        }
                    }
                }
            }
            if (!id.empty() && !store.empty()) {
                struct stat st{};
                std::string object = store + "/" + std::string(id);
                if (::lstat(object.c_str(), &st) == 0 && !S_ISDIR(st.st_mode) &&
                    !S_ISLNK(st.st_mode)) {
                    const char reg = DT_REG;
                    (void)write_mem(t.tid, p.buf + off + kTypeOff, &reg, 1);
                }
            }
        }
        off += reclen;
    }
}

SyscallAction Supervisor::dispatch(Tracee& t, arch::RegsAccess& regs,
                                   const abi::SyscallInfo& info) {
    const auto& a = regs.view().args;
    const core::SessionConfig& c = cfg_->cfg;
    bool identity = c.uid.has_value() || c.gid.has_value();
    bool fake_root = c.uid.has_value() && *c.uid == 0;
    StackWriter sw(t.tid, regs.view().sp);

    auto run = [&](const PathSpec& s, TranslatedPath* out = nullptr) -> SyscallAction {
        TranslatedPath tp;
        if (int e = translate_path(t, regs, sw, s, tp); e != 0) {
            return {true, -static_cast<long>(e)};
        }
        if (out != nullptr) {
            *out = std::move(tp);
        }
        return {};
    };
    auto stat_fixup = [&](std::uint64_t buf, Fixup kind) {
        if (identity || links_.has_objects()) {
            t.pending.active = true;
            t.pending.fixup = kind;
            t.pending.buf = buf;
        }
    };
    // Resolves a read-only query; true when the supervisor answers it (host_queryable).
    auto resolve_query = [&](PathSpec s, TranslatedPath& tp, SyscallAction& act) -> bool {
        s.resolve_only = true;
        if (int e = translate_path(t, regs, sw, s, tp); e != 0) {
            act = {true, -static_cast<long>(e)};
            return true;
        }
        if (host_queryable(tp)) {
            return true;
        }
        s.resolve_only = false;
        if (int e = apply_translation(t, regs, sw, s, tp); e != 0) {
            act = {true, -static_cast<long>(e)};
            return true;
        }
        return false;
    };
    auto stat_path = [&](const PathSpec& s, std::uint64_t buf, Fixup kind, std::uint64_t flags,
                         std::uint64_t mask) -> SyscallAction {
        if (long e = early_stat_error(kind == Fixup::statx, flags, mask); e != 0) {
            return {true, e};
        }
        TranslatedPath tp;
        SyscallAction act;
        bool answered = resolve_query(s, tp, act);
        if (act.skip) {
            return act;
        }
        if (tp.present && tp.r.mount != nullptr && !tp.r.link_id.empty()) {
            links_.note_object(*tp.r.mount, tp.r.link_id);
        }
        if (answered) {
            return {true, host_stat(t, tp.r.host, buf, kind == Fixup::statx, flags, mask)};
        }
        stat_fixup(buf, kind);
        return {};
    };
    auto access_path = [&](const PathSpec& s, std::uint64_t mode) -> SyscallAction {
        if ((mode & ~std::uint64_t{7}) != 0) {
            return {true, -EINVAL}; // only F_OK and R_OK|W_OK|X_OK
        }
        TranslatedPath tp;
        SyscallAction act;
        if (!resolve_query(s, tp, act) || act.skip) {
            return act;
        }
        long r = ::syscall(__NR_faccessat, AT_FDCWD, tp.r.host.c_str(), static_cast<int>(mode));
        return {true, r == 0 ? 0 : -static_cast<long>(errno)};
    };
    // Directory-entry operations and emulated hard links (link_store.hpp).
    auto unlink_like = [&](PathSpec s, bool removedir) -> SyscallAction {
        s.link_name = !removedir;
        TranslatedPath tp;
        SyscallAction act = run(s, &tp);
        if (act.skip || removedir || !tp.present || !tp.r.final_is_symlink ||
            tp.r.mount == nullptr) {
            return act;
        }
        std::string id = LinkStore::entry_id(tp.r.host);
        if (id.empty()) {
            return act;
        }
        return {true, -static_cast<long>(links_.unlink_name(*tp.r.mount, id, tp.r.host))};
    };
    auto rename_like = [&](PathSpec s1, PathSpec s2, std::uint64_t flags) -> SyscallAction {
        s1.link_name = true;
        s2.link_name = true;
        TranslatedPath p1;
        TranslatedPath p2;
        if (int e = translate_path(t, regs, sw, s1, p1); e != 0) {
            return {true, -static_cast<long>(e)};
        }
        if (int e = translate_path(t, regs, sw, s2, p2); e != 0) {
            return {true, -static_cast<long>(e)};
        }
        if (!p1.present || !p2.present) {
            return {};
        }
        if (p1.r.mount != p2.r.mount) {
            return {true, -EXDEV};
        }
        std::string src = p1.r.final_is_symlink ? LinkStore::entry_id(p1.r.host) : std::string();
        std::string dst = p2.r.final_is_symlink ? LinkStore::entry_id(p2.r.host) : std::string();
        if (src.empty() && dst.empty()) {
            return {};
        }
        if (src == dst) {
            if ((flags & kRenameExchange) != 0) {
                return {}; // two names of one file swap: its record is unchanged
            }
            // Two names of one file: rename(2) leaves both in place and succeeds.
            return {true, (flags & kRenameNoReplace) != 0 ? -EEXIST : 0};
        }
        t.pending.active = true;
        t.pending.fixup = Fixup::rename_links;
        t.pending.link_mount = p1.r.mount;
        t.pending.link_src = std::move(src);
        t.pending.link_dst = std::move(dst);
        t.pending.link_from = p1.r.host;
        t.pending.link_to = p2.r.host;
        t.pending.link_exchange = (flags & kRenameExchange) != 0;
        return {};
    };
    auto link_like = [&](PathSpec old_spec, PathSpec new_spec) -> SyscallAction {
        old_spec.link_name = !old_spec.follow;
        new_spec.link_name = true;
        TranslatedPath p1;
        TranslatedPath p2;
        if (int e = translate_path(t, regs, sw, old_spec, p1); e != 0) {
            return {true, -static_cast<long>(e)};
        }
        if (int e = translate_path(t, regs, sw, new_spec, p2); e != 0) {
            return {true, -static_cast<long>(e)};
        }
        if (!p1.present || !p2.present || p1.r.mount == nullptr || !p1.r.exists) {
            return {}; // the kernel reports the missing old name (ENOENT) first
        }
        if (p2.r.exists) {
            return {true, -EEXIST}; // then an existing new name, before the mount check
        }
        if (p1.r.mount != p2.r.mount) {
            return {true, -EXDEV};
        }
        std::string id = p1.r.link_id;
        if (id.empty() && p1.r.final_is_symlink) {
            id = LinkStore::entry_id(p1.r.host);
        }
        if (!id.empty()) {
            return {true, -static_cast<long>(links_.link_existing(*p1.r.mount, id, p2.r.host))};
        }
        if (!links_enabled_ || p1.r.is_dir) {
            return {}; // the kernel links, or refuses a directory with EPERM
        }
        if (p1.r.final_is_symlink) {
            // A hard link to a symlink itself: an identical symlink cannot be told apart.
            auto target = platform::read_link(p1.r.host);
            if (!target.is_ok()) {
                return {true, -EIO};
            }
            if (::symlink(target.value().c_str(), p2.r.host.c_str()) != 0) {
                return {true, -static_cast<long>(errno)};
            }
            return {true, 0};
        }
        return {true, -static_cast<long>(links_.link_new(*p1.r.mount, p1.r.host, p2.r.host))};
    };
    auto open_path = [&](PathSpec s, std::uint64_t flags) -> SyscallAction {
        bool read_only = (flags & O_ACCMODE) == O_RDONLY &&
                         (flags & (O_CREAT | O_TRUNC | O_TMPFILE | O_DIRECTORY | O_PATH)) == 0;
        if (!read_only || c.proc != core::ProcMode::host) {
            return run(s);
        }
        s.resolve_only = true;
        TranslatedPath tp;
        if (int e = translate_path(t, regs, sw, s, tp); e != 0) {
            return {true, -static_cast<long>(e)};
        }
        std::string stand_in;
        if (tp.present && tp.r.exists && tp.r.mount != nullptr &&
            tp.r.mount->kind == vfs::MountKind::proc && !tp.r.proc_magic &&
            proc_stand_in(t, tp, stand_in)) {
            tp.r.host = stand_in;
            t.pending.active = true;
            t.pending.scratch = std::move(stand_in);
        }
        s.resolve_only = false;
        if (int e = apply_translation(t, regs, sw, s, tp); e != 0) {
            if (!t.pending.scratch.empty()) {
                ::unlink(t.pending.scratch.c_str());
                t.pending.scratch.clear();
            }
            return {true, -static_cast<long>(e)};
        }
        return {};
    };
    auto chown_like = [&](const PathSpec& s) -> SyscallAction {
        TranslatedPath tp;
        SyscallAction act = run(s, &tp);
        if (act.skip || !fake_root || !guest_is_root(t)) {
            return act;
        }
        if (tp.present && !tp.r.exists) {
            return {true, -ENOENT};
        }
        return {true, 0}; // emulated for the fake-root identity
    };

    switch (info.handler) {
        case Handler::none:
            if (info.nr == __NR_fstat) {
                stat_fixup(a[1], Fixup::stat);
            }
            // A guest installing a seccomp filter of its own: a seccomp SIGSYS in its thread
            // group may now be its filter's (handle_host_seccomp_trap).
            if ((info.nr == __NR_prctl && a[0] == PR_SET_SECCOMP && a[1] == kSeccompModeFilter)
#ifdef __NR_seccomp
                || (info.nr == __NR_seccomp && a[0] == kSeccompSetModeFilter)
#endif
            ) {
                guest_filtered_.insert(tgid_of(t));
            }
            if (links_enabled_ && info.nr == __NR_getdents64 && links_visible()) {
                t.pending.active = true;
                t.pending.fixup = Fixup::dirents;
                t.pending.fd = static_cast<int>(a[0]);
                t.pending.buf = a[1];
                t.pending.size = a[2] & 0xffffffffu;
            }
#ifdef __NR_getgroups
            if (identity && info.nr == __NR_getgroups) {
                return handle_identity(t, regs, info);
            }
#endif
            return {};

        case Handler::open:
            return open_path(open_spec(0, -1, a[1]), a[1]);
        case Handler::creat:
            return run(spec(0, -1, true, true, true));
        case Handler::openat:
            return open_path(open_spec(1, 0, a[2]), a[2]);

        case Handler::stat:
            return stat_path(spec(0, -1, true), a[1], Fixup::stat, 0, 0);
        case Handler::lstat:
            return stat_path(spec(0, -1, false), a[1], Fixup::stat, kAtSymlinkNofollow, 0);
        case Handler::newfstatat: {
            PathSpec s = spec(1, 0, (a[3] & kAtSymlinkNofollow) == 0);
            s.empty_ok = (a[3] & kAtEmptyPath) != 0;
            return stat_path(s, a[2], Fixup::stat, a[3], 0);
        }
        case Handler::statx: {
            PathSpec s = spec(1, 0, (a[2] & kAtSymlinkNofollow) == 0);
            s.empty_ok = (a[2] & kAtEmptyPath) != 0;
            return stat_path(s, a[4], Fixup::statx, a[2], a[3]);
        }
        case Handler::access:
            return access_path(spec(0, -1, true, (a[1] & W_OK) != 0), a[1]);
        case Handler::faccessat:
            return access_path(spec(1, 0, true, (a[2] & W_OK) != 0), a[2]);
        case Handler::faccessat2: {
            PathSpec s = spec(1, 0, (a[3] & kAtSymlinkNofollow) == 0, (a[2] & W_OK) != 0);
            s.empty_ok = (a[3] & kAtEmptyPath) != 0;
            if ((a[3] & ~(kAtEaccess | kAtSymlinkNofollow | kAtEmptyPath)) != 0 ||
                (a[2] & ~std::uint64_t{7}) != 0) {
                return {true, -EINVAL};
            }
            if (a[3] == 0) {
                return access_path(s, a[2]); // faccessat(2) semantics exactly
            }
            return run(s);
        }

        case Handler::readlink:
        case Handler::readlinkat: {
            bool at = info.handler == Handler::readlinkat;
            PathSpec s = at ? spec(1, 0, false) : spec(0, -1, false);
            s.empty_ok = at;
            std::uint64_t buf = at ? a[2] : a[1];
            std::uint64_t size = (at ? a[3] : a[2]) & 0xffffffffu;
            if (static_cast<std::int32_t>(size) <= 0) {
                return {true, -EINVAL};
            }
            TranslatedPath tp;
            SyscallAction act;
            if (resolve_query(s, tp, act)) {
                return act.skip ? act : SyscallAction{true, host_readlink(t, tp.r.host, buf, size)};
            }
            if (!act.skip && tp.present && tp.r.mount != nullptr &&
                tp.r.mount->kind == vfs::MountKind::proc) {
                t.pending.active = true;
                t.pending.fixup = Fixup::readlink_proc;
                t.pending.buf = at ? a[2] : a[1];
                t.pending.size = (at ? a[3] : a[2]) & 0xffffffffu;
                t.pending.link_host = tp.r.host;
                if (loader_mode()) {
                    t.pending.exe_tgid = proc_exe_owner(tp.r.guest, tgid_of(t));
                }
            }
            return act;
        }
        case Handler::getcwd:
            t.pending.active = true;
            t.pending.fixup = Fixup::getcwd;
            t.pending.buf = a[0];
            t.pending.size = a[1];
            return {};
        case Handler::chdir:
            return run(spec(0, -1, true));

        case Handler::mkdir:
            return run(spec(0, -1, false, false, true));
        case Handler::mkdirat:
            return run(spec(1, 0, false, false, true));
        case Handler::mknod:
            return run(spec(0, -1, false, false, true));
        case Handler::mknodat:
            return run(spec(1, 0, false, false, true));
        case Handler::rmdir:
            return run(spec(0, -1, false, true));
        case Handler::unlink:
            return unlink_like(spec(0, -1, false, true), false);
        case Handler::unlinkat:
            return unlink_like(spec(1, 0, false, true), (a[2] & kAtRemoveDir) != 0);
        case Handler::rename:
            return rename_like(spec(0, -1, false, true), spec(1, -1, false, true, true), 0);
        case Handler::renameat:
            return rename_like(spec(1, 0, false, true), spec(3, 2, false, true, true), 0);
        case Handler::renameat2:
            return rename_like(spec(1, 0, false, true), spec(3, 2, false, true, true), a[4]);
        case Handler::link:
            return link_like(spec(0, -1, false), spec(1, -1, false, false, true));
        case Handler::linkat: {
            PathSpec old = spec(1, 0, (a[4] & kAtSymlinkFollow) != 0);
            old.empty_ok = (a[4] & kAtEmptyPath) != 0;
            return link_like(old, spec(3, 2, false, false, true));
        }
        case Handler::symlink:
            return run(spec(1, -1, false, false, true));
        case Handler::symlinkat:
            return run(spec(2, 1, false, false, true));

        case Handler::chmod:
            return run(spec(0, -1, true, true));
        case Handler::fchmodat:
            return run(spec(1, 0, true, true));
        case Handler::fchmodat2: {
            PathSpec s = spec(1, 0, (a[3] & kAtSymlinkNofollow) == 0, true);
            s.empty_ok = (a[3] & kAtEmptyPath) != 0;
            return run(s);
        }
        case Handler::chown:
            return chown_like(spec(0, -1, true, true));
        case Handler::lchown:
            return chown_like(spec(0, -1, false, true));
        case Handler::fchownat: {
            PathSpec s = spec(1, 0, (a[4] & kAtSymlinkNofollow) == 0, true);
            s.empty_ok = (a[4] & kAtEmptyPath) != 0;
            return chown_like(s);
        }
        case Handler::utime:
        case Handler::utimes:
            return run(spec(0, -1, true, true));
        case Handler::futimesat:
        case Handler::utimensat: {
            if (a[1] == 0) {
                return handle_fd_meta(t, regs, info, static_cast<int>(a[0]), false);
            }
            bool nofollow = info.handler == Handler::utimensat && (a[3] & kAtSymlinkNofollow) != 0;
            PathSpec s = spec(1, 0, !nofollow, true);
            s.empty_ok = info.handler == Handler::utimensat && (a[3] & kAtEmptyPath) != 0;
            return run(s);
        }
        case Handler::truncate:
            return run(spec(0, -1, true, true));
        case Handler::statfs:
            return run(spec(0, -1, true));
        case Handler::xattr_get:
        case Handler::xattr_lget: {
            bool list = false;
#ifdef __NR_listxattr
            list = info.nr == __NR_listxattr;
#endif
#ifdef __NR_llistxattr
            list = list || info.nr == __NR_llistxattr;
#endif
            if (!list) {
                std::string name;
                int e = read_cstring(t.tid, a[1], 256, name);
                if (e == 0 && name.empty()) {
                    e = ERANGE;
                }
                if (e != 0) {
                    return {true, -static_cast<long>(e == ENAMETOOLONG ? ERANGE : e)};
                }
            }
            TranslatedPath tp;
            SyscallAction act;
            if (!resolve_query(spec(0, -1, info.handler == Handler::xattr_get), tp, act) ||
                act.skip) {
                return act;
            }
            return {true, host_xattr(t, info, tp.r.host, regs)};
        }
        case Handler::xattr_set:
            return run(spec(0, -1, true, true));
        case Handler::xattr_lset:
            return run(spec(0, -1, false, true));
        case Handler::inotify_add_watch:
            return run(spec(1, -1, (a[2] & kInDontFollow) == 0));
        case Handler::fanotify_mark: {
            PathSpec s = spec(4, 3, (a[1] & kFanMarkDontFollow) == 0);
            s.null_ok = true;
            return run(s);
        }
        case Handler::fd_meta_write:
            return handle_fd_meta(t, regs, info, static_cast<int>(a[0]), false);
        case Handler::fchown_fd:
            return handle_fd_meta(t, regs, info, static_cast<int>(a[0]), true);
        case Handler::ioctl: {
            // Only reached for the termios2 requests, and only on a host that refuses them
            // (termios2.hpp): the original request does the same job on the same buffer.
            if (!termios2_refused_) {
                return {};
            }
            const auto request = static_cast<std::uint32_t>(a[1]);
            std::uint32_t classic = termios2::classic_request(request);
            if (classic == 0) {
                return {};
            }
            rewrite_arg(t, regs, 1, classic);
            if (termios2::is_get(request) && a[2] != 0) {
                t.pending.fixup = Fixup::termios2_get;
                t.pending.buf = a[2];
            }
            return {};
        }

        case Handler::execve:
            return handle_exec(t, regs, false);
        case Handler::execveat:
            return handle_exec(t, regs, true);

        case Handler::socket:
            if (c.network == core::NetworkPolicy::none && static_cast<int>(a[0]) != AF_UNIX) {
                return deny(t, &info, info.nr, EACCES, "network.denied",
                            "--network none: only AF_UNIX sockets may be created", Severity::info);
            }
            if (audit_refused_ && static_cast<int>(a[0]) == AF_NETLINK &&
                static_cast<int>(a[2]) == NETLINK_AUDIT) {
                return deny(t, &info, info.nr, EPROTONOSUPPORT, "audit.unavailable",
                            "the host refuses audit sockets; answered as a kernel without audit",
                            Severity::debug);
            }
            return {};
        case Handler::connect:
            return handle_sockaddr(t, regs, 1, 2, false);
        case Handler::bind:
            return handle_sockaddr(t, regs, 1, 2, true);
        case Handler::sendto:
            return a[4] != 0 ? handle_sockaddr(t, regs, 4, 5, false) : SyscallAction{};
        case Handler::sendmsg:
            return handle_sendmsg(t, regs, false);
        case Handler::sendmmsg:
            return handle_sendmsg(t, regs, true);

        case Handler::clone:
            if ((a[0] & kCloneUntraced) != 0) {
                // CLONE_UNTRACED would create a child outside the supervisor.
                regs.set_arg(0, a[0] & ~kCloneUntraced);
            }
            return {};
        case Handler::kill:
        case Handler::tkill:
        case Handler::tgkill:
        case Handler::sigqueue:
        case Handler::tgsigqueue:
        case Handler::pidfd_open:
            return handle_signal_target(t, info, regs);

        case Handler::getuid:
        case Handler::geteuid:
        case Handler::getgid:
        case Handler::getegid:
        case Handler::getresuid:
        case Handler::getresgid:
        case Handler::setid:
            // Without a configured guest identity these run on the host credentials unchanged.
            return identity ? handle_identity(t, regs, info) : SyscallAction{};

        case Handler::deny_eperm:
        case Handler::deny_enosys:
        case Handler::deny_eopnotsupp:
        case Handler::deny_eacces: {
            int err = info.handler == Handler::deny_eperm        ? EPERM
                      : info.handler == Handler::deny_enosys     ? ENOSYS
                      : info.handler == Handler::deny_eopnotsupp ? EOPNOTSUPP
                                                                 : EACCES;
            std::string_view n = info.name;
            Severity sev = (n == "clone3" || n == "openat2") ? Severity::debug : Severity::warning;
            return deny(t, &info, info.nr, err,
                        info.cls == abi::SyscallClass::unsupported ? "syscall.unsupported"
                                                                   : "syscall.denied",
                        denial_reason(info), sev);
        }
    }
    return {};
}

} // namespace vhdp::rootless
