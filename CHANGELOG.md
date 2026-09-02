# Changelog

Rilis dracxterm, versi terbaru di atas. Format tanggalnya YYYY-MM-DD.

`versionCode` adalah nomor internal Google Play. Ia hanya boleh naik, dan satu
angka tidak boleh dipakai ulang untuk unggahan yang berbeda.

## [1.0.0] - 2026-09-03

Rilis publik pertama. `applicationId` `com.xdrac`, `versionCode` 1, hanya untuk
perangkat `arm64-v8a` dengan Android 7.0 (API 24) atau lebih baru.

### Terminal

Mesin ANSI/VT, PTY, buffer layar, dan render UTF-8 ditulis dengan C++, tanpa
pustaka terminal pihak ketiga. Mendukung warna dan escape sequence ANSI/VT,
scrollback, layar alternatif, karakter lebar CJK dan combining mark, seleksi
teks, pencarian isi buffer, clipboard, bracketed paste, serta mouse tracking.

Sampai lima workspace bisa dibuka sekaligus, masing-masing dengan shell, PTY,
direktori kerja, dan riwayat sendiri; berpindah dengan usap. Foreground service
menjaga sesi tetap hidup saat aplikasi ditinggalkan.

Bar tombol tambahan di bawah keyboard menyediakan panah, gulir ke dasar, zoom,
Ctrl dan Alt yang bisa dikunci, Esc, Tab, Home, End, PgUp, PgDn, pencarian,
tempel, dan backspace. Ukuran font juga bisa diubah dengan cubit.

### Lingkungan Linux

Shell BusyBox ikut di dalam APK dan langsung bisa dipakai tanpa root. Distribusi
Linux ARM64 dijalankan lewat PRoot di dalam sandbox aplikasi.

Katalog `assets/rootfsURLS.json` menyediakan empat image untuk `arm64-v8a`:
Debian 13 (trixie) sebagai bawaan, serta Kali Linux nano, minimal, dan full
pentesting. Unduhan hanya dimulai setelah Anda memilih dan menyetujui, lalu
dicocokkan dengan SHA-256 yang dipin di dalam APK sebelum diekstrak.

Sebuah build bisa membawa image di dalam APK atau tidak. Yang menentukan adalah
isi `app/src/main/assets/rootfs/` saat build: satu arsip berarti image ikut
dikemas dan dipasang tanpa jaringan, tidak ada arsip berarti aplikasi menawarkan
katalog. Keduanya aplikasi yang sama dengan paket dan versi yang sama.

Ollama bisa dipasang dari dalam terminal atas permintaan Anda. Berkasnya tidak
ikut di dalam APK; yang diunduh adalah artefak rilis resmi Ollama v0.32.14-rc0,
yang juga dicocokkan SHA-256 sebelum dipasang.

### Pengaturan

Ketik `xset` di terminal untuk membuka pengaturan yang digambar di dalam
terminal itu sendiri, dengan pratinjau langsung: tema dan warna, font JetBrains
Mono atau font sistem, ukuran dan spasi teks, bentuk cursor, kedalaman
scrollback, padding, akses penyimpanan, informasi perangkat, serta ekspor dan
impor konfigurasi sebagai JSON. Setiap perubahan langsung tersimpan.

Antarmukanya tersedia dalam Bahasa Indonesia dan Inggris. Kedua terjemahan
selalu ikut terpasang, jadi toggle bahasanya bekerja tanpa unduhan tambahan.

### Penyimpanan dan privasi

Akses penyimpanan opsional dan tidak pernah diminta saat aplikasi dibuka. Kalau
diberikan, penyimpanan internal muncul di `~/sdcard` dan volume lepas-pasang di
`~/sdcard-1`. Kalau ditolak, semua fitur lain tetap berjalan.

Tidak ada analitik, iklan, pustaka pelacak, telemetri, atau akun. Aplikasi
membuka koneksi jaringan hanya ketika Anda memintanya mengunduh sesuatu.

### Yang ikut dikirim

APK memuat lima biner ARM64, semuanya dibangun dari sumber upstream yang dipin
oleh `prebuilts/build.sh`: BusyBox 1.38.0, PRoot v5.1.107.91 dan loader-nya,
talloc 2.5.0, dan libandroid-shmem v0.7. Versi, lisensi, dan written offer untuk
source code-nya ada di [`NOTICE`](NOTICE).

### Keterbatasan yang diketahui

- Hanya `arm64-v8a`. Perangkat `armeabi-v7a` dan x86 tidak didukung.
- `android:allowBackup` masih `true` tanpa aturan pengecualian, jadi data
  aplikasi termasuk isi rootfs dan riwayat shell bisa ikut terbawa backup atau
  transfer perangkat.
- Digest image Debian berasal dari mirror pihak ketiga dan sudah tidak bisa
  diturunkan ulang dari otoritas independen. Pin-nya tetap menahan substitusi,
  tapi rantai kepercayaannya lebih pendek daripada image Kali.
- Tidak ada unit test JVM dan tidak ada CI di repo ini. Verifikasi bersandar
  pada suite C++ di `native-tests/`, lint Android, dan pemeriksaan artefak.
- Ollama berjalan lewat PRoot, jadi kecepatannya di bawah biner native.
