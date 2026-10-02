#include "util.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#else
#include <unistd.h>
#include <climits>
#endif

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#endif

namespace claw {

double now_seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int64_t unix_time() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string timestamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream os;
  os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return os.str();
}

std::string get_env(const std::string& name) {
#ifdef _WIN32
  char* buf = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, name.c_str()) == 0 && buf) {
    std::string v(buf);
    free(buf);
    return v;
  }
  return "";
#else
  const char* v = std::getenv(name.c_str());
  return v ? std::string(v) : std::string();
#endif
}

void set_env(const std::string& name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name.c_str(), value.c_str());
#else
  setenv(name.c_str(), value.c_str(), 1);
#endif
}

fs::path home_dir() {
#ifdef _WIN32
  std::string h = get_env("USERPROFILE");
  if (h.empty()) h = get_env("HOMEDRIVE") + get_env("HOMEPATH");
#else
  std::string h = get_env("HOME");
#endif
  return h.empty() ? fs::current_path() : fs::path(h);
}

fs::path executable_path() {
#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return fs::path(std::string(buf, n));
#elif defined(__APPLE__)
  char buf[PATH_MAX];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0) return fs::weakly_canonical(fs::path(buf));
#else
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) return fs::path(std::string(buf, static_cast<size_t>(n)));
#endif
  return fs::current_path() / "claw";
}

fs::path resolve_data_dir(const std::string& override_dir) {
  if (!override_dir.empty()) return fs::absolute(override_dir);
  std::string env = get_env("CLAW_DATA_DIR");
  if (!env.empty()) return fs::absolute(env);
  // Cari folder data/ yang berisi providers.json, mulai dari lokasi executable
  // (misalnya build/claw -> data), lalu dari direktori kerja.
  std::vector<fs::path> roots;
  fs::path exe_dir = executable_path().parent_path();
  roots.push_back(exe_dir);
  roots.push_back(exe_dir.parent_path());
  roots.push_back(exe_dir.parent_path().parent_path());
  roots.push_back(fs::current_path());
  for (const auto& r : roots) {
    std::error_code ec;
    if (fs::exists(r / "data" / "providers.json", ec)) return fs::absolute(r / "data");
  }
  return fs::absolute(exe_dir.parent_path() / "data");
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw std::runtime_error("tidak bisa membaca " + p.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_file_atomic(const fs::path& p, const std::string& content) {
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  fs::path tmp = p;
  tmp += ".tmp-" + random_hex(4);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("tidak bisa menulis " + tmp.string());
    out << content;
    out.flush();
    if (!out) throw std::runtime_error("gagal menulis " + tmp.string());
  }
  fs::rename(tmp, p, ec);
  if (ec) {
    // Windows menolak rename ke file yang sudah ada pada beberapa kondisi.
    fs::remove(p, ec);
    fs::rename(tmp, p, ec);
    if (ec) {
      fs::remove(tmp, ec);
      throw std::runtime_error("gagal mengganti " + p.string());
    }
  }
}

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
  return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string random_hex(size_t bytes) {
  static thread_local std::mt19937_64 rng{std::random_device{}() ^
                                          static_cast<uint64_t>(now_seconds() * 1e6)};
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; ++i) {
    auto b = static_cast<unsigned>(rng() & 0xff);
    out.push_back(hex[b >> 4]);
    out.push_back(hex[b & 0xf]);
  }
  return out;
}

std::string shell_quote(const std::string& arg) {
#ifdef _WIN32
  // Aturan quoting CommandLineToArgvW + cmd.exe.
  std::string out = "\"";
  size_t backslashes = 0;
  for (char c : arg) {
    if (c == '\\') {
      ++backslashes;
      continue;
    }
    if (c == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      if (c == '%') out.push_back('^');
      out.push_back(c);
    }
    backslashes = 0;
  }
  out.append(backslashes * 2, '\\');
  out.push_back('"');
  return out;
#else
  std::string out = "'";
  for (char c : arg) {
    if (c == '\'')
      out += "'\\''";
    else
      out.push_back(c);
  }
  out.push_back('\'');
  return out;
#endif
}

namespace {
std::mutex g_log_mu;
std::ofstream g_log_file;
bool g_console = true;
bool g_verbose = false;
}  // namespace

void Log::set_file(const fs::path& p) {
  std::lock_guard<std::mutex> lk(g_log_mu);
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  if (g_log_file.is_open()) g_log_file.close();
  g_log_file.open(p, std::ios::app);
}

void Log::set_console(bool enabled) {
  std::lock_guard<std::mutex> lk(g_log_mu);
  g_console = enabled;
}

void Log::set_verbose(bool v) { g_verbose = v; }
bool Log::verbose() { return g_verbose; }

void Log::note(const std::string& msg) {
  std::lock_guard<std::mutex> lk(g_log_mu);
  if (g_log_file.is_open()) {
    g_log_file << "[" << timestamp() << "] NOTE  " << msg << "\n";
    g_log_file.flush();
  }
}

void Log::write(const char* level, const std::string& msg) {
  std::lock_guard<std::mutex> lk(g_log_mu);
  std::string line = "[" + timestamp() + "] " + level + " " + msg;
  if (g_log_file.is_open()) {
    g_log_file << line << "\n";
    g_log_file.flush();
  }
  if (g_console) std::cerr << line << std::endl;
}

void Log::info(const std::string& msg) { write("INFO ", msg); }
void Log::warn(const std::string& msg) { write("WARN ", msg); }
void Log::error(const std::string& msg) { write("ERROR", msg); }
void Log::debug(const std::string& msg) {
  if (g_verbose) write("DEBUG", msg);
}

int run_capture(const std::vector<std::string>& argv, int timeout_sec, std::string& out,
                const fs::path& stderr_file, size_t max_bytes) {
  out.clear();
  if (argv.empty()) return 127;
#ifdef _WIN32
  (void)timeout_sec;
  (void)stderr_file;
  (void)max_bytes;
  return 127;
#else
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC) != 0) return 127;
  std::vector<char*> cargv;
  for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);
  std::string errpath = stderr_file.empty() ? std::string("/dev/null") : stderr_file.string();

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return 127;
  }
  if (pid == 0) {
    // Grup proses sendiri, supaya timeout ikut mematikan anak-anaknya.
    setpgid(0, 0);
    dup2(pipefd[1], STDOUT_FILENO);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) dup2(devnull, STDIN_FILENO);
    int errfd = open(errpath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (errfd >= 0) dup2(errfd, STDERR_FILENO);
    execvp(cargv[0], cargv.data());
    _exit(127);
  }
  close(pipefd[1]);
  double deadline = now_seconds() + std::max(1, timeout_sec);
  bool timed_out = false;
  char buf[8192];
  while (true) {
    double left = deadline - now_seconds();
    if (left <= 0) {
      timed_out = true;
      break;
    }
    pollfd pfd{pipefd[0], POLLIN, 0};
    int pr = poll(&pfd, 1, static_cast<int>(std::min(left, 1.0) * 1000) + 1);
    if (pr < 0 && errno != EINTR) break;
    if (pr <= 0) continue;
    ssize_t n = read(pipefd[0], buf, sizeof(buf));
    if (n <= 0) break;
    if (out.size() < max_bytes) out.append(buf, std::min(static_cast<size_t>(n), max_bytes - out.size()));
  }
  close(pipefd[0]);
  if (timed_out) kill(-pid, SIGKILL);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (timed_out) return 124;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 1;
#endif
}

}  // namespace claw

