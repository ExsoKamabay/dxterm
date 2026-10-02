#include "catalog.hpp"

#include <stdexcept>

namespace claw {

Catalog::Catalog(fs::path data_dir) : data_dir_(std::move(data_dir)) {}

bool Catalog::split_name(const std::string& name, std::string& provider, std::string& id) {
  auto slash = name.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 >= name.size()) return false;
  provider = name.substr(0, slash);
  id = name.substr(slash + 1);
  return true;
}

void Catalog::load_providers() {
  providers_.clear();
  json root;
  try {
    root = json::parse(read_file(providers_path()), nullptr, true, true);
  } catch (const std::exception& e) {
    throw std::runtime_error("providers.json tidak valid: " + std::string(e.what()));
  }
  const json& list = root.contains("providers") ? root["providers"] : root;
  if (!list.is_object()) throw std::runtime_error("providers.json: 'providers' harus object");
  for (auto it = list.begin(); it != list.end(); ++it) {
    const json& p = it.value();
    Provider pr;
    pr.name = it.key();
    pr.chat_url = p.value("chat_url", "");
    pr.models_url = p.value("models_url", "");
    pr.models_format = p.value("models_format", "openai");
    pr.api_key = p.value("api_key", "");
    std::string key_env = p.value("api_key_env", "");
    if (!key_env.empty() && !get_env(key_env).empty()) pr.api_key = get_env(key_env);
    pr.include = p.value("include", "");
    pr.exclude = p.value("exclude", "");
    if (p.contains("match") && p["match"].is_array()) pr.match = p["match"];
    if (p.contains("models") && p["models"].is_array())
      for (const auto& m : p["models"])
        if (m.is_string()) pr.static_models.push_back(m.get<std::string>());
    pr.max_models = p.value("max_models", 0);
    pr.concurrency = std::max(1, p.value("concurrency", 2));
    pr.enabled = p.value("enabled", true);
    pr.note = p.value("note", "");
    // Provider free-tier yang butuh key (akun) hanya ikut scan bila key tersedia.
    if (p.value("requires_key", false) && pr.api_key.empty()) pr.enabled = false;
    if (pr.chat_url.empty()) {
      Log::warn("provider '" + pr.name + "' tidak punya chat_url, dilewati");
      continue;
    }
    providers_.push_back(std::move(pr));
  }
}

const Provider* Catalog::find_provider(const std::string& name) const {
  for (const auto& p : providers_)
    if (p.name == name) return &p;
  return nullptr;
}

std::vector<ModelRef> Catalog::load_models() const {
  std::vector<ModelRef> out;
  std::error_code ec;
  if (!fs::exists(models_path(), ec)) return out;
  ojson root;
  try {
    root = ojson::parse(read_file(models_path()), nullptr, true, true);
  } catch (const std::exception& e) {
    throw std::runtime_error("models.json tidak valid: " + std::string(e.what()));
  }
  if (!root.is_object()) throw std::runtime_error("models.json harus berupa object {model: apikey}");
  for (auto it = root.begin(); it != root.end(); ++it) {
    ModelRef m;
    m.name = it.key();
    if (!split_name(m.name, m.provider, m.upstream_id)) {
      Log::warn("nama model '" + m.name + "' harus berformat provider/model, dilewati");
      continue;
    }
    if (!find_provider(m.provider)) {
      Log::warn("provider '" + m.provider + "' untuk model '" + m.name +
                "' tidak ada di providers.json, dilewati");
      continue;
    }
    if (it.value().is_string())
      m.api_key = it.value().get<std::string>();
    else if (!it.value().is_null()) {
      Log::warn("apikey untuk '" + m.name + "' harus string, dilewati");
      continue;
    }
    out.push_back(std::move(m));
  }
  return out;
}

void Catalog::save_models(const std::vector<ModelRef>& models) const {
  ojson root = ojson::object();
  for (const auto& m : models) root[m.name] = m.api_key;
  write_file_atomic(models_path(), root.dump(2) + "\n");
}

}  // namespace claw
