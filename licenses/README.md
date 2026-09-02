# Licence texts for bundled third-party binaries

These are the licences covering the prebuilt binaries in
`app/src/main/jniLibs/arm64-v8a/` and the bundled fonts in
`app/src/main/res/font/`. The table below says which file is covered by which
licence. [`NOTICE`](../NOTICE) carries the upstream version of each binary and
the written offer of corresponding source.

| File | Covers |
|---|---|
| `GPL-2.0.txt` | BusyBox, PRoot, PRoot loader |
| `LGPL-3.0.txt` + `GPL-3.0.txt` | talloc (LGPL-3.0 is written as a set of additional permissions on top of GPL-3.0, so both texts are required) |
| `BSD-3-Clause.txt` | android-shmem |
| `OFL-1.1-Copse.txt` | Copse, the UI text face in `app/src/main/res/font/copse.ttf` |
| `OFL-1.1-BlackOpsOne.txt` | Black Ops One, the wordmark face in `app/src/main/res/font/black_ops_one.ttf` |

`GPL-2.0.txt`, `GPL-3.0.txt` and `LGPL-3.0.txt` are the canonical FSF texts as shipped in
Debian's `base-files` package (`/usr/share/common-licenses/`).

`BSD-3-Clause.txt` must be fetched from upstream so that the copyright line matches the
actual android-shmem authors rather than a generic template, run
`scripts/fetch-licenses.sh` once after cloning.
