// Auto-repair: saat sebuah model kehabisan token atau key-nya ditolak, cari
// model AI gratis pengganti lewat Scrapling, uji sampai HTTP 200, lalu masukkan
// ke models.json (dan ke pool yang sedang berjalan, kalau ada). Percakapan tetap
// jalan karena router memakai model baru itu untuk pesan berikutnya.
#pragma once

#include <string>
#include <vector>

#include "catalog.hpp"
#include "scanner.hpp"

namespace claw {

class Router;  // forward

struct RepairResult {
  int discovered = 0;              // kandidat dari internet
  int tested = 0;                  // kandidat yang benar-benar diuji
  int added = 0;                   // model baru yang lolos dan disimpan
  std::vector<std::string> added_names;
  std::vector<std::string> new_providers;   // provider baru yang didaftarkan
  std::vector<std::string> leads;           // tautan situs open source (info)
  std::string error;               // diisi bila proses gagal total
};

struct RepairOptions {
  int max_test = 25;        // batas kandidat baru yang diuji per sesi repair
  bool probe_tools = false; // repair cukup uji chat biasa agar cepat
  bool leads = true;        // ikut cari daftar di GitHub/GitLab/HF/Codeberg
  bool quiet = false;       // true: tanpa cetak ke stdout (dipakai auto-repair)
};

// Lokasi python Scrapling (scrapling/.venv/bin/python) untuk project ini.
// Kosong bila belum di-setup. project_root = folder di atas data/.
fs::path scrapling_python(const fs::path& project_root);

// Lokasi python venv alat (tools/.venv/bin/python) yang berisi scapy + pyrit untuk skill
// `networking` dan `ai-red-team`. Kosong bila belum di-setup. project_root = folder di atas data/.
fs::path tools_python(const fs::path& project_root);

// Jalankan satu siklus repair. Aman dipanggil dari thread latar.
RepairResult run_repair(Catalog& catalog, const RepairOptions& opt, Router* live = nullptr);

}  // namespace claw
