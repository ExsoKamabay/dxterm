#include "browse.hpp"

#include <algorithm>
#include <iostream>
#include <utility>

#include "launcher.hpp"
#include "repair.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace claw {

using json = nlohmann::json;

const char* const kScraplingSpec = "scrapling[fetchers]==0.4.15";

std::string precheck_url(const std::string& url) {
  std::string u = trim(url);
  if (u.empty()) return "URL kosong";
  if (u.size() > 2048) return "URL terlalu panjang";
  if (u[0] == '-') return "URL tidak valid";
  auto p = u.find("://");
  if (p != std::string::npos) {
    std::string scheme = to_lower(u.substr(0, p));
    if (scheme != "http" && scheme != "https") return "hanya http/https yang diizinkan";
  }
  for (char c : u)
    if (static_cast<unsigned char>(c) < 0x20) return "URL berisi karakter kontrol";
  return "";
}

std::string url_host(const std::string& url) {
  std::string u = trim(url);
  auto p = u.find("://");
  if (p != std::string::npos) u = u.substr(p + 3);
  auto end = u.find_first_of("/?#");
  if (end != std::string::npos) u = u.substr(0, end);
  auto at = u.rfind('@');
  if (at != std::string::npos) u = u.substr(at + 1);
  return u;
}

namespace {

// Cari python Scrapling + browse.py, lalu jalankan argv (sesudah script) dan parse JSON keluaran.
BrowseResult run_browse(const fs::path& project_root, const std::string& url,
                        std::vector<std::string> extra) {
  BrowseResult r;
  fs::path py = scrapling_python(project_root);
  fs::path script = project_root / "data" / "scripts" / "browse.py";
  std::error_code ec;
  if (py.empty()) {
    r.error = "Scrapling belum siap (mungkin masih dipasang otomatis di latar belakang; "
              "pantau: tail -f /opt/claw/scrapling/setup.log). Untuk memasang sendiri: claw browse --setup";
    return r;
  }
  if (!fs::exists(script, ec)) {
    r.error = "browse.py tidak ditemukan di " + script.string();
    return r;
  }
  std::vector<std::string> argv = {py.string(), script.string()};
  argv.insert(argv.end(), extra.begin(), extra.end());
  argv.push_back("--");
  argv.push_back(trim(url));

  std::string out;
  int rc = run_capture(argv, 60, out);
  if (rc == 124) {
    r.error = "waktu habis (60 detik) saat menghubungi " + url_host(url);
    return r;
  }
  auto nl = out.find_last_of('\n', out.size() > 1 ? out.size() - 2 : 0);
  std::string last = nl == std::string::npos ? out : out.substr(nl + 1);
  json j = json::parse(trim(last), nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    r.error = "browse.py gagal (exit " + std::to_string(rc) + ")";
    return r;
  }
  r.data = j;
  r.ok = j.value("ok", false);
  if (!r.ok) {
    r.error = j.value("error", "");
    if (r.error.empty() && j.contains("status")) r.error = "HTTP " + std::to_string(j.value("status", 0));
  }
  return r;
}

}  // namespace

BrowseResult web_fetch(const fs::path& project_root, const std::string& url, const std::string& selector,
                       int max_chars, bool allow_private) {
  BrowseResult r;
  r.error = precheck_url(url);
  if (!r.error.empty()) return r;
  std::vector<std::string> extra = {"--max-chars", std::to_string(std::max(200, max_chars))};
  if (!trim(selector).empty()) {
    extra.push_back("--select");
    extra.push_back(trim(selector));
  }
  if (allow_private) extra.push_back("--allow-private");
  return run_browse(project_root, url, extra);
}

BrowseResult web_request(const fs::path& project_root, const std::string& url, const std::string& method,
                         const std::string& body, const std::string& content_type,
                         const std::vector<std::pair<std::string, std::string>>& headers, int max_chars,
                         bool allow_private) {
  BrowseResult r;
  r.error = precheck_url(url);
  if (!r.error.empty()) return r;
  std::string m = to_lower(trim(method));
  static const std::vector<std::string> kAllowed = {"get", "post", "put", "delete", "patch"};
  if (std::find(kAllowed.begin(), kAllowed.end(), m) == kAllowed.end()) {
    r.error = "metode tidak didukung: " + method;
    return r;
  }
  std::vector<std::string> extra = {"--max-chars", std::to_string(std::max(200, max_chars)),
                                    "--method", m};
  if (!body.empty()) {
    extra.push_back("--data");
    extra.push_back(body);
    extra.push_back("--data-type");
    extra.push_back(to_lower(content_type) == "json" ? "json" : "form");
  }
  for (const auto& [k, v] : headers) {
    if (trim(k).empty()) continue;
    extra.push_back("--header");
    extra.push_back(k + ": " + v);
  }
  if (allow_private) extra.push_back("--allow-private");
  return run_browse(project_root, url, extra);
}

BrowseResult web_download(const fs::path& project_root, const std::string& url, const std::string& dest,
                          bool allow_private) {
  BrowseResult r;
  r.error = precheck_url(url);
  if (!r.error.empty()) return r;
  if (trim(dest).empty()) {
    r.error = "tujuan unduhan kosong";
    return r;
  }
  std::vector<std::string> extra = {"--save", dest};
  if (allow_private) extra.push_back("--allow-private");
  return run_browse(project_root, url, extra);
}

int browse_setup(const fs::path& project_root, bool assume_yes) {
  fs::path venv = project_root / "scrapling" / ".venv";
  fs::path vpy = venv / "bin" / "python";
  std::error_code ec;

  std::string py3 = find_in_path("python3");
  if (py3.empty()) {
    std::cerr << "python3 belum ada di Linux ini. Pasang dulu:\n"
                 "  sudo apt install -y python3 python3-venv\n"
                 "lalu jalankan lagi: claw browse --setup\n";
    return 1;
  }
  if (!fs::exists(vpy, ec)) {
    std::string tmp;
    if (run_capture({py3, "-c", "import venv, ensurepip"}, 30, tmp) != 0) {
      std::cerr << "modul venv Python belum lengkap. Pasang dulu:\n"
                   "  sudo apt install -y python3-venv\n"
                   "lalu jalankan lagi: claw browse --setup\n";
      return 1;
    }
  }

  std::cout << "Memasang " << kScraplingSpec << " dari PyPI ke " << venv.string() << "\n"
            << "Unduhan dan ruang yang dipakai sekitar 350 MB. Browser tidak diunduh;\n"
            << "claw hanya memakai fetcher HTTP Scrapling.\n";
  if (!assume_yes) {
#ifndef _WIN32
    if (!isatty(STDIN_FILENO)) {
      std::cerr << "butuh konfirmasi; jalankan ulang dengan --yes\n";
      return 1;
    }
#endif
    std::cout << "Lanjutkan? [y/N] " << std::flush;
    std::string ans;
    if (!std::getline(std::cin, ans) || (to_lower(trim(ans)) != "y" && to_lower(trim(ans)) != "ya")) {
      std::cout << "dibatalkan, tidak ada yang dipasang\n";
      return 1;
    }
  }

  if (!fs::exists(vpy, ec)) {
    std::cout << "\n[1/3] membuat virtualenv...\n";
    int rc = run_command({py3, "-m", "venv", venv.string()});
    if (rc != 0) {
      std::cerr << "gagal membuat virtualenv (exit " << rc << ")\n";
      return 1;
    }
  } else {
    std::cout << "\n[1/3] virtualenv sudah ada, dipakai ulang\n";
  }
  std::cout << "[2/3] pip install " << kScraplingSpec << "...\n";
  int rc = run_command({vpy.string(), "-m", "pip", "install", "--disable-pip-version-check", kScraplingSpec});
  if (rc != 0) {
    std::cerr << "pip install gagal (exit " << rc << "). Periksa koneksi lalu ulangi claw browse --setup\n";
    return 1;
  }
  std::cout << "[3/3] memeriksa instalasi...\n";
  std::string out;
  rc = run_capture({vpy.string(), "-c", "from scrapling.fetchers import Fetcher; print('ok')"}, 120, out);
  if (rc != 0 || trim(out) != "ok") {
    std::cerr << "Scrapling terpasang tapi tidak bisa diimpor (exit " << rc << ")\n";
    return 1;
  }
  std::cout << "Scrapling siap. Coba: claw browse https://example.com\n";
  return 0;
}

}  // namespace claw
