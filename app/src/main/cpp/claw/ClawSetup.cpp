// Native setup of the in-guest `claw` command (com.dracxterm.claw.ClawSetup -> libclawsetup.so).
//
// claw ships in the APK under assets/claw: its sources, its data (providers.json, models.json,
// the repair script), the OpenClaw skills, the Scrapling sources, and one static musl build per
// ABI at bin/<abi>/claw (built by prebuilts/claw/build.sh). This file puts the runtime parts of
// that tree into the rootfs and a `claw` command onto the guest PATH:
//
//   /opt/claw/bin/claw        the static binary for this device's ABI
//   /opt/claw/data/...        providers.json, models.json, repair-sources.json, scripts/, state/
//   /opt/claw/skills/...      OpenClaw skills `claw run` hands to OpenClaw
//   /opt/claw/scrapling/...   Scrapling sources, for `claw repair` once the user makes a venv
//   /opt/claw/share/ca-certificates.crt   the device's trusted CAs, for a rootfs without its own
//   /usr/local/bin/claw       the launcher (a POSIX sh script, see kLauncher)
//
// Without a rootfs the terminal runs BusyBox, where claw cannot resolve names (Android has no
// /etc/resolv.conf), so a stub that says so goes onto the BusyBox PATH instead.
//
// Rules kept here:
//  - The user's data is never overwritten. providers.json, models.json and repair-sources.json
//    are rewritten by `claw scan`, `check --prune` and `repair`, so they are only seeded when
//    missing. Nothing is ever deleted.
//  - Every file is written to a temporary name and renamed into place, so a crash or a full disk
//    leaves the previous copy intact instead of a truncated binary.
//  - The bulk copy runs only when the stamp (the APK's update time and version) changes; the
//    launcher is compared and rewritten on every launch, which costs one small read.
//  - No C++ exception and no Java exception escapes to the caller; failures come back as text
//    for the log.
#include <jni.h>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* kTag = "claw-setup";
constexpr const char* kGuestRoot = "/opt/claw";

// Seeded once, then owned by the user (claw rewrites them itself).
constexpr const char* kUserOwned[] = {
    "data/providers.json",
    "data/models.json",
    "data/repair-sources.json",
};

// Asset subtrees that reach the guest. src/, third_party/, tests/ and CMakeLists.txt stay in
// the APK only, as the source of the shipped binary.
constexpr const char* kRuntimeTrees[] = {"data", "skills", "scrapling"};

// The guest launcher. @ROOT@ is replaced with kGuestRoot.
//
// CLAW_DATA_DIR is exported because claw otherwise finds its data next to /proc/self/exe, and
// under the VHDP backend that link names the userland loader, not /opt/claw/bin/claw.
//
// CA store: the rootfs's own bundle wins, so `update-ca-certificates` keeps working as usual.
// Only a rootfs without one falls back to the copy of Android's store made at setup time.
constexpr const char* kLauncher = R"SH(#!/bin/sh
# drac-Xterm `claw` launcher, written by the app on every start
# (app/src/main/cpp/claw/ClawSetup.cpp). Edits here are overwritten.
CLAW_ROOT="@ROOT@"
if [ ! -x "$CLAW_ROOT/bin/claw" ]; then
    echo "claw: $CLAW_ROOT/bin/claw is missing. Restart drac-Xterm to reinstall it." >&2
    exit 127
fi
if [ -z "${CLAW_DATA_DIR:-}" ]; then
    CLAW_DATA_DIR="$CLAW_ROOT/data"
    export CLAW_DATA_DIR
fi
if [ -z "${SSL_CERT_FILE:-}" ]; then
    for f in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt \
             /etc/ssl/ca-bundle.pem /etc/ssl/cert.pem "$CLAW_ROOT/share/ca-certificates.crt"; do
        if [ -s "$f" ]; then
            SSL_CERT_FILE="$f"
            export SSL_CERT_FILE
            break
        fi
    done
fi
exec "$CLAW_ROOT/bin/claw" "$@"
)SH";

constexpr const char* kBusyboxStub = R"SH(#!/bin/sh
# drac-Xterm `claw` stub for the BusyBox shell (no Linux rootfs installed).
echo "claw needs the Linux rootfs. Install one from the terminal's setup screen, then run claw again." >&2
exit 127
)SH";

constexpr const char* kUnsupportedAbi = R"SH(#!/bin/sh
# drac-Xterm `claw` launcher: this device's ABI has no claw build in the APK.
echo "claw is not available for this device (@ABI@). The APK ships arm64-v8a and x86_64 builds." >&2
exit 127
)SH";

void logi(const std::string& s) { __android_log_print(ANDROID_LOG_INFO, kTag, "%s", s.c_str()); }
void logw(const std::string& s) { __android_log_print(ANDROID_LOG_WARN, kTag, "%s", s.c_str()); }

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
        s.replace(p, from.size(), to);
    return s;
}

bool mkdirs(const std::string& path, mode_t mode) {
    if (path.empty()) return false;
    std::string cur;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        cur = path.substr(0, j);
        i = j + 1;
        if (cur.empty()) continue;
        if (mkdir(cur.c_str(), mode) != 0 && errno != EEXIST) return false;
    }
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string parent_of(const std::string& p) {
    size_t s = p.rfind('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

bool exists(const std::string& p) {
    struct stat st{};
    return lstat(p.c_str(), &st) == 0;
}

bool read_file(const std::string& p, std::string& out) {
    int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    out.clear();
    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    close(fd);
    return n == 0;
}

// Writes through a temporary sibling and renames it over dest: the old file stays whole until
// the new one is complete and synced.
class AtomicFile {
 public:
    AtomicFile(const std::string& dest, mode_t mode) : dest_(dest), mode_(mode) {
        tmp_ = dest + ".claw-tmp";
        unlink(tmp_.c_str());
        fd_ = open(tmp_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    }
    ~AtomicFile() {
        if (fd_ >= 0) close(fd_);
        if (!committed_) unlink(tmp_.c_str());
    }
    bool ok() const { return fd_ >= 0 && !failed_; }
    void write_all(const void* data, size_t len) {
        const char* p = static_cast<const char*>(data);
        while (len > 0 && fd_ >= 0 && !failed_) {
            ssize_t n = ::write(fd_, p, len);
            if (n < 0) {
                if (errno == EINTR) continue;
                failed_ = true;
                break;
            }
            p += n;
            len -= static_cast<size_t>(n);
        }
    }
    bool commit() {
        if (!ok()) return false;
        if (fsync(fd_) != 0 || fchmod(fd_, mode_) != 0) return false;
        close(fd_);
        fd_ = -1;
        if (rename(tmp_.c_str(), dest_.c_str()) != 0) return false;
        committed_ = true;
        return true;
    }

 private:
    std::string dest_, tmp_;
    mode_t mode_;
    int fd_ = -1;
    bool failed_ = false;
    bool committed_ = false;
};

bool write_text(const std::string& dest, const std::string& body, mode_t mode) {
    std::string cur;
    if (read_file(dest, cur) && cur == body) {
        chmod(dest.c_str(), mode);
        return true;
    }
    if (!mkdirs(parent_of(dest), 0755)) return false;
    AtomicFile f(dest, mode);
    f.write_all(body.data(), body.size());
    return f.commit();
}

bool copy_asset(AAssetManager* am, const std::string& asset, const std::string& dest, mode_t mode) {
    AAsset* a = AAssetManager_open(am, asset.c_str(), AASSET_MODE_STREAMING);
    if (a == nullptr) return false;
    bool ok = mkdirs(parent_of(dest), 0755);
    if (ok) {
        AtomicFile f(dest, mode);
        char buf[65536];
        // -1 until the asset has been read to its end, so a temp file that never opened
        // cannot pass as a complete copy.
        int n = -1;
        while (f.ok() && (n = AAsset_read(a, buf, sizeof(buf))) > 0) f.write_all(buf, static_cast<size_t>(n));
        ok = n == 0 && f.commit();
    }
    AAsset_close(a);
    return ok;
}

bool asset_exists(AAssetManager* am, const std::string& asset) {
    AAsset* a = AAssetManager_open(am, asset.c_str(), AASSET_MODE_UNKNOWN);
    if (a == nullptr) return false;
    AAsset_close(a);
    return true;
}

// AAssetDir only lists files, never subdirectories, so walking a tree needs the Java
// AssetManager.list(), which returns both.
bool list_assets(JNIEnv* env, jobject jam, const std::string& dir, std::vector<std::string>& out) {
    jclass cls = env->GetObjectClass(jam);
    jmethodID list = env->GetMethodID(cls, "list", "(Ljava/lang/String;)[Ljava/lang/String;");
    env->DeleteLocalRef(cls);
    if (list == nullptr) {
        env->ExceptionClear();
        return false;
    }
    jstring jdir = env->NewStringUTF(dir.c_str());
    auto arr = static_cast<jobjectArray>(env->CallObjectMethod(jam, list, jdir));
    env->DeleteLocalRef(jdir);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    out.clear();
    if (arr == nullptr) return true;
    jsize n = env->GetArrayLength(arr);
    for (jsize i = 0; i < n; ++i) {
        auto s = static_cast<jstring>(env->GetObjectArrayElement(arr, i));
        if (s == nullptr) continue;
        const char* c = env->GetStringUTFChars(s, nullptr);
        if (c != nullptr) {
            out.emplace_back(c);
            env->ReleaseStringUTFChars(s, c);
        }
        env->DeleteLocalRef(s);
    }
    env->DeleteLocalRef(arr);
    return true;
}

bool is_user_owned(const std::string& rel) {
    for (const char* u : kUserOwned)
        if (rel == u) return true;
    return false;
}

struct CopyStats {
    int copied = 0;
    int kept = 0;
    int failed = 0;
};

// Copies assets/claw/<rel> (a file or a directory) to <root>/<rel>.
void copy_tree(JNIEnv* env, jobject jam, AAssetManager* am, const std::string& rel,
               const std::string& root, CopyStats& st, int depth = 0) {
    if (depth > 16) return;
    const std::string asset = "claw/" + rel;
    std::vector<std::string> kids;
    if (!list_assets(env, jam, asset, kids)) {
        ++st.failed;
        return;
    }
    if (!kids.empty()) {
        // Data the guest user writes to (models.json, state/) must stay writable whichever
        // uid the backend presents; the directory lives in app-private storage either way.
        mode_t mode = rel.rfind("data", 0) == 0 ? 0777 : 0755;
        mkdirs(root + "/" + rel, mode);
        chmod((root + "/" + rel).c_str(), mode);
        for (const auto& k : kids) copy_tree(env, jam, am, rel + "/" + k, root, st, depth + 1);
        return;
    }
    const std::string dest = root + "/" + rel;
    if (is_user_owned(rel) && exists(dest)) {
        ++st.kept;
        return;
    }
    mode_t mode = rel.rfind("data", 0) == 0 ? 0666 : 0644;
    if (rel.size() > 3 && (rel.compare(rel.size() - 3, 3, ".sh") == 0 || rel.compare(rel.size() - 3, 3, ".py") == 0))
        mode |= 0111;
    if (copy_asset(am, asset, dest, mode))
        ++st.copied;
    else
        ++st.failed;
}

// Concatenates Android's trusted CA certificates into one PEM bundle. Each file in the store is
// a PEM block followed by a text dump; OpenSSL's PEM reader skips everything outside the
// BEGIN/END lines, so the files can be appended as they are.
int write_ca_bundle(const std::string& dest) {
    const char* stores[] = {"/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts"};
    for (const char* store : stores) {
        DIR* d = opendir(store);
        if (d == nullptr) continue;
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] != '.') names.emplace_back(e->d_name);
        }
        closedir(d);
        if (names.empty()) continue;
        std::string bundle;
        int count = 0;
        for (const auto& n : names) {
            std::string body;
            if (!read_file(std::string(store) + "/" + n, body)) continue;
            size_t b = body.find("-----BEGIN CERTIFICATE-----");
            size_t e = body.find("-----END CERTIFICATE-----", b);
            if (b == std::string::npos || e == std::string::npos) continue;
            bundle.append(body, b, e + 25 - b);
            bundle.push_back('\n');
            ++count;
        }
        if (count == 0) continue;
        return write_text(dest, bundle, 0644) ? count : -1;
    }
    return 0;
}

std::string from_bytes(JNIEnv* env, jbyteArray a) {
    if (a == nullptr) return {};
    jsize n = env->GetArrayLength(a);
    std::string s(static_cast<size_t>(n), '\0');
    env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(s.data()));
    return s;
}

jbyteArray to_bytes(JNIEnv* env, const std::string& s) {
    jbyteArray a = env->NewByteArray(static_cast<jsize>(s.size()));
    if (a != nullptr)
        env->SetByteArrayRegion(a, 0, static_cast<jsize>(s.size()), reinterpret_cast<const jbyte*>(s.data()));
    return a;
}

std::string install_busybox_stub(const std::string& bin_dir, bool& ok) {
    ok = false;
    if (bin_dir.empty()) return "no busybox bin dir";
    if (!write_text(bin_dir + "/claw", kBusyboxStub, 0755))
        return "busybox stub write failed: " + std::string(strerror(errno));
    ok = true;
    return "busybox stub installed at " + bin_dir + "/claw";
}

std::string install_guest(JNIEnv* env, jobject jam, AAssetManager* am, const std::string& rootfs,
                          const std::string& abi, const std::string& stamp, bool& ok) {
    ok = false;
    const std::string root = rootfs + kGuestRoot;
    const std::string launcher = rootfs + "/usr/local/bin/claw";
    const std::string bin_asset = "claw/bin/" + abi + "/claw";

    if (!asset_exists(am, bin_asset)) {
        ok = write_text(launcher, replace_all(kUnsupportedAbi, "@ABI@", abi), 0755);
        return "no claw build for " + abi + "; unsupported-ABI launcher installed";
    }

    const std::string stamp_path = root + "/.setup-stamp";
    std::string have;
    bool fresh = read_file(stamp_path, have) && have == stamp && access((root + "/bin/claw").c_str(), X_OK) == 0;

    std::string report;
    bool complete = true;
    if (!fresh) {
        if (!mkdirs(root + "/bin", 0755)) return "cannot create " + root + ": " + strerror(errno);
        if (!copy_asset(am, bin_asset, root + "/bin/claw", 0755))
            return "copying the claw binary failed: " + std::string(strerror(errno));

        CopyStats st;
        for (const char* t : kRuntimeTrees) copy_tree(env, jam, am, t, root, st);
        mkdirs(root + "/data/state", 0777);
        chmod((root + "/data/state").c_str(), 0777);

        int cas = write_ca_bundle(root + "/share/ca-certificates.crt");
        // A partial copy leaves the stamp alone, so the next launch tries again.
        complete = st.failed == 0;
        if (complete) write_text(stamp_path, stamp, 0644);
        report = "installed " + abi + " build, " + std::to_string(st.copied) + " files copied, " +
                 std::to_string(st.kept) + " user files kept, " + std::to_string(st.failed) + " failed, " +
                 std::to_string(cas) + " device CAs";
    } else {
        report = "up to date";
    }

    if (!write_text(launcher, replace_all(kLauncher, "@ROOT@", kGuestRoot), 0755))
        return report + "; launcher write failed: " + strerror(errno);
    ok = complete;
    return report;
}

jbyteArray native_install(JNIEnv* env, jclass, jobject jam, jbyteArray jrootfs, jbyteArray jbusybox,
                          jbyteArray jabi, jbyteArray jstamp) {
    std::string result;
    bool ok = false;
    try {
        AAssetManager* am = jam != nullptr ? AAssetManager_fromJava(env, jam) : nullptr;
        if (am == nullptr) {
            result = "no AssetManager";
        } else {
            std::string rootfs = from_bytes(env, jrootfs);
            if (rootfs.empty())
                result = install_busybox_stub(from_bytes(env, jbusybox), ok);
            else
                result = install_guest(env, jam, am, rootfs, from_bytes(env, jabi), from_bytes(env, jstamp), ok);
        }
    } catch (const std::exception& e) {
        result = std::string("setup error: ") + e.what();
    } catch (...) {
        result = "setup error";
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    (ok ? logi : logw)(result);
    return to_bytes(env, result);
}

const JNINativeMethod kMethods[] = {
    {"nativeInstall", "(Landroid/content/res/AssetManager;[B[B[B[B)[B",
     reinterpret_cast<void*>(native_install)},
};

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass cls = env->FindClass("com/dracxterm/claw/ClawSetup");
    if (cls == nullptr) return JNI_ERR;
    jint rc = env->RegisterNatives(cls, kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
    env->DeleteLocalRef(cls);
    return rc == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
