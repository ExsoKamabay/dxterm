#!/usr/bin/env python3
"""Check the release metadata and the repository for things a script can decide.

Store descriptions within their length limits, a changelog for the versionCode
being released in every locale, icon and screenshot dimensions, licence texts
present for every bundled binary, and no signing material anywhere in the tree.

The limits come from the F-Droid fastlane layout, which is the only place these
numbers are written down anywhere. The project is not submitted to F-Droid or to
IzzyOnDroid, but the rules are a reasonable bar and they catch real mistakes,
such as a release tagged with no changelog for it.

Only mechanical rules live here. Whether the app is useful, or whether a licence
is really honoured, is not something a green checkmark should ever claim.

Exit status: 0 when every hard rule passes, 1 otherwise. Warnings never fail the
run; they mark things a human still has to decide.

Image dimensions are parsed from the file headers with the standard library, so
this runs on a bare CI image with no Pillow and no ImageMagick.
"""

from __future__ import annotations

import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
META = ROOT / "fastlane" / "metadata" / "android"
BUILD_GRADLE = ROOT / "app" / "build.gradle.kts"

# F-Droid fastlane limits.
MAX_TITLE = 50
MAX_SHORT_DESC = 80
MAX_FULL_DESC = 4000
MAX_CHANGELOG = 500
ICON_MIN, ICON_MAX = 48, 512
SCREENSHOT_MIN_EDGE = 320
MAX_ASPECT = 2.0  # longer edge / shorter edge
APK_BUDGET_MB = 30  # what the online installer should stay under

failures: list[str] = []
warnings: list[str] = []


def fail(msg: str) -> None:
    failures.append(msg)
    print(f"  FAIL  {msg}")


def warn(msg: str) -> None:
    warnings.append(msg)
    print(f"  WARN  {msg}")


def ok(msg: str) -> None:
    print(f"  ok    {msg}")


def section(title: str) -> None:
    print(f"\n== {title}")


# --------------------------------------------------------------------------
# Image headers, standard library only
# --------------------------------------------------------------------------


def png_size(data: bytes) -> tuple[int, int] | None:
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        return None
    w, h = struct.unpack(">II", data[16:24])
    return w, h


def jpeg_size(data: bytes) -> tuple[int, int] | None:
    if data[:2] != b"\xff\xd8":
        return None
    i = 2
    n = len(data)
    while i < n - 9:
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        # Standalone markers carry no length field.
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7 or marker == 0x01:
            i += 2
            continue
        (seg_len,) = struct.unpack(">H", data[i + 2 : i + 4])
        # SOF0..SOF15, excluding the DHT/JPG/DAC markers that share the range.
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            h, w = struct.unpack(">HH", data[i + 5 : i + 9])
            return w, h
        i += 2 + seg_len
    return None


def image_size(path: Path) -> tuple[int, int] | None:
    data = path.read_bytes()
    return png_size(data) or jpeg_size(data)


# --------------------------------------------------------------------------
# Build file facts
# --------------------------------------------------------------------------


def gradle_value(pattern: str) -> str | None:
    text = BUILD_GRADLE.read_text(encoding="utf-8")
    m = re.search(pattern, text, re.MULTILINE)
    return m.group(1) if m else None


def main() -> int:
    print("Release metadata check")
    print(f"repository: {ROOT}")

    version_code = gradle_value(r"^\s*versionCode\s*=\s*(\d+)")
    version_name = gradle_value(r'^\s*versionName\s*=\s*"([^"]+)"')

    section("Build identity")
    if version_code and version_name:
        ok(f"versionCode={version_code}  versionName={version_name}")
    else:
        fail("could not read versionCode/versionName from app/build.gradle.kts")
        return 1

    application_id = gradle_value(r'^\s*applicationId\s*=\s*"([^"]+)"')
    if application_id:
        ok(f"applicationId={application_id}")
    else:
        fail("applicationId not found")

    # ----------------------------------------------------------------------
    section("Licensing")
    for name in ("LICENSE", "NOTICE"):
        p = ROOT / name
        if p.is_file() and p.stat().st_size > 0:
            ok(f"{name} present ({p.stat().st_size} bytes)")
        else:
            fail(f"{name} is missing at the repository root")

    lic_dir = ROOT / "licenses"
    for name in ("GPL-2.0.txt", "GPL-3.0.txt", "LGPL-3.0.txt", "BSD-3-Clause.txt"):
        p = lic_dir / name
        if p.is_file() and p.stat().st_size > 0:
            ok(f"licenses/{name} present")
        else:
            fail(f"licenses/{name} is missing. A bundled binary's licence text is not shipped")

    # ----------------------------------------------------------------------
    section("Installer payload")
    # One build, one source set. Which installer an APK is comes from the packaged assets:
    # exactly one archive in app/src/main/assets/rootfs makes it the offline installer, none
    # makes it the online one. There are no product flavours to drift apart any more.
    gradle_text = BUILD_GRADLE.read_text(encoding="utf-8")
    if "flavorDimensions" in gradle_text or "productFlavors" in gradle_text:
        fail("product flavours are back; the installer kind must come from assets/rootfs alone")
    else:
        ok("single build (no product flavours)")

    rootfs_dir = ROOT / "app/src/main/assets/rootfs"
    archive_re = re.compile(r"\.(tar\.xz|txz|tar\.gz|tgz|tar)$", re.IGNORECASE)
    if not rootfs_dir.is_dir():
        fail("app/src/main/assets/rootfs is missing; the build reads it to decide the installer")
    else:
        archives = sorted(p.name for p in rootfs_dir.iterdir()
                          if p.is_file() and archive_re.search(p.name))
        if len(archives) > 1:
            fail("more than one local rootfs: " + ", ".join(archives) +
                 " (only one offline image is allowed)")
        elif len(archives) == 1:
            ok(f"offline installer: bundling {archives[0]}")
        else:
            ok("online installer: no local rootfs, the catalogue is used")

    section("Download catalogue")
    catalog = ROOT / "app/src/main/assets/rootfsURLS.json"
    if not catalog.is_file():
        fail("app/src/main/assets/rootfsURLS.json is missing; the online installer has "
             "nothing to offer")
    else:
        import json as _json
        try:
            data = _json.loads(catalog.read_text(encoding="utf-8"))
        except Exception as exc:
            fail(f"rootfsURLS.json is not valid JSON: {exc}")
            data = {}
        total = 0
        for abi, images in data.items():
            if abi.startswith("_"):
                continue
            for label, e in (images or {}).items():
                total += 1
                url = e.get("url", "")
                sha = str(e.get("sha256", "")).lower()
                # Every entry must be verifiable. An unverified 200 MB download
                # unpacked into the sandbox is exactly what the SHA-256 gate exists
                # to prevent, and an entry without a digest silently opts out of it.
                if not url.startswith("https://"):
                    fail(f"{abi}/{label}: url is not https")
                if len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha):
                    fail(f"{abi}/{label}: sha256 is missing or malformed")
                if not isinstance(e.get("downloadBytes"), int) or e["downloadBytes"] <= 0:
                    fail(f"{abi}/{label}: downloadBytes is missing or not positive")
        if total:
            ok(f"{total} image(s) catalogued, all https with a full sha256")
        else:
            warn("the catalogue lists no images for any architecture")

    section("Payload size")
    rootfs_dir = ROOT / "app" / "src" / "main" / "assets" / "rootfs"
    strays = [p for p in rootfs_dir.glob("*") if p.name != "README.txt"] if rootfs_dir.is_dir() else []
    if strays:
        for p in strays:
            fail(
                f"{p.relative_to(ROOT)} ({p.stat().st_size / 1e6:.1f} MB) is inside the APK payload; "
                f"ADR-0001 says no root filesystem ships in the APK"
            )
    else:
        ok("assets/rootfs holds only README.txt: no bundled root filesystem")

    total = 0
    for p in (ROOT / "app" / "src" / "main").rglob("*"):
        if p.is_file():
            total += p.stat().st_size
    ok(f"app/src/main totals {total / 1e6:.1f} MB before packaging (budget ~{APK_BUDGET_MB} MB per APK)")
    if total > APK_BUDGET_MB * 1e6:
        fail(f"source payload alone already exceeds the ~{APK_BUDGET_MB} MB reserve")

    # ----------------------------------------------------------------------
    section("Fastlane metadata")
    if not META.is_dir():
        fail("fastlane/metadata/android does not exist")
        return 1

    locales = sorted(p.name for p in META.iterdir() if p.is_dir())
    if not locales:
        fail("no locale directories under fastlane/metadata/android")
        return 1
    if "en-US" not in locales:
        fail("en-US is required; it is the fallback for every other locale")
    ok(f"locales: {', '.join(locales)}")

    for locale in locales:
        d = META / locale
        print(f"\n  -- {locale}")

        checks = (
            ("title.txt", MAX_TITLE, True),
            ("short_description.txt", MAX_SHORT_DESC, True),
            ("full_description.txt", MAX_FULL_DESC, True),
        )
        for name, limit, required in checks:
            p = d / name
            if not p.is_file():
                (fail if required else warn)(f"{locale}/{name} is missing")
                continue
            text = p.read_text(encoding="utf-8")
            n = len(text.strip())
            if n == 0:
                fail(f"{locale}/{name} is empty")
            elif n > limit:
                fail(f"{locale}/{name} is {n} chars, limit is {limit}")
            else:
                ok(f"{locale}/{name}: {n}/{limit} chars")
            if "\r" in text:
                fail(f"{locale}/{name} contains CRLF line endings")

        # changelogs/<versionCode>.txt for the version being released
        cl = d / "changelogs" / f"{version_code}.txt"
        if not cl.is_file():
            fail(f"{locale}/changelogs/{version_code}.txt is missing: the current versionCode has no changelog")
        else:
            raw = cl.read_bytes()
            if len(raw) > MAX_CHANGELOG:
                fail(f"{locale}/changelogs/{version_code}.txt is {len(raw)} bytes, limit is {MAX_CHANGELOG}")
            else:
                ok(f"{locale}/changelogs/{version_code}.txt: {len(raw)}/{MAX_CHANGELOG} bytes")
            if b"\r" in raw:
                fail(f"{locale}/changelogs/{version_code}.txt contains CRLF line endings")

        # Stale changelogs for versionCodes that do not exist yet are a common mistake.
        cl_dir = d / "changelogs"
        if cl_dir.is_dir():
            for p in sorted(cl_dir.glob("*.txt")):
                if not p.stem.isdigit():
                    fail(f"{locale}/changelogs/{p.name} is not named <versionCode>.txt")
                elif int(p.stem) > int(version_code):
                    warn(f"{locale}/changelogs/{p.name} is ahead of versionCode {version_code}")

    # ----------------------------------------------------------------------
    section("Images")
    icons = list(META.glob("*/images/icon.png")) + list(META.glob("*/images/icon.jpg"))
    if not icons:
        fail("no images/icon.png (or .jpg) in any locale")
    for icon in icons:
        size = image_size(icon)
        rel = icon.relative_to(ROOT)
        if size is None:
            fail(f"{rel} is not a readable PNG/JPEG")
            continue
        w, h = size
        if w != h:
            warn(f"{rel} is {w}x{h}; icons are expected to be square")
        if not (ICON_MIN <= w <= ICON_MAX and ICON_MIN <= h <= ICON_MAX):
            fail(f"{rel} is {w}x{h}; must be between {ICON_MIN} and {ICON_MAX} px")
        else:
            ok(f"{rel}: {w}x{h}")

    shots = sorted(META.glob("*/images/phoneScreenshots/*"))
    if not shots:
        warn("no phone screenshots: not a blocker, but the listing will look empty")
    for shot in shots:
        rel = shot.relative_to(ROOT)
        if shot.suffix.lower() not in (".png", ".jpg", ".jpeg"):
            fail(f"{rel}: only PNG and JPEG are accepted")
            continue
        size = image_size(shot)
        if size is None:
            fail(f"{rel} is not a readable PNG/JPEG")
            continue
        w, h = size
        long_edge, short_edge = max(w, h), min(w, h)
        aspect = long_edge / short_edge
        if short_edge < SCREENSHOT_MIN_EDGE:
            fail(f"{rel} is {w}x{h}; the shorter edge must be at least {SCREENSHOT_MIN_EDGE} px")
        elif aspect > MAX_ASPECT:
            fail(f"{rel} is {w}x{h} (aspect {aspect:.2f}:1); the limit is {MAX_ASPECT:.0f}:1")
        else:
            ok(f"{rel}: {w}x{h} (aspect {aspect:.2f}:1)")

    # ----------------------------------------------------------------------
    section("Repository hygiene")
    secrets = []
    for pattern in ("*.keystore", "*.jks", "*.p12", "*.pem"):
        secrets += [p for p in ROOT.rglob(pattern) if ".gradle" not in p.parts and "build" not in p.parts]
    if secrets:
        for p in secrets:
            fail(f"signing material in the working tree: {p.relative_to(ROOT)}")
    else:
        ok("no keystore or private key in the working tree")

    gitignore = (ROOT / ".gitignore").read_text(encoding="utf-8") if (ROOT / ".gitignore").is_file() else ""
    for rule in ("*.keystore", "*.jks", "*.apk", "*.aab", "/apps/"):
        if rule in gitignore:
            ok(f".gitignore covers {rule}")
        else:
            fail(f".gitignore does not cover {rule}")

    # A rule in .gitignore does nothing for a file that was already added, so ask git
    # what it actually tracks rather than trusting the rule.
    try:
        tracked = subprocess.run(
            ["git", "ls-files", "-z"], cwd=ROOT,
            capture_output=True, text=True, check=True,
        ).stdout.split("\0")
    except (OSError, subprocess.CalledProcessError):
        warn("could not ask git which files are tracked; skipped the artifact check")
    else:
        staged = [f for f in tracked
                  if f.endswith((".apk", ".aab")) or f.startswith("apps/")]
        if staged:
            for f in staged:
                fail(f"build artifact tracked by git: {f}")
        else:
            ok("no APK, AAB or apps/ content is tracked by git")

    # ----------------------------------------------------------------------
    section("Result")
    print(f"  {len(failures)} failure(s), {len(warnings)} warning(s)")
    if failures:
        print("\nBlocking:")
        for f in failures:
            print(f"  - {f}")
    if warnings:
        print("\nFor a human to decide:")
        for w in warnings:
            print(f"  - {w}")
    if not failures:
        print("\nEvery mechanical rule passes. Nothing here judges whether the")
        print("app is any good, or whether a licence is really honoured.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
