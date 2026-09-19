#include "vfs/resolver.hpp"

#include "vfs/guest_path.hpp"
#include "vfs/link_marker.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <deque>
#include <vector>

namespace vhdp::vfs {

namespace {

// readlink into std::string; returns errno or 0.
int read_link(const std::string& host, std::string& out) {
    char buf[kPathMax];
    ssize_t n = ::readlink(host.c_str(), buf, sizeof(buf));
    if (n < 0) {
        return errno;
    }
    if (static_cast<std::size_t>(n) >= sizeof(buf)) {
        return ENAMETOOLONG;
    }
    out.assign(buf, static_cast<std::size_t>(n));
    return 0;
}

std::string build(const std::vector<std::string>& parts) {
    if (parts.empty()) {
        return "/";
    }
    std::string s;
    for (const auto& p : parts) {
        s.push_back('/');
        s.append(p);
    }
    return s;
}

bool only_dots(const std::deque<std::string>& pending) {
    for (const auto& p : pending) {
        if (p != ".") {
            return false;
        }
    }
    return true;
}

// Target of a symlink inside a proc mount, from the guest's point of view.
// Returns 0 with `out` set, or an errno. `magic` is set for non-path objects
// such as "pipe:[123]" that the kernel resolves itself.
int proc_link_target(const MountTable& table, const std::string& guest, const std::string& host,
                     const Mount& mount, const ResolveOptions& opts, std::string& out,
                     bool& magic) {
    magic = false;
    std::string_view rel = suffix_after(guest, mount.guest);
    if (rel == "/self") {
        if (opts.proc_tgid <= 0) {
            return EACCES;
        }
        out = std::to_string(opts.proc_tgid);
        return 0;
    }
    if (rel == "/thread-self") {
        if (opts.proc_tgid <= 0 || opts.proc_tid <= 0) {
            return EACCES;
        }
        out = std::to_string(opts.proc_tgid) + "/task/" + std::to_string(opts.proc_tid);
        return 0;
    }
    std::string raw;
    if (int e = read_link(host, raw); e != 0) {
        return e;
    }
    if (raw.empty()) {
        return ENOENT;
    }
    if (raw.front() != '/') {
        // Either an ordinary relative link (e.g. "self/mounts") or a magic object.
        if (raw.find(":[") != std::string::npos || raw.rfind("anon_inode:", 0) == 0) {
            magic = true;
            out = std::move(raw);
            return 0;
        }
        out = std::move(raw);
        return 0;
    }
    // Absolute host path from a magic link (root, cwd, exe, fd/N): map back into
    // the guest. Paths outside every mount fail closed.
    static constexpr std::string_view kDeleted = " (deleted)";
    if (raw.size() > kDeleted.size() &&
        raw.compare(raw.size() - kDeleted.size(), kDeleted.size(), kDeleted) == 0) {
        return ENOENT;
    }
    auto guest_target = table.to_guest(raw);
    if (!guest_target) {
        return EACCES;
    }
    out = std::move(*guest_target);
    return 0;
}

} // namespace

void DirCache::put(const std::string& guest, const std::string& host, const struct stat& st) {
    auto it = entries_.find(guest);
    if (it != entries_.end()) {
        it->second.dev = st.st_dev;
        it->second.ino = st.st_ino;
        return;
    }
    if (entries_.size() >= kMaxEntries) {
        entries_.clear();
    }
    entries_.emplace(guest, Entry{host, st.st_dev, st.st_ino});
}

int resolve(const MountTable& table, std::string_view guest_abs, const ResolveOptions& opts,
            Resolved& out) {
    out = Resolved{};
    if (!is_absolute(guest_abs)) {
        return EINVAL;
    }
    if (guest_abs.size() >= kPathMax) {
        return ENAMETOOLONG;
    }
    bool trailing_slash = guest_abs.size() > 1 && guest_abs.back() == '/';

    std::deque<std::string> pending;
    bool plain = true; // no "." or ".." components
    for (auto c : components(guest_abs)) {
        plain = plain && c != "..";
        pending.emplace_back(c);
    }
    if (trailing_slash) {
        pending.emplace_back(".");
    }
    std::vector<std::string> done;
    int links = 0;
    bool exists = true;
    bool is_dir = true; // "/" is a directory
    bool final_symlink = false;

    if (opts.dir_cache != nullptr && plain && pending.size() >= 2) {
        // Start below the longest cached directory. The final component is always looked up,
        // so following and see-through apply to it as usual.
        std::size_t prefix = pending.size() - 1 - (trailing_slash ? 1 : 0);
        std::string key;
        key.reserve(guest_abs.size());
        std::vector<std::size_t> ends;
        ends.reserve(prefix);
        for (std::size_t i = 0; i < prefix; ++i) {
            key.push_back('/');
            key.append(pending[i]);
            ends.push_back(key.size());
        }
        for (std::size_t k = prefix; k > 0; --k) {
            key.resize(ends[k - 1]);
            const DirCache::Entry* e = opts.dir_cache->find(key);
            if (e == nullptr) {
                continue;
            }
            struct stat st{};
            if (::lstat(e->host.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_dev == e->dev &&
                st.st_ino == e->ino) {
                for (std::size_t i = 0; i < k; ++i) {
                    done.push_back(std::move(pending.front()));
                    pending.pop_front();
                }
            } else {
                opts.dir_cache->drop(key);
            }
            break;
        }
    }

    while (!pending.empty()) {
        std::string comp = std::move(pending.front());
        pending.pop_front();
        if (comp == ".") {
            if (!is_dir && exists) {
                return ENOTDIR;
            }
            continue;
        }
        if (comp == "..") {
            if (!exists) {
                return ENOENT;
            }
            if (!is_dir) {
                return ENOTDIR;
            }
            if (!done.empty()) {
                done.pop_back();
            }
            is_dir = true;
            continue;
        }
        if (!exists) {
            // Below a missing component: the kernel reports ENOENT for the lookup.
            return ENOENT;
        }
        if (!is_dir) {
            return ENOTDIR;
        }
        bool last = only_dots(pending);
        bool must_follow = !last || opts.follow_final || !pending.empty();

        done.push_back(comp);
        std::string guest = build(done);
        HostMapping map = table.to_host(guest);
        struct stat st{};
        if (::lstat(map.host.c_str(), &st) != 0) {
            int e = errno;
            if (e == ENOENT) {
                exists = false;
                is_dir = false;
                continue;
            }
            if (e == EACCES && last && map.mount->kind == MountKind::proc) {
                // A host policy may hide even the attributes of a /proc file (an Android app may
                // not getattr /proc/stat). The call itself gets the kernel's answer -- the same
                // EACCES -- unless the supervisor stands in for the file.
                final_symlink = false;
                is_dir = false;
                exists = true;
                continue;
            }
            return e;
        }
        std::string target;
        bool have_target = false;
        bool follow = S_ISLNK(st.st_mode) && must_follow;
        if (S_ISLNK(st.st_mode) && !follow && opts.see_through_links &&
            map.mount->kind != MountKind::proc && read_link(map.host, target) == 0) {
            have_target = true;
            std::string_view id = link_target_id(target);
            if (!id.empty()) {
                // Only a link whose object is present stands for a file; a dangling one stays a
                // symlink, so nothing is ever created inside the store through it.
                std::string object = link_store_path(map.mount->host);
                object.push_back('/');
                object.append(id);
                struct stat ost{};
                follow = ::lstat(object.c_str(), &ost) == 0 && !S_ISDIR(ost.st_mode) &&
                         !S_ISLNK(ost.st_mode);
            }
        }
        if (follow) {
            if (++links > opts.max_symlinks) {
                return ELOOP;
            }
            bool magic = false;
            if (map.mount->kind == MountKind::proc) {
                if (int e =
                        proc_link_target(table, guest, map.host, *map.mount, opts, target, magic);
                    e != 0) {
                    return e;
                }
            } else if (!have_target) {
                if (int e = read_link(map.host, target); e != 0) {
                    return e;
                }
            }
            if (map.mount->kind != MountKind::proc) {
                // Emulated hard link: its object is in the store of the mount holding the link,
                // whatever directory prefix the target was written with.
                std::string_view id = link_target_id(target);
                if (!id.empty()) {
                    std::string object = link_store_path(map.mount->guest);
                    object.push_back('/');
                    object.append(id);
                    target = std::move(object);
                }
            }
            if (magic) {
                if (!last) {
                    return ENOTDIR;
                }
                // Hand the magic link itself to the kernel; it refers to an object
                // the guest already holds (its own fd), not to a host path.
                out.guest = guest;
                out.host = map.host;
                out.mount = map.mount;
                out.exists = true;
                out.proc_magic = true;
                return 0;
            }
            if (target.empty()) {
                return ENOENT;
            }
            done.pop_back();
            if (is_absolute(target)) {
                done.clear();
            }
            auto parts = components(target);
            bool target_trailing = target.size() > 1 && target.back() == '/';
            if (target_trailing) {
                pending.emplace_front(".");
            }
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                pending.emplace_front(*it);
            }
            is_dir = true;
            exists = true;
            continue;
        }
        final_symlink = S_ISLNK(st.st_mode);
        is_dir = S_ISDIR(st.st_mode);
        exists = true;
        if (is_dir && opts.dir_cache != nullptr && map.mount->kind != MountKind::proc &&
            map.mount->kind != MountKind::device) {
            opts.dir_cache->put(guest, map.host, st);
        }
    }

    out.guest = build(done);
    HostMapping map = table.to_host(out.guest);
    out.host = std::move(map.host);
    out.mount = map.mount;
    out.exists = exists;
    out.is_dir = exists && is_dir;
    out.final_is_symlink = exists && final_symlink;
    if (exists && !is_dir && !final_symlink && done.size() >= 2 &&
        done[done.size() - 2] == kLinkStoreDir && is_link_id(done.back())) {
        std::string store = link_store_path(map.mount->guest);
        if (out.guest.size() == store.size() + 1 + kLinkIdLen &&
            out.guest.compare(0, store.size(), store) == 0) {
            out.link_id = done.back();
        }
    }
    if (trailing_slash && out.host.back() != '/') {
        out.host.push_back('/');
    }
    if (out.host.size() >= kPathMax) {
        return ENAMETOOLONG;
    }
    return 0;
}

} // namespace vhdp::vfs
