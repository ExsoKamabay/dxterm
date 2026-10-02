#!/usr/bin/env python3
"""Ambil satu halaman web dengan Scrapling untuk claw (tool web_fetch dan `claw browse`).

Pemakaian:
  browse.py URL [--select CSS] [--max-chars N] [--allow-private] [--timeout DETIK]

Keluaran selalu satu objek JSON di stdout:
  {"ok": true, "url", "final_url", "status", "title", "content_type", "text",
   "truncated", "links": [{"text", "url"}]}
  {"ok": false, "error": "..."}

Pengamanan:
  * Hanya http dan https.
  * Alamat yang tidak publik (localhost, 10.x, 192.168.x, 172.16-31.x, 100.64.x,
    link-local, IPv6 lokal) ditolak kecuali --allow-private. Pemeriksaan diulang
    untuk setiap redirect, jadi situs publik tidak bisa membelokkan request ke
    router atau layanan di perangkat.
  * Redirect paling banyak 5 kali, ukuran teks dibatasi.
"""

import argparse
import ipaddress
import json
import socket
import sys
from urllib.parse import urljoin, urlsplit

MAX_REDIRECTS = 5
HARD_MAX_CHARS = 20000
MAX_LINKS = 25
REDIRECT_CODES = {301, 302, 303, 307, 308}


def emit(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False))
    sys.stdout.write("\n")
    sys.stdout.flush()


def blocked_reason(url, allow_private):
    """Alasan URL ditolak, atau None bila boleh dibuka."""
    parts = urlsplit(url)
    if parts.scheme not in ("http", "https"):
        return f"hanya http/https yang diizinkan (dapat: {parts.scheme or 'kosong'})"
    host = parts.hostname
    if not host:
        return "URL tanpa nama host"
    if allow_private:
        return None
    try:
        infos = socket.getaddrinfo(host, parts.port or (443 if parts.scheme == "https" else 80))
    except socket.gaierror as e:
        return f"nama host tidak ditemukan: {host} ({e})"
    for info in infos:
        ip = ipaddress.ip_address(info[4][0].split("%")[0])
        if getattr(ip, "ipv4_mapped", None):
            ip = ip.ipv4_mapped
        if not ip.is_global or ip.is_multicast:
            return f"alamat lokal/privat diblokir: {host} -> {ip} (izinkan lewat xset > claw)"
    return None


def header(headers, name):
    for k, v in (headers or {}).items():
        if k.lower() == name:
            return v
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url")
    ap.add_argument("--select", default="")
    ap.add_argument("--max-chars", type=int, default=6000)
    ap.add_argument("--allow-private", action="store_true")
    ap.add_argument("--timeout", type=int, default=25)
    ap.add_argument("--method", default="GET")
    ap.add_argument("--data", default="")
    ap.add_argument("--data-type", default="form", choices=["form", "json"])
    ap.add_argument("--header", action="append", default=[])
    ap.add_argument("--save", default="")
    a = ap.parse_args()
    max_chars = max(200, min(a.max_chars, HARD_MAX_CHARS))
    method = a.method.upper()

    req_headers = {}
    for h in a.header:
        if ":" in h:
            k, v = h.split(":", 1)
            req_headers[k.strip()] = v.strip()

    body_kwargs = {}
    if a.data:
        if a.data_type == "json":
            try:
                body_kwargs["json"] = json.loads(a.data)
            except Exception as e:  # noqa: BLE001
                emit({"ok": False, "error": f"--data bukan JSON valid: {e}"})
                return
        else:
            # form: "k=v&k2=v2" atau objek JSON.
            form = {}
            if a.data.lstrip().startswith("{"):
                try:
                    form = json.loads(a.data)
                except Exception:  # noqa: BLE001
                    form = {}
            if not form:
                for pair in a.data.split("&"):
                    if "=" in pair:
                        k, v = pair.split("=", 1)
                        form[k] = v
            body_kwargs["data"] = form

    try:
        from scrapling.fetchers import Fetcher
    except Exception as e:  # noqa: BLE001
        emit({"ok": False, "error": f"Scrapling belum terpasang ({e}). Jalankan: claw browse --setup"})
        return

    url = a.url.strip()
    if "://" not in url:
        url = "https://" + url
    resp = None
    for _ in range(MAX_REDIRECTS + 1):
        why = blocked_reason(url, a.allow_private)
        if why:
            emit({"ok": False, "error": why, "url": url})
            return
        try:
            if method == "GET":
                resp = Fetcher.get(url, timeout=a.timeout, follow_redirects=False,
                                   stealthy_headers=True, headers=req_headers or None)
            else:
                fn = getattr(Fetcher, method.lower(), None)
                if fn is None:
                    emit({"ok": False, "error": f"metode tidak didukung: {method}"})
                    return
                resp = fn(url, timeout=a.timeout, follow_redirects=False, stealthy_headers=True,
                          headers=req_headers or None, **body_kwargs)
        except Exception as e:  # noqa: BLE001
            emit({"ok": False, "error": f"request gagal: {e}", "url": url})
            return
        loc = header(resp.headers, "location")
        # Ikuti redirect hanya untuk GET; untuk POST/PUT/DELETE kembalikan respons apa adanya
        # (biar model yang memutuskan langkah berikutnya, dan cek SSRF tetap per-URL).
        if method == "GET" and resp.status in REDIRECT_CODES and loc:
            url = urljoin(url, loc)
            continue
        break
    else:
        emit({"ok": False, "error": f"terlalu banyak redirect (>{MAX_REDIRECTS})", "url": url})
        return

    # Simpan isi respons ke berkas (unduhan pustaka/aset pendukung project).
    if a.save:
        try:
            import os
            body = resp.body if isinstance(resp.body, (bytes, bytearray)) else str(resp.body).encode()
            d = os.path.dirname(a.save)
            if d:
                os.makedirs(d, exist_ok=True)
            with open(a.save, "wb") as f:
                f.write(body)
            emit({"ok": 200 <= resp.status < 400, "url": a.url, "final_url": url,
                  "status": resp.status, "saved": True, "path": a.save, "bytes": len(body)})
        except Exception as e:  # noqa: BLE001
            emit({"ok": False, "error": f"gagal menyimpan: {e}", "url": url})
        return

    ctype = (header(resp.headers, "content-type") or "").lower()
    title = ""
    links = []
    if "html" in ctype or not ctype:
        try:
            title = (resp.css("title::text").get() or "").strip()
        except Exception:  # noqa: BLE001
            title = ""
        if a.select:
            try:
                nodes = resp.css(a.select)
            except Exception as e:  # noqa: BLE001
                emit({"ok": False, "error": f"selector CSS tidak valid: {e}", "url": url})
                return
            text = "\n\n".join(str(n.get_all_text(separator=" ", strip=True)) for n in nodes)
            if not text.strip():
                text = f"(selector '{a.select}' tidak menemukan apa pun)"
        else:
            text = str(resp.get_all_text(separator="\n", strip=True,
                                          ignore_tags=("script", "style", "noscript", "svg")))
        seen = set()
        for el in resp.css("a[href]"):
            href = urljoin(url, el.attrib.get("href", ""))
            if not href.startswith(("http://", "https://")) or href in seen:
                continue
            seen.add(href)
            label = str(el.get_all_text(separator=" ", strip=True))[:80]
            links.append({"text": label, "url": href})
            if len(links) >= MAX_LINKS:
                break
    else:
        body = resp.body if isinstance(resp.body, (bytes, bytearray)) else str(resp.body).encode()
        text = body.decode(resp.encoding or "utf-8", errors="replace")

    truncated = len(text) > max_chars
    # Header respons yang berguna untuk alur interaksi (mis. set-cookie, location).
    keep = ("set-cookie", "location", "content-type", "www-authenticate", "retry-after")
    resp_headers = {}
    for k, v in (resp.headers or {}).items():
        if k.lower() in keep:
            resp_headers[k.lower()] = v
    emit({
        "ok": 200 <= resp.status < 400,
        "url": a.url,
        "final_url": url,
        "status": resp.status,
        "method": method,
        "title": title,
        "content_type": ctype,
        "text": text[:max_chars],
        "truncated": truncated,
        "links": links,
        "headers": resp_headers,
    })


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa: BLE001
        emit({"ok": False, "error": f"browse.py error: {e}"})
