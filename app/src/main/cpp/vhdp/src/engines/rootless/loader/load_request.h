/*
 * Load request: what the supervisor hands the userland loader after an execve.
 *
 * Why a loader exists at all: an Android app targeting API 29+ may not execve()
 * files from its own writable storage, and a guest rootfs lives in exactly that
 * storage. It may still execve() files from its nativeLibraryDir, and it may still
 * map app files PROT_EXEC. So every guest execve is turned into an execve of the
 * loader (from nativeLibraryDir) with the guest's own argv and envp; the kernel
 * builds the stack and auxv, and the loader then maps the guest program and its
 * dynamic linker itself, fixes the auxv entries that describe the image, and jumps.
 *
 * Handover: at the PTRACE_EVENT_EXEC stop of the loader, the supervisor writes one
 * request below the new stack pointer and places its address and size in the
 * second and third syscall-argument registers (x86_64 rsi/rdx, aarch64 x1/x2).
 * Those two are chosen because neither is overwritten when execve returns (the
 * return value goes to rax/x0; sysret clobbers rcx/r11 only).
 *
 * Paths are GUEST paths. The loader opens them with ordinary openat(), which the
 * supervisor translates like any other guest open, so the loader never needs to
 * know where the rootfs is on the host.
 */
#ifndef VHDP_ROOTLESS_LOAD_REQUEST_H
#define VHDP_ROOTLESS_LOAD_REQUEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VHDP_LOAD_MAGIC 0x4c504856u /* "VHPL" little-endian */
#define VHDP_LOAD_VERSION 1u
#define VHDP_LOAD_MAX_SIZE 16384u  /* header + strings */

/* Fixed header, immediately followed by NUL-terminated strings. Offsets are from
 * the start of the header; an offset of 0 means "absent". */
struct vhdp_load_request {
    uint32_t magic;
    uint32_t version;
    uint32_t total_size;
    uint32_t program_off; /* guest path of the ELF to map (required) */
    uint32_t interp_off;  /* guest path of its PT_INTERP loader, 0 for a static ELF */
    uint32_t execfn_off;  /* guest path reported as AT_EXECFN (the path given to execve) */
    uint32_t name_off;    /* task name set with PR_SET_NAME (basename, <= 15 bytes used) */
    uint32_t flags;       /* reserved, must be 0 */
};

#ifdef __cplusplus
}
#endif

#endif
