#!/usr/bin/env bash
# Build every prebuilt binary drac-Xterm ships, from pinned upstream sources.
#
#   ./prebuilts/build.sh                 build all, in dependency order
#   ./prebuilts/build.sh talloc proot    build only these
#   ./prebuilts/build.sh --install       build all, then copy into jniLibs
#
# ANDROID_ABI selects the architecture (default arm64-v8a, see manifest.env);
# the APK is universal, so a full refresh is both of:
#   ./prebuilts/build.sh --install
#   ANDROID_ABI=x86_64 TRIPLE=x86_64-linux-android ./prebuilts/build.sh --install
#
# Results land in prebuilts/work/$ANDROID_ABI/out/. Nothing touches
# app/src/main/jniLibs unless --install is passed: replacing the shipped
# binaries changes what the APK contains, and that should be a decision, not a
# side effect of running a build script.
#
# Set PREBUILTS_WORK to build somewhere other than prebuilts/work.
set -euo pipefail

PREBUILTS_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$PREBUILTS_DIR/.." && pwd)"
WORK_DIR="${PREBUILTS_WORK:-$PREBUILTS_DIR/work}"

# For ANDROID_ABI, which decides both where the recipes build and which jniLibs
# directory --install writes to. The app ships a universal APK (arm64-v8a and
# x86_64), so "install" has to mean "install for the ABI just built" -- a fixed
# arm64 path would quietly overwrite the x86_64 binaries with arm64 ones.
# shellcheck source=manifest.env
. "$PREBUILTS_DIR/manifest.env"

OUT_DIR="$WORK_DIR/$ANDROID_ABI/out"
JNI_DIR="$REPO_ROOT/app/src/main/jniLibs/$ANDROID_ABI"

# Dependency order, not alphabetical:
#   proot links against both talloc and android-shmem, and compiles against
#   android-shmem's sys/shm.h. Building it first fails with a clear message,
#   but there is no reason to make that happen.
# vhdp-cli is last and independent: it builds from sources already in this
# repository. Its *.so outputs (libphdp.so, libvhdp-loader.so) go into jniLibs
# through the install loop below like every other prebuilt, while the in-guest
# `vhdp` binary is installed by its own recipe into assets/, outside that loop.
ORDER="android-shmem talloc busybox proot vhdp-cli"

INSTALL=0
TARGETS=""
for arg in "$@"; do
    case "$arg" in
        --install) INSTALL=1 ;;
        -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) printf 'unknown option: %s\n' "$arg" >&2; exit 2 ;;
        *)
            case " $ORDER " in
                *" $arg "*) TARGETS="$TARGETS $arg" ;;
                *) printf 'unknown component: %s (known: %s)\n' "$arg" "$ORDER" >&2; exit 2 ;;
            esac
            ;;
    esac
done

# Preserve dependency order even when the caller lists components out of order.
if [ -n "$TARGETS" ]; then
    selected=""
    for c in $ORDER; do
        case " $TARGETS " in *" $c "*) selected="$selected $c" ;; esac
    done
    TARGETS="$selected"
else
    TARGETS="$ORDER"
fi

printf '\033[1mBuilding:\033[0m%s\n' "$TARGETS"
printf 'work dir: %s\n\n' "$WORK_DIR"

# Recipes whose artifact does not belong in jniLibs install themselves, and read
# this to know whether the caller asked for it.
export PREBUILTS_INSTALL="$INSTALL"

for c in $TARGETS; do
    printf '\033[1m===== %s =====\033[0m\n' "$c"
    PREBUILTS_WORK="$WORK_DIR" "$PREBUILTS_DIR/$c/build.sh"
    printf '\n'
done

printf '\033[1m===== results =====\033[0m\n'
if [ -d "$OUT_DIR" ]; then
    for f in "$OUT_DIR"/*.so; do
        [ -e "$f" ] || continue
        name=$(basename "$f")
        new_sha=$(sha256sum "$f" | cut -d' ' -f1)
        if [ -f "$JNI_DIR/$name" ]; then
            old_sha=$(sha256sum "$JNI_DIR/$name" | cut -d' ' -f1)
            old_size=$(stat -c%s "$JNI_DIR/$name")
        else
            old_sha="(not currently shipped)"
            old_size="-"
        fi
        printf '%s\n  built   %s  %s bytes\n  shipped %s  %s bytes\n' \
            "$name" "$new_sha" "$(stat -c%s "$f")" "$old_sha" "$old_size"
    done
fi

if [ "$INSTALL" -eq 1 ]; then
    printf '\n\033[1m===== installing into jniLibs =====\033[0m\n'
    [ -d "$JNI_DIR" ] || { printf 'error: %s does not exist\n' "$JNI_DIR" >&2; exit 1; }
    for f in "$OUT_DIR"/*.so; do
        [ -e "$f" ] || continue
        cp "$f" "$JNI_DIR/"
        printf '  %s\n' "$(basename "$f")"
    done
    cat <<'EOF'

Installed. Before committing these, understand what changed:

  * NOTICE records the upstream version of each shipped file. Update it, or the
    documented inventory no longer matches the APK.
  * The binaries are no longer Termux builds, so the /data/data/com.termux/...
    workarounds in Bootstrap are no longer load-bearing. They are harmless, but
    they now describe something untrue.
  * NOTHING here has been run on a device. A build that links is not a build
    that works: exercise the BusyBox shell, then a full rootfs launch on the
    VHDP backend AND on the PRoot fallback (GuestBackend picks one; force the
    other by writing files/.guest-backend), on real arm64 hardware before
    shipping.
EOF
else
    printf '\nNothing was installed. Re-run with --install to copy these into\n%s/.\n' "$JNI_DIR"
fi
