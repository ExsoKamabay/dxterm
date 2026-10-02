// Perender Markdown streaming ke ANSI untuk jawaban `claw chat`. Teks dimasukkan
// potongan demi potongan (feed) dan langsung dicetak per karakter, sambil memberi
// gaya pada blok kode berpagar (```), kode inline (`), tebal (**), dan judul (#).
// Aman dipanggil berkali-kali; finish() menutup gaya yang masih terbuka.
#pragma once

#include <string>

namespace claw {

class MarkdownStreamer {
 public:
  // enabled=false: cetak apa adanya (tanpa ANSI), tetap per karakter.
  explicit MarkdownStreamer(bool enabled = true) : enabled_(enabled) {}

  void feed(const std::string& chunk);
  void finish();

  // Warna dasar prosa (SGR) untuk membedakan teks respons model dari teks lain.
  // Kosong = warna terminal bawaan. Setelah tiap gaya ditutup, warna kembali ke sini.
  void set_base(const std::string& sgr) { base_ = sgr; }

  // Teks yang dicetak tepat sebelum karakter terlihat pertama (mis. "\n" sebagai jarak dari
  // prompt). Tidak dicetak sama sekali bila model hanya mengirim spasi/baris kosong.
  void set_lead(const std::string& s) { lead_ = s; }

  // Sudah ada karakter terlihat yang dicetak.
  bool shown() const { return started_; }

  // Keluaran terakhir berakhir di awal baris (mis. setelah pagar ``` penutup), jadi pemanggil
  // tidak perlu menambah baris baru lagi.
  bool at_col0() const { return col0_; }

  bool in_code_block() const { return code_block_; }

 private:
  void filter(char c);  // rapikan spasi/baris kosong, lalu teruskan ke put()
  void put(char c);
  void raw(const std::string& s);
  void emit(const std::string& s);  // teks terlihat (didahului bar kode yang tertunda)
  void end_span();  // tutup gaya lalu kembali ke warna dasar prosa
  void resolve_backticks();
  void resolve_stars();
  void start_line();

  bool enabled_;
  std::string lead_;          // dicetak sekali sebelum karakter terlihat pertama
  bool started_ = false;      // karakter terlihat pertama sudah dicetak
  int pend_nl_ = 0;           // baris baru yang ditahan (dirapatkan saat karakter berikutnya)
  std::string pend_ws_;       // spasi/tab yang ditahan (dibuang bila baris berakhir)
  bool col0_ = true;          // kursor ada di awal baris
  bool bar_pending_ = false;  // bar kiri blok kode belum dicetak untuk baris ini
  bool skip_nl_ = false;      // baris baru setelah pagar ``` yang berdiri sendiri: dibuang
  std::string base_;          // warna dasar prosa model
  bool base_open_ = false;    // base_ sudah dipancarkan untuk sesi ini
  bool line_start_ = true;
  bool code_block_ = false;
  bool inline_code_ = false;
  bool bold_ = false;
  bool heading_ = false;
  int backticks_ = 0;  // run backtick yang belum diselesaikan
  int stars_ = 0;      // run '*' yang belum diselesaikan
  bool bt_at_line_start_ = false;  // run backtick dimulai di awal baris
};

}  // namespace claw
