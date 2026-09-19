# Keamanan dan Threat Model VHDP

## Pernyataan batas yang tegas

VHDP rootless engine memberi **compatibility isolation** (path translation),
**bukan security isolation**. Ia **tidak diizinkan dan tidak didukung untuk
menjalankan rootfs yang hostile**. UID/GID 0 yang terlihat guest adalah emulasi
dan **bukan** root di host. Untuk workload tak tepercaya, gunakan VM terisolasi
dengan boundary yang sesuai (backend VM belum diimplementasikan di rilis ini).

## Aset yang harus dilindungi

Parser/importer harus aman menghadapi input tak tepercaya: isi rootfs, ELF, nama
path, symlink, config, environment, argument, sumber bind, dan pesan driver. Yang
dilindungi: berkas host di luar mount yang diizinkan, kredensial/secret host,
integritas proses host, dan file descriptor host.

## Kontrol yang diterapkan

- **Path translation fail-closed.** Resolver bekerja di ruang guest; `..` tidak
  bisa naik di atas root guest; symlink absolut diinterpretasikan relatif root
  guest; loop symlink → `ELOOP`; path/fd yang menunjuk ke luar mount ditolak
  `EACCES`. Diuji: `T-FS-DOTDOT`, `T-FS-SYMLINK-ESCAPE`, `T-UNIT-RESOLVER-ESCAPE`,
  `T-FS-INHERITED-FD`, `T-FS-MAGIC-LINK`.
- **Syscall fail-closed.** Nomor tak dikenal, ABI asing (compat/x32), dan syscall
  berbahaya (`mount`, `unshare`, `setns`, `chroot`, `open_by_handle_at`,
  `io_uring`, `pidfd_getfd`, dst.) ditolak dengan errno + diagnostic; tidak ada
  pass-through diam-diam. Diuji: `T-SYS-UNSUPPORTED`, `T-UNIT-SYSCALL-TABLE`.
- **`no_new_privs` selalu diset**; setuid/file-capability tidak pernah memberi
  privilege host. Loader indirection juga menetralkan bit setuid.
- **Environment disanitasi.** Hanya allowlist (`TERM`, `COLORTERM`, `LANG`,
  `LC_ALL`, `TZ`) yang diwariskan; secret host tidak diteruskan default.
- **FD ditutup.** Semua fd `>2` jadi close-on-exec di child; hanya stdin/stdout/
  stderr/PTY yang diperlukan yang diwariskan.
- **Bind eksplisit, dikanonikalisasi, read-only default** (`:rw` opt-in).
- **Batas ukuran** dari input guest: path `PATH_MAX`, argv/env 256 KiB, argumen
  exec 512 KiB, tabel program header ELF 64 KiB, `PT_INTERP` `PATH_MAX`.
- **Cleanup process-tree** deterministik + `PTRACE_O_EXITKILL` agar tree mati bila
  supervisor mati. Diuji: `T-LIFE-TREE-CANCEL`, `T-LIFE-100-CYCLES`.
- **Signal ke luar session ditolak** (`ESRCH`).
- **ELF/config decoder** memakai aritmetika overflow-safe, decoding little-endian
  eksplisit, dan tidak mengalokasi berdasarkan angka dari input tanpa batas.
- **Async-signal-safety** di jalur post-`fork`/pre-`exec` (hanya raw syscall).

## Sisa risiko yang diketahui (didokumentasikan, bukan diklaim aman)

- **Rename/TOCTOU.** Antara resolusi path oleh VHDP dan lookup oleh kernel, sebuah
  rename/symlink-swap konkuren dapat mengubah target. Ini melekat pada pendekatan
  path-translation dan menjadi alasan rootless engine bukan sandbox. Kernel lama
  tanpa `openat2` memperbesar jendela ini.
- **`--proc host`** mengekspos informasi proses host (read-only) ke guest.
- **Reuse PID** untuk tracee yang bukan child langsung: setelah di-reap oleh parent
  aslinya, ada jendela kecil sebelum VHDP mengamati exit-nya.
- **Abstract `AF_UNIX`** tetap terjangkau saat `--network none` (bukan path).
- **Emulasi hardlink.** Nama hardlink diwakili symlink penanda ke
  `<mount>/.vhdp-hardlinks/<id>` dan resolver mengikutinya *see-through* (lstat,
  `O_NOFOLLOW` dan `readlink` melihat file biasa). Guest yang menulis sendiri
  symlink berbentuk sama ikut diperlakukan begitu — tetap di dalam mount-nya, jadi
  bukan jalan keluar, tapi guest bisa membuat dua nama tampak sebagai satu file.
  Store juga terlihat oleh pembaca host di tree yang sama, dan objeknya tetap ada
  bila sebuah sesi mati sebelum collapse. Aktif hanya bila host menolak `link(2)`.
- **Stand-in `/proc` global.** Isinya disintesis (uptime, loadavg, stat, vmstat,
  dst.) dari sumber yang diizinkan, jadi bukan kebenaran kernel: tool yang memakai
  angka itu untuk keputusan keamanan/akuntansi tidak boleh dipercaya. `status`
  proses sesi memperlihatkan identitas yang diemulasikan, bukan uid host.

## Untuk packaging Android (AAR)

- Jalankan engine di service non-exported dan, bila kompatibel, isolated process.
- Broker hanya file descriptor/resource yang diizinkan.
- Jangan menurunkan `targetSdkVersion`, membypass SELinux, memakai exploit, atau
  menyamarkan executable. Aplikasi modern (targetSdk ≥ 29) **tidak boleh**
  meng-`execve` rootfs dari storage app yang writable, dan VHDP tidak
  melakukannya: satu-satunya berkas yang di-`execve` adalah `phdp` dan
  `libvhdp-loader.so` dari direktori pustaka native (dipasang installer,
  read-only). Kode guest dimuat loader itu lewat `mmap(PROT_EXEC)` atas berkas
  aplikasi — jalur yang memang diizinkan platform — sehingga W^X tetap dihormati.
  Konsekuensinya jujur: kode guest berjalan di konteks keamanan aplikasi dengan
  izin aplikasi, sama seperti kode lain yang dimuat aplikasi itu, dan tidak
  mendapat privilese apa pun di luar itu. Lihat `phdp capabilities` (profil
  `android-app`) dan [ARCHITECTURE.md](ARCHITECTURE.md).

## Rooted backend (belum diimplementasikan)

Bila kelak ada: harus opt-in dengan peringatan keras, tidak bind `/` host writable
secara default, root helper sekecil mungkin dengan protokol tervalidasi yang segera
menurunkan capability.

## Lisensi & provenance

Implementasi original; tidak menyalin PRoot/QEMU/proyek GPL lain. Lihat
[docs/adr/0007-licensing.md](docs/adr/0007-licensing.md).

## Melaporkan masalah keamanan

Ini proyek fondasi tanpa jaminan. Sebelum audit eksternal, jangan
mengandalkannya sebagai batas keamanan terhadap kode hostile.
