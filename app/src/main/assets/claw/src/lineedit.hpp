// Editor baris raw-mode untuk `claw chat`: menafsirkan tombol panah (atas/bawah =
// riwayat, kiri/kanan = geser kursor), Backspace, Home/End, Ctrl+C/Ctrl+D, dan
// MENGABAIKAN escape sequence yang tidak dikenal. Ini menghilangkan bug saat tombol
// panah menyuntikkan karakter ^[[A/^[[B mentah ke input.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace claw {

enum class LineStatus { Ok, Eof, Interrupt };

// Jam hidup di baris header prompt: field waktu di baris tepat DI ATAS baris input
// ditulis ulang tiap detik tanpa mengganggu kursor input. Baris header wajib muat satu
// baris fisik (pemanggil memangkas isi bila perlu) agar posisinya bisa dihitung.
struct LiveClock {
  std::function<std::string()> now;  // string waktu terkini, mis. "HH:MM:SS"
  int col = 0;                        // kolom tampilan tempat waktu mulai di baris header
  std::string color;                  // SGR untuk waktu (mis. "\033[38;5;179m")
};

// Baca satu baris dengan penyuntingan + riwayat. prompt dicetak lebih dulu. Baris
// yang dikirim (tidak kosong) ditambahkan ke history. Saat bukan TTY, jatuh ke
// getline biasa. line diisi saat status Ok.
//
// input_color (opsional): SGR ANSI untuk mewarnai teks yang diketik user, mis.
// "\033[36m". Kosong = warna terminal bawaan. prompt boleh mengandung SGR; lebar
// tampilannya dihitung dengan mengabaikan escape ANSI.
LineStatus read_line(const std::string& prompt, std::vector<std::string>& history, std::string& line,
                     const std::string& input_color = "", const LiveClock* clock = nullptr);

}  // namespace claw
