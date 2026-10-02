// Utilitas umum: path, env, waktu, log, dan helper lintas platform.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace claw {

namespace fs = std::filesystem;

// Waktu monotonic dalam detik (untuk cooldown) dan waktu dinding (untuk log).
double now_seconds();
int64_t unix_time();
std::string timestamp();

// Env var lintas platform.
std::string get_env(const std::string& name);
void set_env(const std::string& name, const std::string& value);

fs::path home_dir();
fs::path executable_path();

// Direktori data runner (berisi providers.json, models.json, state/).
fs::path resolve_data_dir(const std::string& override_dir);

std::string read_file(const fs::path& p);
// Tulis atomik: tulis ke file sementara lalu rename.
void write_file_atomic(const fs::path& p, const std::string& content);

std::string to_lower(std::string s);
bool contains_ci(const std::string& haystack, const std::string& needle);
std::string trim(const std::string& s);
std::string random_hex(size_t bytes);
std::string shell_quote(const std::string& arg);

// Jalankan argv langsung (tanpa shell), tangkap stdout sampai max_bytes. stderr
// ditulis ke stderr_file (kosong = dibuang). Proses dibunuh bila lewat
// timeout_sec. Mengembalikan exit code, 124 bila timeout, 127 bila gagal exec.
int run_capture(const std::vector<std::string>& argv, int timeout_sec, std::string& out,
                const fs::path& stderr_file = {}, size_t max_bytes = 1 << 20);

// Logger sederhana: ke stderr dan/atau file. Aman dipakai banyak thread.
class Log {
 public:
  static void set_file(const fs::path& p);
  static void set_console(bool enabled);
  static void set_verbose(bool v);
  static bool verbose();
  static void info(const std::string& msg);
  static void warn(const std::string& msg);
  static void error(const std::string& msg);
  static void debug(const std::string& msg);
  // Hanya ke file log, tidak ke layar. Untuk jejak tugas latar belakang yang
  // tidak boleh mengganggu tampilan chat.
  static void note(const std::string& msg);

 private:
  static void write(const char* level, const std::string& msg);
};

}  // namespace claw
