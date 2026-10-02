// Browsing lewat Scrapling: `claw browse` dan tool web_fetch di `claw chat`.
// Request dijalankan oleh data/scripts/browse.py di venv Scrapling
// (scrapling/.venv), yang juga menolak alamat lokal/privat di setiap redirect.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "json.hpp"
#include "util.hpp"

namespace claw {

// Versi Scrapling dari PyPI yang dipasang `claw browse --setup`.
extern const char* const kScraplingSpec;

struct BrowseResult {
  bool ok = false;
  std::string error;
  nlohmann::json data = nlohmann::json::object();  // keluaran browse.py
};

// Validasi ringan sebelum menjalankan Python: http/https, tidak kosong, tidak
// diawali '-', maksimal 2048 karakter. Mengembalikan alasan penolakan atau "".
std::string precheck_url(const std::string& url);

// Host dari URL, untuk log yang tidak menyimpan path/query.
std::string url_host(const std::string& url);

BrowseResult web_fetch(const fs::path& project_root, const std::string& url, const std::string& selector,
                       int max_chars, bool allow_private);

// Interaksi web umum: GET/POST/PUT/DELETE/PATCH dengan body (form/json) dan header.
// Untuk mengikuti tautan, mengisi form, atau memanggil API, atas nama pemakaian sah
// milik pengguna. Penjaga alamat lokal/privat tetap berlaku.
BrowseResult web_request(const fs::path& project_root, const std::string& url, const std::string& method,
                         const std::string& body, const std::string& content_type,
                         const std::vector<std::pair<std::string, std::string>>& headers, int max_chars,
                         bool allow_private);

// Pasang Scrapling dari PyPI ke project_root/scrapling/.venv. assume_yes melewati
// pertanyaan konfirmasi. Mengembalikan exit code.
// Unduh isi URL ke berkas dest (mis. pustaka pendukung project). GET saja, penjaga
// alamat lokal/privat tetap berlaku. Hasil.data berisi {saved, path, bytes, status}.
BrowseResult web_download(const fs::path& project_root, const std::string& url, const std::string& dest,
                          bool allow_private);

int browse_setup(const fs::path& project_root, bool assume_yes);

}  // namespace claw
