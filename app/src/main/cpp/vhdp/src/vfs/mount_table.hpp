// Guest <-> host path mapping table: the rootfs plus explicit binds and the
// projections declared by vdr drivers. Immutable after construction.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vhdp::vfs {

enum class MountKind { rootfs, bind, device, proc };

struct Mount {
    std::string guest; // normalised absolute guest path, no trailing slash (except "/")
    std::string host;  // canonical absolute host path
    bool read_only = false;
    MountKind kind = MountKind::rootfs;
    std::string source; // owner for diagnostics: "rootfs", "bind", "vdr:dev-minimal", ...
};

struct HostMapping {
    const Mount* mount = nullptr;
    std::string host;
};

class MountTable {
public:
    // rootfs must be canonical and absolute. Mounts added later are matched by
    // longest guest prefix, so order of insertion does not matter.
    explicit MountTable(std::string rootfs_host, bool rootfs_read_only);

    // guest: absolute, no "." / ".." / empty components.
    void add(Mount m);

    HostMapping to_host(std::string_view guest) const;

    // Longest host prefix wins. nullopt when the host path is outside every mount.
    std::optional<std::string> to_guest(std::string_view host) const;

    const Mount* mount_for_guest(std::string_view guest) const;
    const std::vector<Mount>& mounts() const noexcept { return mounts_; }
    const Mount& rootfs() const noexcept { return mounts_.front(); }

private:
    std::vector<Mount> mounts_;
};

} // namespace vhdp::vfs
