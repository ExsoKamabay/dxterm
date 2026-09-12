# Changelog

Rilis dracxterm, versi terbaru di atas. Format tanggalnya YYYY-MM-DD.

`versionCode` adalah nomor internal Google Play. Ia hanya boleh naik, dan satu
angka tidak boleh dipakai ulang untuk unggahan yang berbeda.

## [Belum dirilis]

`versionCode` belum dinaikkan. Perubahannya ada di katalog image, skrip
pemeriksaan, dan dokumentasi; mesin terminal, antarmuka, dan izin tidak
tersentuh.

### Keempat image pindah ke mirror sendiri

Katalog `assets/rootfsURLS.json` sekarang menunjuk ke aset rilis
`rootfs-20260913` di https://github.com/ExsoKamabay/rootless. Repositori itu
hanya menyimpan arsip rootfs beserta `SHA256SUMS`-nya, tanpa kode program.

Untuk ketiga entri Kali yang berubah cuma host-nya. Digest, ukuran unduhan, dan
perkiraan ruang terpasang sama persis dengan 1.0.3, dan ketiganya masih cocok
dengan `SHA256SUMS` yang diterbitkan Kali untuk kali-2026.2.

### Entri Debian kembali ke image 1.0.0

URL `images.linuxcontainers.org` yang dipin sejak 1.0.1 sudah mati. Server itu
menyimpan build bertanggal sekitar tiga hari saja, dan build
`20260904_05:24` menjawab 404 sekarang. Catatan di 1.0.1 sudah memperingatkan
ini akan terjadi.

Entri Debian kembali memakai `debian-trixie-aarch64-pd-v4.37.0.tar.xz`, build
proot-distro yang dipin proyek ini di 1.0.0, dengan digest
`9bd3b19ff7cd300c7c7bf33124b726eb199f4bab9a3b1472f34749c6d12c9195`. Unduhannya
turun dari 90 MB ke 35 MB dan perkiraan ruang terpasang dari 520 MB ke 400 MB,
angka yang dipakai 1.0.0 untuk image yang sama.

Digest itu tidak bisa lagi diturunkan ulang dari pihak luar. `easycli.sh` yang
dulu menerbitkannya sudah menutup hosting. Yang menopangnya sekarang adalah pin
yang dibawa proyek ini sejak rilis pertama, dan byte di mirror cocok dengan pin
itu.

### Skrip pemeriksaan tahu letak SHA256SUMS mirror

`scripts/verify-rootfs-urls.sh` memetakan URL aset rilis GitHub ke
`SHA256SUMS` di branch main repositori yang sama, karena mirror menerbitkan satu
berkas untuk semua image alih-alih satu per direktori rilis. Tanpa itu keempat
entri akan lewat dengan peringatan "no known checksum layout for this host".

## [1.0.3] - 2026-09-11

Perubahan lisensi. `versionCode` 4. Kode aplikasi sekarang berlisensi GNU
General Public License versi 3, menggantikan Apache License 2.0. Ini menyamakan
lisensi aplikasi dengan biner GPL yang ikut dibundel, yaitu BusyBox dan PRoot.
Berkas `LICENSE`, `NOTICE`, dan bagian lisensi di `README.md` sudah diperbarui.
Biner pihak ketiga tetap memakai lisensinya sendiri; tidak ada yang direlisensi.

Tidak ada perubahan pada mesin terminal, antarmuka, izin, maupun katalog image.

## [1.0.2] - 2026-09-10

Rilis perkakas build. `versionCode` 3. Mesin terminal, antarmuka, izin, dan
katalog image tidak berubah sama sekali. Yang berubah adalah apa yang ikut
ditulis ke dalam APK, dan satu skrip pemeriksaan pra-rilis yang ternyata
berhenti di tengah jalan tanpa bilang apa-apa.

### Blob dependency Google tidak lagi ikut dikemas

Android Gradle Plugin menulis deskripsi pohon dependency ke dalam signing block
APK, dienkripsi dengan kunci publik milik Google. Tidak ada pihak di luar Google
yang bisa membacanya kembali, jadi tidak ada peninjau maupun pengguna yang bisa
memeriksa apa yang sebenarnya dinyatakan di sana.

`dependenciesInfo` sekarang mematikannya untuk APK maupun App Bundle. Build
tidak membutuhkan isinya: satu-satunya yang membaca blob itu adalah Play, dan
aplikasi ini tidak disalurkan lewat sana.

### Pemeriksaan katalog berhenti diam-diam

`scripts/verify-rootfs-urls.sh` berjalan dengan `set -e` dan `pipefail`. Ketika
sebuah host memangkas `SHA256SUMS`-nya tapi tetap menyajikan arsipnya, `grep`
yang tidak menemukan apa pun menggagalkan seluruh pipeline, dan gagalnya
substitusi perintah di dalam assignment ikut menggagalkan assignment itu. Skrip
keluar di tengah tanpa mencetak sebaris pun.

Itulah yang terjadi pada entri Debian. Direktori build bertanggalnya masih
menyajikan `rootfs.tar.xz`, tapi `SHA256SUMS` di sebelahnya sudah 404. Skrip
berhenti di entri pertama, dan tiga entri Kali tidak pernah diperiksa. Dari
layar, hasilnya terlihat seperti pemeriksaan yang lolos.

Digest yang tidak terbaca sekarang kembali jadi peringatan seperti yang
dirancang, dan keempat entri selalu diperiksa sampai habis.

### Keadaan katalog saat rilis ini dipotong

Keempat entri terjangkau dan ukurannya cocok. Ketiga image Kali cocok dengan
`SHA256SUMS` upstream. Digest Debian sudah tidak bisa diturunkan ulang dari
upstream karena `SHA256SUMS` build itu dihapus, tapi byte yang disajikan masih
sama persis dengan yang dipin di dalam APK. Itu diperiksa dengan mengunduh ulang
arsipnya dan menghitung SHA-256-nya sendiri sebelum rilis ini dipotong.

Keterbatasan yang dicatat di 1.0.1 masih berlaku. Server linuxcontainers
merotasi build bertanggal, jadi URL Debian akan berhenti resolve dan pemasangan
Debian gagal lagi sampai katalognya dipindah ke mirror sendiri.

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
