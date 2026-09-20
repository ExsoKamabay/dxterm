// First-boot configuration of a freshly extracted rootfs, performed natively by VHDP.
//
// This is the "configure rootfs" capability: bind mountpoints, working DNS + hosts, a login
// profile, and a normal 'dracos' user with passwordless sudo. It is pure filesystem work (no
// engine, no ptrace, no execve), so it runs even inside an Android app process where the
// rootless engine cannot.
//
// Idempotent by construction: existing files are never overwritten (write_if_absent), lines are
// appended at most once, and the one deliberate exception is /etc/resolv.conf, which is force
// written because images often ship it as a symlink to a target that does not exist here.
#include "core/reports.hpp"

#include "common/json.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <fcntl.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace vhdp::core {
namespace {

bool path_exists(const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0; // lstat: a dangling symlink counts as existing
}

bool is_dir(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// mkdir -p: create every missing component of an absolute path. EEXIST is ignored.
bool mkdirs(const std::string& path) {
    std::string cur;
    std::size_t i = 0;
    if (!path.empty() && path[0] == '/') {
        cur = "/";
        i = 1;
    }
    for (; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty() && cur != "/") {
                ::mkdir(cur.c_str(), static_cast<mode_t>(0755));
            }
            if (i < path.size()) {
                cur += '/';
            }
        } else {
            cur += path[i];
        }
    }
    return is_dir(path);
}

std::string read_all(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool write_all(const std::string& p, std::string_view content) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(f);
}

// Write only when nothing exists at the path (a dangling symlink counts as existing and is left
// alone).
bool write_if_absent(const std::string& p, std::string_view content) {
    if (path_exists(p)) {
        return false;
    }
    return write_all(p, content);
}

// Remove any regular file OR symlink at the path first, then write a real regular file, so the
// write cannot be redirected through a broken link. Used for resolv.conf.
bool force_write(const std::string& p, std::string_view content) {
    ::unlink(p.c_str()); // removes the symlink itself (not its target); ignore failure
    return write_all(p, content);
}

// Append a line exactly once. No-op if the file is missing or already contains the line.
bool append_line_once(const std::string& p, const std::string& line) {
    if (!path_exists(p)) {
        return false;
    }
    std::string cur = read_all(p);
    if (cur.find(line) != std::string::npos) {
        return false;
    }
    std::string add = (cur.empty() || cur.back() == '\n') ? (line + "\n") : ("\n" + line + "\n");
    std::ofstream f(p, std::ios::binary | std::ios::app);
    if (!f) {
        return false;
    }
    f.write(add.data(), static_cast<std::streamsize>(add.size()));
    return static_cast<bool>(f);
}

bool has_user_dracos(const std::string& passwd_text) {
    return passwd_text.rfind("dracos:", 0) == 0 ||
           passwd_text.find("\ndracos:") != std::string::npos;
}

struct Action {
    std::string id;
    std::string detail;
};

} // namespace

std::string build_configure_json(const std::string& rootfs_in) {
    JsonWriter w;
    w.begin_object();
    w.key("input").value(rootfs_in);

    char canon[PATH_MAX];
    struct stat st{};
    if (rootfs_in.empty() || ::realpath(rootfs_in.c_str(), canon) == nullptr ||
        ::stat(canon, &st) != 0 || !S_ISDIR(st.st_mode)) {
        w.key("rootfs").null();
        w.key("status").value("error");
        w.key("message").value("rootfs path does not exist or is not a directory");
        w.key("actions").begin_array().end_array();
        w.end_object();
        return std::move(w).take();
    }

    const std::string root(canon);
    const std::string etc = root + "/etc";
    w.key("rootfs").value(root);

    std::vector<Action> actions;
    auto did = [&](const char* id, std::string detail) {
        actions.push_back({id, std::move(detail)});
    };

    // 1. Bind mountpoints (bound at launch; the guest needs them to exist).
    for (const char* d : {"dev", "proc", "sys", "tmp", "root", "etc"}) {
        mkdirs(root + "/" + d);
    }
    ::chmod((root + "/tmp").c_str(), static_cast<mode_t>(0777)); // rwxrwxrwx for /tmp
    did("mountpoints", "dev proc sys tmp root etc");

    // 1b. The guest /dev for VHDP's minimal device projection. That projection maps individual
    //     host nodes (null, zero, full, random, urandom, tty, ptmx, pts) over these names, so
    //     /dev can be listed and shows only what the guest may use -- unlike a whole-/dev bind,
    //     which an Android app may search but not list. What the projection does not provide is
    //     supplied here: the /proc/self/fd links bash needs for process substitution and scripts
    //     use as /dev/stdin, and a writable /dev/shm for POSIX shared memory. Empty placeholder
    //     files make the projected names appear in a listing; they are shadowed at runtime, and
    //     nothing is created where the image already has an entry.
    {
        const std::string dev = root + "/dev";
        int made = 0;
        for (auto [name, target] : {std::pair{"fd", "/proc/self/fd"},
                                    std::pair{"stdin", "/proc/self/fd/0"},
                                    std::pair{"stdout", "/proc/self/fd/1"},
                                    std::pair{"stderr", "/proc/self/fd/2"}}) {
            std::string p = dev + "/" + name;
            if (!path_exists(p) && ::symlink(target, p.c_str()) == 0) {
                ++made;
            }
        }
        for (const char* d : {"shm", "pts"}) {
            std::string p = dev + "/" + d;
            if (!path_exists(p) && mkdirs(p)) {
                ++made;
            }
        }
        ::chmod((dev + "/shm").c_str(), static_cast<mode_t>(01777));
        for (const char* n : {"null", "zero", "full", "random", "urandom", "tty", "ptmx"}) {
            std::string p = dev + "/" + n;
            if (!path_exists(p)) {
                int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
                if (fd >= 0) {
                    ::close(fd);
                    ++made;
                }
            }
        }
        did("dev", made > 0 ? "created " + std::to_string(made) + " /dev entries" : "present");
    }

    // 2. Working DNS. Forced because images often ship resolv.conf as a symlink to a target
    //    (…/run/…) that does not exist here; following it would leave the guest with no resolver.
    if (force_write(etc + "/resolv.conf",
                    "nameserver 8.8.8.8\nnameserver 1.1.1.1\n"
                    "nameserver 2001:4860:4860::8888\nnameserver 2606:4700:4700::1111\n")) {
        did("resolv.conf", "forced");
    }

    // 3. Ensure the resolver consults DNS (only if the image ships none).
    if (write_if_absent(etc + "/nsswitch.conf",
                        "passwd: files\ngroup: files\nshadow: files\n"
                        "hosts: files dns\nnetworks: files\nprotocols: files\nservices: files\n")) {
        did("nsswitch.conf", "written");
    }

    // 4. Localhost resolution.
    if (write_if_absent(etc + "/hosts", "127.0.0.1 localhost\n::1 localhost\n")) {
        did("hosts", "written");
    }

    // 5. Login profile for the root fallback shell (only if the image ships none).
    if (write_if_absent(root + "/root/.profile",
                        "export TERM=xterm-256color\n"
                        "if [ -n \"$BASH_VERSION\" ] && [ -f \"$HOME/.bashrc\" ]; then\n"
                        "    . \"$HOME/.bashrc\"\n"
                        "else\n"
                        "    export PS1='\\[\\e[35m\\]\\u@vhdp\\[\\e[0m\\]:"
                        "\\[\\e[36m\\]\\w\\[\\e[0m\\]$ '\n"
                        "fi\n")) {
        did("root_profile", "written");
    }

    // 6. Suppress the MOTD on the root fallback login.
    if (write_if_absent(root + "/root/.hushlogin", "")) {
        did("hushlogin", "written");
    }

    // 7. A normal login user 'dracos' (uid/gid 1000) with passwordless sudo, so the terminal
    //    starts as a non-root $ prompt and escalates cleanly. Only on a passwd-based rootfs.
    const std::string passwd = etc + "/passwd";
    if (path_exists(passwd)) {
        if (!has_user_dracos(read_all(passwd))) {
            append_line_once(passwd, "dracos:x:1000:1000:dracos:/home/dracos:/bin/bash");
            append_line_once(etc + "/group", "dracos:x:1000:");
            if (path_exists(etc + "/shadow")) {
                append_line_once(etc + "/shadow", "dracos:!:19999:0:99999:7:::"); // login disabled
            }
            did("user", "created dracos");
        } else {
            did("user", "present");
        }
        const std::string sudoersd = etc + "/sudoers.d";
        if (is_dir(sudoersd)) {
            const std::string drop = sudoersd + "/dracos";
            if (!path_exists(drop) && write_all(drop, "dracos ALL=(ALL) NOPASSWD:ALL\n")) {
                ::chmod(drop.c_str(), static_cast<mode_t>(0440)); // 0440, required by sudo
                did("sudoers", "dracos NOPASSWD");
            }
        }
        mkdirs(root + "/home/dracos"); // in-rootfs mountpoint for the home bind
    } else {
        did("user", "skipped: no /etc/passwd");
    }

    w.key("status").value("ok");
    w.key("actions").begin_array();
    for (const auto& a : actions) {
        w.begin_object().key("id").value(a.id).key("detail").value(a.detail).end_object();
    }
    w.end_array();
    w.end_object();
    return std::move(w).take();
}

} // namespace vhdp::core
