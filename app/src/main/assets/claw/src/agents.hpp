// Humanizer: mode otonom multi-agen. Satu model perencana memecah tujuan pengguna
// menjadi beberapa tugas, memilih skill untuk tiap tugas (pengembangan project,
// riset, biologi, desain, keamanan, ...), lalu tiap tugas dikerjakan agen
// tersendiri. Tugas tanpa ketergantungan berjalan bersamaan.
//
// File ini hanya berisi logika murni (parsing rencana, pemilihan skill, urutan
// gelombang) supaya bisa diuji tanpa jaringan. Eksekusinya ada di main.cpp.
#pragma once

#include <string>
#include <vector>

#include "route.hpp"
#include "skills.hpp"

namespace claw {

struct AgentTask {
  std::string id;                       // "a1", "a2", ...
  std::string role;                     // mis. "peneliti", "developer"
  std::string task;                     // instruksi untuk agen ini
  TaskKind kind = TaskKind::General;    // dipakai memilih model
  std::vector<std::string> skills;      // nama skill yang dimuat untuk agen ini
  std::vector<std::string> depends_on;  // id agen yang hasilnya dibutuhkan
};

struct AgentPlan {
  std::string domain;  // bidang tujuan, mis. "pengembangan", "riset", "biologi"
  std::vector<AgentTask> tasks;
};

// Permintaan delegasi dari agen depan (`claw chat`) ke agen pekerja: satu tugas,
// jenis pekerjaan (memilih model/spesialis), peran opsional, dan skill yang dimuat.
struct DelegateRequest {
  std::string task;                 // pekerjaan yang harus diselesaikan pekerja
  std::string role;                 // peran opsional yang diminta agen depan
  TaskKind kind = TaskKind::General;
  std::vector<std::string> skills;  // hanya skill yang benar-benar ada, maks 4
};

// Baca argumen tool delegate_task (JSON dari agen depan). Toleran terhadap JSON
// rusak/kosong: task dipangkas, kind lewat parse_kind, skill difilter ke yang ada
// (maks 4, tanpa duplikat). task kosong berarti permintaan tidak sah.
DelegateRequest parse_delegate(const std::string& raw_args, const std::vector<SkillInfo>& skills);

// Label peran pekerja default dari jenis tugas (developer, desainer, analis, ...).
const char* worker_role(TaskKind kind);

// Batas kompaksi riwayat chat (konteks tak terbatas): dari daftar peran pesan, cari
// indeks awal blok terbaru yang dipertahankan apa adanya. Mundur dari (size - keep) ke
// batas pesan 'user' pertama, tidak pernah lebih awal dari system_end (jumlah pesan
// system di depan). Semua pesan [system_end, hasil) diringkas jadi satu memori.
// Mengembalikan system_end bila tidak ada yang layak diringkas.
size_t compact_keep_from(const std::vector<std::string>& roles, size_t system_end, size_t keep);

// "coding"/"reasoning"/"design"/"vision"/lainnya -> TaskKind.
TaskKind parse_kind(const std::string& s);

// Skill yang paling relevan untuk sebuah teks, dari kata kunci bidang (pengembangan,
// riset, biologi, desain, keamanan, Android, ...) dan kecocokan nama/deskripsi skill.
// Hanya skill yang benar-benar ada di `skills`. Paling banyak max_n, urut relevansi.
std::vector<std::string> suggest_skills(const std::string& text, const std::vector<SkillInfo>& skills,
                                        size_t max_n = 3);

// Tebak bidang dari teks tujuan ("pengembangan", "riset", "biologi", "desain",
// "keamanan", "umum").
std::string guess_domain(const std::string& text);

// Baca rencana JSON dari jawaban model perencana. Toleran terhadap pagar ```json
// dan teks di sekitarnya. Skill yang tidak ada dibuang, id ganda diganti,
// ketergantungan ke id tak dikenal / ke diri sendiri / yang membentuk siklus
// dibuang, jumlah tugas dibatasi max_agents. Mengembalikan rencana kosong dan
// mengisi *error bila tidak ada JSON rencana yang bisa dipakai.
AgentPlan parse_plan(const std::string& text, const std::vector<SkillInfo>& skills, int max_agents,
                     std::string* error = nullptr);

// Rencana cadangan bila perencana gagal: satu agen dengan skill hasil suggest_skills.
AgentPlan fallback_plan(const std::string& goal, const std::vector<SkillInfo>& skills);

// Urutan eksekusi: gelombang berisi indeks tugas yang semua ketergantungannya sudah
// selesai di gelombang sebelumnya. Tugas dalam satu gelombang boleh berjalan bersamaan.
std::vector<std::vector<size_t>> plan_waves(const AgentPlan& plan);

}  // namespace claw
