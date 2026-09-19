# Licence texts for bundled third-party binaries

These are the licences covering the prebuilt binaries in
`app/src/main/jniLibs/<abi>/` (arm64-v8a and x86_64) and the bundled fonts in
`app/src/main/res/font/`. The table below says which file is covered by which
licence. [`NOTICE`](../NOTICE) carries the upstream version of each binary and
the written offer of corresponding source.

| File | Covers |
|---|---|
| `GPL-2.0.txt` | BusyBox, PRoot, PRoot loader |
| `LGPL-3.0.txt` + `GPL-3.0.txt` | talloc (LGPL-3.0 is written as a set of additional permissions on top of GPL-3.0, so both texts are required) |
| `BSD-3-Clause.txt` | android-shmem |
| `Apache-2.0.txt` | VHDP: the prebuilts `libphdp.so` and `libvhdp-loader.so`, the `vhdp` command in `app/src/main/assets/vhdp/bin/<abi>/`, and `libvhdp.so` plus `libvhdpjni.so`, which are built from the source in `app/src/main/cpp/vhdp/` |
| `OFL-1.1-Copse.txt` | Copse, the UI text face in `app/src/main/res/font/copse.ttf` |
| `OFL-1.1-BlackOpsOne.txt` | Black Ops One, the wordmark face in `app/src/main/res/font/black_ops_one.ttf` |

`GPL-2.0.txt`, `GPL-3.0.txt` and `LGPL-3.0.txt` are the canonical FSF texts as shipped in
Debian's `base-files` package (`/usr/share/common-licenses/`).

`BSD-3-Clause.txt` is the upstream android-shmem `LICENSE`, copied verbatim from
<https://raw.githubusercontent.com/pelya/android-shmem/master/LICENSE> so the copyright
line names the actual authors rather than a generic template.
