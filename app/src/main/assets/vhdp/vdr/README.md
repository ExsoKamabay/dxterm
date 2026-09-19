# vdr — Virtual Drivers / Guest Services

Folder ini menyimpan **adapter yang mengekspos resource host ke guest**. Ini
melanjutkan maksud awal folder `vdr`. Setiap driver punya lifecycle, deklarasi
permission, probe, policy, test, dan cleanup — bukan emulasi hardware, melainkan
proyeksi/broker resource yang benar-benar ada.

Sumber kode: [`drivers.hpp`](drivers.hpp) / [`drivers.cpp`](drivers.cpp). Katalog
lengkap juga muncul di `phdp capabilities --json` pada field `drivers`.

Driver yang tersedia:

| id | ringkasan | policy default |
|----|-----------|----------------|
| `host-bind` | bind direktori host eksplisit | tidak ada bind; read-only kecuali `:rw`; source dikanonikalisasi; guest target tanpa `..` |
| `dev-minimal` | proyeksi `/dev/null,zero,full,random,urandom,tty,ptmx,pts` | aktif default (`--dev minimal`); tidak ada device node lain; `--dev none` mematikan |
| `proc-host` | proyeksi read-only host `/proc` | mati default (`--proc none`); `/proc/self` diremap ke proses guest; magic link ke luar mount ditolak (fail closed) |
| `pty` | pseudo-terminal untuk guest | guest jadi session leader dengan PTY sebagai controlling terminal; resize via `TIOCSWINSZ` |
| `network-policy` | pass-through host atau tolak IP socket | `--network host` (default) atau `--network none` (tolak socket non-`AF_UNIX`, ditegakkan seccomp bila ada) |

Kontrak menambah driver:

1. Definisikan `DriverDescriptor` di `driver_catalogue()` dengan `permissions`,
   `policy`, dan `cleanup` yang jujur.
2. Sediakan mekanisme proyeksi (mount table entry lewat `build_mount_table`, atau
   objek dengan lifecycle seperti `PtyDriver`).
3. Fail closed untuk resource yang tidak dapat diproyeksikan dengan aman.
4. Tambahkan test integrasi dan hubungkan ke ID test di
   [../docs/CAPABILITIES.md](../docs/CAPABILITIES.md).

Driver masa depan (audio/clipboard/GUI bridge) harus mengikuti kontrak yang sama
dan tidak boleh dilaporkan sebagai "supported" sebelum ada test yang lulus.
