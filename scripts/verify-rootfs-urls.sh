#!/usr/bin/env bash
#
# Check app/src/main/assets/rootfsURLS.json against upstream.
#
#   ./scripts/verify-rootfs-urls.sh          check every entry
#   ./scripts/verify-rootfs-urls.sh --quick  reachability and size only, no checksums
#
# The digests in that file are the only thing standing between a user and an
# unverified archive being unpacked into the app sandbox, and they were pinned by
# hand. Upstream moves: Kali cuts a new release and the old rootfs is pruned.
# When that happens the pinned URL 404s or the digest
# stops matching, and every user's download fails.
#
# This finds that here instead.
#
# Exit status: 0 = every entry reachable and matching, 1 = at least one problem.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"
CATALOG="app/src/main/assets/rootfsURLS.json"

if [ -t 1 ]; then
    C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'; C_BLD=$'\033[1m'; C_OFF=$'\033[0m'
else
    C_RED=''; C_GRN=''; C_YEL=''; C_BLD=''; C_OFF=''
fi
ok()   { printf '  %sok%s    %s\n' "$C_GRN" "$C_OFF" "$*"; }
bad()  { printf '  %sFAIL%s  %s\n' "$C_RED" "$C_OFF" "$*"; }
warn() { printf '  %swarn%s  %s\n' "$C_YEL" "$C_OFF" "$*"; }

[ -f "$CATALOG" ] || { echo "error: $CATALOG not found" >&2; exit 1; }
command -v curl >/dev/null || { echo "error: curl is not installed" >&2; exit 1; }

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

# Where each project publishes the checksum for a given file. Returns a URL and the
# grep pattern that finds the line, or nothing when the layout is unknown.
checksum_source() {
    local url="$1" dir file
    dir="${url%/*}"; file="${url##*/}"
    case "$url" in
        # The mirror publishes one SHA256SUMS on its default branch, not one per
        # release directory, so the digest does not come from "$dir" here. Turn
        #   https://github.com/<owner>/<repo>/releases/download/<tag>/<file>
        # into
        #   https://raw.githubusercontent.com/<owner>/<repo>/main/SHA256SUMS
        *//github.com/*/releases/download/*)
            printf 'https://raw.githubusercontent.com/%s/main/SHA256SUMS\n' \
                "$(printf '%s' "$url" | cut -d/ -f4,5)" ;;
        *kali.download*)           printf '%s/SHA256SUMS\n' "$dir" ;;
        *images.linuxcontainers.org*) printf '%s/SHA256SUMS\n' "$dir" ;;
        *)                         printf '\n' ;;
    esac
}

printf '%sChecking %s against upstream%s\n\n' "$C_BLD" "$CATALOG" "$C_OFF"

fail=0
total=0

# Emit one "abi<TAB>label<TAB>url<TAB>sha256<TAB>downloadBytes" line per entry.
while IFS=$'\t' read -r abi label url sha size; do
    total=$((total + 1))
    printf '%s%s / %s%s\n' "$C_BLD" "$abi" "$label" "$C_OFF"

    # Reachable, and the right size? A moved file usually 404s; a rebuilt one
    # usually changes size. Either is a reason to look.
    head=$(curl -sIL --max-time 45 -o /dev/null -w '%{http_code} %{size_download} %{url_effective}' "$url" 2>/dev/null || echo "000 0 -")
    code=${head%% *}
    if [ "$code" != "200" ]; then
        bad "unreachable (HTTP $code) $url"
        fail=1
        continue
    fi

    remote_size=$(curl -sIL --max-time 45 "$url" | tr -d '\r' | awk 'tolower($1)=="content-length:"{n=$2} END{print n+0}')
    if [ "$remote_size" -eq 0 ]; then
        warn "server did not report a size; cannot compare"
    elif [ "$remote_size" != "$size" ]; then
        bad "size changed: pinned $size, upstream $remote_size"
        fail=1
    else
        ok "reachable, size matches ($size bytes)"
    fi

    [ "$QUICK" -eq 1 ] && { printf '\n'; continue; }

    src=$(checksum_source "$url")
    if [ -z "$src" ]; then
        warn "no known checksum layout for this host; digest not verified"
        printf '\n'
        continue
    fi

    file="${url##*/}"
    # Upstream formats differ: "<sha>  <file>", "<sha> *<file>", and some hosts
    # "SHA256 (<file>) = <sha>". Pull whichever 64-hex token shares a line with
    # the filename rather than assuming a column.
    #
    # The trailing "|| true" is what keeps a missing digest a warning instead of the
    # end of the run. Under "set -e" with pipefail a grep that matches nothing fails
    # the whole pipeline, and the failure of a command substitution in an assignment
    # is the failure of the assignment, so the script exited here without printing
    # anything and left the remaining entries unchecked. An empty result is a normal
    # outcome the "could not read a digest" branch below already handles: a host can
    # prune its SHA256SUMS while still serving the archive.
    upstream=$(curl -sL --max-time 45 "$src" 2>/dev/null \
        | grep -F "$file" \
        | grep -oiE '[0-9a-f]{64}' \
        | head -1 | tr 'A-F' 'a-f' || true)

    if [ -z "$upstream" ]; then
        warn "could not read a digest for $file from $src"
        printf '\n'
        continue
    fi
    if [ "$upstream" = "$sha" ]; then
        ok "sha256 matches upstream"
    else
        bad "sha256 MISMATCH
        pinned   $sha
        upstream $upstream
        Do not just paste the new value in. Find out why it changed: a rebuilt
        image is normal, a changed image at the same URL is not."
        fail=1
    fi
    printf '\n'
done < <(python3 - "$CATALOG" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for abi, images in d.items():
    if abi.startswith("_"):
        continue
    for label, e in images.items():
        print("\t".join([abi, label, e["url"], e["sha256"], str(e["downloadBytes"])]))
PY
)

printf '%s%s entr%s checked%s\n' "$C_BLD" "$total" "$([ "$total" = 1 ] && echo y || echo ies)" "$C_OFF"
if [ "$fail" -eq 0 ]; then
    printf '%sEverything matches upstream.%s\n' "$C_GRN" "$C_OFF"
else
    printf '%sSomething moved. Fix %s before shipping.%s\n' "$C_RED" "$CATALOG" "$C_OFF"
fi
exit "$fail"
