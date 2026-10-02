#!/usr/bin/env python3
"""Cari model AI gratis tanpa login memakai Scrapling, untuk auto-repair claw.

Dipanggil oleh runner claw saat sebuah model kehabisan token / key-nya ditolak.
Script ini hanya MENEMUKAN kandidat; pengujian HTTP 200 dan penyimpanan ke
models.json dilakukan oleh sisi C++.

Sumber yang dibaca:
  1. Endpoint daftar model tiap provider di providers.json (gratis, tanpa login).
  2. Sumber tambahan buatan pengguna di data/repair-sources.json.
  3. Situs open source (GitHub, GitLab, Hugging Face, Codeberg) yang mendaftar
     API model gratis. Hasil dari sini hanya jadi catatan ("leads"), tidak
     dipakai otomatis, karena bentuk API-nya belum tentu OpenAI-compatible.

Keluaran (stdout, JSON):
  {
    "candidates": [ {"name","provider","upstream_id","api_key"}, ... ],
    "providers":  { "<nama>": {chat_url, models_url, api_key, models_format} },
    "leads":      [ {"source","url","note"}, ... ],
    "notes":      [ "..." ]
  }

Selalu keluar dengan kode 0 dan mencetak JSON yang valid, walau sebagian sumber
gagal, supaya runner tetap bisa memakai apa yang berhasil didapat.
"""

import argparse
import json
import re
import sys

NOTES = []


def log(msg):
    NOTES.append(msg)
    print(f"[repair-discover] {msg}", file=sys.stderr, flush=True)


# --- Pengambilan HTTP: pakai Scrapling kalau ada, kalau tidak urllib -----------

_fetch_impl = None


def _load_fetcher():
    """Kembalikan fungsi get(url, headers) -> (status, text). Utamakan Scrapling."""
    global _fetch_impl
    if _fetch_impl is not None:
        return _fetch_impl
    try:
        from scrapling.fetchers import Fetcher

        def _get(url, headers=None, timeout=25):
            r = Fetcher.get(url, headers=headers or {}, timeout=timeout, stealthy_headers=True)
            body = getattr(r, "body", None)
            if body is None:
                body = str(r)
            return int(getattr(r, "status", 0) or 0), body

        _fetch_impl = _get
        log("memakai Scrapling Fetcher")
    except Exception as exc:  # noqa: BLE001
        import urllib.request

        def _get(url, headers=None, timeout=25):
            req = urllib.request.Request(url, headers=headers or {"User-Agent": "claw-repair/1.0"})
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.status, resp.read().decode("utf-8", "replace")

        _fetch_impl = _get
        log(f"Scrapling tidak tersedia ({exc}); memakai urllib")
    return _fetch_impl


def fetch(url, headers=None, timeout=25):
    try:
        return _load_fetcher()(url, headers=headers, timeout=timeout)
    except Exception as exc:  # noqa: BLE001
        log(f"gagal ambil {url}: {exc}")
        return 0, ""


# --- Parsing daftar model (samakan dengan discover_models di scanner.cpp) -------


def extract_ids(text, include="", exclude="", match=None, max_models=0):
    try:
        data = json.loads(text)
    except Exception:  # noqa: BLE001
        return []
    if isinstance(data, list):
        items = data
    elif isinstance(data, dict) and isinstance(data.get("data"), list):
        items = data["data"]
    elif isinstance(data, dict) and isinstance(data.get("models"), list):
        items = data["models"]
    else:
        return []

    inc = re.compile(include, re.I) if include else None
    exc = re.compile(exclude, re.I) if exclude else None

    def ok(mid):
        if inc and not inc.search(mid):
            return False
        if exc and exc.search(mid):
            return False
        return True

    def matches(obj):
        if not match:
            return True
        for cond in match:
            if all(obj.get(k) == v for k, v in cond.items()):
                return True
        return False

    out, seen = [], set()
    for it in items:
        mid = ""
        if isinstance(it, str):
            mid = it
        elif isinstance(it, dict):
            if not matches(it):
                continue
            mid = it.get("id") or it.get("name") or ""
        if mid and mid not in seen and ok(mid):
            seen.add(mid)
            out.append(mid)
    if max_models and len(out) > max_models:
        out = out[:max_models]
    return out


def scan_source(name, cfg):
    """cfg: dict dgn models_url, api_key, include, exclude, match, max_models."""
    url = cfg.get("models_url")
    if not url:
        return []
    headers = {}
    key = cfg.get("api_key") or ""
    if key and key not in ("unused",):
        headers["Authorization"] = f"Bearer {key}"
    status, text = fetch(url, headers=headers)
    if status != 200:
        log(f"{name}: daftar model HTTP {status}")
        return []
    ids = extract_ids(
        text,
        include=cfg.get("include", ""),
        exclude=cfg.get("exclude", ""),
        match=cfg.get("match"),
        max_models=cfg.get("max_models", 0),
    )
    log(f"{name}: {len(ids)} model")
    return [
        {"name": f"{name}/{mid}", "provider": name, "upstream_id": mid, "api_key": key}
        for mid in ids
    ]


# --- Leads dari situs open source ----------------------------------------------

# API pencarian publik (tanpa login) untuk daftar "free LLM API".
LEAD_SEARCHES = [
    ("github", "https://api.github.com/search/repositories?q=free+llm+api+list&sort=stars&per_page=5"),
    ("codeberg", "https://codeberg.org/api/v1/repos/search?q=free+llm+api&limit=5"),
    ("huggingface", "https://huggingface.co/api/models?search=openai%20compatible&limit=5"),
    ("gitlab", "https://gitlab.com/api/v4/projects?search=free%20llm%20api&per_page=5"),
]


def collect_leads():
    leads = []
    for source, url in LEAD_SEARCHES:
        status, text = fetch(url, headers={"Accept": "application/json"})
        if status != 200:
            log(f"leads {source}: HTTP {status}")
            continue
        try:
            data = json.loads(text)
        except Exception:  # noqa: BLE001
            continue
        rows = data.get("items") if isinstance(data, dict) else data
        if isinstance(data, dict) and "data" in data:
            rows = data["data"]
        if not isinstance(rows, list):
            continue
        for row in rows[:5]:
            if not isinstance(row, dict):
                continue
            link = (
                row.get("html_url")
                or row.get("web_url")
                or (f"https://huggingface.co/{row.get('id')}" if row.get("id") else "")
                or row.get("url", "")
            )
            note = row.get("full_name") or row.get("path_with_namespace") or row.get("id") or ""
            if link:
                leads.append({"source": source, "url": link, "note": note})
        log(f"leads {source}: {len([l for l in leads if l['source'] == source])} tautan")
    return leads


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--providers", required=True, help="path providers.json")
    ap.add_argument("--sources", default="", help="path repair-sources.json (opsional)")
    ap.add_argument("--no-leads", action="store_true", help="lewati pencarian situs open source")
    ap.add_argument("--out", default="", help="tulis JSON ke file, bukan stdout")
    args = ap.parse_args()

    result = {"candidates": [], "providers": {}, "leads": [], "notes": NOTES}

    # 1. Provider yang sudah dikenal.
    try:
        proot = json.load(open(args.providers, encoding="utf-8"))
        provs = proot.get("providers", proot)
        for name, cfg in provs.items():
            if not isinstance(cfg, dict) or cfg.get("enabled") is False:
                continue
            if not cfg.get("models_url"):
                continue
            result["candidates"].extend(scan_source(name, cfg))
    except Exception as exc:  # noqa: BLE001
        log(f"providers.json gagal dibaca: {exc}")

    # 2. Sumber tambahan buatan pengguna.
    if args.sources:
        try:
            sroot = json.load(open(args.sources, encoding="utf-8"))
            extra = sroot.get("sources", sroot) if isinstance(sroot, dict) else sroot
            for cfg in extra or []:
                name = cfg.get("name")
                if not name or not cfg.get("chat_url"):
                    continue
                cands = scan_source(name, cfg)
                if cands:
                    result["providers"][name] = {
                        "chat_url": cfg["chat_url"],
                        "models_url": cfg.get("models_url", ""),
                        "api_key": cfg.get("api_key", ""),
                        "models_format": cfg.get("models_format", "openai"),
                    }
                    if cfg.get("match"):
                        result["providers"][name]["match"] = cfg["match"]
                    result["candidates"].extend(cands)
        except Exception as exc:  # noqa: BLE001
            log(f"repair-sources.json gagal dibaca: {exc}")

    # 3. Leads open source (informasi saja).
    if not args.no_leads:
        try:
            result["leads"] = collect_leads()
        except Exception as exc:  # noqa: BLE001
            log(f"pencarian leads gagal: {exc}")

    log(f"total {len(result['candidates'])} kandidat, {len(result['leads'])} leads")
    text = json.dumps(result, ensure_ascii=False, indent=2)
    if args.out:
        open(args.out, "w", encoding="utf-8").write(text)
    else:
        print(text)


if __name__ == "__main__":
    main()
