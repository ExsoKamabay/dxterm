# Building the prebuilt binaries from source

dracxterm ships five ARM64 ELF binaries in `app/src/main/jniLibs/arm64-v8a/`.
Every one of them is built here, from a pinned upstream revision, by the recipes
in this directory. That is what makes reproducible-build verification possible
and what keeps the shipped BusyBox current instead of a 2018 build carrying
accumulated CVEs.

This directory closes that gap. Every binary in the APK can now be built from a
pinned upstream source with a pinned NDK, by anyone, with one command.

```sh
./prebuilts/build.sh              # build all five, print SHA-256 of each
./prebuilts/build.sh talloc proot # build a subset (dependency order is enforced)
./prebuilts/build.sh --install    # build, then copy into app/src/main/jniLibs/
```

Output goes to `prebuilts/work/out/`. **Nothing is installed into `jniLibs`
unless you pass `--install`**, replacing the shipped binaries changes what the
APK contains, and that should be deliberate.

## Requirements

- NDK `27.0.12077973` (the version pinned in `app/build.gradle.kts`). The scripts
  refuse to substitute a different one, since the whole point is that the output
  is a function of the recipe rather than of the machine.
  `sdkmanager "ndk;27.0.12077973"`, or set `ANDROID_NDK_HOME`.
- `git`, `curl`, `make`, `patch`, `tar`, a host C compiler for BusyBox's build tools.
- No gawk. Upstream PRoot needs it; patch `0002` removes that dependency.

## What gets built

| Artifact | Upstream | Version | Notes |
|---|---|---|---|
| `libbusybox.so` | busybox.net | **1.38.0** | Was 1.29.3 (2018). Static, 332 applets |
| `libtalloc.so` | samba.org | 2.5.0 | `SONAME libtalloc.so.2`, as PRoot expects |
| `libproot.so` | termux/proot | v5.1.107.91 | |
| `libproot-loader.so` | termux/proot | v5.1.107.91 | Built alongside proot |
| `libandroid-shmem.so` | termux/libandroid-shmem | v0.7 | **Not** pelya/android-shmem, see below |

Sources are pinned in [`manifest.env`](manifest.env): tarballs by SHA-256, git
checkouts by full commit SHA. A digest mismatch is a hard failure, never a
warning.

## Reproducibility

The five artifacts are **bit-identical across build directories**. Verified by
building the whole set twice into paths of deliberately different lengths and
comparing SHA-256:

```
libandroid-shmem.so    c80ccfe16e30c28f00d3a46cb1f33983…  identical
libtalloc.so           557e93989ea7b0ce471620f04941442e…  identical
libbusybox.so          0d9f6306030e7af3ee8e98a8c6dc432b…  identical
libproot.so            ec31292345954a947bcf7dd01c66a0f1…  identical
libproot-loader.so     493c90afc5a88523d864007bca95172c…  identical
```

Getting there needed three fixes, all found by measurement rather than assumed:

1. **Absolute paths leak into the binary.** clang records the source path in
   `__FILE__`, assertion strings and `DW_AT_comp_dir`, so the same sources built
   from two directories produced different bytes, `libtalloc.so` came out
   37,960 bytes from one and 37,896 from another purely because the paths
   differed in length. `-ffile-prefix-map` and `-no-canonical-prefixes` are now
   applied by every recipe.

2. **BusyBox stamps the wall clock into its banner.** kconfig writes the moment
   it ran into `AUTOCONF_TIMESTAMP`, and `libbb/messages.c` builds the
   `busybox --help` banner from it. Exactly three bytes differed between two
   builds, a clock reading. The recipe replaces it with a fixed string, so the
   banner reads `BusyBox v1.38.0 (drac-Xterm)`.

3. **PRoot inherits whatever git repository it is built inside.** Its
   `GNUmakefile` stamps `git describe --tags --dirty --always` into `build.h`,
   and git walks up parent directories, so a build under `prebuilts/work`
   stamped *dracxterm's* commit into PRoot's version string, while a build in
   `/tmp` got no version at all. Wrong twice over: the value is meaningless, and
   it makes the binary depend on where it was built. The recipe now feeds the
   makefile a stand-in that reports the pinned tag, so `proot --version` says
   `v5.1.107.91` wherever it was built.

Verified three ways: across build directories of different lengths; with one
build inside the git repository and one outside it (the case that produced the
third bug); and **across machines**, a GitHub `ubuntu-latest` runner produces
all five of the hashes above, byte for byte, and CI re-checks that on every
scheduled run.

What is still unverified is a different host OS or a differently-built NDK.
Both builds compared here are Linux x86-64 with the same NDK package.

## Patches

Every patch is small, carries its reasoning in the diff, and exists because
upstream is wrong or stale, not to change behaviour.

**`busybox/patches/`**

- `0001-strchrnul-is-in-bionic-from-api-24`, `include/platform.h` undefines
  `HAVE_STRCHRNUL` for all Android builds, so libbb defines its own, which then
  collides with libc.a's at static link time. bionic has had `strchrnul` since
  API 24.
- `0002-dont-redefine-syscalls-bionic-has`, `libbb/missing_syscalls.c` still
  defines `getsid`, `sethostname` and `adjtimex` for Android, as it did in 2012.
  bionic has all three, and the duplicates break the static link. `pivot_root`
  stays: bionic really has no wrapper for it.

**`proot/patches/`**

- `0001-ashmem-memfd-include-string-h`, `extension/ashmem_memfd/ashmem_memfd.c`
  calls `memset` and `strcmp` without including `<string.h>`.
- `0002-loader-info-awk-posix`, `loader/loader-info.awk` used two gawk
  extensions (`strtonum()` and the `\y` word boundary). On any system whose
  `/usr/bin/awk` is mawk, the Debian and Ubuntu default, the build dies with
  `function strtonum never defined`. Rewritten in POSIX awk, which removes gawk
  from the build requirements entirely.

**`android-shmem/patches/`**

- `0001-bionic-fcntl-and-runtime-tmpdir`, two things. `shmem.c` calls `open()`
  without `<fcntl.h>`. And it builds its key-symlink path from `_PATH_TMP`,
  which Termux patches into its own `<paths.h>` as `$PREFIX/tmp`, bionic has no
  `_PATH_TMP` at all, so it does not compile outside Termux, and where it did it
  baked `/data/data/com.termux/…` into the binary. The path is now read from
  `TMPDIR` at runtime, which `Bootstrap` already sets in every environment the
  app spawns, with `sprintf` upgraded to `snprintf` since the length is no
  longer bounded by the source.

**`busybox/bionic-compat.h`** is not a patch but a header injected through
`libbb.h`. It supplies `addmntent()`, which bionic lacks at every API level and
which `util-linux/mount.c` calls outside any config guard, so no combination of
options avoids needing it. The implementation writes the six fstab(5) fields
`getmntent` parses back; it is not a stub that returns success.

**`talloc/bionic-replace.h`** replaces Samba's `lib/replace` portability layer.
The alternative is cross-compiling waf with a hand-maintained "cross answers"
file, a second source of truth whose wrong answers fail silently. talloc.c is
one translation unit and its portability surface is small enough to state
explicitly. The file explains each define; the notable one is `memset_explicit`,
which bionic declares only from API 34 while this app targets minSdk 24.

## A note on the checks themselves

Each recipe ends with checks, architecture, symbols, `NEEDED` entries, applet
count, absence of embedded Termux paths. They are written without pipes, through
`contains`/`file_contains` in `lib/common.sh`, and that is deliberate.

The obvious idiom, `producer | grep -q pattern`, is silently inverted under
`set -o pipefail`: `grep -q` exits the instant it matches, the producer dies of
SIGPIPE, and pipefail makes that the pipeline's status. The pipeline reports
failure exactly when the pattern **was** found, so the check passes precisely
when it should have failed.

It was not theoretical here. The proot recipe checks that the built binary
carries no Termux paths. An early build did carry one, the check found it, and
the script printed `no com.termux paths` and continued. A CI runner reported the
truth only because its grep buffered differently, which is how the bug was
noticed at all.

A check that cannot fail is worse than no check, because it is read as evidence.

## Two upstreams that are not what they look like

**android-shmem must be Termux's fork.** PRoot's sysvipc extension calls
`libandroid_shmat_fd()` and `libandroid_shmdt_fd()`, which exist only in
`termux/libandroid-shmem`. Building against the original `pelya/android-shmem`
fails with `call to undeclared function libandroid_shmat_fd`. The licence is the same BSD 3-Clause, with Fredrik
Fornwall's copyright added alongside Sergii Pylypenko's.

**PRoot must be Termux's fork** as well, for the Android fixes the app depends
on. That one was already documented correctly.

## What changes if you install these

The binaries currently in `jniLibs` are Termux builds and still contain
`/data/data/com.termux/files/usr/lib` and `…/libexec/proot/loader`. `Bootstrap`
works around that at runtime by setting `PROOT_LOADER` and the library search
path. Binaries built here contain no Termux paths, the recipe fails the build
if they do, so those workarounds stop being load-bearing. They are harmless,
but they will describe something that is no longer true.

`NOTICE` records the upstream version of each shipped file. Installing new
binaries without updating it leaves the documented inventory disagreeing with
the APK.

## The thing this does not do

**None of these binaries has been run.** They compile, they link, they are
AArch64, their symbol tables and `NEEDED` entries match what the app expects,
and the recipes check all of that. That is not the same as working.

A BusyBox jump from 1.29.3 (2018) to 1.38.0 crosses seven years of upstream
change, and the applet set is not identical, `ifconfig`, `route`, `netstat`,
`ip`, `hush`, `logname` and the SysV IPC tools are gone, each for a reason
recorded in `android.fragment`. PRoot changes from a Termux-patched binary to
one built here.

Before any of this ships: install on real arm64 hardware and exercise the
BusyBox fallback shell, then a full rootfs provisioning and PRoot launch. Until
that has happened, `--install` produces a candidate, not a release.
