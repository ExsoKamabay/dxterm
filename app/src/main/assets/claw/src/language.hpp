// Gaya bahasa Indonesia untuk claw: kalimat ucapan diambil dari
// data/language/id.jsonl, dan claw belajar dari pengalaman dengan menambah
// kalimat baru ke ~/.claw/learned-id.jsonl di HOME terminal.
#pragma once

#include <string>
#include <vector>

#include "util.hpp"

namespace claw {

struct Phrase {
  std::string intent;
  std::string text;
};

// Baca id.jsonl bawaan (data_dir/language/id.jsonl) lalu kalimat yang sudah
// dipelajari (home/.claw/learned-id.jsonl). Baris rusak dilewati. Untuk
// kompatibilitas juga membaca ejaan folder "languange" bila ada.
std::vector<Phrase> load_phrases(const fs::path& data_dir, const fs::path& home);

// Tambah satu kalimat ke berkas yang dipelajari. Duplikat persis dilewati.
// Mengembalikan false bila gagal menulis.
bool learn_phrase(const fs::path& home, const std::string& intent, const std::string& text);

// Blok singkat untuk system prompt: beberapa contoh kalimat per intent, supaya
// nada model terdengar seperti kalimat di id.jsonl. Kosong bila tak ada kalimat.
std::string style_block(const std::vector<Phrase>& phrases, size_t per_intent = 2);

}  // namespace claw
