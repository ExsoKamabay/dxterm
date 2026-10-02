---
name: security-audit
description: "Audit keamanan & pentest berwenang untuk kode/sistem milik pengguna sendiri (atau CTF/lab/target yang diizinkan): pipeline temuan berbasis bukti, kurangi false positive, cari akar masalah, perbaiki, lalu retest."
---

# Audit Keamanan & Pentest Berwenang (berbasis bukti)

Skill ini menjadikan security engineering sebisa mungkin otomatis, berulang, berbasis bukti, dan
**non-destruktif**: temukan → pahami → perbaiki → verifikasi. Bukan sekadar "temukan → lapor → berhenti".

## Batas otorisasi — WAJIB
Uji keamanan HANYA pada: sistem milik pengguna, localhost, lab/dev/staging, repository yang diberi
pengguna, atau target yang secara eksplisit masuk scope. Jangan perluas scope ke pihak lain. Kalau
target `127.0.0.1:8080`, jangan otomatis memindai LAN/internet/domain lain. Kalau kepemilikan/izin
tidak jelas, berhenti dan tanya dulu.

## Default non-destruktif — WAJIB
Jangan menghapus data, mengambil akun, menghentikan service, mengenkripsi/merusak data, DoS, atau
mengekstrak data nyata yang tidak perlu. Kalau sebuah celah bisa dibuktikan dengan request aman,
pakai cara itu. Gunakan least privilege; jangan naikkan privilege hanya karena bisa. Jangan pernah
menampilkan secret penuh — selalu redaksi (mis. `sk_live_****91ab`).

## Pipeline (jalankan yang relevan dengan tool yang ADA)
DISCOVER → ENUMERATE → STATIC ANALYSIS → DEPENDENCY AUDIT → SECRET SCAN → CONFIG AUDIT →
DYNAMIC ANALYSIS → FUZZING → NETWORK ASSESSMENT → CORRELATE → REMOVE FALSE POSITIVES →
RISK ANALYSIS → ROOT CAUSE → REMEDIATION → RETEST → REPORT.

Pahami dulu project (struktur, entry point, bahasa/framework, dependency, build, deploy, auth,
storage, API, secrets, container, CI/CD, service yang terekspos), lalu bangun attack-surface map.

## Pilih tool sesuai stack (pakai yang tersedia; jangan pasang berpuluh tool tanpa alasan)
- Static: Semgrep, CodeQL, Bandit (Python), cppcheck/clang-tidy (C/C++), ESLint security (JS).
- Dependency: Trivy, Grype, `npm audit`, `pip-audit`, `cargo audit`, osv-scanner. SBOM: Syft.
- Secret: Gitleaks, detect-secrets (cek git history bila perlu).
- Web/API: OWASP ZAP, Nuclei (template aman & relevan).
- Network: Nmap (tetap dalam scope).
- Container: Trivy + audit konfigurasi Docker.
- Fuzzing: libFuzzer/AFL++ + sanitizer. Runtime C/C++: ASan/UBSan/TSan/LeakSanitizer/Valgrind.
Kalau tool belum ada dan environment mengizinkan, pasang lewat package manager yang sesuai; kalau
sebuah tool gagal, cari sebabnya (belum terpasang, config, permission, platform) — jangan mengulang
perintah sama yang sudah terbukti gagal.

## Baseline yang diperiksa
Secret/credential hardcoded, permission tidak aman, default tidak aman, dependency usang/rentan,
deserialization tak aman, injeksi (SQL/perintah), auth/authorization rusak, path traversal, file
upload, eksekusi perintah, memory corruption (C/C++), kripto lemah, konfigurasi jaringan tak aman,
information disclosure, race condition, batas privilege, IPC tak aman, validasi input kurang. Untuk
web/API pakai kategori OWASP yang relevan — tapi cari implementasi aktual penyebabnya, bukan sekadar
mencocokkan nama kategori.

## Data-flow sebelum menyatakan celah
SOURCE (input tak tepercaya) → DATA FLOW → SENSITIVE SINK (shell, SQL, filesystem, template,
network, deserialization, privileged API). Telusuri alurnya dulu; jangan menyimpulkan celah dari
pola saja. Untuk CVE dependency: cek versi terpasang, rentang terdampak, versi fixed, dan apakah
jalur kode rentan benar-benar dipakai (reachability) sebelum menyimpulkan aplikasi rentan.

## Validasi & kurangi false positive
Temuan scanner ≠ celah nyata. SCANNER FINDING → SOURCE REVIEW → RUNTIME CONFIRMATION (aman) →
CONFIRMED / FALSE POSITIVE. Kelompokkan: CONFIRMED / LIKELY / FALSE POSITIVE / INFORMATIONAL / NOT
APPLICABLE. Setiap temuan penting wajib punya bukti (source-code/scanner/runtime/log/test/config).
Jangan menyerahkan ratusan alert mentah. Jangan bilang "critical vulnerability" tanpa bukti cukup —
pakai "potential" / "likely" / "confirmed" sesuai bukti.

## Format temuan
Finding · Komponen · Severity · Evidence · Root cause · Potential impact · Affected code ·
Recommended remediation · Verification method · Status. Severity untuk triase, bukan pengganti
analisis. Prioritaskan remediasi dari exploitability × impact × exposure × required privileges ×
affected assets × confidence.

## Perbaiki akar masalah, lalu retest
Jangan berhenti di "SQL injection ditemukan" — cari mengapa input mencapai sink (concatenation?
ORM salah pakai? validasi hilang? authorization terlewati?). Bila punya akses source: buat patch
terkecil yang benar → build → test → jalankan ulang uji keamanan yang tadi menemukan masalah →
pastikan temuan hilang → jalankan regression test → buat security regression test bila memungkinkan.

## Laporan akhir
SECURITY STATUS · CONFIRMED · FIXED · UNRESOLVED · FALSE POSITIVES · TESTS · TOOLS · FILES MODIFIED ·
RETEST RESULT · LIMITATIONS. Jangan menyatakan sistem "100% aman" — tidak ada audit yang membuktikan
keamanan absolut. Untuk debug program pakai `audit-debug`; untuk baca CVE/advisory pakai `web-research`.
