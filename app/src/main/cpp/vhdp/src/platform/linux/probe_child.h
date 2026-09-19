/*
 * Disposable probe children (C17, raw syscalls only, see bootstrap.h for the
 * post-fork safety rules). Every probe runs in a freshly cloned child; namespace
 * probes create private user+mount namespaces that disappear with the child and
 * never modify host mounts or configuration.
 */
#ifndef VHDP_PLATFORM_PROBE_CHILD_H
#define VHDP_PLATFORM_PROBE_CHILD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Child makes itself dumpable -- the child of a non-dumpable process, such as an Android app,
 * cannot be traced by it otherwise -- writes one byte to ready_fd, blocks reading sync_fd, then
 * exits 0. Returns pid or -errno. */
long vhdp_probe_spawn_waiter(int32_t sync_fd, int32_t ready_fd);

/* Child installs an allow-all seccomp filter under no_new_privs.
 * Exit status 0 on success, otherwise 1 + errno (capped at 254). */
long vhdp_probe_spawn_seccomp(void);

#define VHDP_OPENAT2_PROBE_AVAILABLE 0
#define VHDP_OPENAT2_PROBE_ENOSYS 1

/* Child resets SIGSYS to its default action and issues openat2(-1, NULL, NULL, 0).
 * Exit status VHDP_OPENAT2_PROBE_AVAILABLE when the kernel answers with anything
 * but ENOSYS, VHDP_OPENAT2_PROBE_ENOSYS otherwise. When an inherited seccomp filter
 * traps the call (Android app processes) the child is killed by SIGSYS instead; the
 * default disposition keeps a crash handler inherited from the caller out of it.
 * Returns pid or -errno (-ENOSYS when the headers do not define openat2). */
long vhdp_probe_spawn_openat2(void);

#define VHDP_NS_PROBE_USERNS 1
#define VHDP_NS_PROBE_MOUNTNS 2
#define VHDP_NS_PROBE_TMPFS 4
#define VHDP_NS_PROBE_DEVPTS 8
#define VHDP_NS_PROBE_PROC 16

/* Child probes unprivileged user/mount namespaces and tmpfs/devpts/proc mounts
 * inside them. Exit status is a VHDP_NS_PROBE_* bitmask.
 *
 * mountpoint is an existing, empty, caller-owned directory to mount the probe
 * tmpfs on, and the caller removes it afterwards. It is a parameter rather than
 * a fixed path because the probe must never mount over a directory anything
 * else uses: see the comment in userns_child(). */
long vhdp_probe_spawn_userns(uint32_t host_uid, uint32_t host_gid, const char* mountpoint);

#ifdef __cplusplus
}
#endif

#endif
