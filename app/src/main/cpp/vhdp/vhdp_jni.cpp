// JNI bridge between com.dracxterm.vhdp.VhdpNative (Kotlin) and the libvhdp C ABI
// (include/vhdp/vhdp.h, next to this file). Only the public C ABI is used.
//
// Scope is diagnostics plus an honest session attempt: whatever the library reports is
// passed through unchanged. The rootless engine is supported in an app process now (it
// execs the userland loader from nativeLibraryDir), so a session started here really can
// run, and a failure carries the library's own status/phase -- e.g. E_EXEC_FAILED at the
// start phase for a rootfs without the requested program. Nothing here reports success
// the library did not report.
//
// Rules kept at this boundary (see ADR 0003 and 0008 in the upstream VHDP repository):
//  - Only JNI_OnLoad is exported; natives are registered dynamically.
//  - No C++ exception reaches the JVM: bad_alloc becomes OutOfMemoryError, anything else
//    IllegalStateException.
//  - Every call creates and destroys its own vhdp_context, so no native object outlives a
//    call and concurrent calls share nothing.
//  - Strings cross as UTF-8 byte arrays, never as modified UTF-8, and an embedded NUL is
//    rejected with VHDP_E_INVALID_ARGUMENT instead of being silently truncated.
#include <jni.h>

#include <android/log.h>
#include <vhdp/vhdp.h>

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace {

constexpr const char* kTag = "vhdp-jni";
constexpr const char* kEventTag = "vhdp";

// Verbose library events only in debuggable builds; release keeps warnings and errors.
#ifdef NDEBUG
constexpr vhdp_severity_t kMinSeverity = VHDP_SEVERITY_WARNING;
#else
constexpr vhdp_severity_t kMinSeverity = VHDP_SEVERITY_DEBUG;
#endif

// Upper bound for a session attempt, mirrored by Vhdp.MAX_RUN_TIMEOUT_MS.
constexpr std::int64_t kMaxRunTimeoutMs = 600000;
// Extra time granted to vhdp_session_wait beyond the library-enforced session timeout.
constexpr std::int64_t kWaitGraceMs = 5000;

// Indices of the int[] carried by VhdpResult (mirrored in VhdpResult.kt).
enum ExitSlot : int {
    kPhase = 0,
    kState,
    kReason,
    kExitCode,
    kTermSignal,
    kShellStatus,
    kExitStatus,
    kSlotCount
};

// Phase in which a session attempt stopped (mirrored in VhdpResult.kt).
constexpr std::int32_t kPhaseConfig = 1;
constexpr std::int32_t kPhaseCreate = 2;
constexpr std::int32_t kPhaseStart = 3;
constexpr std::int32_t kPhaseWait = 4;
constexpr std::int32_t kPhaseDone = 5;

jclass gResultClass = nullptr;
jmethodID gResultCtor = nullptr;

int androidPriority(vhdp_severity_t severity) {
    switch (severity) {
        case VHDP_SEVERITY_DEBUG:
            return ANDROID_LOG_DEBUG;
        case VHDP_SEVERITY_INFO:
            return ANDROID_LOG_INFO;
        case VHDP_SEVERITY_WARNING:
            return ANDROID_LOG_WARN;
        default:
            return ANDROID_LOG_ERROR;
    }
}

// Library event stream -> logcat. Runs on library-internal threads; touches nothing but
// the Android log, which is thread-safe.
void onEvent(void*, const vhdp_event* e) {
    if (e == nullptr) {
        return;
    }
    __android_log_print(androidPriority(e->severity), kEventTag,
                        "session=%" PRIu64 " pid=%" PRId64 " kind=%" PRIu32 " code=%" PRId32
                        " %s: %s %s",
                        e->session_id, e->pid, e->kind, e->code, e->name, e->message,
                        e->detail_json);
}

// Copies the calling thread's last libvhdp error. Call only right after a failing vhdp_* call.
std::string lastError() {
    char small[512] = {};
    std::size_t needed = 0;
    vhdp_status_t st = vhdp_last_error_message(small, sizeof(small), &needed);
    if (st == VHDP_E_BUFFER_TOO_SMALL && needed > sizeof(small)) {
        std::string big(needed, '\0');
        if (vhdp_last_error_message(big.data(), big.size(), &needed) == VHDP_OK) {
            big.resize(std::strlen(big.c_str()));
            return big;
        }
    }
    return std::string(small, strnlen(small, sizeof(small)));
}

class Context {
public:
    Context() {
        vhdp_context_options o{};
        o.struct_size = sizeof(o);
        o.abi_version = VHDP_ABI_VERSION;
        o.event_fn = onEvent;
        o.event_user = nullptr;
        o.min_severity = kMinSeverity;
        status_ = vhdp_context_create(&o, &ctx_);
        if (status_ != VHDP_OK) {
            error_ = lastError();
        }
    }
    ~Context() {
        if (ctx_ != nullptr) {
            vhdp_status_t st = vhdp_context_destroy(ctx_);
            if (st != VHDP_OK) {
                __android_log_print(ANDROID_LOG_ERROR, kTag, "vhdp_context_destroy: %s",
                                    vhdp_status_name(st));
            }
        }
    }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    vhdp_context* get() const { return ctx_; }
    vhdp_status_t status() const { return status_; }
    const std::string& error() const { return error_; }

private:
    vhdp_context* ctx_ = nullptr;
    vhdp_status_t status_ = VHDP_E_INTERNAL;
    std::string error_;
};

class Config {
public:
    explicit Config(vhdp_context* ctx) { status_ = vhdp_config_create(ctx, &cfg_); }
    ~Config() { vhdp_config_destroy(cfg_); }
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    vhdp_config* get() const { return cfg_; }
    vhdp_status_t status() const { return status_; }

private:
    vhdp_config* cfg_ = nullptr;
    vhdp_status_t status_ = VHDP_E_INTERNAL;
};

// Declared after Context at every use site, so it is destroyed first.
class Session {
public:
    Session() = default;
    ~Session() {
        if (s_ != nullptr) {
            vhdp_status_t st = vhdp_session_destroy(s_);
            if (st != VHDP_OK) {
                __android_log_print(ANDROID_LOG_ERROR, kTag, "vhdp_session_destroy: %s",
                                    vhdp_status_name(st));
            }
        }
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    vhdp_session** out() { return &s_; }
    vhdp_session* get() const { return s_; }

private:
    vhdp_session* s_ = nullptr;
};

struct Outcome {
    vhdp_status_t status = VHDP_E_INTERNAL;
    std::string message;
    std::string payload;
    std::int32_t exit[kSlotCount] = {};
};

// Runs a JSON query, growing the buffer to the size the library asks for.
template <typename Query>
Outcome queryJson(Query&& query) {
    Outcome out;
    std::vector<char> buf(16 * 1024);
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::size_t needed = 0;
        out.status = query(buf.data(), buf.size(), &needed);
        if (out.status == VHDP_E_BUFFER_TOO_SMALL && needed > buf.size()) {
            buf.assign(needed, '\0');
            continue;
        }
        break;
    }
    if (out.status == VHDP_OK) {
        out.payload.assign(buf.data(), strnlen(buf.data(), buf.size()));
    } else {
        out.message = lastError();
    }
    return out;
}

bool fits(std::size_t n) {
    return n <= static_cast<std::size_t>(std::numeric_limits<jsize>::max());
}

jbyteArray toBytes(JNIEnv* env, const std::string& s) {
    if (!fits(s.size())) {
        env->ThrowNew(env->FindClass("java/lang/OutOfMemoryError"), "string too large for a Java array");
        return nullptr;
    }
    auto n = static_cast<jsize>(s.size());
    jbyteArray a = env->NewByteArray(n);
    if (a == nullptr) {
        return nullptr; // OutOfMemoryError pending
    }
    if (n > 0) {
        env->SetByteArrayRegion(a, 0, n, reinterpret_cast<const jbyte*>(s.data()));
    }
    return a;
}

// UTF-8 bytes from Kotlin. Returns false (nothing pending) when the input holds a NUL.
bool fromBytes(JNIEnv* env, jbyteArray a, std::string& out) {
    out.clear();
    if (a == nullptr) {
        return false;
    }
    jsize n = env->GetArrayLength(a);
    out.resize(static_cast<std::size_t>(n));
    if (n > 0) {
        env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(out.data()));
    }
    return out.find('\0') == std::string::npos;
}

jobject toResult(JNIEnv* env, const Outcome& o) {
    jbyteArray message = toBytes(env, o.message);
    if (message == nullptr) {
        return nullptr;
    }
    jbyteArray payload = toBytes(env, o.payload);
    if (payload == nullptr) {
        return nullptr;
    }
    jintArray exit = env->NewIntArray(kSlotCount);
    if (exit == nullptr) {
        return nullptr;
    }
    env->SetIntArrayRegion(exit, 0, kSlotCount, o.exit);
    return env->NewObject(gResultClass, gResultCtor, static_cast<jint>(o.status), message, payload,
                          exit);
}

void logOutcome(const char* call, const Outcome& o) {
#ifdef NDEBUG
    (void)call;
    (void)o;
#else
    __android_log_print(ANDROID_LOG_DEBUG, kTag, "%s -> %s phase=%" PRId32 " bytes=%zu%s%s", call,
                        vhdp_status_name(o.status), o.exit[kPhase], o.payload.size(),
                        o.message.empty() ? "" : " message=", o.message.c_str());
#endif
}

// Exception containment for every native entry point.
template <typename Fn, typename R>
R guard(JNIEnv* env, R failValue, Fn&& fn) noexcept {
    try {
        return fn();
    } catch (const std::bad_alloc&) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "out of memory in native call");
        env->ThrowNew(env->FindClass("java/lang/OutOfMemoryError"), "vhdp-jni: out of memory");
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "unexpected C++ exception in native call");
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),
                      "vhdp-jni: unexpected native exception");
    }
    return failValue;
}

Outcome contextFailure(const Context& ctx) {
    Outcome o;
    o.status = ctx.status();
    o.message = ctx.error();
    return o;
}

// ---- natives ----

jint nativeAbiVersion(JNIEnv*, jclass) {
    return static_cast<jint>(vhdp_abi_version());
}

jbyteArray nativeVersion(JNIEnv* env, jclass) {
    return guard(env, static_cast<jbyteArray>(nullptr),
                 [&] { return toBytes(env, vhdp_version_string()); });
}

jbyteArray nativeStatusName(JNIEnv* env, jclass, jint status) {
    return guard(env, static_cast<jbyteArray>(nullptr), [&] {
        return toBytes(env, vhdp_status_name(static_cast<vhdp_status_t>(status)));
    });
}

jobject nativeCapabilities(JNIEnv* env, jclass) {
    return guard(env, static_cast<jobject>(nullptr), [&]() -> jobject {
        Context ctx;
        Outcome o = ctx.status() != VHDP_OK ? contextFailure(ctx)
                                            : queryJson([&](char* b, std::size_t c, std::size_t* n) {
                                                  return vhdp_capabilities_json(ctx.get(), b, c, n);
                                              });
        logOutcome("capabilities", o);
        return toResult(env, o);
    });
}

jobject nativeDoctor(JNIEnv* env, jclass, jint flags) {
    return guard(env, static_cast<jobject>(nullptr), [&]() -> jobject {
        Context ctx;
        Outcome o = ctx.status() != VHDP_OK
                        ? contextFailure(ctx)
                        : queryJson([&](char* b, std::size_t c, std::size_t* n) {
                              return vhdp_doctor_json(ctx.get(), static_cast<uint32_t>(flags), b, c,
                                                      n);
                          });
        logOutcome("doctor", o);
        return toResult(env, o);
    });
}

jobject nativeInspectRootfs(JNIEnv* env, jclass, jbyteArray pathBytes) {
    return guard(env, static_cast<jobject>(nullptr), [&]() -> jobject {
        Outcome o;
        std::string path;
        if (!fromBytes(env, pathBytes, path)) {
            o.status = VHDP_E_INVALID_ARGUMENT;
            o.message = "rootfs path is null or contains a NUL byte";
        } else {
            Context ctx;
            o = ctx.status() != VHDP_OK
                    ? contextFailure(ctx)
                    : queryJson([&](char* b, std::size_t c, std::size_t* n) {
                          return vhdp_inspect_rootfs_json(ctx.get(), path.c_str(), b, c, n);
                      });
        }
        logOutcome("inspect", o);
        return toResult(env, o);
    });
}

jobject nativeConfigureRootfs(JNIEnv* env, jclass, jbyteArray pathBytes) {
    return guard(env, static_cast<jobject>(nullptr), [&]() -> jobject {
        Outcome o;
        std::string path;
        if (!fromBytes(env, pathBytes, path)) {
            o.status = VHDP_E_INVALID_ARGUMENT;
            o.message = "rootfs path is null or contains a NUL byte";
        } else {
            Context ctx;
            o = ctx.status() != VHDP_OK
                    ? contextFailure(ctx)
                    : queryJson([&](char* b, std::size_t c, std::size_t* n) {
                          return vhdp_configure_rootfs(ctx.get(), path.c_str(), b, c, n);
                      });
        }
        logOutcome("configure", o);
        return toResult(env, o);
    });
}

jobject nativeDpkgPlan(JNIEnv* env, jclass, jbyteArray pathBytes) {
    return guard(env, static_cast<jobject>(nullptr), [&]() -> jobject {
        Outcome o;
        std::string path;
        if (!fromBytes(env, pathBytes, path)) {
            o.status = VHDP_E_INVALID_ARGUMENT;
            o.message = "rootfs path is null or contains a NUL byte";
        } else {
            Context ctx;
            o = ctx.status() != VHDP_OK
                    ? contextFailure(ctx)
                    : queryJson([&](char* b, std::size_t c, std::size_t* n) {
                          return vhdp_dpkg_plan_json(ctx.get(), path.c_str(), b, c, n);
                      });
        }
        logOutcome("dpkgPlan", o);
        return toResult(env, o);
    });
}

jobject nativeProjectionPlan(JNIEnv* env, jclass) {
    return guard(env, static_cast<jobject>(nullptr), [&]() -> jobject {
        Context ctx;
        Outcome o = ctx.status() != VHDP_OK
                        ? contextFailure(ctx)
                        : queryJson([&](char* b, std::size_t c, std::size_t* n) {
                              return vhdp_projection_plan_json(ctx.get(), b, c, n);
                          });
        logOutcome("projectionPlan", o);
        return toResult(env, o);
    });
}

void fillExit(Outcome& o, const vhdp_exit_info& info) {
    o.exit[kState] = static_cast<std::int32_t>(info.state);
    o.exit[kReason] = static_cast<std::int32_t>(info.reason);
    o.exit[kExitCode] = info.exit_code;
    o.exit[kTermSignal] = info.term_signal;
    o.exit[kShellStatus] = info.shell_status;
    o.exit[kExitStatus] = static_cast<std::int32_t>(info.status);
}

Outcome runSession(JNIEnv* env, jbyteArray rootfsBytes, jobjectArray argvArr, jlong timeoutMs) {
    Outcome o;
    o.exit[kPhase] = kPhaseConfig;
    auto reject = [&](const char* why) {
        o.status = VHDP_E_INVALID_ARGUMENT;
        o.message = why;
        return o;
    };

    if (timeoutMs <= 0 || timeoutMs > kMaxRunTimeoutMs) {
        return reject("timeoutMs must be in 1..600000");
    }
    std::string rootfs;
    if (!fromBytes(env, rootfsBytes, rootfs)) {
        return reject("rootfs path is null or contains a NUL byte");
    }
    std::vector<std::string> argv;
    jsize argc = argvArr == nullptr ? 0 : env->GetArrayLength(argvArr);
    argv.reserve(static_cast<std::size_t>(argc));
    for (jsize i = 0; i < argc; ++i) {
        auto el = static_cast<jbyteArray>(env->GetObjectArrayElement(argvArr, i));
        std::string arg;
        bool ok = fromBytes(env, el, arg);
        env->DeleteLocalRef(el);
        if (!ok) {
            return reject("argv element is null or contains a NUL byte");
        }
        argv.push_back(std::move(arg));
    }
    std::vector<const char*> argvPtrs;
    argvPtrs.reserve(argv.size());
    for (const auto& a : argv) {
        argvPtrs.push_back(a.c_str());
    }

    Context ctx;
    if (ctx.status() != VHDP_OK) {
        Outcome f = contextFailure(ctx);
        f.exit[kPhase] = kPhaseConfig;
        return f;
    }
    Session session; // destroyed before ctx
    {
        Config cfg(ctx.get());
        auto step = [&](vhdp_status_t st) {
            if (st != VHDP_OK && o.status == VHDP_E_INTERNAL) {
                o.status = st;
                o.message = lastError();
            }
            return st == VHDP_OK;
        };
        o.status = VHDP_E_INTERNAL;
        bool configured = step(cfg.status()) && step(vhdp_config_set_rootfs(cfg.get(), rootfs.c_str())) &&
                          step(vhdp_config_set_argv(cfg.get(), argvPtrs.size(),
                                                    argvPtrs.empty() ? nullptr : argvPtrs.data())) &&
                          step(vhdp_config_set_timeout_ms(cfg.get(),
                                                          static_cast<std::uint64_t>(timeoutMs)));
        if (!configured) {
            return o;
        }
        o.exit[kPhase] = kPhaseCreate;
        vhdp_status_t st = vhdp_session_create(ctx.get(), cfg.get(), session.out());
        if (st != VHDP_OK) {
            o.status = st;
            o.message = lastError();
            return o;
        }
    }

    o.exit[kPhase] = kPhaseStart;
    vhdp_status_t st = vhdp_session_start(session.get());
    if (st != VHDP_OK) {
        o.status = st;
        o.message = lastError();
        vhdp_session_state_t state = VHDP_STATE_CREATED;
        if (vhdp_session_state(session.get(), &state) == VHDP_OK) {
            o.exit[kState] = static_cast<std::int32_t>(state);
        }
        return o;
    }

    o.exit[kPhase] = kPhaseWait;
    vhdp_exit_info info{};
    info.struct_size = sizeof(info);
    info.abi_version = VHDP_ABI_VERSION;
    st = vhdp_session_wait(session.get(), timeoutMs + kWaitGraceMs, &info);
    if (st == VHDP_E_TIMEOUT) {
        // The library-side timeout should have fired first; do not leave a guest behind.
        __android_log_print(ANDROID_LOG_WARN, kTag, "session outlived its timeout; cancelling");
        (void)vhdp_session_cancel(session.get());
        st = vhdp_session_wait(session.get(), -1, &info);
    }
    if (st != VHDP_OK) {
        o.status = st;
        o.message = lastError();
        return o;
    }
    o.exit[kPhase] = kPhaseDone;
    o.status = VHDP_OK;
    fillExit(o, info);
    return o;
}

jobject nativeRun(JNIEnv* env, jclass, jbyteArray rootfs, jobjectArray argv, jlong timeoutMs) {
    return guard(env, static_cast<jobject>(nullptr), [&]() -> jobject {
        Outcome o = runSession(env, rootfs, argv, timeoutMs);
        if (env->ExceptionCheck()) {
            return nullptr;
        }
        logOutcome("run", o);
        return toResult(env, o);
    });
}

const JNINativeMethod kMethods[] = {
    {"nativeAbiVersion", "()I", reinterpret_cast<void*>(nativeAbiVersion)},
    {"nativeVersion", "()[B", reinterpret_cast<void*>(nativeVersion)},
    {"nativeStatusName", "(I)[B", reinterpret_cast<void*>(nativeStatusName)},
    {"nativeCapabilities", "()Lcom/dracxterm/vhdp/VhdpResult;",
     reinterpret_cast<void*>(nativeCapabilities)},
    {"nativeDoctor", "(I)Lcom/dracxterm/vhdp/VhdpResult;", reinterpret_cast<void*>(nativeDoctor)},
    {"nativeInspectRootfs", "([B)Lcom/dracxterm/vhdp/VhdpResult;",
     reinterpret_cast<void*>(nativeInspectRootfs)},
    {"nativeConfigureRootfs", "([B)Lcom/dracxterm/vhdp/VhdpResult;",
     reinterpret_cast<void*>(nativeConfigureRootfs)},
    {"nativeDpkgPlan", "([B)Lcom/dracxterm/vhdp/VhdpResult;",
     reinterpret_cast<void*>(nativeDpkgPlan)},
    {"nativeProjectionPlan", "()Lcom/dracxterm/vhdp/VhdpResult;",
     reinterpret_cast<void*>(nativeProjectionPlan)},
    {"nativeRun", "([B[[BJ)Lcom/dracxterm/vhdp/VhdpResult;", reinterpret_cast<void*>(nativeRun)},
};

} // namespace

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    if (vhdp_abi_version() != VHDP_ABI_VERSION) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "libvhdp ABI %u, bridge built for %u",
                            vhdp_abi_version(), VHDP_ABI_VERSION);
        return JNI_ERR;
    }
    jclass result = env->FindClass("com/dracxterm/vhdp/VhdpResult");
    if (result == nullptr) {
        return JNI_ERR;
    }
    gResultCtor = env->GetMethodID(result, "<init>", "(I[B[B[I)V");
    if (gResultCtor == nullptr) {
        return JNI_ERR;
    }
    gResultClass = static_cast<jclass>(env->NewGlobalRef(result));
    env->DeleteLocalRef(result);
    if (gResultClass == nullptr) {
        return JNI_ERR;
    }
    jclass native = env->FindClass("com/dracxterm/vhdp/VhdpNative");
    if (native == nullptr) {
        return JNI_ERR;
    }
    auto count = static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0]));
    jint rc = env->RegisterNatives(native, kMethods, count);
    env->DeleteLocalRef(native);
    if (rc != JNI_OK) {
        return JNI_ERR;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "libvhdp %s (ABI %u) registered",
                        vhdp_version_string(), vhdp_abi_version());
    return JNI_VERSION_1_6;
}
