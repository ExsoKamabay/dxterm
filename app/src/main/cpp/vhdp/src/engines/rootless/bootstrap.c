#define _GNU_SOURCE
#include "engines/rootless/bootstrap.h"

#include "arch/raw_syscall.h"

#include <asm/unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/prctl.h>
#include <signal.h>
#include <sys/ioctl.h>

#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x00001000
#endif
#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif
#define VHDP_SECCOMP_MODE_FILTER 2 /* SECCOMP_MODE_FILTER for PR_SET_SECCOMP */
#define VHDP_KERNEL_SIGSET_SIZE 8
#define VHDP_KERNEL_NSIG 64
#define VHDP_HIGH_FD_BASE 64

/* Sanitizer runtimes are not fork-safe when the child is created by a raw clone;
 * the bootstrap only touches memory prepared by the parent. */
#if defined(__clang__) || defined(__GNUC__)
#define VHDP_CHILD_ATTRS __attribute__((noinline, no_sanitize("thread")))
#else
#define VHDP_CHILD_ATTRS
#endif

static long sys6(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    return vhdp_raw_syscall6(nr, a1, a2, a3, a4, a5, a6);
}

VHDP_CHILD_ATTRS
static void report_and_exit(const struct vhdp_bootstrap* b, int32_t stage, long err) {
    int32_t msg[2];
    msg[0] = stage;
    msg[1] = (int32_t)(-err);
    (void)sys6(__NR_write, b->error_fd, (long)msg, (long)sizeof(msg), 0, 0, 0);
    (void)sys6(__NR_exit_group, 127, 0, 0, 0, 0, 0);
    for (;;) {
        (void)sys6(__NR_exit_group, 127, 0, 0, 0, 0, 0);
    }
}

VHDP_CHILD_ATTRS
static void child_main(const struct vhdp_bootstrap* b) {
    long r;

    /* The child of a non-dumpable process (an Android app embedding libvhdp) cannot be traced
     * by it until it is dumpable again; execve recomputes the flag either way. The tracer
     * attaches once told so. */
    (void)sys6(__NR_prctl, PR_SET_DUMPABLE, 1, 0, 0, 0, 0);
    if (b->ready_fd >= 0) {
        char one = 1;
        (void)sys6(__NR_write, b->ready_fd, (long)&one, 1, 0, 0, 0);
    }

    if (b->reset_signals) {
        /* Kernel struct sigaction: an all-zero struct is SIG_DFL, no flags, empty mask. */
        unsigned long dfl[4] = {0, 0, 0, 0};
        for (long sig = 1; sig <= VHDP_KERNEL_NSIG; ++sig) {
            if (sig == SIGKILL || sig == SIGSTOP) {
                continue;
            }
            /* EINVAL for signals the kernel reserves is expected and ignored. */
            (void)sys6(__NR_rt_sigaction, sig, (long)dfl, 0, VHDP_KERNEL_SIGSET_SIZE, 0, 0);
        }
        unsigned long empty = 0;
        r = sys6(__NR_rt_sigprocmask, SIG_SETMASK, (long)&empty, 0, VHDP_KERNEL_SIGSET_SIZE, 0, 0);
        if (vhdp_raw_is_error(r)) {
            report_and_exit(b, VHDP_BOOT_STAGE_SIGNALS, r);
        }
    }

    if (b->new_session) {
        r = sys6(__NR_setsid, 0, 0, 0, 0, 0, 0);
        if (vhdp_raw_is_error(r)) {
            report_and_exit(b, VHDP_BOOT_STAGE_SESSION, r);
        }
        if (b->ctty_fd >= 0) {
            r = sys6(__NR_ioctl, b->ctty_fd, TIOCSCTTY, 0, 0, 0, 0);
            if (vhdp_raw_is_error(r)) {
                report_and_exit(b, VHDP_BOOT_STAGE_SESSION, r);
            }
        }
    }

    /* Move sources out of the 0..2 range first so overlapping assignments are safe. */
    long high[3] = {-1, -1, -1};
    for (int i = 0; i < 3; ++i) {
        if (b->stdio_fds[i] >= 0) {
            r = sys6(__NR_fcntl, b->stdio_fds[i], F_DUPFD_CLOEXEC, VHDP_HIGH_FD_BASE, 0, 0, 0);
            if (vhdp_raw_is_error(r)) {
                report_and_exit(b, VHDP_BOOT_STAGE_STDIO, r);
            }
            high[i] = r;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (high[i] >= 0) {
            r = sys6(__NR_dup3, high[i], i, 0, 0, 0, 0);
            if (vhdp_raw_is_error(r)) {
                report_and_exit(b, VHDP_BOOT_STAGE_STDIO, r);
            }
        }
    }

    /* Wait for the tracer. EOF means the supervisor is gone. */
    char byte = 0;
    for (;;) {
        r = sys6(__NR_read, b->sync_fd, (long)&byte, 1, 0, 0, 0);
        if (r == -EINTR) {
            continue;
        }
        if (r != 1) {
            report_and_exit(b, VHDP_BOOT_STAGE_SYNC, r == 0 ? -EPIPE : r);
        }
        break;
    }

    r = sys6(__NR_chdir, (long)b->host_cwd, 0, 0, 0, 0, 0);
    if (vhdp_raw_is_error(r)) {
        report_and_exit(b, VHDP_BOOT_STAGE_CHDIR, r);
    }

    /* Everything above 2 becomes close-on-exec (error_fd stays usable until exec). */
#ifdef __NR_close_range
    r = sys6(__NR_close_range, 3, 0xffffffffL, CLOSE_RANGE_CLOEXEC, 0, 0, 0);
#else
    r = -ENOSYS;
#endif
    if (vhdp_raw_is_error(r)) {
        for (long fd = 3; fd < (long)b->max_fd; ++fd) {
            (void)sys6(__NR_fcntl, fd, F_SETFD, FD_CLOEXEC, 0, 0, 0);
        }
    }

    r = sys6(__NR_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0, 0);
    if (vhdp_raw_is_error(r)) {
        report_and_exit(b, VHDP_BOOT_STAGE_NO_NEW_PRIVS, r);
    }

    if (b->seccomp != NULL) {
        /* prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER) rather than seccomp(2): same effect for a
         * filter without flags, and prctl is callable under every host policy we run under,
         * whereas the seccomp syscall itself is outside some (an Android app's is one). */
        r = sys6(__NR_prctl, PR_SET_SECCOMP, VHDP_SECCOMP_MODE_FILTER, (long)b->seccomp, 0, 0, 0);
        if (vhdp_raw_is_error(r)) {
            report_and_exit(b, VHDP_BOOT_STAGE_SECCOMP, r);
        }
    }

    r = sys6(__NR_execve, (long)b->exec_path, (long)b->argv, (long)b->envp, 0, 0, 0);
    report_and_exit(b, VHDP_BOOT_STAGE_EXEC, r);
}

long vhdp_bootstrap_spawn(const struct vhdp_bootstrap* b, int32_t* pidfd) {
    int32_t fd = -1;
    /* Raw clone behaves like fork() without running libc atfork handlers in the
     * child. clone(flags, stack=0, parent_tid, ...): on x86_64 the 3rd argument is
     * parent_tid and on aarch64 as well, so CLONE_PIDFD writes the pidfd there. */
    long pid = sys6(__NR_clone, SIGCHLD | CLONE_PIDFD, 0, (long)&fd, 0, 0, 0);
    if (pid == -EINVAL) {
        fd = -1;
        pid = sys6(__NR_clone, SIGCHLD, 0, 0, 0, 0, 0);
    }
    if (pid == 0) {
        child_main(b);
        /* not reached */
        (void)sys6(__NR_exit_group, 127, 0, 0, 0, 0, 0);
    }
    if (pidfd != NULL) {
        *pidfd = pid > 0 ? fd : -1;
    }
    return pid;
}
