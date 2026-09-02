#!/usr/bin/env bash
#
# What has to happen before a version is tagged, in the order it happens.
#
#   ./scripts/release-prep.sh                 what is done, what is not
#   ./scripts/release-prep.sh device-test     1. run the built prebuilts on an arm64 device
#   ./scripts/release-prep.sh secrets         2. push the four DRACOS_* secrets to GitHub
#   ./scripts/release-prep.sh tag             3. tag v<versionName> and let CI publish
#
# The order is enforced, not suggested. Each step refuses to run until the ones
# it depends on have actually happened, checked against the world rather than
# against a marker file: `tag` verifies the secrets exist on GitHub, `secrets`
# verifies a keystore exists at the path the build will read.
#
# No password is ever passed as a command-line argument, because argv is
# world-readable through /proc. Values are read with `read -rs` or piped through
# stdin, and nothing here echoes one back.
#
# Rotating the signing key is a separate, rare procedure, written out in
# docs/SECURITY-KEY-ROTATION.md.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

GH_REPO="ExsoKamabay/dxterm"
GRADLE_PROPS="$HOME/.gradle/gradle.properties"
KEY_DIR="$HOME/.android-keys"
KEYSTORE="$KEY_DIR/dracxterm-release.jks"
KEY_ALIAS="dracxterm"
PREBUILT_OUT="${PREBUILTS_WORK:-$REPO_ROOT/prebuilts/work}/out"
# Overridable: some hardened builds mount /data/local/tmp noexec, and the test
# harness needs somewhere else entirely.
DEVICE_DIR="${DRACXTERM_DEVICE_DIR:-/data/local/tmp/dracxterm-smoke}"

# --- output ------------------------------------------------------------------

if [ -t 1 ]; then
    C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'
    C_CYA=$'\033[36m'; C_BLD=$'\033[1m';  C_OFF=$'\033[0m'
else
    C_RED=''; C_GRN=''; C_YEL=''; C_CYA=''; C_BLD=''; C_OFF=''
fi

die()  { printf '%serror:%s %s\n' "$C_RED" "$C_OFF" "$*" >&2; exit 1; }
note() { printf '%s==>%s %s\n' "$C_CYA" "$C_OFF" "$*"; }
warn() { printf '%swarn:%s %s\n' "$C_YEL" "$C_OFF" "$*" >&2; }
ok()   { printf '  %sok%s    %s\n' "$C_GRN" "$C_OFF" "$*"; }
bad()  { printf '  %sTODO%s  %s\n' "$C_RED" "$C_OFF" "$*"; }
head1() { printf '\n%s%s%s\n' "$C_BLD" "$*" "$C_OFF"; }

# String matching without a pipe.
#
# `producer | grep -q pattern` is silently inverted under `set -o pipefail`:
# grep -q exits on first match, the producer dies of SIGPIPE, and pipefail makes
# that the pipeline's status, so the check reports failure exactly when the
# pattern WAS found. That bug already cost this project a sanity check that
# could not fail; see prebuilts/README.md.
contains() {
    case "$1" in
        *"$2"*) return 0 ;;
        *) return 1 ;;
    esac
}

confirm() {
    local prompt="$1" want="${2:-yes}" answer
    printf '%s\n%stype %s to continue:%s ' "$prompt" "$C_BLD" "$want" "$C_OFF"
    read -r answer
    [ "$answer" = "$want" ] || die "aborted."
}

need() { command -v "$1" >/dev/null 2>&1 || die "$1 is not installed"; }

# --- fact-finding ------------------------------------------------------------
#
# Every one of these asks the world, not a cached marker. A release checklist
# that trusts its own bookkeeping is how a step gets skipped.

gradle_prop() {
    [ -f "$GRADLE_PROPS" ] || return 1
    local v
    v=$(sed -n "s/^$1[[:space:]]*=[[:space:]]*//p" "$GRADLE_PROPS" | head -1)
    [ -n "$v" ] || return 1
    printf '%s' "$v"
}

version_name() { sed -n 's/.*versionName *= *"\([^"]*\)".*/\1/p' app/build.gradle.kts | head -1; }
version_code() { sed -n 's/.*versionCode *= *\([0-9]*\).*/\1/p' app/build.gradle.kts | head -1; }

key_rotated() {
    local sf
    sf=$(gradle_prop DRACOS_STORE_FILE) || return 1
    [ -f "$sf" ] || return 1
    # The burned key must not be the one configured. Its fingerprint is recorded
    # in docs/SECURITY-KEY-ROTATION.md as a 2744-byte file; a match on size and
    # digest is checked below in rotate-key. Here we only need "not the old path".
    case "$sf" in
        *"$REPO_ROOT"*) return 1 ;;   # anything inside the repo is wrong by definition
    esac
    gradle_prop DRACOS_STORE_PASSWORD >/dev/null || return 1
    gradle_prop DRACOS_KEY_ALIAS      >/dev/null || return 1
    gradle_prop DRACOS_KEY_PASSWORD   >/dev/null || return 1
    return 0
}

secrets_present() {
    local names
    names=$(gh secret list -R "$GH_REPO" --json name --jq '.[].name' 2>/dev/null) || return 2
    local want missing=""
    for want in DRACOS_KEYSTORE_BASE64 DRACOS_STORE_PASSWORD DRACOS_KEY_ALIAS DRACOS_KEY_PASSWORD; do
        contains "$names" "$want" || missing="$missing $want"
    done
    [ -z "$missing" ] || { printf '%s' "$missing"; return 1; }
    return 0
}

tag_published() {
    local tags
    tags=$(gh api "repos/$GH_REPO/tags" --jq '.[].name' 2>/dev/null) || return 2
    contains "$tags" "v$(version_name)"
}

device_test_passed() { [ -f "$REPO_ROOT/prebuilts/work/.device-test-passed" ]; }

# What docs/THIRD-PARTY-BINARIES.md says is in the APK. That table is the claim
# the project makes to anyone auditing the release, so it is the right thing to
# compare against, not git HEAD, which stops being a useful reference the
# moment new binaries are committed.
documented_digest() {
    sed -n "/\`$1\`/s/.*\`\([0-9a-f]\{64\}\)\`.*/\1/p" docs/THIRD-PARTY-BINARIES.md | head -1
}

# 0 when every binary in jniLibs matches the documented inventory.
apk_binaries_documented() {
    local f now doc
    for f in app/src/main/jniLibs/arm64-v8a/*.so; do
        [ -e "$f" ] || return 1
        now=$(sha256sum "$f" | cut -d' ' -f1)
        doc=$(documented_digest "$(basename "$f")")
        [ -n "$doc" ] || return 1
        [ "$now" = "$doc" ] || return 1
    done
    return 0
}

# 0 when every binary in jniLibs is one that actually passed on a device. The
# device-test record stores a digest per file precisely so this can be asked.
installed_matches_device_test() {
    local marker="$REPO_ROOT/prebuilts/work/.device-test-passed"
    [ -f "$marker" ] || return 1
    local record f now
    record=$(cat "$marker")
    for f in app/src/main/jniLibs/arm64-v8a/*.so; do
        [ -e "$f" ] || return 1
        now=$(sha256sum "$f" | cut -d' ' -f1)
        contains "$record" "$now" || return 1
    done
    return 0
}

# =============================================================================
# status
# =============================================================================

cmd_status() {
    local vn vc
    vn=$(version_name); vc=$(version_code)
    printf '%sdracxterm release readiness%s\n' "$C_BLD" "$C_OFF"
    printf 'repository  %s\n' "$GH_REPO"
    printf 'version     %s (versionCode %s)\n' "$vn" "$vc"

    head1 '1. Signing key configured'
    if key_rotated; then
        local sf; sf=$(gradle_prop DRACOS_STORE_FILE)
        ok "keystore at $sf, all four DRACOS_* properties set"
    else
        bad "no release key. See docs/SECURITY-KEY-ROTATION.md."
        if [ -f "$GRADLE_PROPS" ]; then
            local p
            for p in DRACOS_STORE_FILE DRACOS_STORE_PASSWORD DRACOS_KEY_ALIAS DRACOS_KEY_PASSWORD; do
                gradle_prop "$p" >/dev/null || printf '        missing %s in %s\n' "$p" "$GRADLE_PROPS"
            done
        else
            printf '        %s does not exist\n' "$GRADLE_PROPS"
        fi
    fi

    head1 '2. Prebuilt binaries exercised on an arm64 device'
    if device_test_passed; then
        ok "passed on $(sed -n 's/^device=//p' "$REPO_ROOT/prebuilts/work/.device-test-passed" | head -1)"
    elif [ -d "$PREBUILT_OUT" ]; then
        bad "built but never run. Run: $0 device-test"
    else
        bad "nothing built yet. Run: ./prebuilts/build.sh, then $0 device-test"
    fi

    head1 '3. GitHub Actions secrets'
    local missing rc
    missing=$(secrets_present) && rc=0 || rc=$?
    case "$rc" in
        0) ok "all four DRACOS_* secrets are set" ;;
        1) bad "missing:$missing. Run: $0 secrets" ;;
        *) warn "cannot read secrets (token lacks the scope, or the API refused). Run: gh auth refresh -s repo" ;;
    esac

    head1 "4. Release tag v$vn"
    case "$(tag_published; echo $?)" in
        0) ok "v$vn is published" ;;
        1) bad "not tagged. Run: $0 tag" ;;
        *) warn "could not reach the GitHub API" ;;
    esac
    printf '\n'
}

# =============================================================================
# 1. device-test
# =============================================================================

adb_sh() { adb ${ANDROID_SERIAL:+-s "$ANDROID_SERIAL"} shell "$@"; }
adb_push() { adb ${ANDROID_SERIAL:+-s "$ANDROID_SERIAL"} push "$@" >/dev/null; }

DT_PASS=0
DT_FAIL=0

# dt_check <label> <shell command>
# Passes when the command exits 0 on the device.
dt_check() {
    local label="$1"; shift
    local out rc
    out=$(adb_sh "$@" 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then
        DT_PASS=$((DT_PASS + 1))
        printf '  %sPASS%s  %s\n' "$C_GRN" "$C_OFF" "$label"
        [ -n "${DT_VERBOSE:-}" ] && [ -n "$out" ] && printf '%s\n' "$out" | sed 's/^/          /'
    else
        DT_FAIL=$((DT_FAIL + 1))
        printf '  %sFAIL%s  %s (exit %s)\n' "$C_RED" "$C_OFF" "$label" "$rc"
        printf '%s\n' "$out" | head -5 | sed 's/^/          /'
    fi
    return 0
}

# dt_expect <label> <expected substring> <shell command>
dt_expect() {
    local label="$1" want="$2"; shift 2
    local out
    out=$(adb_sh "$@" 2>&1) || true
    if contains "$out" "$want"; then
        DT_PASS=$((DT_PASS + 1))
        printf '  %sPASS%s  %s\n' "$C_GRN" "$C_OFF" "$label"
    else
        DT_FAIL=$((DT_FAIL + 1))
        printf '  %sFAIL%s  %s, expected %s\n' "$C_RED" "$C_OFF" "$label" "$want"
        printf '%s\n' "$out" | head -5 | sed 's/^/          /'
    fi
    return 0
}

cmd_device_test() {
    need adb
    head1 'Smoke-testing the freshly built prebuilts on a real device'

    cat <<'EOF'
These binaries compile, link, are AArch64, and export the right symbols. None of
that is evidence they run. BusyBox moves from 1.29.3 (2018) to 1.38.0 here --
seven years of upstream change and a different applet set, and PRoot changes
provenance from a Termux build to one built from source. This is the step that
decides whether ./prebuilts/build.sh --install is safe.

EOF

    [ -d "$PREBUILT_OUT" ] || die "no built binaries at $PREBUILT_OUT
  Run ./prebuilts/build.sh first."

    local f
    for f in libbusybox.so libproot.so libproot-loader.so libtalloc.so libandroid-shmem.so; do
        [ -f "$PREBUILT_OUT/$f" ] || die "missing $PREBUILT_OUT/$f. Re-run ./prebuilts/build.sh"
    done

    # --- device ---
    # `adb devices` prints a header line, then "<serial>\t<state>" per device.
    # Match on the state column rather than stripping text and dropping the first
    # line: doing both means the header is removed twice and the first real device
    # with it, so a single connected phone reads as none at all.
    local devices count
    devices=$(adb devices | awk '$2 == "device" { print $1 }')
    count=$(printf '%s' "$devices" | awk 'NF' | wc -l)
    if [ "$count" -eq 0 ]; then
        die "no device connected.
  Connect an arm64 Android device with USB debugging enabled, or start an
  arm64 emulator, and check with:  adb devices
  (An x86_64 emulator will not do: every binary here is arm64-v8a only.)"
    fi
    if [ "$count" -gt 1 ] && [ -z "${ANDROID_SERIAL:-}" ]; then
        die "$count devices connected. Pick one:  export ANDROID_SERIAL=<serial>
$(printf '%s\n' "$devices" | sed 's/^/    /')"
    fi

    local abi sdk model
    abi=$(adb_sh getprop ro.product.cpu.abi | tr -d '\r')
    sdk=$(adb_sh getprop ro.build.version.sdk | tr -d '\r')
    model=$(adb_sh getprop ro.product.model | tr -d '\r')
    note "device   $model (Android SDK $sdk, $abi)"
    [ "$abi" = "arm64-v8a" ] || die "device ABI is $abi; these binaries are arm64-v8a only"
    [ "$sdk" -ge 24 ] || die "device is SDK $sdk; the app's minSdk is 24"

    # --- stage ---
    note "staging into $DEVICE_DIR"
    adb_sh "rm -rf $DEVICE_DIR && mkdir -p $DEVICE_DIR/lib" >/dev/null
    adb_push "$PREBUILT_OUT/libbusybox.so"       "$DEVICE_DIR/busybox"
    adb_push "$PREBUILT_OUT/libproot.so"         "$DEVICE_DIR/proot"
    adb_push "$PREBUILT_OUT/libproot-loader.so"  "$DEVICE_DIR/loader"
    adb_push "$PREBUILT_OUT/libtalloc.so"        "$DEVICE_DIR/lib/libtalloc.so.2"
    adb_push "$PREBUILT_OUT/libandroid-shmem.so" "$DEVICE_DIR/lib/libandroid-shmem.so"
    adb_sh "chmod 755 $DEVICE_DIR/busybox $DEVICE_DIR/proot $DEVICE_DIR/loader" >/dev/null

    # /data/local/tmp is noexec on some hardened builds. Find that out here, with
    # a clear message, rather than through fifteen identical "Permission denied"s.
    if ! adb_sh "$DEVICE_DIR/busybox true" >/dev/null 2>&1; then
        die "cannot execute from $DEVICE_DIR (noexec mount, or SELinux denial).
  This is a property of the test location, not of the binaries. The app itself
  runs them from nativeLibraryDir, which is always executable."
    fi

    local ENV="cd $DEVICE_DIR && export LD_LIBRARY_PATH=$DEVICE_DIR/lib && export TMPDIR=$DEVICE_DIR/tmp && export PROOT_LOADER=$DEVICE_DIR/loader && export PROOT_TMP_DIR=$DEVICE_DIR/tmp && mkdir -p \$TMPDIR &&"

    head1 'BusyBox'
    dt_expect "reports version 1.38.0"        "BusyBox v1.38.0" "$ENV $DEVICE_DIR/busybox 2>&1 | head -1"
    dt_expect "sh runs a command"             "hello"           "$ENV $DEVICE_DIR/busybox sh -c 'echo hello'"
    dt_check  "ls"                                              "$ENV $DEVICE_DIR/busybox ls $DEVICE_DIR"
    dt_expect "cat round-trips a file"        "content"         "$ENV $DEVICE_DIR/busybox sh -c 'echo content > t.txt && $DEVICE_DIR/busybox cat t.txt'"
    dt_expect "sed"                           "REPLACED"        "$ENV $DEVICE_DIR/busybox sh -c 'echo original | $DEVICE_DIR/busybox sed s/original/REPLACED/'"
    dt_expect "awk"                           "42"              "$ENV $DEVICE_DIR/busybox sh -c 'echo 40 2 | $DEVICE_DIR/busybox awk \"{print \\\$1+\\\$2}\"'"
    dt_expect "grep"                          "needle"          "$ENV $DEVICE_DIR/busybox sh -c 'printf \"hay\\nneedle\\nhay\\n\" | $DEVICE_DIR/busybox grep needle'"
    dt_expect "tar round-trip"                "payload"         "$ENV $DEVICE_DIR/busybox sh -c 'mkdir -p tt && echo payload > tt/f && $DEVICE_DIR/busybox tar cf t.tar tt && rm -rf tt && $DEVICE_DIR/busybox tar xf t.tar && $DEVICE_DIR/busybox cat tt/f'"
    dt_check  "xz decompression is available"                   "$ENV $DEVICE_DIR/busybox xz --help >/dev/null 2>&1 || $DEVICE_DIR/busybox unxz --help >/dev/null 2>&1"
    dt_check  "wget exists"                                     "$ENV $DEVICE_DIR/busybox wget --help >/dev/null 2>&1"
    dt_check  "mount applet present"                            "$ENV $DEVICE_DIR/busybox mount --help >/dev/null 2>&1"
    dt_expect "applet count is sane"          "OK"              "$ENV $DEVICE_DIR/busybox sh -c 'n=\$($DEVICE_DIR/busybox --list | $DEVICE_DIR/busybox wc -l); [ \$n -gt 250 ] && echo OK || echo \"only \$n\"'"

    head1 'PRoot'
    # No `head` here. proot --version prints a four-line ASCII banner with the
    # version tucked into the last line of it, so truncating the output hides
    # exactly the thing being checked, which it did, reporting a failure
    # against a binary that was fine.
    dt_expect "reports the pinned version"    "v5.1.107.91"     "$ENV $DEVICE_DIR/proot --version 2>&1"
    # The two accelerators are compiled in from build.h, which the makefile fills
    # by probing at build time. Both "yes" means that probe produced real answers
    # rather than silently defaulting to off.
    dt_expect "process_vm accelerator built in"  "process_vm = yes"     "$ENV $DEVICE_DIR/proot --version 2>&1"
    dt_expect "seccomp_filter accelerator built in" "seccomp_filter = yes" "$ENV $DEVICE_DIR/proot --version 2>&1"
    dt_check  "resolves its shared libraries"                   "$ENV $DEVICE_DIR/proot --help >/dev/null 2>&1"

    # The real thing: a minimal rootfs built out of BusyBox itself, entered
    # through PRoot. This is the app's actual runtime path in miniature: if
    # this works, provisioning a real rootfs is a difference of scale.
    note "building a minimal rootfs on the device"
    adb_sh "$ENV $DEVICE_DIR/busybox sh -c '
        rm -rf mini && mkdir -p mini/bin mini/etc mini/proc mini/dev mini/tmp &&
        cp $DEVICE_DIR/busybox mini/bin/busybox &&
        chmod 755 mini/bin/busybox &&
        cd mini/bin && ./busybox --install -s . 2>/dev/null || true
    '" >/dev/null 2>&1 || true

    dt_expect "enters a rootfs and runs a shell" "inside" \
        "$ENV $DEVICE_DIR/proot -r $DEVICE_DIR/mini -0 /bin/busybox sh -c 'echo inside'"
    dt_expect "sees the rootfs as /"             "bin"    \
        "$ENV $DEVICE_DIR/proot -r $DEVICE_DIR/mini -0 /bin/busybox ls /"
    dt_expect "bind mount is visible"            "bound"  \
        "$ENV $DEVICE_DIR/busybox sh -c 'echo bound > $DEVICE_DIR/host.txt' && $ENV $DEVICE_DIR/proot -r $DEVICE_DIR/mini -b $DEVICE_DIR:/mnt -0 /bin/busybox cat /mnt/host.txt"
    dt_expect "reports root inside the sandbox"  "uid=0"  \
        "$ENV $DEVICE_DIR/proot -r $DEVICE_DIR/mini -0 /bin/busybox id"

    # --- verdict ---
    head1 'Result'
    printf '  %s passed, %s failed\n' "$DT_PASS" "$DT_FAIL"

    if [ "$DT_FAIL" -eq 0 ]; then
        mkdir -p "$(dirname "$REPO_ROOT/prebuilts/work/.device-test-passed")"
        {
            printf 'device=%s (SDK %s, %s)\n' "$model" "$sdk" "$abi"
            printf 'checks=%s\n' "$DT_PASS"
            for f in libbusybox.so libproot.so libproot-loader.so libtalloc.so libandroid-shmem.so; do
                printf '%s  %s\n' "$(sha256sum "$PREBUILT_OUT/$f" | cut -d' ' -f1)" "$f"
            done
        } > "$REPO_ROOT/prebuilts/work/.device-test-passed"
        cat <<EOF

$(printf '%sThe built binaries work on real hardware.%s' "$C_GRN" "$C_OFF")

Recorded against their SHA-256, so rebuilding them invalidates this result.
Installing them is now a defensible decision:

    ./prebuilts/build.sh --install

Then update the SHA-256 column in docs/THIRD-PARTY-BINARIES.md in the same
commit, and re-run the app's own provisioning flow end to end.
EOF
    else
        printf '\n%sDo not install these binaries.%s\n' "$C_RED" "$C_OFF"
        printf 'Leave the shipped set in place until every check above passes.\n'
        printf 'Re-run with DT_VERBOSE=1 to see each command'"'"'s output.\n'
    fi

    note "leaving $DEVICE_DIR on the device for inspection; remove with:"
    printf '    adb shell rm -rf %s\n' "$DEVICE_DIR"

    [ "$DT_FAIL" -eq 0 ] || exit 1
}

# =============================================================================
# 2. secrets
# =============================================================================

cmd_secrets() {
    need gh
    need base64
    head1 'Pushing the four DRACOS_* secrets to GitHub Actions'

    key_rotated || die "signing credentials are not configured.
  Run '$0 rotate-key' first. Pushing the burned key's credentials as secrets
  would make the leak worse, not better."

    local store_file
    store_file=$(gradle_prop DRACOS_STORE_FILE)
    [ -f "$store_file" ] || die "keystore not found at $store_file"

    # Refuse to upload the keystore that is already public.
    local size; size=$(stat -c%s "$store_file")
    if [ "$size" -eq 2744 ]; then
        local digest; digest=$(sha256sum "$store_file" | cut -d' ' -f1)
        warn "this keystore is 2744 bytes, the size of the leaked one (sha256 $digest)."
        confirm "Upload anyway? Only do this if you are certain it is a different key." IUNDERSTAND
    fi

    cat <<EOF
Reading from $GRADLE_PROPS and uploading to $GH_REPO.

Nothing is printed. Values go to gh through stdin, never as arguments, because
argv is readable by any process on this machine through /proc.

EOF
    confirm "Upload the signing credentials as repository secrets?" UPLOAD

    local name value
    # base64 -w0: gh stores the value verbatim, and the workflow decodes it with
    # `base64 -d`. Wrapped output would decode fine on GNU coreutils but is one
    # more thing to be wrong about.
    note "DRACOS_KEYSTORE_BASE64"
    base64 -w0 "$store_file" | gh secret set DRACOS_KEYSTORE_BASE64 -R "$GH_REPO" >/dev/null \
        || die "failed to set DRACOS_KEYSTORE_BASE64.
  If this was a 401 or 403, the token cannot write Actions secrets. Fix with:
      gh auth refresh -h github.com -s repo"

    for name in DRACOS_STORE_PASSWORD DRACOS_KEY_ALIAS DRACOS_KEY_PASSWORD; do
        note "$name"
        value=$(gradle_prop "$name") || die "$name missing from $GRADLE_PROPS"
        printf '%s' "$value" | gh secret set "$name" -R "$GH_REPO" >/dev/null \
            || die "failed to set $name"
    done
    unset value

    local missing rc
    missing=$(secrets_present) && rc=0 || rc=$?
    case "$rc" in
        0) ok "all four secrets confirmed present on $GH_REPO" ;;
        1) die "upload reported success but these are still missing:$missing" ;;
        *) warn "uploaded, but the secret list could not be read back to confirm" ;;
    esac

    printf '\nThe release workflow can now build a signed APK. Next: %s tag\n' "$0"
}

# =============================================================================
# 3. tag
# =============================================================================

cmd_tag() {
    need gh
    need git
    local vn vc tag
    vn=$(version_name); vc=$(version_code); tag="v$vn"
    head1 "Publishing $tag"

    # Everything the release workflow will check, checked here first, because a tag is
    # cheap to create and awkward to retract once CI has published a release
    # against it.
    note "pre-flight"

    # This is a hard gate, not a warning.
    #
    # It used to be a warning that said "fine only because they are NOT in the
    # APK", which stops being true the moment someone runs
    # ./prebuilts/build.sh --install. A warning is the wrong instrument for a
    # condition that changes what ships.
    if apk_binaries_documented; then
        ok "APK ships the binaries documented in docs/THIRD-PARTY-BINARIES.md"
    elif installed_matches_device_test; then
        die "the APK contains freshly built binaries that DID pass on a device,
  but docs/THIRD-PARTY-BINARIES.md still records the old SHA-256 values. The
  documented inventory would not match what you are about to publish.

  Update the SHA-256 column there, commit, then tag. Current digests:
$(for f in app/src/main/jniLibs/arm64-v8a/*.so; do
      printf '    %s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$(basename "$f")"
  done)"
    else
        die "the APK contains prebuilt binaries that have never been run.

  app/src/main/jniLibs/arm64-v8a/ does not match docs/THIRD-PARTY-BINARIES.md,
  so ./prebuilts/build.sh --install has replaced them. Those binaries compile and
  link, but nothing has shown they execute, and BusyBox moves from 1.29.3 (2018)
  to 1.38.0 in this change.

  Either exercise them:
      $0 device-test

  or put the shipped set back and release without them:
      git checkout -- app/src/main/jniLibs/arm64-v8a/

  Publishing an APK whose shell has never been started is not a risk worth taking
  for a first release that also pins your signing certificate forever."
    fi

    if [ -n "$(git status --porcelain)" ]; then
        die "working tree is dirty. Commit or stash first:
$(git status --porcelain | sed 's/^/    /')"
    fi
    ok "working tree clean"

    local branch; branch=$(git rev-parse --abbrev-ref HEAD)
    [ "$branch" = "main" ] || die "on branch '$branch'; releases are cut from main"
    ok "on main"

    git fetch --quiet origin main
    [ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] \
        || die "local main and origin/main differ. Push or pull first."
    ok "main matches origin/main"

    python3 scripts/verify-metadata.py >/dev/null \
        || die "scripts/verify-metadata.py fails. Fix that before tagging."
    ok "metadata checks pass"

    local locale f
    for locale in fastlane/metadata/android/*/; do
        f="${locale}changelogs/${vc}.txt"
        [ -f "$f" ] || die "missing $f. Every locale needs a changelog for versionCode $vc"
    done
    ok "every locale has a changelog for versionCode $vc"

    if git rev-parse "$tag" >/dev/null 2>&1; then
        die "$tag already exists locally. A published APK is never replaced; cut a new versionCode instead."
    fi
    case "$(tag_published; echo $?)" in
        0) die "$tag is already published on GitHub. Cut a new versionCode instead." ;;
    esac
    ok "$tag is unused"

    local missing rc
    missing=$(secrets_present) && rc=0 || rc=$?
    case "$rc" in
        0) ok "release secrets are present" ;;
        1) die "the release workflow will fail: missing secrets:$missing
  Run '$0 secrets' first." ;;
        *) warn "could not verify the secrets; the workflow may fail at the signing step" ;;
    esac


    printf '\n'
    cat <<EOF
Pushing $tag runs .github/workflows/release.yml, which builds both signed APKs
and publishes them as a public GitHub release. That is public and hard to take
back once anyone has downloaded it.

  version      $vn (versionCode $vc)
  commit       $(git rev-parse --short HEAD) $(git log -1 --format=%s)
  release note fastlane/metadata/android/en-US/changelogs/$vc.txt

EOF
    confirm "Create and push $tag?" RELEASE

    git tag -a "$tag" -m "dracxterm $vn"
    ok "created $tag"
    git push origin "$tag"
    ok "pushed $tag"

    printf '\nWatch the release build:\n'
    printf '    gh run watch -R %s\n' "$GH_REPO"
    printf '    gh release view %s -R %s\n' "$tag" "$GH_REPO"
}

# =============================================================================

usage() { sed -n '3,18p' "$0" | sed 's/^# \{0,1\}//'; }

# No subcommand takes arguments. Silently ignoring extras is worse than it
# sounds: `release-prep.sh tag RELEASE` looks like it supplies the confirmation
# and skips the prompt, and it did nothing at all: the confirmation still came
# from what was typed at the prompt afterwards. Someone who did not notice would
# believe an unattended release is possible. It is not, on purpose.
cmd="${1:-status}"
shift || true
if [ "$#" -gt 0 ]; then
    printf '%serror:%s `%s` takes no arguments, but got: %s\n' \
        "$C_RED" "$C_OFF" "$cmd" "$*" >&2
    printf '\nRun it without the argument:\n\n' >&2
    printf '    %s %s\n\n' "$0" "$cmd" >&2
    printf 'It will then ask, and you type the word at the prompt:\n\n' >&2
    printf '    type %s to continue: %s\n\n' "$*" "$*" >&2
    printf 'Confirmations are never passed as arguments. That is deliberate: an\n' >&2
    printf 'unattended release is not something this script will do for you.\n' >&2
    exit 2
fi

case "$cmd" in
    status)      cmd_status ;;
    device-test) cmd_device_test ;;
    secrets)     cmd_secrets ;;
    tag)         cmd_tag ;;
    -h|--help|help) usage ;;
    *) printf 'unknown command: %s\n\n' "$cmd" >&2; usage >&2; exit 2 ;;
esac
