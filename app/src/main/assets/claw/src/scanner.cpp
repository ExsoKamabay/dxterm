#include "scanner.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "http.hpp"
#include "router.hpp"

namespace claw {

namespace {

std::mutex g_print_mu;

void print_line(const std::string& s) {
  std::lock_guard<std::mutex> lk(g_print_mu);
  std::cout << s << std::endl;
}

// Ke layar (kecuali quiet) dan selalu ke file log, supaya jalannya scan/update
// bisa dipantau realtime lewat state/claw.log.
void say(const ScanOptions& opt, const std::string& s) {
  if (!opt.quiet) print_line(s);
  Log::note(trim(s));
}

bool matches_any(const json& model, const json& match) {
  if (!match.is_array() || match.empty()) return true;
  for (const auto& cond : match) {
    if (!cond.is_object()) continue;
    bool all = true;
    for (auto it = cond.begin(); it != cond.end(); ++it) {
      if (!model.contains(it.key()) || model[it.key()] != it.value()) {
        all = false;
        break;
      }
    }
    if (all) return true;
  }
  return false;
}

std::string snippet(const std::string& s, size_t n = 160) {
  std::string out;
  for (char c : s) out.push_back(c == '\n' || c == '\r' ? ' ' : c);
  if (out.size() > n) out = out.substr(0, n) + "...";
  return out;
}

int retry_after(const HttpResponse& res, int fallback, int cap) {
  auto it = res.headers.find("retry-after");
  if (it != res.headers.end()) {
    try {
      return std::min(std::max(std::stoi(it->second), 1), std::max(1, cap));
    } catch (...) {
    }
  }
  return std::min(fallback, cap);
}

void sleep_sec(int s) { std::this_thread::sleep_for(std::chrono::seconds(s)); }

const json kProbeTool = json::parse(R"({
  "type": "function",
  "function": {
    "name": "get_time",
    "description": "Return the current time for a city.",
    "parameters": {
      "type": "object",
      "properties": {"city": {"type": "string"}},
      "required": ["city"]
    }
  }
})");

}  // namespace

std::vector<std::string> discover_models(const Provider& p, std::string* error) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  auto add = [&](const std::string& id) {
    if (id.empty() || seen.count(id)) return;
    seen.insert(id);
    out.push_back(id);
  };

  std::regex inc, exc;
  bool has_inc = !p.include.empty(), has_exc = !p.exclude.empty();
  try {
    if (has_inc) inc = std::regex(p.include, std::regex::icase);
    if (has_exc) exc = std::regex(p.exclude, std::regex::icase);
  } catch (const std::regex_error& e) {
    if (error) *error = "regex include/exclude tidak valid: " + std::string(e.what());
    return out;
  }
  auto accept_id = [&](const std::string& id) {
    if (has_inc && !std::regex_search(id, inc)) return false;
    if (has_exc && std::regex_search(id, exc)) return false;
    return true;
  };

  if (!p.models_url.empty()) {
    HttpOptions ho;
    ho.bearer = p.api_key;
    ho.read_timeout_sec = 30;
    HttpResponse res = http_get(p.models_url, ho);
    if (res.status == 200) {
      json j = json::parse(res.body, nullptr, false);
      json list = json::array();
      if (j.is_array())
        list = j;
      else if (j.is_object() && j.contains("data") && j["data"].is_array())
        list = j["data"];
      else if (j.is_object() && j.contains("models") && j["models"].is_array())
        list = j["models"];
      for (const auto& m : list) {
        std::string id;
        if (m.is_string()) {
          id = m.get<std::string>();
        } else if (m.is_object()) {
          if (!matches_any(m, p.match)) continue;
          if (m.contains("id") && m["id"].is_string())
            id = m["id"].get<std::string>();
          else if (m.contains("name") && m["name"].is_string())
            id = m["name"].get<std::string>();
        }
        if (!id.empty() && accept_id(id)) add(id);
      }
    } else if (error) {
      *error = res.status ? "HTTP " + std::to_string(res.status) : res.error;
    }
  }
  for (const auto& id : p.static_models)
    if (accept_id(id)) add(id);
  if (p.max_models > 0 && static_cast<int>(out.size()) > p.max_models) out.resize(p.max_models);
  return out;
}

ProbeResult probe_model(const ModelRef& ref, const Provider& p, const ScanOptions& opt) {
  ProbeResult r;
  r.ref = ref;
  HttpOptions ho;
  ho.bearer = ref.api_key;
  ho.read_timeout_sec = 90;

  json req = {{"model", ref.upstream_id},
              {"messages", json::array({{{"role", "user"},
                                         {"content", "Reply with exactly one word: OK"}}})},
              {"max_tokens", 512},
              {"stream", false}};

  HttpResponse res;
  Failure f = Failure::Network;
  for (int attempt = 0; attempt <= opt.rate_limit_retries; ++attempt) {
    res = http_post_json(p.chat_url, req.dump(), ho);
    f = classify_response(res.status, res.body);
    if (f == Failure::RateLimit && opt.defer_rate_limited) {
      r.rate_limited = true;
      r.retry_after = retry_after(res, 20, opt.max_wait_sec);
      break;
    }
    bool transient = f == Failure::RateLimit || f == Failure::Network || f == Failure::Server;
    if (!transient || attempt == opt.rate_limit_retries) break;
    sleep_sec(retry_after(res, f == Failure::RateLimit ? 31 : 5, opt.max_wait_sec));
  }
  r.status = res.status;
  r.latency = res.elapsed;
  if (res.status != 200 || f != Failure::None) {
    r.failure = failure_name(f);
    r.error = snippet(res.status ? res.body : res.error);
    return r;
  }
  r.passed = true;

  if (opt.probe_tools) {
    json treq = {{"model", ref.upstream_id},
                 {"messages", json::array({{{"role", "user"},
                                            {"content", "What time is it in Paris? Use the tool."}}})},
                 {"tools", json::array({kProbeTool})},
                 {"max_tokens", 512},
                 {"stream", false}};
    for (int attempt = 0; attempt <= opt.rate_limit_retries; ++attempt) {
      HttpResponse tr = http_post_json(p.chat_url, treq.dump(), ho);
      Failure tf = classify_response(tr.status, tr.body);
      if (tf == Failure::RateLimit && attempt < opt.rate_limit_retries) {
        sleep_sec(retry_after(tr, 31, opt.max_wait_sec));
        continue;
      }
      if (tr.status == 200 && tf == Failure::None) {
        json j = json::parse(tr.body, nullptr, false);
        const json& msg = j["choices"][0]["message"];
        r.tools_ok = msg.contains("tool_calls") && msg["tool_calls"].is_array() &&
                     !msg["tool_calls"].empty();
      }
      break;
    }
  }
  return r;
}

namespace {

std::vector<ProbeResult> probe_all(const Catalog& catalog, std::vector<ModelRef> refs,
                                   const ScanOptions& opt) {
  // Kelompokkan per provider supaya batas concurrency per provider terjaga.
  std::map<std::string, std::vector<size_t>> by_provider;
  for (size_t i = 0; i < refs.size(); ++i) by_provider[refs[i].provider].push_back(i);

  std::vector<ProbeResult> results(refs.size());
  std::vector<std::thread> threads;
  for (auto& [pname, idxs] : by_provider) {
    const Provider* p = catalog.find_provider(pname);
    if (!p) continue;
    auto next = std::make_shared<std::atomic<size_t>>(0);
    auto list = std::make_shared<std::vector<size_t>>(idxs);
    for (int w = 0; w < p->concurrency; ++w) {
      threads.emplace_back([&, p, next, list] {
        while (true) {
          size_t k = next->fetch_add(1);
          if (k >= list->size()) break;
          size_t i = (*list)[k];
          ProbeResult r = probe_model(refs[i], *p, opt);
          char lat[32];
          std::snprintf(lat, sizeof(lat), "%5.1fs", r.latency);
          if (r.passed)
            say(opt, "  [ OK ] " + std::string(lat) + "  " + r.ref.name +
                       (r.tools_ok ? "  (tools)" : ""));
          else if (r.rate_limited)
            say(opt, "  [TUNDA] " + std::string(lat) + "  " + r.ref.name +
                       "  -> rate limit, diuji ulang nanti");
          else
            say(opt, "  [GAGAL] " + std::string(lat) + "  " + r.ref.name + "  -> " +
                       (r.status ? "HTTP " + std::to_string(r.status) + " " : "") + r.failure);
          results[i] = std::move(r);
        }
      });
    }
  }
  for (auto& t : threads) t.join();
  return results;
}

// Putaran pertama tanpa menunggu rate limit, lalu model yang tertunda diuji
// ulang sekali lagi (kali ini dengan menunggu Retry-After).
std::vector<ProbeResult> probe_with_retry_pass(const Catalog& catalog,
                                               const std::vector<ModelRef>& refs,
                                               const ScanOptions& opt) {
  ScanOptions first = opt;
  first.defer_rate_limited = true;
  auto results = probe_all(catalog, refs, first);

  std::vector<ModelRef> again;
  std::vector<size_t> where;
  int wait = 0;
  for (size_t i = 0; i < results.size(); ++i) {
    if (results[i].rate_limited) {
      again.push_back(results[i].ref);
      where.push_back(i);
      wait = std::max(wait, results[i].retry_after);
    }
  }
  if (again.empty()) return results;
  wait = std::min(std::max(wait, 5), std::max(1, opt.max_wait_sec));
  say(opt, std::to_string(again.size()) + " model kena rate limit, diuji ulang setelah jeda " +
             std::to_string(wait) + " detik...");
  sleep_sec(wait);
  ScanOptions second = opt;
  second.defer_rate_limited = false;
  auto retry = probe_all(catalog, again, second);
  for (size_t k = 0; k < retry.size(); ++k) results[where[k]] = std::move(retry[k]);
  return results;
}

// Urutan prioritas: model yang bisa tool calling dulu, lalu yang tercepat.
// Provider diselang-seling agar rate limit satu provider tidak menjatuhkan
// beberapa model berurutan.
std::vector<ProbeResult> order_results(std::vector<ProbeResult> passed) {
  std::vector<ProbeResult> ordered;
  for (bool tools : {true, false}) {
    std::map<std::string, std::vector<ProbeResult>> groups;
    for (const auto& r : passed)
      if (r.tools_ok == tools) groups[r.ref.provider].push_back(r);
    std::vector<std::vector<ProbeResult>*> lists;
    for (auto& [_, v] : groups) {
      std::sort(v.begin(), v.end(),
                [](const ProbeResult& a, const ProbeResult& b) { return a.latency < b.latency; });
      lists.push_back(&v);
    }
    std::sort(lists.begin(), lists.end(), [](auto* a, auto* b) {
      return a->front().latency < b->front().latency;
    });
    for (size_t round = 0;; ++round) {
      bool any = false;
      for (auto* l : lists) {
        if (round < l->size()) {
          ordered.push_back((*l)[round]);
          any = true;
        }
      }
      if (!any) break;
    }
  }
  return ordered;
}

void write_report(const Catalog& catalog, const std::vector<ProbeResult>& results) {
  json rep = json::object();
  rep["generated_at"] = timestamp();
  json arr = json::array();
  for (const auto& r : results) {
    arr.push_back({{"model", r.ref.name},
                   {"passed", r.passed},
                   {"status", r.status},
                   {"latency_sec", std::round(r.latency * 100) / 100},
                   {"tools", r.tools_ok},
                   {"failure", r.failure},
                   {"error", r.error}});
  }
  rep["results"] = arr;
  write_file_atomic(catalog.state_dir() / "scan-report.json", rep.dump(2) + "\n");
}

bool wanted(const ScanOptions& opt, const std::string& provider) {
  if (opt.only_providers.empty()) return true;
  return std::find(opt.only_providers.begin(), opt.only_providers.end(), provider) !=
         opt.only_providers.end();
}

}  // namespace

bool is_permanent_failure(const std::string& failure) {
  return failure == failure_name(Failure::Auth) || failure == failure_name(Failure::BadRequest) ||
         failure == failure_name(Failure::Empty);
}

MergeOutcome merge_update(const std::vector<ModelRef>& existing, const std::vector<ProbeResult>& results,
                          const std::set<std::string>& listed, const std::set<std::string>& listed_providers) {
  MergeOutcome out;
  std::map<std::string, const ProbeResult*> by_name;
  for (const auto& r : results) by_name[r.ref.name] = &r;
  std::set<std::string> had;
  for (const auto& m : existing) had.insert(m.name);

  std::vector<ProbeResult> passed;
  for (const auto& r : results)
    if (r.passed) passed.push_back(r);
  std::set<std::string> in_list;
  for (const auto& r : order_results(passed)) {
    if (!in_list.insert(r.ref.name).second) continue;
    out.models.push_back(r.ref);
    if (!had.count(r.ref.name)) out.added.push_back(r.ref.name);
  }

  for (const auto& m : existing) {
    if (in_list.count(m.name)) continue;
    auto it = by_name.find(m.name);
    if (it == by_name.end()) {
      // Tidak diuji kali ini (provider dimatikan atau di luar --provider): biarkan.
      out.models.push_back(m);
      in_list.insert(m.name);
      continue;
    }
    const ProbeResult& r = *it->second;
    if (is_permanent_failure(r.failure)) {
      out.removed.push_back({m.name, r.failure + (r.status ? " (HTTP " + std::to_string(r.status) + ")" : "")});
    } else if (listed_providers.count(m.provider) && !listed.count(m.name)) {
      out.removed.push_back({m.name, "tidak lagi ditawarkan provider (" + r.failure + ")"});
    } else {
      out.models.push_back(m);
      in_list.insert(m.name);
      out.kept_unverified.push_back(m.name);
    }
  }
  return out;
}

namespace {

std::mutex g_update_mu;

// Kunci antar proses: dua `claw` yang jalan bersamaan tidak memperbarui models.json
// berbarengan. Dilepas otomatis saat proses selesai atau mati.
class UpdateLock {
 public:
  explicit UpdateLock(const fs::path& p) {
#ifndef _WIN32
    fd_ = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ >= 0 && flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      ::close(fd_);
      fd_ = -1;
    }
    held_ = fd_ >= 0;
#else
    (void)p;
    held_ = true;
#endif
  }
  ~UpdateLock() {
#ifndef _WIN32
    if (fd_ >= 0) ::close(fd_);
#endif
  }
  bool held() const { return held_; }

 private:
  int fd_ = -1;
  bool held_ = false;
};

void write_update_report(const Catalog& catalog, const UpdateResult& r) {
  json rep = {{"time", timestamp()},
              {"unix", unix_time()},
              {"tested", r.tested},
              {"passed", r.passed},
              {"written", r.written},
              {"error", r.error},
              {"added", r.merge.added},
              {"kept_unverified", r.merge.kept_unverified}};
  json removed = json::array();
  for (const auto& [name, why] : r.merge.removed) removed.push_back({{"model", name}, {"reason", why}});
  rep["removed"] = removed;
  write_file_atomic(catalog.state_dir() / "update-report.json", rep.dump(2) + "\n");
}

}  // namespace

int64_t last_update_time(const Catalog& catalog) {
  try {
    json j = json::parse(read_file(catalog.state_dir() / "last-update.json"));
    return j.value("unix", static_cast<int64_t>(0));
  } catch (...) {
    return 0;
  }
}

UpdateResult run_update(Catalog& catalog, const ScanOptions& opt) {
  UpdateResult r;
  std::unique_lock<std::mutex> lk(g_update_mu, std::try_to_lock);
  std::error_code ec;
  fs::create_directories(catalog.state_dir(), ec);
  UpdateLock plock(catalog.state_dir() / "update.lock");
  if (!lk.owns_lock() || !plock.held()) {
    r.error = "update lain sedang berjalan";
    return r;
  }

  std::vector<ModelRef> existing;
  try {
    existing = catalog.load_models();
  } catch (const std::exception& e) {
    Log::warn(std::string(e.what()) + " (models.json akan dibuat ulang)");
  }
  std::map<std::string, std::string> existing_keys;
  for (const auto& m : existing) existing_keys[m.name] = m.api_key;

  std::vector<ModelRef> refs;
  std::set<std::string> seen, listed, listed_providers;
  for (const auto& p : catalog.providers()) {
    if (!p.enabled || !wanted(opt, p.name)) continue;
    std::string err;
    auto ids = discover_models(p, &err);
    say(opt, "provider " + p.name + ": " + std::to_string(ids.size()) + " kandidat" +
                 (err.empty() ? "" : " (daftar model gagal diambil: " + err + ")"));
    if (err.empty() && !ids.empty()) listed_providers.insert(p.name);
    for (const auto& id : ids) {
      ModelRef m;
      m.provider = p.name;
      m.upstream_id = id;
      m.name = p.name + "/" + id;
      listed.insert(m.name);
      auto it = existing_keys.find(m.name);
      m.api_key = it != existing_keys.end() ? it->second : p.api_key;
      if (seen.insert(m.name).second) refs.push_back(m);
    }
  }
  // Model yang ditambahkan manual di models.json ikut diuji.
  for (const auto& m : existing)
    if (wanted(opt, m.provider) && seen.insert(m.name).second) refs.push_back(m);

  say(opt, "menguji " + std::to_string(refs.size()) + " model...");
  auto results = probe_with_retry_pass(catalog, refs, opt);
  r.tested = static_cast<int>(results.size());
  for (const auto& x : results)
    if (x.passed) ++r.passed;
  write_report(catalog, results);

  if (r.passed == 0) {
    r.error = "tidak ada model yang lolos uji (offline?), models.json tidak diubah";
    write_update_report(catalog, r);
    return r;
  }

  r.merge = merge_update(existing, results, listed, listed_providers);
  catalog.save_models(r.merge.models);
  r.written = true;
  // Hasil uji baru lebih akurat daripada cooldown lama milik router.
  fs::remove(catalog.state_dir() / "router-state.json", ec);
  write_file_atomic(catalog.state_dir() / "last-update.json",
                    json({{"unix", unix_time()}, {"time", timestamp()}}).dump() + "\n");
  write_update_report(catalog, r);
  (opt.quiet ? Log::note : Log::info)("update model: " + std::to_string(r.passed) + "/" + std::to_string(r.tested) + " lolos, " +
            std::to_string(r.merge.added.size()) + " baru, " + std::to_string(r.merge.removed.size()) +
            " dibuang, " + std::to_string(r.merge.models.size()) + " di models.json");
  return r;
}

int run_scan(Catalog& catalog, const ScanOptions& opt) {
  UpdateResult r = run_update(catalog, opt);
  print_line("");
  if (!r.written) {
    print_line("scan: " + r.error);
    return 0;
  }
  for (const auto& n : r.merge.added) print_line("  + " + n);
  for (const auto& [n, why] : r.merge.removed) print_line("  - " + n + "  (" + why + ")");
  if (!r.merge.kept_unverified.empty())
    print_line(std::to_string(r.merge.kept_unverified.size()) +
               " model gagal sementara (rate limit/jaringan), tetap disimpan");
  print_line(std::to_string(r.passed) + " dari " + std::to_string(r.tested) + " model lolos (HTTP 200); " +
             std::to_string(r.merge.models.size()) + " model disimpan ke " + catalog.models_path().string());
  return r.passed;
}

int run_check(Catalog& catalog, const ScanOptions& opt, bool prune) {
  auto models = catalog.load_models();
  std::vector<ModelRef> targets;
  for (const auto& m : models)
    if (wanted(opt, m.provider)) targets.push_back(m);
  say(opt, "menguji ulang " + std::to_string(targets.size()) + " model dari models.json...");
  auto results = probe_with_retry_pass(catalog, targets, opt);

  int failed = 0;
  std::map<std::string, std::string> permanent;
  for (const auto& r : results)
    if (!r.passed) {
      ++failed;
      if (is_permanent_failure(r.failure)) permanent[r.ref.name] = r.failure;
    }
  if (prune && !permanent.empty()) {
    std::vector<ModelRef> kept;
    for (const auto& m : models)
      if (!permanent.count(m.name)) kept.push_back(m);
    catalog.save_models(kept);
    for (const auto& [n, why] : permanent) say(opt, "  - " + n + "  (" + why + ")");
    say(opt, std::to_string(permanent.size()) + " model rusak permanen dihapus dari models.json");
  }
  if (prune && failed > static_cast<int>(permanent.size()))
    say(opt, std::to_string(failed - static_cast<int>(permanent.size())) +
                 " model gagal sementara (rate limit/jaringan), tetap disimpan");
  say(opt, std::to_string(results.size() - failed) + "/" + std::to_string(results.size()) +
               " model menjawab HTTP 200");
  return failed;
}

}  // namespace claw
