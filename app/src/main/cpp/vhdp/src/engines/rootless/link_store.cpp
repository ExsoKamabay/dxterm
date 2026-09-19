#include "engines/rootless/link_store.hpp"

#include "common/unique_fd.hpp"
#include "vfs/guest_path.hpp"
#include "vfs/link_marker.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <cerrno>
#include <cstdlib>
#include <ctime>

namespace vhdp::rootless {

namespace {

// Kernel limit for a symlink's contents (PATH_MAX including the NUL).
constexpr std::size_t kRecordMax = 4095;

std::atomic<std::uint64_t> g_counter{0};

std::string random_id() {
    unsigned char raw[16] = {};
    bool ok = false;
#ifdef SYS_getrandom
    ok = ::syscall(SYS_getrandom, raw, sizeof(raw), 0) == static_cast<long>(sizeof(raw));
#endif
    if (!ok) {
        UniqueFd fd(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
        ok = fd.valid() && ::read(fd.get(), raw, sizeof(raw)) == static_cast<ssize_t>(sizeof(raw));
    }
    if (!ok) {
        // Uniqueness is what matters, and symlink(2) refuses an existing name anyway.
        timespec ts{};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        std::uint64_t a = static_cast<std::uint64_t>(ts.tv_sec) * 1000000000u +
                          static_cast<std::uint64_t>(ts.tv_nsec);
        std::uint64_t b = (static_cast<std::uint64_t>(::getpid()) << 32) ^ ++g_counter;
        for (int i = 0; i < 8; ++i) {
            raw[i] = static_cast<unsigned char>(a >> (8 * i));
            raw[8 + i] = static_cast<unsigned char>(b >> (8 * i));
        }
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(vfs::kLinkIdLen);
    for (unsigned char c : raw) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0xf]);
    }
    return out;
}

int read_link(const std::string& path, std::string& out) {
    char buf[kRecordMax + 1];
    ssize_t n = ::readlink(path.c_str(), buf, sizeof(buf));
    if (n < 0) {
        return errno;
    }
    if (static_cast<std::size_t>(n) >= sizeof(buf)) {
        return ENAMETOOLONG;
    }
    out.assign(buf, static_cast<std::size_t>(n));
    return 0;
}

std::uint64_t dev_key(dev_t dev) {
    return makedev(major(dev), minor(dev));
}

// Exclusive lock on a store for the lifetime of the object; a store that cannot be locked
// (e.g. being created concurrently) is used unlocked rather than failing the operation.
class StoreLock {
public:
    explicit StoreLock(const std::string& store) {
        std::string path = store + "/.lock";
        fd_.reset(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (fd_.valid()) {
            while (::flock(fd_.get(), LOCK_EX) != 0 && errno == EINTR) {
            }
        }
    }

private:
    UniqueFd fd_;
};

} // namespace

bool LinkStore::kernel_refuses_link(const std::string& dir) {
    std::string tmpl = dir;
    if (tmpl.empty() || tmpl.back() != '/') {
        tmpl.push_back('/');
    }
    tmpl.append(".vhdp-link-probe-XXXXXX");
    int fd = ::mkostemp(tmpl.data(), O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    std::string other = tmpl + ".l";
    int r = ::link(tmpl.c_str(), other.c_str());
    int e = errno;
    if (r == 0) {
        ::unlink(other.c_str());
    }
    ::unlink(tmpl.c_str());
    return r != 0 && (e == EACCES || e == EPERM);
}

std::string LinkStore::entry_id(const std::string& host_name) {
    std::string target;
    if (read_link(host_name, target) != 0) {
        return {};
    }
    return std::string(vfs::link_target_id(target));
}

std::string LinkStore::store_of(const vfs::Mount& m) {
    return vfs::link_store_path(m.host);
}

std::string LinkStore::target_for(const vfs::Mount& m, const std::string& name,
                                  const std::string& id) {
    std::string rel;
    if (vfs::is_below(name, m.host)) {
        auto parts = vfs::components(vfs::suffix_after(name, m.host));
        for (std::size_t i = 1; i < parts.size(); ++i) {
            rel.append("../");
        }
        rel.append(vfs::kLinkStoreDir);
    } else {
        rel = store_of(m);
    }
    rel.push_back('/');
    rel.append(id);
    return rel;
}

std::optional<LinkStore::Record> LinkStore::read_record(const std::string& store,
                                                        const std::string& id) {
    Record rec;
    std::string raw;
    if (read_link(store + "/" + id + ".n", raw) != 0) {
        return std::nullopt;
    }
    std::size_t nl = raw.find('\n');
    std::string head = raw.substr(0, nl);
    char* end = nullptr;
    unsigned long n = std::strtoul(head.c_str(), &end, 10);
    if (end != head.c_str() && *end == '\0' && n > 0 && n < 0x7fffffffu) {
        rec.count = static_cast<std::uint32_t>(n);
    }
    while (nl != std::string::npos) {
        std::size_t next = raw.find('\n', nl + 1);
        std::string name = raw.substr(nl + 1, next == std::string::npos ? std::string::npos
                                                                         : next - nl - 1);
        if (!name.empty()) {
            rec.names.push_back(std::move(name));
        }
        nl = next;
    }
    return rec;
}

int LinkStore::write_record(const std::string& store, const std::string& id, const Record& rec) {
    std::string body = std::to_string(rec.count);
    for (const auto& name : rec.names) {
        // The names only serve to restore the last one; an incomplete list is safe.
        if (name.find('\n') != std::string::npos ||
            body.size() + 1 + name.size() > kRecordMax) {
            continue;
        }
        body.push_back('\n');
        body.append(name);
    }
    std::string final_path = store + "/" + id + ".n";
    std::string tmp = final_path + "." + std::to_string(::getpid()) + "." +
                      std::to_string(++g_counter);
    if (::symlink(body.c_str(), tmp.c_str()) != 0) {
        return errno;
    }
    if (::rename(tmp.c_str(), final_path.c_str()) != 0) {
        int e = errno;
        ::unlink(tmp.c_str());
        return e;
    }
    return 0;
}

void LinkStore::remove_record(const std::string& store, const std::string& id) {
    std::string path = store + "/" + id + ".n";
    ::unlink(path.c_str());
}

void LinkStore::remember(const std::string& store, const std::string& id) {
    std::string path = store + "/" + id;
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        return;
    }
    Key key{dev_key(st.st_dev), static_cast<std::uint64_t>(st.st_ino)};
    objects_[key] = Object{store, id};
    by_path_[path] = key;
}

void LinkStore::forget(const std::string& store, const std::string& id) {
    std::string path = store + "/" + id;
    auto it = by_path_.find(path);
    if (it == by_path_.end()) {
        return;
    }
    objects_.erase(it->second);
    by_path_.erase(it);
}

void LinkStore::scan(const vfs::MountTable& table) {
    for (const auto& m : table.mounts()) {
        if (m.kind != vfs::MountKind::rootfs && m.kind != vfs::MountKind::bind) {
            continue;
        }
        std::string store = store_of(m);
        DIR* d = ::opendir(store.c_str());
        if (d == nullptr) {
            continue;
        }
        while (dirent* ent = ::readdir(d)) {
            if (vfs::is_link_id(ent->d_name)) {
                remember(store, ent->d_name);
            }
        }
        ::closedir(d);
    }
}

bool LinkStore::stores_present(const vfs::MountTable& table) {
    constexpr std::uint64_t kRecheckNs = 500000000; // 0.5 s
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    auto now = static_cast<std::uint64_t>(ts.tv_sec) * 1000000000u +
               static_cast<std::uint64_t>(ts.tv_nsec);
    if (stores_checked_ns_ != 0 && now - stores_checked_ns_ < kRecheckNs) {
        return stores_present_;
    }
    stores_checked_ns_ = now;
    stores_present_ = false;
    for (const auto& m : table.mounts()) {
        if (m.kind != vfs::MountKind::rootfs && m.kind != vfs::MountKind::bind) {
            continue;
        }
        // An empty store (only its lock) is left behind by a session that had links and gave
        // them all up; what matters is whether an object is in it now.
        DIR* d = ::opendir(store_of(m).c_str());
        if (d == nullptr) {
            continue;
        }
        while (dirent* ent = ::readdir(d)) {
            if (vfs::is_link_id(ent->d_name)) {
                stores_present_ = true;
                break;
            }
        }
        ::closedir(d);
        if (stores_present_) {
            break;
        }
    }
    return stores_present_;
}

void LinkStore::note_object(const vfs::Mount& m, const std::string& id) {
    std::string store = store_of(m);
    if (by_path_.count(store + "/" + id) == 0) {
        remember(store, id);
    }
}

std::optional<std::uint32_t> LinkStore::count_for(std::uint32_t dev_major,
                                                  std::uint32_t dev_minor, std::uint64_t ino) {
    auto it = objects_.find(Key{makedev(dev_major, dev_minor), ino});
    if (it == objects_.end()) {
        return std::nullopt;
    }
    auto rec = read_record(it->second.store, it->second.id);
    return rec ? std::optional<std::uint32_t>(rec->count) : std::nullopt;
}

int LinkStore::link_new(const vfs::Mount& m, const std::string& old_host,
                        const std::string& new_host) {
    std::string id = random_id();
    std::string store = store_of(m);
    std::string object = store + "/" + id;
    // The new name first: symlink(2) fails atomically when it exists, which is the property
    // link-based lock files rely on, and nothing has been touched yet if it does.
    if (::symlink(target_for(m, new_host, id).c_str(), new_host.c_str()) != 0) {
        return errno;
    }
    auto undo_new = [&] { ::unlink(new_host.c_str()); };
    if (::mkdir(store.c_str(), 0700) != 0 && errno != EEXIST) {
        int e = errno;
        undo_new();
        return e;
    }
    StoreLock lock(store);
    Record rec;
    rec.count = 2;
    rec.names = {old_host, new_host};
    if (int e = write_record(store, id, rec); e != 0) {
        undo_new();
        return e;
    }
    if (::rename(old_host.c_str(), object.c_str()) != 0) {
        int e = errno;
        remove_record(store, id);
        undo_new();
        return e;
    }
    if (::symlink(target_for(m, old_host, id).c_str(), old_host.c_str()) != 0) {
        int e = errno;
        ::rename(object.c_str(), old_host.c_str());
        remove_record(store, id);
        undo_new();
        return e;
    }
    remember(store, id);
    return 0;
}

int LinkStore::link_existing(const vfs::Mount& m, const std::string& id,
                             const std::string& new_host) {
    std::string store = store_of(m);
    std::string object = store + "/" + id;
    struct stat st{};
    if (::lstat(object.c_str(), &st) != 0) {
        return ENOENT;
    }
    if (::symlink(target_for(m, new_host, id).c_str(), new_host.c_str()) != 0) {
        return errno;
    }
    StoreLock lock(store);
    // Checked again under the lock: another session may have dropped the last other name and
    // moved the file back over it in the meantime, leaving nothing here to link to.
    if (::lstat(object.c_str(), &st) != 0) {
        ::unlink(new_host.c_str());
        return ENOENT;
    }
    // An object without a record (a session died between its steps) has at least the name
    // that led here.
    Record rec = read_record(store, id).value_or(Record{});
    rec.count += 1;
    if (std::find(rec.names.begin(), rec.names.end(), new_host) == rec.names.end()) {
        rec.names.push_back(new_host);
    }
    if (int e = write_record(store, id, rec); e != 0) {
        ::unlink(new_host.c_str());
        return e;
    }
    remember(store, id);
    return 0;
}

int LinkStore::unlink_name(const vfs::Mount& m, const std::string& id,
                           const std::string& host_name) {
    if (::unlink(host_name.c_str()) != 0) {
        return errno;
    }
    release(store_of(m), id, host_name);
    return 0;
}

void LinkStore::name_dropped(const vfs::Mount& m, const std::string& id,
                             const std::string& host_name) {
    release(store_of(m), id, host_name);
}

void LinkStore::name_moved(const vfs::Mount& m, const std::string& id, const std::string& from,
                           const std::string& to) {
    std::string store = store_of(m);
    StoreLock lock(store);
    std::optional<Record> found = read_record(store, id);
    if (!found) {
        return;
    }
    Record& rec = *found;
    std::replace(rec.names.begin(), rec.names.end(), from, to);
    if (std::find(rec.names.begin(), rec.names.end(), to) == rec.names.end()) {
        rec.names.push_back(to);
    }
    (void)write_record(store, id, rec);
}

void LinkStore::release(const std::string& store, const std::string& id,
                        const std::string& gone) {
    StoreLock lock(store);
    std::optional<Record> found = read_record(store, id);
    if (!found) {
        return; // count unknown (or the object was already restored to a name): leave it
    }
    Record& rec = *found;
    rec.names.erase(std::remove(rec.names.begin(), rec.names.end(), gone), rec.names.end());
    rec.count = rec.count > 0 ? rec.count - 1 : 0;
    std::string object = store + "/" + id;
    if (rec.count == 0) {
        forget(store, id);
        ::unlink(object.c_str());
        remove_record(store, id);
        return;
    }
    if (rec.count == 1) {
        for (const auto& name : rec.names) {
            // Only a name that still links to this object is replaced: names recorded before
            // their directory was renamed no longer point here.
            if (entry_id(name) == id && ::rename(object.c_str(), name.c_str()) == 0) {
                forget(store, id);
                remove_record(store, id);
                return;
            }
        }
    }
    (void)write_record(store, id, rec);
}

} // namespace vhdp::rootless
