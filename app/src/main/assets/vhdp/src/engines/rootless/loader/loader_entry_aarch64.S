/*
 * vhdp-loader entry and exit stubs, aarch64 (AAPCS64, Linux process start-up ABI).
 *
 * _start: the kernel starts the loader with sp -> argc and the general registers
 * zero, except what the supervisor placed at the PTRACE_EVENT_EXEC stop: x1 =
 * address of the vhdp_load_request, x2 = its size (load_request.h). x0 is not used
 * for the handover because execve's return value is written to it after the stop.
 * The C code runs on a stack below the request so the kernel-built argv/envp/auxv
 * above the original sp are left untouched.
 *
 * vhdp_loader_jump(entry, sp): restores the guest's start-up state and jumps. The
 * ABI promises sp -> argc and x0 = 0 (no atexit routine); every other register the
 * loader used is zeroed. x18 is never touched: it is the platform register.
 */
#if defined(__aarch64__)

    .text
    .globl  _start
    .type   _start, %function
    .p2align 2
_start:
    mov     x29, #0
    mov     x30, #0
    mov     x0, sp                  /* arg0: initial stack (argc) */
    cbz     x1, 1f                  /* no request: stay on the original stack, fail cleanly */
    sub     x3, x1, #4096           /* stay clear of the request */
    and     x3, x3, #~15            /* AAPCS64: sp 16-byte aligned */
    mov     sp, x3
1:  bl      vhdp_loader_main        /* (sp, request, size): never returns */
    brk     #0
    .size   _start, .-_start

    .globl  vhdp_loader_jump
    .hidden vhdp_loader_jump
    .type   vhdp_loader_jump, %function
    .p2align 2
vhdp_loader_jump:
    mov     x16, x0                 /* entry (x16/x17 are intra-procedure scratch) */
    mov     sp, x1                  /* guest stack: argc on top */
    mov     x0, #0
    mov     x1, #0
    mov     x2, #0
    mov     x3, #0
    mov     x4, #0
    mov     x5, #0
    mov     x6, #0
    mov     x7, #0
    mov     x8, #0
    mov     x9, #0
    mov     x10, #0
    mov     x11, #0
    mov     x12, #0
    mov     x13, #0
    mov     x14, #0
    mov     x15, #0
    mov     x17, #0
    mov     x19, #0
    mov     x20, #0
    mov     x21, #0
    mov     x22, #0
    mov     x23, #0
    mov     x24, #0
    mov     x25, #0
    mov     x26, #0
    mov     x27, #0
    mov     x28, #0
    mov     x29, #0
    mov     x30, #0
    br      x16
    .size   vhdp_loader_jump, .-vhdp_loader_jump

    .section .note.GNU-stack, "", %progbits
#endif
