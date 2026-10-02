#include "agents.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "json.hpp"
#include "util.hpp"

namespace claw {

using json = nlohmann::json;

namespace {

bool has_any(const std::string& hay, const std::vector<const char*>& needles) {
  for (const char* n : needles)
    if (hay.find(n) != std::string::npos) return true;
  return false;
}

struct DomainRule {
  const char* domain;
  std::vector<const char*> words;
  std::vector<const char*> skills;  // urut prioritas
};

// Bidang -> kata kunci -> skill. Urutan menentukan prioritas bila beberapa cocok.
const std::vector<DomainRule>& domain_rules() {
  static const std::vector<DomainRule> rules = {
      {"biologi",
       {"biologi", "biology", "gen ", "genom", "genome", "protein", "sel ", "cell", "dna", "rna", "enzim",
        "enzyme", "bakteri", "bacteria", "virus", "mikroba", "ekologi", "spesies", "species", "evolusi",
        "bioinform", "molekul", "klinis", "clinical"},
       {"riset", "web-research", "visualize"}},
      {"keamanan",
       {"keamanan", "security", "celah", "vulnerab", "pentest", "ctf", "exploit", "hardening", "cve",
        "audit keamanan"},
       {"security-audit", "audit-debug", "shell"}},
      {"android",
       {"android", "apk", "kotlin", "jetpack", "compose", "gradle"},
       {"android-dev", "engineering", "audit-debug"}},
      {"web",
       {"website", "web app", "webapp", "situs", "html", "css", "javascript", "typescript", "react", "vue",
        "frontend", "front-end", "backend", "back-end", "node"},
       {"web-dev", "engineering", "design"}},
      {"desain",
       {"desain", "design", "ui/ux", "tampilan", "logo", "banner", "warna", "palet", "tipografi",
        "mockup", "wireframe", "tui"},
       {"design", "diagram-maker"}},
      {"riset",
       {"riset", "research", "penelitian", "literatur", "jurnal", "paper", "survei", "survey", "bandingkan",
        "analisa", "analisis", "studi", "ide ", "brainstorm"},
       {"riset", "web-research", "library-reference"}},
      {"pengembangan",
       {"project", "proyek", "aplikasi", "app ", "program", "kode", "code", "coding", "koding", "script",
        "skrip", "build", "bangun", "buat ", "compile", "debug", "bug", "api", "library", "cli", "python",
        "c++", "rust", "golang", "kernel"},
       {"engineering", "audit-debug", "library-reference"}},
      {"terminal",
       {"terminal", "shell", "bash", "perintah linux", "command"},
       {"shell"}},
  };
  return rules;
}

std::vector<std::string> words_of(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || u >= 0x80) {
      cur += static_cast<char>(std::tolower(u));
    } else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// Ambil objek JSON pertama yang utuh dari teks bebas (jawaban model).
bool extract_json_object(const std::string& text, json& out) {
  for (size_t start = text.find('{'); start != std::string::npos; start = text.find('{', start + 1)) {
    int depth = 0;
    bool in_str = false, esc = false;
    for (size_t i = start; i < text.size(); ++i) {
      char c = text[i];
      if (in_str) {
        if (esc) esc = false;
        else if (c == '\\') esc = true;
        else if (c == '"') in_str = false;
        continue;
      }
      if (c == '"') in_str = true;
      else if (c == '{') ++depth;
      else if (c == '}' && --depth == 0) {
        json j = json::parse(text.substr(start, i - start + 1), nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
          out = j;
          return true;
        }
        break;
      }
    }
  }
  return false;
}

}  // namespace

TaskKind parse_kind(const std::string& s) {
  std::string k = to_lower(trim(s));
  if (k == "coding" || k == "code" || k == "development") return TaskKind::Coding;
  if (k == "reasoning" || k == "research" || k == "riset" || k == "analysis") return TaskKind::Reasoning;
  if (k == "design" || k == "desain") return TaskKind::Design;
  if (k == "vision") return TaskKind::Vision;
  return TaskKind::General;
}

const char* worker_role(TaskKind kind) {
  switch (kind) {
    case TaskKind::Coding: return "developer";
    case TaskKind::Design: return "desainer";
    case TaskKind::Reasoning: return "analis";
    case TaskKind::Vision: return "analis visual";
    default: return "asisten";
  }
}

size_t compact_keep_from(const std::vector<std::string>& roles, size_t system_end, size_t keep) {
  if (roles.size() <= system_end) return system_end;
  size_t from = roles.size() > keep ? roles.size() - keep : system_end;
  if (from < system_end) from = system_end;
  while (from > system_end && roles[from] != "user") --from;
  return from;
}

DelegateRequest parse_delegate(const std::string& raw_args, const std::vector<SkillInfo>& skills) {
  DelegateRequest d;
  json j = json::parse(raw_args.empty() ? "{}" : raw_args, nullptr, false);
  if (!j.is_object()) return d;
  d.task = trim(j.value("task", std::string()));
  d.role = trim(j.value("role", std::string()));
  d.kind = parse_kind(j.value("kind", std::string()));
  if (j.contains("skills") && j["skills"].is_array())
    for (const auto& s : j["skills"])
      if (s.is_string()) {
        const SkillInfo* si = find_skill(skills, trim(s.get<std::string>()));
        if (si && std::find(d.skills.begin(), d.skills.end(), si->name) == d.skills.end() &&
            d.skills.size() < 4)
          d.skills.push_back(si->name);
      }
  return d;
}

std::string guess_domain(const std::string& text) {
  std::string t = " " + to_lower(text) + " ";
  for (const auto& r : domain_rules())
    if (has_any(t, r.words)) return r.domain;
  return "umum";
}

std::vector<std::string> suggest_skills(const std::string& text, const std::vector<SkillInfo>& skills,
                                        size_t max_n) {
  std::string t = " " + to_lower(text) + " ";
  std::map<std::string, int> score;
  auto exists = [&](const std::string& n) { return find_skill(skills, n) != nullptr; };

  int weight = 100;
  for (const auto& r : domain_rules()) {
    if (has_any(t, r.words)) {
      int w = weight;
      for (const char* s : r.skills) {
        if (exists(s)) score[s] += w;
        w -= 10;
      }
    }
    weight -= 5;  // aturan yang lebih awal (lebih spesifik) sedikit lebih kuat
  }
  // Nama skill atau kata dari deskripsinya muncul di teks.
  std::set<std::string> tw;
  for (const auto& w : words_of(t))
    if (w.size() >= 5) tw.insert(w);
  for (const auto& sk : skills) {
    std::string name = to_lower(sk.name);
    if (t.find(name) != std::string::npos) score[sk.name] += 120;
    int hits = 0;
    for (const auto& w : words_of(sk.description))
      if (w.size() >= 5 && tw.count(w)) ++hits;
    if (hits) score[sk.name] += std::min(hits, 4) * 8;
  }
  std::vector<std::pair<std::string, int>> v(score.begin(), score.end());
  std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::vector<std::string> out;
  for (const auto& [n, s] : v) {
    if (s < 20 || out.size() >= max_n) break;
    out.push_back(n);
  }
  return out;
}

AgentPlan parse_plan(const std::string& text, const std::vector<SkillInfo>& skills, int max_agents,
                     std::string* error) {
  AgentPlan plan;
  json j;
  if (!extract_json_object(text, j)) {
    if (error) *error = "jawaban perencana tidak berisi JSON";
    return plan;
  }
  const json* arr = nullptr;
  for (const char* key : {"agents", "tasks", "plan"})
    if (j.contains(key) && j[key].is_array()) {
      arr = &j[key];
      break;
    }
  if (!arr || arr->empty()) {
    if (error) *error = "rencana tidak punya daftar agents";
    return plan;
  }
  if (j.contains("domain") && j["domain"].is_string()) plan.domain = trim(j["domain"].get<std::string>());

  max_agents = std::clamp(max_agents, 1, 8);
  std::set<std::string> ids;
  for (const auto& a : *arr) {
    if (static_cast<int>(plan.tasks.size()) >= max_agents) break;
    if (!a.is_object()) continue;
    AgentTask t;
    t.task = trim(a.value("task", std::string()));
    if (t.task.empty()) continue;
    t.id = trim(a.value("id", std::string()));
    if (t.id.empty() || ids.count(t.id)) t.id = "a" + std::to_string(plan.tasks.size() + 1);
    while (ids.count(t.id)) t.id += "_";
    ids.insert(t.id);
    t.role = trim(a.value("role", std::string("agen")));
    if (t.role.empty()) t.role = "agen";
    t.kind = parse_kind(a.value("kind", std::string()));
    if (a.contains("skills") && a["skills"].is_array())
      for (const auto& s : a["skills"])
        if (s.is_string()) {
          const SkillInfo* si = find_skill(skills, trim(s.get<std::string>()));
          if (si && std::find(t.skills.begin(), t.skills.end(), si->name) == t.skills.end() &&
              t.skills.size() < 4)
            t.skills.push_back(si->name);
        }
    if (a.contains("depends_on") && a["depends_on"].is_array())
      for (const auto& d : a["depends_on"])
        if (d.is_string()) t.depends_on.push_back(trim(d.get<std::string>()));
    plan.tasks.push_back(std::move(t));
  }
  if (plan.tasks.empty()) {
    if (error) *error = "rencana tidak punya tugas yang bisa dikerjakan";
    return plan;
  }
  // Ketergantungan hanya boleh ke agen yang disebut LEBIH AWAL: ini membuang
  // referensi ke diri sendiri, ke id tak dikenal, dan setiap siklus sekaligus.
  std::set<std::string> seen;
  for (auto& t : plan.tasks) {
    std::vector<std::string> ok;
    for (const auto& d : t.depends_on)
      if (seen.count(d) && std::find(ok.begin(), ok.end(), d) == ok.end()) ok.push_back(d);
    t.depends_on = ok;
    seen.insert(t.id);
  }
  if (plan.domain.empty()) plan.domain = "umum";
  return plan;
}

AgentPlan fallback_plan(const std::string& goal, const std::vector<SkillInfo>& skills) {
  AgentPlan p;
  p.domain = guess_domain(goal);
  AgentTask t;
  t.id = "a1";
  t.role = "agen utama";
  t.task = goal;
  t.kind = categorize_request(goal);
  t.skills = suggest_skills(goal, skills);
  p.tasks.push_back(std::move(t));
  return p;
}

std::vector<std::vector<size_t>> plan_waves(const AgentPlan& plan) {
  std::vector<std::vector<size_t>> waves;
  std::set<std::string> done;
  std::vector<bool> placed(plan.tasks.size(), false);
  size_t left = plan.tasks.size();
  while (left > 0) {
    std::vector<size_t> wave;
    for (size_t i = 0; i < plan.tasks.size(); ++i) {
      if (placed[i]) continue;
      bool ready = true;
      for (const auto& d : plan.tasks[i].depends_on)
        if (!done.count(d)) ready = false;
      if (ready) wave.push_back(i);
    }
    // Tidak mungkin setelah parse_plan, tapi jangan pernah berputar selamanya.
    if (wave.empty())
      for (size_t i = 0; i < plan.tasks.size(); ++i)
        if (!placed[i]) wave.push_back(i);
    for (size_t i : wave) {
      placed[i] = true;
      --left;
    }
    for (size_t i : wave) done.insert(plan.tasks[i].id);
    waves.push_back(std::move(wave));
  }
  return waves;
}

}  // namespace claw
