# Virtual Hardware Driver Platform (VHDP)

VHDP menjalankan root filesystem (rootfs) distro Linux, misalnya Debian, Ubuntu,
atau Alpine, di atas host Linux atau Android tanpa hak root. Project ini terdiri
dari library `libvhdp` dan program baris perintah `phdp`. Program di dalam rootfs
berjalan sebagai proses biasa milik pengguna host. Sebuah supervisor berbasis
`ptrace` mencegat syscall mereka, menerjemahkan setiap path sehingga rootfs
terlihat sebagai `/`, dan memalsukan identitas yang dilihat guest bila diminta.

Versi 1.0.0, C ABI versi 1, lisensi Apache-2.0.

## Daftar isi

1. [Tujuan project](#tujuan-project)
2. [Fitur](#fitur)
3. [Cara kerja](#cara-kerja)
4. [Persyaratan](#persyaratan)
5. [Build dan test](#build-dan-test)
6. [Menyiapkan rootfs](#menyiapkan-rootfs)
7. [Menjalankan rootfs dengan phdp](#menjalankan-rootfs-dengan-phdp)
8. [Diagnostik](#diagnostik)
9. [Menyematkan libvhdp](#menyematkan-libvhdp)
10. [Build untuk Android](#build-untuk-android)
11. [Masalah umum](#masalah-umum)
12. [Struktur project dan deskripsi file](#struktur-project-dan-deskripsi-file)
13. [Dokumentasi lain](#dokumentasi-lain)
14. [Lisensi](#lisensi)

## Tujuan project

VHDP dibuat untuk menjalankan userland distro Linux dari sebuah direktori biasa,
tanpa root, tanpa `mount`, dan tanpa namespace kernel. Kode di project ini
mengejar tiga hal:

1. Shell interaktif dan tool CLI distro (bash, coreutils, apt/dpkg, procps)
   berjalan seperti di sistem aslinya: file, symlink, hardlink, pipe, PTY,
   signal, job control, dan proses anak.
2. Program lain bisa menyematkan mesin yang sama lewat C ABI yang stabil,
   termasuk aplikasi Android dengan targetSdk 29 ke atas yang tidak boleh
   meng-`execve` file dari penyimpanannya sendiri.
3. Batas kemampuan dilaporkan apa adanya. `phdp doctor` memeriksa host,
   `phdp inspect` memeriksa rootfs, dan `phdp capabilities` mencetak matriks yang
   setiap klaim `supported`-nya menunjuk ID test.

Batas yang perlu diketahui sejak awal:

- Guest harus berarsitektur sama dengan host (x86_64 di x86_64, aarch64 di
  aarch64). VHDP tidak mengemulasikan CPU.
- Tidak ada kernel guest. Syscall dijalankan kernel host setelah diterjemahkan,
  jadi modul kernel, systemd penuh, cgroup, dan mount sungguhan tidak tersedia.
- Isolasi yang diberikan adalah isolasi kompatibilitas (penerjemahan path), tidak
  sama dengan sandbox keamanan. Jangan menjalankan rootfs yang tidak Anda
  percayai. Rinciannya ada di [SECURITY.md](SECURITY.md).
- Engine `rooted`, `emulator`, dan `vm` belum diimplementasikan. Ketiganya
  melaporkan status `backend-required` dan tidak pernah berpura-pura jalan.

## Fitur

- Engine rootless: satu thread supervisor per sesi, `PTRACE_SEIZE` dengan
  pelacakan fork, vfork, clone, dan exec. `seccomp` dipakai sebagai akselerator
  yang hanya meloloskan syscall pass-through; bila probe seccomp gagal, engine
  memakai `PTRACE_SYSCALL` penuh dengan hasil yang sama.
- Penerjemahan path komponen demi komponen di ruang guest. Symlink absolut
  ditafsirkan relatif terhadap root guest, `..` tidak bisa naik di atas `/`
  guest, dan loop symlink menghasilkan `ELOOP`.
- Tabel klasifikasi untuk setiap nomor syscall arsitektur target. Di x86_64:
  235 pass-through, 69 diterjemahkan, 15 diemulasikan, 49 ditolak, 5 tidak
  didukung. Nomor yang tidak dikenal ditolak dengan `ENOSYS`.
- Exec program guest: ELF statis, ELF dinamis lewat dynamic loader milik rootfs,
  script `#!`, penolakan ELF arsitektur lain, dan loader userland `vhdp-loader`
  untuk host yang melarang `execve` dari penyimpanan yang bisa ditulis.
- Identitas guest (`--uid`, `--gid`) dengan kredensial per proses. Host tidak
  pernah memberi hak tambahan, dan `no_new_privs` selalu aktif.
- Proyeksi `/dev` minimal, `/proc` host read-only, bind direktori host
  (read-only kecuali `:rw`), `--network none`, dan `--read-only-rootfs`.
- PTY, relay signal, job control dengan `PTRACE_LISTEN`, serta batas waktu yang
  membunuh seluruh pohon proses guest.
- Adaptasi untuk host berpolicy ketat seperti domain aplikasi Android: emulasi
  hardlink, pengganti berkas `/proc` global, jawaban SIGSYS dari filter seccomp
  host, dan restart syscall legacy x86_64 sebagai padanan `*at`.
- Fungsi bantu rootfs di C ABI: konfigurasi boot pertama, rencana pemulihan
  dpkg, dan rencana proyeksi `/dev`, `/proc`, `/sys`.
- C ABI stabil dengan allowlist simbol yang diuji, wrapper C++20 RAII
  header-only, dan event JSON Lines.

## Cara kerja

### Lapisan

```text
     phdp (CLI)                    program Anda (C / C++)
        |                                   |
        +--> include/vhdp/vhdp.hpp  <-------+   wrapper C++20, header-only
                       |
             include/vhdp/vhdp.h                C ABI stabil (vhdp_*)
                       |
             src/capi/capi.cpp                  validasi handle/struct, tahan exception
                       |
             src/core                           context, config, session, lifecycle,
                       |                        pemilihan engine, laporan
          +------------+--------------+
          |                           |
  src/engines/rootless        src/engines/unavailable
  supervisor ptrace+seccomp,  rooted / emulator / vm
  handler syscall, loader     (backend-required)
          |
          +-- src/vfs          mount table, resolver path guest
          +-- src/linux_abi    decoder ELF, tabel syscall, tabel errno
          +-- src/arch         akses register per arsitektur, gateway raw syscall
          +-- src/platform     probe host, procfs, konstanta ptrace
          +-- vdr/             driver: bind host, /dev minimal, /proc host, PTY, network
          +-- hdr/             registry "hardware" yang dilihat guest
```

CLI tidak punya logika engine sendiri. `phdp` hanya memanggil wrapper C++, yang
memanggil C ABI, sama persis dengan program lain yang menyematkan `libvhdp`.

### Alur `phdp run`

```text
phdp run [OPSI] ROOTFS -- PERINTAH [ARG...]
 |
 1  cli/cli_args.cpp        parse argumen; opsi salah -> exit 2
 2  cli/cli_run.cpp         tolak bila phdp sendiri sedang di-ptrace (TracerPid != 0),
 |                          pasang relay signal, mode raw terminal bila --pty
 3  vhdp_config_* (capi)    opsi CLI disalin ke SessionConfig
 4  core/config.cpp         validate_config: realpath rootfs (menolak "/"), cek bind,
 |                          cwd, env; env host hanya TERM COLORTERM LANG LC_ALL TZ
 5  core/session.cpp        vhdp_session_start: state created -> starting
 6  core/engine_select.cpp  auto memilih rootless bila probe lolos:
 |                          ptrace ke child, seccomp, PTRACE_GET_SYSCALL_INFO, loader
 7  rootless_engine.cpp     buat instance; buka PTY lewat vdr bila --pty
 8  supervisor.cpp          thread supervisor:
 |    - vdr::build_mount_table   rootfs + bind + /dev + /proc
 |    - resolve cwd dan program di ruang guest (PATH guest untuk nama tanpa '/')
 |    - exec_resolver: rencana exec (shebang, loader dinamis, cek ABI)
 |    - clone() -> bootstrap.c di child (hanya raw syscall): reset signal,
 |      setsid/TIOCSCTTY bila PTY, susun stdio, tunggu tracer lewat pipe,
 |      no_new_privs, filter seccomp, execve
 |    - PTRACE_SEIZE (TRACESYSGOOD|TRACEFORK|TRACEVFORK|TRACECLONE|TRACEEXEC|EXITKILL)
 9  loop waitpid            setiap stop:
 |    - syscall-stop        -> syscall_handlers.cpp (lihat alur berikut)
 |    - event fork/clone    -> proses baru ikut dilacak, kredensial diwarisi
 |    - event exec          -> serah terima ke vhdp-loader bila mode loader
 |    - group-stop          -> PTRACE_LISTEN (Ctrl+Z dan SIGTTOU benar)
 |    - SIGSYS dari policy host -> dijawab supervisor
10  proses awal keluar      sisa pohon proses dibunuh SIGKILL dan ditunggu habis
11  core/session.cpp        state -> exited / failed / cancelled, ExitResult dibentuk
12  phdp                    exit dengan shell_status guest
```

Status keluar mengikuti konvensi shell: kode keluar guest, `128+N` bila guest
mati oleh signal `N`, `124` bila `--timeout` habis, `126` bila perintah tidak
bisa dijalankan, `127` bila perintah atau loader tidak ditemukan, `125` bila sesi
gagal dimulai atau dibatalkan, dan `2` untuk kesalahan argumen.

### Alur satu syscall

```text
guest: openat(AT_FDCWD, "/etc/hostname", O_RDONLY)
 |
 v
kernel host: filter seccomp tidak meloloskan openat -> SECCOMP_RET_TRACE
 |           (tanpa seccomp: setiap syscall menghasilkan syscall-stop)
 v
supervisor menerima stop dari waitpid
 |- arch/regs_*.cpp           baca nomor syscall dan argumen dari register
 |- linux_abi/syscall_table   tentukan kelas syscall
 |
 |- pass_through  -> lanjutkan tanpa perubahan
 |- denied        -> hasil diganti -errno tetap, event "diagnostic" dikirim
 |- unsupported   -> sama seperti denied, dengan pesan bahwa emulasinya belum ada
 |- emulated      -> supervisor menjawab sendiri (keluarga get*id / set*id)
 |- translated:
 |     tracee_mem      baca string path dari memori guest (dibatasi PATH_MAX)
 |     vfs/resolver    resolve per komponen: /etc -> <rootfs>/etc -> lstat ...
 |     mount_table     prefix terpanjang menang: rootfs, bind, /dev, /proc
 |     tulis path host di bawah stack pointer guest, ganti register argumen
 |     lanjutkan syscall
 v
syscall-exit stop: register asli dipulihkan, hasil diperbaiki bila perlu
 |                 (uid/gid pada stat, st_nlink hardlink emulasi, getcwd, readlink)
 v
guest menerima hasil seolah rootfs adalah "/"
```

### Alur exec program guest

```text
guest: execve("/usr/bin/ls", argv, envp)
 |
exec_resolver.cpp
 |- resolve path di rootfs, baca header ELF (linux_abi/elf.c, dengan batas ukuran)
 |- script "#!"          -> interpreter dicari di rootfs (kedalaman dibatasi)
 |- e_machine lain       -> ENOEXEC + diagnostic "ABI mismatch"
 |- ELF dinamis          -> PT_INTERP di-resolve di dalam rootfs, bukan di host
 |
 |- mode biasa (host Linux):
 |     execve(<rootfs>/lib64/ld-linux-x86-64.so.2 [--argv0 ls] /usr/bin/ls ...)
 |     loader milik guest membuka program lewat open() yang diterjemahkan
 |
 |- mode loader (domain aplikasi Android, atau VHDP_LOADER di-set):
       execve(vhdp-loader) dari direktori pustaka native
       PTRACE_EVENT_EXEC: supervisor menaruh vhdp_load_request di bawah stack baru
       vhdp-loader memetakan program dan dynamic linker-nya dengan mmap(PROT_EXEC),
       merapikan auxv, lalu lompat ke entry point
```

Di mode biasa `/proc/self/exe` menunjuk ke loader dinamis. Di mode loader ia
menunjuk ke program guest.

## Persyaratan

- Linux x86_64 atau aarch64.
- CMake 3.22 atau lebih baru, dan Ninja.
- Compiler C17 dan C++20:
  - preset `host-debug` dan `host-release` memakai Clang dari `PATH`; bila
    `clang` tidak ada, host Clang dari Android NDK yang dipin (`29.0.14206865`)
    dipakai;
  - preset `host-asan-ubsan` dan `host-tsan` memakai GCC;
  - tanpa preset, CMake memakai compiler bawaan sistem.
- Untuk build Android: Android NDK `29.0.14206865` (lihat
  [docs/ANDROID_BUILD.md](docs/ANDROID_BUILD.md)).
- Opsional: `clang-format` dan `clang-tidy` untuk target `format-check` dan
  `tidy`, Clang dengan libFuzzer untuk target fuzz.

Kombinasi yang sudah diverifikasi untuk rilis ini: Linux 6.17 x86_64, CMake
3.28.3, Ninja 1.11.1, dengan Clang 21 dari NDK 29 (preset `host-debug`) dan GCC
13.3 (tanpa preset). Keduanya lulus 72 dari 72 test.

```sh
sudo apt install build-essential cmake ninja-build   # Debian/Ubuntu
```

## Build dan test

Dengan preset:

```sh
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug
```

Tanpa preset, memakai compiler bawaan:

```sh
cmake -S . -B build/local -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/local --parallel
ctest --test-dir build/local
```

Hasil yang diharapkan: `100% tests passed, 0 tests failed out of 72` (28 test
unit, 40 test integrasi, 1 test simbol ABI, 2 test header, dan 1 test penyiapan
fixture). Hasil build ada di `build/<preset>/src/`:

```text
phdp                   CLI
libvhdp.so             library bersama (hanya simbol vhdp_* yang diekspor)
libvhdp.a              library statis
vhdp-loader            loader ELF userland (static PIE, tanpa libc)
```

Preset yang tersedia: `host-debug`, `host-release`, `host-asan-ubsan`,
`host-tsan`, `android-arm64-release`, `android-x86_64-debug`. Folder `build/`
tidak ikut disimpan di project; ia dibuat ulang oleh perintah di atas.

Opsi CMake:

| Opsi | Default | Fungsi |
|---|---|---|
| `VHDP_BUILD_CLI` | `ON` | Membangun `phdp`, parser-nya, dan `vhdp-loader`. |
| `VHDP_BUILD_TESTS` | `ON` di host, `OFF` di Android | Test unit, integrasi, fuzz, benchmark. Butuh `VHDP_BUILD_CLI=ON` dan `VHDP_BUILD_EXAMPLES=ON`. |
| `VHDP_BUILD_EXAMPLES` | `ON` di host, `OFF` di Android | Contoh embedding C dan C++. |
| `VHDP_ENABLE_ASM` | `ON` | Memakai rutin assembly `raw_syscall.S`. |
| `VHDP_FORCE_PORTABLE` | `OFF` | Memaksa implementasi C portabel untuk raw syscall. |
| `VHDP_WERROR` | `ON` | Warning menjadi error untuk kode milik project. |
| `VHDP_SANITIZE` | kosong | Daftar sanitizer untuk `-fsanitize=`. |
| `VHDP_TEST_LAUNCHER` | kosong | Prefix perintah untuk menjalankan test (preset TSan memakai `setarch -R`). |
| `VHDP_ENABLE_ROOTED` | `OFF` | Engine rooted belum ada; `ON` menggagalkan configure. |
| `VHDP_LLVM_BIN_DIR` | otomatis | Direktori `clang-format`, `clang-tidy`, `llvm-nm`. |
| `VHDP_ANDROID_NDK`, `VHDP_NDK_VERSION` | otomatis, `29.0.14206865` | Lokasi dan versi NDK yang dipin. |

Target tambahan: `format`, `format-check`, `tidy`, dan `install` (library,
`phdp`, header, serta paket CMake `VHDP::vhdp`).

## Menyiapkan rootfs

Rootfs adalah direktori berisi root filesystem Linux untuk arsitektur yang sama
dengan host. Syaratnya:

- bukan `/` host;
- berisi `/bin/sh` (atau `/bin/bash`, `/bin/ash`) untuk shell default, plus
  dynamic loader dan pustaka bersama bila programnya dinamis;
- berada di filesystem yang boleh mengeksekusi file (tidak `noexec`) dan menyimpan
  mode serta symlink POSIX.

VHDP tidak mengunduh apa pun. Ambil arsip rootfs resmi dari distro untuk
arsitektur yang benar, lalu ekstrak sebagai pengguna biasa:

```sh
mkdir -p ~/rootfs/debian
tar -xpf rootfs.tar.xz -C ~/rootfs/debian
```

Periksa hasilnya:

```sh
phdp inspect ~/rootfs/debian
```

Contoh keluaran untuk rootfs fixture milik test suite:

```text
Rootfs: .../build/host-debug/tests/rootfs
Filesystem: ext2/3/4, noexec=no, writable=yes
OS: VHDP Fixture 1
Shell /bin/sh -> /bin/vhdp-fixture [x86_64, ABI match]
  info    rootfs.layout              missing directory /usr
Status: ok
```

`inspect` melaporkan jenis filesystem, `noexec`, isi `os-release`, shell yang
ada, kecocokan ABI ELF, dan keberadaan dynamic loader. Status `error` berarti
rootfs tidak akan berjalan. Penjelasan lengkap ada di
[docs/ROOTFS.md](docs/ROOTFS.md).

Untuk boot pertama, program yang menyematkan `libvhdp` bisa memanggil
`vhdp_configure_rootfs` (lihat [Fungsi bantu rootfs](#fungsi-bantu-rootfs)).

## Menjalankan rootfs dengan phdp

```sh
P=./build/host-debug/src/phdp

$P run ~/rootfs/debian -- /bin/uname -a     # satu perintah
$P ~/rootfs/debian -- /usr/bin/id           # bentuk singkat, kata "run" boleh dilewati
$P ~/rootfs/debian                          # tanpa perintah: /bin/sh, /bin/bash, atau /bin/ash
```

Identitas root palsu di dalam guest, tanpa hak apa pun di host:

```sh
$P run --uid 0 --gid 0 ~/rootfs/debian -- /usr/bin/id
# uid=0(root) gid=0(root) groups=0(root)
```

Shell interaktif dengan terminal sendiri, `/proc`, dan `/dev`:

```sh
$P run --pty --proc host --dev minimal --uid 0 --gid 0 \
       --cwd /root ~/rootfs/debian -- /bin/bash -l
```

Bind direktori host dengan batas waktu:

```sh
mkdir -p ~/kerja
$P run --uid 0 --gid 0 --proc host --dev minimal \
       --bind ~/kerja:/work:rw --cwd /work --timeout 60 \
       ~/rootfs/debian -- /bin/sh -c 'apt-get update && touch /work/selesai'
```

Opsi `phdp run`:

| Opsi | Fungsi |
|---|---|
| `--engine auto\|rootless\|rooted\|emulator\|vm` | Pilihan engine. `auto` memilih rootless bila probenya lolos. |
| `--cwd GUEST_PATH` | Direktori kerja awal di ruang guest (default `/`). |
| `--env KEY=VALUE`, `-e` | Menambah atau mengganti variabel environment guest. |
| `--clear-env` | Mulai dari environment kosong. |
| `--bind HOST:GUEST[:ro\|rw]`, `-b` | Memasang direktori host ke guest, read-only kecuali `:rw`. |
| `--read-only-rootfs` | Penulisan ke rootfs gagal dengan `EROFS`. |
| `--network host\|none` | Jaringan host (default) atau tolak socket selain `AF_UNIX`. |
| `--uid N`, `--gid N` | Identitas yang dilihat guest. Tidak pernah memberi hak host. |
| `--timeout SECONDS` | Bunuh pohon proses guest setelah SECONDS (boleh pecahan, mis. `1.5`), exit 124. |
| `--pty` | Guest berjalan di pseudo-terminal baru. |
| `--seccomp auto\|on\|off` | Akselerasi seccomp. `on` gagal bila tidak tersedia, `off` memaksa `PTRACE_SYSCALL`. |
| `--dev minimal\|none` | Proyeksi `/dev` (default `minimal`: null, zero, full, random, urandom, tty, ptmx, pts). |
| `--proc none\|host` | Proyeksi `/proc` host read-only (default `none`). |
| `--trace` | Event trace untuk setiap syscall. |
| `--verbose`, `-v` | Tampilkan event informasi. |
| `--json` | Event sebagai JSON Lines di stderr (atau `--event-fd`). |
| `--event-fd N` | Tulis event ke descriptor `N` (`N >= 2`). |

Environment guest berisi `PATH`, `HOME`, dan nilai host untuk `TERM`,
`COLORTERM`, `LANG`, `LC_ALL`, `TZ`, kecuali `--clear-env` dipakai. Variabel host
lain tidak diteruskan.

Nama subcommand (`doctor`, `inspect`, `capabilities`, `run`, `help`, `version`)
di posisi pertama selalu dianggap subcommand. Rootfs yang bernama sama harus
ditulis sebagai path (`./doctor`) atau setelah `run`.

## Diagnostik

```sh
phdp doctor                 # ringkasan pemeriksaan host
phdp doctor --json          # untuk dibaca program
phdp doctor --no-active     # tanpa probe yang membuat proses anak
phdp inspect ROOTFS         # pemeriksaan rootfs
phdp capabilities [--json]  # matriks kemampuan per profil
phdp --version
```

`doctor` memeriksa, antara lain: `abi.host`, `memory.page_size`,
`ptrace.yama_scope`, `selinux.mode`, `seccomp.self`, `exec.storage_policy`,
`openat2`, `ptrace.child`, `ptrace.get_syscall_info`, `seccomp.filter`, dan probe
namespace/mount di proses anak sekali pakai. Tiga hal yang paling menentukan:

- `ptrace.child` harus `PASS`. Bila `FAIL`, periksa
  `/proc/sys/kernel/yama/ptrace_scope` atau kebijakan host.
- `seccomp.filter` menentukan apakah akselerasi aktif.
- `exec.storage_policy` menentukan apakah program guest bisa dijalankan dari
  penyimpanan aplikasi di Android.

Event sesi bisa dibaca program pemanggil sebagai JSON Lines:

```sh
phdp run --json --verbose ~/rootfs/debian -- /bin/true 2>&1 | head -3
```

## Menyematkan libvhdp

Contoh lengkap ada di [examples/c/embed_run.c](examples/c/embed_run.c) dan
[examples/cpp/embed_run.cpp](examples/cpp/embed_run.cpp). Keduanya ikut dibangun
preset host dan berperilaku seperti `phdp run ROOTFS -- PERINTAH`. Pola C:

```c
#include <vhdp/vhdp.h>
#include <string.h>

/* 1. context: pemilik callback event dan cache laporan */
vhdp_context_options opts;
memset(&opts, 0, sizeof(opts));
opts.struct_size = sizeof(opts);
opts.abi_version = VHDP_ABI_VERSION;
vhdp_context* ctx = NULL;
vhdp_context_create(&opts, &ctx);

/* 2. config: rootfs, perintah, identitas yang dilihat guest */
vhdp_config* cfg = NULL;
vhdp_config_create(ctx, &cfg);
vhdp_config_set_rootfs(cfg, "/home/anda/rootfs/debian");
const char* argv_guest[] = { "/bin/sh", "-lc", "id" };
vhdp_config_set_argv(cfg, 3, argv_guest);
vhdp_config_set_uid(cfg, 0);
vhdp_config_set_gid(cfg, 0);

/* 3. session: config disalin, jadi cfg boleh dihancurkan setelah create */
vhdp_session* session = NULL;
vhdp_session_create(ctx, cfg, &session);
vhdp_config_destroy(cfg);

vhdp_exit_info info;
memset(&info, 0, sizeof(info));
info.struct_size = sizeof(info);
info.abi_version = VHDP_ABI_VERSION;
if (vhdp_session_start(session) == VHDP_OK &&
    vhdp_session_wait(session, -1, &info) == VHDP_OK) {
    /* info.shell_status memakai konvensi shell: 128+N bila mati oleh signal N */
}
vhdp_session_destroy(session);
vhdp_context_destroy(ctx);
```

Setiap fungsi mengembalikan `vhdp_status_t`; `vhdp_status_name()` dan
`vhdp_last_error_message()` memberi teks yang bisa dicetak. Ringkasan di atas
melewatkan pemeriksaan status; contoh di `examples/` memeriksa setiap langkah.

Versi C++ memakai `vhdp::Context`, `vhdp::Config`, dan `vhdp::Session` dari
`vhdp.hpp`. Wrapper ini tidak melempar exception; operasi yang bisa gagal
mengembalikan `vhdp::Status` atau `vhdp::Result<T>`.

Menyematkan lewat CMake tanpa CLI dan test:

```cmake
set(VHDP_BUILD_CLI OFF)
set(VHDP_BUILD_TESTS OFF)
set(VHDP_BUILD_EXAMPLES OFF)
add_subdirectory(vhdp)
target_link_libraries(app_saya PRIVATE vhdp)
```

Atau setelah `cmake --install`:

```cmake
find_package(VHDP REQUIRED)
target_link_libraries(app_saya PRIVATE VHDP::vhdp)
```

Aturan lifetime, threading, dan callback ada di [docs/EMBEDDING.md](docs/EMBEDDING.md).

### Fungsi bantu rootfs

Tiga fungsi ini hanya memeriksa atau menulis berkas, tanpa menjalankan engine,
sehingga juga aman dipanggil dari proses aplikasi Android. Semuanya menulis
dokumen JSON ke buffer pemanggil dan hanya tersedia di C ABI.

| Fungsi | Isi |
|---|---|
| `vhdp_configure_rootfs` | Menyiapkan rootfs untuk boot pertama: membuat mountpoint `dev proc sys tmp root etc`, symlink `/dev/fd`, `/dev/stdin`, `/dev/stdout`, `/dev/stderr`, direktori `/dev/shm` dan `/dev/pts`, berkas placeholder untuk node yang diproyeksikan, menulis ulang `/etc/resolv.conf` (8.8.8.8, 1.1.1.1, dan dua alamat IPv6), `/etc/nsswitch.conf` dan `/etc/hosts` bila belum ada, `/root/.profile` dan `/root/.hushlogin`, lalu pengguna `dracos` (uid/gid 1000, login password dimatikan, sudo tanpa password bila `/etc/sudoers.d` ada). Idempoten: berkas yang sudah ada tidak ditimpa, kecuali `resolv.conf`. |
| `vhdp_dpkg_plan_json` | Memeriksa state dpkg/apt (`/var/lib/dpkg`, database status, lock yang tertinggal) dan mengembalikan script pemulihan POSIX sh. Script dijalankan pemanggil di sesi dengan uid guest 0: menormalkan kepemilikan pohon sistem ke `root:root`, menghapus lock, memulihkan database status, lalu `dpkg --configure -a` dengan rollback bila gagal. |
| `vhdp_projection_plan_json` | Melaporkan arsitektur, versi kernel, dan ukuran page host, serta apakah `/dev`, `/proc`, `/sys` host bisa diproyeksikan ke guest (diuji dengan izin search, disertai alasan bila tidak). |

## Build untuk Android

```sh
cmake --preset android-arm64-release
cmake --build --preset android-arm64-release --parallel

cmake --preset android-x86_64-debug
cmake --build --preset android-x86_64-debug --parallel
```

Test dan contoh otomatis dimatikan pada cross-build. Hasilnya `libvhdp.so`,
`libvhdp.a`, `phdp`, dan `vhdp-loader`, semuanya dengan alignment page 16 KiB.

Aplikasi dengan targetSdk 29 ke atas hanya boleh meng-`execve` berkas dari
direktori pustaka native, dan Android hanya memasang berkas bernama `lib*.so` di
sana. Karena itu `phdp` dikemas dengan nama seperti `libphdp.so`, dan
`vhdp-loader` sebagai `libvhdp-loader.so`, nama yang dicari
`platform::find_userland_loader` di direktori yang sama dengan `libvhdp`. Contoh
pemanggilan dari aplikasi:

```sh
libphdp.so run --event-fd 3 --engine rootless --uid 0 --gid 0 \
  --proc host --dev minimal --cwd /home/pengguna --clear-env \
  -e HOME=/home/pengguna -e TERM=xterm-256color -e LANG=C.UTF-8 \
  --bind /storage/emulated/0:/mnt/sdcard:rw \
  /data/user/0/paket.anda/files/rootfs -- /bin/bash -l
```

Catatan platform, minimum API, dan larangan terkait W^X ada di
[docs/ANDROID_BUILD.md](docs/ANDROID_BUILD.md).

## Masalah umum

| Gejala | Sebab dan jalan keluar |
|---|---|
| `cannot run the rootless engine here: this process is already traced` | Linux hanya mengizinkan satu tracer per proses. Jalankan `phdp run` dari shell yang tidak sedang di-ptrace. `doctor`, `inspect`, dan `capabilities` tetap jalan. |
| Semua program guest gagal di-exec (126) | Rootfs berada di filesystem `noexec`, atau ABI-nya berbeda dengan host. `phdp inspect` menyebut keduanya. |
| `dynamic loader ... is missing` (127) | `PT_INTERP` program tidak ada di rootfs. Pastikan loader dan pustaka bersama ikut terpasang. |
| `doctor` melaporkan `ptrace.child FAIL` | Kebijakan ptrace host: Yama `ptrace_scope` 2 ke atas, filter seccomp, atau proses non-dumpable. |
| Guest tidak melihat `/proc` | `/proc` harus diminta dengan `--proc host`. `/dev` minimal aktif secara default dan bisa dimatikan dengan `--dev none`. |
| Muncul direktori `.vhdp-hardlinks` di rootfs | Itu store emulasi hardlink, aktif hanya di host yang menolak `link(2)`. Objeknya kembali ke nama aslinya begitu tinggal satu nama. |

Daftar yang lebih panjang ada di [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

## Struktur project dan deskripsi file

```text
.
|-- CMakeLists.txt, CMakePresets.json    konfigurasi build
|-- include/vhdp/                         header publik (C ABI + wrapper C++)
|-- src/
|   |-- capi/                             implementasi C ABI dan daftar ekspor
|   |-- cli/                              program phdp
|   |-- common/                           status, JSON, parsing string, fd
|   |-- core/                             config, session, lifecycle, laporan
|   |-- engines/rootless/                 supervisor ptrace + seccomp
|   |   `-- loader/                       loader ELF userland (vhdp-loader)
|   |-- engines/unavailable/              descriptor engine yang belum ada
|   |-- vfs/                              mount table dan resolver path guest
|   |-- linux_abi/                        ELF, tabel syscall, tabel errno
|   |-- arch/                             register dan raw syscall per arsitektur
|   `-- platform/linux/                   probe host, procfs, ptrace
|-- hdr/                                  registry hardware yang dilihat guest
|-- vdr/                                  driver: bind, /dev, /proc, PTY, network
|-- examples/                             contoh embedding C dan C++
|-- tests/                                test unit, integrasi, ABI, fixture
|-- fuzz/                                 target libFuzzer
|-- benchmarks/                           micro-benchmark
|-- tools/                                generator tabel syscall
`-- cmake/                                modul CMake dan toolchain
```

### Berkas di root

| File | Deskripsi |
|---|---|
| `CMakeLists.txt` | Definisi project `VHDP` 1.0.0 (C17, C++20, ASM), opsi `VHDP_*`, validasi kombinasi opsi, dan pemanggilan subdirektori. |
| `CMakePresets.json` | Preset configure, build, dan test untuk host dan Android; hasil build ke `build/<preset>`. |
| `.clang-format` | Gaya format kode (basis LLVM, indentasi 4, kolom 100) untuk target `format` dan `format-check`. |
| `.clang-tidy` | Daftar check clang-tidy (`bugprone-*`, `clang-analyzer-*`, `performance-*`, dan beberapa check `cert`, `misc`, `modernize`, `readability`) untuk target `tidy`. |

### include/vhdp

| File | Deskripsi |
|---|---|
| `vhdp.h` | C ABI publik: kode status, jenis engine, mode stdio/seccomp/dev/proc, struct berversi (`vhdp_event`, `vhdp_context_options`, `vhdp_exit_info`), handle opaque, fungsi context, config, session, laporan JSON, dan fungsi bantu rootfs. Aturan thread-safety dan lifetime callback ditulis di kepala berkas. |
| `vhdp.hpp` | Wrapper C++20 header-only di atas `vhdp.h`: `Status`, `Result<T>`, `Context`, `Config`, `Session`, `Event`, `ExitInfo`. Tidak melempar exception dan tidak bergantung pada internal library. |

### src/capi

| File | Deskripsi |
|---|---|
| `capi.cpp` | Implementasi setiap fungsi `vhdp_*`: validasi handle, `struct_size`, `abi_version`, dan field reserved; konversi `Status` internal ke `vhdp_status_t`; pesan error per thread; semua exception C++ ditahan di batas ini. |
| `libvhdp.map` | Linker version script: hanya simbol C ABI yang diekspor dari `libvhdp.so`. |

### src/cli

| File | Deskripsi |
|---|---|
| `main.cpp` | Titik masuk `phdp`: memilih subcommand (`run`, `doctor`, `inspect`, `capabilities`, `help`, `version`) dan memanggil wrapper C++. |
| `cli.hpp` | Deklarasi bersama CLI: kode keluar 2 dan 125, `run_command`, fungsi cetak laporan, `write_all`. |
| `cli_args.hpp` | Tata bahasa argumen `phdp` dan struktur `RunOptions`, `Parsed`, `ParseError`. |
| `cli_args.cpp` | Parser murni tanpa efek samping: disambiguasi subcommand, validasi `--bind`, `--timeout`, `--uid`, `--event-fd`, dan teks bantuan. |
| `cli_run.cpp` | Implementasi `phdp run`: cek TracerPid, relay signal, mode raw dan resize untuk `--pty`, pencetak event (teks atau JSON Lines), dan pemetaan status keluar. |
| `cli_report.cpp` | Mencetak laporan `doctor`, `inspect`, `capabilities` dalam bentuk teks atau JSON mentah. |
| `json_dom.hpp`, `json_dom.cpp` | Parser JSON kecil yang dipakai CLI untuk membaca dokumen JSON dari C ABI. |

### src/common

| File | Deskripsi |
|---|---|
| `status.hpp`, `status.cpp` | Model `Status` dan `Result<T>` internal. Nilai kodenya sama dengan konstanta `VHDP_*`; `errno_status` membentuk pesan dari errno. |
| `json.hpp`, `json.cpp` | `JsonWriter` streaming sesuai RFC 8259, `json_quote`, dan `json_validate`. Byte UTF-8 yang tidak valid ditulis sebagai U+FFFD. |
| `strings.hpp`, `strings.cpp` | Parsing desimal dengan deteksi overflow (`parse_u64`, `parse_u32`, `parse_i32`), `split`, dan `contains_nul`. |
| `unique_fd.hpp` | Pemilik file descriptor (RAII) yang menutup fd tepat satu kali. |

### src/core

| File | Deskripsi |
|---|---|
| `config.hpp`, `config.cpp` | `SessionConfig` (builder) dan `ValidatedConfig` (hasil validasi). `validate_config` meng-kanonikalisasi rootfs dan bind dengan `realpath`, menolak rootfs `/`, memeriksa path guest, membatasi jumlah argumen/env/bind, dan menyusun environment akhir guest. |
| `engine.hpp` | Antarmuka `Engine` dan `EngineInstance`, `ExitResult`, `ProbeReport`, `shell_status`, dan `select_engine`. |
| `engine_select.cpp` | Pemetaan hasil keluar ke status shell dan pemilihan engine. `auto` hanya memilih rootless; engine yang diminta eksplisit tapi tidak tersedia menjadi error. |
| `lifecycle.hpp`, `lifecycle.cpp` | State machine sesi `created`, `starting`, `running`, `exited`, `failed`, `cancelled` beserta transisi yang diizinkan. |
| `events.hpp`, `events.cpp` | Model `Event` (lifecycle, diagnostic, log, trace), antarmuka `EventSink`, dan konversi event ke JSON. |
| `context.hpp`, `context.cpp` | `Context`: memiliki engine, sink event, penghitung sesi, dan cache laporan `doctor`/`inspect`. |
| `session.hpp`, `session.cpp` | `Session`: config tervalidasi, state machine, instance engine, thread waiter, dan watchdog untuk `--timeout`. |
| `reports.hpp` | Deklarasi pembuat laporan JSON dan struktur matriks kemampuan (`ProfileEntry`, `FeatureEntry`). |
| `capabilities.cpp` | Matriks kemampuan: profil eksekusi, fitur beserta ID test, jumlah syscall per kelas, registry hardware, dan katalog driver untuk `phdp capabilities`. |
| `doctor.cpp` | Laporan `doctor`: pemeriksaan read-only dulu, lalu probe aktif di proses anak sekali pakai (ptrace, seccomp, namespace, mount). |
| `inspect.cpp` | Laporan `inspect`: jenis filesystem, `noexec`, `os-release`, shell, ABI ELF, dan dynamic loader, dengan path di-resolve di ruang guest. |
| `configure.cpp` | `build_configure_json`, implementasi `vhdp_configure_rootfs`. |
| `dpkg_recovery.cpp` | `build_dpkg_plan_json` dan script pemulihan dpkg yang dikembalikannya. |
| `projection.cpp` | `build_projection_json`: data host dan ketersediaan `/dev`, `/proc`, `/sys` untuk diproyeksikan. |
| `version.hpp.in` | Template header versi (`VHDP_VERSION_STRING`, arsitektur, platform, tipe build) yang diisi CMake. |

### src/engines/rootless

| File | Deskripsi |
|---|---|
| `rootless_engine.cpp` | Engine rootless: probe sekali per proses (ptrace ke child, seccomp, `PTRACE_GET_SYSCALL_INFO`, loader userland, domain aplikasi Android) dan pembuatan instance sesi, termasuk PTY. |
| `supervisor.hpp`, `supervisor.cpp` | Thread supervisor per sesi: spawn guest, `PTRACE_SEIZE`, loop `waitpid`, penanganan stop (syscall, event, group-stop, signal), SIGSYS dari policy host, restart syscall legacy, kredensial per proses, serah terima ke loader userland, serta pembersihan pohon proses. |
| `syscall_handlers.cpp` | Handler setiap syscall yang diterjemahkan atau diemulasikan: path, exec, socket `AF_UNIX`, signal, identitas, stat, readlink, getdents, emulasi hardlink, dan penolakan dengan errno plus diagnostic. |
| `exec_resolver.hpp`, `exec_resolver.cpp` | Rencana exec: pencarian di `PATH` guest, script `#!`, pemeriksaan ABI ELF, dan indirection lewat dynamic loader milik rootfs. |
| `bootstrap.h`, `bootstrap.c` | Kode child antara `clone()` dan `execve()` yang hanya memakai raw syscall: reset signal, PTY, stdio, sinkronisasi dengan tracer, `close-on-exec` untuk fd di atas 2, `no_new_privs`, dan filter seccomp. |
| `seccomp_filter.hpp`, `seccomp_filter.cpp` | Pembentuk program BPF seccomp: syscall pass-through di-`ALLOW`, sisanya `SECCOMP_RET_TRACE` dengan marker milik VHDP. |
| `tracee_mem.hpp`, `tracee_mem.cpp` | Baca/tulis memori tracee dengan batas ukuran, dengan cadangan lewat `/proc/<tid>/mem`. |
| `link_store.hpp`, `link_store.cpp` | Emulasi hardlink untuk host yang menolak `link(2)`: store `.vhdp-hardlinks/` per mount, record jumlah link, `flock` antar sesi, dan pengembalian objek ke namanya saat tinggal satu nama. |
| `proc_synth.hpp`, `proc_synth.cpp` | Pengganti berkas `/proc` global yang ditolak host (`uptime`, `loadavg`, `stat`, `version`, `vmstat`, `sys/kernel/*`, dan lainnya) serta `/proc/<pid>/status` dengan identitas guest. |
| `loader/load_request.h` | Format `vhdp_load_request` yang diserahkan supervisor kepada loader pada `PTRACE_EVENT_EXEC`. |
| `loader/loader.c` | `vhdp-loader`: loader ELF freestanding (static PIE tanpa libc) yang memetakan program guest dan dynamic linker-nya, memperbaiki auxv, lalu melompat ke entry point. |
| `loader/loader_entry_x86_64.S` | Stub `_start` dan `vhdp_loader_jump` untuk x86_64. |
| `loader/loader_entry_aarch64.S` | Stub `_start` dan `vhdp_loader_jump` untuk aarch64. |

### src/engines/unavailable

| File | Deskripsi |
|---|---|
| `unavailable_engine.cpp` | Descriptor engine `rooted`, `emulator`, dan `vm` yang selalu melaporkan `backend-required` dan tidak bisa membuat instance. |

### src/vfs

| File | Deskripsi |
|---|---|
| `guest_path.hpp`, `guest_path.cpp` | Helper leksikal path guest: komponen, deteksi `..`, normalisasi, join. Tidak menyentuh filesystem. |
| `mount_table.hpp`, `mount_table.cpp` | Tabel pemetaan guest ke host dan sebaliknya dengan aturan prefix terpanjang (rootfs, bind, proyeksi driver). Tidak berubah setelah dibuat. |
| `resolver.hpp`, `resolver.cpp` | Resolver path guest yang sadar symlink, komponen demi komponen, termasuk kebijakan magic link `/proc` dan `DirCache` (cache direktori yang divalidasi dev/ino). |
| `link_marker.hpp` | Nama bersama untuk emulasi hardlink (`.vhdp-hardlinks`) yang dipakai resolver dan `link_store`. |

### src/linux_abi

| File | Deskripsi |
|---|---|
| `elf.h`, `elf.c` | Decoder header ELF dan program header dengan pemeriksaan batas; tidak mengalokasi memori dan menolak ELF big-endian. |
| `errno_table.h`, `errno_table.c` | Nama dan deskripsi errno Linux yang tidak bergantung pada locale libc. |
| `syscall_table.hpp`, `syscall_table.cpp` | Tabel klasifikasi syscall arsitektur target (kelas dan handler) yang dibangun dari `syscalls.def`. |
| `syscalls.def` | Daftar `VHDP_SYSCALL(nama, kelas, handler)` hasil `tools/gen_syscall_def.py`. Jangan diedit manual. |

### src/arch

| File | Deskripsi |
|---|---|
| `raw_syscall.h` | Deklarasi `vhdp_raw_syscall6`: satu syscall tanpa errno, TLS, atau lock, aman dipakai setelah `fork`. |
| `regs.hpp` | Antarmuka `RegsAccess` untuk membaca dan mengubah register tracee saat stop. |
| `x86_64/raw_syscall.S` | Gateway raw syscall x86_64 (argumen ke-4 di `r10`, sadar IBT/SHSTK). |
| `x86_64/regs_x86_64.cpp` | Akses register x86_64 lewat `PTRACE_GETREGS`/`PTRACE_SETREGS`. |
| `aarch64/raw_syscall.S` | Gateway raw syscall aarch64 (nomor di `x8`, `svc #0`, BTI/PAC, `x18` tidak disentuh). |
| `aarch64/regs_aarch64.cpp` | Akses register aarch64 lewat `PTRACE_GETREGSET` dan `NT_ARM_SYSTEM_CALL`. |
| `portable/raw_syscall_portable.c` | Implementasi C rujukan untuk raw syscall, dipakai uji differential dan saat `VHDP_FORCE_PORTABLE`. |

### src/platform/linux

| File | Deskripsi |
|---|---|
| `host_probe.hpp`, `host_probe.cpp` | Probe host untuk `doctor`, pemilihan engine, dan `inspect`: ptrace, seccomp, namespace, `openat2`, Yama, SELinux, cgroup, KVM, deteksi Android, TracerPid, lokasi loader userland, dan izin pemetaan `PROT_EXEC`. |
| `probe_child.h`, `probe_child.c` | Proses anak sekali pakai untuk probe aktif (hanya raw syscall); probe namespace berjalan di namespace pribadi yang hilang bersama child. |
| `proc.hpp`, `proc.cpp` | Helper kecil dan berbatas untuk membaca procfs/sysfs: baca berkas, `readlink`, field `/proc/<pid>/status`, hitung fd. |
| `ptrace_defs.hpp` | Konstanta ptrace dari UAPI kernel dan wrapper pemanggilan lewat raw syscall. |

### src (berkas lain)

| File | Deskripsi |
|---|---|
| `src/CMakeLists.txt` | Target `vhdp_objects`, `vhdp` (shared, dengan version script), `vhdp_static`, `vhdp_internal`, `phdp_cli`, `phdp`, dan `vhdp_loader`. |

### hdr

| File | Deskripsi |
|---|---|
| `hardware_registry.hpp`, `hardware_registry.cpp` | Daftar kemampuan mirip hardware yang dilihat guest (cpu, memory, clock, entropy, null-zero-full, terminal, identity, uname, network, gpu, usb-block) beserta cara penyediaannya: `host-passthrough`, `projected`, `emulated`, atau `absent`. |

### vdr

| File | Deskripsi |
|---|---|
| `drivers.hpp`, `drivers.cpp` | Katalog driver (`host-bind`, `dev-minimal`, `proc-host`, `pty`, `network-policy`), `build_mount_table` yang menyusun mount table sesi, dan `PtyDriver` (alokasi PTY, resize, input, pompa output ke callback). |

### examples

| File | Deskripsi |
|---|---|
| `CMakeLists.txt` | Membangun `embed_run_c` dan `embed_run_cpp` yang menautkan `libvhdp.so` lewat header publik saja. |
| `c/embed_run.c` | Contoh C: menjalankan `ROOTFS PERINTAH [ARG...]`, mencetak event ke stderr, dan keluar dengan status gaya shell. |
| `cpp/embed_run.cpp` | Contoh yang sama memakai wrapper C++ RAII. |

### tests

| File | Deskripsi |
|---|---|
| `CMakeLists.txt` | Mendaftarkan setiap kasus test sebagai test CTest ber-ID, membangun fixture, dan test ABI. |
| `framework/vtest.hpp`, `framework/vtest_main.cpp` | Framework test mandiri (tanpa dependensi luar). Kode keluar 0 lulus, 1 gagal, 77 dilewati. |
| `abi/exported_symbols.txt` | Daftar persis simbol yang boleh diekspor `libvhdp.so`. |
| `abi/check_symbols.cmake` | Test `T-ABI-SYMBOLS`: membandingkan hasil `nm` dengan daftar di atas dan menolak simbol C++/STL. |
| `abi/include_c.c` | Bukti bahwa `vhdp.h` mandiri dan valid sebagai C17 (`T-ABI-HEADERS-C`). |
| `abi/include_cpp.cpp` | Bukti bahwa `vhdp.hpp` mandiri dan valid sebagai C++20 (`T-ABI-HEADERS-CPP`). |
| `fixtures/vhdp_fixture.c` | Program guest multi-call milik project (applet `echo`, `cat`, `stat`, `try`, `fanout`, `uid`, `winsize`, dan lainnya) dalam varian statis dan dinamis. |
| `fixtures/gen_foreign_elf.c` | Membuat ELF64 dengan `e_machine` arsitektur lain untuk test ABI mismatch. |
| `fixtures/make_rootfs.cmake` | Menyusun rootfs fixture dari biner fixture. |
| `fixtures/populate_dynamic.cmake` | Menyalin interpreter dan pustaka bersama biner dinamis ke rootfs fixture. |
| `integration/harness.hpp`, `integration/harness.cpp` | Helper test integrasi: menjalankan `phdp` sebagai child (biasa atau di PTY), menjalankan guest lewat `libvhdp`, dan salinan rootfs sementara per test. |
| `integration/test_cli_run.cpp` | Status keluar, alias tanpa `run`, env/cwd, dan shell default. |
| `integration/test_exec.cpp` | ELF dinamis, shebang, ABI mismatch, loader hilang, perintah tidak ditemukan. |
| `integration/test_filesystem.cpp` | Path absolut/relatif, dirfd, `..`, symlink escape, rename/link, bind ro/rw, rootfs read-only, fd warisan, socket `AF_UNIX`. |
| `integration/test_identity.cpp` | Identitas uid/gid guest dan `/dev` minimal. |
| `integration/test_library.cpp` | Contoh embedding C dan C++, serta resize PTY lewat library. |
| `integration/test_lifecycle_stress.cpp` | 100 siklus start/stop tanpa kebocoran dan pembatalan pohon proses. |
| `integration/test_network.cpp` | `--network host` dan `--network none`. |
| `integration/test_process.cpp` | Fork/vfork/clone/thread, syscall yang ditolak, dan mode ptrace tanpa seccomp. |
| `integration/test_pty.cpp` | Ctrl-C di PTY (exit 130), ukuran jendela, dan job control. |
| `integration/test_reports.cpp` | JSON `doctor`, `capabilities`, `inspect`, dan event JSON Lines. |
| `integration/test_signals.cpp` | `--timeout` (exit 124) dan penerusan signal (exit 143). |
| `unit/test_capi.cpp` | C ABI dari sisi konsumen dan penjagaan `struct_size`/`abi_version`. |
| `unit/test_cli_args.cpp` | Parser argumen dan disambiguasi subcommand. |
| `unit/test_config.cpp` | Validasi config dan penyusunan environment. |
| `unit/test_cpp_wrapper.cpp` | Wrapper C++ termasuk move semantics. |
| `unit/test_elf.cpp` | Decoder ELF: statis, dinamis, rusak, arsitektur lain. |
| `unit/test_engine_select.cpp` | Aturan pemilihan engine. |
| `unit/test_guest_path.cpp` | Helper path guest dan normalisasi. |
| `unit/test_json.cpp` | Round-trip, escaping, dan validasi JSON. |
| `unit/test_lifecycle.cpp` | Transisi state machine. |
| `unit/test_mount_table.cpp` | Pemetaan prefix terpanjang dan pemetaan balik. |
| `unit/test_raw_syscall.cpp` | Kesetaraan gateway assembly dengan rujukan C, register callee-saved, dan errno. |
| `unit/test_resolver.cpp` | Resolver: dasar, symlink, dan upaya keluar dari rootfs. |
| `unit/test_seccomp_filter.cpp` | Isi filter seccomp (ALLOW atau TRACE). |
| `unit/test_strings.cpp` | Parsing angka. |
| `unit/test_syscall_table.cpp` | Setiap nomor di `<asm/unistd.h>` target punya klasifikasi. |

### fuzz, benchmarks, tools

| File | Deskripsi |
|---|---|
| `fuzz/CMakeLists.txt` | Membangun target fuzz bila compiler mendukung `-fsanitize=fuzzer`; bila tidak, dilewati. |
| `fuzz/fuzz_elf.cpp` | Fuzz decoder ELF. |
| `fuzz/fuzz_json.cpp` | Fuzz validator dan escaping JSON. |
| `fuzz/fuzz_path.cpp` | Fuzz helper path guest. |
| `fuzz/fuzz_shebang.cpp` | Fuzz parser baris `#!`. |
| `benchmarks/CMakeLists.txt` | Membangun `vhdp_bench` (tidak ikut dijalankan `ctest`). |
| `benchmarks/bench_main.cpp` | Mengukur p50/p95 loop syscall dan waktu mulai sesi dengan seccomp on/off: `vhdp_bench <rootfs> [iterasi]`. |
| `tools/gen_syscall_def.py` | Generator `src/linux_abi/syscalls.def`. Opsi `--check-header` gagal bila header kernel punya syscall tanpa klasifikasi. |

### cmake

| File | Deskripsi |
|---|---|
| `VhdpArch.cmake` | Menormalkan arsitektur (`x86_64`, `aarch64`), menentukan platform (`linux`, `android`), dan apakah assembly dipakai. |
| `VhdpCompilerFlags.cmake` | Target `vhdp_flags` (hardening, `-fno-rtti`, branch protection, RELRO/NOW, stack non-exec, page 16 KiB di Android, sanitizer), `vhdp_warnings`, baseline `-O2`, dan fungsi `vhdp_add_test`. |
| `VhdpFindNdk.cmake` | Mencari Android NDK versi yang dipin dengan urutan pencarian yang tetap. |
| `VhdpInstall.cmake` | Aturan install library, `phdp`, header, dan paket CMake `VHDP`. |
| `VHDPConfig.cmake.in` | Template `VHDPConfig.cmake` untuk `find_package(VHDP)`. |
| `VhdpTools.cmake` | Target `format`, `format-check`, dan `tidy`; melaporkan `NOT RUN` bila tool tidak ditemukan. |
| `RunClangTidy.cmake` | Script `cmake -P` yang menjalankan clang-tidy secara paralel. |
| `toolchains/host-clang.cmake` | Toolchain host dengan Clang dari `PATH` atau dari NDK. |
| `toolchains/host-gcc.cmake` | Toolchain host dengan GCC, dipakai preset sanitizer. |
| `toolchains/android-ndk.cmake` | Pembungkus toolchain resmi NDK. |

## Dokumentasi lain

- [ARCHITECTURE.md](ARCHITECTURE.md)
- [SECURITY.md](SECURITY.md)
- [CHANGELOG.md](CHANGELOG.md)
- [docs/](docs/)

## Lisensi

Apache License 2.0. Lihat [LICENSE](LICENSE) dan [NOTICE](NOTICE). Project ini
tidak membawa dependensi pihak ketiga di dalam sumbernya.
