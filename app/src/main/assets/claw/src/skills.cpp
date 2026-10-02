#include "skills.hpp"

#include <algorithm>
#include <set>
#include <sstream>

namespace claw {

namespace {

std::string unquote(std::string v) {
  v = trim(v);
  if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
    v = v.substr(1, v.size() - 2);
  return v;
}

void collect(const fs::path& dir, int depth, std::vector<fs::path>& out) {
  std::error_code ec;
  if (fs::exists(dir / "SKILL.md", ec)) {
    out.push_back(dir / "SKILL.md");
    return;
  }
  if (depth >= 2) return;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code ec2;
    std::string name = it->path().filename().string();
    if (!name.empty() && name[0] != '.' && name != "node_modules" && it->is_directory(ec2))
      collect(it->path(), depth + 1, out);
  }
}

}  // namespace

SkillInfo parse_skill(const fs::path& skill_md, const std::string& content) {
  SkillInfo s;
  s.file = skill_md;
  s.name = skill_md.parent_path().filename().string();
  std::istringstream in(content);
  std::string line;
  bool in_front = false, first = true;
  std::string body_first;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (first) {
      first = false;
      if (trim(line) == "---") {
        in_front = true;
        continue;
      }
    }
    if (in_front) {
      if (trim(line) == "---") {
        in_front = false;
        continue;
      }
      if (line.rfind("name:", 0) == 0) {
        std::string v = unquote(line.substr(5));
        if (!v.empty()) s.name = v;
      } else if (line.rfind("description:", 0) == 0) {
        s.description = unquote(line.substr(12));
      }
      continue;
    }
    std::string t = trim(line);
    if (body_first.empty() && !t.empty() && t[0] != '#') body_first = t;
  }
  if (s.description.empty()) s.description = body_first;
  if (s.description.size() > 300) s.description = s.description.substr(0, 297) + "...";
  return s;
}

std::vector<SkillInfo> list_skills(const std::vector<fs::path>& dirs) {
  std::vector<SkillInfo> out;
  std::set<std::string> seen;
  for (const auto& d : dirs) {
    std::vector<fs::path> files;
    collect(d, 0, files);
    for (const auto& f : files) {
      std::string content;
      try {
        content = read_file(f);
      } catch (...) {
        continue;
      }
      SkillInfo s = parse_skill(f, content);
      if (seen.insert(s.name).second) out.push_back(std::move(s));
    }
  }
  std::sort(out.begin(), out.end(), [](const SkillInfo& a, const SkillInfo& b) { return a.name < b.name; });
  return out;
}

std::string read_skill(const SkillInfo& s, size_t max_chars) {
  std::string c = read_file(s.file);
  if (c.size() > max_chars) c = c.substr(0, max_chars) + "\n\n[... dipotong, " + std::to_string(c.size()) + " karakter total]";
  return c;
}

const SkillInfo* find_skill(const std::vector<SkillInfo>& skills, const std::string& name) {
  std::string want = to_lower(trim(name));
  for (const auto& s : skills)
    if (to_lower(s.name) == want) return &s;
  return nullptr;
}

}  // namespace claw
