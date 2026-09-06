# Changelog

Rilis dracxterm, versi terbaru di atas. Format tanggalnya YYYY-MM-DD.

`versionCode` adalah nomor internal Google Play. Ia hanya boleh naik, dan satu
angka tidak boleh dipakai ulang untuk unggahan yang berbeda.

## [1.0.1] - 2026-09-07

Rilis perbaikan. `versionCode` 2. Tidak ada perubahan pada mesin terminal,
antarmuka, maupun izin; yang berubah hanya dari mana image Debian diambil.

### Image Debian pindah sumber

Sampai versi 1.0.0 image Debian diambil dari `easycli.sh`. Operator situs itu
menghentikan hosting-nya. URL yang dipin di dalam APK sekarang menjawab
302 ke `/` dengan halaman pemberitahuan sebesar 43 byte, bukan arsip rootfs.

Pemasangan Debian karena itu gagal di versi 1.0.0. Gagalnya di tempat yang
benar: pin SHA-256 mencocokkan byte yang diterima, tidak cocok, lalu menolak
mengekstraknya. Tidak ada yang terpasang dan tidak ada sandbox yang tersentuh.
Tapi hasil akhirnya tetap sama bagi Anda, yaitu Debian tidak bisa dipasang.

Entri Debian sekarang menunjuk ke `images.linuxcontainers.org`, yang
menerbitkan `SHA256SUMS` di sebelah tiap build beserta signature OpenPGP
terpisah untuk arsip maupun berkas sums-nya. Digest yang dipin di dalam APK
jadi bisa diturunkan ulang dari otoritas independen, sesuatu yang tidak bisa
dilakukan sumber sebelumnya.

Image barunya lebih besar: unduhan naik dari 35 MB ke 90 MB, dan ruang
terpasang dari sekitar 400 MB ke sekitar 520 MB. Angka itu yang sekarang
ditampilkan di layar pemilihan distro.

Keempat entri Kali tidak berubah. URL, digest, dan ukurannya sama persis
seperti 1.0.0.

### Pemeriksaan katalog

`scripts/verify-rootfs-urls.sh` sebelumnya hanya mengenali tata letak
`SHA256SUMS` milik `kali.download`, jadi entri Debian selalu dilewati dengan
peringatan "digest not verified". Sekarang skrip itu juga mengenali tata letak
`images.linuxcontainers.org`, sehingga keempat entri katalog benar-benar
dicocokkan dengan sumbernya, bukan tiga dari empat.

### Keterbatasan baru yang perlu Anda tahu

Server image linuxcontainers hanya menyimpan build bertanggal sekitar tiga
hari. URL Debian di katalog akan berhenti resolve begitu build itu dirotasi
keluar, dan pemasangan Debian gagal lagi sampai katalognya dipindah ke mirror
sendiri. Digest-nya tetap berlaku di mana pun byte yang sama disajikan, jadi
pemindahan itu tidak mengubah apa yang dipasang.

Jalankan `./scripts/verify-rootfs-urls.sh` sebelum memotong rilis untuk tahu
apakah URL-nya masih hidup.

### Dokumentasi

`README.md` ditulis ulang di bagian yang menjelaskan sumber image, ditambah
penjelasan alur aplikasi dari layar pertama sampai shell siap, dan tutorial
build yang bisa diikuti dari mesin kosong. Screenshot-nya diganti dengan
delapan gambar baru yang diambil dari Kali Linux (nano) di Android 15.

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
