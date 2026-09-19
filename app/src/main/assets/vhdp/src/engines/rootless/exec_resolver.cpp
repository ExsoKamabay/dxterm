#include "engines/rootless/exec_resolver.hpp"

#include "common/unique_fd.hpp"
#include "linux_abi/elf.h"
#include "vfs/guest_path.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace vhdp::rootless {

namespace {

constexpr std::size_t kLoaderScanMax = std::size_t{16} * 1024 * 1024;

int pread_full(int fd, void* buf, std::size_t len, off_t off, std::size_t& got) {
    got = 0;
    while (got < len) {
        ssize_t n =
            ::pread(fd, static_cast<char*>(buf) + got, len - got, off + static_cast<off_t>(got));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        if (n == 0) {
            break;
        }
        got += static_cast<std::size_t>(n);
    }
    return 0;
}

} // namespace

bool parse_shebang(const char* buf, std::size_t len, std::string& interp, std::string& arg,
                   bool& has_arg) {
    interp.clear();
    arg.clear();
    has_arg = false;
    if (len < 2 || buf[0] != '#' || buf[1] != '!') {
        return false;
    }
    std::size_t end = 2;
    while (end < len && buf[end] != '\n') {
        ++end;
    }
    std::string_view line(buf + 2, end - 2);
    auto is_space = [](char c) { return c == ' ' || c == '\t'; };
    std::size_t i = 0;
    while (i < line.size() && is_space(line[i])) {
        ++i;
    }
    std::size_t start = i;
    while (i < line.size() && !is_space(line[i]) && line[i] != '\0') {
        ++i;
    }
    if (i == start) {
        return false;
    }
    interp.assign(line.substr(start, i - start));
    while (i < line.size() && is_space(line[i])) {
        ++i;
    }
    std::size_t aend = line.size();
    while (aend > i && (is_space(line[aend - 1]) || line[aend - 1] == '\r')) {
        --aend;
    }
    if (aend > i) {
        std::string_view a = line.substr(i, aend - i);
        std::size_t nul = a.find('\0');
        arg.assign(a.substr(0, nul));
        has_arg = !arg.empty();
    }
    return true;
}

bool ExecResolver::loader_supports_argv0(const std::string& host_loader) {
    struct stat st{};
    if (::stat(host_loader.c_str(), &st) != 0) {
        return false;
    }
    auto key = std::make_tuple(
        static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino),
        static_cast<std::int64_t>(st.st_size), static_cast<std::int64_t>(st.st_mtime));
    if (auto it = argv0_cache_.find(key); it != argv0_cache_.end()) {
        return it->second;
    }
    bool found = false;
    UniqueFd fd(::open(host_loader.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.valid()) {
        static constexpr std::string_view kNeedle = "--argv0";
        char buf[65536];
        std::size_t carry = 0;
        std::size_t total = 0;
        while (!found && total < kLoaderScanMax) {
            ssize_t n = ::read(fd.get(), buf + carry, sizeof(buf) - carry);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            std::size_t avail = carry + static_cast<std::size_t>(n);
            total += static_cast<std::size_t>(n);
            std::string_view view(buf, avail);
            if (view.find(kNeedle) != std::string_view::npos) {
                found = true;
                break;
            }
            carry = avail >= kNeedle.size() - 1 ? kNeedle.size() - 1 : avail;
            std::memmove(buf, buf + avail - carry, carry);
        }
    }
    argv0_cache_.emplace(key, found);
    return found;
}

int ExecResolver::search_path(const std::string& name, const std::string& path_list,
                              const std::string& guest_cwd, const vfs::ResolveOptions& opts,
                              std::string& guest_out) {
    int last = ENOENT;
    std::size_t start = 0;
    for (;;) {
        std::size_t colon = path_list.find(':', start);
        std::string dir =
            path_list.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (dir.empty()) {
            dir = guest_cwd;
        }
        std::string candidate =
            vfs::join(vfs::is_absolute(dir) ? dir : vfs::join(guest_cwd, dir), name);
        vfs::Resolved r;
        int e = vfs::resolve(table_, candidate, opts, r);
        if (e == 0 && r.exists && !r.is_dir) {
            if (::access(r.host.c_str(), X_OK) == 0) {
                guest_out = candidate;
                return 0;
            }
            last = EACCES;
        }
        if (colon == std::string::npos) {
            break;
        }
        start = colon + 1;
    }
    return last;
}

int ExecResolver::plan(const std::string& guest_path, const std::vector<std::string>& argv_in,
                       const std::string& guest_cwd, const vfs::ResolveOptions& opts, ExecPlan& out,
                       ExecDiag& diag) {
    out = ExecPlan{};
    if (guest_path.empty()) {
        return ENOENT;
    }
    std::string current =
        vfs::is_absolute(guest_path) ? guest_path : vfs::join(guest_cwd, guest_path);
    std::vector<std::string> argv = argv_in;
    if (argv.empty()) {
        argv.push_back(guest_path);
    }

    for (int depth = 0;; ++depth) {
        if (depth > kMaxShebangDepth) {
            diag = {"exec.shebang_loop", "too many nested #! interpreters for " + guest_path};
            return ELOOP;
        }
        vfs::Resolved r;
        if (int e = vfs::resolve(table_, current, opts, r); e != 0) {
            return e;
        }
        if (!r.exists) {
            return ENOENT;
        }
        if (r.is_dir) {
            return EACCES;
        }
        // With loader indirection the kernel never checks the program's own
        // permissions or mount flags, so they are enforced here.
        if (::access(r.host.c_str(), X_OK) != 0) {
            return errno;
        }
        if (auto noexec =
                [&] {
                    struct statvfs v{};
                    return ::statvfs(r.host.c_str(), &v) == 0 && (v.f_flag & ST_NOEXEC) != 0;
                }();
            noexec) {
            diag = {"exec.noexec_mount", "program is on a noexec mount: " + r.guest};
            return EACCES;
        }
        UniqueFd fd(::open(r.host.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (!fd.valid()) {
            return errno;
        }
        struct stat st{};
        if (::fstat(fd.get(), &st) != 0) {
            return errno;
        }
        if (!S_ISREG(st.st_mode)) {
            return EACCES;
        }
        char head[512];
        std::size_t got = 0;
        if (int e = pread_full(fd.get(), head, sizeof(head), 0, got); e != 0) {
            return e;
        }

        std::string interp;
        std::string iarg;
        bool has_arg = false;
        if (parse_shebang(head, got < kShebangMax ? got : kShebangMax, interp, iarg, has_arg)) {
            std::vector<std::string> next;
            next.push_back(interp);
            if (has_arg) {
                next.push_back(iarg);
            }
            next.push_back(current);
            for (std::size_t i = 1; i < argv.size(); ++i) {
                next.push_back(argv[i]);
            }
            argv = std::move(next);
            current = vfs::is_absolute(interp) ? interp : vfs::join(guest_cwd, interp);
            out.shebang_depth = depth + 1;
            continue;
        }

        vhdp_elf_info info{};
        int ee = vhdp_elf_parse_header(reinterpret_cast<const std::uint8_t*>(head), got,
                                       static_cast<std::uint64_t>(st.st_size), &info);
        if (ee == VHDP_ELF_E_NOT_ELF || (ee == VHDP_ELF_E_TRUNCATED && got < 4)) {
            diag = {"exec.format", "not an ELF executable or #! script: " + r.guest};
            return ENOEXEC;
        }
        std::uint32_t host_machine = vhdp_elf_host_machine();
        if (info.machine != 0 && (info.machine != host_machine || info.elf_class == 32)) {
            diag = {"exec.abi_mismatch",
                    std::string("ABI mismatch: ") + r.guest + " is " +
                        vhdp_elf_machine_name(info.machine) +
                        (info.elf_class == 32 ? " (32-bit)" : "") + ", host engine runs " +
                        vhdp_elf_machine_name(host_machine) +
                        " guests only; cross-architecture execution needs an emulator backend"};
            return ENOEXEC;
        }
        if (ee != VHDP_ELF_OK) {
            diag = {"exec.bad_elf",
                    std::string("malformed ELF (") + vhdp_elf_error_name(ee) + "): " + r.guest};
            return ENOEXEC;
        }
        std::vector<std::uint8_t> phdrs(static_cast<std::size_t>(info.phnum) * info.phentsize);
        if (int e = pread_full(fd.get(), phdrs.data(), phdrs.size(), static_cast<off_t>(info.phoff),
                               got);
            e != 0) {
            return e;
        }
        if (got != phdrs.size() ||
            vhdp_elf_scan_phdrs(phdrs.data(), phdrs.size(), static_cast<std::uint64_t>(st.st_size),
                                &info) != VHDP_ELF_OK) {
            diag = {"exec.bad_elf", "malformed ELF program headers: " + r.guest};
            return ENOEXEC;
        }
        out.guest_program = r.guest;
        out.program_argv = argv;
        if (!info.has_interp) {
            out.host_exec = r.host;
            out.argv = std::move(argv);
            return 0;
        }
        std::vector<std::uint8_t> ibuf(static_cast<std::size_t>(info.interp_size));
        if (int e = pread_full(fd.get(), ibuf.data(), ibuf.size(),
                               static_cast<off_t>(info.interp_offset), got);
            e != 0) {
            return e;
        }
        if (got != ibuf.size() ||
            vhdp_elf_validate_interp(ibuf.data(), ibuf.size()) != VHDP_ELF_OK) {
            diag = {"exec.bad_elf", "malformed PT_INTERP in " + r.guest};
            return ENOEXEC;
        }
        std::string loader(reinterpret_cast<const char*>(ibuf.data()));
        vfs::Resolved lr;
        int le = vfs::resolve(table_, loader, opts, lr);
        if (le != 0 || !lr.exists || lr.is_dir) {
            diag = {"exec.loader_missing", "dynamic loader " + loader + " required by " + r.guest +
                                               " is missing from the rootfs"};
            return le != 0 && le != ENOENT ? le : ENOENT;
        }
        UniqueFd lfd(::open(lr.host.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (!lfd.valid()) {
            return errno;
        }
        char lhead[64];
        std::size_t lgot = 0;
        if (int e = pread_full(lfd.get(), lhead, sizeof(lhead), 0, lgot); e != 0) {
            return e;
        }
        struct stat lst{};
        (void)::fstat(lfd.get(), &lst);
        vhdp_elf_info linfo{};
        int lee = vhdp_elf_parse_header(reinterpret_cast<const std::uint8_t*>(lhead), lgot,
                                        static_cast<std::uint64_t>(lst.st_size), &linfo);
        if (lee != VHDP_ELF_OK || linfo.machine != host_machine) {
            diag = {"exec.loader_invalid", "dynamic loader " + loader + " is not a valid " +
                                               vhdp_elf_machine_name(host_machine) + " ELF"};
            return ENOEXEC;
        }
        out.via_loader = true;
        out.guest_loader = lr.guest;
        out.host_exec = lr.host;
        out.loader_argv0 = loader_supports_argv0(lr.host);
        std::vector<std::string> final_argv;
        final_argv.push_back(argv.empty() ? r.guest : argv[0]);
        if (out.loader_argv0) {
            final_argv.emplace_back("--argv0");
            final_argv.push_back(argv.empty() ? r.guest : argv[0]);
        }
        final_argv.push_back(r.guest);
        for (std::size_t i = 1; i < argv.size(); ++i) {
            final_argv.push_back(argv[i]);
        }
        out.argv = std::move(final_argv);
        return 0;
    }
}

} // namespace vhdp::rootless
