#include "router.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace claw {

const char* failure_name(Failure f) {
  switch (f) {
    case Failure::None: return "ok";
    case Failure::RateLimit: return "rate-limit";
    case Failure::Quota: return "token/kuota habis";
    case Failure::Auth: return "auth ditolak";
    case Failure::Context: return "konteks terlalu panjang";
    case Failure::Server: return "server error";
    case Failure::Network: return "jaringan gagal";
    case Failure::BadRequest: return "request ditolak";
    case Failure::Empty: return "jawaban kosong";
  }
  return "?";
}

namespace {

bool has_any(const std::string& text, std::initializer_list<const char*> needles) {
  std::string low = to_lower(text);
  for (const char* n : needles)
    if (low.find(n) != std::string::npos) return true;
  return false;
}

bool looks_quota(const std::string& body) {
  return has_any(body, {"quota", "insufficient", "credit", "billing", "exhausted", "out of tokens",
                        "token limit reached", "daily limit", "monthly limit", "limit reached",
                        "exceeded your", "usage limit", "payment required", "balance",
                        "free tier", "upgrade your plan", "tokens per day", "requests per day"});
}

bool looks_context(const std::string& body) {
  return has_any(body, {"context length", "context_length", "context window", "maximum context",
                        "too many tokens", "prompt is too long", "reduce the length",
                        "input is too long", "too long for", "exceeds the maximum",
                        "maximum input", "max_input_tokens", "request too large",
                        "input tokens exceed", "prompt too long"});
}

bool looks_rate_limit(const std::string& body) {
  return has_any(body, {"rate limit", "rate_limit", "too many requests", "slow down",
                        "requests per minute", "try again in"});
}

bool non_empty_content(const json& content) {
  if (content.is_string()) return !trim(content.get<std::string>()).empty();
  if (content.is_array()) {
    for (const auto& part : content) {
      if (part.is_string() && !part.get<std::string>().empty()) return true;
      if (part.is_object() && part.contains("text") && part["text"].is_string() &&
          !part["text"].get<std::string>().empty())
        return true;
    }
  }
  return false;
}

std::string error_text(const json& j) {
  if (!j.is_object() || !j.contains("error")) return "";
  const json& e = j["error"];
  if (e.is_string()) return e.get<std::string>();
  return e.dump();
}

}  // namespace

Failure classify_response(int status, const std::string& body) {
  if (status == 0) return Failure::Network;
  if (status == 200) {
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return Failure::Server;
    std::string err = error_text(j);
    if (!err.empty() && !j.contains("choices")) {
      if (looks_context(err)) return Failure::Context;
      if (looks_quota(err)) return Failure::Quota;
      if (looks_rate_limit(err)) return Failure::RateLimit;
      return Failure::Server;
    }
    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
      return Failure::Empty;
    const json& c0 = j["choices"][0];
    if (!c0.is_object() || !c0.contains("message") || !c0["message"].is_object())
      return Failure::Empty;
    const json& msg = c0["message"];
    bool has_tools = msg.contains("tool_calls") && msg["tool_calls"].is_array() &&
                     !msg["tool_calls"].empty();
    bool has_content = msg.contains("content") && non_empty_content(msg["content"]);
    if (!has_tools && !has_content) {
      // finish_reason "length" tanpa isi: token keluaran habis untuk model ini.
      std::string fr = c0.value("finish_reason", "");
      if (fr == "length") return Failure::Quota;
      return Failure::Empty;
    }
    return Failure::None;
  }
  if (status == 402) return Failure::Quota;
  if (status == 429) return looks_quota(body) ? Failure::Quota : Failure::RateLimit;
  if (status == 401 || status == 403) return looks_quota(body) ? Failure::Quota : Failure::Auth;
  if (status == 413) return Failure::Context;
  if (status == 408 || status >= 500) {
    if (looks_quota(body)) return Failure::Quota;
    if (looks_rate_limit(body)) return Failure::RateLimit;
    return Failure::Server;
  }
  if (status >= 400) {
    if (looks_context(body)) return Failure::Context;
    if (looks_quota(body)) return Failure::Quota;
    if (looks_rate_limit(body)) return Failure::RateLimit;
    return Failure::BadRequest;
  }
  return Failure::Server;
}

json sanitize_request(const json& req, const std::string& upstream_id, int max_tokens_cap) {
  static const char* kPassthrough[] = {"temperature",     "top_p", "stop",
                                       "response_format", "seed",  "presence_penalty",
                                       "frequency_penalty"};
  json out = json::object();
  out["model"] = upstream_id;
  for (const char* k : kPassthrough)
    if (req.contains(k) && !req[k].is_null()) out[k] = req[k];

  int max_tokens = 0;
  for (const char* k : {"max_tokens", "max_completion_tokens"})
    if (req.contains(k) && req[k].is_number_integer()) max_tokens = req[k].get<int>();
  if (max_tokens > 0) out["max_tokens"] = std::min(max_tokens, max_tokens_cap);

  if (req.contains("tools") && req["tools"].is_array() && !req["tools"].empty()) {
    out["tools"] = req["tools"];
    if (req.contains("tool_choice") && !req["tool_choice"].is_null())
      out["tool_choice"] = req["tool_choice"];
  }

  json messages = json::array();
  if (req.contains("messages") && req["messages"].is_array()) {
    for (const auto& m : req["messages"]) {
      if (!m.is_object()) continue;
      json nm = json::object();
      std::string role = m.value("role", "user");
      if (role == "developer") role = "system";
      nm["role"] = role;

      json content = m.contains("content") ? m["content"] : json(nullptr);
      if (content.is_array()) {
        // Gabungkan jika semua bagian berupa teks; banyak provider gratis
        // belum menerima content berbentuk array.
        bool all_text = true;
        std::string joined;
        for (const auto& part : content) {
          if (part.is_object() && part.value("type", "") == "text" && part.contains("text") &&
              part["text"].is_string()) {
            if (!joined.empty()) joined += "\n";
            joined += part["text"].get<std::string>();
          } else {
            all_text = false;
            break;
          }
        }
        if (all_text) content = joined;
      }
      if (content.is_null() && !(role == "assistant" && m.contains("tool_calls"))) content = "";
      nm["content"] = content;

      if (m.contains("name") && m["name"].is_string()) nm["name"] = m["name"];
      if (m.contains("tool_call_id") && m["tool_call_id"].is_string())
        nm["tool_call_id"] = m["tool_call_id"];
      if (m.contains("tool_calls") && m["tool_calls"].is_array() && !m["tool_calls"].empty()) {
        json calls = json::array();
        for (const auto& tc : m["tool_calls"]) {
          if (!tc.is_object()) continue;
          json fn = tc.value("function", json::object());
          json args = fn.contains("arguments") ? fn["arguments"] : json("{}");
          if (!args.is_string()) args = args.dump();
          calls.push_back({{"id", tc.value("id", "call_" + random_hex(6))},
                           {"type", "function"},
                           {"function", {{"name", fn.value("name", "")}, {"arguments", args}}}});
        }
        nm["tool_calls"] = calls;
      }
      messages.push_back(std::move(nm));
    }
  }
  out["messages"] = messages;
  out["stream"] = false;
  return out;
}

bool trim_history(json& messages) {
  if (!messages.is_array()) return false;
  std::vector<size_t> convo;
  for (size_t i = 0; i < messages.size(); ++i)
    if (messages[i].value("role", "") != "system") convo.push_back(i);
  if (convo.size() <= 1) return false;

  size_t drop = (convo.size() + 1) / 2;
  if (drop >= convo.size()) drop = convo.size() - 1;
  // Setelah dipotong, percakapan harus diawali pesan user agar pasangan
  // tool_call/tool result tidak terputus.
  while (drop < convo.size() - 1 && messages[convo[drop]].value("role", "") != "user") ++drop;

  json kept = json::array();
  for (size_t i = 0; i < messages.size(); ++i) {
    bool is_convo = messages[i].value("role", "") != "system";
    if (is_convo) {
      auto pos = std::find(convo.begin(), convo.end(), i) - convo.begin();
      if (static_cast<size_t>(pos) < drop) continue;
    }
    kept.push_back(messages[i]);
  }
  bool changed = kept.size() != messages.size();
  messages = std::move(kept);
  return changed;
}

Router::Router(const Catalog& catalog, const std::vector<ModelRef>& models, RouterOptions opt)
    : opt_(opt) {
  for (const auto& m : models) {
    const Provider* p = catalog.find_provider(m.provider);
    if (!p) continue;
    Entry e;
    e.ref = m;
    e.chat_url = p->chat_url;
    entries_.push_back(std::move(e));
  }
  load_state();
}

void Router::load_state() {
  if (opt_.state_file.empty()) return;
  std::error_code ec;
  if (!fs::exists(opt_.state_file, ec)) return;
  try {
    json st = json::parse(read_file(opt_.state_file));
    double mono_now = now_seconds();
    int64_t wall_now = unix_time();
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < entries_.size(); ++i) {
      Entry& e = entries_[i];
      if (st.contains("models") && st["models"].contains(e.ref.name)) {
        const json& m = st["models"][e.ref.name];
        int64_t until = m.value("cooldown_until_unix", static_cast<int64_t>(0));
        if (until > wall_now) {
          e.cooldown_until = mono_now + static_cast<double>(until - wall_now);
          std::string lf = m.value("last_failure", "");
          for (Failure f : {Failure::RateLimit, Failure::Quota, Failure::Auth, Failure::Server,
                            Failure::Network, Failure::BadRequest, Failure::Empty})
            if (lf == failure_name(f)) e.last_failure = f;
        }
      }
      if (st.value("current", "") == e.ref.name) current_ = i;
    }
  } catch (const std::exception& ex) {
    Log::warn("state router diabaikan (" + std::string(ex.what()) + ")");
  }
}

void Router::save_state() const {
  if (opt_.state_file.empty()) return;
  json st = json::object();
  {
    std::lock_guard<std::mutex> lk(mu_);
    double mono_now = now_seconds();
    int64_t wall_now = unix_time();
    json models = json::object();
    for (const auto& e : entries_) {
      if (e.cooldown_until <= mono_now) continue;
      models[e.ref.name] = {
          {"cooldown_until_unix", wall_now + static_cast<int64_t>(e.cooldown_until - mono_now)},
          {"last_failure", failure_name(e.last_failure)}};
    }
    st["current"] = entries_.empty() ? "" : entries_[current_].ref.name;
    st["models"] = models;
  }
  try {
    write_file_atomic(opt_.state_file, st.dump(2) + "\n");
  } catch (const std::exception& ex) {
    Log::warn("gagal menyimpan state router: " + std::string(ex.what()));
  }
}

std::vector<std::string> Router::names() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<std::string> out;
  for (const auto& e : entries_) out.push_back(e.ref.name);
  return out;
}

std::string Router::current() const {
  std::lock_guard<std::mutex> lk(mu_);
  return entries_.empty() ? "" : entries_[current_].ref.name;
}

bool Router::set_current(const std::string& name) {
  bool found = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].ref.name == name) {
        current_ = i;
        entries_[i].cooldown_until = 0;
        found = true;
        break;
      }
    }
  }
  if (found) save_state();
  return found;
}

std::vector<ModelStatus> Router::status() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<ModelStatus> out;
  double now = now_seconds();
  for (size_t i = 0; i < entries_.size(); ++i) {
    const auto& e = entries_[i];
    ModelStatus s;
    s.name = e.ref.name;
    s.current = i == current_;
    s.cooldown_left = std::max(0.0, e.cooldown_until - now);
    s.last_failure = e.last_failure == Failure::None ? "" : failure_name(e.last_failure);
    s.ok = e.ok;
    s.failed = e.failed;
    s.tokens = e.tokens;
    out.push_back(s);
  }
  return out;
}

std::vector<size_t> Router::plan_order(size_t start, bool& has_cooling) const {
  std::vector<size_t> ready, cooling;
  double now = now_seconds();
  for (size_t k = 0; k < entries_.size(); ++k) {
    size_t i = (start + k) % entries_.size();
    if (entries_[i].cooldown_until > now)
      cooling.push_back(i);
    else
      ready.push_back(i);
  }
  // Model yang sedang cooldown tetap dicoba paling akhir, mulai dari yang
  // paling cepat pulih, supaya request tidak langsung gagal.
  std::sort(cooling.begin(), cooling.end(), [&](size_t a, size_t b) {
    return entries_[a].cooldown_until < entries_[b].cooldown_until;
  });
  has_cooling = !cooling.empty();
  ready.insert(ready.end(), cooling.begin(), cooling.end());
  return ready;
}

int Router::cooldown_for(Failure f, const HttpResponse& res) const {
  switch (f) {
    case Failure::RateLimit: {
      auto it = res.headers.find("retry-after");
      if (it != res.headers.end()) {
        try {
          int s = std::stoi(it->second);
          return std::clamp(s, 1, 900);
        } catch (...) {
        }
      }
      return opt_.rate_limit_cooldown_sec;
    }
    case Failure::Quota: return opt_.quota_cooldown_sec;
    case Failure::Auth: return opt_.auth_cooldown_sec;
    case Failure::Server:
    case Failure::Network: return opt_.server_cooldown_sec;
    case Failure::BadRequest: return opt_.bad_request_cooldown_sec;
    case Failure::Empty: return opt_.rate_limit_cooldown_sec;
    case Failure::Context:
    case Failure::None: return 0;
  }
  return 0;
}

void Router::mark_failure(size_t idx, Failure f, const HttpResponse& res) {
  std::string broken_name;
  {
    std::lock_guard<std::mutex> lk(mu_);
    Entry& e = entries_[idx];
    e.failed++;
    e.last_failure = f;
    int cd = cooldown_for(f, res);
    if (cd > 0) e.cooldown_until = now_seconds() + cd;
    // Token/kuota habis atau key ditolak: model ini butuh diganti, bukan sekadar
    // ditunggu. Beri tahu handler auto-repair sekali per kejadian.
    if (f == Failure::Quota || f == Failure::Auth) broken_name = e.ref.name;
  }
  save_state();
  if (!broken_name.empty() && on_broken) on_broken(broken_name, failure_name(f));
}

size_t Router::usable() const {
  std::lock_guard<std::mutex> lk(mu_);
  double t = now_seconds();
  size_t n = 0;
  for (const auto& e : entries_)
    if (e.cooldown_until <= t) ++n;
  return n;
}

int Router::add_models(const Catalog& catalog, const std::vector<ModelRef>& models) {
  std::lock_guard<std::mutex> lk(mu_);
  std::set<std::string> have;
  for (const auto& e : entries_) have.insert(e.ref.name);
  int added = 0;
  for (const auto& m : models) {
    if (have.count(m.name)) continue;
    const Provider* p = catalog.find_provider(m.provider);
    if (!p) continue;
    Entry e;
    e.ref = m;
    e.chat_url = p->chat_url;
    entries_.push_back(std::move(e));
    have.insert(m.name);
    ++added;
  }
  return added;
}

void Router::mark_success(size_t idx, const json& body, const std::string& prior_reason) {
  std::string from, to;
  {
    std::lock_guard<std::mutex> lk(mu_);
    Entry& e = entries_[idx];
    e.ok++;
    e.cooldown_until = 0;
    e.last_failure = Failure::None;
    if (body.contains("usage") && body["usage"].is_object()) {
      const json& u = body["usage"];
      if (u.contains("total_tokens") && u["total_tokens"].is_number())
        e.tokens += u["total_tokens"].get<int64_t>();
    }
    if (idx != current_) {
      from = entries_[current_].ref.name;
      to = e.ref.name;
      current_ = idx;
    }
  }
  save_state();
  if (!to.empty()) {
    Log::info("auto-switch: " + from + " -> " + to +
              (prior_reason.empty() ? "" : " (" + prior_reason + ")"));
    if (on_switch) on_switch(from, to, prior_reason);
  }
}

namespace {

void normalize_response(json& body, const std::string& client_model, const std::string& used) {
  body["model"] = client_model;
  body["x_claw_model"] = used;
  if (!body.contains("id")) body["id"] = "chatcmpl-" + random_hex(12);
  if (!body.contains("object")) body["object"] = "chat.completion";
  if (!body.contains("created")) body["created"] = unix_time();
  for (auto& ch : body["choices"]) {
    if (!ch.is_object() || !ch.contains("message")) continue;
    json& msg = ch["message"];
    if (!msg.contains("role")) msg["role"] = "assistant";
    if (msg.contains("reasoning") && !msg.contains("reasoning_content"))
      msg["reasoning_content"] = msg["reasoning"];
    msg.erase("reasoning");
    bool has_tools = msg.contains("tool_calls") && msg["tool_calls"].is_array() &&
                     !msg["tool_calls"].empty();
    if (msg.contains("tool_calls") && !has_tools) msg.erase("tool_calls");
    if (has_tools) {
      for (auto& tc : msg["tool_calls"]) {
        if (!tc.contains("id") || !tc["id"].is_string() || tc["id"].get<std::string>().empty())
          tc["id"] = "call_" + random_hex(8);
        tc["type"] = "function";
        if (tc.contains("function") && tc["function"].contains("arguments") &&
            !tc["function"]["arguments"].is_string())
          tc["function"]["arguments"] = tc["function"]["arguments"].dump();
      }
      if (ch.value("finish_reason", "") != "tool_calls") ch["finish_reason"] = "tool_calls";
    }
    if (!msg.contains("content") || msg["content"].is_null()) msg["content"] = has_tools ? json(nullptr) : json("");
    if (!ch.contains("finish_reason") || ch["finish_reason"].is_null()) ch["finish_reason"] = "stop";
  }
}

}  // namespace

ChatResult Router::complete(const json& request, const std::string& requested_model) {
  ChatResult result;
  std::string client_model = requested_model.empty() ? "auto" : requested_model;
  if (requested_model != "auto" && !requested_model.empty()) set_current(requested_model);

  size_t n;
  size_t start;
  {
    std::lock_guard<std::mutex> lk(mu_);
    n = entries_.size();
    start = current_;
  }
  if (n == 0) {
    result.status = 503;
    result.body = {{"error",
                    {{"message", "pool model kosong: jalankan `claw scan` dulu"},
                     {"type", "server_error"}}}};
    return result;
  }

  json messages = request.contains("messages") ? request["messages"] : json::array();
  double deadline = now_seconds() + opt_.total_budget_sec;
  std::string first_failure;
  int trims = 0;
  bool all_context = true;

  while (true) {
    bool has_cooling = false;
    std::vector<size_t> order;
    {
      std::lock_guard<std::mutex> lk(mu_);
      order = plan_order(start, has_cooling);
    }
    bool context_seen = false;
    json req = request;
    req["messages"] = messages;

    for (size_t idx : order) {
      double remaining = deadline - now_seconds();
      if (remaining < 3) break;
      Entry snapshot;
      {
        std::lock_guard<std::mutex> lk(mu_);
        snapshot = entries_[idx];
      }
      json body = sanitize_request(req, snapshot.ref.upstream_id, opt_.max_tokens_cap);
      if (!opt_.dump_dir.empty()) {
        // Untuk debugging: simpan request klien dan request ke upstream.
        try {
          write_file_atomic(fs::path(opt_.dump_dir) / "last-client-request.json", request.dump(2));
          write_file_atomic(fs::path(opt_.dump_dir) / "last-upstream-request.json", body.dump(2));
        } catch (...) {
        }
      }
      HttpOptions ho;
      ho.bearer = snapshot.ref.api_key;
      ho.read_timeout_sec = std::max(5, std::min(opt_.read_timeout_sec, static_cast<int>(remaining)));
      HttpResponse res = http_post_json(snapshot.chat_url, body.dump(), ho);
      Failure f = classify_response(res.status, res.body);

      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.1fs", res.elapsed);
      std::string line = snapshot.ref.name + " -> " +
                         (res.status ? "HTTP " + std::to_string(res.status) : "network: " + res.error) +
                         " [" + failure_name(f) + ", " + buf + "]";
      result.attempts.push_back(line);
      Log::debug("attempt " + line);

      if (f == Failure::None) {
        json out = json::parse(res.body);
        normalize_response(out, client_model, snapshot.ref.name);
        mark_success(idx, out, first_failure);
        result.status = 200;
        result.body = std::move(out);
        result.model = snapshot.ref.name;
        return result;
      }
      if (f != Failure::Context) all_context = false;
      if (first_failure.empty())
        first_failure = snapshot.ref.name + ": " + failure_name(f) +
                        (res.status ? " HTTP " + std::to_string(res.status) : "");
      std::string detail_snip = res.status ? res.body : res.error;
      for (auto& ch : detail_snip)
        if (ch == '\n' || ch == '\r') ch = ' ';
      if (detail_snip.size() > 300) detail_snip = detail_snip.substr(0, 300) + "...";
      Log::warn("model " + snapshot.ref.name + " gagal (" + failure_name(f) +
                (res.status ? ", HTTP " + std::to_string(res.status) : "") +
                "), mencoba model berikutnya: " + detail_snip);
      mark_failure(idx, f, res);
      if (f == Failure::Context) context_seen = true;
    }

    if (context_seen && trims < opt_.max_trims && deadline - now_seconds() > 5 &&
        trim_history(messages)) {
      ++trims;
      Log::warn("riwayat percakapan dipangkas (" + std::to_string(trims) +
                "x) karena melebihi konteks model");
      std::lock_guard<std::mutex> lk(mu_);
      start = current_;
      continue;
    }
    break;
  }

  std::string detail;
  for (const auto& a : result.attempts) detail += "\n  " + a;
  if (all_context && !result.attempts.empty()) {
    result.status = 400;
    result.body = {{"error",
                    {{"message", "context length exceeded on every model in the pool" + detail},
                     {"type", "invalid_request_error"},
                     {"code", "context_length_exceeded"}}}};
  } else {
    result.status = 503;
    result.body = {{"error",
                    {{"message", "semua model di pool gagal atau kuotanya habis" + detail},
                     {"type", "server_error"},
                     {"code", "pool_exhausted"}}}};
  }
  Log::error("semua model gagal:" + detail);
  return result;
}

Router::StreamResult Router::stream(const json& request, const std::string& requested_model,
                                    const DeltaSink& on_delta,
                                    const std::function<void()>& on_first_content) {
  StreamResult result;
  std::string client_model = requested_model.empty() ? "auto" : requested_model;
  if (requested_model != "auto" && !requested_model.empty()) set_current(requested_model);

  size_t n, start;
  {
    std::lock_guard<std::mutex> lk(mu_);
    n = entries_.size();
    start = current_;
  }
  if (n == 0) {
    result.status = 503;
    result.error = {{"message", "pool model kosong: jalankan `claw scan` dulu"}, {"type", "server_error"}};
    return result;
  }

  json messages = request.contains("messages") ? request["messages"] : json::array();
  double deadline = now_seconds() + opt_.total_budget_sec;
  std::string first_failure;
  int trims = 0;
  bool all_context = true;

  while (true) {
    bool has_cooling = false;
    std::vector<size_t> order;
    {
      std::lock_guard<std::mutex> lk(mu_);
      order = plan_order(start, has_cooling);
    }
    bool context_seen = false;
    json req = request;
    req["messages"] = messages;

    for (size_t idx : order) {
      double remaining = deadline - now_seconds();
      if (remaining < 3) break;
      Entry snapshot;
      {
        std::lock_guard<std::mutex> lk(mu_);
        snapshot = entries_[idx];
      }
      json body = sanitize_request(req, snapshot.ref.upstream_id, opt_.max_tokens_cap);
      body["stream"] = true;

      std::string sse_buf, content;
      std::map<int, json> tool_acc;
      json usage;
      bool got_content = false, first_sent = false, saw_data = false;

      auto handle_event = [&](const std::string& data) {
        if (data == "[DONE]") return;
        json j = json::parse(data, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return;
        saw_data = true;
        if (j.contains("usage") && j["usage"].is_object()) usage = j["usage"];
        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;
        const json& ch = j["choices"][0];
        if (!ch.contains("delta") || !ch["delta"].is_object()) return;
        const json& delta = ch["delta"];
        if (delta.contains("content") && delta["content"].is_string()) {
          std::string piece = delta["content"].get<std::string>();
          if (!piece.empty()) {
            if (!first_sent) {
              first_sent = true;
              if (on_first_content) on_first_content();
            }
            content += piece;
            got_content = true;
            if (on_delta) on_delta(piece);
          }
        }
        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
          for (const auto& tc : delta["tool_calls"]) {
            int ix = tc.value("index", 0);
            json& acc = tool_acc[ix];
            if (acc.is_null()) acc = json{{"id", ""}, {"name", ""}, {"args", ""}};
            if (tc.contains("id") && tc["id"].is_string() && !tc["id"].get<std::string>().empty())
              acc["id"] = tc["id"];
            if (tc.contains("function") && tc["function"].is_object()) {
              const json& f = tc["function"];
              if (f.contains("name") && f["name"].is_string() && !f["name"].get<std::string>().empty())
                acc["name"] = f["name"];
              if (f.contains("arguments") && f["arguments"].is_string())
                acc["args"] = acc["args"].get<std::string>() + f["arguments"].get<std::string>();
            }
          }
        }
      };
      auto sink = [&](const char* data, size_t len) -> bool {
        sse_buf.append(data, len);
        size_t pos;
        while ((pos = sse_buf.find('\n')) != std::string::npos) {
          std::string ln = sse_buf.substr(0, pos);
          sse_buf.erase(0, pos + 1);
          if (!ln.empty() && ln.back() == '\r') ln.pop_back();
          if (ln.rfind("data:", 0) == 0) {
            std::string d = ln.substr(5);
            if (!d.empty() && d.front() == ' ') d.erase(0, 1);
            handle_event(d);
          }
        }
        return true;
      };

      HttpOptions ho;
      ho.bearer = snapshot.ref.api_key;
      ho.read_timeout_sec = std::max(5, std::min(opt_.read_timeout_sec, static_cast<int>(remaining)));
      HttpResponse res = http_post_stream(snapshot.chat_url, body.dump(), ho, sink);
      if (!sse_buf.empty()) {
        std::string ln = sse_buf;
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        if (ln.rfind("data:", 0) == 0) {
          std::string d = ln.substr(5);
          if (!d.empty() && d.front() == ' ') d.erase(0, 1);
          handle_event(d);
        }
      }

      json tool_calls = json::array();
      for (auto& [ix, acc] : tool_acc) {
        (void)ix;
        std::string name = acc.value("name", "");
        if (name.empty()) continue;
        std::string id = acc.value("id", "");
        if (id.empty()) id = "call_" + random_hex(8);
        std::string argstr = acc.value("args", "");
        if (argstr.empty()) argstr = "{}";
        tool_calls.push_back(
            {{"id", id}, {"type", "function"}, {"function", {{"name", name}, {"arguments", argstr}}}});
      }

      bool ok = res.status == 200 && (got_content || !tool_calls.empty());
      Failure f = ok ? Failure::None : classify_response(res.status, saw_data ? std::string("{}") : res.body);
      char tb[64];
      std::snprintf(tb, sizeof(tb), "%.1fs", res.elapsed);
      result.attempts.push_back(
          snapshot.ref.name + " -> " +
          (res.status ? "HTTP " + std::to_string(res.status) : "network: " + res.error) + " [" +
          failure_name(f) + ", " + tb + "]");

      if (ok) {
        json bu = json::object();
        if (usage.is_object()) bu["usage"] = usage;
        mark_success(idx, bu, first_failure);
        result.status = 200;
        result.model = snapshot.ref.name;
        result.content = content;
        result.tool_calls = tool_calls;
        return result;
      }
      if (f != Failure::Context) all_context = false;
      if (first_failure.empty())
        first_failure = snapshot.ref.name + ": " + failure_name(f) +
                        (res.status ? " HTTP " + std::to_string(res.status) : "");
      Log::warn("stream: model " + snapshot.ref.name + " gagal (" + failure_name(f) + ")");
      mark_failure(idx, f, res);
      if (f == Failure::Context) context_seen = true;
    }

    if (context_seen && trims < opt_.max_trims && deadline - now_seconds() > 5 && trim_history(messages)) {
      ++trims;
      std::lock_guard<std::mutex> lk(mu_);
      start = current_;
      continue;
    }
    break;
  }

  std::string detail;
  for (const auto& a : result.attempts) detail += "\n  " + a;
  result.status = (all_context && !result.attempts.empty()) ? 400 : 503;
  result.error = {{"message", (all_context ? "context length exceeded on every model"
                                           : "semua model di pool gagal atau kuotanya habis") + detail},
                  {"type", all_context ? "invalid_request_error" : "server_error"},
                  {"code", all_context ? "context_length_exceeded" : "pool_exhausted"}};
  return result;
}

}  // namespace claw
