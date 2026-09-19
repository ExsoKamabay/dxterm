/*
 * vhdp-loader: maps a guest ELF program (and its dynamic linker) into the current
 * process and starts it. See load_request.h for why the loader exists and how the
 * supervisor hands it a request.
 *
 * Runs as the first code of a freshly exec'd process, before any libc: it is
 * freestanding, allocates only with mmap, and must link without dynamic
 * relocations (it is a static PIE that nobody relocates). Everything here follows
 * the ELF64 specification and the Linux process start-up ABI; the kernel's own
 * binfmt_elf is the behaviour it reproduces for one image.
 *
 * Failure after this point cannot be reported to the execve caller -- its image is
 * already gone, exactly as for a kernel exec that fails late -- so errors are
 * written to stderr and the process exits with 127.
 */
#include "arch/raw_syscall.h"
#include "engines/rootless/loader/load_request.h"

#include <asm/unistd.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__)
#define LOADER_HOST_MACHINE 62 /* EM_X86_64 */
#elif defined(__aarch64__)
#define LOADER_HOST_MACHINE 183 /* EM_AARCH64 */
#else
#error "vhdp-loader supports x86_64 and aarch64 only"
#endif

/* Kernel ABI constants, identical on x86_64 and aarch64. Defined here rather than
 * taken from libc headers: this file must not depend on a libc. */
#define L_AT_FDCWD (-100)
#define L_O_RDONLY 0
#define L_O_CLOEXEC 02000000
#define L_PROT_NONE 0
#define L_PROT_READ 1
#define L_PROT_WRITE 2
#define L_PROT_EXEC 4
#define L_MAP_PRIVATE 0x02
#define L_MAP_FIXED 0x10
#define L_MAP_ANONYMOUS 0x20
#define L_MAP_FIXED_NOREPLACE 0x100000
#define L_PR_SET_NAME 15
#define L_EEXIST 17

#define L_AT_NULL 0
#define L_AT_PHDR 3
#define L_AT_PHENT 4
#define L_AT_PHNUM 5
#define L_AT_PAGESZ 6
#define L_AT_BASE 7
#define L_AT_ENTRY 9
#define L_AT_EXECFN 31

#define L_ET_EXEC 2
#define L_ET_DYN 3
#define L_PT_LOAD 1
#define L_PT_INTERP 3
#define L_PT_PHDR 6
#define L_PF_X 1
#define L_PF_W 2
#define L_PF_R 4

#define L_MAX_PHNUM 512

struct elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct image {
    uint64_t entry;
    uint64_t base; /* lowest mapped address (AT_BASE for the interpreter) */
    uint64_t phdr; /* in-memory address of the program headers */
    uint16_t phnum;
    uint16_t phent;
    int has_interp;
};

/* Provided by loader_entry_<arch>.S. */
_Noreturn void vhdp_loader_jump(uint64_t entry, uint64_t* sp);
_Noreturn void vhdp_loader_main(uint64_t* sp, const struct vhdp_load_request* req, uint64_t size);

/* The compiler may emit calls to these even in freestanding code. */
void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

void* memset(void* dst, int c, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    while (n-- != 0) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n-- != 0) {
        *d++ = *s++;
    }
    return dst;
}

static long sys(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    return vhdp_raw_syscall6(nr, a1, a2, a3, a4, a5, a6);
}

static size_t str_len(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

static void put(const char* s) {
    size_t n = str_len(s);
    while (n > 0) {
        long w = sys(__NR_write, 2, (long)s, (long)n, 0, 0, 0);
        if (w == -4 /* EINTR */) {
            continue;
        }
        if (w <= 0) {
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static void put_num(unsigned long v) {
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0 && i > 0);
    put(buf + i);
}

static _Noreturn void die(const char* what, const char* path, long err) {
    put("vhdp-loader: ");
    put(what);
    if (path != NULL) {
        put(" ");
        put(path);
    }
    if (err < 0) {
        put(" (errno ");
        put_num((unsigned long)-err);
        put(")");
    }
    put("\n");
    for (;;) {
        (void)sys(__NR_exit_group, 127, 0, 0, 0, 0, 0);
    }
}

static uint64_t trunc_page(uint64_t v, uint64_t ps) {
    return v & ~(ps - 1);
}

static uint64_t round_page(uint64_t v, uint64_t ps) {
    return (v + ps - 1) & ~(ps - 1);
}

static long map(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off) {
    return sys(__NR_mmap, (long)addr, (long)len, prot, flags, fd, (long)off);
}

static int read_full(int fd, void* buf, uint64_t len, uint64_t off) {
    uint64_t got = 0;
    while (got < len) {
        long n = sys(__NR_pread64, fd, (long)((char*)buf + got), (long)(len - got),
                     (long)(off + got), 0, 0);
        if (n == -4 /* EINTR */) {
            continue;
        }
        if (n <= 0) {
            return 0;
        }
        got += (uint64_t)n;
    }
    return 1;
}

static int elf_prot(uint32_t flags) {
    int prot = 0;
    if ((flags & L_PF_R) != 0) {
        prot |= L_PROT_READ;
    }
    if ((flags & L_PF_W) != 0) {
        prot |= L_PROT_WRITE;
    }
    if ((flags & L_PF_X) != 0) {
        prot |= L_PROT_EXEC;
    }
    return prot;
}

/*
 * Maps one PT_LOAD segment of fd at bias. Mirrors what binfmt_elf does: the file
 * part is mapped private from the file, the tail of its last page is zeroed when
 * the segment has a .bss, and the rest of the .bss is anonymous memory.
 *
 * A segment whose file offset is not congruent with its address modulo this
 * device's page size (a 4 KB-aligned binary on a 16 KB-page device) cannot be
 * file-mapped at all; it is copied into anonymous memory instead, which is slower
 * but correct.
 */
static void map_segment(int fd, const struct elf64_phdr* ph, uint64_t bias, uint64_t ps,
                        const char* path) {
    uint64_t va = bias + ph->p_vaddr;
    uint64_t start = trunc_page(va, ps);
    uint64_t file_end = va + ph->p_filesz;
    uint64_t mem_end = va + ph->p_memsz;
    int prot = elf_prot(ph->p_flags);
    long r;

    if (ph->p_filesz > ph->p_memsz) {
        die("PT_LOAD with p_filesz > p_memsz in", path, 0);
    }

    if (ph->p_filesz > 0) {
        uint64_t delta = va - start;
        if (ph->p_offset < delta) {
            die("PT_LOAD offset below its page in", path, 0);
        }
        uint64_t off = ph->p_offset - delta;
        uint64_t map_end = round_page(file_end, ps);
        int zero_tail = ph->p_memsz > ph->p_filesz && (file_end & (ps - 1)) != 0;

        if ((off & (ps - 1)) == 0) {
            int map_prot = prot | (zero_tail ? L_PROT_WRITE : 0);
            r = map(start, map_end - start, map_prot, L_MAP_PRIVATE | L_MAP_FIXED, fd, off);
            if (vhdp_raw_is_error(r)) {
                die((prot & L_PROT_EXEC) != 0 ? "executable mapping refused for"
                                               : "cannot map segment of",
                    path, r);
            }
            if (zero_tail) {
                memset((void*)file_end, 0, map_end - file_end);
                if (map_prot != prot) {
                    (void)sys(__NR_mprotect, (long)start, (long)(map_end - start), prot, 0, 0,
                              0);
                }
            }
        } else {
            r = map(start, map_end - start, L_PROT_READ | L_PROT_WRITE,
                    L_MAP_PRIVATE | L_MAP_FIXED | L_MAP_ANONYMOUS, -1, 0);
            if (vhdp_raw_is_error(r)) {
                die("cannot allocate segment of", path, r);
            }
            if (!read_full(fd, (void*)va, ph->p_filesz, ph->p_offset)) {
                die("short read of segment in", path, 0);
            }
            r = sys(__NR_mprotect, (long)start, (long)(map_end - start), prot, 0, 0, 0);
            if (vhdp_raw_is_error(r)) {
                die("cannot protect copied segment of", path, r);
            }
        }
    }

    uint64_t anon_start = ph->p_filesz > 0 ? round_page(file_end, ps) : start;
    uint64_t anon_end = round_page(mem_end, ps);
    if (anon_end > anon_start) {
        r = map(anon_start, anon_end - anon_start, prot,
                L_MAP_PRIVATE | L_MAP_FIXED | L_MAP_ANONYMOUS, -1, 0);
        if (vhdp_raw_is_error(r)) {
            die("cannot map .bss of", path, r);
        }
    }
}

static void load_image(const char* path, int is_interp, uint64_t ps, struct image* out) {
    long fd = sys(__NR_openat, L_AT_FDCWD, (long)path, L_O_RDONLY | L_O_CLOEXEC, 0, 0, 0);
    if (vhdp_raw_is_error(fd)) {
        die("cannot open", path, fd);
    }

    struct elf64_ehdr eh;
    if (!read_full((int)fd, &eh, sizeof(eh), 0)) {
        die("cannot read the ELF header of", path, 0);
    }
    if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' ||
        eh.e_ident[3] != 'F' || eh.e_ident[4] != 2 /* ELFCLASS64 */ ||
        eh.e_ident[5] != 1 /* ELFDATA2LSB */) {
        die("not a 64-bit little-endian ELF:", path, 0);
    }
    if (eh.e_machine != LOADER_HOST_MACHINE) {
        die("ELF machine does not match this host:", path, 0);
    }
    if (eh.e_type != L_ET_DYN && (is_interp || eh.e_type != L_ET_EXEC)) {
        die("unsupported ELF type:", path, 0);
    }
    if (eh.e_phentsize < sizeof(struct elf64_phdr) || eh.e_phnum == 0 ||
        eh.e_phnum > L_MAX_PHNUM) {
        die("unsupported program header table in", path, 0);
    }

    uint64_t ph_bytes = (uint64_t)eh.e_phnum * eh.e_phentsize;
    uint64_t ph_alloc = round_page(ph_bytes, ps);
    long buf = map(0, ph_alloc, L_PROT_READ | L_PROT_WRITE, L_MAP_PRIVATE | L_MAP_ANONYMOUS, -1, 0);
    if (vhdp_raw_is_error(buf)) {
        die("cannot allocate program headers for", path, buf);
    }
    if (!read_full((int)fd, (void*)buf, ph_bytes, eh.e_phoff)) {
        die("cannot read program headers of", path, 0);
    }

    uint64_t lo = ~(uint64_t)0;
    uint64_t hi = 0;
    int loads = 0;
    const struct elf64_phdr* phdr_seg = NULL;
    out->has_interp = 0;
    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        const struct elf64_phdr* ph =
            (const struct elf64_phdr*)((char*)buf + (uint64_t)i * eh.e_phentsize);
        if (ph->p_type == L_PT_LOAD) {
            if (ph->p_memsz == 0) {
                continue;
            }
            uint64_t s = trunc_page(ph->p_vaddr, ps);
            uint64_t e = round_page(ph->p_vaddr + ph->p_memsz, ps);
            if (e < s) {
                die("PT_LOAD wraps the address space in", path, 0);
            }
            lo = s < lo ? s : lo;
            hi = e > hi ? e : hi;
            ++loads;
        } else if (ph->p_type == L_PT_INTERP) {
            out->has_interp = 1;
        } else if (ph->p_type == L_PT_PHDR) {
            phdr_seg = ph;
        }
    }
    if (loads == 0) {
        die("no loadable segments in", path, 0);
    }
    if (is_interp && out->has_interp) {
        die("dynamic linker itself requests an interpreter:", path, 0);
    }

    /* Reserve the whole span first so segments land at fixed offsets from one base
     * and nothing else can be placed in the gaps between them. */
    uint64_t bias;
    long base;
    if (eh.e_type == L_ET_DYN) {
        base = map(0, hi - lo, L_PROT_NONE, L_MAP_PRIVATE | L_MAP_ANONYMOUS, -1, 0);
        if (vhdp_raw_is_error(base)) {
            die("cannot reserve address space for", path, base);
        }
        bias = (uint64_t)base - lo;
    } else {
        base = map(lo, hi - lo, L_PROT_NONE,
                   L_MAP_PRIVATE | L_MAP_ANONYMOUS | L_MAP_FIXED_NOREPLACE, -1, 0);
        if (base == -L_EEXIST || (!vhdp_raw_is_error(base) && (uint64_t)base != lo)) {
            die("fixed load address already in use for", path, 0);
        }
        if (vhdp_raw_is_error(base)) {
            die("cannot reserve the fixed load address of", path, base);
        }
        bias = 0;
    }

    uint64_t phdr_addr = 0;
    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        const struct elf64_phdr* ph =
            (const struct elf64_phdr*)((char*)buf + (uint64_t)i * eh.e_phentsize);
        if (ph->p_type != L_PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        map_segment((int)fd, ph, bias, ps, path);
        if (phdr_seg == NULL && eh.e_phoff >= ph->p_offset &&
            eh.e_phoff + ph_bytes <= ph->p_offset + ph->p_filesz) {
            phdr_addr = bias + ph->p_vaddr + (eh.e_phoff - ph->p_offset);
        }
    }
    if (phdr_seg != NULL) {
        phdr_addr = bias + phdr_seg->p_vaddr;
    }

    out->entry = bias + eh.e_entry;
    out->base = (uint64_t)base;
    out->phdr = phdr_addr;
    out->phnum = eh.e_phnum;
    out->phent = eh.e_phentsize;

    (void)sys(__NR_munmap, buf, (long)ph_alloc, 0, 0, 0, 0);
    (void)sys(__NR_close, fd, 0, 0, 0, 0, 0);
}

/* Returns the NUL-terminated string at off, or NULL if it does not lie wholly inside
 * the request. */
static const char* req_str(const struct vhdp_load_request* req, uint64_t size, uint32_t off) {
    if (off == 0 || off < sizeof(*req) || off >= size) {
        return NULL;
    }
    const char* s = (const char*)req + off;
    for (uint64_t i = off; i < size; ++i) {
        if (s[i - off] == '\0') {
            return s;
        }
    }
    return NULL;
}

_Noreturn void vhdp_loader_main(uint64_t* sp, const struct vhdp_load_request* req,
                                uint64_t size) {
    if (req == NULL || size < sizeof(*req) || size > VHDP_LOAD_MAX_SIZE ||
        req->magic != VHDP_LOAD_MAGIC || req->version != VHDP_LOAD_VERSION ||
        req->total_size != size || req->flags != 0) {
        die("started without a load request; this program is started by the VHDP supervisor, "
            "not directly",
            NULL, 0);
    }
    const char* program = req_str(req, size, req->program_off);
    const char* interp = req->interp_off != 0 ? req_str(req, size, req->interp_off) : NULL;
    const char* execfn = req_str(req, size, req->execfn_off);
    const char* name = req_str(req, size, req->name_off);
    if (program == NULL || execfn == NULL || name == NULL ||
        (req->interp_off != 0 && interp == NULL)) {
        die("malformed load request", NULL, 0);
    }

    /* Walk the kernel-built stack: argc, argv[], NULL, envp[], NULL, auxv pairs. */
    uint64_t argc = sp[0];
    uint64_t* p = sp + 1 + argc + 1;
    while (*p != 0) {
        ++p;
    }
    uint64_t* auxv = p + 1;

    uint64_t ps = 4096;
    for (uint64_t* a = auxv; a[0] != L_AT_NULL; a += 2) {
        if (a[0] == L_AT_PAGESZ && a[1] != 0 && (a[1] & (a[1] - 1)) == 0) {
            ps = a[1];
        }
    }

    struct image prog;
    load_image(program, 0, ps, &prog);
    if (prog.has_interp && interp == NULL) {
        die("program needs a dynamic linker but none was resolved:", program, 0);
    }
    struct image dl;
    uint64_t jump_to = prog.entry;
    uint64_t at_base = 0;
    if (interp != NULL) {
        load_image(interp, 1, ps, &dl);
        jump_to = dl.entry;
        at_base = dl.base;
    }
    if (prog.phdr == 0) {
        die("program headers are not mapped by any segment of", program, 0);
    }

    /* AT_EXECFN must outlive this stack frame: the guest's stack reuses it. */
    uint64_t fn_len = str_len(execfn) + 1;
    uint64_t fn_alloc = round_page(fn_len, ps);
    long fn = map(0, fn_alloc, L_PROT_READ | L_PROT_WRITE, L_MAP_PRIVATE | L_MAP_ANONYMOUS, -1, 0);
    if (vhdp_raw_is_error(fn)) {
        die("cannot allocate AT_EXECFN", NULL, fn);
    }
    memcpy((void*)fn, execfn, fn_len);
    (void)sys(__NR_mprotect, fn, (long)fn_alloc, L_PROT_READ, 0, 0, 0);

    /* Replace every auxv entry that described the loader with the guest image. */
    for (uint64_t* a = auxv; a[0] != L_AT_NULL; a += 2) {
        switch (a[0]) {
            case L_AT_PHDR:
                a[1] = prog.phdr;
                break;
            case L_AT_PHENT:
                a[1] = prog.phent;
                break;
            case L_AT_PHNUM:
                a[1] = prog.phnum;
                break;
            case L_AT_ENTRY:
                a[1] = prog.entry;
                break;
            case L_AT_BASE:
                a[1] = at_base;
                break;
            case L_AT_EXECFN:
                a[1] = (uint64_t)fn;
                break;
            default:
                break;
        }
    }

    /* The kernel named the task after the loader; name it after the guest program
     * so ps, top and /proc/<pid>/comm show what is really running. */
    char comm[16];
    uint64_t n = 0;
    while (n < sizeof(comm) - 1 && name[n] != '\0') {
        comm[n] = name[n];
        ++n;
    }
    comm[n] = '\0';
    (void)sys(__NR_prctl, L_PR_SET_NAME, (long)comm, 0, 0, 0, 0);

    vhdp_loader_jump(jump_to, sp);
}
