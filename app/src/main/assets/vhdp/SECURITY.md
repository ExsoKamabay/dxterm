# Keamanan dan Threat Model VHDP

## Batas yang tegas

Engine rootless VHDP memberi isolasi kompatibilitas (penerjemahan path). Ia
tidak memberi isolasi keamanan, dan tidak boleh dipakai untuk menjalankan rootfs
yang bermusuhan. UID/GID 0 yang terlihat guest adalah emulasi, bukan root di
host. Untuk workload yang tidak tepercaya, gunakan VM terisolasi; backend VM
belum diimplementasikan di rilis ini.

## Aset yang dilindungi

Parser dan importer harus aman terhadap input yang tidak tepercaya: isi rootfs,
ELF, nama path, symlink, config, environment, argumen, sumber bind, dan pesan
driver. Yang dilindungi: berkas host di luar mount yang diizinkan, kredensial dan
rahasia host, integritas proses host, dan file descriptor host.

## Kontrol yang diterapkan

- Penerjemahan path fail-closed. Resolver bekerja di ruang guest; `..` tidak
  bisa naik di atas root guest; symlink absolut ditafsirkan relatif root guest;
  loop symlink menghasilkan `ELOOP`; path atau fd yang menunjuk ke luar mount
  ditolak `EACCES`. Diuji: `T-FS-DOTDOT`, `T-FS-SYMLINK-ESCAPE`,
  `T-UNIT-RESOLVER-ESCAPE`, `T-FS-INHERITED-FD`, `T-FS-MAGIC-LINK`.
- Syscall fail-closed. Nomor tak dikenal, ABI asing (compat/x32), dan syscall
  berbahaya (`mount`, `unshare`, `setns`, `chroot`, `open_by_handle_at`,
  `io_uring`, `pidfd_getfd`, dan lainnya) ditolak dengan errno dan diagnostic.
  Tidak ada pass-through diam-diam. Diuji: `T-SYS-UNSUPPORTED`,
  `T-UNIT-SYSCALL-TABLE`.
- `no_new_privs` selalu diset, sehingga setuid dan file capability tidak pernah
  memberi hak di host. Loader indirection juga menetralkan bit setuid.
- Environment disaring. Hanya `TERM`, `COLORTERM`, `LANG`, `LC_ALL`, dan `TZ`
  yang diwariskan dari host; rahasia host tidak diteruskan secara default.
- FD ditutup. Semua fd di atas 2 menjadi close-on-exec di child; hanya
  stdin/stdout/stderr dan PTY yang diperlukan yang diwariskan.
- Bind eksplisit, dikanonikalisasi, dan read-only secara default (`:rw` harus
  diminta).
- Batas ukuran untuk input dari guest: path `PATH_MAX`, argv/env 256 KiB, argumen
  exec 512 KiB, tabel program header ELF 64 KiB, `PT_INTERP` `PATH_MAX`.
- Pembersihan pohon proses yang deterministik dan `PTRACE_O_EXITKILL`, sehingga
  pohon proses ikut mati bila supervisor mati. Diuji: `T-LIFE-TREE-CANCEL`,
  `T-LIFE-100-CYCLES`.
- Signal ke proses di luar sesi ditolak (`ESRCH`).
- Decoder ELF dan config memakai aritmetika yang aman dari overflow, decoding
  little-endian eksplisit, dan tidak mengalokasi berdasarkan angka dari input
  tanpa batas.
- Jalur setelah `fork` dan sebelum `exec` async-signal-safe (hanya raw syscall).

## Sisa risiko yang diketahui

Risiko berikut didokumentasikan, tidak diklaim aman.

- Rename/TOCTOU. Di antara resolusi path oleh VHDP dan lookup oleh kernel,
  rename atau pertukaran symlink yang berjalan bersamaan bisa mengubah target.
  Ini melekat pada pendekatan penerjemahan path dan menjadi alasan engine
  rootless tidak disebut sandbox. Kernel lama tanpa `openat2` memperlebar
  jendela ini.
- `--proc host` membuka informasi proses host (read-only) ke guest.
- Pemakaian ulang PID untuk tracee yang bukan anak langsung: setelah di-reap oleh
  induk aslinya, ada jendela kecil sebelum VHDP melihat bahwa proses itu keluar.
- Socket `AF_UNIX` abstract tetap bisa dijangkau saat `--network none` karena
  tidak berbentuk path.
- Emulasi hardlink. Nama hardlink diwakili symlink penanda ke
  `<mount>/.vhdp-hardlinks/<id>` dan resolver mengikutinya secara see-through
  (`lstat`, `O_NOFOLLOW`, dan `readlink` melihat berkas biasa). Guest yang
  menulis sendiri symlink berbentuk sama ikut diperlakukan begitu. Symlink itu
  tetap berada di dalam mount-nya, jadi tidak membuka jalan keluar, tetapi guest
  bisa membuat dua nama tampak sebagai satu berkas. Store juga terlihat oleh
  pembaca host di pohon yang sama, dan objeknya tertinggal bila sebuah sesi mati
  sebelum objek dikembalikan ke namanya. Mekanisme ini aktif hanya bila host
  menolak `link(2)`.
- Pengganti `/proc` global. Isinya disintesis (uptime, loadavg, stat, vmstat,
  dan lainnya) dari sumber yang diizinkan, jadi tidak sama dengan data kernel.
  Tool yang memakai angka itu untuk keputusan keamanan atau akuntansi tidak
  boleh dipercaya. `status` proses sesi menampilkan identitas guest, bukan uid
  host.

## Packaging Android

- Jalankan engine di service yang tidak diekspor dan, bila memungkinkan, di
  isolated process.
- Hanya teruskan file descriptor dan resource yang diizinkan.
- Jangan menurunkan `targetSdkVersion`, membypass SELinux, memakai exploit, atau
  menyamarkan executable. Aplikasi dengan targetSdk 29 ke atas tidak boleh
  meng-`execve` rootfs dari penyimpanan aplikasi yang bisa ditulis, dan VHDP
  tidak melakukannya: berkas yang di-`execve` hanya `phdp` dan
  `libvhdp-loader.so` dari direktori pustaka native (dipasang installer,
  read-only). Kode guest dimuat loader itu lewat `mmap(PROT_EXEC)` atas berkas
  aplikasi, jalur yang memang diizinkan platform, sehingga W^X tetap dihormati.
  Kode guest berjalan di konteks keamanan aplikasi dengan izin aplikasi, sama
  seperti kode lain yang dimuat aplikasi itu, dan tidak mendapat hak di luar
  itu. Lihat `phdp capabilities` (profil `android-app`) dan
  [ARCHITECTURE.md](ARCHITECTURE.md).

## Backend rooted (belum diimplementasikan)

Bila kelak dibuat, backend ini harus opt-in dengan peringatan yang jelas, tidak
mem-bind `/` host dalam mode tulis secara default, dan memakai root helper
sekecil mungkin dengan protokol tervalidasi yang segera melepas capability.

## Lisensi dan provenance

Implementasi original. Lihat [docs/adr/0007-licensing.md](docs/adr/0007-licensing.md).

## Melaporkan masalah keamanan

Project ini belum diaudit pihak luar. Sebelum audit, jangan mengandalkannya
sebagai batas keamanan terhadap kode yang bermusuhan.
