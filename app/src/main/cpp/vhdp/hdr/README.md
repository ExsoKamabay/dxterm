# hdr — Virtual Hardware Registry

Folder ini menyimpan **registry dan descriptor virtual hardware**: deskripsi jujur
tentang kemampuan "seperti hardware" yang dilihat guest dan **bagaimana** tiap
kemampuan disediakan. Ini melanjutkan maksud awal folder `hdr`.

Kontrak: setiap entri punya `Provision` yang eksplisit sehingga forwarding syscall
biasa **tidak** disebut sebagai emulasi hardware:

- `host-passthrough` — resource kernel host dipakai langsung (CPU, memory, clock,
  uname, network). Bukan emulasi.
- `projected` — resource host diekspos lewat driver `vdr` di bawah policy
  (entropy, `/dev/null|zero|full`, terminal).
- `emulated` — nilai disintesis oleh VHDP (identitas uid/gid guest).
- `absent` — tidak disediakan (GPU, USB/raw block device, akselerasi).

Sumber kode: [`hardware_registry.hpp`](hardware_registry.hpp) /
[`hardware_registry.cpp`](hardware_registry.cpp). Registry ini juga muncul di
`phdp capabilities --json` pada field `hardware`.

Menambah descriptor baru: tambahkan entri di `hardware_registry()`, isi
`provision`, `provided_by` (modul/driver yang benar-benar menyediakannya), dan
`notes` yang menyatakan batasannya. Jangan menaikkan klaim melebihi yang benar
disediakan; kalau hanya pass-through, tandai `host-passthrough`.
