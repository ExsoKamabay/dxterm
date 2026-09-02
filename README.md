# dracxterm

Emulator terminal untuk Android ARM64. Mesin ANSI/VT, PTY, dan render UTF-8-nya
ditulis dari nol dengan C++; antarmukanya Kotlin. Tidak perlu root.

Bisa dipakai apa adanya sebagai shell BusyBox, atau menjalankan distribusi Linux
lewat PRoot di dalam sandbox aplikasi. Tujuannya satu: memberi shell yang benar
di ponsel, tanpa meminta root, akun, atau layanan apa pun.

## Screenshot

| | |
|---|---|
| ![Pemilihan distro Linux](Screenshot/01-pilih-distro-linux-dracxterm.jpg) | ![Layar instalasi Debian 13](Screenshot/02-instalasi-debian-13-dracxterm.jpg) |
| 1. Pemilihan distro. Aplikasi membaca ABI perangkat dan hanya menampilkan image yang cocok. | 2. Layar instalasi Debian 13, dengan ukuran unduhan dan ruang penyimpanan yang dibutuhkan. |
| ![Menu xset](Screenshot/03-konfigurasi-tampilan-terminal-dracxterm.jpg) | ![Debian berjalan di terminal](Screenshot/04-terminal-debian-berjalan-dracxterm.jpg) |
| 3. `xset`, pengaturan tampilan terminal dengan pratinjau langsung. | 4. Debian berjalan di dalam terminal, dilihat lewat `screenfetch`. |

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
| Backup | Ekspor dan impor konfigurasi sebagai JSON, serta reset ke bawaan. |
| About | Informasi perangkat dan aplikasi, serta alamat kontak pengembang. |

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

## Yang dibutuhkan

- Android 7.0 (API 24) atau lebih baru.
- Prosesor ARM64. APK hanya memuat biner `arm64-v8a`; perangkat 32-bit
  (`armeabi-v7a`) dan x86 tidak didukung.
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

Lima biner ARM64 ikut dikirim di dalam APK: BusyBox, PRoot dan loader-nya,
talloc, dan libandroid-shmem. Rinciannya di
[Biner yang ikut dikirim](#biner-yang-ikut-dikirim).

## Versi dan paket

Rilis saat ini 1.0.0, `versionCode` 1, dengan `applicationId` `com.xdrac`.

## Unduh dan pasang

Ambil APK dari [Releases](https://github.com/ExsoKamabay/dxterm/releases).
Tiap berkas punya `.sha256` di sebelahnya. Periksa dulu sebelum memasang:

```bash
sha256sum -c dracxterm-1.0.0-vc1-release.apk.sha256
```

Lalu pasang lewat `adb`:

```bash
adb install -r dracxterm-1.0.0-vc1-release.apk
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

| Distro | Unduhan | Terpasang | Sumber |
|---|---|---|---|
| Debian 13 (trixie) | 35 MB | 400 MB | `easycli.sh` |
| Kali Linux (nano) | 198 MB | 1,0 GB | `kali.download` |
| Kali Linux (minimal) | 137 MB | 1,0 GB | `kali.download` |
| Kali Linux (full pentesting) | 1,8 GB | 9,5 GB | `kali.download` |

Debian 13 adalah pilihan default. URL, digest, dan catatan tiap image ada di
[`rootfsURLS.json`](app/src/main/assets/rootfsURLS.json), termasuk penjelasan
soal asal-usul tiap sumber.

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

### Perkakas yang dibutuhkan

JDK 17 dan Android SDK command-line tools. Android Studio tidak wajib.

```bash
export ANDROID_HOME="/path/ke/Android/Sdk"
sdkmanager "platforms;android-36" "build-tools;35.0.0" \
           "ndk;27.0.12077973" "cmake;3.22.1"
```

Versi NDK dan CMake dipin di [`app/build.gradle.kts`](app/build.gradle.kts), jadi
mesin developer dan mesin lain menghasilkan kode native yang sama.

Build debug tidak butuh keystore:

```bash
./gradlew assembleDebug
```

Ada satu varian saja: `assembleDebug`, `assembleRelease`, dan `bundleRelease`.
Tidak ada product flavor.

Uji mesin terminalnya. Ini tidak butuh SDK maupun perangkat:

```bash
./native-tests/run-tests.sh
```

### Menyiapkan keystore

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

### Build APK release

```bash
./gradlew assembleRelease
```

Hasilnya `app/build/outputs/apk/release/app-release.apk`, sudah ditandatangani
dan sudah lewat `lintVitalRelease`. Periksa sebelum dipakai:

```bash
BT="$ANDROID_HOME/build-tools/35.0.0"
"$BT/apksigner" verify --print-certs app/build/outputs/apk/release/app-release.apk
"$BT/zipalign" -c -p 4 app/build/outputs/apk/release/app-release.apk
```

`apksigner` harus melaporkan skema v2 dan v3 sebagai `true`, dan `zipalign`
harus keluar tanpa pesan. APK inilah yang dipasang ke perangkat dan dilampirkan
ke GitHub Release.

### Build App Bundle release

```bash
./gradlew bundleRelease
```

Hasilnya `app/build/outputs/bundle/release/app-release.aab`. Format ini hanya
untuk diunggah ke Play Console; Android tidak bisa memasangnya langsung.
Validasi dengan [bundletool](https://github.com/google/bundletool):

```bash
bundletool validate --bundle=app/build/outputs/bundle/release/app-release.aab
```

Bundle-nya sengaja tidak memecah resource per bahasa (`bundle { language {
enableSplit = false } }`), karena aplikasi punya toggle bahasa sendiri dan kedua
terjemahan harus selalu ada di perangkat.

Untuk mencoba hasil bundle di perangkat, `bundletool` bisa menurunkannya jadi
APK set:

```bash
bundletool build-apks --bundle=app/build/outputs/bundle/release/app-release.aab \
                      --output=/tmp/dracxterm.apks --local-testing
bundletool install-apks --apks=/tmp/dracxterm.apks
```

### Membangun keduanya sekaligus

`scripts/release-build.sh` menjalankan seluruh langkah di atas dalam satu
perintah: build APK dan AAB, verifikasi tanda tangan dan alignment, validasi
bundle, lalu menyalin keduanya ke `apps/` dengan nama berversi dan berkas
`.sha256` masing-masing.

```bash
./scripts/release-build.sh
```

`apps/` ada di `.gitignore` dan tidak pernah ikut ter-commit. Apa isinya dan apa
beda kedua format itu dijelaskan di `apps/APPS.md`.

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

### Pemeriksaan lain

`scripts/verify-metadata.py` memeriksa hal-hal yang bisa diperiksa mesin:
identitas aplikasi, panjang teks Fastlane, ukuran dan rasio screenshot, serta
apakah ada material penandatanganan atau artefak build yang ikut terlacak Git.

```bash
python3 scripts/verify-metadata.py
./scripts/verify-rootfs-urls.sh           # cocokkan digest katalog dengan sumbernya
```

## Biner yang ikut dikirim

APK memuat lima biner ARM64: BusyBox, PRoot dan loader-nya, talloc, dan
libandroid-shmem. Semuanya dibangun dari sumber oleh `prebuilts/build.sh` dari
revisi upstream yang dipin. Hasilnya reproducible: digest yang sama keluar dari
direktori build mana pun.

Versi upstream, lisensi, dan written offer untuk source code-nya ada di
[`NOTICE`](NOTICE). Resep build tiap biner ada di
[`prebuilts/`](prebuilts/README.md).

## Keterbatasan yang diketahui

- **ARM64 saja.** Perangkat `armeabi-v7a` dan x86 tidak didukung karena biner
  yang ikut dikirim hanya dibangun untuk `arm64-v8a`.
- **Backup Android aktif.** `android:allowBackup` masih `true` dan aplikasi
  belum punya aturan pengecualian. Artinya data aplikasi, termasuk isi rootfs
  dan riwayat shell, bisa ikut terbawa oleh backup atau transfer perangkat.
  Matikan backup untuk aplikasi ini lewat Settings kalau Anda mengetik hal
  sensitif di terminal.
- **Provenance image Debian lebih lemah dari Kali.** Kali menerbitkan
  `SHA256SUMS` sendiri, jadi digest-nya bisa diturunkan ulang dari sumber
  resminya. Image Debian datang dari mirror pihak ketiga `easycli.sh`, dan
  digest-nya sudah tidak bisa dicocokkan dengan otoritas independen. Digest
  yang dipin tetap menahan substitusi di kemudian hari, tapi rantai
  kepercayaannya lebih pendek.
- **Tidak ada unit test JVM dan tidak ada CI di repo ini.** Yang tersedia hanya
  suite C++ untuk mesin terminal di `native-tests/` dan lint Android.
- **Ollama berjalan lewat PRoot**, jadi kecepatannya di bawah biner native.

## Lisensi

Apache-2.0, lihat [`LICENSE`](LICENSE).

Biner pihak ketiga punya lisensinya sendiri: GPL-2.0 untuk BusyBox dan PRoot,
LGPL-3.0 untuk talloc, dan BSD-3-Clause untuk libandroid-shmem. Font JetBrains
Mono, Copse, dan Black Ops One memakai SIL Open Font License 1.1. Teks
lengkapnya ada di [`licenses/`](licenses/) dan rinciannya di
[`NOTICE`](NOTICE), termasuk written offer untuk source code biner GPL.

Image Linux yang diunduh tunduk pada lisensi distribusinya masing-masing, bukan
pada lisensi dracxterm.
