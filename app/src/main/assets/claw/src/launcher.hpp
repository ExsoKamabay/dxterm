// Menemukan dan menjalankan OpenClaw dengan config yang mengarah ke proxy lokal.
#pragma once

#include <string>
#include <vector>

#include "util.hpp"

namespace claw {

struct OpenClawCommand {
  bool found = false;
  std::vector<std::string> prefix;  // mis. {"/path/openclaw"} atau {"node", "openclaw.mjs"}
  std::string source;               // keterangan asal penemuan
};

OpenClawCommand find_openclaw();

std::string find_in_path(const std::string& exe);

// Folder skill milik project (skills/, .agents/skills/, claude-skills/) yang
// ada di bawah project_root. Didaftarkan ke OpenClaw lewat skills.load.extraDirs
// supaya tetap termuat walaupun workspace agent berada di folder lain. Kalau
// workspace sama dengan project_root, skills/ dan .agents/skills/ sudah dibaca
// OpenClaw sebagai skill workspace, jadi tidak didaftarkan dua kali.
std::vector<fs::path> project_skill_dirs(const fs::path& project_root, const fs::path& workspace = {});

// Hitung skill (folder berisi SKILL.md) di bawah root dengan aturan OpenClaw untuk
// skills.load.extraDirs: berhenti turun begitu SKILL.md ditemukan, maksimal 2
// tingkat, dan lewati folder berawalan titik serta node_modules.
int count_skills(const fs::path& root);

// Tulis config OpenClaw terisolasi (provider "clawpool" -> proxy lokal).
void write_openclaw_config(const fs::path& config_path, int port, const std::string& token,
                           const fs::path& workspace, const std::vector<fs::path>& skill_dirs);

// Jalankan perintah dengan terminal yang diwarisi. Mengembalikan exit code.
int run_command(const std::vector<std::string>& argv);

}  // namespace claw
