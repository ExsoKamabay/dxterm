// Names shared by the hard-link emulation (engines/rootless/link_store.hpp) and the resolver.
//
// An emulated hard link is a symlink whose target ends in "<kLinkStoreDir>/<id>". The object it
// names lives in the store directory at the root of the mount holding the link, so the resolver
// maps the target there itself: the link keeps working wherever it is moved inside that mount.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace vhdp::vfs {

inline constexpr std::string_view kLinkStoreDir = ".vhdp-hardlinks";
inline constexpr std::size_t kLinkIdLen = 32;

inline bool is_link_id(std::string_view s) noexcept {
    if (s.size() != kLinkIdLen) {
        return false;
    }
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

// The object id a symlink target refers to, or an empty view when the target is an ordinary
// symlink target.
inline std::string_view link_target_id(std::string_view target) noexcept {
    if (target.size() < kLinkStoreDir.size() + 1 + kLinkIdLen) {
        return {};
    }
    std::string_view id = target.substr(target.size() - kLinkIdLen);
    std::string_view head = target.substr(0, target.size() - kLinkIdLen);
    if (!is_link_id(id) || head.back() != '/') {
        return {};
    }
    head.remove_suffix(1);
    if (head.size() < kLinkStoreDir.size() ||
        head.substr(head.size() - kLinkStoreDir.size()) != kLinkStoreDir) {
        return {};
    }
    if (head.size() > kLinkStoreDir.size() &&
        head[head.size() - kLinkStoreDir.size() - 1] != '/') {
        return {};
    }
    return id;
}

// Store directory of a mount, given the mount's guest or host root.
inline std::string link_store_path(std::string_view mount_root) {
    std::string out(mount_root);
    if (out.empty() || out.back() != '/') {
        out.push_back('/');
    }
    out.append(kLinkStoreDir);
    return out;
}

} // namespace vhdp::vfs
