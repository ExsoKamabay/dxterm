# Changelog

Perubahan dracxterm, versi terbaru di atas.

## Soal nomor versi

Nomor 1.0.0, 1.0.1, dan 1.1.0 di bagian bawah berkas ini milik `com.dracxterm`,
paket yang dipakai project ini sebelum berganti nama. Catatannya dibiarkan karena
menjelaskan bagaimana kodenya sampai ke bentuk sekarang, tapi tidak satu pun
nomor itu berlaku untuk paket yang Anda pasang hari ini.

Rilis publik pertama dracxterm adalah **1.0.0** dengan `versionCode 1`,
diterbitkan sebagai tag `v1.0.0` di repository
[`ExsoKamabay/dxterm`](https://github.com/ExsoKamabay/dxterm). Penomoran yang
terlihat pengguna dimulai dari sini.

`versionCode` adalah nomor internal Google Play: ia hanya boleh naik, dan satu
angka tidak boleh dipakai ulang untuk unggahan yang berbeda. Rilis berikutnya
menaikkannya ke 2.

## [1.0.0] - 2026-09-03

Rilis publik pertama, dengan `applicationId` `com.xdrac` dan `versionCode 1`.
Isinya adalah hasil audit kualitas menyeluruh yang dicatat di bawah: tidak ada
fitur baru, dan tidak ada perubahan perilaku yang tidak disengaja.

### Diubah

- **Nama aplikasi jadi `dracxterm`, dan paketnya jadi `com.xdrac`.** Label
  launcher, judul Fastlane, dan `namespace` Gradle sekarang memakai nama yang
  sama. `applicationId` berpindah dari `com.dracxterm`, jadi paket ini terpisah
  dari apa pun yang pernah terpasang dengan id lama: Android memperlakukannya
  sebagai aplikasi baru, bukan pembaruan.
- **`versionName` jadi `1.0.0`, `versionCode` direset ke 1.** Keduanya milik
  paket baru, yang belum pernah punya unggahan sebelumnya.

- **Product flavour `online`/`offline` dihapus; tinggal satu source set dan satu
  alur build.** Jenis installer bukan lagi varian build: yang menentukan adalah isi
  `app/src/main/assets/rootfs/` saat build. Tepat satu arsip berarti image ikut
  dikemas dan dipasang otomatis dari assets; tidak ada arsip berarti aplikasi
  menawarkan katalog `assets/rootfsURLS.json` seperti sebelumnya. Lebih dari satu
  arsip menggagalkan build dan menyebut nama-nama file yang ditemukan, karena tidak
  ada cara menebak image mana yang dimaksud.

  Ini menggantikan `docs/adr/0003-one-app-two-installers.md`: keputusannya tetap
  "satu aplikasi", cuma mekanismenya tidak lagi lewat flavour. Task yang dipakai
  sekarang `assembleDebug`, `assembleRelease`, `bundleRelease`.

  Keputusannya dikumpulkan di satu kelas, `RootfsSourceResolver`, yang membaca
  `assets/rootfs/` lewat `AssetManager`; `BootManager` bertanya ke sana dan tidak ada
  lagi pemeriksaan mode yang tersebar. Tidak ada `BuildConfig.FLAVOR`, tidak ada
  ketergantungan pada nama task atau nama APK.
- **`app/src/offline/` dan `app/src/test/` dihapus.** Yang pertama hanya berisi
  penampung arsip flavour; isinya pindah ke `app/src/main/assets/rootfs/`. Test JVM
  dihapus atas permintaan; verifikasi sekarang bersandar pada lint, suite C++ di
  `native-tests/`, dan pemeriksaan isi artefak.

### Diperbaiki

- **Trace diagnostik ikut terkirim ke build release.** `FLICKER_TRACE` di
  `TerminalView` dibiarkan `true`, padahal komentarnya sendiri menyebut trace itu
  sementara dan "Off = zero cost". Akibatnya setiap frame menghitung hash FNV-1a
  atas seluruh grid glyph/fg/bg/attr lalu menulis logcat. Trace-nya dihapus; ia
  sudah menemukan flicker yang jadi alasan keberadaannya di 1.0.1.
- **Target link di dalam arsip tidak pernah diperiksa.** `RootfsExtractor` menguji
  nama entry terhadap canonical path direktori staging, tapi `Os.symlink` dan
  `Os.link` menerima target apa adanya. Entry bernama biasa seperti `bin/sh` dengan
  target `../../../../data/data/com.dracxterm/shared_prefs/xset.xml` membuat
  hardlink ke berkas privat aplikasi di dalam rootfs. Pemeriksaannya kini satu
  objek, `archive.ArchivePaths`, dipakai oleh kedua extractor.
- **Rootfs lama dihapus sebelum yang baru masuk.** Kalau proses mati di antaranya,
  pengguna kehilangan rootfs tanpa pengganti. Sekarang yang lama disingkirkan,
  dikembalikan kalau rename gagal, dan baru dihapus setelah yang baru terpasang.
- **Tiga izin `READ_MEDIA_*` dideklarasikan tapi tidak pernah diminta.** Izin itu
  juga memberi akses MediaStore, bukan akses lewat path yang dibutuhkan
  `~/sdcard`, jadi tidak ada yang memakainya andai diberikan. Dihapus.
- **`su -c CMD` melaporkan identitas yang salah.** Ia tidak meng-export `DRAC_SU`,
  jadi `su -c whoami` menjawab `dracos` sementara `su` interaktif menjawab `root`.
- **`consent_body` dipanggil dengan empat argumen dan memakai satu.** Label dan dua
  pemanggilan `Formatter.formatShortFileSize()` dihitung lalu dibuang setiap kali
  layar persetujuan digambar.
- Dua downloader punya penanganan redirect sendiri-sendiri dan sudah menyimpang:
  satu menolak hop non-HTTPS dan menangani `Location` relatif, satu lagi tidak.
  Sekarang keduanya lewat `net.HttpsOnly`, yang juga menangani 307 dan 308.
- Ruang kosong bernilai nol diperlakukan sebagai "tidak diketahui" di kedua
  downloader, jadi disk yang benar-benar penuh lolos pemeriksaan.
- **UTF-8 rusak menjatuhkan aplikasi.** Decoder di `AnsiParser` menerima bentuk
  overlong, surrogate UTF-16, dan nilai di atas U+10FFFF. Empat byte `f7 bf bf bf`
  menaruh U+1FFFFF di grid, lalu `TerminalView` menggambarnya lewat
  `String(Character.toChars(cp))` yang melempar `IllegalArgumentException` dari
  dalam `onDraw`. `cat` pada berkas biner apa pun cukup untuk memicunya. Ketiganya
  kini jadi U+FFFD. Bentuk overlong juga penting sendiri: `e0 80 af` adalah `/`,
  cara klasik menyelundupkan byte melewati filter.
- **Parameter CSI tanpa batas.** `curParam_ * 10 + digit` melimpah-kan signed int
  (undefined behaviour), dan nilainya lalu menggerakkan loop per baris. `printf
  '\033[2000000000S'` men-scroll selama hampir satu jam dengan UI thread menunggu
  parser. Nilai dibatasi 65535 (batas xterm sendiri), jumlah parameter 256, dan
  `scrollUp`/`scrollDown`/`tabForward`/`tabBack`/`eraseChars` dibatasi ke titik di
  mana hasilnya berhenti berubah, jadi batasnya persis dan bukan pemotongan.
  Kasus terburuk turun dari ~52 menit ke 143 ms.
- **`reset` tidak mereset apa yang membuatnya diketik.** RIS (`ESC c`) dan DECSTR
  (`CSI ! p`) hanya membereskan layar. Charset DEC line-drawing, sekuens UTF-8
  yang belum selesai, IRM, pen SGR, dan slot DECSC semuanya bertahan, jadi `reset`
  tidak bisa memperbaiki terminal kacau yang jadi alasan ia dijalankan. Keduanya
  kini mengikuti tabel DECSTR VT510; posisi kursor hidup sengaja dipertahankan,
  karena ia tidak ada di tabel itu.
- **Deskriptor PTY ditutup saat thread pembaca masih memakainya.** `Session::stop`
  menutup fd master lebih dulu, baru join. Menutup fd tidak pernah membangunkan
  `poll` yang sudah menunggu di Linux, jadi urutan itu tidak menghemat apa pun,
  tapi nomor fd-nya langsung bebas dipakai ulang: pembaca bisa keluar dari poll
  lalu `read()` dari deskriptor lain milik proses ini. Join dulu, tutup kemudian.
- **Alokasi heap di anak setelah `fork`.** `argv`/`envp` dibangun di sisi anak
  `forkpty`. Hanya thread pemanggil yang ikut ke anak, jadi arena malloc yang
  kebetulan dipegang thread lain saat fork terkunci selamanya di sana dan anaknya
  menggantung sebelum exec. Sekarang dibangun sebelum fork.
- **MainActivity bocor selama proses hidup.** `OllamaLauncher.attach` menerima
  Context dari Activity dan menyerahkannya ke executor statis dan thread yang
  tidak pernah selesai, menahan seluruh view hierarchy. Diambil
  `applicationContext` sekali di batas modul.
- **`RootfsDownloader.download` bisa rekursi tanpa batas.** Berkas yang gagal
  checksum dihapus tanpa memeriksa hasilnya, lalu fungsi memanggil dirinya sendiri;
  berkas yang tidak bisa dihapus berarti verifikasi yang sama diulang sampai stack
  habis. Kegagalan hapus kini dilaporkan.
- `HttpsOnly.open` membiarkan koneksi tergantung kalau `responseCode` melempar,
  yang justru tempat timeout dan kegagalan TLS muncul.

### Dihapus

- Katalog rootfs punya key `armeabi-v7a`, `x86_64`, dan `x86` yang kosong. Dengan
  `abiFilters` arm64-v8a saja, entry di sana tidak akan pernah bisa ditawarkan,
  dan kalaupun ditawarkan tidak akan bisa dijalankan.
- `PermissionManager.missingRuntime()`, `runtimePermissions()`, dan
  `hasAllFilesAccess()` tidak dipanggil siapa pun. Cabang `else` di setiap `when`
  juga tidak terjangkau, karena `minSdk` adalah 24.
- `Bootstrap.captureStorageBackable()` dan field yang ia isi hanya memberi makan
  satu baris log.
- `OllamaConfig.CHECKSUM_URL` tanpa pemanggil.
- `presplash.png` (400 KB, ikut di setiap APK), `dot_connected.xml`, warna
  `status_ok`, dan tiga string, semuanya tanpa referensi.
- `release-prep.sh` tidak lagi menawarkan `rotate-key` dan `clean-refs`. Keduanya
  langkah sekali pakai dan keduanya sudah selesai.
- **Seluruh `docs/` dan kedua workflow di `.github/workflows/`.** Isi `docs/`
  sudah tidak sinkron dengan kode setelah flavour dihapus, dan `ci.yml` masih
  memanggil `assembleOnlineDebug` yang sudah tidak ada. Yang masih dibutuhkan
  dipindahkan: versi upstream, lisensi, dan written offer untuk source code
  biner GPL sekarang ada di `NOTICE`; pemetaan berkas ke lisensi ada di
  `licenses/README.md`; resep build tiap biner ada di `prebuilts/`.

### Pengujian

- 39 test engine C++ baru: penolakan UTF-8 tak sah (overlong, surrogate, di atas
  U+10FFFF), batas parameter CSI dengan anggaran waktu, dan cakupan reset
  RIS/DECSTR. Suite C++ naik dari 160 ke 199 lulus, dijalankan lewat
  `./native-tests/run-tests.sh`.
- Menyalakan lint Android menemukan lima error yang sudah lama ada; semuanya
  diperbaiki. `lintVitalRelease` lolos tanpa isu fatal.
- Unit test JVM dihapus bersama `app/src/test/`, jadi verifikasi rilis ini
  bersandar pada suite C++, lint, dan pemeriksaan artefak
  (`apksigner verify`, `zipalign -c`, `bundletool validate`).

### Keterbatasan yang diketahui

- Hanya `arm64-v8a`. Perangkat `armeabi-v7a` dan x86 tidak didukung.
- `android:allowBackup` masih `true` tanpa aturan pengecualian, jadi data
  aplikasi termasuk isi rootfs dan riwayat shell bisa ikut terbawa backup atau
  transfer perangkat.
- Digest image Debian berasal dari mirror pihak ketiga dan sudah tidak bisa
  diturunkan ulang dari otoritas independen. Pin-nya tetap menahan substitusi,
  tapi rantai kepercayaannya lebih pendek daripada image Kali.
- Repo ini tidak lagi punya unit test JVM maupun CI.

## [1.0.1] - 2026-08-25, paket `com.dracxterm`

Hampir semuanya perbaikan, dan tiga di antaranya baru ketahuan saat menguji
sesuatu yang lain. Ketiganya cuma muncul di kondisi tertentu, yang menjelaskan
kenapa lolos sejauh ini.

### Diperbaiki

- **Aplikasi menutup sendiri beberapa detik setelah dibuka, tapi hanya setelah
  di-update.** proot butuh `libtalloc.so.2`, sementara Android hanya mengekstrak
  berkas bernama `lib*.so`, jadi ia dikirim sebagai `libtalloc.so` dan symlink-nya
  dibuat saat aplikasi jalan. Symlink itu diganti dengan
  `if (f.exists()) f.delete()`, dan `File.exists()` mengikuti tautan: symlink yang
  targetnya hilang dilaporkan tidak ada, jadi tidak pernah dihapus, dan
  `Os.symlink` berikutnya gagal dengan EEXIST. Update aplikasi mengganti nama
  direktori instalasi APK, jadi tautan lama menunjuk ke tempat yang sudah tidak
  ada. Shell mati begitu dijalankan dan terminal menutup dirinya sendiri tanpa
  pesan apa pun. Sekarang penghapusannya tanpa syarat, dan hasilnya diperiksa.
- **Shell BusyBox tidak bisa jalan sama sekali.** BusyBox memilih applet dari
  argv[0], dan Java tidak bisa menyetel argv[0] terpisah dari path program. Biner
  dikemas sebagai `libbusybox.so`, yang bukan nama applet, jadi menjalankannya
  lewat path sendiri menjawab "applet not found" dan keluar dengan status 127.
  Aplikasi kini menjalankan symlink `usr/bin/ash`, bukan binernya langsung.
- **Daftar applet BusyBox tidak pernah benar.** `busybox --list` gagal karena
  alasan yang sama, status keluarnya diabaikan, dan satu baris pesan error
  "libbusybox.so: applet not found" jadi satu-satunya isi daftar. Hasilnya satu
  symlink bernama kalimat error itu, dan log menulis "installed 1 busybox applet
  links" setiap kali tanpa ada yang membaca. Sekarang 332 applet, status keluar
  diperiksa, dan daftar yang terlalu pendek ditolak.
- **Tautan applet tidak diperbarui setelah update.** Penanda `.bootstrap_v1` cuma
  menjawab "pernah jalan atau belum", dan jawabannya tetap ya sementara semua
  tautan yang dibuatnya sudah menggantung. Penanda itu kini menyimpan direktori
  tujuan tautan, jadi update membatalkannya sendiri.
- **Penyiapan pertama kadang gagal dengan "cannot create staging dir".**
  `ProvisioningActivity` memanggil `setApplicationLocales` untuk memasang default
  Bahasa Indonesia, dan itu me-recreate activity yang baru saja mulai
  mengekstrak. Instance kedua memulai ekstraksi kedua ke direktori staging yang
  sama. Balapan, jadi kadang lolos. Default bahasa sekarang dipasang di
  `App.onCreate`, sebelum ada activity, dan provisioning dibatasi satu proses satu
  kali.
- Papan ketik tidak muncul saat aplikasi dibuka. Sekarang muncul, sekali per
  peluncuran, bukan tiap kali kembali dari aplikasi lain.

### Berubah

- Live preview penyiapan rootfs ditaruh di tengah per baris. Sebelumnya rata kiri
  dengan sisa ruang menumpuk di kanan. Pemotongan barisnya sekarang diukur dengan
  paint milik view, bukan dihitung per karakter, karena perangkat bisa saja
  merender font monospace secara proporsional.
- Warna live preview jadi hijau tua (`#2E7D32`).
- README, CHANGELOG, dan sebagian besar dokumen ditulis ulang supaya lebih enak
  dibaca.

### Dihapus

- Catatan kerja bertanggal yang tidak dirujuk siapa pun:
  `docs/CHANGES-storage-visibility-2026-08-11.md`,
  `docs/DRAC-Xterm-Terminal-Settings-Integration.md`,
  `docs/OLLAMA-BASELINE-2026-08-15.txt`, `docs/OLLAMA-BUILD-FIX-2026-08-15.md`.
- `docs/IZZYONDROID-SUBMISSION.md` dan `docs/izzyondroid-request.md`. Project ini
  tidak lagi diarahkan ke IzzyOnDroid, dan isinya juga sudah salah: keduanya masih
  menggambarkan dua applicationId yang sudah diganti ADR-0003.
- Tiga screenshot yang tidak dipakai di mana pun, dan `local.properties.template`.
- `scripts/verify-izzy-metadata.py` jadi `scripts/verify-metadata.py`. Aturan yang
  diperiksanya tetap berguna, cuma namanya tidak lagi menyebut toko yang tidak
  dituju.
- `docs/OLLAMA-INTEGRATION-2026-08-15.md` jadi `docs/OLLAMA.md`.

## [1.0.0] - 2026-08-23, paket `com.dracxterm`

Satu aplikasi, dua installer. Nama, paket, dan versinya sama; yang berbeda cuma
cara image Linux-nya sampai ke perangkat. Alasan dan biayanya ada di
`docs/adr/0003-one-app-two-installers.md`.

### Ditambahkan

- Flavour Gradle `online` (±9 MB) dan `offline` (±45 MB) pada dimensi `installer`.
  Karena keduanya `com.dracxterm` pada versionCode yang sama, Android
  memperlakukannya sebagai satu aplikasi: memasang salah satu menggantikan yang
  lain di tempat.
- `app/src/main/assets/rootfsURLS.json`, katalog image yang dikunci per ABI
  Android. Menambah distribusi jadi perubahan data, bukan perubahan kode.
  `RootfsCatalog` membacanya saat runtime dan hanya mengembalikan entri yang cocok
  dengan `Build.SUPPORTED_ABIS`, karena PRoot tidak bisa menjalankan rootfs untuk
  arsitektur lain.
- Dua belas image untuk arm64: Debian 13, Kali NetHunter (full, nano, minimal),
  Ubuntu 22.04, 24.04, dan 25.10, Deepin, Pardus, Trisquel, Alpine, dan Void.
  Semuanya membawa SHA-256, dan entri tanpa digest 64 hex yang sah dibuang saat
  katalog dibaca.
- Bahasa Indonesia sebagai tampilan default, dengan tombol untuk pindah ke
  Inggris.
- Live preview saat penyiapan rootfs, menggantikan subjudul statis.

### Berubah

- Layar persetujuan menampilkan arsitektur perangkat sebelum daftar distribusi,
  supaya penyaringannya kelihatan dan bukan misteri.
- Tombol "Continue with BusyBox" dihapus dari layar persetujuan. Tombol itu masih
  ada kalau unduhan gagal, sebagai jalan keluar.
- Banner dipaskan ke 78 kolom dengan lantai font 7,5dp, dan langkah zoom
  dikuantisasi supaya tombol perbesar/perkecil selalu terasa.

### Catatan

Parrot OS diminta tapi tidak ada di daftar. Image arm64 resminya cuma manifest
Docker Hub yang butuh token, jadi tidak ada URL statis untuk dikunci.

---

Catatan di bawah ini ditulis dengan penomoran lama dan tidak pernah dirilis.
Disimpan karena menjelaskan asal-usul beberapa keputusan.

## [1.1.0] - 2026-08-21, tidak pernah dirilis

### Berubah

- **Root filesystem Linux tidak lagi ikut di dalam APK.** `assets/rootfs/` dikirim
  kosong. Aplikasi menjalankan shell BusyBox secara default dan menawarkan,
  sekali, untuk mengunduh image ARM64 setelah persetujuan eksplisit. Alasannya ada
  di `docs/adr/0001-rootfs-delivery.md`.
- Penandatanganan rilis mencari keystore lewat properti Gradle `DRACOS_STORE_FILE`,
  bukan path di dalam repo.
- Nama tampilan jadi `drac-Xterm`. Sebelumnya `xterm`, yang bentrok dengan
  emulator terminal X11 bernama sama.
- `ndkVersion` dikunci ke 27.0.12077973. Tanpa itu AGP mengompilasi engine C++
  dengan NDK apa pun yang kebetulan terpasang, jadi kode native yang dikirim
  bergantung pada siapa yang membangunnya.
- `.gitignore` memblokir arsip rootfs, keluaran APK/AAB, dan seluruh
  `assets/rootfs/` kecuali README-nya.
- `activity_provisioning.xml` dibungkus `ScrollView` supaya teks persetujuan tetap
  terbaca di layar pendek dan pada skala font besar.

### Ditambahkan

- `RootfsCatalog`, URL unduhan dan digest SHA-256 yang dikunci, memakai path rilis
  Kali yang tidak berubah, bukan direktori `current/` yang bergulir.
- `RootfsDownloader`, HTTPS saja, bisa dilanjutkan, menulis ke `.part` dan baru
  memindahkan berkas setelah digest-nya cocok.
- `RootfsArchive.Source`, jadi pipeline penyiapan membaca arsip dari aset APK atau
  dari berkas di perangkat dengan cara yang sama.
- Layar persetujuan di `ProvisioningActivity`. Menolak akan diingat dan berujung
  ke BusyBox.
- `fastlane/metadata/android/{en-US,id}/`, deskripsi, changelog, ikon, screenshot.
- `docs/THIRD-PARTY-BINARIES.md` dan `licenses/`, berisi asal, versi, checksum,
  lisensi, dan tawaran sumber tertulis untuk tiap biner prebuilt di dalam APK.
- `docs/SECURITY-KEY-ROTATION.md` dan `scripts/purge-keystore-history.sh`,
  penanganan untuk keystore penandatanganan yang sempat masuk repo publik.
- `.github/workflows/release.yml`, build rilis bertanda tangan dari commit yang
  di-tag. Menolak menerbitkan kalau tag tidak sama dengan `versionName`, kalau ada
  locale tanpa changelog untuk versionCode yang dirilis, kalau uji engine gagal,
  atau kalau tanda tangannya tidak terverifikasi.
- `.github/workflows/ci.yml`, uji engine, pemeriksaan metadata, dan build debug
  penuh di tiap push dan pull request. Tidak ada bahan penandatanganan yang
  terekspos, jadi fork bisa menjalankannya.
- `LICENSE` (Apache-2.0) di akar repo, dan `licenses/BSD-3-Clause.txt` dengan baris
  hak cipta android-shmem yang sebenarnya. `NOTICE` merujuk keduanya dan keduanya
  tidak ada.

### Ditambahkan, biner prebuilt yang reproducible

- `prebuilts/`. Tiap biner ARM64 di dalam APK kini bisa dibangun ulang dari sumber
  upstream yang dikunci dengan NDK yang dikunci: BusyBox, talloc, PRoot dan
  loader-nya, serta libandroid-shmem. Sebelumnya tidak satu pun bisa direproduksi
  dari repo ini, yang membuat verifikasi reproducible-build mustahil dan
  meninggalkan BusyBox dari 2018 tanpa cara menggantinya.
- Kelima artefak identik bit demi bit lintas direktori build, dibuktikan dengan
  membangun seluruh set dua kali ke path dengan panjang berbeda. Dua penyebab
  perbedaan harus diperbaiki dulu: clang mencatat path sumber absolut
  (`-ffile-prefix-map`) dan BusyBox menstempel jam dinding ke banner `--help`-nya.
- CI membangunnya ulang tiap minggu dan memeriksa lagi reproducibility-nya, jadi
  resepnya tidak diam-diam membusuk melawan empat upstream yang bergerak.
- Enam patch, masing-masing membawa alasannya. BusyBox masih menganggap bionic
  tidak punya `strchrnul`, `getsid`, `sethostname`, dan `adjtimex` seperti pada
  2012, yang merusak static link. PRoot tidak menyertakan `<string.h>` di ekstensi
  ashmem-nya dan butuh ekstensi awk khas gawk untuk dibangun. android-shmem tidak
  menyertakan `<fcntl.h>` dan menuliskan `$PREFIX/tmp` milik Termux ke dalam
  `_PATH_TMP`, yang sekarang dibaca dari `TMPDIR` saat runtime.
- Koreksi untuk `docs/THIRD-PARTY-BINARIES.md`: `libandroid-shmem.so` yang dikirim
  berasal dari `termux/libandroid-shmem`, bukan `pelya/android-shmem`. Ekstensi
  sysvipc PRoot memanggil `libandroid_shmat_fd()`, yang hanya ada di fork itu,
  jadi upstream yang tertulis tidak mungkin menghasilkan biner yang dikirim.

### Berubah, APK kini mengirim biner yang dibangun dari sumber

- `app/src/main/jniLibs/arm64-v8a/` tidak lagi berisi build warisan Termux dan
  build 2018. Kelima biner adalah keluaran `prebuilts/build.sh`, dengan nilai
  SHA-256 tercatat di `docs/THIRD-PARTY-BINARIES.md` dan `NOTICE` disesuaikan.
- **BusyBox 1.29.3 (2018) ke 1.38.0.** Tujuh tahun perbaikan upstream, termasuk
  CVE yang membuat build lama jadi utang keamanan yang menganggur. Set applet-nya
  tidak sama persis: `ifconfig`, `route`, `netstat`, `ip`, `hush`, `logname`,
  `swapon`/`swapoff`, dan perkakas SysV IPC hilang, masing-masing karena tidak
  bisa dikompilasi terhadap bionic. Alasannya dicatat per opsi di
  `prebuilts/busybox/android.fragment`.
- **PRoot bukan lagi biner Termux.** Yang lama menanamkan
  `/data/data/com.termux/files/usr/lib` dan `…/libexec/proot/loader`, yang diakali
  `Bootstrap` saat runtime. Penggantinya tidak memuat path Termux sama sekali.
- Diuji di perangkat sebelum dikirim: 20 pemeriksaan di Infinix X6726B (Android
  15, arm64-v8a), mencakup shell BusyBox dan coreutils, tar bolak-balik, dua
  akselerator bawaan PRoot, dan masuk ke rootfs minimal untuk menjalankan shell,
  membaca lewat bind mount, dan memastikan `uid=0`. Bisa diulang dengan
  `./scripts/release-prep.sh device-test`.

### Keamanan

- `OllamaInstaller.download()` mengikuti header `Location` sebuah redirect tanpa
  memeriksa skemanya, jadi redirect ke `http://` akan mengambil payload
  executable dalam bentuk polos. `RootfsDownloader` sudah menolak tiap lompatan
  non-HTTPS; keduanya kini berperilaku sama. Pin SHA-256 membuat ini tidak pernah
  jadi eksekusi kode jarak jauh, tapi ia membocorkan apa yang sedang diunduh dan
  bertentangan dengan perilaku jaringan yang dijanjikan aplikasi. Loop yang sama
  juga meneruskan `Location` yang mungkin relatif ke `URL(String)`, yang melempar
  exception; sekarang diresolusi terhadap URL saat ini.
- `docs/PRIVACY-AUDIT.md`, tiap titik panggilan jaringan dan tiap titik permintaan
  izin ditelusuri sampai ke pemicunya, supaya "tanpa telemetri" dan "tidak pernah
  diminta saat startup" bisa diperiksa, bukan dipercaya begitu saja.

### Diperbaiki

- `RootfsArchive.sizeBytes` tidak bisa dikompilasi: cabang `Source.Asset` membaca
  subjek `when` dari dalam lambda `runCatching`, tempat smart cast tidak berlaku.
  Tidak ada apa pun di repo ini yang bisa dibangun sampai ini diperbaiki.
- Kredensial penandatanganan rilis divalidasi saat *configuration* Gradle, yang
  membuat tiap perintah gagal tanpa kredensial itu, termasuk `assembleDebug` dan
  lint. Pemeriksaannya kini berjalan terhadap graf task dan hanya menyala kalau
  artefak rilis benar-benar dibangun.

### Dihapus

- `app/src/main/assets/rootfs/kali-nethunter-rootfs-nano-arm64.tar.xz`
  (206.922.656 byte) dan aturan pelacakan Git LFS-nya.

### Batasan yang diketahui

Menolak image Linux akan diingat dan belum ada cara di dalam aplikasi untuk
membuka tawarannya lagi. Menghapus data aplikasi mengembalikan pertanyaannya.

## [1.0.1] - 2026-08-21, tidak pernah dirilis

### Ditambahkan

- `kali-nethunter-rootfs-nano-arm64.tar.xz` ikut dikirim untuk perangkat Android
  ARM64 di `app/src/main/assets/rootfs/`.
- Pelacakan Git LFS untuk rootfs yang ikut dikirim, supaya repo tetap bisa
  di-clone sambil mempertahankan path aset rilis yang diperlukan.
