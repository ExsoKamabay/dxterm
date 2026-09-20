# Changelog

Semua perubahan penting pada project ini dicatat di sini. Formatnya mengikuti
Keep a Changelog dan penomoran versinya mengikuti Semantic Versioning.

## [1.0.0] - 2026-09-19

### Lisensi

- Lisensi project ditetapkan Apache-2.0 (`LICENSE` dan `NOTICE` ditambahkan,
  ADR 0007 diperbarui).

### Ditambahkan

- Opsi CMake `VHDP_BUILD_CLI` (default `ON`). Program yang menyematkan
  `libvhdp` lewat `add_subdirectory` bisa mematikannya sehingga `phdp`,
  `phdp_cli`, dan aturan install `phdp` tidak dibuat. `VHDP_BUILD_TESTS=ON`
  bersama `VHDP_BUILD_CLI=OFF` ditolak saat configure.
- Loader ELF userland (`engines/rootless/loader/`, target `vhdp_loader`, berkas
  `vhdp-loader`) untuk host yang melarang `execve` dari penyimpanan yang bisa
  ditulis aplikasi (Android API 29 ke atas). Supervisor meng-exec loader dari
  direktori pustaka native, lalu menyerahkan permintaan muat pada
  `PTRACE_EVENT_EXEC`; ELF guest dipetakan dengan `mmap(PROT_EXEC)`. Terkait:
  `SupervisorOptions::loader_path`, `platform::find_userland_loader`,
  `probe_exec_mapping`, dan variabel `VHDP_LOADER`.
- Emulasi hardlink (`engines/rootless/link_store.*`, `vfs/link_marker.hpp`) untuk
  host yang menolak `link(2)`, yang dibutuhkan dpkg, shadow, tar, dan `cp -al`.
  Store per mount (`.vhdp-hardlinks/<id>` dan record), `flock` antar sesi,
  objek kembali ke namanya saat hitungan turun ke satu, presentasi
  `st_nlink`/`st_ino`, patch `d_type` pada `getdents64`, dan perawatan saat
  `unlink`, `rename`, dan `RENAME_EXCHANGE`.
- Pengganti berkas `/proc` global (`engines/rootless/proc_synth.*`) yang
  dibangun dari sumber yang diizinkan (`CLOCK_BOOTTIME`, `sysinfo`, `uname`,
  residensi cpuidle, `meminfo`, `self/mounts`): `uptime`, `loadavg`, `stat`,
  `version`, `filesystems`, `swaps`, `vmstat`, `sys/kernel/*`.
  `/proc/<pid>/status` proses sesi menampilkan identitas guest sehingga `ps`,
  `pstree`, dan `vmstat` bekerja.
- Jawaban SIGSYS dari policy host (`handle_host_seccomp_trap`): policy aplikasi
  Android memakai `SECCOMP_RET_TRAP` yang mengalahkan `RET_TRACE`; supervisor
  menjawabnya kecuali filter yang men-trap dipasang guest sendiri.
- `RegsAccess::restart_as` (x86_64 dan aarch64) dan `restart_legacy`: syscall
  legacy yang ditrap policy x86_64 (`open`, `stat`, `access`, `dup2`, `poll`, dan
  lainnya) di-restart sebagai padanan `*at`.
- Kredensial per proses (`struct Creds`), dan `socket(AF_NETLINK, ...,
  NETLINK_AUDIT)` dijawab `EPROTONOSUPPORT` agar `useradd`/`groupadd` tetap
  lanjut.
- Fungsi bantu rootfs di C ABI: `vhdp_configure_rootfs`, `vhdp_dpkg_plan_json`,
  dan `vhdp_projection_plan_json`.

### Diubah

- README ditulis ulang dari isi source code: tujuan project, fitur, diagram alur
  kerja (lapisan, `phdp run`, satu syscall, exec program guest), persyaratan
  yang sudah diverifikasi, cara build, test, dan pakai, fungsi bantu rootfs di
  C ABI, serta deskripsi setiap file source, build, test, dan tool. File
  dokumentasi (README, CHANGELOG, ARCHITECTURE, SECURITY, LICENSE, NOTICE,
  `docs/`, README di `hdr/` dan `vdr/`) tidak dideskripsikan di README.
- `ARCHITECTURE.md`, `SECURITY.md`, `NOTICE`, dokumen di `docs/`, dan ADR
  diselaraskan dengan kode saat ini. ADR 0008 kini mencatat bahwa profil
  `android-app` sudah didukung lewat loader userland. ADR 0004 memuat hasil
  `vhdp_bench` yang diukur ulang, menggantikan angka lama yang tidak cocok
  dengan pengukuran.
- Teks yang menyebut project lain dihapus dari dokumen, komentar source, dan
  pesan yang dilihat pengguna. Yang berubah pada output program:
  - `phdp run` saat prosesnya sudah di-ptrace: pesan menyebut "an enclosing
    vhdp session, a debugger or a syscall tracer" dan menyarankan `phdp run`
    serta `phdp doctor` (sebelumnya tertulis `vhdp run` dan `vhdp doctor`).
  - `phdp capabilities`: deskripsi profil `terminal-unprivileged`, `android-app`,
    dan `vm`, catatan fitur `lib.jni_aar`, serta deskripsi fitur
    `guest.containers_kvm_ebpf` ("nested containers, KVM, eBPF, netfilter,
    FUSE"). Catatan fitur `tty.job_control` kini menjelaskan apa yang benar-benar
    diuji `T-PTY-JOB-CONTROL` (shell fixture di PTY); catatan lama menyebut tool
    host yang tidak dipakai test itu. ID profil dan fitur tidak berubah.
  - Alasan `backend-required` untuk engine `emulator` dan `vm`.
  - Pesan `skip` probe `namespace.user` di `doctor` saat proses di-ptrace.
  - `vhdp_configure_rootfs`: kolom GECOS pengguna `dracos` di `/etc/passwd`
    menjadi `dracos`, dan prompt di `/root/.profile` menjadi `\u@vhdp`.
  - Script dari `vhdp_dpkg_plan_json`: prefix log `[vhdp-recovery]` dan berkas
    rollback `/var/lib/dpkg/status.vhdp-bak`.
- Komentar di `vhdp.h`, `reports.hpp`, `configure.cpp`, `dpkg_recovery.cpp`,
  dan `projection.cpp` kini menyebut bahwa pemanggil yang menjalankan script
  pemulihan dan menerapkan rencana bind.

### Diperbaiki

- Group-stop tidak dihormati. `PTRACE_EVENT_STOP` membawa signal penghenti,
  bukan `SIGTRAP`, sehingga dikira signal-delivery-stop dan tracee dijalankan
  lagi; `timeout N apt-get` berputar stop/run tanpa henti. Event-stop kini
  diklasifikasikan lebih dulu dan dilanjutkan dengan `PTRACE_LISTEN`.
- `readlink("/proc/self/exe")` terpotong. Tautan kini dibaca utuh di supervisor
  dan hanya path guest yang dipendekkan.
- `execve` dengan path absolut gagal saat cwd sudah dihapus. Cwd kini hanya
  diwajibkan untuk nama relatif.
- Anak dari proses aplikasi non-dumpable tidak bisa di-`PTRACE_SEIZE`. Child
  bootstrap dan probe memasang `PR_SET_DUMPABLE` lalu memberi tanda siap lewat
  pipe.
- Resolver menganggap `EACCES` pada komponen terakhir mount `/proc` sebagai
  "ada", supaya pengganti berkas `/proc` bisa mengambil alih.
- Urutan errno `link(2)` disamakan dengan kernel (`ENOENT`, lalu `EEXIST`, lalu
  `EXDEV`); `link_existing` memeriksa ulang objek di bawah lock; record yang
  hilang tidak lagi menghapus objek; `rename` dengan `src == dst` tidak
  diperbaiki.
- Direktori scratch `/proc` dibuat dengan `mkdtemp`. Sebelumnya namanya tetap,
  mudah ditebak, dan dipakai bersama antar sesi.
- `vfork` tidak lagi di-restart sebagai syscall lain (glibc bergantung pada
  register di sekitar instruksi itu); timeout `select` dinormalkan seperti
  kernel; tracee yang sudah dilaporkan keluar tidak ditambahkan ulang saat event
  fork (`platform::process_state`).
- `doctor`: `exec.storage_policy` lolos di aplikasi bila loader ada dan probe
  pemetaan tidak ditolak; `rootless_ready` ikut memperhitungkannya.
- `doctor` mematikan proses pemanggil dengan `SIGSYS` bila proses itu berjalan
  di bawah filter seccomp yang men-trap `openat2`, seperti setiap proses
  aplikasi Android (di API 35: `Fatal signal 31 (SIGSYS), code 1 (SYS_SECCOMP),
  syscall 437`). Probe `openat2` kini tidak pernah dipanggil langsung di proses
  yang terfilter: mode aktif mengujinya di child sekali pakai, mode pasif
  melaporkan `skip`.

### Dihapus

- Folder `build/` berisi hasil build `host-debug` bawaan (85 MB). Folder ini
  dibuat ulang oleh `cmake --preset host-debug`.
- Rujukan CHANGELOG ke test `T-BUILD-CLI-OPTION` dan `T-UNIT-PROBE-SECCOMP-TRAP`,
  karena kedua test itu tidak ada di project ini.

### Performa

- `vfs::DirCache` (cache prefix direktori yang divalidasi dev/ino), query
  read-only yang dijalankan supervisor sendiri (`host_stat`, `host_readlink`,
  `host_xattr`: satu stop, bukan dua), fixup stat yang digabung, dan penelusuran
  hardlink see-through yang dibatasi `links_visible()`/`stores_present()`
  (TTL 0,5 detik) sehingga sesi tanpa objek hardlink tidak menanggung biayanya.

### Diverifikasi

- `ctest --preset host-debug` (Clang 21 dari NDK 29): 72 dari 72 test lulus.
- Build tanpa preset dengan GCC 13.3: 72 dari 72 test lulus.
- `tools/gen_syscall_def.py` menghasilkan `syscalls.def` yang identik dengan
  berkas yang ada.
- `format-check` masih gagal karena pelanggaran format yang sudah ada sebelumnya
  di 13 berkas (antara lain `supervisor.cpp`, `syscall_handlers.cpp`,
  `cli_run.cpp`). Perubahan ini tidak menambah pelanggaran baru dan merapikan
  satu blok komentar di `dpkg_recovery.cpp`.

### Status

- Profil `android-app` menjadi `supported` (`core/capabilities.cpp`). Bukti QA
  perangkat: ponsel arm64 Android 15 (API 35, SELinux enforcing) dan Android 13
  x86_64 yang menjalankan rootfs Debian lewat `phdp` dari aplikasi, termasuk
  apt/dpkg, job control, dan procps. CTest host 72 dari 72 lulus, di mode
  default maupun mode loader.

## [0.1.0]

Rilis fondasi: vertical slice rootless untuk guest berarsitektur sama, diuji
test suite di host Linux x86_64.

### Ditambahkan

- Core: config tervalidasi dan immutable, state machine lifecycle eksplisit,
  model event dan error, pemilihan engine berbasis probe, session dengan waiter
  dan watchdog timeout, laporan `doctor`, `inspect`, `capabilities` (JSON dan
  teks).
- Engine rootless: supervisor `ptrace` per sesi (fork/clone diikuti, pembersihan
  pohon proses, `PTRACE_O_EXITKILL`), akselerator `seccomp` dengan fallback
  `PTRACE_SYSCALL`, bootstrap tracee yang async-signal-safe, loader indirection
  untuk ELF dinamis, resolusi `#!`, deteksi ABI yang tidak cocok.
- VFS: mount table prefix terpanjang dan resolver simbolik di ruang guest
  (menahan `..`, symlink escape, dan loop; magic link `/proc` fail-closed), bind
  ro/rw, rootfs read-only.
- Lapisan syscall: tabel klasifikasi untuk setiap nomor `<asm/unistd.h>` target
  (translated, pass-through, emulated, denied, unsupported; nomor tak dikenal
  dijawab `ENOSYS`), handler path, exec, socket, signal, dan identitas.
- Arsitektur: adapter register x86_64 dan aarch64, rutin `.S` gateway raw
  syscall (sadar BTI/PAC/IBT) dengan rujukan C portabel dan uji differential,
  opsi `VHDP_FORCE_PORTABLE`.
- Library: C ABI stabil (`vhdp.h`) berversi dengan allowlist ekspor dan snapshot
  simbol, wrapper C++20 RAII (`vhdp.hpp`), contoh C dan C++.
- CLI `phdp`: `run` dan alias tanpa `run`, `doctor`, `inspect`,
  `capabilities`, `--help`, `--version`; event JSON Lines di stderr atau
  `--event-fd`; relay signal dan PTY.
- `hdr` dan `vdr`: registry hardware virtual (membedakan pass-through dan
  emulasi) dan driver guest-service (PTY, `/dev` minimal, `/proc` host, bind
  host, kebijakan network).
- Test: 72 test CTest (unit, integrasi, ABI), rootfs fixture milik project,
  target fuzz (ELF, JSON, path, shebang), benchmark.
- Build: preset `host-debug`, `host-release`, `host-asan-ubsan`, `host-tsan`,
  `android-arm64-release`, `android-x86_64-debug`; target format-check dan tidy;
  hardening; page 16 KiB.
- Dokumentasi: README, ARCHITECTURE, SECURITY, dokumen `docs/`, dan ADR 0001
  sampai 0008.

### Status saat rilis

- Profil Android (`terminal-unprivileged`, emulator) belum diuji di perangkat.
- Profil `android-app` belum didukung; `rooted`, `vm`, dan lintas arsitektur
  berstatus `backend-required`.
- Engine rootless memberi isolasi kompatibilitas, bukan sandbox keamanan.
- Sisa masalah: SIGPIPE guest memakai disposisi default, job control terbatas,
  keluarga `*at` tanpa `openat2`, risiko rename/TOCTOU yang melekat (lihat
  SECURITY.md).
- Lisensi belum dipilih (ADR 0007). JNI/AAR ditunda (ADR 0008). Backend rooted,
  emulator, dan VM belum diimplementasikan.
