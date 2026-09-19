# Virtual Hardware Driver Platform (VHDP)

> Salinan yang disematkan di aplikasi dracxterm hanya membawa sumber. Direktori
> `docs/`, `tests/`, `examples/`, `benchmarks/`, `fuzz/`, dan `tools/` yang dirujuk
> di sini dan di `ARCHITECTURE.md` ada di proyek VHDP sendiri, bukan di pohon ini.
> Pekerjaan pada library dilakukan di proyek itu, lalu dicermin ke sini.

VHDP adalah **library** (`libvhdp`) dan **CLI** (`phdp`) untuk menjalankan sebuah
Linux root filesystem di atas Linux/Android **tanpa root perangkat**, dengan cara
menerjemahkan syscall dan path guest melalui sebuah *rootless supervisor* berbasis
`ptrace` (dipercepat `seccomp` bila tersedia). Pengalamannya mirip PRoot: shell
interaktif, file/symlink, pipe, PTY, signal, proses anak, dan tool CLI yang
syscall/kernel host-nya tersedia.

Status proyek: **1.0.0 BETA**. Vertical slice rootless untuk guest
same-architecture sudah berjalan dan terverifikasi oleh test suite di host Linux
x86_64, dan profil `android-app` kini `supported`: engine ini menjalankan terminal
aplikasi dracxterm di perangkat nyata (arm64, API 35, SELinux enforcing) maupun di
WayDroid x86_64 — lihat `phdp capabilities` dan
[ARCHITECTURE.md](ARCHITECTURE.md#host-berpolicy-ketat-domain-aplikasi-android).
Ini tetap **bukan** "full Linux" dan **bukan** security sandbox untuk rootfs tak
tepercaya.

## Isolasi: kompatibilitas, bukan keamanan

Rootless engine memberi **compatibility isolation** (path translation), **bukan
security isolation**. UID/GID 0 yang terlihat guest adalah emulasi, bukan root di
host. Jangan menjalankan rootfs yang tidak tepercaya dengan rootless engine; untuk
itu gunakan VM terisolasi. Lihat [SECURITY.md](SECURITY.md).

## Quick start (host Linux, arsitektur sama dengan guest)

```sh
cmake --preset host-debug
cmake --build --preset host-debug --parallel
ctest --preset host-debug --output-on-failure

# Jalankan sebuah command dalam rootfs
./build/host-debug/src/phdp run ./my-rootfs -- /bin/sh -l
# Bentuk singkat (alias run)
./build/host-debug/src/phdp ./my-rootfs -- /usr/bin/env
# Tanpa command -> /bin/sh
./build/host-debug/src/phdp ./my-rootfs

# Diagnostik dan inspeksi
./build/host-debug/src/phdp doctor
./build/host-debug/src/phdp inspect ./my-rootfs
./build/host-debug/src/phdp capabilities
```

Kontrak CLI kanonis:

```text
phdp run [OPTIONS] <ROOTFS_DIR> [-- <COMMAND> [ARG...]]
phdp [OPTIONS] <ROOTFS_DIR> [-- <COMMAND> [ARG...]]   # alias run
```

Nama `run`, `doctor`, `inspect`, `capabilities`, `help`, `version` di posisi
pertama dianggap subcommand. Rootfs bernama seperti itu ditulis sebagai path
(`./doctor`) atau lewat `phdp run doctor`. Jalankan `phdp run --help` untuk opsi.

Exit status: status guest; `128+N` bila guest dibunuh signal `N`; `124` pada
`--timeout`; `126`/`127` bila command tak dapat dieksekusi/ditemukan; `125` bila
sesi gagal dimulai; `2` untuk error argumen.

## Menggunakan sebagai library

Header C stabil ada di [`include/vhdp/vhdp.h`](include/vhdp/vhdp.h) dan wrapper
C++20 RAII di [`include/vhdp/vhdp.hpp`](include/vhdp/vhdp.hpp). CLI hanyalah
facade tipis di atas library yang sama. Lihat [docs/EMBEDDING.md](docs/EMBEDDING.md)
dan contoh di [`examples/`](examples/).

## Membangun untuk Android

```sh
cmake --preset android-arm64-release        # arm64-v8a (NDK resmi)
cmake --build --preset android-arm64-release --parallel
```

Menghasilkan `libvhdp.so`, `libvhdp.a`, dan `phdp` ARM aarch64 dengan
kompatibilitas page 16 KiB. Test/contoh dimatikan otomatis pada cross-build karena
tidak dapat dijalankan di host. Lihat [docs/ANDROID_BUILD.md](docs/ANDROID_BUILD.md).

## Struktur

```text
include/vhdp/    header publik: C ABI (vhdp.h) + wrapper C++ (vhdp.hpp)
src/core/        config, lifecycle, event, engine selection, session, reports
src/linux_abi/   ELF decoder, tabel syscall, tabel errno
src/vfs/         path translation: mount table + resolver simbolik
src/engines/     rootless (ptrace+seccomp), unavailable (rooted/emulator/vm)
src/arch/        adapter register per-arch + rutin .S (raw syscall gateway)
src/platform/    probe host, procfs, ptrace defs
src/capi/        implementasi C ABI + export map
src/cli/         phdp (parser, run, report) — facade tipis
hdr/             registry virtual hardware
vdr/             virtual driver / guest service (PTY, /dev, /proc, bind, network)
tests/ examples/ benchmarks/ fuzz/ docs/
```

## Dokumentasi

- [ARCHITECTURE.md](ARCHITECTURE.md) — arsitektur dan alur eksekusi
- [SECURITY.md](SECURITY.md) — threat model dan batas keamanan
- [docs/CAPABILITIES.md](docs/CAPABILITIES.md) — matriks kemampuan per profil
- [docs/ANDROID_BUILD.md](docs/ANDROID_BUILD.md) — build NDK & batas platform
- [docs/ROOTFS.md](docs/ROOTFS.md) — menyiapkan rootfs
- [docs/EMBEDDING.md](docs/EMBEDDING.md) — memakai `libvhdp` dari C/C++
- [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)
- [docs/adr/](docs/adr/) — keputusan arsitektur fundamental
- [CHANGELOG.md](CHANGELOG.md)

## Lisensi

Apache License 2.0. Lihat [`LICENSE`](LICENSE) dan [`NOTICE`](NOTICE).
Implementasi ini original dan tidak menyalin PRoot, QEMU, atau proyek lain; lihat
[docs/adr/0007-licensing.md](docs/adr/0007-licensing.md).
