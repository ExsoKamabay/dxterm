// Memilih model yang cocok dengan konteks percakapan dari pool models.json.
// Contoh: permintaan coding -> model yang namanya mengandung "coder/code/codestral";
// analisa/riset -> model "reasoning" atau yang paling besar; sisanya -> model instruct
// terbesar yang tersedia. Semua berbasis pola nama, jadi bekerja untuk model apa pun.
#pragma once

#include <string>
#include <vector>

namespace claw {

enum class TaskKind { General, Coding, Vision, Reasoning, Design };

const char* task_name(TaskKind k);

// Tebak jenis tugas dari teks permintaan (Indonesia + Inggris).
TaskKind categorize_request(const std::string& text);

// Skor kecocokan nama model untuk sebuah jenis tugas (makin besar makin cocok).
// Dipublikasikan untuk pengujian.
int score_model(const std::string& model_name, TaskKind kind);

// Dari daftar model yang SIAP dipakai (tidak sedang cooldown), pilih yang paling
// cocok untuk jenis tugas. Mengembalikan "" bila tidak ada yang lebih baik dari
// pilihan biasa (pemanggil lalu memakai "auto"). Urutan daftar = urutan prioritas
// pool, dipakai sebagai pemecah seri.
std::string pick_model(const std::vector<std::string>& usable, TaskKind kind);

}  // namespace claw
