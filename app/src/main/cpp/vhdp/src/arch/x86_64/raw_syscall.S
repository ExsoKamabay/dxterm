/*
 * long vhdp_raw_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
 * Architecture: x86_64 (System V AMD64 ABI, Linux kernel syscall ABI).
 *
 * Why assembly: the Linux x86_64 syscall convention passes the 4th argument in r10
 * (not rcx) and clobbers rcx/r11. This routine gives an exact, errno-free, TLS-free
 * register mapping for the post-fork/pre-exec tracee bootstrap. The C reference is
 * src/arch/portable/raw_syscall_portable.c; tests/unit/test_raw_syscall.cpp checks
 * equivalence, callee-saved preservation and stack neutrality.
 *
 * Input  (SysV): rdi=nr rsi=a1 rdx=a2 rcx=a3 r8=a4 r9=a5 [rsp+8]=a6
 * Kernel (in)  : rax=nr rdi=a1 rsi=a2 rdx=a3 r10=a4 r8=a5 r9=a6
 * Output       : rax = raw kernel result (-4095..-1 means -errno)
 * Clobbers     : rax rcx rdx rsi rdi r8 r9 r10 r11 (all caller-saved)
 * Preserves    : rbx rbp rsp r12 r13 r14 r15, x87/SSE control words, direction flag
 * Stack        : leaf, no frame, does not change rsp; alignment requirement is the
 *                caller's (no calls are made from here).
 * Unwind       : CFI covers the whole body (CFA = rsp+8 throughout).
 * TLS/signals  : no TLS access, no errno; async-signal-safe.
 * CPU features : baseline x86-64; ENDBR64 landing pad for CET/IBT.
 */
#if defined(__x86_64__)

    .text
    .globl  vhdp_raw_syscall6
    .hidden vhdp_raw_syscall6
    .type   vhdp_raw_syscall6, @function
    .p2align 4
vhdp_raw_syscall6:
    .cfi_startproc
    endbr64
    movq    %rdi, %rax          /* nr */
    movq    %rsi, %rdi          /* a1 */
    movq    %rdx, %rsi          /* a2 */
    movq    %rcx, %rdx          /* a3 */
    movq    %r8,  %r10          /* a4 */
    movq    %r9,  %r8           /* a5 */
    movq    8(%rsp), %r9        /* a6 from the caller's outgoing argument area */
    syscall
    ret
    .cfi_endproc
    .size   vhdp_raw_syscall6, .-vhdp_raw_syscall6

    /* GNU property note: this object is IBT and SHSTK compatible. */
    .section .note.gnu.property, "a"
    .p2align 3
    .long   4                   /* n_namesz */
    .long   16                  /* n_descsz */
    .long   5                   /* NT_GNU_PROPERTY_TYPE_0 */
    .asciz  "GNU"
    .long   0xc0000002          /* GNU_PROPERTY_X86_FEATURE_1_AND */
    .long   4                   /* pr_datasz */
    .long   3                   /* IBT | SHSTK */
    .p2align 3

#endif

    .section .note.GNU-stack, "", @progbits
