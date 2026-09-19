// `phdp inspect ROOTFS`: read-only rootfs inspection. Paths inside the rootfs
// are resolved in guest space (symlinks cannot point outside the rootfs).
#include "common/json.hpp"
#include "common/unique_fd.hpp"
#include "core/reports.hpp"
#include "linux_abi/elf.h"
#include "platform/linux/host_probe.hpp"
#include "vfs/mount_table.hpp"
#include "vfs/resolver.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>

namespace vhdp::core {

namespace {

struct Finding {
    std::string severity; // info | warning | error
    std::string id;
    std::string message;
};

std::string read_guest_file(const vfs::MountTable& table, const std::string& guest,
                            std::size_t max) {
    vfs::Resolved r;
    if (vfs::resolve(table, guest, {}, r) != 0 || !r.exists || r.is_dir) {
        return {};
    }
    UniqueFd fd(::open(r.host.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!fd.valid()) {
        return {};
    }
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return {};
    }
    std::string out(max, '\0');
    ssize_t n = ::read(fd.get(), out.data(), max);
    out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return out;
}

std::string os_release_value(const std::string& text, const std::string& key) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) {
            eol = text.size();
        }
        std::string line = text.substr(pos, eol - pos);
        if (line.rfind(key + "=", 0) == 0) {
            std::string v = line.substr(key.size() + 1);
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) {
                v = v.substr(1, v.size() - 2);
            }
            return v;
        }
        pos = eol + 1;
    }
    return {};
}

} // namespace

std::string build_inspect_json(const std::string& rootfs_in) {
    std::vector<Finding> findings;
    JsonWriter w;
    w.begin_object();
    w.key("input").value(rootfs_in);

    char canon[PATH_MAX];
    struct stat st{};
    if (rootfs_in.empty() || ::realpath(rootfs_in.c_str(), canon) == nullptr ||
        ::stat(canon, &st) != 0 || !S_ISDIR(st.st_mode)) {
        w.key("rootfs").null();
        w.key("status").value("error");
        w.key("findings").begin_array();
        w.begin_object().key("severity").value("error").key("id").value("rootfs.missing");
        w.key("message").value("rootfs path does not exist or is not a directory").end_object();
        w.end_array().end_object();
        return std::move(w).take();
    }
    std::string root(canon);
    w.key("rootfs").value(root);
    vfs::MountTable table(root, true);

    std::string fstype = platform::filesystem_type(root);
    auto noexec = platform::mount_noexec(root);
    bool writable = ::access(root.c_str(), W_OK) == 0;
    w.key("filesystem").begin_object();
    w.key("type").value(fstype);
    w.key("noexec").value(noexec.value_or(false));
    w.key("writable").value(writable);
    w.end_object();
    if (root == "/") {
        findings.push_back(
            {"error", "rootfs.host_root", "the host root cannot be used as a rootfs"});
    }
    if (noexec.value_or(false)) {
        findings.push_back({"error", "rootfs.noexec",
                            "rootfs is on a noexec mount; guest programs cannot be executed"});
    }
    if (fstype == "fuse" || fstype == "sdcardfs" || fstype == "vfat" || fstype == "exfat") {
        findings.push_back(
            {"warning", "rootfs.storage_semantics",
             "rootfs filesystem (" + fstype +
                 ") may not preserve POSIX ownership, modes, symlinks or case sensitivity"});
    }

    std::string osr = read_guest_file(table, "/etc/os-release", std::size_t{16} * 1024);
    if (osr.empty()) {
        osr = read_guest_file(table, "/usr/lib/os-release", std::size_t{16} * 1024);
    }
    w.key("os_release").begin_object();
    w.key("id").value(os_release_value(osr, "ID"));
    w.key("version_id").value(os_release_value(osr, "VERSION_ID"));
    w.key("pretty_name").value(os_release_value(osr, "PRETTY_NAME"));
    w.end_object();
    if (osr.empty()) {
        findings.push_back({"info", "rootfs.os_release", "no /etc/os-release found"});
    }

    std::uint32_t host_machine = vhdp_elf_host_machine();
    bool have_shell = false;
    w.key("shells").begin_array();
    for (const char* shell : {"/bin/sh", "/bin/bash", "/bin/ash"}) {
        vfs::Resolved r;
        if (vfs::resolve(table, shell, {}, r) != 0 || !r.exists || r.is_dir) {
            continue;
        }
        w.begin_object();
        w.key("path").value(shell);
        w.key("resolved").value(r.guest);
        UniqueFd fd(::open(r.host.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        std::uint8_t head[64] = {};
        ssize_t n = fd.valid() ? ::pread(fd.get(), head, sizeof(head), 0) : -1;
        struct stat sst{};
        if (fd.valid()) {
            (void)::fstat(fd.get(), &sst);
        }
        vhdp_elf_info info{};
        int e = n > 0 ? vhdp_elf_parse_header(head, static_cast<std::size_t>(n),
                                              static_cast<std::uint64_t>(sst.st_size), &info)
                      : VHDP_ELF_E_TRUNCATED;
        bool is_elf = e == VHDP_ELF_OK || e == VHDP_ELF_E_UNSUPPORTED_CLASS;
        w.key("elf").value(is_elf);
        if (is_elf) {
            bool match = info.machine == host_machine && info.elf_class == 64;
            w.key("machine").value(vhdp_elf_machine_name(info.machine));
            w.key("class").value(info.elf_class);
            w.key("abi_match").value(match);
            if (!match) {
                findings.push_back({"error", "rootfs.abi_mismatch",
                                    std::string(shell) + " is " +
                                        vhdp_elf_machine_name(info.machine) + ", host runs " +
                                        vhdp_elf_machine_name(host_machine) + " guests only"});
            }
            if (e == VHDP_ELF_OK && fd.valid()) {
                std::vector<std::uint8_t> ph(static_cast<std::size_t>(info.phnum) * info.phentsize);
                ssize_t pn =
                    ::pread(fd.get(), ph.data(), ph.size(), static_cast<off_t>(info.phoff));
                if (pn == static_cast<ssize_t>(ph.size()) &&
                    vhdp_elf_scan_phdrs(ph.data(), ph.size(),
                                        static_cast<std::uint64_t>(sst.st_size),
                                        &info) == VHDP_ELF_OK &&
                    info.has_interp) {
                    std::vector<std::uint8_t> ib(static_cast<std::size_t>(info.interp_size));
                    ssize_t in = ::pread(fd.get(), ib.data(), ib.size(),
                                         static_cast<off_t>(info.interp_offset));
                    if (in == static_cast<ssize_t>(ib.size()) &&
                        vhdp_elf_validate_interp(ib.data(), ib.size()) == VHDP_ELF_OK) {
                        std::string loader(reinterpret_cast<const char*>(ib.data()));
                        vfs::Resolved lr;
                        bool present = vfs::resolve(table, loader, {}, lr) == 0 && lr.exists;
                        w.key("interpreter").value(loader);
                        w.key("interpreter_present").value(present);
                        if (!present) {
                            findings.push_back({"error", "rootfs.loader_missing",
                                                "dynamic loader " + loader + " needed by " + shell +
                                                    " is missing"});
                        }
                    }
                } else {
                    w.key("interpreter").null();
                }
            }
            if (match) {
                have_shell = true;
            }
        } else {
            w.key("script_or_other").value(true);
            have_shell = true;
        }
        w.end_object();
    }
    w.end_array();
    if (!have_shell) {
        findings.push_back({"warning", "rootfs.no_shell",
                            "no usable /bin/sh, /bin/bash or /bin/ash; a command must be given"});
    }
    for (const char* dir : {"/etc", "/bin", "/usr", "/tmp", "/dev", "/proc"}) {
        vfs::Resolved r;
        if (vfs::resolve(table, dir, {}, r) != 0 || !r.exists) {
            findings.push_back({"info", "rootfs.layout", std::string("missing directory ") + dir});
        }
    }

    std::string status = "ok";
    for (const auto& f : findings) {
        if (f.severity == "error") {
            status = "error";
        } else if (f.severity == "warning" && status == "ok") {
            status = "warning";
        }
    }
    w.key("status").value(status);
    w.key("findings").begin_array();
    for (const auto& f : findings) {
        w.begin_object()
            .key("severity")
            .value(f.severity)
            .key("id")
            .value(f.id)
            .key("message")
            .value(f.message)
            .end_object();
    }
    w.end_array();
    w.end_object();
    return std::move(w).take();
}

} // namespace vhdp::core
