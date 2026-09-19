// Native-architecture syscall classification table (generated from syscalls.def).
//
// Every syscall number known to the build target's <asm/unistd.h> has an entry.
// Numbers without an entry are *unknown* and are denied with ENOSYS plus a
// diagnostic; nothing is passed through by default.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace vhdp::abi {

enum class SyscallClass : std::uint8_t {
    pass_through, // executed by the host kernel unchanged
    translated,   // arguments/results rewritten (paths, pids, flags) then executed
    emulated,     // result synthesised by the supervisor (e.g. guest uid)
    denied,       // refused by policy with a fixed errno
    unsupported,  // would need emulation that is not implemented; refused with errno
};

enum class Handler : std::uint16_t {
    none,
    // path syscalls
    open,
    openat,
    creat,
    stat,
    lstat,
    newfstatat,
    statx,
    access,
    faccessat,
    faccessat2,
    readlink,
    readlinkat,
    chdir,
    getcwd,
    mkdir,
    mkdirat,
    mknod,
    mknodat,
    rmdir,
    unlink,
    unlinkat,
    rename,
    renameat,
    renameat2,
    link,
    linkat,
    symlink,
    symlinkat,
    chmod,
    fchmodat,
    fchmodat2,
    chown,
    lchown,
    fchownat,
    utime,
    utimes,
    futimesat,
    utimensat,
    truncate,
    statfs,
    xattr_get,
    xattr_lget,
    xattr_set,
    xattr_lset,
    inotify_add_watch,
    fanotify_mark,
    execve,
    execveat,
    fd_meta_write,
    fchown_fd,
    // sockets
    socket,
    connect,
    bind,
    sendto,
    sendmsg,
    sendmmsg,
    // process / signals
    clone,
    kill,
    tkill,
    tgkill,
    sigqueue,
    tgsigqueue,
    pidfd_open,
    // identity
    getuid,
    geteuid,
    getgid,
    getegid,
    getresuid,
    getresgid,
    setid,
    // fixed denials
    deny_eperm,
    deny_enosys,
    deny_eopnotsupp,
    deny_eacces,
};

struct SyscallInfo {
    long nr;
    const char* name;
    SyscallClass cls;
    Handler handler;
};

// nullptr for numbers outside the table.
const SyscallInfo* lookup_syscall(long nr) noexcept;
std::span<const SyscallInfo> all_syscalls() noexcept;
const char* class_name(SyscallClass c) noexcept;
long lookup_nr(std::string_view name) noexcept; // -1 if unknown on this architecture

} // namespace vhdp::abi
