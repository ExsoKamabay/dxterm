# Arsitektur VHDP

Dokumen ini menjelaskan cara VHDP menjalankan rootfs distro Linux tanpa root.
Diagram alur `phdp run`, alur satu syscall, dan alur exec ada di
[README.md](README.md#cara-kerja); di sini dibahas alasan dan detailnya.

## Lapisan

```text
                 phdp (CLI)                    contoh C / C++
                     |                               |
             include/vhdp/vhdp.hpp  (wrapper RAII C++20)
                     |
             include/vhdp/vhdp.h    (C ABI stabil, berversi)
                     |
   +-------------------------------------------------------------+
   |  src/capi        implementasi C ABI, penahan exception      |
   |  src/core        context, config, lifecycle, session,       |
   |                  event, pemilihan engine, laporan           |
   |  src/engines     rootless (ptrace+seccomp) | unavailable    |
   |  src/vfs         mount table + resolver simbolik            |
   |  src/linux_abi   decoder ELF, tabel syscall, tabel errno    |
   |  src/arch        adapter register per arsitektur + rutin .S |
   |  src/platform    probe host, procfs, konstanta ptrace       |
   |  hdr / vdr       registry hardware / driver guest-service   |
   +-------------------------------------------------------------+
```

CLI tidak menduplikasi engine atau lifecycle. Ia hanya memanggil C ABI lewat
wrapper C++, sama seperti program lain yang menyematkan `libvhdp`.

## Kebijakan bahasa

- C++20 untuk core, state machine, engine, concurrency, CLI, dan wrapper.
- C17 untuk C ABI publik, decoder ELF dan syscall, serta kode yang berjalan
  setelah `fork` dan sebelum `exec`.
- Assembly `.S` hanya untuk batas per arsitektur yang butuh kontrol register:
  gateway raw syscall (`src/arch/*/raw_syscall.S`) dan stub masuk/keluar loader
  userland. Ada rujukan C portabel (`src/arch/portable/raw_syscall_portable.c`),
  uji differential (`T-DIFF-RAW-SYSCALL-REGS`, `T-UNIT-RAW-SYSCALL`), dan opsi
  `VHDP_FORCE_PORTABLE`.

Alasan lengkapnya ada di [docs/adr/](docs/adr/).

## Model eksekusi rootless

Setiap sesi punya satu thread supervisor, karena ptrace terikat ke thread
tracer.

1. Spawn (`bootstrap.c`). `clone(SIGCHLD|CLONE_PIDFD)` menghasilkan child yang
   hanya memakai raw syscall (async-signal-safe, tanpa errno, TLS, atau
   alokasi). Child memasang `PR_SET_DUMPABLE` dan memberi tanda siap lewat pipe,
   mereset signal, menjalankan `setsid` dan `TIOCSCTTY` bila memakai PTY,
   menata stdio, menunggu tracer lewat pipe sinkronisasi, `chdir` ke cwd host,
   menjadikan semua fd di atas 2 close-on-exec, memasang
   `PR_SET_NO_NEW_PRIVS`, memasang filter seccomp bila aktif, lalu `execve`.
2. Seize. Parent melakukan `PTRACE_SEIZE` dengan
   `TRACESYSGOOD|TRACEFORK|TRACEVFORK|TRACECLONE|TRACEEXEC|EXITKILL` (ditambah
   `TRACESECCOMP` bila seccomp aktif), lalu melepas child dari pipe
   sinkronisasi.
3. Loop. `waitpid(-1, __WALL|__WNOTHREAD)`. Pada setiap syscall-stop, register
   dibaca (`src/arch`), syscall diklasifikasikan (`linux_abi/syscall_table`),
   lalu diteruskan ke handler (`syscall_handlers.cpp`): path diterjemahkan ke
   host, argumen ditulis di bawah stack pointer tracee, hasil diperbaiki saat
   exit-stop, atau syscall ditolak/diemulasikan. fork dan clone diikuti otomatis;
   `CLONE_UNTRACED` dilucuti dan `clone3` ditolak supaya libc kembali ke `clone`.
4. Akhir. Saat proses awal keluar, seluruh pohon proses dibunuh dan ditunggu
   sampai habis. Tidak ada zombie atau fd yang bocor (`T-LIFE-*`).

### Seccomp sebagai akselerator

Filter seccomp hanya meng-`ALLOW` syscall pass-through; sisanya
`SECCOMP_RET_TRACE`, sehingga keputusan tetap di tangan supervisor. Syscall tak
dikenal, ABI asing (compat/x32), clone dengan `CLONE_UNTRACED`, dan socket saat
`--network none` selalu ditrace. Bila probe seccomp gagal, engine memakai
`PTRACE_SYSCALL` penuh. Semantik keamanannya sama (lihat SECURITY.md).

## Penerjemahan path (VFS)

`MountTable` memetakan guest ke host dan sebaliknya dengan aturan prefix
terpanjang (rootfs, bind, proyeksi `vdr`). `resolver.cpp` menyelesaikan path
komponen demi komponen di ruang guest: setiap prefix di-`lstat`, symlink
ditafsirkan relatif terhadap root guest, dan `..` tidak pernah naik di atas `/`
guest. Hasilnya path host tanpa symlink di komponen antara, pada saat
pemeriksaan. Sisa risiko rename/TOCTOU dicatat di SECURITY.md; resolver ini
batas kompatibilitas, bukan sandbox.

## Exec dan loader indirection

Kernel me-resolve `PT_INTERP` terhadap root host. Untuk ELF dinamis guest,
supervisor menjalankan loader milik guest (di-resolve di dalam rootfs) dan
memberikan program sebagai argumen, misalnya
`ld-linux-*.so [--argv0 A] /prog args`. Akibatnya `/proc/self/exe` menunjuk ke
loader, dan bit setuid maupun file capability tidak pernah berlaku
(`no_new_privs` tetap aktif). Script `#!` di-resolve sampai kedalaman terbatas.
ELF untuk arsitektur lain ditolak `ENOEXEC` dengan diagnostic.

### Loader userland

Sebagian host melarang `execve` dari penyimpanan yang bisa ditulis proses itu
sendiri. Android API 29 ke atas melakukannya untuk domain aplikasi (W^X), dan
di sana kernel tidak akan menjalankan ELF apa pun dari dalam rootfs. Jalan
keluarnya `engines/rootless/loader/`: loader ELF userland yang dibangun sebagai
executable terpisah (`vhdp-loader`) dan dipasang di direktori pustaka native
aplikasi dengan nama `libvhdp-loader.so`, satu-satunya tempat yang boleh
di-exec. Supervisor meng-exec loader itu, lalu menyerahkan permintaan muat pada
`PTRACE_EVENT_EXEC`. Loader memetakan ELF guest dengan `mmap(PROT_EXEC)`, yang
diizinkan untuk berkas aplikasi, lalu melompat ke entry point.
`platform::find_userland_loader` menemukan loader (atau memakai `$VHDP_LOADER`),
`probe_exec_mapping` membuktikan pemetaan `PROT_EXEC` diizinkan, dan `doctor`
melaporkannya sebagai `exec.storage_policy`. Implementasinya original.

## Host berpolicy ketat (domain aplikasi Android)

Engine rootless tidak menganggap host menyediakan seluruh API Linux. Yang
ditolak host disediakan ulang, dan setiap mekanisme punya probe sendiri supaya
hanya aktif di host yang membutuhkannya.

- SIGSYS dari policy host. Policy seccomp aplikasi Android memakai
  `SECCOMP_RET_TRAP`, yang mengalahkan `RET_TRACE` milik engine. Supervisor
  menjawab SIGSYS itu sendiri (`handle_host_seccomp_trap`), tetapi hanya bila
  filter yang men-trap milik host. Begitu guest memasang filter sendiri
  (`prctl`/`seccomp` ditrace dan tgid dicatat), SIGSYS diteruskan ke handler
  guest.
- Syscall legacy yang ditrap. Di x86_64 policy itu men-trap `open`, `stat`,
  `access`, `dup2`, `poll`, dan lainnya yang masih dipakai glibc.
  `restart_legacy` menulis ulang panggilan menjadi padanan `*at` lewat
  `RegsAccess::restart_as` (memutar ulang instruction pointer). `vfork` sengaja
  tidak di-restart.
- Hardlink. `link(2)` ditolak di penyimpanan aplikasi, padahal dpkg, shadow, tar,
  dan `cp -al` membutuhkannya. `link_store.*` mengemulasikannya per mount
  (`.vhdp-hardlinks/<id>` dan record, `flock` antar sesi, objek kembali ke
  namanya saat hitungan turun ke satu). `st_nlink`, `st_ino`, `readlink`,
  `getdents64`, dan `rename` dirapikan agar tampak seperti hardlink sungguhan.
  Aktif hanya setelah `kernel_refuses_link` terbukti, dan penelusuran
  see-through dibatasi `links_visible()` agar sesi tanpa objek tidak
  menanggung biayanya.
- `/proc` global. Banyak berkas `/proc`, bahkan `lstat`-nya, ditolak.
  `proc_synth.*` membangunnya dari sumber yang diizinkan (`CLOCK_BOOTTIME`,
  `sysinfo`, `uname`, residensi cpuidle, `meminfo`, `self/mounts`) sehingga
  `uptime`, `vmstat`, `pstree`, dan `ps` bekerja. `/proc/<pid>/status` milik
  proses sesi ditulis ulang ke identitas guest.
- Identitas. Kredensial per proses (`struct Creds`) menjawab `getuid`,
  `setgid`, dan keluarganya tanpa privilese nyata. `NETLINK_AUDIT` dijawab
  `EPROTONOSUPPORT` karena shadow berhenti pada errno lain.
- Proses non-dumpable. Anak dari proses aplikasi tidak bisa di-ptrace sebelum
  `PR_SET_DUMPABLE`. Child bootstrap dan child probe memasangnya lalu memberi
  tanda siap lewat pipe sebelum `PTRACE_SEIZE`.

Biaya stop tambahan ditekan dengan `vfs::DirCache` (divalidasi dev/ino), query
read-only yang dijalankan supervisor sendiri (`host_stat`, `host_readlink`,
`host_xattr`, cukup satu stop), dan fixup stat yang digabung.

## Fungsi bantu rootfs

`core/configure.cpp`, `core/dpkg_recovery.cpp`, dan `core/projection.cpp`
bekerja tanpa engine: mereka hanya membaca atau menulis berkas rootfs, sehingga
bisa dipanggil dari proses yang tidak boleh menjalankan ptrace. Hasilnya berupa
dokumen JSON lewat `vhdp_configure_rootfs`, `vhdp_dpkg_plan_json`, dan
`vhdp_projection_plan_json`. Script pemulihan dpkg dan rencana bind dijalankan
atau diterapkan oleh pemanggil, biasanya lewat sesi rootless dengan uid guest 0.

## Lifecycle dan threading

State machine eksplisit: `created`, `starting`, `running`, lalu salah satu dari
`exited`, `failed`, `cancelled` (`core/lifecycle`). `Session` memiliki thread
waiter dan, bila ada timeout, thread watchdog. Callback event dan output berjalan
di thread internal library; aturan lifetime-nya ada di `vhdp.h`. Semua exception
ditahan di batas C ABI dan di titik masuk thread.

## C ABI stabil

Struct berversi (`struct_size` dan `abi_version`), handle opaque, integer
fixed-width untuk status/flag/kind, field reserved yang wajib nol, allowlist
ekspor lewat linker version script (`src/capi/libvhdp.map`), dan snapshot simbol
(`T-ABI-SYMBOLS`) memastikan tidak ada simbol STL atau C++ yang bocor. Detailnya
di [docs/EMBEDDING.md](docs/EMBEDDING.md) dan
[docs/adr/0005-c-abi.md](docs/adr/0005-c-abi.md).
