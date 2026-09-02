#!/usr/bin/env bash
#
# Put a Linux root filesystem archive where the build will bundle it.
#
#   ./scripts/fetch-rootfs.sh                    download the default image
#   ./scripts/fetch-rootfs.sh "Kali Linux (nano)" pick one by name
#   ./scripts/fetch-rootfs.sh --local PATH        use an archive you already have
#   ./scripts/fetch-rootfs.sh --list              show what the catalogue offers
#   ./scripts/fetch-rootfs.sh --clear             empty the directory again
#
# The URL and SHA-256 come from app/src/main/assets/rootfsURLS.json, not from this
# script. That file is what the app itself downloads from at runtime, so an
# a bundled APK and an APK that downloaded the same image end up with
# byte-identical filesystems. Two sources of truth for one archive would
# eventually disagree, and the disagreement would surface on a user's device.
#
# The archive never enters git; see app/src/main/assets/rootfs/README.txt.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

CATALOG="app/src/main/assets/rootfsURLS.json"
DEST_DIR="app/src/main/assets/rootfs"

# The destination is inside the tracked tree now (that is what makes the build pick it up).
# A 200-400 MB archive committed by accident is paid for by every clone forever, and cannot be
# undone without rewriting history, so refuse to stage one into a path git is not ignoring.
warn_if_tracked() {
    command -v git >/dev/null 2>&1 || return 0
    git rev-parse --is-inside-work-tree >/dev/null 2>&1 || return 0
    if ! git check-ignore -q "$DEST_DIR/probe.tar.xz" 2>/dev/null; then
        printf '\n%sWARNING%s  %s is NOT ignored by git.\n' "$C_BLD" "$C_OFF" "$DEST_DIR"
        printf '         An image fetched here can be committed by accident, and a few hundred\n'
        printf '         megabytes in history is paid for by every clone from then on.\n'
        printf '         Add this line to .gitignore before continuing:\n\n'
        printf '             %s/*.tar.*\n\n' "$DEST_DIR"
        printf '         Continue anyway? [y/N] '
        read -r reply
        case "$reply" in [yY]*) ;; *) die "stopped; nothing was written" ;; esac
    fi
}

if [ -t 1 ]; then
    C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_CYA=$'\033[36m'; C_BLD=$'\033[1m'; C_OFF=$'\033[0m'
else
    C_RED=''; C_GRN=''; C_CYA=''; C_BLD=''; C_OFF=''
fi
die()  { printf '%serror:%s %s\n' "$C_RED" "$C_OFF" "$*" >&2; exit 1; }
note() { printf '%s==>%s %s\n' "$C_CYA" "$C_OFF" "$*"; }
ok()   { printf '  %sok%s    %s\n' "$C_GRN" "$C_OFF" "$*"; }

[ -f "$CATALOG" ] || die "$CATALOG not found"

# --- read the catalogue ------------------------------------------------------
#
# Parsed from the JSON the app ships, never duplicated. A URL hardcoded here
# would drift from the one the app uses the first time either is bumped, and
# nothing would notice until a maintainer's build disagreed with a user's
# download.
#
# Only arm64-v8a is read: every prebuilt binary in this project is arm64-v8a, so
# PRoot could not execute an image for another architecture even if one were
# bundled.
ABI="arm64-v8a"

catalog_field() {   # <label> <field>
    python3 - "$CATALOG" "$ABI" "$1" "$2" <<'PY3'
import json, sys
path, abi, label, field = sys.argv[1:5]
e = json.load(open(path)).get(abi, {}).get(label)
if e is None:
    sys.exit(1)
print(e.get(field, ""))
PY3
}

catalog_default() {
    python3 - "$CATALOG" "$ABI" <<'PY3'
import json, sys
d = json.load(open(sys.argv[1]))
want = d.get("_default", "")
print(want if want in d.get(sys.argv[2], {}) else "")
PY3
}

catalog_labels() {
    python3 - "$CATALOG" "$ABI" <<'PY3'
import json, sys
for label in json.load(open(sys.argv[1])).get(sys.argv[2], {}):
    print(label)
PY3
}

cmd_list() {
    printf '%sImages in %s for %s%s\n\n' "$C_BLD" "$CATALOG" "$ABI" "$C_OFF"
    local label
    while IFS= read -r label; do
        printf '  %s%s%s\n' "$C_BLD" "$label" "$C_OFF"
        printf '    url     %s\n' "$(catalog_field "$label" url)"
        printf '    sha256  %s\n' "$(catalog_field "$label" sha256)"
        printf '    size    %s bytes\n' "$(catalog_field "$label" downloadBytes)"
        printf '    needs   %s bytes free once extracted\n\n' "$(catalog_field "$label" installBytes)"
    done < <(catalog_labels)
    printf 'Default: %s\n' "$(catalog_default)"
}

cmd_clear() {
    local f n=0
    for f in "$DEST_DIR"/*; do
        case "$(basename "$f")" in .gitkeep|README.txt|'*') continue ;; esac
        rm -f "$f"; n=$((n + 1))
    done
    ok "removed $n archive(s) from $DEST_DIR"
    printf '\nThe next build will now be the ONLINE installer: with no archive here the\n'
    printf 'app offers the catalogue in assets/rootfsURLS.json instead.\n'
    printf 'Build it with:  ./gradlew assembleRelease\n'
}

# verify <file> <expected-sha256>. Never leaves a mismatched file behind.
verify() {
    local file="$1" want="$2" have
    note "verifying SHA-256 of $(basename "$file") ($(stat -c%s "$file") bytes)"
    have=$(sha256sum "$file" | cut -d' ' -f1)
    if [ "$have" != "$want" ]; then
        rm -f "$file"
        die "SHA-256 mismatch. The file has been deleted.
    expected $want
    got      $have
  The archive is corrupt, truncated, or not the one the catalogue pins.
  Do not work around this by editing the pin: the app verifies the same digest
  on the device, so a mismatch here is a mismatch there."
    fi
    ok "digest matches the pin in the catalogue"
}

cmd_local() {
    local src="$1"
    [ -f "$src" ] || die "no such file: $src"

    # Match it against the catalogue so a local copy gets the same scrutiny as a
    # download. An unrecognised archive is allowed, because assets/rootfs/README.txt
    # documents bring-your-own images, but it is named as unverified.
    local have label matched=""
    have=$(sha256sum "$src" | cut -d' ' -f1)
    while IFS= read -r label; do
        [ "$have" = "$(catalog_field "$label" sha256)" ] && { matched="$label"; break; }
    done < <(catalog_labels)

    mkdir -p "$DEST_DIR"
    cmd_clear >/dev/null
    cp "$src" "$DEST_DIR/$(basename "$src")"
    if [ -n "$matched" ]; then
        ok "matches the catalogue entry '$matched'"
    else
        printf '  %sNOTE%s  this archive is not one the catalogue pins (sha256 %s).\n' \
            "$C_BLD" "$C_OFF" "$have"
        printf '        That is supported (see app/src/main/assets/rootfs/README.txt),\n'
        printf '        but nothing has verified what is inside it. You are vouching for it.\n'
    fi
    ok "staged $DEST_DIR/$(basename "$src")"
}

cmd_fetch() {
    # The catalogue states its own default. Falling back to the first entry only
    # when it does not, so reordering the file cannot change what gets bundled.
    local label="${1:-$(catalog_default)}"
    [ -n "$label" ] || label=$(catalog_labels | head -1)
    catalog_labels | grep -Fxq "$label" || die "unknown image: $label
  Known:
$(catalog_labels | sed 's/^/    /')
  List them with:  $0 --list"

    local name sha size url
    url=$(catalog_field "$label" url)
    sha=$(catalog_field "$label" sha256)
    size=$(catalog_field "$label" downloadBytes)
    name="${url##*/}"

    printf '%sFetching the image this build will bundle%s\n\n' "$C_BLD" "$C_OFF"
    printf '  image   %s\n  file    %s\n  from    %s\n  size    ~%s MB\n  sha256  %s\n\n' \
        "$label" "$name" "$url" "$((size / 1000000))" "$sha"

    mkdir -p "$DEST_DIR"

    # Reuse a correct copy rather than downloading it again.
    if [ -f "$DEST_DIR/$name" ]; then
        if [ "$(sha256sum "$DEST_DIR/$name" | cut -d' ' -f1)" = "$sha" ]; then
            ok "already present and verified: $DEST_DIR/$name"
            return 0
        fi
        note "existing copy has the wrong digest; refetching"
        rm -f "$DEST_DIR/$name"
    fi

    # Also reuse a copy left wherever the maintainer keeps them.
    local candidate
    for candidate in \
        "$HOME/Desktop/dracxterm-local-assets/$name" \
        "$HOME/Downloads/$name"
    do
        if [ -f "$candidate" ] && [ "$(sha256sum "$candidate" | cut -d' ' -f1)" = "$sha" ]; then
            note "found a verified local copy at $candidate"
            cp "$candidate" "$DEST_DIR/$name"
            ok "staged without downloading"
            return 0
        fi
    done

    command -v curl >/dev/null || die "curl is not installed"
    note "downloading (~$((size / 1000000)) MB)"
    # .part staging: a truncated download must never be mistaken for the archive.
    curl -fL --retry 3 --progress-bar -o "$DEST_DIR/$name.part" "$url" \
        || { rm -f "$DEST_DIR/$name.part"; die "download failed: $url"; }
    mv "$DEST_DIR/$name.part" "$DEST_DIR/$name"
    verify "$DEST_DIR/$name" "$sha"

    printf '\nBuild it (the archive above makes this an OFFLINE installer):\n    ./gradlew assembleRelease\n'
}

case "${1:-}" in
    --list|-l)  cmd_list ;;
    --clear)    cmd_clear ;;
    --local)    [ $# -ge 2 ] || die "--local needs a path"; warn_if_tracked; cmd_local "$2" ;;
    -h|--help)  sed -n '3,18p' "$0" | sed 's/^# \{0,1\}//' ;;
    -*)         die "unknown option: $1" ;;
    "")         warn_if_tracked; cmd_fetch ;;
    *)          warn_if_tracked; cmd_fetch "$1" ;;
esac
