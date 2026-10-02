#!/bin/sh
# Pasang dan konfigurasi Scrapling untuk claw, agar tool browsing (web_fetch dan
# `claw browse`) siap dipakai tanpa langkah manual.
#
# Dijalankan otomatis sebagai fake-root oleh aplikasi setelah rootfs Linux dipasang
# (com.dracxterm.claw.ScraplingProvisioner), dan aman dijalankan sendiri kapan saja:
#
#     sh /opt/claw/data/scripts/scrapling-setup.sh
#
# Sifatnya idempotent: kalau sudah siap, langsung keluar. Kalau gagal di tengah
# (mis. jaringan putus), berkas penanda tidak ditulis sehingga percobaan diulang
# pada peluncuran berikutnya. Semua langkah ditulis ke $LOG supaya bisa dipantau:
#
#     tail -f /opt/claw/scrapling/setup.log
set -u

ROOT=/opt/claw
DIR="$ROOT/scrapling"
VENV="$DIR/.venv"
PY="$VENV/bin/python"
MARK="$DIR/.ready"
LOG="$DIR/setup.log"
SPEC="scrapling[fetchers]==0.4.15"

mkdir -p "$DIR" 2>/dev/null || true
log() { echo "$*"; echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG" 2>/dev/null || true; }

# Sudah siap? (penanda ada, venv bisa dipakai, dan Scrapling bisa diimpor)
if [ -f "$MARK" ] && [ -x "$PY" ] && "$PY" -c "from scrapling.fetchers import Fetcher" >/dev/null 2>&1; then
    log "Scrapling sudah siap, tidak ada yang dikerjakan."
    exit 0
fi

# Jangan berebut lock dpkg dengan apt/dpkg lain (mis. `apt install` milik pengguna). Bila ada
# proses apt/apt-get/dpkg lain berjalan, tunda: keluar diam-diam dan coba lagi peluncuran berikutnya.
# Ini mencegah provisioning latar memblokir perintah paket interaktif pengguna.
apt_busy() {
    for c in /proc/[0-9]*/comm; do
        IFS= read -r n < "$c" 2>/dev/null || continue
        case "$n" in apt|apt-get|dpkg|aptitude|unattended-upgr|unattended-upgrade) return 0 ;; esac
    done
    return 1
}
if apt_busy; then
    log "apt/dpkg sedang dipakai proses lain; menunda setup Scrapling (dicoba lagi nanti)."
    exit 0
fi

log "=== menyiapkan Scrapling untuk browsing claw ==="

# 1. python3 + venv + pip (butuh root; skrip ini dijalankan sebagai fake-root).
if ! command -v python3 >/dev/null 2>&1; then
    log "[1/4] memasang python3 (apt)..."
    export DEBIAN_FRONTEND=noninteractive
    if command -v apt-get >/dev/null 2>&1; then
        apt-get update >>"$LOG" 2>&1 && \
        apt-get install -y python3 python3-venv python3-pip >>"$LOG" 2>&1 || {
            log "GAGAL: apt tidak bisa memasang python3. Coba lagi saat jaringan stabil."; exit 1; }
    else
        log "GAGAL: apt-get tidak ada di distro ini. Pasang python3 secara manual."; exit 1
    fi
else
    log "[1/4] python3 sudah ada ($(python3 --version 2>&1))."
fi

# Sebagian distro memisah modul venv; pastikan ada.
if ! python3 -c "import venv, ensurepip" >/dev/null 2>&1; then
    log "[1b] melengkapi modul venv..."
    export DEBIAN_FRONTEND=noninteractive
    apt-get install -y python3-venv >>"$LOG" 2>&1 || true
fi

# 2. virtualenv khusus claw.
if [ ! -x "$PY" ]; then
    log "[2/4] membuat virtualenv di $VENV ..."
    python3 -m venv "$VENV" >>"$LOG" 2>&1 || { log "GAGAL: tidak bisa membuat virtualenv."; exit 1; }
else
    log "[2/4] virtualenv sudah ada."
fi

# 3. pasang Scrapling (fetcher HTTP; browser tidak diunduh).
log "[3/4] memasang $SPEC (unduhan sekitar 350 MB, sekali saja)..."
"$PY" -m pip install --disable-pip-version-check "$SPEC" >>"$LOG" 2>&1 || {
    log "GAGAL: pip tidak bisa memasang Scrapling. Periksa jaringan lalu ulangi."; exit 1; }

# 4. verifikasi impor.
log "[4/4] memeriksa instalasi..."
if ! "$PY" -c "from scrapling.fetchers import Fetcher" >>"$LOG" 2>&1; then
    log "GAGAL: Scrapling terpasang tapi tidak bisa diimpor."; exit 1
fi

: > "$MARK" 2>/dev/null || true
log "SIAP: browsing claw sudah bisa dipakai. Coba: claw browse https://example.com"
exit 0
