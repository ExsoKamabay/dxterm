#!/bin/sh
# drac-Xterm `ollama` launcher  ,  installed at /usr/local/bin/ollama inside the Linux rootfs.
#
# It is a THIN WRAPPER, not a reimplementation: it exports the runtime configuration, makes sure a
# server exists when the requested sub-command needs one, then `exec`s the REAL, official Ollama
# binary. There is no simulated output anywhere in this file and no terminal-side interception:
# the guest shell resolves `ollama` through the ordinary PATH lookup that already exists
# (/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin), exactly like the `xset` command.
#
# Tokens (@PREFIX@ etc.) are substituted from OllamaConfig.kt when this asset is installed, so the
# pinned version lives in ONE place in the codebase.
#
# RECURSION: we exec an ABSOLUTE path to the real binary and never the name `ollama`, so
# launcher -> ollama -> launcher is structurally impossible. DRACX_OLLAMA_LAUNCHER is a second,
# explicit guard in case this file is ever copied somewhere unexpected.
set -u

PREFIX="@PREFIX@"
BIN="$PREFIX/bin/ollama"
ODIR="@OLLAMA_HOME@"
DEF_HOST="@HOST@"
DEF_MODELS="@MODELS@"
VERSION="@VERSION@"
ARTIFACT_MB="@ARTIFACT_MB@"
INSTALL_MB="@INSTALL_MB@"

REQ="$ODIR/install.request"
STATE="$ODIR/install.state"
ILOG="$ODIR/install.log"
SRVLOG="$ODIR/server.log"
PIDF="$ODIR/server.pid"
LOCK="$ODIR/server.lock"

if [ "${DRACX_OLLAMA_LAUNCHER:-}" = "1" ]; then
    echo "ollama: launcher recursion detected, aborting" >&2
    exit 70
fi
DRACX_OLLAMA_LAUNCHER=1
export DRACX_OLLAMA_LAUNCHER

mkdir -p "$ODIR" 2>/dev/null || true

# ---------------------------------------------------------------------------------------------
# 1. Is the runtime installed?  Lazy provisioning: nothing is downloaded until `ollama` is run.
# ---------------------------------------------------------------------------------------------
if [ ! -x "$BIN" ]; then
    echo "OLLAMA RUNTIME NOT INSTALLED" >&2
    echo >&2
    echo "  Pinned release : $VERSION (official, pre-release)" >&2
    echo "  Artifact       : ollama-linux-arm64.tar.zst (~${ARTIFACT_MB} MB download)" >&2
    echo "  Installed size : ~${INSTALL_MB} MB (NVIDIA CUDA runners are not installed)" >&2
    echo "  Install path   : $PREFIX" >&2
    echo "  Models path    : $DEF_MODELS" >&2
    echo >&2

    if [ ! -t 0 ]; then
        echo "Not an interactive terminal, refusing to start a ~${ARTIFACT_MB} MB download." >&2
        echo "Run 'ollama' from an interactive drac-Xterm session to install it." >&2
        exit 69
    fi

    printf 'Download and install it now? [y/N] ' >&2
    read -r ans
    case "$ans" in
        y|Y|yes|YES) ;;
        *) echo "Aborted. Nothing was downloaded." >&2; exit 69 ;;
    esac

    : > "$ILOG" 2>/dev/null || true
    if ! : > "$REQ" 2>/dev/null; then
        echo "ollama: cannot write the install request file ($REQ)" >&2
        exit 1
    fi

    echo "Requesting installation from drac-Xterm..." >&2
    last=""
    n=0
    while [ "$n" -lt 3600 ]; do
        [ -x "$BIN" ] && break
        cur=""
        [ -f "$STATE" ] && cur=$(cat "$STATE" 2>/dev/null)
        if [ "$cur" != "$last" ] && [ -n "$cur" ]; then
            printf '  %s\n' "$cur" >&2
            last="$cur"
            case "$cur" in
                FAILED*) echo >&2; echo "Installation failed. See $ILOG" >&2; exit 1 ;;
            esac
        fi
        sleep 1
        n=$((n + 1))
    done

    if [ ! -x "$BIN" ]; then
        echo "Timed out waiting for the Ollama runtime to install." >&2
        echo "See $ILOG for details." >&2
        exit 1
    fi
    echo "Ollama runtime ready." >&2
    echo >&2
fi

# ---------------------------------------------------------------------------------------------
# 2. Preflight: the shared libraries the official binaries actually declare (DT_NEEDED).
#    A missing one must produce an explicit, actionable message, never a bare "not found".
# ---------------------------------------------------------------------------------------------
missing=""
for lib in libstdc++.so.6 libgcc_s.so.1 libresolv.so.2; do
    found=0
    if command -v ldconfig >/dev/null 2>&1; then
        ldconfig -p 2>/dev/null | grep -q "[[:space:]]$lib[[:space:]]" && found=1
    fi
    if [ "$found" -eq 0 ]; then
        for d in /lib /usr/lib /lib/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu \
                 /usr/local/lib /usr/lib64; do
            [ -e "$d/$lib" ] && { found=1; break; }
        done
    fi
    [ "$found" -eq 0 ] && missing="$missing $lib"
done

if [ -n "$missing" ]; then
    echo "OLLAMA RUNTIME BLOCKED" >&2
    echo >&2
    echo "Reason:" >&2
    echo "  Required shared libraries are missing from this Linux rootfs." >&2
    echo >&2
    echo "Detected:" >&2
    for m in $missing; do echo "  missing: $m" >&2; done
    echo >&2
    echo "Required:" >&2
    echo "  The official ollama ARM64 build is a glibc binary" >&2
    echo "  (interpreter /lib/ld-linux-aarch64.so.1, GLIBC_2.28, GLIBCXX_3.4.22)." >&2
    echo >&2
    echo "Resolution:" >&2
    echo "  sudo apt update && sudo apt install -y libstdc++6 libgcc-s1" >&2
    echo "  then run 'ollama' again." >&2
    exit 1
fi

# ---------------------------------------------------------------------------------------------
# 3. Configuration. Only variables that envconfig/config.go in this release actually reads, and a
#    user-supplied value always wins. OLLAMA_TMPDIR is deliberately NOT set: it is not read by
#    this release. LD_LIBRARY_PATH is deliberately NOT set: llama-server carries RUNPATH=$ORIGIN
#    and ollama builds the library path for its own child processes.
# ---------------------------------------------------------------------------------------------
OLLAMA_HOST="${OLLAMA_HOST:-$DEF_HOST}"
OLLAMA_MODELS="${OLLAMA_MODELS:-$DEF_MODELS}"
export OLLAMA_HOST OLLAMA_MODELS
mkdir -p "$OLLAMA_MODELS" 2>/dev/null || true

# ---------------------------------------------------------------------------------------------
# 4. Server lifecycle.
#    In v0.32.14-rc0 a bare `ollama` runs runInteractiveTUI() (cmd/cmd.go), which talks to the
#    HTTP API, so a server IS required. These sub-commands are the only ones that are not:
#      serve | runner | gpu-discover | help | --help | -h | --version | -v
#    The PID file lives under /home/dracos/.ollama, which PRoot binds from a single host directory
#    shared by every workspace, so all workspaces see ONE server, and workspace 2 never spawns a
#    duplicate of workspace 1's. We only ever inspect the PID we recorded; nothing is killed by
#    name, so an unrelated process can never be signalled.
# ---------------------------------------------------------------------------------------------
needs_server() {
    case "${1:-__none__}" in
        serve|runner|gpu-discover|help|--help|-h|--version|-v) return 1 ;;
        *) return 0 ;;
    esac
}

port_open() {
    host_port=${OLLAMA_HOST#http://}
    host_port=${host_port#https://}
    h=${host_port%%:*}
    p=${host_port##*:}
    [ "$p" = "$host_port" ] && p=11434
    [ -n "$h" ] || h=127.0.0.1
    if command -v bash >/dev/null 2>&1; then
        bash -c "exec 3<>/dev/tcp/$h/$p" >/dev/null 2>&1 && return 0
        return 1
    fi
    # No bash: fall back to the recorded PID.
    [ -f "$PIDF" ] || return 1
    pid=$(cat "$PIDF" 2>/dev/null) || return 1
    [ -n "$pid" ] || return 1
    kill -0 "$pid" 2>/dev/null
}

start_server() {
    rm -f "$PIDF" 2>/dev/null || true
    if command -v setsid >/dev/null 2>&1; then
        setsid "$BIN" serve >>"$SRVLOG" 2>&1 &
    else
        "$BIN" serve >>"$SRVLOG" 2>&1 &
    fi
    echo $! > "$PIDF" 2>/dev/null || true
}

wait_up() {
    i=0
    while [ "$i" -lt "${1:-30}" ]; do
        port_open && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}

if needs_server "${1:-}"; then
    if ! port_open; then
        # Drop a lock older than 2 minutes: a previous attempt died before releasing it.
        if [ -d "$LOCK" ] && command -v find >/dev/null 2>&1; then
            find "$LOCK" -maxdepth 0 -mmin +2 -exec rmdir {} \; 2>/dev/null || true
        fi
        if mkdir "$LOCK" 2>/dev/null; then
            trap 'rmdir "$LOCK" 2>/dev/null || true' EXIT INT TERM HUP
            port_open || { echo "Starting the Ollama server..." >&2; start_server; }
            wait_up 30 || echo "ollama: server did not answer on $OLLAMA_HOST (see $SRVLOG)" >&2
            rmdir "$LOCK" 2>/dev/null || true
            trap - EXIT INT TERM HUP
        else
            # Another workspace is starting it right now, just wait for it.
            wait_up 30 || echo "ollama: server did not answer on $OLLAMA_HOST (see $SRVLOG)" >&2
        fi
    fi
fi

# ---------------------------------------------------------------------------------------------
# 5. Hand over to the REAL Ollama CLI. `exec` replaces this shell, so argv, stdio, the controlling
#    terminal and the exit code are the binary's own, the CLI behaves exactly as upstream.
# ---------------------------------------------------------------------------------------------
exec "$BIN" "$@"
