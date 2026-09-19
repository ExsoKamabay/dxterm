// Hard-link emulation for hosts whose policy refuses link(2).
//
// An Android app's private storage is the case that matters: SELinux denies the app domain the
// "link" permission there, while dpkg (status-old and unpack backups), shadow's lock files
// (useradd/groupadd from maintainer scripts, which also check st_nlink), tar, cp -a and ln all
// need hard links to work.
//
// Layout, per mount (rootfs or bind) that holds emulated links:
//   <mount>/.vhdp-hardlinks/<id>     the file itself, moved here from the name it was linked from
//   <mount>/.vhdp-hardlinks/<id>.n   record: a symlink whose contents are "<count>\n<name>\n..."
//                                    (host paths of the names, best effort), replaced atomically
//   <mount>/.vhdp-hardlinks/.lock    flock(2) serialising record updates across sessions
// Every name of the file is a symlink whose target ends in ".vhdp-hardlinks/<id>". The target
// is written relative to the name's directory so host-side readers of the tree follow it too;
// the resolver ignores the prefix (vfs/link_marker.hpp), so a name keeps working when moved.
//
// The resolver follows a name even where a symlink would not be followed, so lstat, O_NOFOLLOW
// and readlink see a regular file. The supervisor presents the recorded count as st_nlink and
// keeps it current through link, unlink and rename. When the count drops to one and the last
// name is known, the file is moved back over that name, so short-lived links -- lock files,
// backups -- leave nothing behind.
//
// All operations run on the supervisor thread with the guest's host credentials; every step
// that can fail is undone before the errno is reported, so a refused link leaves no trace.
#pragma once

#include "vfs/mount_table.hpp"

#include <sys/types.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vhdp::rootless {

class LinkStore {
public:
    // Whether the kernel refuses link(2) inside host directory `dir` (EACCES/EPERM), probed with
    // a scratch file. False when the probe cannot run at all.
    static bool kernel_refuses_link(const std::string& dir);

    // The object id an emulated link at host path `name` (not followed) refers to, or empty.
    static std::string entry_id(const std::string& host_name);

    // Registers the objects already present in the stores of `table`'s mounts.
    void scan(const vfs::MountTable& table);

    // link(2) from a name that is not an emulated link yet: `old_host` (a non-directory) becomes
    // the object and both names link to it. Returns 0 or an errno.
    int link_new(const vfs::Mount& m, const std::string& old_host, const std::string& new_host);
    // link(2) to the existing object `id`.
    int link_existing(const vfs::Mount& m, const std::string& id, const std::string& new_host);
    // unlink(2) of the emulated link `host_name`.
    int unlink_name(const vfs::Mount& m, const std::string& id, const std::string& host_name);
    // A rename(2) that already succeeded replaced the emulated link `host_name`.
    void name_dropped(const vfs::Mount& m, const std::string& id, const std::string& host_name);
    // A rename(2) that already succeeded moved an emulated link from `from` to `to`.
    void name_moved(const vfs::Mount& m, const std::string& id, const std::string& from,
                    const std::string& to);

    // True when any mount of `table` holds a store object, so a name in this session may be an
    // emulated link (another session can create one at any time). Re-checked at most every few
    // hundred milliseconds, and it drops back to false once the objects are gone.
    bool stores_present(const vfs::MountTable& table);
    // Makes an object reached by path known to count_for.
    void note_object(const vfs::Mount& m, const std::string& id);
    // The link count to present for a stat result naming (dev, ino), when that is an object.
    std::optional<std::uint32_t> count_for(std::uint32_t dev_major, std::uint32_t dev_minor,
                                           std::uint64_t ino);
    bool has_objects() const noexcept { return !objects_.empty(); }

private:
    struct Record {
        std::uint32_t count = 1;
        std::vector<std::string> names;
    };
    struct Object {
        std::string store; // host path of the store directory
        std::string id;
    };
    using Key = std::pair<std::uint64_t, std::uint64_t>; // (dev, ino)

    static std::string store_of(const vfs::Mount& m);
    static std::string target_for(const vfs::Mount& m, const std::string& name,
                                  const std::string& id);
    // nullopt when the object has no record: then its count is unknown, and it is never deleted
    // on that basis (another session may have restored it to a name already).
    static std::optional<Record> read_record(const std::string& store, const std::string& id);
    static int write_record(const std::string& store, const std::string& id, const Record& rec);
    static void remove_record(const std::string& store, const std::string& id);
    // Drops one name of `id`; removes the object at zero, restores a single remaining name.
    void release(const std::string& store, const std::string& id, const std::string& gone);
    void remember(const std::string& store, const std::string& id);
    void forget(const std::string& store, const std::string& id);

    std::map<Key, Object> objects_;
    std::uint64_t stores_checked_ns_ = 0;
    bool stores_present_ = false;
    std::unordered_map<std::string, Key> by_path_; // object host path -> key
};

} // namespace vhdp::rootless
