#include "lineedit.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace claw {

#ifndef _WIN32

namespace {

// Lebar tampilan satu titik-kode (perkiraan wcwidth): 0 untuk combining/zero-width,
// 2 untuk CJK/emoji lebar-ganda, selain itu 1. Dibutuhkan agar posisi kursor dan pembungkusan
// baris cocok dengan cara emulator DracXterm menaruh sel (yang juga memakai lebar sebenarnya).
int cp_width(uint32_t cp) {
  if (cp == 0) return 0;
  if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1160 && cp <= 0x11FF) ||
      (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
      (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x20D0 && cp <= 0x20FF) ||
      (cp >= 0xFE20 && cp <= 0xFE2F) || cp == 0xFEFF)
    return 0;  // combining / zero-width
  if ((cp >= 0x1100 && cp <= 0x115F) || cp == 0x2329 || cp == 0x232A ||
      (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
      (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
      (cp >= 0xA000 && cp <= 0xA4CF) || (cp >= 0xA960 && cp <= 0xA97F) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFE10 && cp <= 0xFE19) || (cp >= 0xFE30 && cp <= 0xFE6F) ||
      (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) ||
      (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x20000 && cp <= 0x3FFFD))
    return 2;  // East-Asian wide / fullwidth / emoji
  return 1;
}

// Jumlah kolom tampilan. Escape ANSI (CSI: ESC [ ... huruf akhir) tidak memakan kolom, jadi
// prompt boleh berwarna tanpa merusak perhitungan; titik-kode dihitung dengan lebar sebenarnya.
size_t columns(const std::string& s, size_t upto) {
  size_t n = 0;
  for (size_t i = 0; i < upto && i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == 0x1b && i + 1 < s.size() && s[i + 1] == '[') {  // ESC [ ... final (@..~)
      i += 2;
      while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
      if (i < s.size()) ++i;  // lewati byte akhir
      continue;
    }
    int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    uint32_t cp;
    if (len == 1) {
      cp = c;
    } else {
      cp = c & (0xFF >> (len + 1));
      for (int k = 1; k < len && i + k < s.size(); ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    i += len;
    n += static_cast<size_t>(cp_width(cp));
  }
  return n;
}

size_t utf8_prev(const std::string& s, size_t pos) {
  if (pos == 0) return 0;
  --pos;
  while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) --pos;
  return pos;
}
size_t utf8_next(const std::string& s, size_t pos) {
  if (pos >= s.size()) return s.size();
  unsigned char c = static_cast<unsigned char>(s[pos]);
  pos += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
  return pos > s.size() ? s.size() : pos;
}

int term_cols() {
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
  return 80;
}

struct RawMode {
  termios saved{};
  bool ok = false;
  RawMode() {
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return;
    termios raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    ok = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
  }
  ~RawMode() {
    if (ok) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
  }
};

bool read_byte(char& c) {
  ssize_t n;
  do {
    n = ::read(STDIN_FILENO, &c, 1);
  } while (n < 0 && errno == EINTR);
  return n == 1;
}
bool read_byte_timeout(char& c, int ms) {
  pollfd p{STDIN_FILENO, POLLIN, 0};
  int r = poll(&p, 1, ms);
  if (r <= 0) return false;
  return read_byte(c);
}

// State perender multi-baris (menyerupai linenoise), aman untuk input yang membungkus.
struct Editor {
  std::string plast;   // baris terakhir prompt (mis. "kamu> ")
  std::string incolor; // SGR untuk mewarnai teks input (kosong = tanpa warna)
  int oldrows = 1;     // jumlah baris yang dipakai render terakhir
  int oldcol = 0;      // kolom kursor (dalam kolom) pada render terakhir
  int cur_input_row = 0;  // baris kursor (0-indeks) di dalam area input, untuk jam header

  void refresh(const std::string& buf, size_t cur) {
    int cols = term_cols();
    int plen = static_cast<int>(columns(plast, plast.size()));
    int len = static_cast<int>(columns(buf, buf.size()));
    int colpos = static_cast<int>(columns(buf, cur));
    int rows = (plen + len + cols - 1) / cols;
    if (rows < 1) rows = 1;
    int rpos = (plen + oldcol + cols) / cols;  // baris kursor pada layout LAMA
    int old = oldrows;
    oldrows = rows;

    std::string ab;
    // Turun ke baris terakhir layout lama, lalu bersihkan ke atas.
    if (old - rpos > 0) ab += "\033[" + std::to_string(old - rpos) + "B";
    for (int j = 0; j < old - 1; ++j) ab += "\r\033[0K\033[1A";
    ab += "\r\033[0K";
    ab += plast;
    if (!incolor.empty()) ab += incolor;
    ab += buf;
    if (!incolor.empty()) ab += "\033[0m";
    // Kursor di ujung tepat di batas kanan: pindah baris agar kursor tidak nyangkut.
    if (cur == buf.size() && (plen + len) % cols == 0 && (plen + len) > 0) {
      ab += "\r\n";
      ++rows;
      if (rows > oldrows) oldrows = rows;
    }
    int rpos2 = (plen + colpos + cols) / cols;  // baris kursor sekarang
    if (rows - rpos2 > 0) ab += "\033[" + std::to_string(rows - rpos2) + "A";
    int col = (plen + colpos) % cols;
    ab += "\r";
    if (col) ab += "\033[" + std::to_string(col) + "C";
    oldcol = colpos;
    cur_input_row = (plen + colpos) / cols;  // baris kursor dalam area input (untuk jam header)
    ::fwrite(ab.data(), 1, ab.size(), stdout);
    ::fflush(stdout);
  }
};

}  // namespace

LineStatus read_line(const std::string& prompt, std::vector<std::string>& history, std::string& line,
                     const std::string& input_color, const LiveClock* clock) {
  if (!isatty(STDIN_FILENO)) {
    std::cout << prompt << std::flush;
    if (!std::getline(std::cin, line)) return LineStatus::Eof;
    return LineStatus::Ok;
  }
  RawMode raw;
  if (!raw.ok) {
    std::cout << prompt << std::flush;
    if (!std::getline(std::cin, line)) return LineStatus::Eof;
    return LineStatus::Ok;
  }

  // Cetak bagian awal prompt (mis. "\n") apa adanya; pakai baris terakhirnya untuk render.
  size_t nl = prompt.find_last_of('\n');
  Editor ed;
  ed.incolor = input_color;
  if (nl != std::string::npos) {
    ::fwrite(prompt.data(), 1, nl + 1, stdout);
    ed.plast = prompt.substr(nl + 1);
  } else {
    ed.plast = prompt;
  }

  std::string buf;
  size_t cur = 0;
  size_t hist = history.size();
  std::string stash;

  // Semua penulisan ke terminal (redraw editor + jam header) diserialisasi lewat io mutex,
  // supaya thread jam dan thread input tidak menimpa byte satu sama lain.
  std::mutex io;
  auto redraw = [&] {
    std::lock_guard<std::mutex> lk(io);
    ed.refresh(buf, cur);
  };
  redraw();

  // Jam hidup opsional: tiap detik tulis ulang field waktu di baris header (satu baris fisik
  // TEPAT di atas baris input) lewat simpan/pulih kursor (DECSC/DECRC), tanpa menyentuh buffer.
  std::atomic<bool> run{true};
  std::thread ticker;
  if (clock && clock->now) {
    ticker = std::thread([&] {
      std::string last;
      while (run.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        if (!run.load()) break;
        std::string t = clock->now();
        if (t == last) continue;  // hanya gambar ulang saat teks waktu berubah (per detik)
        last = t;
        std::lock_guard<std::mutex> lk(io);
        if (!run.load()) break;
        int up = ed.cur_input_row + 1;  // header = 1 baris di atas baris pertama input
        std::string s = "\033" "7";                       // DECSC: simpan kursor
        s += "\033[" + std::to_string(up) + "A";          // naik ke baris header
        s += "\r";
        if (clock->col > 0) s += "\033[" + std::to_string(clock->col) + "C";
        s += clock->color + t + "\033[0m";
        s += "\033" "8";                                   // DECRC: pulihkan kursor
        ::fwrite(s.data(), 1, s.size(), stdout);
        ::fflush(stdout);
      }
    });
  }
  // Hentikan + gabungkan thread jam sebelum menulis baris akhir, agar tidak ada penulisan
  // jam yang menyusul setelah layout prompt berubah.
  auto stop = [&] {
    run.store(false);
    if (ticker.joinable()) ticker.join();
  };

  auto set_hist = [&](size_t idx) {
    buf = history[idx];
    cur = buf.size();
  };

  char c;
  while (read_byte(c)) {
    unsigned char b = static_cast<unsigned char>(c);
    if (b == '\r' || b == '\n') {
      stop();
      ::fputs("\r\n", stdout);
      ::fflush(stdout);
      line = buf;
      if (!buf.empty() && (history.empty() || history.back() != buf)) history.push_back(buf);
      return LineStatus::Ok;
    }
    if (b == 3) {
      stop();
      ::fputs("^C\r\n", stdout);
      ::fflush(stdout);
      line.clear();
      return LineStatus::Interrupt;
    }
    if (b == 4) {
      if (buf.empty()) {
        stop();
        ::fputs("\r\n", stdout);
        ::fflush(stdout);
        return LineStatus::Eof;
      }
      continue;
    }
    if (b == 21) {  // Ctrl+U
      buf.clear();
      cur = 0;
      redraw();
      continue;
    }
    if (b == 1) {  // Ctrl+A
      cur = 0;
      redraw();
      continue;
    }
    if (b == 5) {  // Ctrl+E
      cur = buf.size();
      redraw();
      continue;
    }
    if (b == 127 || b == 8) {  // Backspace
      if (cur > 0) {
        size_t prev = utf8_prev(buf, cur);
        buf.erase(prev, cur - prev);
        cur = prev;
        redraw();
      }
      continue;
    }
    if (b == 27) {  // ESC
      char c1;
      if (!read_byte_timeout(c1, 150)) continue;
      if (c1 != '[' && c1 != 'O') continue;
      char c2;
      if (!read_byte_timeout(c2, 150)) continue;
      if (c2 == 'A') {
        if (hist > 0) {
          if (hist == history.size()) stash = buf;
          --hist;
          set_hist(hist);
          redraw();
        }
      } else if (c2 == 'B') {
        if (hist < history.size()) {
          ++hist;
          if (hist == history.size()) {
            buf = stash;
            cur = buf.size();
          } else {
            set_hist(hist);
          }
          redraw();
        }
      } else if (c2 == 'C') {
        if (cur < buf.size()) {
          cur = utf8_next(buf, cur);
          redraw();
        }
      } else if (c2 == 'D') {
        if (cur > 0) {
          cur = utf8_prev(buf, cur);
          redraw();
        }
      } else if (c2 == 'H') {
        cur = 0;
        redraw();
      } else if (c2 == 'F') {
        cur = buf.size();
        redraw();
      } else if (c2 >= '0' && c2 <= '9') {
        // CSI dengan parameter numerik (mis. ESC[3~ Delete, ESC[1;5C Ctrl+panah, ESC[23~ F11).
        // Kuras SELALU sampai byte akhir (0x40..0x7E) agar tidak ada byte sisa yang bocor jadi
        // teks liar, dan bandingkan parameter PERTAMA secara UTUH (bukan hanya digit terakhir)
        // supaya F3/F11 (ESC[13~/ESC[23~) tidak salah dikira Delete (ESC[3~).
        std::string first(1, c2);
        bool in_first = true;
        char x;
        while (read_byte_timeout(x, 150)) {
          if (x >= '@' && x <= '~') {  // byte akhir sekuens CSI
            if (x == '~' && first == "3" && cur < buf.size()) {
              size_t nx = utf8_next(buf, cur);
              buf.erase(cur, nx - cur);
              redraw();
            }
            break;
          }
          if (x == ';') {
            in_first = false;
          } else if (in_first && x >= '0' && x <= '9') {
            first += x;
          }
          // byte parameter/intermediate lain: terus dikuras tanpa disisipkan
        }
      }
      continue;
    }
    if (b < 0x20) continue;  // kontrol lain: abaikan
    buf.insert(cur, 1, c);
    cur += 1;
    redraw();
  }
  stop();
  ::fputs("\r\n", stdout);
  ::fflush(stdout);
  return LineStatus::Eof;
}

#else  // _WIN32

LineStatus read_line(const std::string& prompt, std::vector<std::string>& history, std::string& line,
                     const std::string& input_color, const LiveClock* clock) {
  (void)history;
  (void)input_color;
  (void)clock;
  std::cout << prompt << std::flush;
  if (!std::getline(std::cin, line)) return LineStatus::Eof;
  return LineStatus::Ok;
}

#endif

}  // namespace claw
