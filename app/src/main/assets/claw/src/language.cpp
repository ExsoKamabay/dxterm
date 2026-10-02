#include "language.hpp"

#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "json.hpp"

namespace claw {

using json = nlohmann::json;

namespace {

void read_jsonl(const fs::path& file, std::vector<Phrase>& out, std::set<std::string>& seen) {
  std::error_code ec;
  if (!fs::exists(file, ec)) return;
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    json j = json::parse(t, nullptr, false);
    if (j.is_discarded() || !j.is_object()) continue;
    Phrase p;
    p.intent = j.value("intent", "umum");
    p.text = j.value("text", "");
    if (p.text.empty()) continue;
    if (seen.insert(p.intent + "\x1f" + p.text).second) out.push_back(std::move(p));
  }
}

}  // namespace

std::vector<Phrase> load_phrases(const fs::path& data_dir, const fs::path& home) {
  std::vector<Phrase> out;
  std::set<std::string> seen;
  read_jsonl(data_dir / "language" / "id.jsonl", out, seen);
  read_jsonl(data_dir / "languange" / "id.jsonl", out, seen);  // ejaan alternatif
  if (!home.empty()) read_jsonl(home / ".claw" / "learned-id.jsonl", out, seen);
  return out;
}

bool learn_phrase(const fs::path& home, const std::string& intent, const std::string& text) {
  if (home.empty() || trim(text).empty()) return false;
  std::set<std::string> seen;
  std::vector<Phrase> have;
  read_jsonl(home / ".claw" / "learned-id.jsonl", have, seen);
  if (seen.count((intent.empty() ? "umum" : intent) + "\x1f" + text)) return true;  // sudah ada
  std::error_code ec;
  fs::create_directories(home / ".claw", ec);
  std::ofstream f(home / ".claw" / "learned-id.jsonl", std::ios::app);
  if (!f) return false;
  json j = {{"intent", intent.empty() ? "umum" : intent}, {"text", text}};
  f << j.dump() << "\n";
  return true;
}

std::string style_block(const std::vector<Phrase>& phrases, size_t per_intent) {
  if (phrases.empty()) return "";
  std::map<std::string, std::vector<std::string>> by;
  for (const auto& p : phrases)
    if (by[p.intent].size() < per_intent) by[p.intent].push_back(p.text);
  std::ostringstream os;
  os << "Contoh gaya bahasa Indonesia yang dipakai di terminal ini (tiru nada dan panjangnya, "
        "jangan disalin mentah):";
  for (const auto& [intent, list] : by) {
    os << "\n- " << intent << ": ";
    for (size_t i = 0; i < list.size(); ++i) os << (i ? " / " : "") << "\"" << list[i] << "\"";
  }
  return os.str();
}

}  // namespace claw
