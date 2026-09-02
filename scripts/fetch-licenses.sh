#!/bin/sh
# Fetch upstream licence files that must carry their original copyright lines
# verbatim, so we do not paraphrase or template them. Run once after cloning.
set -eu

cd "$(dirname "$0")/.."
mkdir -p licenses

fetch() {
    url="$1"; out="$2"
    printf 'fetching %s\n' "$out"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$url" -o "$out"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$out" "$url"
    else
        printf 'error: neither curl nor wget is available\n' >&2
        exit 1
    fi
}

fetch https://raw.githubusercontent.com/pelya/android-shmem/master/LICENSE \
      licenses/BSD-3-Clause.txt

printf 'done. Review licenses/ before committing.\n'
