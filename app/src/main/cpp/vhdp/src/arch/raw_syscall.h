/*
 * Raw Linux system call gateway.
 *
 * vhdp_raw_syscall6 performs one system call and returns the kernel's raw result:
 * values in [-4095, -1] are negated errno values, anything else is success.
 * It never reads or writes errno (no TLS access), takes no locks, is not a
 * pthread cancellation point and runs no fork/fdsan/fdtrack hooks. That makes it
 * usable in the post-fork child of a multithreaded host before execve, where only
 * async-signal-safe operations are allowed (see src/engines/rootless/bootstrap.c).
 *
 * Implementations:
 *  - src/arch/x86_64/raw_syscall.S   (VHDP_USE_ASM, x86_64)
 *  - src/arch/aarch64/raw_syscall.S  (VHDP_USE_ASM, aarch64)
 *  - src/arch/portable/raw_syscall_portable.c (reference; always built and exported
 *    as vhdp_raw_syscall6_portable for differential tests; used as
 *    vhdp_raw_syscall6 when VHDP_FORCE_PORTABLE or on other architectures)
 */
#ifndef VHDP_ARCH_RAW_SYSCALL_H
#define VHDP_ARCH_RAW_SYSCALL_H

#ifdef __cplusplus
extern "C" {
#endif

long vhdp_raw_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6);

/* Reference implementation built on syscall(2); saves/restores errno. */
long vhdp_raw_syscall6_portable(long nr, long a1, long a2, long a3, long a4, long a5, long a6);

/* Non-zero when vhdp_raw_syscall6 is the hand-written assembly routine. */
int vhdp_raw_syscall_is_asm(void);

static inline int vhdp_raw_is_error(long r) {
    return r < 0 && r >= -4095;
}

#ifdef __cplusplus
}
#endif

#endif
