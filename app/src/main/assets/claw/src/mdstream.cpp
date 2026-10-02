#include "mdstream.hpp"

#include <cstdio>

namespace claw {

namespace {
constexpr const char* kReset = "\033[0m";
constexpr const char* kCode = "\033[38;5;79m";       // hijau muda: kode
constexpr const char* kBar = "\033[38;5;240m│\033[0m ";  // bar kiri blok kode
constexpr const char* kInline = "\033[38;5;213m";    // merah muda: kode inline
constexpr const char* kBold = "\033[1m";
constexpr const char* kBoldOff = "\033[22m";
constexpr const char* kHead = "\033[1;38;5;39m";     // biru tebal: judul
}  // namespace

void MarkdownStreamer::raw(const std::string& s) {
  ::fwrite(s.data(), 1, s.size(), stdout);
}

// Tutup gaya yang sedang aktif (reset penuh), lalu pulihkan warna dasar prosa model.
void MarkdownStreamer::end_span() {
  raw(kReset);
  if (!base_.empty()) raw(base_);
}

void MarkdownStreamer::start_line() {
  // Bar kiri blok kode ditunda sampai baris itu benar-benar berisi (lihat emit), agar baris
  // pagar penutup ``` tidak menyisakan bar kosong di ujung blok.
  bar_pending_ = code_block_;
}

// Cetak teks terlihat: bar kiri blok kode yang tertunda lebih dulu.
void MarkdownStreamer::emit(const std::string& s) {
  if (bar_pending_) {
    bar_pending_ = false;
    if (enabled_) {
      raw(kBar);
      raw(kCode);
    }
  }
  skip_nl_ = false;
  col0_ = false;
  raw(s);
}

void MarkdownStreamer::resolve_backticks() {
  if (backticks_ == 0) return;
  int n = backticks_;
  backticks_ = 0;
  bt_at_line_start_ = false;
  if (n >= 3) {
    // Pagar blok kode. Pagar yang berdiri sendiri di barisnya tidak ditampilkan, begitu juga
    // baris barunya, supaya blok kode tidak diapit baris kosong.
    code_block_ = !code_block_;
    bar_pending_ = false;
    if (enabled_) {
      if (code_block_) raw(kCode);
      else end_span();
    }
    skip_nl_ = col0_;
    return;
  }
  if (code_block_) {  // di dalam kode, backtick literal
    emit(std::string(n, '`'));
    return;
  }
  if (n == 1) {
    inline_code_ = !inline_code_;
    if (enabled_) {
      if (inline_code_) raw(kInline);
      else end_span();
    }
    // pulihkan tebal bila sedang aktif setelah reset
    if (!inline_code_ && bold_ && enabled_) raw(kBold);
  } else {  // dua backtick: perlakukan literal
    emit("``");
  }
}

void MarkdownStreamer::resolve_stars() {
  if (stars_ == 0) return;
  int n = stars_;
  stars_ = 0;
  if (code_block_ || inline_code_) {
    emit(std::string(n, '*'));
    return;
  }
  if (n >= 2) {
    bold_ = !bold_;
    if (enabled_) raw(bold_ ? kBold : kBoldOff);
    if (n > 2) emit(std::string(n - 2, '*'));
  } else {
    emit("*");  // satu bintang: literal
  }
}

void MarkdownStreamer::put(char c) {
  // Akumulasi run backtick.
  if (c == '`') {
    if (stars_) resolve_stars();
    if (backticks_ == 0) bt_at_line_start_ = line_start_;
    ++backticks_;
    line_start_ = false;
    return;
  }
  if (backticks_) resolve_backticks();

  if (c == '*' && !code_block_) {
    ++stars_;
    line_start_ = false;
    return;
  }
  if (stars_) resolve_stars();

  if (c == '\n') {
    if (skip_nl_) {  // baris pagar ``` yang kosong: tidak menambah baris
      skip_nl_ = false;
      line_start_ = true;
      start_line();
      return;
    }
    if (code_block_ && bar_pending_) emit("");  // baris kosong di dalam kode tetap berbar
    if (heading_ && enabled_) {
      end_span();
      heading_ = false;
    }
    if (inline_code_ && enabled_) {  // jangan biarkan gaya inline lintas baris
      end_span();
      inline_code_ = false;
    }
    if (code_block_ && enabled_) raw(kReset);  // tutup warna di ujung baris kode
    raw("\n");
    col0_ = true;
    line_start_ = true;
    start_line();
    return;
  }

  // Judul: '#' di awal baris (di luar blok kode).
  if (line_start_ && !code_block_ && c == '#') {
    heading_ = true;
    if (enabled_) raw(kHead);
    emit("#");
    line_start_ = false;
    return;
  }

  if (line_start_) line_start_ = false;
  emit(std::string(1, c));
}

// Model sering membuka jawaban dengan beberapa baris kosong, menyelipkan tiga-empat baris
// kosong antar paragraf, atau menutup dengan spasi. Semua itu tampil sebagai ruang kosong
// lebar di layar. Di sini spasi dan baris baru ditahan dulu: yang ada di awal dan akhir
// jawaban dibuang, spasi di ujung baris dibuang, dan baris kosong berturut-turut di luar
// blok kode dirapatkan menjadi satu. Blok kode tetap utuh.
void MarkdownStreamer::filter(char c) {
  if (c == '\r') return;
  if (c == '\n') {
    ++pend_nl_;
    pend_ws_.clear();  // spasi di ujung baris tidak berarti apa-apa
    return;
  }
  if (c == ' ' || c == '\t') {
    pend_ws_ += c;
    return;
  }
  if (!started_) {
    // Karakter terlihat pertama: buang semua spasi/baris kosong sebelumnya.
    pend_nl_ = 0;
    pend_ws_.clear();
    started_ = true;
    if (!lead_.empty()) raw(lead_);
    if (enabled_ && !base_.empty() && !base_open_) {
      raw(base_);
      base_open_ = true;
    }
  }
  if (pend_nl_ > 0) {
    // Pagar ``` yang masih tertahan harus diselesaikan dulu agar status blok kode benar.
    if (backticks_) resolve_backticks();
    int n = code_block_ ? pend_nl_ : (pend_nl_ > 2 ? 2 : pend_nl_);
    pend_nl_ = 0;
    for (int i = 0; i < n; ++i) put('\n');
  }
  for (char w : pend_ws_) put(w);
  pend_ws_.clear();
  put(c);
}

void MarkdownStreamer::feed(const std::string& chunk) {
  // Warna dasar prosa dipancarkan saat karakter terlihat pertama (lihat filter), agar teks
  // respons model punya identitas warna sendiri (berbeda dari teks user dan dari kode).
  for (char c : chunk) filter(c);
  ::fflush(stdout);
}

void MarkdownStreamer::finish() {
  // Spasi/baris baru yang masih tertahan adalah ekor jawaban: dibuang.
  pend_nl_ = 0;
  pend_ws_.clear();
  if (backticks_) resolve_backticks();
  if (stars_) resolve_stars();
  if (enabled_ && (inline_code_ || bold_ || heading_ || code_block_ || base_open_)) raw(kReset);
  inline_code_ = bold_ = heading_ = code_block_ = false;
  bar_pending_ = skip_nl_ = false;
  base_open_ = false;
  ::fflush(stdout);
}

}  // namespace claw
