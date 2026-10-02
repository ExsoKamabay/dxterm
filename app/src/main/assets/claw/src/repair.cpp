#include "repair.hpp"

#include <cstdlib>
#include <iostream>
#include <mutex>

#include "http.hpp"
#include "launcher.hpp"
#include "router.hpp"
#include "util.hpp"

namespace claw {

namespace {

void print_line(const std::string& s) { std::cout << s << std::endl; }

// Jalankan perintah shell, buang keluarannya ke file log. Kembalikan exit code.
int exec_to_log(const std::vector<std::string>& argv, const fs::path& logfile) {
  std::string cmd;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (i) cmd += " ";
    cmd += shell_quote(argv[i]);
  }
  cmd += " > " + shell_quote(logfile.string()) + " 2>&1";
#ifdef _WIN32
  cmd = "\"" + cmd + "\"";
#endif
  int rc = std::system(cmd.c_str());
#ifndef _WIN32
  if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
  return rc;
}

// Cari interpreter python: utamakan venv Scrapling, lalu python3 di PATH.
fs::path find_python(const fs::path& project_root) {
  fs::path venv = scrapling_python(project_root);
  if (!venv.empty()) return venv;
  std::string sys = find_in_path("python3");
  if (sys.empty()) sys = find_in_path("python");
  return sys.empty() ? fs::path() : fs::path(sys);
}

// Daftarkan provider baru ke providers.json (menjaga yang lama), lalu suruh
// catalog memuat ulang. Mengembalikan nama provider yang benar-benar baru.
std::vector<std::string> register_providers(Catalog& catalog, const json& new_providers) {
  std::vector<std::string> added;
  if (!new_providers.is_object() || new_providers.empty()) return added;
  ojson root;
  try {
    root = ojson::parse(read_file(catalog.providers_path()), nullptr, true, true);
  } catch (...) {
    return added;
  }
  if (!root.contains("providers") || !root["providers"].is_object()) return added;
  ojson& list = root["providers"];
  for (auto it = new_providers.begin(); it != new_providers.end(); ++it) {
    if (list.contains(it.key())) continue;  // sudah ada, jangan timpa
    ojson p = ojson::object();
    const json& src = it.value();
    p["chat_url"] = src.value("chat_url", "");
    if (!src.value("models_url", "").empty()) p["models_url"] = src.value("models_url", "");
    if (!src.value("api_key", "").empty()) p["api_key"] = src.value("api_key", "");
    p["models_format"] = src.value("models_format", "openai");
    if (src.contains("match") && src["match"].is_array()) p["match"] = src["match"];
    p["note"] = "ditambahkan otomatis oleh auto-repair";
    if (p["chat_url"].get<std::string>().empty()) continue;
    list[it.key()] = p;
    added.push_back(it.key());
  }
  if (!added.empty()) {
    write_file_atomic(catalog.providers_path(), root.dump(2) + "\n");
    catalog.load_providers();
  }
  return added;
}

// Hanya satu repair berjalan pada satu waktu.
std::mutex g_repair_mu;

}  // namespace

fs::path scrapling_python(const fs::path& project_root) {
#ifdef _WIN32
  fs::path p = project_root / "scrapling" / ".venv" / "Scripts" / "python.exe";
#else
  fs::path p = project_root / "scrapling" / ".venv" / "bin" / "python";
#endif
  std::error_code ec;
  return fs::exists(p, ec) ? p : fs::path();
}

fs::path tools_python(const fs::path& project_root) {
#ifdef _WIN32
  fs::path p = project_root / "tools" / ".venv" / "Scripts" / "python.exe";
#else
  fs::path p = project_root / "tools" / ".venv" / "bin" / "python";
#endif
  std::error_code ec;
  return fs::exists(p, ec) ? p : fs::path();
}

RepairResult run_repair(Catalog& catalog, const RepairOptions& opt, Router* live) {
  RepairResult r;
  std::unique_lock<std::mutex> lock(g_repair_mu, std::try_to_lock);
  if (!lock.owns_lock()) {
    r.error = "repair lain sedang berjalan";
    return r;
  }

  fs::path project_root = catalog.data_dir().parent_path();
  fs::path python = find_python(project_root);
  fs::path script = catalog.data_dir() / "scripts" / "repair_discover.py";
  std::error_code ec;
  if (python.empty() || !fs::exists(script, ec)) {
    r.error = "Scrapling belum di-setup (butuh scrapling/.venv dan " + script.string() + ")";
    return r;
  }

  fs::create_directories(catalog.state_dir(), ec);
  fs::path out = catalog.state_dir() / "repair-candidates.json";
  fs::path logf = catalog.state_dir() / "repair.log";
  fs::path sources = catalog.data_dir() / "repair-sources.json";

  std::vector<std::string> argv = {python.string(), script.string(), "--providers",
                                   catalog.providers_path().string(), "--out", out.string()};
  if (fs::exists(sources, ec)) {
    argv.push_back("--sources");
    argv.push_back(sources.string());
  }
  if (!opt.leads) argv.push_back("--no-leads");

  if (!opt.quiet) print_line("repair: mencari model gratis baru lewat Scrapling...");
  Log::info("repair: menjalankan discovery (" + python.string() + ")");
  int rc = exec_to_log(argv, logf);
  if (rc != 0 || !fs::exists(out, ec)) {
    r.error = "discovery gagal (exit " + std::to_string(rc) + "), lihat " + logf.string();
    return r;
  }

  json disc;
  try {
    disc = json::parse(read_file(out));
  } catch (const std::exception& e) {
    r.error = "hasil discovery tidak valid: " + std::string(e.what());
    return r;
  }

  if (disc.contains("leads") && disc["leads"].is_array())
    for (const auto& l : disc["leads"])
      if (l.is_object() && l.contains("url"))
        r.leads.push_back(l.value("source", "") + ": " + l.value("url", ""));

  // Provider baru dari sumber tambahan pengguna.
  if (disc.contains("providers"))
    r.new_providers = register_providers(catalog, disc["providers"]);

  // Kandidat yang belum ada di pool.
  std::vector<ModelRef> existing;
  try {
    existing = catalog.load_models();
  } catch (...) {
  }
  std::vector<std::string> have;
  for (const auto& m : existing) have.push_back(m.name);

  std::vector<ModelRef> candidates;
  if (disc.contains("candidates") && disc["candidates"].is_array()) {
    r.discovered = static_cast<int>(disc["candidates"].size());
    for (const auto& c : disc["candidates"]) {
      if (!c.is_object()) continue;
      ModelRef m;
      m.name = c.value("name", "");
      m.provider = c.value("provider", "");
      m.upstream_id = c.value("upstream_id", "");
      m.api_key = c.value("api_key", "");
      if (m.name.empty() || m.provider.empty() || m.upstream_id.empty()) continue;
      if (std::find(have.begin(), have.end(), m.name) != have.end()) continue;
      const Provider* pr = catalog.find_provider(m.provider);
      if (!pr || !pr->enabled) continue;  // provider tak dikenal / butuh key yang tak ada
      candidates.push_back(std::move(m));
    }
  }

  if (candidates.empty()) {
    if (!opt.quiet)
      print_line("repair: tidak ada kandidat baru (semua model gratis yang ditemukan sudah ada di pool)");
    return r;
  }

  // Uji kandidat sampai HTTP 200. Batasi jumlahnya agar repair tidak lama.
  ScanOptions so;
  so.probe_tools = opt.probe_tools;
  int limit = std::min(static_cast<int>(candidates.size()), std::max(1, opt.max_test));
  if (!opt.quiet)
    print_line("repair: menguji " + std::to_string(limit) + " dari " +
               std::to_string(candidates.size()) + " kandidat...");

  std::vector<ModelRef> passing;
  for (int i = 0; i < limit; ++i) {
    const ModelRef& m = candidates[i];
    const Provider* p = catalog.find_provider(m.provider);
    if (!p) continue;
    ++r.tested;
    ProbeResult pr = probe_model(m, *p, so);
    if (pr.passed) {
      passing.push_back(m);
      Log::info("repair: " + m.name + " OK (HTTP 200)");
      if (!opt.quiet) print_line("  + " + m.name);
    } else {
      Log::debug("repair: " + m.name + " gagal (" + pr.failure + ")");
    }
  }

  if (passing.empty()) {
    if (!opt.quiet) print_line("repair: tidak ada kandidat yang lolos uji");
    return r;
  }

  // Simpan: model lama tetap, model baru ditaruh setelahnya.
  std::vector<ModelRef> merged = existing;
  for (const auto& m : passing) merged.push_back(m);
  catalog.save_models(merged);
  r.added = static_cast<int>(passing.size());
  for (const auto& m : passing) r.added_names.push_back(m.name);

  // Sisipkan ke pool yang sedang jalan supaya langsung dipakai.
  if (live) live->add_models(catalog, passing);

  // Catatan hasil untuk ditinjau.
  try {
    ojson rep;
    rep["time"] = timestamp();
    rep["discovered"] = r.discovered;
    rep["tested"] = r.tested;
    rep["added"] = r.added;
    rep["added_names"] = r.added_names;
    rep["new_providers"] = r.new_providers;
    rep["leads"] = r.leads;
    write_file_atomic(catalog.state_dir() / "repair-report.json", rep.dump(2) + "\n");
  } catch (...) {
  }

  if (!opt.quiet)
    print_line("repair: " + std::to_string(r.added) + " model baru ditambahkan ke " +
               catalog.models_path().string());
  Log::info("repair: selesai, " + std::to_string(r.added) + " model baru");
  return r;
}

}  // namespace claw
