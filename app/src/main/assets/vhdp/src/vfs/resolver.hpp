// Symlink-aware guest path resolver.
//
// Resolution happens component by component in *guest* space: each prefix is
// mapped to a host path through the MountTable and inspected with lstat(2);
// symlink targets are interpreted relative to the guest root, and ".." never
// climbs above the guest "/". The resulting host path contains no symlink in
// any intermediate component (at the time of the check).
//
// Residual risk (documented in SECURITY.md): between resolution and the kernel's
// own lookup a concurrent rename/symlink swap can change what the host path
// refers to. This resolver is a compatibility boundary, not a sandbox.
#pragma once

#include "vfs/mount_table.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <string>
#include <string_view>
#include <unordered_map>

namespace vhdp::vfs {

// Directories resolved before, so that a lookup below one starts there instead of at "/". An
// entry is used only after one lstat(2) of its host path still shows the same directory (device
// and inode). That also catches a component above it replaced by a symlink since then: the
// kernel resolves the host path through that symlink to some other object. A path of N
// components then costs two lstat calls instead of N. Not thread-safe; one per supervisor.
class DirCache {
public:
    struct Entry {
        std::string host;
        dev_t dev = 0;
        ino_t ino = 0;
    };
    const Entry* find(const std::string& guest) const {
        auto it = entries_.find(guest);
        return it == entries_.end() ? nullptr : &it->second;
    }
    void put(const std::string& guest, const std::string& host, const struct stat& st);
    void drop(const std::string& guest) { entries_.erase(guest); }

private:
    static constexpr std::size_t kMaxEntries = 8192;
    std::unordered_map<std::string, Entry> entries_;
};

struct ResolveOptions {
    bool follow_final = true;
    int max_symlinks = 40; // Linux MAXSYMLINKS
    pid_t proc_tgid = 0;   // guest process for /proc/self in proc mounts
    pid_t proc_tid = 0;    // guest thread for /proc/thread-self
    // An emulated hard link (vfs/link_marker.hpp) stands for a regular file: when set, a final
    // component that is one is followed even without follow_final, so lstat, O_NOFOLLOW and
    // readlink see the file. Operations on the directory entry itself (unlink, rename, link)
    // leave it unset.
    bool see_through_links = false;
    DirCache* dir_cache = nullptr; // optional; directories of proc mounts are never cached
};

struct Resolved {
    std::string guest;            // canonical guest path (symlinks resolved as requested)
    std::string host;             // host path to hand to the kernel
    const Mount* mount = nullptr; // mount the final path belongs to
    bool exists = false;          // final component existed at resolution time
    bool is_dir = false;
    bool final_is_symlink = false;
    bool proc_magic = false; // final component is a /proc magic link to a non-path object
    std::string link_id;     // final path is the object of an emulated hard link: its id
};

// guest_abs must be absolute (callers join relative paths with the guest cwd or
// dirfd path first). Returns 0 or a Linux errno value.
int resolve(const MountTable& table, std::string_view guest_abs, const ResolveOptions& opts,
            Resolved& out);

} // namespace vhdp::vfs
