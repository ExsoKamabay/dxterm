# dracxterm

Emulator terminal untuk Android arm64 dan x86_64. Mesin ANSI/VT, PTY, dan render
UTF-8-nya ditulis dari nol dengan C++; antarmukanya Kotlin. Tidak perlu root.

Bisa dipakai apa adanya sebagai shell BusyBox, atau menjalankan distribusi Linux
di dalam sandbox aplikasi. Sesi Linux dijalankan VHDP, sebuah rootless supervisor
berbasis `ptrace` (dipercepat `seccomp`) yang ikut di dalam APK; PRoot tetap
dikirim sebagai fallback dan dipakai otomatis kalau VHDP gagal self-test di
perangkat itu. Tujuannya satu: memberi shell yang benar di ponsel, tanpa meminta
root, akun, atau layanan apa pun.

## Screenshot

Diambil dari APK release 1.0.5 di WayDroid (Android 13, x86_64), layar logis
1080x2400, dengan Debian 13 amd64 yang dipasang dari katalog. Keterangan tiap
gambar ada di
[`Screenshot/deskripsi-screenshot-01-08-dracxterm.txt`](Screenshot/deskripsi-screenshot-01-08-dracxterm.txt).

| | |
|---|---|
| ![Pemilihan distro](Screenshot/01-pilih-distro-dracxterm.jpg) | ![Pemasangan distro](Screenshot/02-pasang-distro-dracxterm.jpg) |
| 1. Layar pertama. Aplikasi membaca ABI perangkat dan hanya menampilkan image yang cocok, dengan ukuran unduhan dan ruang yang dibutuhkan. | 2. Pemasangan berjalan. Arsipnya sudah lolos pemeriksaan SHA-256 dan sedang diekstrak, dengan pratinjau berkas yang sedang ditulis. |
| ![Debian berjalan](Screenshot/03-terminal-debian-dracxterm.jpg) | ![Warna dan UTF-8](Screenshot/04-warna-utf8-dracxterm.jpg) |
| 3. Debian 13 berjalan lewat VHDP, tanpa root: `fastfetch`, `sudo apt-get install`, sesi root, git, Python, dan curl. | 4. 16 warna ANSI, palet 256 warna, truecolor 24-bit, atribut teks, dan UTF-8 penuh termasuk CJK, emoji, dan combining mark. |
| ![Superuser dan paket .deb](Screenshot/05-superuser-deb-dracxterm.jpg) | ![htop di workspace kedua](Screenshot/06-htop-multi-workspace-dracxterm.jpg) |
| 5. `sudo -i` menjadi root, lalu paket `.deb` diunduh, dipasang dengan `dpkg -i`, dijalankan, dan dihapus. `exit` kembali ke user biasa. | 6. `htop` layar penuh di workspace kedua. Tiap workspace punya shell dan PTY sendiri. |
| ![xset Appearance](Screenshot/07-xset-appearance-dracxterm.jpg) | ![xset Diagnostics](Screenshot/08-xset-diagnostics-vhdp-dracxterm.jpg) |
| 7. `xset`, pengaturan yang digambar di dalam terminal. Halaman Appearance merangkum tema, font, kursor, dan padding. | 8. Halaman Diagnostics, baru di 1.0.5: versi libvhdp, backend yang menjalankan sesi, dan hasil probe aktif. |

## Yang baru di 1.0.5

Rilis ini mengganti mesin yang menjalankan distro Linux, menambah dukungan
x86_64, dan menutup sejumlah bug yang terasa langsung di terminal. Rinciannya
ada di [`CHANGELOG.md`](CHANGELOG.md).

Sampai 1.0.3, shell di dalam distro ditopang PRoot. Sekarang yang menjalankannya
adalah VHDP, engine rootless berbasis `ptrace` yang dipercepat `seccomp`,
dikirim di dalam APK sebagai `libphdp.so` dan loader ELF `libvhdp-loader.so`.
Sekali per pemasangan, dan lagi setelah aplikasi diperbarui, aplikasi
menjalankan self-test VHDP di perangkat itu. Kalau lolos, sesi memakai VHDP;
kalau gagal, sesi tetap berjalan di PRoot seperti dulu, tanpa ada yang perlu
diatur. VHDP juga menyediakan ulang hal-hal yang ditolak kebijakan Android di
dalam sandbox aplikasi: hardlink untuk dpkg, `tar`, dan `cp -al`, berkas `/proc`
global untuk `uptime`, `ps`, dan `vmstat`, serta socket audit yang dibutuhkan
`useradd`.

APK kini memuat biner untuk `arm64-v8a` dan `x86_64`, jadi satu APK yang sama
bisa dipasang di ponsel, emulator Android, WayDroid, dan ChromeOS. Katalog
menambah Debian 13 amd64 untuk x86_64.

Di dalam distro ada perintah `vhdp doctor`, `vhdp inspect`, dan
`vhdp capabilities`. Ketik `xset vhdp` untuk membuka halaman Diagnostics, yang
menampilkan backend yang sedang dipakai dan bisa menjalankan probe aktif.

Beberapa bug terminal ikut ditutup. `tar -cf` tidak lagi gagal dengan ENOSYS.
SIGQUIT, SIGUSR1, dan SIGPIPE kembali sampai ke program di dalam distro, jadi
`trap` bekerja dan penulis pipa yang kehilangan pembacanya berhenti dengan
status 141. `vhdp doctor` tidak lagi merusak `/tmp`. Di ponsel sungguhan, distro
kembali punya `/dev/null`, `/dev/urandom`, dan `/dev/tty`. `Ctrl+Z` dan proses
latar yang menyentuh tty berperilaku seperti di Linux biasa.

Uji rilis di perangkat menemukan tiga masalah lagi. Bahasa yang dipilih pengguna
tidak lagi berganti ke bahasa perangkat setelah layar diputar. Pesan tahap
pemasangan ("Mengekstrak… N berkas", "Linux siap", dan lainnya) kini tersedia
dalam bahasa Indonesia dan Inggris. `xset` menampilkan warna preset sebagai
`#RRGGBB`, bukan angka ARGB mentah.

Kelima image di katalog, empat untuk arm64 dan satu untuk x86_64, diambil dari
aset rilis [`ExsoKamabay/rootless`](https://github.com/ExsoKamabay/rootless).
Entri Debian arm64 kembali ke image yang dipin di 1.0.0, unduhan 35 MB, karena
URL yang dipakai sejak 1.0.1 sudah mati.

Folder `app/src/test` dan `app/src/androidTest` dihapus beserta dependensi
tesnya. Rilis diuji dengan membangun ulang APK release lalu mencobanya di
perangkat sambil membaca logcat. Caranya ada di
[Langkah 6](#langkah-6-uji-di-perangkat).

Nomor versinya 1.0.5 dengan `versionCode` 5. Nomor 1.0.4 dilewati dan tidak
pernah dipakai untuk build yang beredar.

## Yang bisa dilakukan

Terminalnya mendukung warna dan escape sequence ANSI/VT, scrollback, layar
alternatif, serta UTF-8 penuh termasuk karakter lebar CJK dan combining mark.
Ada seleksi teks, pencarian isi buffer, clipboard, bracketed paste, dan mouse
tracking untuk program yang memerlukannya.

Sampai lima workspace bisa dibuka sekaligus, masing-masing punya shell, PTY,
direktori kerja, dan riwayat sendiri. Berpindah antar workspace dengan usap.
Sesi tetap hidup saat aplikasi ditinggalkan, dijaga oleh foreground service.

Di bawah keyboard ada bar tombol tambahan yang bisa digulir: panah, gulir ke
dasar, zoom, Ctrl dan Alt yang bisa dikunci, Esc, Tab, Home, End, PgUp, PgDn,
pencarian, tempel, dan backspace. Cubit layar untuk mengubah ukuran font.

Ketik `xset` di terminal untuk membuka pengaturan. Semuanya digambar di dalam
terminal itu sendiri, dengan pratinjau langsung, dan tersimpan otomatis setiap
kali diubah:

| Menu | Isinya |
|---|---|
| Appearance | Ringkasan pengaturan yang paling sering dipakai, dalam satu layar. |
| Theme | Preset tema, warna foreground, background, dan cursor. |
| Font | JetBrains Mono atau font sistem, ukuran 7 sampai 28 dp, spasi baris, spasi huruf, padding, dan opsi bold sebagai warna terang. |
| Cursor | Bentuk block, bar, underline, atau hollow; kedip; warna. |
| Background | Warna latar dan kontras foreground-nya. |
| Performance | Kedalaman scrollback, 200 sampai 20000 baris. |
| Storage Access | Sakelar akses penyimpanan, berlaku langsung di shell yang sedang jalan. |
| Diagnostics | Versi libvhdp, ABI, arsitektur build, profil yang terdeteksi, kebijakan exec storage, backend yang dipakai sesi ini, dan probe aktif. |
| Backup | Ekspor dan impor konfigurasi sebagai JSON, serta reset ke bawaan. |
| About | Informasi perangkat dan aplikasi, serta alamat kontak pengembang. |

Di dalam guest ada perintah `vhdp`: `vhdp doctor` untuk diagnostik host,
`vhdp inspect <rootfs>` untuk memeriksa sebuah rootfs, dan `vhdp capabilities`
untuk matriks kemampuan per profil. `vhdp run` ditolak dari dalam terminal,
karena Linux hanya mengizinkan satu tracer per proses dan shell itu sudah
ditrace oleh sesi yang sedang berjalan.

Di dalam distro Anda login sebagai user biasa `dracos`. `sudo PERINTAH`,
`su -c PERINTAH`, dan `fakeroot PERINTAH` menjalankan perintah sebagai root;
`sudo -i`, `sudo -s`, `su`, `su -`, dan `sudo su` membuka shell root dengan
prompt `#`, dan `exit` kembali ke `dracos`. Root di sini diemulasikan di dalam
sandbox aplikasi, bukan root perangkat, tetapi cukup untuk `apt-get install`,
`dpkg -i paket.deb`, `apt install ./paket.deb`, menulis ke `/etc` atau `/usr`,
dan `chown`. Perangkat tidak perlu di-root.

Antarmukanya tersedia dalam Bahasa Indonesia dan Inggris. Toggle bahasanya ada
di dalam aplikasi, jadi kedua terjemahan selalu ikut terpasang.

Akses penyimpanan opsional dan tidak pernah diminta saat aplikasi dibuka. Kalau
diberikan, penyimpanan internal muncul di `~/sdcard`, dan volume lepas-pasang di
`~/sdcard-1` kalau memang ada. Kalau ditolak, semuanya tetap jalan.

Ollama bisa dipasang dari dalam terminal kalau Anda memintanya. Berkasnya tidak
ikut di dalam APK; yang diunduh adalah artefak rilis resmi Ollama v0.32.14-rc0,
sekitar 1,5 GB, yang dicocokkan SHA-256 sebelum dipasang.

Tidak ada analitik, iklan, pelacak, atau telemetri. Aplikasi membuka koneksi
hanya kalau Anda memintanya mengunduh sesuatu.

## Alur aplikasi

Bagian ini menjelaskan apa yang terjadi antara Anda menekan ikon aplikasi dan
sebuah prompt muncul. Berguna kalau ada yang gagal dan Anda ingin tahu di
langkah mana.

### Peluncuran pertama

Layar yang terbuka lebih dulu bukan terminal, melainkan layar provisioning. Ia
menyiapkan lingkungan Linux di belakang layar progres, dan menahan gestur back
selama proses berjalan supaya tidak ada instalasi yang terputus di tengah.

Yang pertama diperiksa: apakah sudah ada rootfs yang terpasang dan masih utuh.
Kalau ada, semua langkah di bawah dilewati dan aplikasi langsung membuka
terminal.

Kalau belum ada, langkah berikutnya ditentukan oleh apakah build ini membawa
image di dalam APK atau tidak. Jawabannya dibaca dari isi `assets/rootfs/` di
dalam APK, bukan dari nama berkas APK atau flag build:

- **Ada arsip di dalam APK.** Arsipnya langsung diekstrak. Tidak ada daftar
  distro, tidak ada panel persetujuan, dan tidak ada jaringan yang disentuh.
- **Tidak ada arsip.** Aplikasi menampilkan katalog dari
  `assets/rootfsURLS.json`, sudah disaring sesuai ABI perangkat, lengkap dengan
  ukuran unduhan dan ruang yang dibutuhkan tiap pilihan.

Sebelum itu, aplikasi juga memeriksa apakah ada arsip yang sudah pernah Anda
unduh dan tertinggal di direktori aplikasi. Kalau ada, pemasangan dilanjutkan
dari situ tanpa mengunduh ulang.

### Panel persetujuan

Tidak ada yang diunduh sampai Anda menekan tombolnya. Mengunduh adalah
satu-satunya alasan aplikasi ini membuka koneksi jaringan.

Menolak adalah pilihan yang sah, bukan kondisi error. Kalau Anda menolak,
aplikasi membuka terminal dengan shell BusyBox yang ikut di dalam APK, dan
terminal itu berfungsi penuh. Katalog distro bisa dibuka lagi kapan saja nanti.

### Unduhan

Unduhan hanya berjalan lewat HTTPS. Redirect yang menurunkan protokol ke
plaintext ditolak, tidak diikuti, karena berkas yang diunduh akan menjadi kode
yang dieksekusi di dalam sandbox Anda.

Byte-nya ditulis ke berkas `.part`, bukan langsung ke nama akhirnya. Nama akhir
baru dipakai setelah SHA-256 berkas lengkapnya cocok dengan digest yang dipin di
dalam APK. Akibatnya transfer yang terputus tidak pernah bisa disalahartikan
sebagai image yang siap pakai di peluncuran berikutnya.

Unduhan bisa dijeda dan dilanjutkan. Berkas `.part` yang tertinggal disambung
dengan Range request kalau server mendukungnya, dan diulang dari nol kalau
tidak. Menekan batal memutus koneksi saat itu juga, tanpa menunggu buffer yang
sedang dibaca selesai.

Kalau digest tidak cocok, arsipnya ditolak dan tidak ada yang diekstrak.

### Ekstraksi dan konfigurasi

Arsip yang lolos verifikasi diekstrak ke direktori privat aplikasi, lalu
dikonfigurasi: pengguna non-root di dalam guest, resolver DNS, dan titik masuk
shell-nya. Setelah itu isinya diperiksa sekali lagi, kali ini untuk memastikan
yang terpasang memang bisa dijalankan, bukan sekadar berhasil diekstrak.

Setelah ekstraksi sukses, arsip unduhannya dihapus untuk mengembalikan ruang.

Ada satu langkah pemulihan yang berjalan otomatis pada distro berbasis dpkg.
Karena login interaktif di dalam guest bukan root, state `apt` dan `dpkg` bisa
berakhir dimiliki user biasa, dan `dpkg` lalu menolak menulisnya bahkan lewat
`sudo`. Aplikasi merapikan kepemilikan itu, membersihkan lock yang tertinggal,
dan menyelesaikan transaksi yang sempat terputus. Langkah ini idempoten dan
tidak pernah menggagalkan boot; kalau gagal, terminal tetap terbuka dan
catatannya ditulis ke log di direktori aplikasi.

### Terminal

Sebelum shell dijalankan, aplikasi menyiapkan lingkungannya: symlink pustaka
yang dibutuhkan biner bawaan APK, dan beberapa ratus link applet BusyBox.
Link itu dibuat ulang setelah aplikasi diperbarui, karena pembaruan mengganti
nama direktori tempat biner-binernya tinggal, dan link lama akan menunjuk ke
path yang sudah tidak ada.

Perintah bawaannya `busybox ash`. Aplikasi berpindah sendiri ke rootfs begitu
distro terpasang, jadi tidak ada yang perlu Anda atur untuk pindah dari BusyBox
ke Debian atau Kali.

Backend yang menjalankan sesi dipilih sekali per pemasangan. Aplikasi menjalankan
self-test VHDP di perangkat itu (engine rootless, userland loader, identitas yang
diemulasikan, proyeksi `/dev`), dan hasilnya disimpan di
`files/.guest-backend` bersama path direktori pustaka native. VHDP dipakai kalau
self-test lolos, PRoot kalau tidak, dan keputusannya ditulis ke logcat sebagai
`[GUEST] backend for ...`. Hapus berkas penanda itu untuk memaksa keputusan
dievaluasi ulang di peluncuran berikutnya.

Jalur eksekusinya menghindari larangan W^X Android (sejak API 29 aplikasi tidak
boleh meng-`execve` berkas di penyimpanannya sendiri yang bisa ditulis). Yang
di-`execve` hanya biner di direktori pustaka native, yaitu `libphdp.so`. Program
di dalam rootfs dimuat oleh `libvhdp-loader.so`, sebuah loader ELF userland yang
memetakan program guest dengan `mmap(PROT_EXEC)`.

Sesi yang sudah jalan dijaga foreground service, sehingga shell dan PTY-nya
tetap hidup saat Anda pindah ke aplikasi lain.

### Ringkasan cabangnya

| Kondisi | Yang terjadi |
|---|---|
| Rootfs sudah ada dan utuh | Langsung ke terminal Linux |
| APK membawa image | Ekstrak offline, lalu terminal Linux |
| Arsip sudah pernah diunduh | Pasang dari arsip itu, lalu terminal Linux |
| Tidak ada arsip, belum ditolak | Tampilkan katalog dan tunggu keputusan Anda |
| Tidak ada arsip, tawaran ditolak | Terminal BusyBox, berfungsi penuh |
| Ada arsip tapi gagal dipasang | Berhenti dengan alasan yang jelas |

## Yang dibutuhkan

- Android 7.0 (API 24) atau lebih baru.
- Prosesor arm64 (`arm64-v8a`) atau x86_64. APK memuat biner untuk kedua ABI itu;
  perangkat 32-bit (`armeabi-v7a` dan x86) tidak didukung.
- Ruang penyimpanan sesuai distro yang dipilih. Debian 13 butuh sekitar 400 MB
  setelah terpasang, Kali versi lengkap sekitar 9,5 GB.

Tidak butuh akun, layanan Google, atau akses root.

### Dependensi

Mesin terminalnya tidak memakai pustaka pihak ketiga. Parser ANSI/VT, buffer
layar, PTY, dan penanganan charset-nya ada di `app/src/main/cpp/`. Di sisi
Kotlin, aplikasi memakai:

| Pustaka | Untuk apa |
|---|---|
| `androidx.core:core-ktx`, `androidx.appcompat:appcompat` | Dasar activity, resource, dan pemilihan bahasa per aplikasi. |
| `com.google.android.material:material` | Komponen antarmuka. |
| `org.apache.commons:commons-compress` | Membaca arsip `.tar` saat mengekstrak rootfs. |
| `org.tukaani:xz` | Dekompresi `.tar.xz`, format semua image rootfs. |
| `com.github.luben:zstd-jni` | Dekompresi `.tar.zst`, format artefak rilis Ollama. |

Versi tiap pustaka ada di [`app/build.gradle.kts`](app/build.gradle.kts).

Tujuh biner prebuilt ikut dikirim per ABI: BusyBox, PRoot dan loader-nya,
talloc, libandroid-shmem, CLI VHDP (`libphdp.so`), dan userland loader VHDP
(`libvhdp-loader.so`). Rinciannya di
[Biner yang ikut dikirim](#biner-yang-ikut-dikirim). Di samping itu ada tiga
pustaka yang dibangun dari sumber di repo ini saat build: `libxterm.so` (mesin
terminal), `libvhdp.so`, dan `libvhdpjni.so` (jembatan JNI ke libvhdp).

## Versi dan paket

Rilis saat ini 1.0.5, `versionCode` 5, dengan `applicationId` `com.xdrac`.
Nomor 1.0.4 dilewati. APK 1.0.5 ditandatangani dengan kunci yang sama dengan
rilis 1.0.3 di GitHub, jadi bisa dipasang sebagai pembaruan tanpa menghapus
data.

Riwayat perubahan tiap rilis ada di [`CHANGELOG.md`](CHANGELOG.md).

## Unduh dan pasang

Ambil APK dari [Releases](https://github.com/ExsoKamabay/dxterm/releases).
Tiap berkas punya `.sha256` di sebelahnya. Periksa dulu sebelum memasang:

```bash
sha256sum -c dracxterm-1.0.5-vc5-release.apk.sha256
```

Lalu pasang lewat `adb`:

```bash
adb install -r dracxterm-1.0.5-vc5-release.apk
```

Atau salin APK ke perangkat dan buka dari file manager. Android akan meminta
izin memasang aplikasi dari sumber tidak dikenal.

## Memasang distro Linux

Saat pertama dibuka, aplikasi menampilkan daftar image yang cocok dengan ABI
perangkat, beserta ukuran unduhan dan ruang yang dibutuhkan. Setelah Anda
memilih dan menyetujui, aplikasi mengunduh arsipnya, mencocokkan SHA-256 dengan
digest yang sudah dipin di dalam APK, lalu mengekstraknya. Kalau digest tidak
cocok, arsip ditolak dan tidak diekstrak.

Sebelum ada distro yang terpasang, terminal tetap bisa dipakai memakai shell
BusyBox yang ikut di dalam APK.

Empat image tersedia untuk `arm64-v8a`:

| Distro | Unduhan | Terpasang | Asal image |
|---|---|---|---|
| Debian 13 (trixie) | 35 MB | 400 MB | build proot-distro |
| Kali Linux (nano) | 198 MB | 1,0 GB | Kali NetHunter 2026.2 |
| Kali Linux (minimal) | 137 MB | 1,0 GB | Kali NetHunter 2026.2 |
| Kali Linux (full pentesting) | 1,8 GB | 9,5 GB | Kali NetHunter 2026.2 |

Keempatnya diunduh dari satu tempat, yaitu aset rilis
[`ExsoKamabay/rootless`](https://github.com/ExsoKamabay/rootless), mirror milik
proyek ini. Repositori itu hanya menyimpan arsip rootfs, tanpa kode program.

Untuk `x86_64` ada satu entri, Debian 13 (trixie) amd64, unduhan 89 MB dan
sekitar 700 MB setelah terpasang. Entri itu ditandai uji coba dan dipakai untuk
emulator, WayDroid, atau ChromeOS. Arsipnya build `20260919_05:24` dari
images.linuxcontainers.org yang dicerminkan ke rilis `rootfs-20260920` di mirror
yang sama, karena host asalnya hanya menyimpan build bertanggal sekitar tiga
hari. `SHA256SUMS` asli build itu beserta tanda tangan GPG-nya ikut disimpan di
rilis tersebut.

Debian 13 adalah pilihan default. URL, digest, dan catatan tiap image ada di
[`rootfsURLS.json`](app/src/main/assets/rootfsURLS.json), termasuk penjelasan
asal-usul tiap sumber.

Mirror itu menerbitkan `SHA256SUMS` di branch main-nya, dan tiga digest Kali di
dalamnya masih sama dengan yang diterbitkan Kali untuk kali-2026.2. Sebelum
memotong rilis, jalankan pemeriksaannya:

```bash
./scripts/verify-rootfs-urls.sh
```

Skrip itu membaca katalognya, memastikan tiap URL masih hidup dan ukurannya
belum berubah, lalu membandingkan tiap digest dengan `SHA256SUMS` milik mirror.
Aset rilis tidak dirotasi keluar seperti build bertanggal di server
linuxcontainers, yang dua kali membuat entri Debian mati. Digest tetap berlaku
di mana pun byte yang sama disajikan, jadi pindah host tidak mengubah apa yang
dipasang di perangkat.

## Izin Android

| Izin | Dipakai untuk |
|---|---|
| `INTERNET` | Mengunduh image rootfs yang Anda pilih, dan Ollama kalau Anda memintanya. |
| `ACCESS_NETWORK_STATE` | Memeriksa ada tidaknya jaringan sebelum unduhan dimulai. |
| `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_SPECIAL_USE` | Menjaga proses aplikasi, dan karenanya shell serta PTY-nya, tetap hidup saat Anda pindah ke aplikasi lain. |
| `POST_NOTIFICATIONS` | Hanya menentukan apakah notifikasi service terlihat. Tidak pernah diminta saat aplikasi dibuka; service dan shell-nya tidak bergantung pada izin ini. |
| `READ_EXTERNAL_STORAGE` (sampai API 32), `WRITE_EXTERNAL_STORAGE` (sampai API 29) | Akses penyimpanan bersama pada Android lama. Diabaikan atau otomatis di-scope pada API yang lebih baru. |
| `MANAGE_EXTERNAL_STORAGE` | Izin khusus, opsional, dan hanya aktif kalau Anda menyalakannya sendiri lewat Settings. Tujuannya supaya terminal bisa menjangkau penyimpanan bersama lewat path, seperti terminal Linux pada umumnya. Aplikasi tetap berfungsi penuh kalau izin ini ditolak. |

`MANAGE_EXTERNAL_STORAGE` adalah izin yang paling luas di daftar ini. Aplikasi
tidak pernah memintanya saat dibuka, dan tidak ada fitur yang mati tanpa izin
itu. Yang hilang hanya kemampuan membaca dan menulis berkas di luar direktori
milik aplikasi.

## Build

Bagian ini bisa diikuti dari mesin Linux yang belum pernah menyentuh Android
sama sekali. Build-nya memakai Gradle wrapper apa adanya, tanpa skrip atau flag
tambahan. Urutannya: pasang perkakas, coba build debug, siapkan keystore,
naikkan versi, build release dengan `./gradlew clean` lalu
`./gradlew assembleRelease`, kemudian uji APK-nya di perangkat.

### Langkah 1: perkakas

JDK 17 dan Android SDK command-line tools. Android Studio tidak wajib.

```bash
java -version        # harus 17
```

Kalau `sdkmanager` belum ada, unduh command-line tools dari halaman Android
Studio, buka isinya, lalu tunjuk `ANDROID_HOME` ke folder SDK Anda:

```bash
export ANDROID_HOME="$HOME/Android/Sdk"
export PATH="$PATH:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools"
```

Pasang komponen yang dibutuhkan build ini, ditambah `platform-tools` untuk
`adb`:

```bash
sdkmanager "platforms;android-36" "build-tools;35.0.0" \
           "ndk;27.0.12077973" "cmake;3.22.1" "platform-tools"
```

Versi NDK dan CMake dipin di [`app/build.gradle.kts`](app/build.gradle.kts),
jadi mesin siapa pun menghasilkan kode native yang sama. Angka versinya harus
persis; `sdkmanager` tidak akan memilihkan yang terdekat.

Simpan `ANDROID_HOME` supaya tidak hilang tiap kali shell ditutup, entah di
`~/.bashrc` atau di `local.properties` pada root repo:

```properties
sdk.dir=/home/anda/Android/Sdk
```

### Langkah 2: ambil sumbernya dan build debug

```bash
git clone https://github.com/ExsoKamabay/dxterm.git
cd dxterm
./gradlew assembleDebug
```

Build debug tidak butuh keystore, jadi ini cara tercepat memastikan JDK, SDK,
NDK, dan CMake sudah terpasang benar. Hasilnya
`app/build/outputs/apk/debug/app-debug.apk`.

Build pertama memakan waktu karena mesin terminal C++ dan VHDP dikompilasi dari
nol untuk dua ABI. Build berikutnya jauh lebih cepat.

Ada satu varian saja: `assembleDebug`, `assembleRelease`, dan `bundleRelease`.
Tidak ada product flavor.

Build debug dan build release ditandatangani dengan kunci yang berbeda, dan
Android menolak memasang yang satu di atas yang lain. Pindah jenis build berarti
`adb uninstall com.xdrac` lebih dulu, dan itu menghapus rootfs beserta isi home
di dalam distro. Cadangkan dulu isi home kalau masih dibutuhkan.

### Langkah 3: siapkan keystore

Build release menolak jalan tanpa keystore, dan keystore-nya tidak boleh ada di
dalam repository. Kalau belum punya, buat satu di luar working tree:

```bash
mkdir -p ~/.android-keys
keytool -genkeypair -v \
        -keystore ~/.android-keys/dracxterm-release.jks \
        -alias dracxterm \
        -keyalg RSA -keysize 4096 -validity 10000
chmod 600 ~/.android-keys/dracxterm-release.jks
```

Simpan keystore itu dan kata sandinya baik-baik. Android memakai kunci
penandatangan sebagai identitas aplikasi: kalau kunci itu hilang, tidak ada
pembaruan yang bisa dipasang di atas versi yang sudah beredar.

Kredensialnya dibaca dari `~/.gradle/gradle.properties`, bukan dari repository:

```properties
DRACOS_STORE_FILE=/home/anda/.android-keys/dracxterm-release.jks
DRACOS_STORE_PASSWORD=...
DRACOS_KEY_ALIAS=dracxterm
DRACOS_KEY_PASSWORD=...
```

```bash
chmod 600 ~/.gradle/gradle.properties
```

Keempat properti itu juga bisa dikirim sebagai environment variable
`ORG_GRADLE_PROJECT_DRACOS_STORE_FILE` dan seterusnya. Kalau salah satu kosong,
Gradle menghentikan build release dan menyebut properti mana yang hilang.

Rilis ditandatangani dengan skema v2 dan v3, tanpa v1. Skema v3 membawa
certificate lineage, satu-satunya jalan merotasi kunci tanpa memutus pembaruan
untuk perangkat yang sudah memasang aplikasi.

### Langkah 4: naikkan versi

Untuk rilis baru, ubah dua angka di
[`app/build.gradle.kts`](app/build.gradle.kts):

```kotlin
versionCode = 5
versionName = "1.0.5"
```

Itu nilai yang ada di berkas sekarang; naikkan keduanya untuk rilis berikutnya.

`versionCode` hanya boleh naik dan tidak boleh dipakai ulang untuk unggahan
berbeda. Catat perubahannya di [`CHANGELOG.md`](CHANGELOG.md) dan di
`fastlane/metadata/android/*/changelogs/<versionCode>.txt`, lalu jalankan
`python3 scripts/verify-metadata.py` untuk memastikan changelog itu ada dan
panjangnya masih di bawah batas.

### Langkah 5: build APK release

```bash
./gradlew clean
./gradlew assembleRelease
```

Sebelum mengompilasi, Gradle memeriksa tiga hal dan berhenti kalau salah satunya
gagal: keempat properti `DRACOS_*` terisi, berkas keystore-nya ada, dan semua
biner prebuilt di `jniLibs` ber-align 16 KB. Log build mencetak
`[align] all prebuilts (all ABIs) are >= 16 KB-aligned` dan `[rootfs] ...` yang
menyebut jenis installer yang sedang dibangun.

Di mesin pengembang, build release dari keadaan bersih selesai sekitar dua
menit. Hasilnya `app/build/outputs/apk/release/app-release.apk`, sekitar 14 MB,
sudah ditandatangani dan sudah lewat `lintVitalRelease`. Periksa sebelum
dipakai:

```bash
APK=app/build/outputs/apk/release/app-release.apk
BT="$ANDROID_HOME/build-tools/35.0.0"
"$BT/apksigner" verify --print-certs "$APK"
"$BT/zipalign" -c -p 4 "$APK"
"$BT/aapt2" dump badging "$APK" | grep -E "^package|native-code"
```

`apksigner` harus melaporkan skema v2 dan v3 sebagai `true`, dan `zipalign`
harus keluar tanpa pesan. `aapt2` harus menunjukkan `versionCode='5'`,
`versionName='1.0.5'`, dan `native-code: 'arm64-v8a' 'x86_64'`.

Untuk dilampirkan ke GitHub Release, beri nama berversi dan buat berkas
checksum-nya:

```bash
mkdir -p apps
cp "$APK" apps/dracxterm-1.0.5-vc5-release.apk
(cd apps && sha256sum dracxterm-1.0.5-vc5-release.apk > dracxterm-1.0.5-vc5-release.apk.sha256)
```

`apps/` ada di `.gitignore` dan tidak pernah ikut ter-commit.

### Langkah 6: uji di perangkat

Setiap perubahan diuji dengan membangun ulang APK lalu mencobanya di perangkat
sungguhan sambil membaca log aplikasi.

Sambungkan perangkat dengan USB debugging atau `adb connect`, lalu pastikan
terlihat:

```bash
adb devices -l
```

Kalau ada lebih dari satu perangkat, tambahkan `-s <serial>` ke setiap perintah
`adb` di bawah.

Pasang APK-nya. Kalau perangkat masih memuat build debug, jalankan dulu
`adb uninstall com.xdrac` (lihat catatan di Langkah 2).

```bash
adb install -r app/build/outputs/apk/release/app-release.apk
```

Di terminal kedua, baca log aplikasi secara realtime:

```bash
adb logcat -c
adb logcat -v time -s dracXterm Bootstrap Provisioning xset vhdp-jni xterm-native AndroidRuntime DEBUG
```

Buka aplikasinya dari launcher, atau lewat `adb`. `MainActivity` tidak
diekspor, jadi yang dibuka adalah layar penyiapan:

```bash
adb shell am start -n com.xdrac/.rootfs.ProvisioningActivity
```

Selama pemasangan distro, log harus memuat baris-baris berikut, tanpa
`FATAL EXCEPTION`, `Fatal signal`, atau `ANR`:

```text
[DOWNLOAD] verifying SHA-256 of <nama-arsip>.part
[GUEST] backend for /data/user/0/com.xdrac/files/rootfs: VHDP (self-test passed ...)
[RECOVERY] finished, exit=0
rootfs found at ... -> launching via guest backend VHDP
```

Setelah prompt muncul, jalankan pemeriksaan dasar ini di terminal aplikasi:

```bash
whoami; id -u                     # dracos, 1000
sudo whoami; sudo id -u           # root, 0
su -c whoami                      # root
sudo apt-get update
sudo apt-get install -y hello && hello
apt-get download cowsay && sudo dpkg -i ./cowsay_*.deb
sudo apt-get install -f -y && cowsay selesai
tar -cf /tmp/uji.tar /etc/passwd && echo tar-ok
bash -c 'trap "echo sinyal-ok" USR1; kill -USR1 $$; sleep 0.2'
vhdp doctor --no-active
xset vhdp
```

`dpkg -i` untuk `cowsay` boleh melaporkan dependensi yang belum terpenuhi;
`apt-get install -f` yang membereskannya. `xset vhdp` membuka halaman
Diagnostics, dan baris "Guest backend" di situ harus menyebut `vhdp`.

Coba juga lewat layar: `sudo -i` lalu `exit`, tambah workspace dengan tombol
`+`, sakelar Storage Access di `xset`, Ctrl+C dari bar tombol tambahan, putar
layar di tengah unduhan distro (bahasa antarmuka tidak boleh berubah), dan
tinggalkan aplikasi beberapa saat lalu kembali (proses latar harus masih
berjalan).

WayDroid dan emulator praktis untuk mencoba x86_64, tetapi SELinux di sana
permisif. Perubahan yang menyentuh eksekusi program, sinyal, atau proyeksi
`/dev` wajib dicoba di ponsel arm64 sungguhan, karena bug seperti `/dev` yang
hilang di dalam distro hanya muncul di sana.

### Langkah 7 (opsional): build App Bundle

```bash
./gradlew bundleRelease
```

Hasilnya `app/build/outputs/bundle/release/app-release.aab`. Format ini hanya
untuk diunggah ke Play Console; Android tidak bisa memasangnya langsung dan
berkas ini tidak dilampirkan ke GitHub Release. Validasi dengan
[bundletool](https://github.com/google/bundletool):

```bash
bundletool validate --bundle=app/build/outputs/bundle/release/app-release.aab
```

Bundle-nya sengaja tidak memecah resource per bahasa, karena aplikasi punya
toggle bahasa sendiri dan kedua terjemahan harus selalu ada di perangkat.

### Mengubah kode VHDP atau biner prebuilt

Sumber VHDP ada di dua tempat. `app/src/main/cpp/vhdp` dibangun bersama aplikasi
menjadi `libvhdp.so` plus jembatan JNI-nya, dan `app/src/main/assets/vhdp`
adalah salinannya yang ikut dikemas sebagai sumber untuk biner CLI. Cermin
salinan itu setiap kali pohon pertama berubah:

```bash
rsync -a --delete --exclude vhdp_jni.cpp --exclude bin/ \
      app/src/main/cpp/vhdp/ app/src/main/assets/vhdp/
```

Biner CLI-nya dibangun terpisah oleh `prebuilts/vhdp-cli/build.sh`, yang
menghasilkan `libphdp.so` dan `libvhdp-loader.so` ke `jniLibs/<abi>/` serta
`vhdp` static ke `assets/vhdp/bin/<abi>/`. Perintah untuk kedua ABI ada di
[Biner yang ikut dikirim](#biner-yang-ikut-dikirim). Setelah biner prebuilt
berubah, ulangi Langkah 5 dan Langkah 6.

### Membawa image rootfs di dalam APK

Yang menentukan jenis installer adalah isi `app/src/main/assets/rootfs/`:

| Isi folder itu saat build | Hasil |
|---|---|
| Tidak ada arsip (boleh ada `README.txt`) | APK tidak membawa image. Saat pertama dibuka aplikasi menampilkan katalog distro dan mengunduh pilihan Anda. |
| Tepat satu arsip | Arsipnya ikut dikemas. Saat pertama dibuka aplikasi langsung mengekstraknya, tanpa daftar distro dan tanpa jaringan. |
| Lebih dari satu arsip | Build gagal dan menyebut nama berkas yang ditemukan. |

Format yang dikenali: `.tar.xz`, `.txz`, `.tar.gz`, `.tgz`, dan `.tar`. Berkas
lain seperti `README.txt` diabaikan.

Untuk build yang membawa image, ambil dulu arsipnya. Arsip itu tidak ikut di
repo:

```bash
./scripts/fetch-rootfs.sh --list          # lihat pilihan
./scripts/fetch-rootfs.sh                 # ambil yang default
./gradlew assembleRelease                 # sekarang membawa image
```

Untuk kembali ke build tanpa image, kosongkan lagi folder itu dengan
`./scripts/fetch-rootfs.sh --clear`.

### Pemeriksaan lain

`scripts/verify-metadata.py` memeriksa hal-hal yang bisa diperiksa mesin:
identitas aplikasi, panjang teks Fastlane, ukuran dan rasio screenshot, serta
apakah ada material penandatanganan atau artefak build yang ikut terlacak Git.

```bash
python3 scripts/verify-metadata.py
./scripts/verify-rootfs-urls.sh           # cocokkan digest katalog dengan sumbernya
```

## Biner yang ikut dikirim

APK memuat tujuh biner prebuilt untuk tiap ABI, jadi empat belas berkas
seluruhnya:

| Biner | Isinya | Lisensi |
|---|---|---|
| `libbusybox.so` | BusyBox 1.38.0, shell dan applet yang dipakai sebelum ada distro terpasang | GPL-2.0-only |
| `libproot.so`, `libproot-loader.so` | PRoot v5.1.107.91 (fork termux), backend fallback | GPL-2.0-or-later |
| `libtalloc.so` | talloc 2.5.0, dependensi PRoot | LGPL-3.0-or-later |
| `libandroid-shmem.so` | libandroid-shmem v0.7 (fork termux), dependensi PRoot | BSD-3-Clause |
| `libphdp.so` | CLI VHDP 1.0.0, engine yang menjalankan sesi Linux | Apache-2.0 |
| `libvhdp-loader.so` | userland loader ELF VHDP, pemuat program guest | Apache-2.0 |

Semuanya dibangun dari sumber oleh `prebuilts/build.sh` dari revisi upstream yang
dipin, dengan `max-page-size=16384` supaya bisa di-`exec` di perangkat berhalaman
16 KB. Hasilnya reproducible: digest yang sama keluar dari direktori build mana
pun. Untuk menyegarkan kedua ABI:

```bash
./prebuilts/build.sh --install
ANDROID_ABI=x86_64 TRIPLE=x86_64-linux-android ./prebuilts/build.sh --install
```

Versi upstream, lisensi, dan written offer untuk source code-nya ada di
[`NOTICE`](NOTICE). Resep build tiap biner ada di
[`prebuilts/`](prebuilts/README.md).

## Keterbatasan yang diketahui

- **64-bit saja.** Perangkat `armeabi-v7a` dan x86 32-bit tidak didukung karena
  biner yang ikut dikirim hanya dibangun untuk `arm64-v8a` dan `x86_64`.
- **Katalog x86_64 masih satu entri uji coba.** Hanya Debian 13 amd64 yang
  tersedia, dan pemasangannya baru diuji di WayDroid.
- **Backup Android aktif.** `android:allowBackup` masih `true` dan aplikasi
  belum punya aturan pengecualian. Artinya data aplikasi, termasuk isi rootfs
  dan riwayat shell, bisa ikut terbawa oleh backup atau transfer perangkat.
  Matikan backup untuk aplikasi ini lewat Settings kalau Anda mengetik hal
  sensitif di terminal.
- **Semua image bergantung pada satu mirror.** Kelima URL di katalog menunjuk
  ke aset rilis `ExsoKamabay/rootless`. Kalau rilis itu dihapus atau reponya
  hilang, tidak ada distro yang bisa dipasang sampai katalognya dipindah.
  Digest ketiga image Kali masih bisa diturunkan ulang dari `kali.download`, dan
  digest Debian amd64 dari `SHA256SUMS` bertanda tangan milik linuxcontainers
  yang disimpan di mirror. Digest Debian arm64 tidak bisa: host yang dulu
  menerbitkannya sudah tutup.
- **Tidak ada CI di repo ini.** Rilis diuji manual: APK dibangun ulang lalu
  dicoba di perangkat sambil membaca logcat, seperti di
  [Langkah 6](#langkah-6-uji-di-perangkat). Unit test JVM dan suite
  instrumentation sudah dihapus di 1.0.5. Suite CTest VHDP ada di repositori
  upstream, tidak di salinan yang disematkan di sini.
- **Home menurut `getpwuid` adalah `/root`.** Kernel melihat sesi guest sebagai
  uid 0, sementara shell menampilkan user `dracos`. Program yang memakai `$HOME`
  tidak terpengaruh, tetapi program yang mencari home lewat
  `getpwuid(geteuid())` membaca konfigurasinya dari `/root`. Contohnya `ssh`,
  yang membaca `/root/.ssh/config` alih-alih `~/.ssh/config`. Untuk `ssh`,
  sebutkan berkasnya langsung dengan `-F ~/.ssh/config -i ~/.ssh/id_ed25519`.
- **Ollama hanya untuk arm64.** Artefak yang dipin adalah
  `ollama-linux-arm64.tar.zst`. Di x86_64 perintah `ollama` masih menanyakan
  konfirmasi unduhan, lalu aplikasi menolaknya tanpa mengunduh apa pun.
- **VHDP berlisensi Apache-2.0** (kompatibel satu arah dengan GPL-3.0), jadi APK
  yang memuat `libvhdp.so`, `libphdp.so`, `libvhdp-loader.so`, maupun perintah
  `vhdp` boleh dirilis dari sisi lisensi; lihat
  [`app/src/main/cpp/vhdp/NOTICE`](app/src/main/cpp/vhdp/NOTICE).
- **Sesi Linux berjalan lewat penerjemahan syscall**, entah VHDP atau PRoot, jadi
  beban kerja yang banyak memanggil syscall (kompilasi, `apt`, Ollama) lebih
  lambat daripada biner native. Ini batas pendekatan rootless, bukan bug.
- **Engine rootless adalah isolasi kompatibilitas, bukan sandbox keamanan.**
  UID 0 yang terlihat di dalam guest adalah emulasi. Jangan menjalankan rootfs
  yang tidak Anda percayai.

## Lisensi

GPL-3.0-or-later, lihat [`LICENSE`](LICENSE).

Biner pihak ketiga punya lisensinya sendiri: GPL-2.0 untuk BusyBox dan PRoot,
LGPL-3.0 untuk talloc, BSD-3-Clause untuk libandroid-shmem, dan Apache-2.0 untuk
VHDP, yaitu `libvhdp.so`, `libphdp.so`, dan `libvhdp-loader.so`
([`app/src/main/cpp/vhdp`](app/src/main/cpp/vhdp)). Font JetBrains Mono, Copse,
dan Black Ops One memakai SIL Open Font License 1.1. Teks lengkapnya ada di
[`licenses/`](licenses/) dan rinciannya di [`NOTICE`](NOTICE), termasuk written
offer untuk source code biner GPL.

Image Linux yang diunduh tunduk pada lisensi distribusinya masing-masing, bukan
pada lisensi dracxterm.
