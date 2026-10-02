#!/bin/sh
# Menyediakan modul Python yang DI-BUNDLE bersama aplikasi untuk skill claw, TANPA jaringan/apt/pip:
#   scapy -> skill `networking`  (murni Python, langsung pakai lewat PYTHONPATH)
#   pyrit -> skill `ai-red-team`  (sumber Microsoft PyRIT; dependensinya berat, dipasang saat dipakai)
#
# Modul diekstrak dari /opt/claw/data/pytools.tar ke /opt/claw/pytools, lalu dipakai dengan:
#     PYTHONPATH=/opt/claw/pytools python3 ...
#
# PENTING (akar masalah lock apt): langkah ini TIDAK menjalankan apt/pip/jaringan sama sekali,
# jadi ia tidak pernah menahan lock dpkg. Perintah `apt`/`dpkg` milik pengguna tidak akan lagi
# terblokir oleh provisioning latar seperti sebelumnya. Aman dijalankan sendiri kapan saja:
#     sh /opt/claw/data/scripts/tools-setup.sh
set -u

ROOT=/opt/claw
DIR="$ROOT/pytools"
TARBALL="$ROOT/data/pytools.tar"
MARK="$DIR/.ready"
VERFILE="$DIR/.bundle-version"
LOG="$ROOT/tools/setup.log"

# Versi isi bundel. Naikkan angka ini setiap kali pytools.tar diubah (mis. patch
# modul), supaya instalasi lama yang sudah punya $MARK ikut re-ekstrak isi baru
# alih-alih memakai ekstraksi lama. v2: scapy di-patch agar impor `scapy.all`
# tetap jalan di guest rootless (socket RTNETLINK ditolak -> degrade, bukan error).
VERSION=2

mkdir -p "$ROOT/tools" 2>/dev/null || true
log() { echo "$*"; echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG" 2>/dev/null || true; }

# Sudah diekstrak DAN versinya cocok? (penanda ada, versi sama, kedua paket punya __init__.py)
if [ -f "$MARK" ] && [ "$(cat "$VERFILE" 2>/dev/null)" = "$VERSION" ] && \
   [ -f "$DIR/scapy/__init__.py" ] && [ -f "$DIR/pyrit/__init__.py" ]; then
    log "Modul bundel (scapy + pyrit) v$VERSION sudah siap di $DIR."
    exit 0
fi

if [ ! -f "$TARBALL" ]; then
    log "GAGAL: bundel $TARBALL tidak ada (aset claw belum terpasang lengkap)."
    exit 1
fi

log "=== mengekstrak modul bundel claw (scapy, pyrit) dari $TARBALL ==="
tmp="$ROOT/.pytools.tmp.$$"
rm -rf "$tmp" 2>/dev/null || true
mkdir -p "$tmp" || { log "GAGAL: tidak bisa membuat direktori sementara."; exit 1; }

# Tarball (tar polos) berisi top-level 'pytools/'. Ekstrak ke tmp lalu tempatkan secara atomik.
if ! tar xf "$TARBALL" -C "$tmp" >>"$LOG" 2>&1; then
    log "GAGAL: ekstraksi tarball gagal."; rm -rf "$tmp"; exit 1
fi
rm -rf "$DIR" 2>/dev/null || true
if ! mv "$tmp/pytools" "$DIR" 2>>"$LOG"; then
    mkdir -p "$DIR"; cp -a "$tmp/pytools/." "$DIR/" >>"$LOG" 2>&1   # fallback bila mv lintas-mount gagal
fi
rm -rf "$tmp" 2>/dev/null || true

if [ -f "$DIR/scapy/__init__.py" ] && [ -f "$DIR/pyrit/__init__.py" ]; then
    printf '%s' "$VERSION" > "$VERFILE" 2>/dev/null || true
    : > "$MARK" 2>/dev/null || true
    log "SIAP: scapy langsung bisa dipakai (PYTHONPATH=$DIR). pyrit tersedia sebagai sumber;"
    log "      dependensinya (numpy, pydantic, dll.) dipasang saat dipakai lewat skill ai-red-team."
    exit 0
fi
log "GAGAL: modul tidak lengkap setelah ekstraksi."
exit 1
