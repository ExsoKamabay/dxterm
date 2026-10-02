#include "policy.hpp"

#include <algorithm>

#include "json.hpp"

namespace claw {

using json = nlohmann::json;

Policy load_policy(const fs::path& data_dir, std::string* warning) {
  Policy p;
  fs::path f = data_dir / "policy.json";
  std::error_code ec;
  if (!fs::exists(f, ec)) return p;
  json j;
  try {
    j = json::parse(read_file(f));
  } catch (const std::exception& e) {
    if (warning) *warning = "policy.json tidak valid, memakai pengaturan aman bawaan: " + std::string(e.what());
    return p;
  }
  if (!j.is_object()) return p;

  if (j.contains("browse") && j["browse"].is_string()) {
    std::string b = to_lower(j["browse"].get<std::string>());
    if (b == "off" || b == "ask" || b == "allow") p.browse = b;
  }
  if (j.contains("fs_access") && j["fs_access"].is_string()) {
    std::string a = to_lower(j["fs_access"].get<std::string>());
    if (a == "off" || a == "ask" || a == "allow") p.fs_access = a;
  }
  auto flag = [&](const char* key, bool& out) {
    if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
  };
  flag("private_network", p.private_network);
  flag("skills", p.skills);
  flag("auto_update", p.auto_update);
  flag("proxy_token", p.proxy_token);
  flag("save_conversations", p.save_conversations);
  flag("humanizer", p.humanizer);
  flag("activity_log", p.activity_log);
  auto model = [&](const char* key, std::string& out) {
    if (!j.contains(key) || !j[key].is_string()) return;
    std::string m = trim(j[key].get<std::string>());
    out = (m.empty() || to_lower(m) == "auto") ? "auto" : m;
  };
  model("chat_model", p.chat_model);
  model("humanizer_model", p.humanizer_model);
  if (j.contains("humanizer_agents") && j["humanizer_agents"].is_number_integer())
    p.humanizer_agents = std::clamp(j["humanizer_agents"].get<int>(), 1, 6);
  if (j.contains("update_interval_hours") && j["update_interval_hours"].is_number_integer())
    p.update_interval_hours = std::clamp(j["update_interval_hours"].get<int>(), 1, 24 * 30);
  return p;
}

}  // namespace claw
