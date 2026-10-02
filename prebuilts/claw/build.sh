#!/usr/bin/env bash
# Build the in-guest `claw` command from the sources in app/src/main/assets/claw.
#
#   claw   copied into the guest as /opt/claw/bin/claw by the native claw setup
#          (app/src/main/cpp/claw/ClawSetup.cpp), with /usr/local/bin/claw in front of it
#
# Properties that are load-bearing, and checked below:
#
#   * STATIC, against musl. claw runs inside the rootfs, not on Android: it must resolve
#     names through the guest's /etc/resolv.conf (RootfsConfigurator writes it) and read the
#     guest's CA store. musl does both with no NSS modules to dlopen and no bionic to load.
#   * OpenSSL linked in statically, so nothing in the guest has to match its version.
#   * 16 KB max-page-size, same rule as every other shipped executable.
#
# The toolchain is Zig's bundled clang + musl, pinned in manifest.env. The NDK is still set
# up, only for llvm-strip.
. "$(dirname "$0")/../lib/common.sh"

setup_toolchain

case "$ANDROID_ABI" in
    arm64-v8a) ZIG_TARGET="aarch64-linux-musl"; OSSL_TARGET="linux-aarch64" ;;
    x86_64)    ZIG_TARGET="x86_64-linux-musl";  OSSL_TARGET="linux-x86_64" ;;
    *)         die "claw: unsupported ABI $ANDROID_ABI" ;;
esac

SRC="$REPO_ROOT/app/src/main/assets/claw"
BUILD="$BUILD_ROOT/build/claw"
ASSET_DIR="$SRC/bin/$ANDROID_ABI"
[ -f "$SRC/CMakeLists.txt" ] || die "claw sources not found at $SRC"
command -v cmake >/dev/null || die "cmake is required to build claw"

# --- Zig ---------------------------------------------------------------------
fetch_tarball "$ZIG_URL" "$ZIG_SHA256" "zig-x86_64-linux-$ZIG_VERSION.tar.xz"
ZIG_DIR="$WORK_DIR/tools/zig-x86_64-linux-$ZIG_VERSION"
if [ ! -x "$ZIG_DIR/zig" ]; then
    mkdir -p "$WORK_DIR/tools"
    tar -xJf "$DL_DIR/zig-x86_64-linux-$ZIG_VERSION.tar.xz" -C "$WORK_DIR/tools"
fi
ZIG="$ZIG_DIR/zig"
"$ZIG" version | grep -qx "$ZIG_VERSION" || die "zig at $ZIG is not $ZIG_VERSION"
note "zig      $ZIG_VERSION ($ZIG_TARGET)"

# Wrapper scripts rather than "zig cc -target ..." in CC: CMake and OpenSSL's Configure both
# want CC to be a single executable path.
rm -rf "$BUILD"
mkdir -p "$BUILD/bin"
export ZIG_GLOBAL_CACHE_DIR="$WORK_DIR/tools/zig-cache"
export ZIG_LOCAL_CACHE_DIR="$BUILD/zig-cache"
for tool in cc c++; do
    printf '#!/bin/sh\nexec "%s" %s -target %s "$@"\n' "$ZIG" "$tool" "$ZIG_TARGET" > "$BUILD/bin/zig-$tool"
done
for tool in ar ranlib; do
    printf '#!/bin/sh\nexec "%s" %s "$@"\n' "$ZIG" "$tool" > "$BUILD/bin/zig-$tool"
done
chmod +x "$BUILD"/bin/zig-*

# --- OpenSSL -----------------------------------------------------------------
fetch_tarball "$OPENSSL_URL" "$OPENSSL_SHA256" "openssl-$OPENSSL_VERSION.tar.gz"
tar -xzf "$DL_DIR/openssl-$OPENSSL_VERSION.tar.gz" -C "$BUILD"
OSSL_SRC="$BUILD/openssl-$OPENSSL_VERSION"
OSSL_PREFIX="$BUILD/openssl"

# OPENSSLDIR is where the library looks for cert.pem and certs/ when nothing says otherwise.
# /usr/lib/ssl is the Debian/Kali layout (both are links into /etc/ssl/certs). The guest
# launcher also exports SSL_CERT_FILE, so a rootfs with another layout still verifies.
note "configuring OpenSSL $OPENSSL_VERSION ($OSSL_TARGET)"
( cd "$OSSL_SRC" && \
  CC="$BUILD/bin/zig-cc" AR="$BUILD/bin/zig-ar" RANLIB="$BUILD/bin/zig-ranlib" \
  ./Configure "$OSSL_TARGET" \
      --prefix="$OSSL_PREFIX" --libdir=lib --openssldir=/usr/lib/ssl \
      no-shared no-module no-dso no-engine no-async no-tests no-docs no-apps \
      no-ui-console no-afalgeng no-comp \
      -ffile-prefix-map="$BUILD"=/build \
  ) > "$BUILD_ROOT/claw-openssl-configure.log" 2>&1 \
    || { tail -20 "$BUILD_ROOT/claw-openssl-configure.log" >&2
         die "OpenSSL configure failed; full log at $BUILD_ROOT/claw-openssl-configure.log"; }

note "compiling OpenSSL"
make -C "$OSSL_SRC" -j"$(nproc)" build_libs > "$BUILD_ROOT/claw-openssl-build.log" 2>&1 \
    || { grep -E 'error:' "$BUILD_ROOT/claw-openssl-build.log" | sort -u | head -20 >&2
         die "OpenSSL build failed; full log at $BUILD_ROOT/claw-openssl-build.log"; }
make -C "$OSSL_SRC" install_dev > "$BUILD_ROOT/claw-openssl-install.log" 2>&1 \
    || die "OpenSSL install failed; full log at $BUILD_ROOT/claw-openssl-install.log"

# --- claw --------------------------------------------------------------------
GENERATOR="Unix Makefiles"
command -v ninja >/dev/null && GENERATOR="Ninja"

note "configuring claw ($GENERATOR, $ZIG_TARGET)"
cmake -S "$SRC" -B "$BUILD/claw" -G "$GENERATOR" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_C_COMPILER="$BUILD/bin/zig-cc" \
    -DCMAKE_CXX_COMPILER="$BUILD/bin/zig-c++" \
    -DCMAKE_AR="$BUILD/bin/zig-ar" \
    -DCMAKE_RANLIB="$BUILD/bin/zig-ranlib" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCLAW_BUILD_TESTS=ON \
    -DCLAW_STATIC_OPENSSL=ON \
    -DOPENSSL_ROOT_DIR="$OSSL_PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$OSSL_PREFIX" \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$SRC=/claw -ffile-prefix-map=$BUILD=/build" \
    -DCMAKE_EXE_LINKER_FLAGS="-static -Wl,-z,max-page-size=16384 -Wl,--build-id=none" \
    > "$BUILD_ROOT/claw-configure.log" 2>&1 \
    || { tail -20 "$BUILD_ROOT/claw-configure.log" >&2
         die "claw configure failed; full log at $BUILD_ROOT/claw-configure.log"; }

note "compiling claw"
cmake --build "$BUILD/claw" -j"$(nproc)" > "$BUILD_ROOT/claw-build.log" 2>&1 \
    || { grep -E 'error:|FAILED' "$BUILD_ROOT/claw-build.log" | sort -u | head -20 >&2
         die "claw build failed; full log at $BUILD_ROOT/claw-build.log"; }

BIN="$BUILD/claw/claw"
[ -f "$BIN" ] || die "claw was not produced at $BIN"

note "sanity checks"
assert_arch "$BIN"
if "$READELF" -dW "$BIN" 2>/dev/null | grep -q "NEEDED"; then
    die "$BIN is dynamically linked; it must not depend on the guest's libraries"
fi
"$READELF" -lW "$BIN" 2>/dev/null | grep -q "INTERP" && die "$BIN has a PT_INTERP; it must be fully static"
printf '    statically linked, no NEEDED entries\n'
min_align=$("$READELF" -lW "$BIN" | awk '$1=="LOAD"{print $NF}' | sort -u | head -1)
[ "$min_align" = "0x4000" ] || die "$BIN PT_LOAD alignment is $min_align, expected 0x4000 (16 KB)"
printf '    PT_LOAD alignment %s\n' "$min_align"

# The unit tests can only run where the build host can execute the target.
if [ "$ANDROID_ABI" = "x86_64" ] && [ "$(uname -m)" = "x86_64" ]; then
    note "running claw_tests"
    "$BUILD/claw/claw_tests" > "$BUILD_ROOT/claw-tests.log" 2>&1 \
        || { tail -20 "$BUILD_ROOT/claw-tests.log" >&2; die "claw_tests failed"; }
    tail -1 "$BUILD_ROOT/claw-tests.log"
fi

install_artifact "$BIN" claw

if [ "${PREBUILTS_INSTALL:-0}" = "1" ]; then
    mkdir -p "$ASSET_DIR"
    cp "$OUT_DIR/claw" "$ASSET_DIR/claw"
    note "installed into $ASSET_DIR/claw"
fi
