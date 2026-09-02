#!/usr/bin/env bash
# Build libbusybox.so, the BusyBox multi-call binary that backs drac-Xterm's
# fallback shell (and the tools the provisioning pipeline uses before a rootfs
# exists). Despite the .so name it is an executable, not a shared library; see
# prebuilts/README.md for why everything ships under jniLibs.
#
# Why this recipe exists: the binary previously shipped was BusyBox 1.29.3,
# built by a third party in 2018. Many CVEs have been fixed upstream since, and
# nothing in the repository could rebuild it.
#
# Two upstream patches are applied. Both fix the same class of problem ,
# BusyBox's Android support was written when bionic was much smaller, and it
# still assumes bionic lacks functions it has had for years. See
# patches/ for the details; each carries its reasoning in the diff.
. "$(dirname "$0")/../lib/common.sh"

RECIPE_DIR="$PREBUILTS_DIR/busybox"

setup_toolchain
fetch_tarball "$BUSYBOX_URL" "$BUSYBOX_SHA256" "busybox-$BUSYBOX_VERSION.tar.bz2"

SRC="$WORK_DIR/src/busybox-$BUSYBOX_VERSION"
rm -rf "$SRC"
mkdir -p "$WORK_DIR/src"
tar -xjf "$DL_DIR/busybox-$BUSYBOX_VERSION.tar.bz2" -C "$WORK_DIR/src"
[ -f "$SRC/Makefile" ] || die "busybox sources not found under $SRC"

# --- patches -----------------------------------------------------------------
note "applying patches"
for p in "$RECIPE_DIR"/patches/*.patch; do
    [ -e "$p" ] || break
    printf '    %s\n' "$(basename "$p")"
    patch -s -p1 -d "$SRC" < "$p" || die "patch failed: $(basename "$p")"
done

# addmntent() is not behind any config guard in util-linux/mount.c, so it has to
# exist. Injected through libbb.h rather than -include: CONFIG_EXTRA_CFLAGS also
# reaches the assembler, which chokes on a C header.
cp "$RECIPE_DIR/bionic-compat.h" "$SRC/include/"
if ! grep -q 'bionic-compat.h' "$SRC/include/libbb.h"; then
    printf '\n/* drac-Xterm: bionic gap-fillers, see prebuilts/busybox/ */\n#include "bionic-compat.h"\n' \
        >> "$SRC/include/libbb.h"
fi

# --- configuration -----------------------------------------------------------
note "configuring"
make -C "$SRC" defconfig > /dev/null

# Apply the fragment, then let kconfig resolve the cascade.
#
# This is a loop, not a single pass, because `silentoldconfig` re-defaults some
# symbols after a dependency of theirs changes: applying the fragment once and
# trusting it silently put CONFIG_SHELL_HUSH back to y, and the build then died
# on glob()/sigisemptyset() with a config that claimed hush was off. Apply,
# cascade, and repeat until the file stops changing, then prove every directive
# actually took, rather than assuming it did.
apply_fragment() {
    while IFS= read -r line; do
        case "$line" in
            '# CONFIG_'*' is not set')
                opt=${line#'# '}; opt=${opt%' is not set'}
                sed -i "s/^${opt}=.*/# ${opt} is not set/" "$SRC/.config"
                ;;
            CONFIG_*)
                opt=${line%%=*}
                sed -i -e "s/^# ${opt} is not set/${line}/" -e "s/^${opt}=.*/${line}/" "$SRC/.config"
                ;;
        esac
    done < "$RECIPE_DIR/android.fragment"
}

converged=0
for round in 1 2 3 4 5; do
    cp "$SRC/.config" "$SRC/.config.before"
    apply_fragment
    # BusyBox's kconfig has no `olddefconfig`; `silentoldconfig` is the
    # non-interactive equivalent that resolves the cascade without prompting.
    make -C "$SRC" silentoldconfig > /dev/null
    # Ignore the generated header: kconfig stamps the current time into it, so a
    # byte comparison never reports convergence even when nothing changed.
    if diff -q <(grep -v '^#.*[0-9][0-9]:[0-9][0-9]:[0-9][0-9]' "$SRC/.config.before") \
                <(grep -v '^#.*[0-9][0-9]:[0-9][0-9]:[0-9][0-9]' "$SRC/.config") > /dev/null; then
        note "config converged after $round round(s)"
        converged=1
        break
    fi
done
[ "$converged" -eq 1 ] || die "config did not converge after 5 rounds; the fragment and kconfig disagree"

# Prove it. A directive that kconfig quietly overrode is exactly the failure
# this recipe exists to make impossible.
note "verifying the fragment held"
drift=0
while IFS= read -r line; do
    case "$line" in
        '# CONFIG_'*' is not set')
            opt=${line#'# '}; opt=${opt%' is not set'}
            if grep -q "^${opt}=y" "$SRC/.config"; then
                printf '    NOT APPLIED: %s is still enabled\n' "$opt" >&2
                drift=1
            fi
            ;;
        CONFIG_*)
            grep -qx "$line" "$SRC/.config" || {
                printf '    NOT APPLIED: %s\n' "$line" >&2
                drift=1
            }
            ;;
    esac
done < "$RECIPE_DIR/android.fragment"
[ "$drift" -eq 0 ] || die "kconfig overrode part of android.fragment; see the lines above"
printf '    every directive in android.fragment is reflected in .config\n'

note "applets enabled: $(grep -c '^CONFIG_[A-Z0-9_]*=y' "$SRC/.config")"

# kconfig stamps the moment it ran into AUTOCONF_TIMESTAMP, and libbb/messages.c
# builds the `busybox --help` banner out of it. That single string is the only
# thing that differed between two builds of identical sources here, three bytes
# of a clock reading. Replace it with the upstream version, which is a fact about
# the source rather than about when someone typed make.
sed -i 's/^#define AUTOCONF_TIMESTAMP .*/#define AUTOCONF_TIMESTAMP "drac-Xterm"/' \
    "$SRC/include/autoconf.h"
note "banner: BusyBox v$BUSYBOX_VERSION (drac-Xterm)"

# --- build -------------------------------------------------------------------
# LDLIBS=m overrides BusyBox's default probe list, which includes resolv:
# bionic has no libresolv, the resolver lives in libc, and the probe treats the
# missing library as a hard failure rather than dropping it.
note "compiling"
make -C "$SRC" -j"$(nproc)" \
    CC="$CC" \
    HOSTCC="${HOSTCC:-cc}" \
    AR="$AR" \
    STRIP="$STRIP" \
    SKIP_STRIP=y \
    LDLIBS="m" \
    CONFIG_EXTRA_CFLAGS="$REPRO_CFLAGS" \
    > "$WORK_DIR/busybox-build.log" 2>&1 \
    || {
        grep -E 'error:|ld.lld: error' "$WORK_DIR/busybox-build.log" | sort -u | head -20 >&2
        die "busybox build failed; full log at $WORK_DIR/busybox-build.log"
    }

[ -f "$SRC/busybox" ] || die "no busybox binary was produced"
assert_arm64 "$SRC/busybox"

# --- checks ------------------------------------------------------------------
note "sanity checks"

hdr=$("$READELF" -h "$SRC/busybox")
contains "$hdr" "EXEC" || contains "$hdr" "DYN" || die "not an executable"

# A static binary must have no NEEDED entries. If it does, CONFIG_STATIC did not
# take and the binary will fail to start inside the PRoot rootfs, where the
# dynamic loader resolves against a different libc.
dyn=$("$READELF" --dynamic "$SRC/busybox" 2>/dev/null || true)
if contains "$dyn" "NEEDED"; then
    die "busybox is dynamically linked; CONFIG_STATIC did not take effect"
fi
printf '    statically linked, no NEEDED entries\n'

# The applet table is what makes this a multi-call binary. Read the count kconfig
# generated rather than grepping the binary: BusyBox packs applet names into one
# string table with no separators, so `strings | grep -x` finds none of them and
# reports every applet missing.
applet_count=$(sed -n 's/^#define NUM_APPLETS *\([0-9]*\).*/\1/p' "$SRC/include/NUM_APPLETS.h")
[ -n "$applet_count" ] || die "NUM_APPLETS.h was not generated"
[ "$applet_count" -gt 250 ] || die "only $applet_count applets built; the config cascade dropped too much"
printf '    %s applets\n' "$applet_count"

# The ones drac-Xterm actually depends on. A missing shell means the fallback
# terminal has nothing to run.
missing=""
table=$(cat "$SRC/include/applet_tables.h")
for want in sh ash ls cat cp mv rm mkdir mount tar wget chmod grep sed awk; do
    contains "$table" "\"$want\"" || missing="$missing $want"
done
[ -z "$missing" ] && printf '    required applets present\n' \
                  || die "required applets missing:$missing"

install_artifact "$SRC/busybox" libbusybox.so
