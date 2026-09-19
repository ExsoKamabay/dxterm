#include "linux_abi/elf.h"

#include <string.h>

#define ELF_ET_EXEC 2u
#define ELF_ET_DYN 3u
#define ELF_PT_INTERP 3u
#define ELF64_EHDR_SIZE 64u
#define ELF32_EHDR_SIZE 52u
#define ELF64_PHDR_SIZE 56u

static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* a + b <= limit without overflow */
static int range_ok(uint64_t a, uint64_t b, uint64_t limit) {
    return a <= limit && b <= limit - a;
}

int vhdp_elf_parse_header(const uint8_t* buf, size_t len, uint64_t file_size, vhdp_elf_info* out) {
    if (buf == NULL || out == NULL) {
        return VHDP_ELF_E_TRUNCATED;
    }
    memset(out, 0, sizeof(*out));
    if (len < 20) {
        return len >= 4 && memcmp(buf, "\177ELF", 4) != 0 ? VHDP_ELF_E_NOT_ELF
                                                          : VHDP_ELF_E_TRUNCATED;
    }
    if (memcmp(buf, "\177ELF", 4) != 0) {
        return VHDP_ELF_E_NOT_ELF;
    }
    uint8_t cls = buf[4];
    uint8_t data = buf[5];
    out->osabi = buf[7];
    if (data != 1) { /* ELFDATA2LSB */
        return VHDP_ELF_E_UNSUPPORTED_ENCODING;
    }
    out->type = rd16(buf + 16);
    out->machine = rd16(buf + 18);
    if (cls == 1) {
        out->elf_class = 32;
        return len < ELF32_EHDR_SIZE ? VHDP_ELF_E_TRUNCATED : VHDP_ELF_E_UNSUPPORTED_CLASS;
    }
    if (cls != 2) {
        return VHDP_ELF_E_NOT_ELF;
    }
    out->elf_class = 64;
    if (len < ELF64_EHDR_SIZE) {
        return VHDP_ELF_E_TRUNCATED;
    }
    if (buf[6] != 1 || rd32(buf + 20) != 1) {
        return VHDP_ELF_E_BAD_VERSION;
    }
    if (out->type != ELF_ET_EXEC && out->type != ELF_ET_DYN) {
        return VHDP_ELF_E_BAD_TYPE;
    }
    out->entry = rd64(buf + 24);
    out->phoff = rd64(buf + 32);
    uint16_t ehsize = rd16(buf + 52);
    out->phentsize = rd16(buf + 54);
    out->phnum = rd16(buf + 56);
    if (ehsize < ELF64_EHDR_SIZE) {
        return VHDP_ELF_E_BAD_VERSION;
    }
    if (out->phnum == 0) {
        return VHDP_ELF_E_BAD_PHDR;
    }
    /* PN_XNUM (0xffff) extended numbering is not used by executables we accept. */
    if (out->phnum == 0xffffu || out->phentsize < ELF64_PHDR_SIZE) {
        return VHDP_ELF_E_BAD_PHDR;
    }
    uint64_t table = (uint64_t)out->phnum * (uint64_t)out->phentsize;
    if (table > VHDP_ELF_MAX_PHDR_BYTES || !range_ok(out->phoff, table, file_size)) {
        return VHDP_ELF_E_BAD_PHDR;
    }
    return VHDP_ELF_OK;
}

int vhdp_elf_scan_phdrs(const uint8_t* phdrs, size_t len, uint64_t file_size, vhdp_elf_info* info) {
    if (phdrs == NULL || info == NULL || info->elf_class != 64) {
        return VHDP_ELF_E_BAD_PHDR;
    }
    uint64_t table = (uint64_t)info->phnum * (uint64_t)info->phentsize;
    if (table > len || table > VHDP_ELF_MAX_PHDR_BYTES) {
        return VHDP_ELF_E_TRUNCATED;
    }
    info->has_interp = 0;
    info->interp_offset = 0;
    info->interp_size = 0;
    for (uint32_t i = 0; i < info->phnum; ++i) {
        const uint8_t* ph = phdrs + (size_t)i * info->phentsize;
        uint32_t p_type = rd32(ph);
        if (p_type != ELF_PT_INTERP) {
            continue;
        }
        if (info->has_interp) {
            return VHDP_ELF_E_BAD_INTERP; /* the kernel rejects multiple PT_INTERP too */
        }
        uint64_t off = rd64(ph + 8);
        uint64_t filesz = rd64(ph + 32);
        if (filesz < 2 || filesz > VHDP_ELF_MAX_INTERP || !range_ok(off, filesz, file_size)) {
            return VHDP_ELF_E_BAD_INTERP;
        }
        info->has_interp = 1;
        info->interp_offset = off;
        info->interp_size = filesz;
    }
    return VHDP_ELF_OK;
}

int vhdp_elf_validate_interp(const uint8_t* data, size_t len) {
    if (data == NULL || len < 2 || len > VHDP_ELF_MAX_INTERP) {
        return VHDP_ELF_E_BAD_INTERP;
    }
    if (data[len - 1] != '\0' || data[0] != '/') {
        return VHDP_ELF_E_BAD_INTERP;
    }
    if (memchr(data, '\0', len - 1) != NULL) {
        return VHDP_ELF_E_BAD_INTERP;
    }
    return VHDP_ELF_OK;
}

const char* vhdp_elf_machine_name(uint32_t machine) {
    switch (machine) {
        case VHDP_EM_X86_64:
            return "x86_64";
        case VHDP_EM_AARCH64:
            return "aarch64";
        case VHDP_EM_ARM:
            return "arm";
        case VHDP_EM_386:
            return "i386";
        case VHDP_EM_RISCV:
            return "riscv64";
        default:
            return "unknown";
    }
}

uint32_t vhdp_elf_host_machine(void) {
#if defined(__x86_64__)
    return VHDP_EM_X86_64;
#elif defined(__aarch64__)
    return VHDP_EM_AARCH64;
#else
    return 0;
#endif
}

const char* vhdp_elf_error_name(int err) {
    switch (err) {
        case VHDP_ELF_OK:
            return "ok";
        case VHDP_ELF_E_TRUNCATED:
            return "truncated";
        case VHDP_ELF_E_NOT_ELF:
            return "not-elf";
        case VHDP_ELF_E_UNSUPPORTED_CLASS:
            return "unsupported-class";
        case VHDP_ELF_E_UNSUPPORTED_ENCODING:
            return "unsupported-encoding";
        case VHDP_ELF_E_BAD_VERSION:
            return "bad-version";
        case VHDP_ELF_E_BAD_TYPE:
            return "bad-type";
        case VHDP_ELF_E_BAD_PHDR:
            return "bad-phdr";
        case VHDP_ELF_E_BAD_INTERP:
            return "bad-interp";
        default:
            return "unknown";
    }
}
