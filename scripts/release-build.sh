#!/usr/bin/env bash
#
# Build the signed release APK and App Bundle, check them, and stage both under
# apps/ with their SHA-256 files.
#
#   ./scripts/release-build.sh
#
# The keystore is never read from the repository. Gradle looks it up through the
# four DRACOS_* properties in ~/.gradle/gradle.properties and refuses to produce
# an unsigned release, so this script does not handle passwords at all.
#
# apps/ is ignored by git. The APK belongs on a GitHub Release; the AAB is for
# Play Console uploads only and must not be published anywhere else.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

OUT_DIR="$REPO_ROOT/apps"

if [ -t 1 ]; then
    C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_CYA=$'\033[36m'; C_OFF=$'\033[0m'
else
    C_RED=''; C_GRN=''; C_CYA=''; C_OFF=''
fi
die()  { printf '%serror:%s %s\n' "$C_RED" "$C_OFF" "$*" >&2; exit 1; }
note() { printf '\n%s==>%s %s\n' "$C_CYA" "$C_OFF" "$*"; }
ok()   { printf '  %sok%s    %s\n' "$C_GRN" "$C_OFF" "$*"; }

version_name() { sed -n 's/.*versionName *= *"\([^"]*\)".*/\1/p' app/build.gradle.kts | head -1; }
version_code() { sed -n 's/.*versionCode *= *\([0-9]*\).*/\1/p' app/build.gradle.kts | head -1; }

# The SDK root, and the newest build-tools in it. apksigner and zipalign live there.
sdk_root() {
    if [ -f local.properties ]; then
        local dir
        dir=$(sed -n 's/^sdk\.dir=//p' local.properties | head -1)
        if [ -n "$dir" ]; then printf '%s' "$dir"; return 0; fi
    fi
    printf '%s' "${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
}

VERSION="$(version_name)"
CODE="$(version_code)"
[ -n "$VERSION" ] && [ -n "$CODE" ] || die "could not read versionName/versionCode from app/build.gradle.kts"

BASE="dracxterm-$VERSION-vc$CODE-release"
APK_OUT="$OUT_DIR/$BASE.apk"
AAB_OUT="$OUT_DIR/$BASE.aab"

SDK="$(sdk_root)"
BUILD_TOOLS="$(ls -1d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -1 || true)"
[ -n "$BUILD_TOOLS" ] || die "no build-tools under $SDK. Install them with sdkmanager first."

note "Building $BASE (Gradle)"
ANDROID_HOME="$SDK" ANDROID_SDK_ROOT="$SDK" \
    ./gradlew --no-daemon clean assembleRelease bundleRelease

APK_SRC="app/build/outputs/apk/release/app-release.apk"
AAB_SRC="app/build/outputs/bundle/release/app-release.aab"
[ -f "$APK_SRC" ] || die "Gradle produced no APK at $APK_SRC"
[ -f "$AAB_SRC" ] || die "Gradle produced no AAB at $AAB_SRC"

note "Checking the APK"
"$BUILD_TOOLS/apksigner" verify --print-certs "$APK_SRC" > /dev/null \
    || die "apksigner could not verify the APK"
ok "signature verifies"
"$BUILD_TOOLS/zipalign" -c -p 4 "$APK_SRC" \
    || die "the APK is not 4-byte aligned with page-aligned shared libraries"
ok "zipalign -c -p 4 passes"

abis="$(unzip -Z1 "$APK_SRC" 'lib/*' 2>/dev/null | cut -d/ -f2 | sort -u | tr '\n' ' ')"
ok "native ABIs: ${abis:-none}"

note "Checking the App Bundle"
if command -v bundletool > /dev/null 2>&1; then
    bundletool validate --bundle="$AAB_SRC" > /dev/null \
        || die "bundletool rejected the App Bundle"
    ok "bundletool validate passes"
else
    printf '  skip  bundletool is not installed; the AAB was not validated\n'
fi

note "Staging into apps/"
mkdir -p "$OUT_DIR"
cp -f "$APK_SRC" "$APK_OUT"
cp -f "$AAB_SRC" "$AAB_OUT"
( cd "$OUT_DIR" && sha256sum "$BASE.apk" > "$BASE.apk.sha256" \
                && sha256sum "$BASE.aab" > "$BASE.aab.sha256" )
ok "$(basename "$APK_OUT")  ($(du -h "$APK_OUT" | cut -f1))"
ok "$(basename "$AAB_OUT")  ($(du -h "$AAB_OUT" | cut -f1))"

# A rule in .gitignore does not help a file that was added before the rule, so
# ask git what it actually tracks.
if git -C "$REPO_ROOT" ls-files --error-unmatch apps > /dev/null 2>&1; then
    die "apps/ is tracked by git. It must stay local; fix that before releasing."
fi
ok "apps/ is untracked"

note "Done"
cat <<EOF
  Install the APK:      adb install -r $APK_OUT
  Attach to a release:  gh release create v$VERSION $APK_OUT $APK_OUT.sha256

  Do not attach or commit $BASE.aab. It is for a Play Console upload only.
EOF
