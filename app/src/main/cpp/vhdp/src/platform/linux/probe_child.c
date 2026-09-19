#define _GNU_SOURCE
#include "platform/linux/probe_child.h"

#include "arch/raw_syscall.h"

#include <asm/unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/prctl.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <sys/mount.h>

#if defined(__clang__) || defined(__GNUC__)
#define VHDP_CHILD_ATTRS __attribute__((noinline, no_sanitize("thread")))
#else
#define VHDP_CHILD_ATTRS
#endif

#define PROBE_SECCOMP_MODE_FILTER 2 /* SECCOMP_MODE_FILTER for PR_SET_SECCOMP */

static long sys6(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    return vhdp_raw_syscall6(nr, a1, a2, a3, a4, a5, a6);
}

VHDP_CHILD_ATTRS
static void child_exit(long code) {
    for (;;) {
        (void)sys6(__NR_exit_group, code, 0, 0, 0, 0, 0);
    }
}

static long raw_fork(void) {
    return sys6(__NR_clone, SIGCHLD, 0, 0, 0, 0, 0);
}

long vhdp_probe_spawn_waiter(int32_t sync_fd, int32_t ready_fd) {
    long pid = raw_fork();
    if (pid == 0) {
        char b = 0;
        char one = 1;
        long r;
        (void)sys6(__NR_prctl, PR_SET_DUMPABLE, 1, 0, 0, 0, 0);
        (void)sys6(__NR_write, ready_fd, (long)&one, 1, 0, 0, 0);
        do {
            r = sys6(__NR_read, sync_fd, (long)&b, 1, 0, 0, 0);
        } while (r == -EINTR);
        child_exit(0);
    }
    return pid;
}

/* Layout-compatible with struct sock_filter / sock_fprog. */
struct probe_sock_filter {
    uint16_t code;
    uint8_t jt;
    uint8_t jf;
    uint32_t k;
};
struct probe_sock_fprog {
    uint16_t len;
    struct probe_sock_filter* filter;
};

long vhdp_probe_spawn_seccomp(void) {
    long pid = raw_fork();
    if (pid == 0) {
        struct probe_sock_filter allow = {0x06 /* BPF_RET|BPF_K */, 0, 0,
                                          0x7fff0000U /* SECCOMP_RET_ALLOW */};
        struct probe_sock_fprog prog = {1, &allow};
        long r = sys6(__NR_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0, 0);
        if (vhdp_raw_is_error(r)) {
            child_exit(1 + (-r > 253 ? 253 : -r));
        }
        /* Same installation path as the tracee bootstrap uses (prctl, not seccomp(2)). */
        r = sys6(__NR_prctl, PR_SET_SECCOMP, PROBE_SECCOMP_MODE_FILTER, (long)&prog, 0, 0, 0);
        child_exit(vhdp_raw_is_error(r) ? 1 + (-r > 253 ? 253 : -r) : 0);
    }
    return pid;
}

#ifdef __NR_openat2
VHDP_CHILD_ATTRS
static long openat2_child(void) {
    /* Kernel struct sigaction {handler, flags, restorer, mask} is four machine words on
     * x86_64 and aarch64; all zero is SIG_DFL with an empty mask. The kernel sigset is 8 bytes. */
    unsigned long dfl[4] = {0, 0, 0, 0};
    (void)sys6(__NR_rt_sigaction, SIGSYS, (long)dfl, 0, 8, 0, 0);
    long r = sys6(__NR_openat2, -1, 0, 0, 0, 0, 0);
    return r == -ENOSYS ? VHDP_OPENAT2_PROBE_ENOSYS : VHDP_OPENAT2_PROBE_AVAILABLE;
}
#endif

long vhdp_probe_spawn_openat2(void) {
#ifdef __NR_openat2
    long pid = raw_fork();
    if (pid == 0) {
        child_exit(openat2_child());
    }
    return pid;
#else
    return -ENOSYS;
#endif
}

/* Formats "0 <id> 1" into buf; returns length. */
static long format_map(char* buf, uint32_t id) {
    char digits[16];
    long n = 0;
    do {
        digits[n++] = (char)('0' + (id % 10));
        id /= 10;
    } while (id != 0 && n < (long)sizeof(digits));
    long len = 0;
    buf[len++] = '0';
    buf[len++] = ' ';
    while (n > 0) {
        buf[len++] = digits[--n];
    }
    buf[len++] = ' ';
    buf[len++] = '1';
    return len;
}

VHDP_CHILD_ATTRS
static int write_file(const char* path, const char* data, long len) {
    long fd = sys6(__NR_openat, AT_FDCWD, (long)path, O_WRONLY | O_CLOEXEC, 0, 0, 0);
    if (vhdp_raw_is_error(fd)) {
        return 0;
    }
    long r = sys6(__NR_write, fd, (long)data, len, 0, 0, 0);
    (void)sys6(__NR_close, fd, 0, 0, 0, 0, 0);
    return r == len;
}

/* out = a "/" b, truncated to cap. No libc: this runs in a bare forked child. */
VHDP_CHILD_ATTRS
static void join_path(char* out, long cap, const char* a, const char* b) {
    long n = 0;
    while (*a != '\0' && n < cap - 1) {
        out[n++] = *a++;
    }
    if (n < cap - 1) {
        out[n++] = '/';
    }
    while (*b != '\0' && n < cap - 1) {
        out[n++] = *b++;
    }
    out[n] = '\0';
}

VHDP_CHILD_ATTRS
static long userns_child(uint32_t uid, uint32_t gid, const char* mountpoint) {
    long bits = 0;
    long r = sys6(__NR_unshare, CLONE_NEWUSER | CLONE_NEWNS, 0, 0, 0, 0, 0);
    if (vhdp_raw_is_error(r)) {
        return bits;
    }
    bits |= VHDP_NS_PROBE_USERNS;
    char map[40];
    (void)write_file("/proc/self/setgroups", "deny", 4);
    if (!write_file("/proc/self/uid_map", map, format_map(map, uid)) ||
        !write_file("/proc/self/gid_map", map, format_map(map, gid))) {
        return bits;
    }
    /* Make every mount private first so nothing can propagate to the host. */
    r = sys6(__NR_mount, (long)"none", (long)"/", 0, MS_REC | MS_PRIVATE, 0, 0);
    if (vhdp_raw_is_error(r)) {
        return bits;
    }
    bits |= VHDP_NS_PROBE_MOUNTNS;
    /* Mount on the caller's own throwaway directory, never on a well-known path
     * such as /tmp. The unshare above is supposed to make that harmless, but it
     * only is when the kernel really performed it: under a ptrace supervisor
     * like PRoot, unshare() and mount() are answered with a fake success and the
     * mountpoint is recorded in the supervisor's own table, so mounting over
     * /tmp made every later access to /tmp fail with ENOENT for the rest of that
     * session, with the real directory still on disk. */
    char sub[256];
    r = sys6(__NR_mount, (long)"vhdp-probe", (long)mountpoint, (long)"tmpfs", MS_NOSUID | MS_NODEV,
             (long)"size=64k", 0);
    if (vhdp_raw_is_error(r)) {
        return bits;
    }
    bits |= VHDP_NS_PROBE_TMPFS;
    join_path(sub, (long)sizeof(sub), mountpoint, "pts");
    (void)sys6(__NR_mkdirat, AT_FDCWD, (long)sub, 0700, 0, 0, 0);
    r = sys6(__NR_mount, (long)"devpts", (long)sub, (long)"devpts", MS_NOSUID | MS_NOEXEC,
             (long)"newinstance,ptmxmode=0600", 0);
    if (!vhdp_raw_is_error(r)) {
        bits |= VHDP_NS_PROBE_DEVPTS;
    }
    /* procfs requires a PID namespace owned by this user namespace. */
    join_path(sub, (long)sizeof(sub), mountpoint, "proc");
    (void)sys6(__NR_mkdirat, AT_FDCWD, (long)sub, 0700, 0, 0, 0);
    r = sys6(__NR_unshare, CLONE_NEWPID, 0, 0, 0, 0, 0);
    if (!vhdp_raw_is_error(r)) {
        long pid = raw_fork();
        if (pid == 0) {
            long m = sys6(__NR_mount, (long)"proc", (long)sub, (long)"proc",
                          MS_NOSUID | MS_NODEV | MS_NOEXEC, 0, 0);
            child_exit(vhdp_raw_is_error(m) ? 1 : 0);
        }
        if (pid > 0) {
            int status = 0;
            long w;
            do {
                w = sys6(__NR_wait4, pid, (long)&status, 0, 0, 0, 0);
            } while (w == -EINTR);
            /* exited normally with status 0 */
            if (w == pid && (status & 0xff7f) == 0) {
                bits |= VHDP_NS_PROBE_PROC;
            }
        }
    }
    return bits;
}

long vhdp_probe_spawn_userns(uint32_t host_uid, uint32_t host_gid, const char* mountpoint) {
    long pid = raw_fork();
    if (pid == 0) {
        child_exit(userns_child(host_uid, host_gid, mountpoint));
    }
    return pid;
}
