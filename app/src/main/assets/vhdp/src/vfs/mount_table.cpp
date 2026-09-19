#include "vfs/mount_table.hpp"

#include "vfs/guest_path.hpp"

#include <utility>

namespace vhdp::vfs {

MountTable::MountTable(std::string rootfs_host, bool rootfs_read_only) {
    Mount root;
    root.guest = "/";
    root.host = std::move(rootfs_host);
    root.read_only = rootfs_read_only;
    root.kind = MountKind::rootfs;
    root.source = "rootfs";
    mounts_.push_back(std::move(root));
}

void MountTable::add(Mount m) {
    mounts_.push_back(std::move(m));
}

const Mount* MountTable::mount_for_guest(std::string_view guest) const {
    const Mount* best = nullptr;
    for (const auto& m : mounts_) {
        if (is_below(guest, m.guest) && (best == nullptr || m.guest.size() > best->guest.size())) {
            best = &m;
        }
    }
    return best;
}

HostMapping MountTable::to_host(std::string_view guest) const {
    HostMapping out;
    out.mount = mount_for_guest(guest);
    if (out.mount == nullptr) {
        out.mount = &mounts_.front();
    }
    std::string_view rest = suffix_after(guest, out.mount->guest);
    out.host = out.mount->host;
    if (!rest.empty()) {
        if (out.host == "/") {
            out.host.assign(rest);
        } else {
            out.host.append(rest);
        }
    }
    return out;
}

std::optional<std::string> MountTable::to_guest(std::string_view host) const {
    const Mount* best = nullptr;
    for (const auto& m : mounts_) {
        if (is_below(host, m.host) && (best == nullptr || m.host.size() > best->host.size())) {
            best = &m;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    std::string_view rest = suffix_after(host, best->host);
    if (best->guest == "/") {
        return rest.empty() ? std::string("/") : std::string(rest);
    }
    std::string out = best->guest;
    out.append(rest);
    return out;
}

} // namespace vhdp::vfs
