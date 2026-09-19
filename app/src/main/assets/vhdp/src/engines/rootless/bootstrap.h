/*
 * Tracee bootstrap: code that runs in the child between clone() and execve().
 *
 * The parent (supervisor thread) prepares every buffer before cloning. The child
 * uses only vhdp_raw_syscall6: no libc wrappers, no errno/TLS, no allocation, no
 * locks, no atfork handlers. This keeps the path async-signal-safe even when the
 * embedding process is multithreaded (docs/adr/0001-language-split.md).
 */
#ifndef VHDP_ROOTLESS_BOOTSTRAP_H
#define VHDP_ROOTLESS_BOOTSTRAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Failure stages reported through error_fd as {int32 stage, int32 errno}. */
#define VHDP_BOOT_STAGE_SIGNALS 1
#define VHDP_BOOT_STAGE_SESSION 2
#define VHDP_BOOT_STAGE_STDIO 3
#define VHDP_BOOT_STAGE_SYNC 4
#define VHDP_BOOT_STAGE_CHDIR 5
#define VHDP_BOOT_STAGE_FDS 6
#define VHDP_BOOT_STAGE_NO_NEW_PRIVS 7
#define VHDP_BOOT_STAGE_SECCOMP 8
#define VHDP_BOOT_STAGE_EXEC 9

struct vhdp_sock_fprog_view {
    uint16_t len;
    const void* filter; /* struct sock_filter[len] */
};

struct vhdp_bootstrap {
    int32_t ready_fd;     /* write end or -1: one byte once the child is traceable */
    int32_t sync_fd;      /* read end; child blocks until the tracer has seized it */
    int32_t error_fd;     /* write end, O_CLOEXEC: receives failure stage + errno */
    int32_t stdio_fds[3]; /* -1 keeps the inherited descriptor */
    int32_t ctty_fd;      /* PTY slave to become controlling terminal, or -1 */
    int32_t new_session;  /* setsid() (required with ctty_fd) */
    int32_t reset_signals;
    uint32_t max_fd; /* upper bound for the FD_CLOEXEC fallback loop */
    const char* host_cwd;
    const char* exec_path;
    char* const* argv;
    char* const* envp;
    const struct vhdp_sock_fprog_view* seccomp; /* NULL: no filter */
};

/*
 * Clones a child that runs the bootstrap. Returns the child pid (> 0) or a
 * negative errno. *pidfd receives a pidfd when the kernel supports CLONE_PIDFD,
 * otherwise -1.
 */
long vhdp_bootstrap_spawn(const struct vhdp_bootstrap* b, int32_t* pidfd);

#ifdef __cplusplus
}
#endif

#endif
