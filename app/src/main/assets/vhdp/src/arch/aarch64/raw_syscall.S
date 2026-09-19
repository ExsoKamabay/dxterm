/*
 * long vhdp_raw_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
 * Architecture: AArch64 (AAPCS64, Linux kernel syscall ABI).
 *
 * Why assembly: the kernel expects the syscall number in x8 and arguments in x0..x5
 * while the C prototype delivers nr in x0; this routine performs that exact shift
 * with no errno/TLS access for the post-fork/pre-exec tracee bootstrap. The C
 * reference is src/arch/portable/raw_syscall_portable.c.
 *
 * Input  (AAPCS64): x0=nr x1=a1 x2=a2 x3=a3 x4=a4 x5=a5 x6=a6
 * Kernel (in)     : x8=nr x0..x5=a1..a6, svc #0
 * Output          : x0 = raw kernel result (-4095..-1 means -errno)
 * Clobbers        : x0-x8 (caller-saved). x16/x17 untouched. Flags undefined.
 * Preserves       : x18 (platform register, reserved on Android: never touched),
 *                   x19-x29, sp, lr (leaf: no call, lr not modified), d8-d15.
 * Stack           : leaf, no frame, sp unchanged; no alignment dependency.
 * Unwind          : CFI covers the body (CFA = sp, return address in x30).
 * PAC/BTI         : 'bti c' landing pad; no LR signing needed (lr never stored).
 * TLS/signals     : no TLS access, no errno; async-signal-safe.
 * Endianness      : little-endian only (Android AArch64).
 */
#if defined(__aarch64__)

    .text
    .globl  vhdp_raw_syscall6
    .hidden vhdp_raw_syscall6
    .type   vhdp_raw_syscall6, %function
    .p2align 2
vhdp_raw_syscall6:
    .cfi_startproc
    hint    #34                 /* bti c */
    mov     x8, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, x3
    mov     x3, x4
    mov     x4, x5
    mov     x5, x6
    svc     #0
    ret
    .cfi_endproc
    .size   vhdp_raw_syscall6, .-vhdp_raw_syscall6

    /* GNU property note: BTI and PAC compatible. */
    .section .note.gnu.property, "a"
    .p2align 3
    .long   4                   /* n_namesz */
    .long   16                  /* n_descsz */
    .long   5                   /* NT_GNU_PROPERTY_TYPE_0 */
    .asciz  "GNU"
    .long   0xc0000000          /* GNU_PROPERTY_AARCH64_FEATURE_1_AND */
    .long   4                   /* pr_datasz */
    .long   3                   /* BTI | PAC */
    .p2align 3

#endif

    .section .note.GNU-stack, "", %progbits
