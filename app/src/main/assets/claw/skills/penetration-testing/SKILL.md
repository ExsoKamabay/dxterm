---
name: penetration-testing
description: "Penetration testing white-hat untuk target milik sendiri atau yang berizin tertulis (lab, CTF, scope yang disetujui): recon, enumerasi, analisis kerentanan, pembuktian aman (PoC non-destruktif), lalu laporan dan remediasi. Mengendarai tool yang ada di guest (nmap, nikto, gobuster/ffuf, whatweb, nuclei, sqlmap, hydra, metasploit, searchsploit)."
---

# Penetration Testing (white-hat, berbasis bukti)

Skill ini menjalankan satu engagement pentest terarah: dari pengenalan target sampai laporan yang
bisa ditindaklanjuti. Fokusnya membuktikan risiko dengan cara paling aman, bukan merusak. Untuk audit
kode sumber pakai `security-audit`; untuk uji ketahanan model AI pakai `ai-red-team`; skill ini untuk
menguji sistem/jaringan/web yang berjalan.

## Batas otorisasi, WAJIB dibaca lebih dulu
Uji HANYA target yang kamu miliki atau yang punya izin tertulis: localhost, lab/dev/staging milik
pengguna, mesin CTF, atau host/aplikasi/rentang yang eksplisit masuk scope. Kalau kepemilikan atau
izin tidak jelas, berhenti dan tanya dulu. Jangan memperluas scope sendiri: kalau target
`10.0.0.5`, jangan ikut memindai sisa LAN, gateway, internet, atau domain lain. Simpan catatan scope
(host/port/aplikasi yang boleh, jendela waktu, yang dikecualikan) sebelum mengirim satu paket pun.

## Aturan main, WAJIB
- Non-destruktif. Jangan menghapus, mengubah, atau mengenkripsi data target. Jangan mematikan atau
  membuat service kewalahan. DoS dan uji ketahanan beban tidak termasuk kecuali diminta tertulis dan
  dijadwalkan.
- Jejak minimal. Pakai intensitas sekecil mungkin yang masih membuktikan temuan. Satu request yang
  cukup mengalahkan pemindaian membabi buta.
- Bukti tanpa eksploitasi penuh. Kalau sebuah celah bisa ditunjukkan dengan PoC read-only (membaca
  satu baris `/etc/passwd`, mengambil versi, memicu error terkontrol), cukup sampai di situ. Jangan
  menanam backdoor, jangan menambah akun, jangan mengambil data nyata yang tidak perlu.
- Least privilege. Jangan menaikkan privilege hanya karena bisa. Kalau perlu untuk membuktikan
  dampak, lakukan sekali, catat, lalu berhenti.
- Redaksi. Jangan pernah menampilkan kredensial atau token penuh. Potong (mis. `admin:S3cr3t****`).
- Jangan menyembunyikan diri untuk tujuan jahat. Teknik anti-deteksi hanya relevan saat scope
  memang menguji deteksi/respons (purple team), dan hanya dalam batas itu.
- Sesudah selesai, bersihkan artefak yang kamu buat (file upload uji, shell PoC, entri yang
  ditambahkan) dan laporkan apa yang dibersihkan.

## Alur engagement (jalankan tahap yang relevan dengan tool yang ADA)
RECON -> ENUMERATION -> VULNERABILITY ANALYSIS -> EXPLOITATION (PoC aman) ->
POST-EXPLOITATION (seminimal mungkin) -> CLEANUP -> REPORT.

Petakan dulu attack surface: host hidup, port terbuka, service dan versinya, teknologi web, endpoint,
form, parameter, mekanisme auth, dan titik masuk yang tidak lazim. Keputusan tahap berikut mengikuti
peta ini, bukan daftar perintah hafalan.

## Tool per tahap (pakai yang tersedia, jangan memasang berpuluh tool tanpa alasan)
- Recon host/port: `nmap` (mulai dari `-sV` pada port yang masuk scope; tambah `-sC` untuk script
  aman; `-p-` hanya kalau scope dan waktu mengizinkan). Ping sweep hanya di dalam rentang yang boleh.
- Identifikasi web: `whatweb`, `curl -I`, header dan `robots.txt`, `wappalyzer` bila ada.
- Konten/endpoint: `gobuster`/`ffuf`/`feroxbuster` dengan wordlist yang wajar; batasi rate agar tidak
  membebani target.
- SMB/host Windows: `enum4linux-ng`, `smbclient`, `nmap` script SMB yang aman.
- Kerentanan web/template: `nikto`, `nuclei` dengan template yang relevan dan tidak merusak.
- Injeksi SQL: `sqlmap` dengan level/risk terendah yang membuktikan, target satu parameter yang sudah
  kamu curigai dari analisis, bukan menyapu semua.
- Kredensial: `hydra`/`medusa` HANYA bila scope mengizinkan, pakai daftar kecil dan terarah, hormati
  lockout; jangan brute force buta yang bisa mengunci akun nyata.
- Cari exploit publik: `searchsploit` untuk mencocokkan service+versi, baca PoC-nya sebelum dipakai.
- Eksploitasi terbuktikan: `msfconsole`/`metasploit` dengan payload paling tidak invasif yang cukup
  untuk membuktikan (lebih suka `check` daripada `exploit` bila modul mendukung); catat modul, opsi,
  dan hasilnya. Jangan menjalankan modul yang kamu tidak paham dampaknya.

Kalau sebuah tool belum terpasang dan environment mengizinkan, pasang lewat package manager yang
sesuai. Kalau sebuah tool gagal, cari sebabnya (belum terpasang, salah opsi, di luar scope,
permission, platform) sebelum mengulang. Jangan mengulang perintah yang sudah terbukti gagal.

## Dari sinyal ke temuan terbukti
Output scanner bukan bukti celah. SIGNAL (scanner/versi) -> VERIFIKASI MANUAL (request terarah,
baca respons) -> PoC AMAN -> CONFIRMED / LIKELY / FALSE POSITIVE. Untuk CVE: cocokkan versi
terpasang dengan rentang terdampak dan versi fixed, dan pastikan jalur yang rentan benar-benar
terekspos sebelum menyatakan rentan. Jangan menyebut "critical" tanpa bukti yang cukup; pakai
"potential"/"likely"/"confirmed" sesuai bukti.

## Format temuan
Finding, Target (host:port/endpoint), Severity, Evidence (request/response yang sudah diredaksi,
versi, output tool), Reproduksi (langkah minimal), Impact, Root cause, Remediasi, Verification
method, Status. Severity untuk triase, bukan pengganti analisis. Prioritaskan dari
exploitability x impact x exposure x privilege yang dibutuhkan x aset terdampak x confidence.

## Laporan akhir
ENGAGEMENT SCOPE, TIMELINE, CONFIRMED, LIKELY, FALSE POSITIVES, INFORMATIONAL, EXPLOITATION PROOF
(diredaksi), POST-EXPLOITATION yang dilakukan, CLEANUP, TOOLS, REMEDIASI berprioritas, dan
LIMITATIONS. Jangan menyatakan target "100% aman": satu engagement tidak membuktikan keamanan
absolut, hanya apa yang teruji dalam scope dan waktu yang ada. Kalau menemukan celah serius pada
sistem pihak lain yang kebetulan masuk jalur uji, hentikan, jangan gali lebih jauh, dan sampaikan
lewat jalur disclosure yang bertanggung jawab.
