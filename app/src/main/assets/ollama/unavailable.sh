#!/bin/sh
# drac-Xterm `ollama`, NO-ROOTFS (BusyBox) path.
#
# Installed on the BusyBox PATH so that typing `ollama` without a Linux rootfs produces an
# explicit, actionable explanation instead of "ollama: not found", and never a faked success.
#
# This is not a guess. The official ARM64 artifact was inspected directly:
#
#   bin/ollama: ELF 64-bit LSB pie executable, ARM aarch64, dynamically linked,
#               interpreter /lib/ld-linux-aarch64.so.1, for GNU/Linux 3.7.0
#   DT_NEEDED : libresolv.so.2 libdl.so.2 libpthread.so.0 libstdc++.so.6 libc.so.6
#
# `/lib/ld-linux-aarch64.so.1` is the GLIBC dynamic loader. Android uses Bionic (linker64) and has
# no glibc, no libstdc++.so.6 and no /lib/ld-linux-aarch64.so.1, and BusyBox supplies none of
# them. The binary therefore CANNOT execute on the BusyBox/Android host under any configuration.
# It requires the Linux rootfs, and that is where drac-Xterm installs it.
cat >&2 <<'EOF'
OLLAMA RUNTIME BLOCKED

Reason:
  Ollama requires the Linux runtime/rootfs.
  This session is running the BusyBox shell directly on Android, with no Linux rootfs.

Detected:
  runtime            = BusyBox / Android (Bionic libc)
  rootfs             = not provisioned

Required:
  The official ollama ARM64 build is a glibc binary:
    ELF 64-bit LSB pie executable, ARM aarch64
    interpreter /lib/ld-linux-aarch64.so.1   (glibc, not Bionic)
    needs libstdc++.so.6, libresolv.so.2, libpthread.so.0, libdl.so.2, libc.so.6
  Android/BusyBox provides none of these, so the binary cannot be executed here.

Resolution:
  Start drac-Xterm with the Linux rootfs provisioned (the bundled Kali ARM64 image
  is extracted to the app's private storage on first run), then run `ollama` again
  from inside the Linux shell.
EOF
exit 1
