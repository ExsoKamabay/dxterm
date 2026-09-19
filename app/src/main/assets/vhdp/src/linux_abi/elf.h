/*
 * Bounds-checked ELF header / program-header decoder (C17).
 *
 * Inputs are treated as untrusted: every offset/size is checked against the buffer
 * length and the file size with overflow-safe arithmetic. Integers are decoded
 * explicitly as little-endian, independent of host endianness. Big-endian ELF
 * files are rejected (no supported guest uses them). The decoder allocates nothing.
 */
#ifndef VHDP_LINUX_ABI_ELF_H
#define VHDP_LINUX_ABI_ELF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VHDP_ELF_OK 0
#define VHDP_ELF_E_TRUNCATED 1
#define VHDP_ELF_E_NOT_ELF 2
#define VHDP_ELF_E_UNSUPPORTED_CLASS 3
#define VHDP_ELF_E_UNSUPPORTED_ENCODING 4
#define VHDP_ELF_E_BAD_VERSION 5
#define VHDP_ELF_E_BAD_TYPE 6
#define VHDP_ELF_E_BAD_PHDR 7
#define VHDP_ELF_E_BAD_INTERP 8

#define VHDP_EM_386 3u
#define VHDP_EM_ARM 40u
#define VHDP_EM_X86_64 62u
#define VHDP_EM_AARCH64 183u
#define VHDP_EM_RISCV 243u

/* Largest program header table the decoder accepts (bytes). */
#define VHDP_ELF_MAX_PHDR_BYTES 65536u
/* Largest PT_INTERP segment accepted (PATH_MAX). */
#define VHDP_ELF_MAX_INTERP 4096u

typedef struct vhdp_elf_info {
    uint32_t elf_class; /* 32 or 64 */
    uint32_t machine;   /* e_machine */
    uint32_t type;      /* e_type */
    uint32_t osabi;
    uint64_t entry;
    uint64_t phoff;
    uint32_t phentsize;
    uint32_t phnum;
    uint32_t has_interp;
    uint32_t reserved0;
    uint64_t interp_offset;
    uint64_t interp_size; /* includes the terminating NUL */
} vhdp_elf_info;

/*
 * Decodes the ELF identification and header from buf (at least 64 bytes needed for
 * ELF64; 52 for ELF32). On VHDP_ELF_E_UNSUPPORTED_CLASS, out->elf_class and
 * out->machine are still filled so callers can report an ABI mismatch.
 */
int vhdp_elf_parse_header(const uint8_t* buf, size_t len, uint64_t file_size, vhdp_elf_info* out);

/*
 * Scans the program header table. phdrs must contain the bytes starting at
 * info->phoff and at least info->phnum * info->phentsize bytes long.
 * Sets has_interp/interp_offset/interp_size when a single PT_INTERP exists.
 */
int vhdp_elf_scan_phdrs(const uint8_t* phdrs, size_t len, uint64_t file_size, vhdp_elf_info* info);

/* Validates PT_INTERP contents: NUL-terminated at the end only, absolute, non-empty. */
int vhdp_elf_validate_interp(const uint8_t* data, size_t len);

/* "x86_64", "aarch64", "arm", "i386", "riscv64" or "unknown". Static string. */
const char* vhdp_elf_machine_name(uint32_t machine);

/* e_machine of the architecture libvhdp was compiled for. */
uint32_t vhdp_elf_host_machine(void);

const char* vhdp_elf_error_name(int err);

#ifdef __cplusplus
}
#endif

#endif
