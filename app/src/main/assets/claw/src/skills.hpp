// Skill untuk `claw chat`: model melihat daftar nama + deskripsi skill, lalu
// memuat isi SKILL.md yang cocok dengan percakapan lewat tool use_skill.
#pragma once

#include <string>
#include <vector>

#include "util.hpp"

namespace claw {

struct SkillInfo {
  std::string name;
  std::string description;
  fs::path file;  // SKILL.md
};

// Skill (folder berisi SKILL.md, maksimal 2 tingkat) di bawah dirs. Nama ganda:
// yang pertama menang. Urut sesuai nama.
std::vector<SkillInfo> list_skills(const std::vector<fs::path>& dirs);

// Baca frontmatter SKILL.md (name, description). Dipakai list_skills; dipisah untuk tes.
SkillInfo parse_skill(const fs::path& skill_md, const std::string& content);

// Isi SKILL.md, dipotong sampai max_chars.
std::string read_skill(const SkillInfo& s, size_t max_chars = 16000);

const SkillInfo* find_skill(const std::vector<SkillInfo>& skills, const std::string& name);

}  // namespace claw
