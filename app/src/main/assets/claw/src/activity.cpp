#include "activity.hpp"

#include <cstdint>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace claw {

namespace {

// "2026-09-26 01-33-07": aman dipakai sebagai nama berkas (tanpa ':').
std::string stamp_for_name() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream os;
  os << std::put_time(&tm, "%Y-%m-%d %H-%M-%S");
  return os.str();
}

std::string jam() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream os;
  os << std::put_time(&tm, "%H:%M:%S");
  return os.str();
}

std::string indent(const std::string& s) {
  if (s.empty()) return s;
  std::string out;
  std::istringstream in(s);
  std::string line;
  while (std::getline(in, line)) out += "    " + line + "\n";
  return out;
}

}  // namespace

Activity::Activity(bool detailed, bool live, const fs::path& home, const std::string& command)
    : detailed_(detailed), live_(live), home_(home), command_(command) {
  if (detailed_) path_ = home_ / ("activity(" + stamp_for_name() + ").txt");
  if (live_) {
    // Mulai umpan live bersih: buang aktivitas sesi lama agar panel graph hanya menampilkan
    // tugas saat ini. Tulis-atomik (rename), jadi monitor yang sedang membaca tak melihat
    // berkas setengah jadi; cmd_watch juga tahan file menyusut (reset prev_lines).
    clear_live();
    this->live("== sesi claw " + command_ + " dimulai ==");
  }
}

void Activity::clear_live() {
  try {
    fs::path lp = live_path(home_);
    std::error_code ec;
    fs::create_directories(lp.parent_path(), ec);
    write_file_atomic(lp, std::string());
  } catch (...) {
  }
}

fs::path Activity::live_path(const fs::path& home) { return home / ".claw" / "live.log"; }

void Activity::set_detailed(bool on) {
  detailed_ = on;
  if (on && path_.empty()) path_ = home_ / ("activity(" + stamp_for_name() + ").txt");
}

void Activity::live(const std::string& desc) {
  if (!live_) return;
  std::lock_guard<std::mutex> lk(*mu_);
  try {
    fs::path lp = live_path(home_);
    std::error_code ec;
    fs::create_directories(lp.parent_path(), ec);
    {
      std::ofstream f(lp, std::ios::app);
      if (f) f << "> " << desc << " (" << timestamp() << ")\n";
    }
    rotate_live(lp);
  } catch (...) {
  }
}

void Activity::rotate_live(const fs::path& lp) {
  // a-work never-ending terus meng-append live.log; tanpa plafon, berkas membengkak (MB→GB)
  // dan `claw watch` membacanya ulang ~11x/detik. Saat melewati ~512 KiB, pangkas jadi ~1500
  // baris terakhir lewat tulis-atomik (rename). cmd_watch sudah tahan file menyusut: ia me-reset
  // prev_lines saat jumlah baris berkurang.
  constexpr std::uintmax_t kMaxBytes = 512 * 1024;
  constexpr std::size_t kKeepLines = 1500;
  std::error_code ec;
  std::uintmax_t sz = fs::file_size(lp, ec);
  if (ec || sz <= kMaxBytes) return;
  std::ifstream in(lp);
  if (!in) return;
  std::deque<std::string> tail;
  std::string line;
  while (std::getline(in, line)) {
    tail.push_back(line);
    if (tail.size() > kKeepLines) tail.pop_front();
  }
  in.close();
  std::string out;
  for (const auto& l : tail) {
    out += l;
    out += '\n';
  }
  write_file_atomic(lp, out);
}

void Activity::ensure_header() {
  if (header_done_) return;
  header_done_ = true;
  std::error_code ec;
  fs::create_directories(home_, ec);
  std::ostringstream h;
  h << "Catatan aktivitas claw\n"
    << "Perintah   : claw " << command_ << "\n"
    << "Mulai      : " << timestamp() << "\n"
    << "Berkas ini menjelaskan setiap langkah yang claw kerjakan pada sesi ini.\n"
    << "====================================================================\n\n";
  std::ofstream f(path_, std::ios::app);
  if (f) f << h.str();
}

void Activity::write(const std::string& block) {
  if (!detailed_) return;
  std::lock_guard<std::mutex> lk(*mu_);
  try {
    ensure_header();
    std::ofstream f(path_, std::ios::app);
    if (f) f << block;
  } catch (...) {
    // Catatan aktivitas tidak boleh menghentikan claw.
  }
}

void Activity::step(const std::string& judul, const std::string& rincian) {
  std::ostringstream b;
  b << "[" << jam() << "] " << ++n_ << ". " << judul << "\n";
  if (!rincian.empty()) b << indent(rincian);
  b << "\n";
  write(b.str());
}

void Activity::tool(const std::string& nama, const std::string& argumen, const std::string& hasil) {
  std::ostringstream d;
  d << "tool   : " << nama << "\n";
  if (!argumen.empty()) d << "minta  : " << argumen << "\n";
  if (!hasil.empty()) d << "hasil  : " << hasil;
  step("Model memakai tool " + nama, d.str());
}

void Activity::turn(const std::string& permintaan, const std::string& model) {
  std::ostringstream d;
  d << "permintaan pengguna: " << permintaan << "\n";
  d << "dijawab oleh model : " << model;
  step("Giliran percakapan", d.str());
}

}  // namespace claw
