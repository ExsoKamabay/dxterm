/*
 * libvhdp - Virtual Hardware Driver Platform, stable C ABI.
 *
 * ABI rules (see docs/EMBEDDING.md and docs/adr/0005-c-abi.md):
 *  - Every status/flag/kind is a fixed-width integer typedef with named constants.
 *    No C enum, bool, bitfield, C++ type or packed struct crosses this boundary.
 *  - Versioned structs start with {uint32_t struct_size; uint32_t abi_version;}.
 *    Callers set struct_size = sizeof(T) and abi_version = VHDP_ABI_VERSION.
 *    Reserved fields must be zero; the library rejects non-zero reserved fields.
 *  - Handles are opaque and owned by the caller between *_create and *_destroy.
 *  - Strings passed in are copied; the library never retains caller pointers
 *    (except callback user pointers, whose lifetime rules are stated below).
 *  - Strings passed out are either static (documented), valid only for the
 *    duration of a callback, or written into caller-provided buffers.
 *  - No C++ exception crosses this boundary; failures are reported as status codes.
 *
 * Thread safety:
 *  - vhdp_abi_version, vhdp_version_string, vhdp_status_name: any thread.
 *  - vhdp_context: create/destroy sessions from any thread; query functions are
 *    internally synchronised.
 *  - vhdp_config: single owner, not synchronised.
 *  - vhdp_session: start/wait/state/signal/resize/write_input/cancel are safe to
 *    call concurrently from different threads. vhdp_session_destroy must not race
 *    with any other call on the same session.
 *
 * Callback lifetime:
 *  - Callbacks run on library-internal threads (session supervisor, I/O pump,
 *    watchdog) or on the thread calling vhdp_session_start.
 *  - Data pointers passed to a callback are valid only until it returns.
 *  - No session callback is invoked after vhdp_session_destroy returns; no
 *    context callback after vhdp_context_destroy returns.
 *  - Calling vhdp_session_wait or vhdp_session_destroy from inside a callback of
 *    the same session returns VHDP_E_BUSY instead of deadlocking.
 *  - Callbacks must return promptly; they are invoked synchronously and apply
 *    backpressure to the guest.
 *
 * Isolation: the rootless engine provides compatibility isolation (path
 * translation), NOT a security sandbox. Do not run hostile root filesystems.
 */
#ifndef VHDP_VHDP_H
#define VHDP_VHDP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(VHDP_BUILDING_LIBRARY)
#define VHDP_API __attribute__((visibility("default")))
#else
#define VHDP_API
#endif

#define VHDP_ABI_VERSION 1u

/* ---- Status codes (stable values, never renumbered) ---- */
typedef uint32_t vhdp_status_t;
#define VHDP_OK 0u
#define VHDP_E_INVALID_ARGUMENT 1u
#define VHDP_E_ABI_MISMATCH 2u
#define VHDP_E_NO_MEMORY 3u
#define VHDP_E_INVALID_STATE 4u
#define VHDP_E_UNSUPPORTED 5u
#define VHDP_E_ENGINE_UNAVAILABLE 6u
#define VHDP_E_ROOTFS_INVALID 7u
#define VHDP_E_BIND_INVALID 8u
#define VHDP_E_EXEC_FAILED 9u
#define VHDP_E_PERMISSION_DENIED 10u
#define VHDP_E_TIMEOUT 11u
#define VHDP_E_BUFFER_TOO_SMALL 12u
#define VHDP_E_IO 13u
#define VHDP_E_BUSY 14u
#define VHDP_E_CANCELLED 15u
#define VHDP_E_NOT_FOUND 16u
#define VHDP_E_HOST_ENVIRONMENT 17u
#define VHDP_E_INTERNAL 255u

/* ---- Engine kinds ---- */
typedef uint32_t vhdp_engine_kind_t;
#define VHDP_ENGINE_AUTO 0u
#define VHDP_ENGINE_ROOTLESS 1u
#define VHDP_ENGINE_ROOTED 2u
#define VHDP_ENGINE_EMULATOR 3u
#define VHDP_ENGINE_VM 4u

/* ---- Policies ---- */
typedef uint32_t vhdp_network_t;
#define VHDP_NETWORK_HOST 0u
#define VHDP_NETWORK_NONE 1u

typedef uint32_t vhdp_stdio_mode_t;
#define VHDP_STDIO_INHERIT 0u /* guest uses the caller's fds 0,1,2 */
#define VHDP_STDIO_PTY 1u     /* library allocates a PTY; guest gets it as controlling tty */
#define VHDP_STDIO_FDS 2u     /* caller-provided fds, duplicated at start */

typedef uint32_t vhdp_seccomp_mode_t;
#define VHDP_SECCOMP_AUTO 0u /* use seccomp acceleration when the probe succeeds */
#define VHDP_SECCOMP_ON 1u   /* require seccomp; fail start if unavailable */
#define VHDP_SECCOMP_OFF 2u  /* trace every syscall with PTRACE_SYSCALL */

typedef uint32_t vhdp_dev_mode_t;
#define VHDP_DEV_MINIMAL 0u /* project null, zero, full, random, urandom, tty, ptmx, pts */
#define VHDP_DEV_NONE 1u

typedef uint32_t vhdp_proc_mode_t;
#define VHDP_PROC_NONE 0u
#define VHDP_PROC_HOST 1u /* read-only host /proc with magic-link policy */

typedef uint32_t vhdp_bind_flags_t;
#define VHDP_BIND_READ_ONLY 0u
#define VHDP_BIND_READ_WRITE 1u

/* ---- Session state / exit reason ---- */
typedef uint32_t vhdp_session_state_t;
#define VHDP_STATE_CREATED 0u
#define VHDP_STATE_STARTING 1u
#define VHDP_STATE_RUNNING 2u
#define VHDP_STATE_EXITED 3u
#define VHDP_STATE_FAILED 4u
#define VHDP_STATE_CANCELLED 5u

typedef uint32_t vhdp_exit_reason_t;
#define VHDP_EXIT_NONE 0u
#define VHDP_EXIT_NORMAL 1u    /* guest called exit; exit_code valid */
#define VHDP_EXIT_SIGNALED 2u  /* guest killed by term_signal */
#define VHDP_EXIT_TIMEOUT 3u   /* configured timeout expired; tree killed */
#define VHDP_EXIT_CANCELLED 4u /* vhdp_session_cancel or destroy while running */
#define VHDP_EXIT_START_FAILED 5u

/* ---- Events ---- */
typedef uint32_t vhdp_event_kind_t;
#define VHDP_EVENT_LIFECYCLE 1u
#define VHDP_EVENT_DIAGNOSTIC 2u
#define VHDP_EVENT_LOG 3u
#define VHDP_EVENT_TRACE 4u

typedef uint32_t vhdp_severity_t;
#define VHDP_SEVERITY_DEBUG 0u
#define VHDP_SEVERITY_INFO 1u
#define VHDP_SEVERITY_WARNING 2u
#define VHDP_SEVERITY_ERROR 3u

typedef uint32_t vhdp_stream_t;
#define VHDP_STREAM_STDOUT 1u /* PTY output is reported as STDOUT */
#define VHDP_STREAM_STDERR 2u

typedef struct vhdp_event {
    uint32_t struct_size;
    uint32_t abi_version;
    vhdp_event_kind_t kind;
    vhdp_severity_t severity;
    uint64_t session_id;   /* 0 for context-level events */
    int64_t pid;           /* host pid of the guest process, or 0 */
    uint64_t timestamp_ns; /* CLOCK_MONOTONIC */
    int32_t code;          /* Linux errno for diagnostics, status for lifecycle, else 0 */
    uint32_t reserved0;
    const char* name;        /* machine-readable, e.g. "syscall.denied"; never NULL */
    const char* message;     /* human-readable; never NULL */
    const char* detail_json; /* JSON object text; never NULL ("{}" when empty) */
    uint64_t reserved[4];
} vhdp_event;

typedef void (*vhdp_event_fn)(void* user, const vhdp_event* event);
typedef void (*vhdp_output_fn)(void* user, vhdp_stream_t stream, const uint8_t* data, size_t len);

/* ---- Opaque handles ---- */
typedef struct vhdp_context vhdp_context;
typedef struct vhdp_config vhdp_config;
typedef struct vhdp_session vhdp_session;

typedef struct vhdp_context_options {
    uint32_t struct_size;
    uint32_t abi_version;
    vhdp_event_fn event_fn; /* optional; receives context and session events */
    void* event_user;       /* must stay valid until vhdp_context_destroy returns */
    vhdp_severity_t min_severity;
    uint32_t flags; /* must be 0 */
    uint64_t reserved[4];
} vhdp_context_options;

typedef struct vhdp_exit_info {
    uint32_t struct_size;
    uint32_t abi_version;
    vhdp_session_state_t state;
    vhdp_exit_reason_t reason;
    int32_t exit_code;    /* valid when reason == VHDP_EXIT_NORMAL */
    int32_t term_signal;  /* valid when reason == VHDP_EXIT_SIGNALED */
    int32_t shell_status; /* exit_code, 128+signal, 124 timeout, 125 start failure/cancel */
    vhdp_status_t status; /* start failure status, else VHDP_OK */
    uint64_t reserved[4];
} vhdp_exit_info;

/* ---- Library info ---- */
VHDP_API uint32_t vhdp_abi_version(void);
/* Static string, e.g. "0.1.0". */
VHDP_API const char* vhdp_version_string(void);
/* Static string such as "VHDP_E_INVALID_ARGUMENT"; "VHDP_E_UNKNOWN" for unknown values. */
VHDP_API const char* vhdp_status_name(vhdp_status_t status);
/*
 * Copies the calling thread's most recent error message (set by the last failing
 * vhdp_* call on this thread) into buf. *needed receives strlen+1. Returns
 * VHDP_E_BUFFER_TOO_SMALL when cap is insufficient (buf gets a truncated copy).
 */
VHDP_API vhdp_status_t vhdp_last_error_message(char* buf, size_t cap, size_t* needed);

/* ---- Context ---- */
VHDP_API vhdp_status_t vhdp_context_create(const vhdp_context_options* options,
                                           vhdp_context** out_context);
/* Returns VHDP_E_BUSY (and destroys nothing) while sessions created from it are alive. NULL is OK.
 */
VHDP_API vhdp_status_t vhdp_context_destroy(vhdp_context* context);

/*
 * JSON query functions write a NUL-terminated UTF-8 JSON document into buf.
 * *needed receives the required size including NUL. With cap too small the call
 * returns VHDP_E_BUFFER_TOO_SMALL; the result is cached in the context so an
 * immediate retry with a larger buffer returns the same document.
 */
#define VHDP_DOCTOR_NO_ACTIVE_PROBES 1u /* read-only inspection only */
#define VHDP_DOCTOR_REFRESH 2u          /* ignore the cached report */
VHDP_API vhdp_status_t vhdp_doctor_json(vhdp_context* context, uint32_t flags, char* buf,
                                        size_t cap, size_t* needed);
VHDP_API vhdp_status_t vhdp_capabilities_json(vhdp_context* context, char* buf, size_t cap,
                                              size_t* needed);
VHDP_API vhdp_status_t vhdp_inspect_rootfs_json(vhdp_context* context, const char* rootfs_path,
                                                char* buf, size_t cap, size_t* needed);

/* Configure an already-extracted rootfs for first boot: proot bind mountpoints, working DNS and
 * hosts, a login profile, and a normal 'dracos' user with passwordless sudo. Pure filesystem
 * work (no engine/ptrace), so it runs in an app process too. Idempotent (resolv.conf is the one
 * deliberate overwrite). Writes a JSON report {input, rootfs, status, actions[]} into buf. */
VHDP_API vhdp_status_t vhdp_configure_rootfs(vhdp_context* context, const char* rootfs_path,
                                             char* buf, size_t cap, size_t* needed);

/* Decide dpkg/apt recovery for a rootfs. Inspects the package-manager state and writes a JSON plan
 * {input, rootfs, is_dpkg_rootfs, status_db, locks[], needs_recovery, script} into buf. VHDP owns
 * the decision and the recovery script but does NOT execute it (running dpkg needs to exec guest
 * binaries): the caller runs `script` through the proot execution backend as a fake-root pass. */
VHDP_API vhdp_status_t vhdp_dpkg_plan_json(vhdp_context* context, const char* rootfs_path,
                                           char* buf, size_t cap, size_t* needed);

/* Decide the guest system/device/arch projection: the host arch/kernel/page-size VHDP reads from
 * the device, plus which host trees (/dev, /proc, /sys) to project into the guest. Writes a JSON
 * plan {host, system_binds[], status} into buf; the execution backend (proot) applies the binds. */
VHDP_API vhdp_status_t vhdp_projection_plan_json(vhdp_context* context, char* buf, size_t cap,
                                                 size_t* needed);

/* ---- Config (builder; copied into a session at vhdp_session_create) ---- */
VHDP_API vhdp_status_t vhdp_config_create(vhdp_context* context, vhdp_config** out_config);
VHDP_API void vhdp_config_destroy(vhdp_config* config);
VHDP_API vhdp_status_t vhdp_config_set_rootfs(vhdp_config* config, const char* host_path);
VHDP_API vhdp_status_t vhdp_config_set_engine(vhdp_config* config, vhdp_engine_kind_t engine);
VHDP_API vhdp_status_t vhdp_config_set_cwd(vhdp_config* config, const char* guest_path);
/* argv[0] is the command (guest path or name searched in guest PATH). argc may be 0 (default
 * shell). */
VHDP_API vhdp_status_t vhdp_config_set_argv(vhdp_config* config, size_t argc,
                                            const char* const* argv);
/* "KEY=VALUE"; later entries override earlier ones and the default environment. */
VHDP_API vhdp_status_t vhdp_config_add_env(vhdp_config* config, const char* key_value);
VHDP_API vhdp_status_t vhdp_config_set_clear_env(vhdp_config* config, uint32_t clear);
VHDP_API vhdp_status_t vhdp_config_add_bind(vhdp_config* config, const char* host_path,
                                            const char* guest_path, vhdp_bind_flags_t flags);
VHDP_API vhdp_status_t vhdp_config_set_read_only_rootfs(vhdp_config* config, uint32_t read_only);
VHDP_API vhdp_status_t vhdp_config_set_network(vhdp_config* config, vhdp_network_t network);
/* Guest-visible identity only; never changes host credentials. */
VHDP_API vhdp_status_t vhdp_config_set_uid(vhdp_config* config, uint32_t uid);
VHDP_API vhdp_status_t vhdp_config_set_gid(vhdp_config* config, uint32_t gid);
/* 0 disables the timeout. */
VHDP_API vhdp_status_t vhdp_config_set_timeout_ms(vhdp_config* config, uint64_t timeout_ms);
VHDP_API vhdp_status_t vhdp_config_set_trace(vhdp_config* config, uint32_t trace);
/* fds are used only with VHDP_STDIO_FDS (duplicated at start); pass -1 otherwise. */
VHDP_API vhdp_status_t vhdp_config_set_stdio(vhdp_config* config, vhdp_stdio_mode_t mode,
                                             int32_t stdin_fd, int32_t stdout_fd,
                                             int32_t stderr_fd);
VHDP_API vhdp_status_t vhdp_config_set_pty_size(vhdp_config* config, uint16_t rows, uint16_t cols);
/* PTY mode only: user must stay valid until vhdp_session_destroy returns. */
VHDP_API vhdp_status_t vhdp_config_set_output_callback(vhdp_config* config, vhdp_output_fn fn,
                                                       void* user);
VHDP_API vhdp_status_t vhdp_config_set_seccomp(vhdp_config* config, vhdp_seccomp_mode_t mode);
VHDP_API vhdp_status_t vhdp_config_set_dev(vhdp_config* config, vhdp_dev_mode_t mode);
VHDP_API vhdp_status_t vhdp_config_set_proc(vhdp_config* config, vhdp_proc_mode_t mode);

/* ---- Session ---- */
/* Validates and deep-copies config; the config may be destroyed afterwards. */
VHDP_API vhdp_status_t vhdp_session_create(vhdp_context* context, const vhdp_config* config,
                                           vhdp_session** out_session);
VHDP_API vhdp_status_t vhdp_session_start(vhdp_session* session);
/*
 * timeout_ms: -1 waits forever, 0 polls. Returns VHDP_E_TIMEOUT if still running.
 * out may be NULL. When the session reached a terminal state, out is filled.
 */
VHDP_API vhdp_status_t vhdp_session_wait(vhdp_session* session, int64_t timeout_ms,
                                         vhdp_exit_info* out);
VHDP_API vhdp_status_t vhdp_session_state(vhdp_session* session, vhdp_session_state_t* out_state);
VHDP_API vhdp_status_t vhdp_session_engine(vhdp_session* session, vhdp_engine_kind_t* out_engine);
/* Sends signo to the guest's initial process. */
VHDP_API vhdp_status_t vhdp_session_signal(vhdp_session* session, int32_t signo);
/* PTY mode only. */
VHDP_API vhdp_status_t vhdp_session_resize(vhdp_session* session, uint16_t rows, uint16_t cols);
VHDP_API vhdp_status_t vhdp_session_write_input(vhdp_session* session, const uint8_t* data,
                                                size_t len, size_t* out_written);
/* PTY mode only: borrowed fd, valid until vhdp_session_destroy. Do not close it. */
VHDP_API vhdp_status_t vhdp_session_pty_master(vhdp_session* session, int32_t* out_fd);
/* Kills the whole guest process tree. Idempotent. */
VHDP_API vhdp_status_t vhdp_session_cancel(vhdp_session* session);
/* Cancels if running, joins internal threads, releases everything. NULL is OK. */
VHDP_API vhdp_status_t vhdp_session_destroy(vhdp_session* session);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VHDP_VHDP_H */
