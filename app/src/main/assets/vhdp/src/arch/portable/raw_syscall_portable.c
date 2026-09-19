/*
 * Portable reference for vhdp_raw_syscall6 (see raw_syscall.h).
 *
 * Invariant: errno observed by the caller is unchanged, the return value uses the
 * kernel convention (-errno on failure). syscall(2) itself is async-signal-safe;
 * the saved/restored errno is thread-local so the save/restore is also safe after
 * fork in the child (the child has a copy of the forking thread's TLS).
 */
#define _GNU_SOURCE
#include "arch/raw_syscall.h"

#include <errno.h>
#include <unistd.h>

long vhdp_raw_syscall6_portable(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    int saved = errno;
    errno = 0;
    long r = syscall(nr, a1, a2, a3, a4, a5, a6);
    if (r == -1 && errno != 0) {
        r = -(long)errno;
    }
    errno = saved;
    return r;
}

#if !defined(VHDP_USE_ASM)
long vhdp_raw_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    return vhdp_raw_syscall6_portable(nr, a1, a2, a3, a4, a5, a6);
}

int vhdp_raw_syscall_is_asm(void) {
    return 0;
}
#else
int vhdp_raw_syscall_is_asm(void) {
    return 1;
}
#endif
