# Changelog

Rilis dracxterm, versi terbaru di atas. Format tanggalnya YYYY-MM-DD.

`versionCode` adalah nomor internal Google Play. Ia hanya boleh naik, dan satu
angka tidak boleh dipakai ulang untuk unggahan yang berbeda.

## [1.0.6] - 2026-09-20

`versionCode` 6. Satu bug yang membuat Kali Linux tidak pernah sampai ke prompt,
dua perbaikan lingkungan yang muncul saat memburunya, dan pengujian ulang empat
image di perangkat. Tidak ada fitur baru dan tidak ada perubahan tampilan.

### Kali Linux berhenti di banner, tidak pernah menampilkan prompt

Gejalanya: pemasangan Kali selesai tanpa error, terminal menggambar banner, lalu
diam. Tidak ada prompt, Ctrl-C tidak terasa, panah atas tidak memanggil riwayat,
dan editor layar penuh kacau. Perintah yang diketik sebenarnya tetap dijalankan,
hanya tanpa prompt dan tanpa readline. Debian tidak terpengaruh sama sekali.

Shell-nya hidup, dan itu bagian yang menyesatkan. `bash -l` yang menggantung
terlihat sedang `read()` dari fd 0, dengan hanya SIGCHLD yang ditangkap dan
hanya SIGQUIT yang diabaikan, tanda shell **non-interaktif**. Shell login
non-interaktif tetap membaca `/etc/profile` dan `~/.profile`, jadi banner tetap
tergambar; yang tidak pernah terjadi adalah prompt.

Yang membuatnya memutuskan begitu: `isatty()`. Sejak glibc 2.42, yang dipakai
Kali 2026.x, `tcgetattr()` (dan karenanya `isatty()`) memanggil ioctl `TCGETS2`
alih-alih `TCGETS`, supaya baud rate bebas dan split speed bisa diwakili.
Kebijakan SELinux Android mendaftar ioctl tty mana yang boleh dipakai sebuah
aplikasi pada pseudo-terminalnya sendiri, dan termios2 tidak ada di daftar itu.
Hasilnya, di perangkat nyata: `TCGETS` boleh, `TCGETS2` menjawab EACCES,
`isatty()` palsu, dan setiap program yang dibangun di atas glibc 2.42
menyimpulkan tidak ada terminal. Debian 13 masih memakai glibc 2.41 dengan
`TCGETS`, jadi lolos.

Perbaikannya di mesin, bukan di distro. VHDP sekarang memeriksa sekali per sesi
apakah host menolak termios2, dengan mencoba `TCGETS2` pada sepasang pty baru
yang membawa label SELinux yang sama dengan terminal sesi. Kalau ditolak,
`TCGETS2`/`TCSETS2`/`TCSETSW2`/`TCSETSF2` dari guest dijalankan sebagai
`TCGETS`/`TCSETS`/`TCSETSW`/`TCSETSF` pada buffer yang sama: 36 byte pertama
kedua struktur identik, dan untuk pembacaan supervisor mengisi `c_ispeed` dan
`c_ospeed` dari bit `CBAUD`/`CIBAUD` persis seperti kernel menurunkannya. Hanya
baud rate non-standar (`BOTHER`) yang tidak terwakili; sebuah pseudo-terminal
tidak punya kecepatan jalur, dan host yang menolak termios2 juga tidak memberi
guest jalur serial.

Filter seccomp ikut menyaring per argumen, jadi hanya empat nomor request itu
yang berhenti di supervisor dan ioctl lain tetap lewat tanpa biaya. Di host
Linux biasa, yang mengizinkan termios2, probe-nya negatif dan tidak ada yang
berubah. `VHDP_TERMIOS2=translate` memaksa terjemahan untuk pengujian.

Efek yang terlihat di distro dengan pustaka baru: prompt kembali, riwayat
perintah dan readline jalan, `tty` menjawab nama pty, `stty` bekerja, job
control dan Ctrl-C kembali, dan editor layar penuh menggambar dengan benar.

### Pemasangan paket berhenti di "cannot open security status notification channel"

Muncul saat menguji Kali Full: `sudo dpkg -i paket.deb` dan `sudo apt install`
berhenti dengan pesan itu, dan paketnya tidak terpasang. Kali Nano dan Minimal
tidak terpengaruh, yang sempat membuatnya tampak seperti masalah image.

Bukan image, melainkan versi dpkg. Android memasang `selinuxfs` di
`/sys/fs/selinux`, dan guest melihat mount itu di `/proc/self/mounts`, jadi
libselinux di dalam distro menyimpulkan SELinux aktif (`selinuxenabled`
mengembalikan 0). Isi `/sys/fs/selinux` sendiri tertutup untuk domain aplikasi.
dpkg 1.23.7 yang dibawa Kali Full membuka kanal status SELinux di awal setiap
operasi dan menganggap penolakan itu fatal; dpkg 1.23.5 di Nano dan Minimal
masih melewatinya. Distro mana pun akan kena begitu dpkg-nya cukup baru.

Sekarang, kalau host menolak selinuxfs-nya sendiri, VHDP menyembunyikan
`/sys/fs/selinux` dari guest: yang dilihat guest adalah kernel tanpa SELinux,
persis yang diperiksa libselinux sebelum memakainya. Tidak ada yang hilang,
karena aplikasi Android memang tidak pernah bisa membaca policy atau menyetel
konteks. Di host Linux yang SELinux-nya benar-benar bisa dipakai, probe-nya
negatif dan tidak ada yang berubah.

### Berkas yang dibuat di terminal memakai hak standar Linux

Proses aplikasi Android berjalan dengan `umask` 077, dan sesi terminal mewarisi
itu. Akibatnya setiap berkas dan direktori yang dibuat di dalam distro hanya
bisa dibaca pemiliknya: `dpkg-deb --build` menolak direktori `DEBIAN` bermode
700 ("control directory has bad permissions 700"), dan berkas yang ditulis skrip
paket tidak terbaca oleh layanan yang dituju. Anak PTY sekarang menyetel `umask`
022, yaitu nilai yang dipakai login Linux biasa (`login.defs`, `pam_umask`), dan
pemulihan dpkg menjalankan skripnya dengan mask yang sama.

### Prompt tidak lagi mencetak "Permission denied"

Pada perangkat yang menutup `/proc/sys/kernel` untuk aplikasi, skrip prompt
systemd 258+ yang dibawa Kali membaca `/proc/sys/kernel/random/boot_id` dan
`/proc/sys/kernel/random/uuid` di setiap prompt dan menghasilkan dua sampai tiga
baris `Permission denied` setiap kali. Keduanya kini punya stand-in di VHDP,
seperti `/proc/stat` dan `/proc/uptime` sebelumnya: `boot_id` tetap sama selama
perangkat menyala (diturunkan dari saat boot, yang bisa dihitung setiap proses),
`uuid` acak tiap kali dibaca. Stand-in hanya dipakai kalau host benar-benar
menolak berkasnya.

### Self-test backend memanggil API yang belum ada di Android 7

`minSdk` aplikasi ini 24, tetapi pemilihan backend memakai
`Process.waitFor(timeout)` dan `destroyForcibly()`, keduanya baru ada di API 26.
Di Android 7.0 dan 7.1 panggilan itu melempar `NoSuchMethodError` justru di
jalur yang tugasnya memutuskan apakah perangkat sanggup menjalankan VHDP. Di
bawah API 26 keduanya kini punya pengganti: status keluar di-polling, dan
`destroy()` di Android sudah mengirim SIGKILL seperti `destroyForcibly()`.

### Pengujian

Empat image diuji di perangkat nyata dari berkas rootfs lokal, tanpa unduhan:
Kali Nano, Kali Minimal, Kali Full, dan Debian 13. Di samping perintah dasar dan
fitur terminal, tiap image diuji untuk perintah superuser, pembuatan dan
pemasangan paket `.deb` sendiri, serta pemasangan paket dari arsip distronya.
Suite CTest VHDP di repositori upstream bertambah dua kasus untuk terjemahan
termios2 dan aturan filter seccomp-nya.

### Keterbatasan yang tersisa

Jalur cadangan PRoot, yang dipakai hanya kalau VHDP gagal self-test di sebuah
perangkat, belum menerjemahkan termios2. Di sana distro dengan glibc 2.42 masih
membuka shell tanpa prompt.

## [1.0.5] - 2026-09-20

`versionCode` 5. Nomor 1.0.4 dilewati dan tidak pernah dipakai untuk build yang
beredar. Yang berubah: sesi Linux sekarang dijalankan VHDP alih-alih PRoot, APK
memuat biner untuk x86_64 di samping arm64, empat bug stabilitas terminal
ditutup, katalog image pindah ke mirror sendiri, dan `xset` mendapat halaman
Diagnostics. Layar utama dan daftar izin tidak berubah.

### Sesi Linux dijalankan VHDP, PRoot jadi fallback

Shell di dalam distro dulu ditopang PRoot. Sekarang yang menjalankannya VHDP,
rootless supervisor berbasis `ptrace` yang dipercepat `seccomp`, dikirim sebagai
`libphdp.so` plus loader ELF userland `libvhdp-loader.so`.

Backend dipilih sekali per pemasangan. Aplikasi menjalankan self-test di
perangkat itu sendiri (engine rootless, loader, identitas yang diemulasikan,
proyeksi `/dev`) dan menyimpan hasilnya di `files/.guest-backend`. Kalau
self-test gagal, sesi jalan di PRoot seperti sebelumnya, tanpa ada yang perlu
diatur. Keputusannya ditulis ke logcat sebagai `[GUEST] backend for ...`, dan
menghapus berkas penanda itu memaksa keputusan dievaluasi ulang.

Jalur exec-nya tetap mematuhi W^X Android: yang di-`execve` hanya biner di
direktori pustaka native, dan program di dalam rootfs dimuat lewat
`mmap(PROT_EXEC)` oleh loader userland itu.

Sumber VHDP ikut di repo, di `app/src/main/cpp/vhdp` (dibangun jadi `libvhdp.so`
dan jembatan JNI-nya) dan `app/src/main/assets/vhdp` (sumber untuk biner CLI).
Lisensinya Apache-2.0.

### APK memuat biner x86_64

`abiFilters` sekarang berisi `arm64-v8a` dan `x86_64`, jadi emulator dan WayDroid
bisa menjalankan aplikasi ini. Tiap ABI membawa tujuh biner prebuilt: BusyBox,
PRoot dan loader-nya, talloc, libandroid-shmem, `libphdp.so`, dan
`libvhdp-loader.so`. Semuanya dibangun dengan `max-page-size=16384`.

Direktori kerja prebuilt dipisah per ABI (`prebuilts/work/<ABI>/`) dan
`--install` menulis ke `jniLibs` ABI yang sedang dibangun. Sebelumnya tujuannya
selalu `arm64-v8a`, jadi build x86_64 akan menimpa biner arm64. Resep baru
`prebuilts/vhdp-cli/build.sh` membangun CLI VHDP, yang dulu dibuat ad-hoc tanpa
resep.

Katalog image menambah satu entri x86_64, Debian 13 amd64, unduhan 89 MB dan
sekitar 700 MB terpasang. Entri itu ditandai uji coba. Arsipnya dicerminkan ke
mirror proyek, dijelaskan di bagian mirror di bawah.

### Yang disediakan ulang VHDP di dalam sandbox aplikasi

Kebijakan Android menolak sebagian panggilan yang dipakai distro Linux biasa.
VHDP menyediakan gantinya, dan tiap mekanisme punya probe sendiri supaya hanya
aktif di host yang memang membutuhkannya:

- `link(2)` ditolak SELinux di penyimpanan aplikasi, sementara dpkg, shadow,
  `tar`, dan `cp -al` memerlukannya. Hardlink diemulasikan per mount di
  `.vhdp-hardlinks/`, dengan `st_nlink` dan `st_ino` yang ikut disesuaikan, dan
  objeknya dikembalikan ke nama aslinya begitu hitungannya turun ke satu.
- Berkas `/proc` global ditolak, bahkan `lstat`-nya. `uptime`, `loadavg`, `stat`,
  `version`, `vmstat`, `swaps`, `filesystems`, dan `sys/kernel/*` dibangun dari
  sumber yang diizinkan (`CLOCK_BOOTTIME`, `sysinfo`, `uname`, residensi
  cpuidle, `meminfo`), jadi `uptime`, `ps`, `pstree`, dan `vmstat` bekerja.
- Socket `NETLINK_AUDIT` ditolak dengan EACCES, dan `useradd`/`groupadd` dari
  shadow berhenti kalau errno-nya bukan EINVAL, EPROTONOSUPPORT, atau
  EAFNOSUPPORT. Sekarang dijawab EPROTONOSUPPORT, sehingga postinst paket
  seperti openssh-client selesai.
- Kebijakan seccomp aplikasi memakai `SECCOMP_RET_TRAP`, yang mengalahkan
  `RET_TRACE` milik engine. Supervisor menjawab SIGSYS itu sendiri, kecuali bila
  filter yang men-trap dipasang guest. Di x86_64 syscall legacy yang ditrap
  (`open`, `stat`, `access`, `dup2`, `poll`) di-restart sebagai padanan `*at`.
- Kredensial dipalsukan per proses, jadi `id` melaporkan uid 0 di dalam guest
  tanpa privilese apa pun di host.

Pemulihan `apt`/`dpkg` setelah pemasangan sekarang diputuskan libvhdp secara
native: ia memeriksa state package manager dan mengembalikan rencananya, lalu
aplikasi menjalankan skrip itu di dalam guest sebagai fake-root lewat backend
yang terpilih.

### Empat perbaikan yang terasa di terminal

- `tar -cf` gagal dengan ENOSYS. GNU tar memakai `creat(2)`, kebijakan seccomp
  Android men-trap-nya, dan `handle_seccomp_event()` PRoot tidak punya case
  untuk syscall itu sehingga menjawab `-ENOSYS`. Patch
  `prebuilts/proot/patches/0004-seccomp-handle-creat.patch` menulis ulang ke
  `openat(AT_FDCWD, path, O_CREAT|O_WRONLY|O_TRUNC, mode)`.
- SIGQUIT, SIGUSR1, dan SIGPIPE tidak pernah sampai ke program di dalam guest.
  Signal mask bertahan lintas `execve` dan ART memblokir ketiganya untuk
  keperluannya sendiri, sementara anak `forkpty` hanya mereset disposisi SIGCHLD
  dan SIGPIPE. Anak PTY sekarang mengembalikan disposisi seluruh sinyal ke
  `SIG_DFL` dan mengosongkan mask sebelum `execve`, jadi `trap ... USR1` jalan
  dan penulis pipa yang kehilangan pembacanya mati dengan status 141.
- `vhdp doctor` merusak `/tmp` untuk sisa sesi. Probe user-namespace me-mount
  tmpfs di atas `/tmp`; di bawah supervisor `unshare` dan `mount` dilaporkan
  sukses lalu mountpoint-nya dicatat, sehingga akses `/tmp` berikutnya menjawab
  ENOENT. Probe itu kini dilewati saat proses sedang ditrace, dan mount-nya
  memakai direktori bikinan sendiri yang langsung dihapus.
- Guest kehilangan `/dev` dan `/sys` di perangkat Android nyata. Uji
  ketersediaan memakai `access(p, R_OK|X_OK)`, dan SELinux melarang aplikasi
  melist kedua direktori itu meski menelusurinya diizinkan, jadi bind-nya
  dijatuhkan dan menulis ke `/dev/null` malah membuat berkas biasa di dalam
  rootfs. Ujinya sekarang `X_OK` saja, dan bind yang dijatuhkan dicatat ke
  logcat beserta alasannya. WayDroid tidak pernah memperlihatkan bug ini karena
  SELinux-nya permisif.

Satu perbaikan lain datang dari VHDP: group-stop dihormati seperti di Linux
asli. `PTRACE_EVENT_STOP` membawa sinyal penghenti, bukan `SIGTRAP`, dan dulu
dikira signal-delivery-stop sehingga tracee dijalankan kembali. Akibatnya
`timeout N apt-get ...` berputar tanpa henti. Sekarang event itu dilanjutkan
dengan `PTRACE_LISTEN`, jadi `Ctrl+Z` dan proses background yang menyentuh tty
berperilaku seperti seharusnya.

### Perintah vhdp di guest dan halaman Diagnostics di xset

Perintah `vhdp` dipasang ke `/usr/local/bin` di dalam guest setiap kali terminal
dibuka: `vhdp doctor` untuk diagnostik host, `vhdp inspect <rootfs>`, dan
`vhdp capabilities`. `vhdp run` ditolak dari dalam terminal dengan alasannya,
karena Linux hanya mengizinkan satu tracer per proses.

`xset` menambah halaman Diagnostics yang menampilkan versi libvhdp, ABI,
arsitektur build, profil yang terdeteksi, kebijakan exec storage, backend yang
dipakai sesi ini, dan probe aktif.

### NOTICE kembali dan pemeriksaan metadata lolos lagi

`NOTICE` di akar repositori sempat hilang dari working tree, padahal `README.md`
dan `scripts/verify-metadata.py` merujuknya dan biner GPL yang ikut dikirim
membawa written offer di situ. Berkasnya dipulihkan, inventarisnya diperluas ke
dua ABI, dan `libphdp.so` serta `libvhdp-loader.so` ditambahkan sebagai butir
Apache-2.0 dengan letak sumbernya. `Apache-2.0.txt` juga masuk daftar lisensi
yang wajib ada di `licenses/`. `scripts/verify-metadata.py` sekarang lolos tanpa
temuan.

### Tiga perbaikan dari uji rilis

Uji build release di perangkat menemukan tiga masalah, dan ketiganya diperbaiki
di rilis ini.

- Bahasa pilihan hilang setelah layar diputar. `ProvisioningActivity` dan
  `MainActivity` menangani rotasi, perubahan ukuran jendela, dan density sendiri
  lewat `android:configChanges`, lalu framework membangun ulang resource
  keduanya dari konfigurasi sistem, termasuk locale perangkat. Di perangkat
  berbahasa Inggris, layar pemasangan yang tadinya berbahasa Indonesia berganti
  menjadi "Downloading ... Cancel" di tengah unduhan. Kedua activity sekarang
  memasang ulang bahasa pilihan di `onConfigurationChanged`.
- Pesan tahap pemasangan selalu berbahasa Inggris. "Extracting… 500 files",
  "Configuring environment…", "Linux ready", dan sembilan pesan lain ditulis
  langsung di `BootManager`. Kedua belas pesan itu sekarang ada di
  `strings.xml` untuk bahasa Indonesia dan Inggris, dan dibaca dalam bahasa yang
  berlaku saat pemasangan dimulai.
- `xset` menampilkan warna sebagai angka. Semua preset tema selain Cyber Neon
  memakai warna yang tidak ada di daftar pilihan, sehingga baris Foreground,
  Background, dan Cursor Color menampilkan nilai ARGB mentah seperti `-33061`.
  Warna seperti itu sekarang tampil sebagai `#FF7EDB`.

### Tes dipindah ke perangkat

Unit test JVM (`app/src/test`) dan dua suite instrumentation
(`app/src/androidTest`) dihapus dari repositori, begitu juga
`testInstrumentationRunner` serta dependensi junit dan androidx.test di
`app/build.gradle.kts`. Rilis diuji dengan membangun ulang APK release, lalu
mencobanya di perangkat sambil membaca logcat.

Suite C++ host di `native-tests/` ikut dihapus, bersama dua skrip yang sudah
tidak dipanggil: `scripts/release-build.sh` (build rilis cukup
`./gradlew assembleRelease`) dan `scripts/fetch-licenses.sh` (`BSD-3-Clause.txt`
sudah ada di repositori, sama persis dengan `LICENSE` upstream android-shmem).

Build release 1.0.5 diuji ulang di WayDroid (x86_64, Android 13) pada
2026-09-20, setelah image x86_64 dipindah ke mirror, mulai dari data kosong:
katalog, unduhan Debian 13 amd64 93 MB dari mirror, pemeriksaan SHA-256,
ekstraksi, lalu pemilihan backend (VHDP, self-test lolos). Pemasangannya selesai
dalam sekitar 35 detik. Hasil suite di dalam guest:

| Suite | Yang diperiksa | Hasil |
|---|---|---|
| QA-A | identitas, superuser, coreutils, filesystem, `/dev` `/proc` `/sys`, jaringan, perintah `vhdp` | 54 lolos, 0 gagal |
| QA-B | `apt` dan `dpkg`: update, install, `.deb` lewat `dpkg -i`, remove, purge, autoremove | 33 lolos, 0 gagal |
| QA-C | git, Python, curl, arsip, sinyal, izin fake-root, job control, I/O 20 MB | 51 lolos, 0 gagal |
| QA-DEB | `.deb` diunduh langsung lewat HTTPS, dicocokkan SHA-256, dipasang dengan `dpkg -i` dan `apt install ./berkas.deb`; `.deb` rusak ditolak | 17 lolos, 0 gagal |
| QA-SCALE | berkas 64 MB, 5000 berkas, path 200 tingkat, 32 proses paralel | 28 lolos, 0 gagal |
| FINAL | sapuan fitur | 21 lolos, 0 gagal |

Di luar skrip, yang dicoba langsung di terminal: shell root interaktif
(`sudo -i`, `sudo -s`, `su`, `su -`, `sudo su`, `fakeroot`) dan kembali ke
user biasa dengan `exit`, `xset` termasuk Diagnostics dan probe aktifnya,
sakelar Storage Access, dua workspace, Ctrl+C dari bar tombol, proses latar yang
tetap berjalan saat aplikasi ditinggalkan, serta pembaruan APK di atas
instalasi yang sudah ada. Logcat proses aplikasi tidak mencatat crash, ANR,
maupun error dari kode dracxterm.

Di Infinix X6726B (arm64, Android 15) build release yang sama dipasang di atas
instalasi 1.0.5 yang sudah ada tanpa kehilangan data. Backend VHDP lolos
self-test, dan uji singkat di terminal berhasil: `uname -m` menjawab `aarch64`,
`sudo whoami` menjawab `root`, `/dev/null` berupa character device, `tty`
menunjuk ke `/dev/pts`, pipe, `vhdp version`, `sudo apt-get update` (16,9 MB),
dan `dpkg --audit` tanpa temuan. Suite lengkap tidak dijalankan ulang di
perangkat ini. Hasil suite arm64 terakhir berasal dari putaran pengembangan
2026-09-18 di perangkat yang sama (build debug): 54, 33, 28, dan 21 lolos, serta
50 lolos plus satu pemeriksaan INFO untuk suite ketiga.

### Keterbatasan yang ditemukan saat uji

- Kernel melihat sesi guest sebagai uid 0 (fake-root), sementara shell
  menampilkan user `dracos`. Program yang mencari direktori home lewat
  `getpwuid(geteuid())` alih-alih `$HOME` memakai `/root`. `fastfetch`
  misalnya membaca konfigurasinya dari sana, dan `ssh` membaca
  `/root/.ssh/config` alih-alih `/home/dracos/.ssh/config`. Program yang memakai
  `$HOME`, seperti bash, git, dan Python, tidak terpengaruh.
- Ollama hanya tersedia untuk arm64. Di perangkat x86_64 perintah `ollama`
  masih menanyakan konfirmasi unduhan, lalu aplikasi menolaknya dengan pesan
  "device ABI is not arm64-v8a" tanpa mengunduh apa pun.

### Kelima image pindah ke mirror sendiri

Katalog `assets/rootfsURLS.json` sekarang menunjuk ke aset rilis
https://github.com/ExsoKamabay/rootless: `rootfs-20260913` untuk keempat image
arm64 dan `rootfs-20260920` untuk image x86_64. Repositori itu hanya menyimpan
arsip rootfs beserta `SHA256SUMS`-nya, tanpa kode program.

Untuk ketiga entri Kali yang berubah cuma host-nya. Digest, ukuran unduhan, dan
perkiraan ruang terpasang sama persis dengan 1.0.3, dan ketiganya masih cocok
dengan `SHA256SUMS` yang diterbitkan Kali untuk kali-2026.2.

### Image x86_64 dicerminkan sebelum rilis

Entri x86_64 sempat memakai build Debian amd64 `20260913_05:24` langsung dari
images.linuxcontainers.org. Pada 2026-09-20, sebelum 1.0.5 dirilis, URL itu
sudah menjawab 404 karena host tersebut hanya menyimpan build bertanggal
sekitar tiga hari. Entrinya sekarang memakai build `20260919_05:24` dari host
yang sama, disajikan dari mirror sebagai
`debian-trixie-amd64-lxc-20260919.tar.xz`, dengan digest
`6c070114809e6fa24dde8f2f845bb349667430c70d1b20281cd83a80bf132c23` dan ukuran
93.142.872 byte.

Digest itu cocok dengan `SHA256SUMS` build tersebut, dan tanda tangan GPG
berkas itu sah dari kunci "LXC pre-built images"
(`E7FB0CAEC8173D669066514CBAEFF88C22F6E216`). `SHA256SUMS` dan tanda tangannya
ikut diunggah ke rilis mirror, jadi digest ini tetap bisa ditelusuri ke
linuxcontainers setelah build aslinya dirotasi keluar.

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
