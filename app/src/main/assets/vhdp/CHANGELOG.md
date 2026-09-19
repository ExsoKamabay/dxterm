# Changelog

Format mengikuti semangat Keep a Changelog. Versi mengikuti SemVer; sebelum 1.0.0
API dapat berubah.

## [Unreleased] — integrasi ke aplikasi dracxterm

### Lisensi

- Lisensi proyek ditetapkan menjadi **Apache-2.0** (`LICENSE` + `NOTICE`
  ditambahkan; ADR 0007 diperbarui). Kompatibel satu arah dengan GPL-3.0, jadi
  `libvhdp` boleh disematkan ke aplikasi GPL-3.0.

### Ditambahkan

- Opsi CMake `VHDP_BUILD_CLI` (default `ON`). Konsumen yang menyematkan `libvhdp`
  lewat `add_subdirectory` bisa mematikannya; `phdp`, `phdp_cli`, dan install rule
  `phdp` lalu tidak dibuat. `VHDP_BUILD_TESTS=ON` bersama `VHDP_BUILD_CLI=OFF`
  ditolak saat configure. Diuji oleh `T-BUILD-CLI-OPTION`.
- **Userland ELF loader** (`engines/rootless/loader/`, target `vhdp_loader` →
  `libvhdp-loader.so`) untuk host yang melarang `execve` dari penyimpanan yang bisa
  ditulis aplikasi (Android API ≥ 29). Supervisor meng-exec loader dari direktori
  pustaka native, lalu menyerahkan permintaan muat pada `PTRACE_EVENT_EXEC`; ELF
  guest dipetakan `mmap(PROT_EXEC)`. `SupervisorOptions::loader_path`,
  `platform::find_userland_loader`, `probe_exec_mapping`. Implementasi original.
- **Emulasi hardlink** (`engines/rootless/link_store.*`, `vfs/link_marker.hpp`)
  untuk host yang menolak `link(2)` — SELinux menolaknya di penyimpanan aplikasi,
  sementara dpkg, shadow, tar dan `cp -al` memerlukannya. Store per mount
  (`.vhdp-hardlinks/<id>` + record, `flock` antar sesi, collapse saat hitungan
  turun ke satu), presentasi `st_nlink`/`st_ino`, patch `d_type` `getdents64`,
  perawatan saat `unlink`/`rename`/`RENAME_EXCHANGE`.
- **Stand-in `/proc` global** (`engines/rootless/proc_synth.*`) dibangun dari
  sumber yang diizinkan (`CLOCK_BOOTTIME`, `sysinfo`, `uname`, residensi cpuidle,
  `meminfo`, `self/mounts`): `uptime`, `loadavg`, `stat`, `version`, `filesystems`,
  `swaps`, `vmstat`, `sys/kernel/*`. `/proc/<pid>/status` proses sesi ditulis ulang
  ke identitas yang diemulasikan sehingga `ps`/`pstree`/`vmstat` bekerja.
- **Jawaban SIGSYS policy host** (`handle_host_seccomp_trap`): policy aplikasi
  Android memakai `SECCOMP_RET_TRAP` yang mengalahkan `RET_TRACE`; supervisor
  menjawabnya, kecuali bila filter yang men-trap adalah milik guest sendiri.
- `RegsAccess::restart_as` (x86_64 & aarch64) plus `restart_legacy`: syscall legacy
  yang ditrap policy x86_64 (`open`, `stat`, `access`, `dup2`, `poll`, …) di-restart
  sebagai padanan `*at`.
- Kredensial per proses (`struct Creds`) dan penolakan `NETLINK_AUDIT` dengan
  `EPROTONOSUPPORT` (shadow abort pada errno lain).
- Jembatan JNI (`vhdp_jni.cpp`) untuk konsumen Android, membuka kembali arah yang
  dibekukan ADR 0008.

### Diperbaiki

- Group-stop tidak dihormati: `PTRACE_EVENT_STOP` membawa sinyal penghenti, bukan
  `SIGTRAP`, sehingga dikira signal-delivery-stop dan tracee dijalankan kembali →
  `timeout N apt-get` berputar stop/run tanpa henti. Event-stop kini
  diklasifikasikan lebih dulu dan dilanjutkan dengan `PTRACE_LISTEN`.
- `readlink("/proc/self/exe")` terpotong: tautan kini dibaca utuh di supervisor dan
  hanya path guest yang dipendekkan.
- `execve` path absolut gagal saat cwd sudah dihapus; cwd kini hanya diwajibkan
  untuk nama relatif.
- Anak proses aplikasi non-dumpable tidak bisa di-`PTRACE_SEIZE`: child bootstrap
  dan probe memasang `PR_SET_DUMPABLE` lalu memberi tanda siap lewat pipe.
- Resolver menganggap `EACCES` pada komponen terakhir sebuah mount `/proc` sebagai
  "ada", supaya stand-in bisa mengambil alih.
- Urutan errno `link(2)` disamakan dengan kernel (`ENOENT` → `EEXIST` → `EXDEV`);
  `link_existing` memeriksa ulang objek di bawah lock; record yang hilang tidak
  lagi menyebabkan penghapusan objek; `rename` dengan `src == dst` tidak difixup.
- Direktori scratch `/proc` dibuat dengan `mkdtemp` (sebelumnya nama tetap yang
  bisa ditebak dan dipakai bersama antar sesi).
- `vfork` tidak di-restart sebagai syscall lain (glibc mengandalkan register yang
  masih hidup di sekitar instruksi itu); timeout `select` dinormalisasi seperti
  kernel; tracee yang sudah dilaporkan keluar tidak ditambahkan ulang saat event
  fork (`platform::process_state`).
- `doctor`: `exec.storage_policy` lolos di aplikasi bila loader ada dan probe
  pemetaan tidak ditolak; `rootless_ready` memperhitungkannya.

### Performa

- `vfs::DirCache` (prefix cache yang divalidasi dev/ino), query read-only
  dijalankan supervisor sendiri (`host_stat`, `host_readlink`, `host_xattr` — satu
  stop, bukan dua), fixup stat digabung, dan penelusuran see-through hardlink
  digating `links_visible()`/`stores_present()` (TTL 0,5 dtk) sehingga sesi tanpa
  objek tidak menanggung biayanya.

### Status yang diperbarui

- Profil **`android-app` kini `supported`** (`core/capabilities.cpp`), dengan bukti
  QA perangkat: Infinix arm64 (API 35, SELinux enforcing) dan WayDroid x86_64,
  masing-masing lolos rangkaian QA A/B/C/SCALE/FINAL; host CTest 72/72 pada mode
  default maupun mode loader.

- `doctor` mematikan proses pemanggil dengan `SIGSYS` bila proses itu berjalan di
  bawah filter seccomp yang men-trap `openat2`, seperti setiap proses aplikasi
  Android. Di perangkat API 35 terlihat sebagai `Fatal signal 31 (SIGSYS), code 1
  (SYS_SECCOMP), syscall 437`. Probe `openat2` kini tidak pernah dipanggil langsung
  di proses yang terfilter: mode aktif mengujinya di child sekali pakai, mode pasif
  melaporkan `skip` ("not probed"). Regression test: `T-UNIT-PROBE-SECCOMP-TRAP`.

## [0.1.0] — fondasi

Rilis fondasi pertama: vertical slice rootless yang nyata untuk guest
same-architecture, terverifikasi test suite di host Linux x86_64.

### Ditambahkan

- **Core**: config tervalidasi & immutable, state machine lifecycle eksplisit,
  event/error model, engine selection berbasis probe, session dengan waiter +
  watchdog timeout, laporan `doctor`/`inspect`/`capabilities` (JSON + human).
- **Rootless engine**: supervisor `ptrace` per-session (fork/clone diikuti,
  cleanup process-tree, `PTRACE_O_EXITKILL`), akselerator `seccomp` dengan
  fallback `PTRACE_SYSCALL`, bootstrap tracee async-signal-safe, loader
  indirection untuk ELF dinamis, resolusi `#!`, deteksi ABI mismatch.
- **VFS**: mount table longest-prefix + resolver simbolik guest-space (anti
  `..`/symlink-escape/loop, magic-link `/proc` fail-closed), bind ro/rw,
  read-only rootfs.
- **Syscall layer**: tabel klasifikasi lengkap untuk seluruh nomor `<asm/unistd.h>`
  target (translated/pass-through/emulated/denied/unsupported; nomor tak dikenal →
  `ENOSYS`), handler path/exec/socket/signal/identity.
- **Arch**: adapter register x86_64 & aarch64; rutin `.S` raw syscall gateway
  (BTI/PAC/IBT-aware) + referensi portable + differential test; `VHDP_FORCE_PORTABLE`.
- **Library**: C ABI stabil (`vhdp.h`) berversi dengan export allowlist & snapshot
  simbol; wrapper C++20 RAII (`vhdp.hpp`); contoh C & C++.
- **CLI `phdp`**: facade tipis; `run` + alias positional, `doctor`, `inspect`,
  `capabilities`, `--help`, `--version`; parser dengan disambiguasi subcommand,
  JSON Lines events di stderr/`--event-fd`, relay signal & PTY.
- **hdr/vdr**: registry virtual hardware (jujur soal pass-through vs emulasi) &
  driver guest-service (PTY, `/dev` minimal, `/proc` host, host-bind,
  network-policy).
- **Test**: 72 test CTest (unit + integrasi + ABI), fixture rootfs milik proyek,
  fuzz target (ELF/JSON/path/shebang), benchmark reproducible.
- **Build**: preset `host-debug/release/asan-ubsan/tsan` +
  `android-{arm64-release,x86_64-debug}`; format-check, tidy, hardening, page 16 KiB.
- **Dokumentasi**: README, ARCHITECTURE, SECURITY, docs/CAPABILITIES/ANDROID_BUILD/
  ROOTFS/EMBEDDING/TROUBLESHOOTING, dan ADR 0001–0008.

### Status yang diketahui

- Profil Android (`terminal-unprivileged`, emulator) **untested/NOT RUN**: host
  build ini x86_64, artifact arm64 tak dapat dijalankan di host.
- Profil `android-app` **unsupported**; `rooted`/`vm`/cross-arch **backend-required**.
- Rootless engine adalah compatibility isolation, **bukan** security sandbox.
- Sisa: sinyal SIGPIPE guest memakai disposisi default; job control terbatas;
  keluarga `*at` tanpa `openat2`; risiko rename/TOCTOU inheren (lihat SECURITY.md).

### Belum ada / diserahkan kemudian

Lisensi belum dipilih (ADR 0007). JNI/AAR dibekukan sampai jalur eksekusi Android
terbukti (ADR 0008). Rooted/emulator/VM backend belum diimplementasikan.
