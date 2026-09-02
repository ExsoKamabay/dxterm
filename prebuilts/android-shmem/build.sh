#!/usr/bin/env bash
# Build libandroid-shmem.so, a System V shared-memory (shmget/shmat/shmdt/shmctl)
# shim over Android's ashmem, plus the fd-based entry points PRoot's sysvipc
# extension calls. Loaded as a real shared library: PRoot links against it.
#
# Source is termux/libandroid-shmem, not pelya/android-shmem, see manifest.env
# for why that distinction matters.
#
# Compiled directly rather than through upstream's Makefile: that Makefile
# hardcodes host `strip` and does not take a sysroot, and the whole recipe is
# one translation unit.
. "$(dirname "$0")/../lib/common.sh"

setup_toolchain
fetch_git "$SHMEM_REPO" "$SHMEM_COMMIT" android-shmem

SRC="$DL_DIR/android-shmem"
BUILD="$WORK_DIR/build/android-shmem"
rm -rf "$BUILD" && mkdir -p "$BUILD"

[ -f "$SRC/shmem.c" ] || die "shmem.c not found in $SRC"
[ -f "$SRC/exports.txt" ] || die "exports.txt not found in $SRC"

# Patch into the build directory, not the checkout: fetch_git treats the
# checkout as pristine and would fail to re-apply on the next run.
cp "$SRC/shmem.c" "$SRC/shm.h" "$SRC/exports.txt" "$BUILD/"
note "applying patches"
for p in "$PREBUILTS_DIR/android-shmem"/patches/*.patch; do
    [ -e "$p" ] || break
    printf '    %s\n' "$(basename "$p")"
    patch -s -p1 -d "$BUILD" < "$p" || die "patch failed: $(basename "$p")"
done

note "compiling shmem.c"
# Flags follow upstream's Makefile: -llog -landroid, and a version script so
# LD_PRELOAD interposes only the SysV shm entry points and nothing else.
# Added here, not upstream: 16 KB max-page-size, required by Android 15 devices
# whose page size is 16 KB rather than 4 KB.
"$CC" \
    -shared -fPIC -O2 \
    -std=c11 \
    -Wall -Wextra \
    $REPRO_CFLAGS \
    -I"$BUILD" \
    -o "$BUILD/libandroid-shmem.so" \
    "$BUILD/shmem.c" \
    -llog -landroid \
    -Wl,--version-script="$BUILD/exports.txt" \
    -Wl,-soname,libandroid-shmem.so \
    -Wl,-z,max-page-size=16384 \
    -Wl,--build-id=none

assert_arm64 "$BUILD/libandroid-shmem.so"

note "exported symbols"
syms=$("$TOOLCHAIN/bin/llvm-nm" -D --defined-only "$BUILD/libandroid-shmem.so")
missing=""
# The first four are what LD_PRELOAD interposes; the fd pair is what PRoot's
# sysvipc extension links against, and its absence is the exact failure that
# building from pelya's upstream produces.
for want in shmget shmat shmdt shmctl libandroid_shmat_fd libandroid_shmdt_fd; do
    contains "$syms" " $want" || missing="$missing $want"
done
[ -z "$missing" ] || die "missing exports:$missing"
printf '%s\n' "$syms" | awk '{print "    " $NF}'

# The header PRoot compiles against. bionic has no <sys/shm.h>, so this is the
# only declaration of shmget/shmctl available to it.
install -D -m 644 "$BUILD/shm.h" "$OUT_DIR/include/sys/shm.h"
printf '    staged sys/shm.h for the proot build\n'

install_artifact "$BUILD/libandroid-shmem.so" libandroid-shmem.so
