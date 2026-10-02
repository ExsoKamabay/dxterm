// Katalog provider (data/providers.json) dan daftar model aktif (data/models.json).
//
// models.json berformat {"<provider>/<model>": "<apikey>"} dan urutannya adalah
// urutan prioritas pemakaian. Nilai apikey kosong berarti request dikirim tanpa
// header Authorization.
#pragma once

#include <string>
#include <vector>

#include "json.hpp"
#include "util.hpp"

namespace claw {

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

struct Provider {
  std::string name;
  std::string chat_url;       // URL penuh endpoint chat completions
  std::string models_url;     // URL daftar model (boleh kosong)
  std::string models_format;  // "openai" ({data:[{id}]}) atau "array" ([{name}])
  std::string api_key;        // key default saat scan; boleh kosong
  std::string include;        // regex id model yang diambil
  std::string exclude;        // regex id model yang dibuang
  json match = json::array(); // model diambil jika cocok salah satu objek ini
  std::vector<std::string> static_models;  // dipakai bila models_url kosong/gagal
  int max_models = 0;         // 0 = tanpa batas
  int concurrency = 2;        // probe paralel per provider
  bool enabled = true;
  std::string note;
};

struct ModelRef {
  std::string name;         // "<provider>/<upstream id>"
  std::string provider;
  std::string upstream_id;
  std::string api_key;
};

class Catalog {
 public:
  explicit Catalog(fs::path data_dir);

  const fs::path& data_dir() const { return data_dir_; }
  fs::path providers_path() const { return data_dir_ / "providers.json"; }
  fs::path models_path() const { return data_dir_ / "models.json"; }
  fs::path state_dir() const { return data_dir_ / "state"; }

  void load_providers();
  const std::vector<Provider>& providers() const { return providers_; }
  const Provider* find_provider(const std::string& name) const;

  // Baca models.json. Entri yang providernya tidak dikenal dilewati dengan peringatan.
  std::vector<ModelRef> load_models() const;
  void save_models(const std::vector<ModelRef>& models) const;

  static bool split_name(const std::string& name, std::string& provider, std::string& id);

 private:
  fs::path data_dir_;
  std::vector<Provider> providers_;
};

}  // namespace claw
