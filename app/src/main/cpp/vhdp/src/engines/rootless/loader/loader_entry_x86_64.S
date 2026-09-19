/*
 * vhdp-loader entry and exit stubs, x86_64 (System V AMD64, Linux process start-up ABI).
 *
 * _start: the kernel starts the loader with %rsp -> argc and every other general
 * register zero, except what the supervisor placed at the PTRACE_EVENT_EXEC stop:
 * %rsi = address of the vhdp_load_request, %rdx = its size (load_request.h). The
 * request sits just below %rsp, so the C code gets its own stack further below the
 * request and the kernel-built argv/envp/auxv above %rsp are left untouched.
 *
 * vhdp_loader_jump(entry, sp): restores the guest's start-up state and jumps. The
 * ABI promises %rsp -> argc, %rdx = 0 (no atexit routine from the "kernel"), and
 * the direction flag clear; the other registers are zeroed so nothing the loader
 * computed leaks into the guest.
 */
#if defined(__x86_64__)

    .text
    .globl  _start
    .type   _start, @function
    .p2align 4
_start:
    xorl    %ebp, %ebp
    movq    %rsp, %rdi              /* arg0: initial stack (argc) */
    testq   %rsi, %rsi
    jz      1f                      /* no request: run on the original stack and fail cleanly */
    movq    %rsi, %rax
    subq    $4096, %rax             /* stay clear of the request */
    andq    $-16, %rax
    movq    %rax, %rsp
1:  andq    $-16, %rsp              /* SysV: %rsp is 16-byte aligned before a call */
    call    vhdp_loader_main        /* (sp, request, size): never returns */
    hlt
    .size   _start, .-_start

    .globl  vhdp_loader_jump
    .hidden vhdp_loader_jump
    .type   vhdp_loader_jump, @function
    .p2align 4
vhdp_loader_jump:
    movq    %rdi, %r11              /* entry */
    movq    %rsi, %rsp              /* guest stack: argc on top */
    xorl    %eax, %eax
    xorl    %ebx, %ebx
    xorl    %ecx, %ecx
    xorl    %edx, %edx
    xorl    %esi, %esi
    xorl    %edi, %edi
    xorl    %ebp, %ebp
    xorl    %r8d, %r8d
    xorl    %r9d, %r9d
    xorl    %r10d, %r10d
    xorl    %r12d, %r12d
    xorl    %r13d, %r13d
    xorl    %r14d, %r14d
    xorl    %r15d, %r15d
    cld
    jmp     *%r11
    .size   vhdp_loader_jump, .-vhdp_loader_jump

    .section .note.GNU-stack, "", @progbits
#endif
