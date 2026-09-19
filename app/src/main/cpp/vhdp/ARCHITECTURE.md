# Arsitektur VHDP

## Lapisan

```
                 phdp (CLI, facade tipis)      examples C/C++
                            |                        |
                    include/vhdp/vhdp.hpp  (wrapper RAII C++20)
                            |
                    include/vhdp/vhdp.h    (C ABI stabil, versioned)
                            |
   +-------------------------------------------------------------+
   |  src/capi        implementasi C ABI, exception containment  |
   |  src/core        context, config, lifecycle, session,       |
   |                  event, engine selection, reports           |
   |  src/engines     rootless (ptrace+seccomp) | unavailable    |
   |  src/vfs         mount table + resolver simbolik            |
   |  src/linux_abi   ELF decoder, tabel syscall, tabel errno    |
   |  src/arch        register adapter per-arch + rutin .S       |
   |  src/platform    probe host, procfs, ptrace defs            |
   |  hdr / vdr       registry hardware / driver guest-service   |
   +-------------------------------------------------------------+
```

CLI tidak menduplikasi engine atau lifecycle; ia hanya memanggil C ABI.

## Kebijakan bahasa

- **C++20** untuk core, state machine, engine, concurrency, CLI, wrapper.
- **C17** untuk C ABI publik, ELF/syscall decoder, dan kode post-`fork`/pre-`exec`.
- **Assembly `.S`** hanya untuk boundary per-arsitektur yang benar-benar butuh
  kontrol register: raw syscall gateway (`src/arch/*/raw_syscall.S`). Ada referensi
  portable C (`src/arch/portable/raw_syscall_portable.c`), differential test
  (`T-DIFF-RAW-SYSCALL-REGS`, `T-UNIT-RAW-SYSCALL`), dan opsi `VHDP_FORCE_PORTABLE`.

Detail dan alasan ada di [docs/adr/](docs/adr/).

## Model eksekusi rootless

Satu **supervisor thread** per session (ptrace terikat ke thread tracer):

1. **Spawn** (`bootstrap.c`): `clone(SIGCHLD|CLONE_PIDFD)` menghasilkan child yang,
   memakai **hanya** raw syscall (async-signal-safe, tanpa errno/TLS/alloc),
   mereset signal, opsional `setsid`+`TIOCSCTTY` (PTY), menata stdio, menunggu
   tracer via pipe, `chdir` ke host cwd, set semua fd `>2` jadi close-on-exec,
   `PR_SET_NO_NEW_PRIVS`, opsional pasang filter seccomp, lalu `execve`.
2. **Seize**: parent melakukan `PTRACE_SEIZE` dengan
   `TRACESYSGOOD|TRACEFORK|TRACEVFORK|TRACECLONE|TRACEEXEC|EXITKILL`
   (+`TRACESECCOMP` bila seccomp aktif), lalu melepas child dari pipe sinkronisasi.
3. **Loop**: `waitpid(-1, __WALL|__WNOTHREAD)`. Pada tiap syscall-stop, register
   dibaca (`src/arch`), syscall diklasifikasikan (`linux_abi/syscall_table`), lalu
   di-*dispatch* (`syscall_handlers.cpp`): path diterjemahkan ke host, argumen
   ditulis di bawah stack pointer tracee, hasil diperbaiki saat exit-stop, atau
   syscall di-*deny*/*emulate*. fork/clone diikuti otomatis (CLONE_UNTRACED
   dilucuti; clone3 ditolak agar libc fallback ke clone).
4. **Akhir**: saat proses awal keluar, seluruh process tree dibunuh dan
   ditunggu sampai habis; tidak ada zombie/FD yang bocor (`T-LIFE-*`).

### Seccomp sebagai akselerator, bukan policy

Filter seccomp meng-`ALLOW` hanya syscall pass-through; sisanya
`SECCOMP_RET_TRACE` sehingga tetap diputuskan supervisor. Syscall tak dikenal,
ABI asing (compat/x32), clone dengan `CLONE_UNTRACED`, dan socket saat
`--network none` selalu ditrace. Bila probe seccomp gagal, engine jatuh ke
`PTRACE_SYSCALL` penuh. Semantik keamanan tidak berubah (lihat SECURITY.md).

## Path translation (VFS)

`MountTable` memetakan guest↔host dengan aturan longest-prefix (rootfs + bind +
proyeksi `vdr`). `resolver.cpp` menyelesaikan path **komponen demi komponen di
ruang guest**: tiap prefix di-`lstat`, symlink diinterpretasikan relatif terhadap
root guest, dan `..` tidak pernah naik di atas `/` guest. Hasilnya path host tanpa
symlink di komponen antara (pada saat pengecekan). Sisa risiko rename/TOCTOU
didokumentasikan di SECURITY.md — ini compatibility boundary, bukan sandbox.

## Exec dan loader indirection

Kernel me-resolve `PT_INTERP` terhadap root **host**. Untuk ELF dinamis guest,
supervisor menjalankan **loader milik guest** (di-resolve di dalam rootfs) dan
melewatkan program sebagai argumen (mis. `ld-linux-*.so [--argv0 A] /prog args`).
Konsekuensi (didokumentasikan): `/proc/self/exe` menunjuk loader; set-uid/file
capability tidak dihormati (`no_new_privs` tetap aktif). Script `#!` di-resolve
sampai kedalaman terbatas. ABI ELF asing ditolak `ENOEXEC` dengan diagnostik.

### Userland loader (host yang menolak exec dari storage tulis)

Sebagian host melarang `execve` dari penyimpanan yang bisa ditulis proses itu
sendiri (Android API ≥ 29: W^X untuk domain aplikasi). Di sana kernel tidak akan
pernah menjalankan ELF di dalam rootfs, sekuat apa pun supervisor-nya. Jalan
keluarnya `engines/rootless/loader/`: sebuah **loader ELF userland** yang dibangun
sebagai pustaka terpisah (`libvhdp-loader.so`, ikut terpasang di direktori pustaka
native aplikasi sehingga boleh di-exec). Supervisor meng-exec loader itu, lalu
menyerahkan permintaan muat pada `PTRACE_EVENT_EXEC`; loader memetakan ELF guest
dengan `mmap(PROT_EXEC)` — yang diizinkan untuk file aplikasi — dan melompat ke
entry point. `platform::find_userland_loader` menemukannya, `probe_exec_mapping`
membuktikan pemetaan PROT_EXEC memang boleh, dan `doctor` melaporkannya sebagai
`exec.storage_policy`. Implementasi original; tidak menyalin loader proyek lain.

## Host berpolicy ketat (domain aplikasi Android)

Engine rootless tidak mengandaikan host memberikan seluruh API Linux. Yang
ditolak host disediakan ulang, dan tiap mekanisme punya probe sendiri supaya
hanya aktif di host yang memang membutuhkannya:

- **SIGSYS dari policy host.** Policy seccomp aplikasi Android memakai
  `SECCOMP_RET_TRAP`, yang mengalahkan `RET_TRACE` milik engine. Supervisor
  menjawab SIGSYS itu sendiri (`handle_host_seccomp_trap`) — tetapi hanya bila
  filter yang men-trap adalah milik host: begitu guest memasang filter sendiri
  (`prctl`/`seccomp` ditrace, tgid dicatat), SIGSYS diteruskan ke handler guest.
- **Syscall legacy yang ditrap.** Di x86_64 policy itu men-trap `open`, `stat`,
  `access`, `dup2`, `poll`, … yang masih dipakai glibc. `restart_legacy` menulis
  ulang instruksi menjadi padanan `*at` lewat `RegsAccess::restart_as` (memutar
  ulang instruction pointer). `vfork` sengaja tidak di-restart.
- **Hardlink.** `link(2)` ditolak di penyimpanan aplikasi, sementara dpkg,
  shadow, tar dan `cp -al` membutuhkannya. `link_store.*` mengemulasikannya per
  mount (`.vhdp-hardlinks/<id>` + record, `flock` antar sesi, collapse saat
  hitungan turun ke satu); `st_nlink`, `st_ino`, `readlink`, `getdents64` dan
  `rename` dirapikan agar tampak seperti hardlink sungguhan. Aktif hanya setelah
  `kernel_refuses_link` terbukti, dan penelusuran see-through digating
  `links_visible()` agar sesi tanpa objek tidak menanggung biayanya.
- **`/proc` global.** Banyak berkas `/proc` (bahkan `lstat`-nya) ditolak.
  `proc_synth.*` membangunnya dari sumber yang diizinkan (`CLOCK_BOOTTIME`,
  `sysinfo`, `uname`, residensi cpuidle, `meminfo`, `self/mounts`) sehingga
  `uptime`, `vmstat`, `pstree`, `ps` bekerja; `/proc/<pid>/status` milik proses
  sesi ditulis ulang ke identitas yang diemulasikan.
- **Identitas.** Kredensial per proses (`struct Creds`) menjawab
  `getuid`/`setgid`/… tanpa privilese nyata, dan `NETLINK_AUDIT` dijawab
  `EPROTONOSUPPORT` karena shadow abort pada errno lain.
- **Proses non-dumpable.** Anak proses aplikasi tidak bisa di-ptrace sebelum
  `PR_SET_DUMPABLE`; child bootstrap/probe memasangnya lalu memberi tanda siap
  lewat pipe sebelum `PTRACE_SEIZE`.

Biaya stop tambahan ditekan dengan `vfs::DirCache` (divalidasi dev/ino), query
read-only yang dijalankan supervisor sendiri (`host_stat`, `host_readlink`,
`host_xattr` — satu stop, bukan dua), dan fixup stat yang digabung.

## Lifecycle & threading

State machine eksplisit: `created → starting → running → {exited, failed,
cancelled}` (`core/lifecycle`). `Session` memiliki waiter thread + optional
watchdog (timeout). Callback event/output berjalan di thread internal library;
aturan lifetime ada di `vhdp.h`. Semua exception ditahan di boundary C ABI dan
thread-entry.

## C ABI stabil

Struct berversi (`struct_size`+`abi_version`), opaque handle, integer
fixed-width untuk status/flag/kind, reserved-zero fields, export allowlist via
linker version script (`src/capi/libvhdp.map`), dan snapshot simbol
(`T-ABI-SYMBOLS`) memastikan tidak ada simbol STL/C++ yang bocor. Detail di
[docs/EMBEDDING.md](docs/EMBEDDING.md) dan
[docs/adr/0005-c-abi.md](docs/adr/0005-c-abi.md).
