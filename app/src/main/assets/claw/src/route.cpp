#include "route.hpp"

#include <array>
#include <regex>

#include "util.hpp"

namespace claw {

const char* task_name(TaskKind k) {
  switch (k) {
    case TaskKind::Coding: return "coding";
    case TaskKind::Vision: return "vision";
    case TaskKind::Reasoning: return "reasoning";
    case TaskKind::Design: return "design";
    case TaskKind::General: return "umum";
  }
  return "umum";
}

namespace {

bool has_any(const std::string& hay, std::initializer_list<const char*> needles) {
  for (const char* n : needles)
    if (hay.find(n) != std::string::npos) return true;
  return false;
}

// Miliaran parameter terbesar yang tersebut di nama (mis. "550b", "27B", "2.6b").
// 0 bila tak ada. Dipakai sebagai bobot "kekuatan" untuk tugas berat.
double param_billions(const std::string& name) {
  static const std::regex re(R"(([0-9]+(?:\.[0-9]+)?)\s*b)", std::regex::icase);
  double best = 0;
  for (std::sregex_iterator it(name.begin(), name.end(), re), end; it != end; ++it) {
    try {
      best = std::max(best, std::stod((*it)[1].str()));
    } catch (...) {
    }
  }
  return best;
}

}  // namespace

TaskKind categorize_request(const std::string& text) {
  std::string t = to_lower(text);
  // Coding paling spesifik dan paling sering, cek lebih dulu.
  if (has_any(t, {"koding", "coding", "kode ", " kode", "program", "fungsi", "function", "script",
                  "skrip", "debug", "compile", "kompilasi", "refactor", "api ", "endpoint", "regex",
                  "sql", "html", "css", "javascript", "typescript", "python", "c++", "rust", "golang",
                  "kotlin", "swift", "kernel", "bug", "class ", "kelas ", "library", "pustaka",
                  "framework", "docker", "git ", "build "}))
    return TaskKind::Coding;
  if (has_any(t, {"desain", "design", "ui/ux", " ui ", " ux ", "tui", "tampilan", "warna", "palet",
                  "layout", "logo", "banner", "tipografi", "font ", "mockup", "wireframe"}))
    return TaskKind::Design;
  if (has_any(t, {"gambar", "image", "foto", "screenshot", "ocr", "vision", "lihat gambar"}))
    return TaskKind::Vision;
  if (has_any(t, {"analisa", "analisis", "analyze", "buktikan", "reasoning", "nalar", "matematika",
                  "math", "hitung", "kalkulasi", "strategi", "rencana", "riset", "research",
                  "bandingkan", "evaluasi", "mengapa", "kenapa", "logika", "teorema"}))
    return TaskKind::Reasoning;
  return TaskKind::General;
}

int score_model(const std::string& model_name, TaskKind kind) {
  std::string n = to_lower(model_name);
  double size = param_billions(n);
  int size_bonus = static_cast<int>(size);  // 0..~550
  bool instruct = has_any(n, {"instruct", "-it", "chat"});

  switch (kind) {
    case TaskKind::Coding:
      if (has_any(n, {"coder", "codestral", "-code", "code-", "starcoder", "deepseek-coder"}))
        return 1000 + size_bonus;
      if (has_any(n, {"code"})) return 700 + size_bonus;
      // Model besar/instruct tetap layak coding walau bukan model khusus.
      return 100 + size_bonus + (instruct ? 20 : 0);
    case TaskKind::Vision:
      if (has_any(n, {"-vl", "vl-", "vision", "omni", "multimodal", "-v-", "2.5-vl"}))
        return 1000 + size_bonus;
      return 50 + size_bonus;
    case TaskKind::Reasoning:
      if (has_any(n, {"reason", "think", "-r1", "qwq", "o1", "o3", "deepseek-r"}))
        return 900 + size_bonus;
      // Untuk nalar, ukuran sangat menentukan.
      return 200 + size_bonus * 2 + (instruct ? 20 : 0);
    case TaskKind::Design:
    case TaskKind::General:
      return 100 + size_bonus + (instruct ? 40 : 0);
  }
  return size_bonus;
}

std::string pick_model(const std::vector<std::string>& usable, TaskKind kind) {
  if (kind == TaskKind::General) return "";  // biarkan router memakai urutan prioritas biasa
  std::string best;
  int best_score = -1;
  for (const auto& name : usable) {
    int sc = score_model(name, kind);
    if (sc > best_score) {
      best_score = sc;
      best = name;
    }
  }
  // Hanya rute bila ada model yang benar-benar cocok (skor tinggi), selain itu "auto".
  int threshold = (kind == TaskKind::Coding || kind == TaskKind::Vision) ? 700
                  : kind == TaskKind::Reasoning                          ? 500
                                                                         : 200;
  if (best_score < threshold) return "";
  return best;
}

}  // namespace claw
