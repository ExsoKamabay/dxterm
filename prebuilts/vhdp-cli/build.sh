#!/usr/bin/env bash
# Build the VHDP executables drac-Xterm ships, from the VHDP sources in this repository:
#
#   vhdp               the phdp CLI, copied into the guest as /usr/local/bin/vhdp
#   libphdp.so         the same CLI, run on the Android side as the terminal's guest backend
#   libvhdp-loader.so  the userland ELF loader the rootless engine starts guest programs with
#
# The source is app/src/main/assets/vhdp (identical to app/src/main/cpp/vhdp apart from the
# JNI bridge), so manifest.env pins nothing for it.
#
# Properties that are load-bearing, and checked below:
#
#   * STATIC phdp. Inside the guest it runs on a glibc Debian with no bionic to load it from;
#     on the Android side a static binary has no library search path to get wrong.
#   * The loader is a static PIE with NO dynamic relocations and no PT_INTERP: it is the first
#     code of a fresh process and nothing would relocate it.
#   * 16 KB max-page-size for every file: Android 15 devices with 16 KB pages refuse to exec a
#     4 KB-aligned image.
#
# vhdp goes into assets/ (the app copies it into the guest at launch). libphdp.so and
# libvhdp-loader.so go into jniLibs/<abi>/ through the parent script's *.so install loop:
# nativeLibraryDir is the only directory an app may execve() from, and the loader has to be
# executable because every guest program starts as an exec of it.
. "$(dirname "$0")/../lib/common.sh"

setup_toolchain

SRC="$REPO_ROOT/app/src/main/assets/vhdp"
BUILD="$BUILD_ROOT/build/vhdp-cli"
ASSET_DIR="$SRC/bin/$ANDROID_ABI"

[ -f "$SRC/CMakeLists.txt" ] || die "VHDP sources not found at $SRC"
command -v cmake >/dev/null || die "cmake is required to build the vhdp CLI"

GENERATOR="Unix Makefiles"
command -v ninja >/dev/null && GENERATOR="Ninja"

rm -rf "$BUILD"
note "configuring ($GENERATOR, $ANDROID_ABI, API $ANDROID_API)"
cmake -S "$SRC" -B "$BUILD" -G "$GENERATOR" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK_ROOT/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ANDROID_ABI" \
    -DANDROID_PLATFORM="android-$ANDROID_API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DVHDP_BUILD_CLI=ON \
    -DVHDP_BUILD_TESTS=OFF \
    -DVHDP_BUILD_EXAMPLES=OFF \
    -DCMAKE_C_FLAGS="$REPRO_CFLAGS" \
    -DCMAKE_CXX_FLAGS="$REPRO_CFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="-static -Wl,-z,max-page-size=16384 -Wl,--build-id=none" \
    > "$BUILD_ROOT/vhdp-cli-configure.log" 2>&1 \
    || { tail -20 "$BUILD_ROOT/vhdp-cli-configure.log" >&2
         die "cmake configure failed; full log at $BUILD_ROOT/vhdp-cli-configure.log"; }

note "compiling"
cmake --build "$BUILD" --target phdp vhdp_loader -j"$(nproc)" \
    > "$BUILD_ROOT/vhdp-cli-build.log" 2>&1 \
    || { grep -E 'error:|FAILED' "$BUILD_ROOT/vhdp-cli-build.log" | sort -u | head -20 >&2
         die "vhdp CLI build failed; full log at $BUILD_ROOT/vhdp-cli-build.log"; }

BIN="$BUILD/src/phdp"
[ -f "$BIN" ] || die "phdp was not produced at $BIN"

note "sanity checks"
assert_arch "$BIN"

# Static, or it cannot run in the guest at all. An ELF with no PT_INTERP and no
# NEEDED entries is what we want; anything else would load bionic from Android.
if "$READELF" -dW "$BIN" 2>/dev/null | grep -q "NEEDED"; then
    die "$BIN is dynamically linked; the guest has no bionic to load it with"
fi
"$READELF" -lW "$BIN" 2>/dev/null | grep -q "INTERP" && \
    die "$BIN has a PT_INTERP; it must be fully static"
printf '    statically linked, no NEEDED entries\n'

# 16 KB pages: same gate the Gradle build applies to jniLibs.
min_align=$("$READELF" -lW "$BIN" | awk '$1=="LOAD"{print $NF}' | sort -u | head -1)
[ "$min_align" = "0x4000" ] || die "$BIN PT_LOAD alignment is $min_align, expected 0x4000 (16 KB)"
printf '    PT_LOAD alignment %s\n' "$min_align"

LOADER="$BUILD/src/vhdp-loader"
[ -f "$LOADER" ] || die "vhdp-loader was not produced at $LOADER"
assert_arch "$LOADER"
"$READELF" -lW "$LOADER" | grep -q "INTERP" && die "$LOADER has a PT_INTERP; it must not"
"$READELF" -hW "$LOADER" | grep -q "DYN" || die "$LOADER is not a PIE (ET_DYN)"
if "$READELF" -rW "$LOADER" 2>/dev/null | grep -qE "^[0-9a-f]{8,}"; then
    die "$LOADER has dynamic relocations; nothing relocates the loader"
fi
min_align=$("$READELF" -lW "$LOADER" | awk '$1=="LOAD"{print $NF}' | sort -u | head -1)
[ "$min_align" = "0x4000" ] || die "$LOADER PT_LOAD alignment is $min_align, expected 0x4000 (16 KB)"
printf '    loader: static PIE, no PT_INTERP, no relocations, PT_LOAD alignment %s\n' "$min_align"

mkdir -p "$OUT_DIR"
cp "$BIN" "$OUT_DIR/vhdp"
cp "$BIN" "$OUT_DIR/libphdp.so"
cp "$LOADER" "$OUT_DIR/libvhdp-loader.so"
for f in vhdp libphdp.so libvhdp-loader.so; do
    "$STRIP" --strip-unneeded "$OUT_DIR/$f" 2>/dev/null || warn "could not strip $f"
    printf '%s  %s  (%s bytes)\n' \
        "$(sha256sum "$OUT_DIR/$f" | cut -d' ' -f1)" "$f" "$(stat -c%s "$OUT_DIR/$f")"
done

# Installed here rather than by the parent script: the destination is the assets
# tree, not jniLibs, so it is outside what the driver's *.so loop handles.
if [ "${PREBUILTS_INSTALL:-0}" = "1" ]; then
    mkdir -p "$ASSET_DIR"
    cp "$OUT_DIR/vhdp" "$ASSET_DIR/vhdp"
    note "installed into $ASSET_DIR/vhdp"
fi
