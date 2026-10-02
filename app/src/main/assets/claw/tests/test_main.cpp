// Tes unit + integrasi untuk claw. Upstream disimulasikan dengan server HTTP
// lokal, sehingga skenario kuota habis, rate limit, konteks terlalu panjang,
// dan server mati bisa diuji tanpa bergantung pada provider asli.
#include <atomic>
#include <iostream>
#include <map>
#include <set>
#include <fstream>
#include <unistd.h>
#include <iterator>
#include <sstream>

#include "catalog.hpp"
#include "httplib.h"
#include "activity.hpp"
#include "agents.hpp"
#include "browse.hpp"
#include "fstools.hpp"
#include "language.hpp"
#include "mdstream.hpp"
#include "launcher.hpp"
#include "policy.hpp"
#include "route.hpp"
#include "proxy.hpp"
#include "router.hpp"
#include "scanner.hpp"
#include "skills.hpp"

using namespace claw;

static int g_failed = 0;
static int g_passed = 0;

#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (cond) {                                                                       \
      ++g_passed;                                                                     \
    } else {                                                                          \
      ++g_failed;                                                                     \
      std::cerr << "  GAGAL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";   \
    }                                                                                 \
  } while (0)

#define TEST(name) static void name()
#define RUN(name)                                  \
  do {                                             \
    int before = g_failed;                         \
    name();                                        \
    std::cout << (g_failed == before ? "[ OK ] " : "[FAIL] ") << #name << "\n"; \
  } while (0)

// ---------------------------------------------------------------- mock upstream

struct MockUpstream {
  httplib::Server srv;
  std::thread th;
  int port = 0;
  std::mutex mu;
  std::map<std::string, int> hits;
  std::map<std::string, size_t> last_msg_count;
  std::atomic<int> flaky_calls{0};
  std::string last_auth;

  MockUpstream() {
    srv.Get("/v1/models", [](const httplib::Request&, httplib::Response& res) {
      json d = {{"data",
                 json::array({{{"id", "ok-a"}, {"free", true}},
                              {{"id", "ok-b"}, {"free", true}},
                              {{"id", "paid-x"}, {"free", false}},
                              {{"id", "tts-voice"}, {"free", true}},
                              {{"id", "down"}, {"free", true}},
                              {{"id", "rate"}, {"free", true}}})}};
      res.set_content(d.dump(), "application/json");
    });
    srv.Post("/v1/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
      json b = json::parse(req.body);
      std::string model = b.value("model", "");
      size_t n = b["messages"].size();
      {
        std::lock_guard<std::mutex> lk(mu);
        hits[model]++;
        last_msg_count[model] = n;
        last_auth = req.get_header_value("Authorization");
      }
      auto ok = [&](const std::string& text) {
        json r = {{"id", "x"},
                  {"object", "chat.completion"},
                  {"created", 1},
                  {"model", model},
                  {"choices", json::array({{{"index", 0},
                                            {"message", {{"role", "assistant"}, {"content", text}}},
                                            {"finish_reason", "stop"}}})},
                  {"usage", {{"prompt_tokens", 5}, {"completion_tokens", 2}, {"total_tokens", 7}}}};
        res.set_content(r.dump(), "application/json");
      };
      if (model == "quota") {
        res.status = 429;
        res.set_content(R"({"error":{"message":"You exceeded your current quota"}})", "application/json");
      } else if (model == "payment") {
        res.status = 402;
        res.set_content(R"({"error":"insufficient credits"})", "application/json");
      } else if (model == "rate") {
        res.status = 429;
        res.set_header("Retry-After", "1");
        res.set_content(R"({"error":{"message":"rate limit"}})", "application/json");
      } else if (model == "gone") {
        res.status = 404;
        res.set_content(R"({"error":{"message":"model not found"}})", "application/json");
      } else if (model == "down") {
        res.status = 503;
        res.set_content("upstream unavailable", "text/plain");
      } else if (model == "empty") {
        json r = {{"choices", json::array({{{"index", 0},
                                            {"message", {{"role", "assistant"}, {"content", ""}}},
                                            {"finish_reason", "stop"}}})}};
        res.set_content(r.dump(), "application/json");
      } else if (model == "ctx") {
        if (n > 3) {
          res.status = 400;
          res.set_content(R"({"error":{"message":"This model's maximum context length is 10 tokens"}})",
                          "application/json");
        } else {
          ok("short enough: " + std::to_string(n));
        }
      } else if (model == "tools") {
        json r = {{"choices",
                   json::array({{{"index", 0},
                                 {"message",
                                  {{"role", "assistant"},
                                   {"content", nullptr},
                                   {"tool_calls",
                                    json::array({{{"function",
                                                   {{"name", "get_time"},
                                                    {"arguments", {{"city", "Paris"}}}}}}})}}},
                                 {"finish_reason", "stop"}}})}};
        res.set_content(r.dump(), "application/json");
      } else if (model == "flaky") {
        if (flaky_calls++ == 0) {
          res.status = 500;
          res.set_content("boom", "text/plain");
        } else {
          ok("recovered");
        }
      } else if (model == "echo") {
        ok("messages=" + std::to_string(n));
      } else {
        ok("hello from " + model);
      }
    });
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
    srv.wait_until_ready();
  }
  ~MockUpstream() {
    srv.stop();
    th.join();
  }
  std::string base() const { return "http://127.0.0.1:" + std::to_string(port); }
  int hit(const std::string& m) {
    std::lock_guard<std::mutex> lk(mu);
    return hits[m];
  }
};

static MockUpstream* g_mock = nullptr;

static fs::path make_data_dir(const std::string& tag) {
  fs::path d = fs::temp_directory_path() / ("claw-test-" + tag + "-" + random_hex(4));
  fs::create_directories(d);
  json prov = {{"providers",
                {{"mock",
                  {{"chat_url", g_mock->base() + "/v1/chat/completions"},
                   {"models_url", g_mock->base() + "/v1/models"},
                   {"api_key", "k-default"},
                   {"match", json::array({{{"free", true}}})},
                   {"exclude", "tts"}}},
                 {"keyed", {{"chat_url", g_mock->base() + "/v1/chat/completions"},
                            {"requires_key", true},
                            {"api_key_env", "CLAW_TEST_UNSET_KEY"}}}}}};
  write_file_atomic(d / "providers.json", prov.dump(2));
  return d;
}

static std::vector<ModelRef> refs(const std::vector<std::string>& ids, const std::string& key = "") {
  std::vector<ModelRef> out;
  for (const auto& id : ids) out.push_back({"mock/" + id, "mock", id, key});
  return out;
}

static json user_msgs(int n) {
  json m = json::array({{{"role", "system"}, {"content", "be brief"}}});
  for (int i = 0; i < n; ++i) {
    m.push_back({{"role", i % 2 == 0 ? "user" : "assistant"}, {"content", "msg " + std::to_string(i)}});
  }
  if (m.back()["role"] != "user") m.push_back({{"role", "user"}, {"content", "last"}});
  return m;
}

// ---------------------------------------------------------------- unit tests

TEST(test_classify) {
  CHECK(classify_response(0, "") == Failure::Network);
  CHECK(classify_response(429, R"({"error":"rate limit"})") == Failure::RateLimit);
  CHECK(classify_response(429, R"({"error":"You exceeded your current quota"})") == Failure::Quota);
  CHECK(classify_response(402, "") == Failure::Quota);
  CHECK(classify_response(401, "invalid key") == Failure::Auth);
  CHECK(classify_response(403, "daily limit reached") == Failure::Quota);
  CHECK(classify_response(400, "maximum context length is 8192") == Failure::Context);
  CHECK(classify_response(413, "") == Failure::Context);
  CHECK(classify_response(400, "model not found") == Failure::BadRequest);
  CHECK(classify_response(503, "down") == Failure::Server);
  CHECK(classify_response(200, "not json") == Failure::Server);
  CHECK(classify_response(200, R"({"choices":[]})") == Failure::Empty);
  CHECK(classify_response(200, R"({"choices":[{"message":{"content":"  "},"finish_reason":"stop"}]})") ==
        Failure::Empty);
  CHECK(classify_response(200, R"({"choices":[{"message":{"content":""},"finish_reason":"length"}]})") ==
        Failure::Quota);
  CHECK(classify_response(200, R"({"choices":[{"message":{"content":"hi"}}]})") == Failure::None);
  CHECK(classify_response(200, R"({"choices":[{"message":{"content":null,"tool_calls":[{"id":"a"}]}}]})") ==
        Failure::None);
  CHECK(classify_response(200, R"({"error":{"message":"quota exhausted"}})") == Failure::Quota);
}

TEST(test_sanitize) {
  json req = {{"model", "auto"},
              {"stream", true},
              {"stream_options", {{"include_usage", true}}},
              {"store", true},
              {"max_completion_tokens", 100000},
              {"temperature", 0.2},
              {"tools", json::array()},
              {"messages",
               json::array({{{"role", "developer"}, {"content", "sys"}},
                            {{"role", "user"},
                             {"content", json::array({{{"type", "text"}, {"text", "a"}},
                                                      {{"type", "text"}, {"text", "b"}}})}},
                            {{"role", "assistant"},
                             {"content", nullptr},
                             {"reasoning_content", "hidden"},
                             {"tool_calls", json::array({{{"id", "c1"},
                                                          {"function", {{"name", "f"}, {"arguments", {{"x", 1}}}}}}})}},
                            {{"role", "tool"}, {"tool_call_id", "c1"}, {"content", "42"}}})}};
  json out = sanitize_request(req, "up-model", 8192);
  CHECK(out["model"] == "up-model");
  CHECK(out["stream"] == false);
  CHECK(!out.contains("stream_options"));
  CHECK(!out.contains("store"));
  CHECK(!out.contains("tools"));
  CHECK(out["max_tokens"] == 8192);
  CHECK(out["temperature"] == 0.2);
  CHECK(out["messages"][0]["role"] == "system");
  CHECK(out["messages"][1]["content"] == "a\nb");
  CHECK(!out["messages"][2].contains("reasoning_content"));
  CHECK(out["messages"][2]["content"].is_null());
  CHECK(out["messages"][2]["tool_calls"][0]["function"]["arguments"] == "{\"x\":1}");
  CHECK(out["messages"][2]["tool_calls"][0]["type"] == "function");
  CHECK(out["messages"][3]["tool_call_id"] == "c1");

  // Konten gambar tidak boleh dirusak.
  json img = {{"messages", json::array({{{"role", "user"},
                                         {"content", json::array({{{"type", "text"}, {"text", "a"}},
                                                                  {{"type", "image_url"}, {"image_url", {{"url", "x"}}}}})}}})}};
  CHECK(sanitize_request(img, "m", 10)["messages"][0]["content"].is_array());
}

TEST(test_trim_history) {
  json m = user_msgs(8);  // system + 8 pesan + user terakhir
  size_t before = m.size();
  CHECK(trim_history(m));
  CHECK(m.size() < before);
  CHECK(m[0]["role"] == "system");
  CHECK(m[1]["role"] == "user");
  CHECK(m.back()["content"] == "last");

  json only = json::array({{{"role", "system"}, {"content", "s"}}, {{"role", "user"}, {"content", "u"}}});
  CHECK(!trim_history(only));

  // Pasangan tool call tidak boleh terpotong di tengah.
  json t = json::array({{{"role", "user"}, {"content", "1"}},
                        {{"role", "assistant"}, {"content", nullptr}, {"tool_calls", json::array()}},
                        {{"role", "tool"}, {"content", "r"}},
                        {{"role", "assistant"}, {"content", "a"}},
                        {{"role", "user"}, {"content", "2"}}});
  CHECK(trim_history(t));
  CHECK(t[0]["role"] == "user");
  CHECK(t[0]["content"] == "2");
}

TEST(test_catalog_roundtrip) {
  fs::path d = make_data_dir("catalog");
  Catalog c(d);
  c.load_providers();
  CHECK(c.providers().size() == 2);
  CHECK(c.find_provider("mock") != nullptr);
  CHECK(c.find_provider("keyed") != nullptr && !c.find_provider("keyed")->enabled);

  // Urutan harus dipertahankan (prioritas).
  auto list = refs({"zeta", "alpha", "mid"}, "key1");
  c.save_models(list);
  auto loaded = c.load_models();
  CHECK(loaded.size() == 3);
  CHECK(loaded[0].name == "mock/zeta" && loaded[1].name == "mock/alpha");
  CHECK(loaded[0].api_key == "key1");
  json raw = json::parse(read_file(c.models_path()));
  CHECK(raw.is_object() && raw["mock/zeta"] == "key1");

  // Entri rusak dilewati, tidak membuat crash.
  write_file_atomic(c.models_path(),
                    R"({"mock/ok":"k","noslash":"k","ghost/m":"k","mock/bad":5,"mock/nokey":null})");
  loaded = c.load_models();
  CHECK(loaded.size() == 2);
  CHECK(loaded[1].name == "mock/nokey" && loaded[1].api_key.empty());

  write_file_atomic(c.models_path(), "{ broken");
  bool threw = false;
  try {
    c.load_models();
  } catch (...) {
    threw = true;
  }
  CHECK(threw);
  fs::remove_all(d);
}

TEST(test_sse) {
  json comp = {{"id", "abc"},
               {"created", 5},
               {"model", "auto"},
               {"choices", json::array({{{"index", 0},
                                         {"message",
                                          {{"role", "assistant"},
                                           {"content", "hi"},
                                           {"reasoning_content", "think"},
                                           {"tool_calls", json::array({{{"id", "c1"},
                                                                        {"type", "function"},
                                                                        {"function", {{"name", "f"}, {"arguments", "{}"}}}}})}}},
                                         {"finish_reason", "tool_calls"}}})},
               {"usage", {{"total_tokens", 3}}}};
  std::string sse = completion_to_sse(comp, true);
  std::istringstream in(sse);
  std::string line;
  int chunks = 0;
  bool done = false, saw_content = false, saw_tool = false, saw_usage = false, saw_reason = false;
  std::string finish;
  while (std::getline(in, line)) {
    if (line.rfind("data: ", 0) != 0) continue;
    std::string payload = line.substr(6);
    if (payload == "[DONE]") {
      done = true;
      continue;
    }
    json j = json::parse(payload);
    ++chunks;
    CHECK(j["object"] == "chat.completion.chunk");
    if (j.contains("usage")) saw_usage = true;
    for (const auto& c : j["choices"]) {
      const json& d = c["delta"];
      if (d.contains("content") && d["content"] == "hi") saw_content = true;
      if (d.contains("reasoning_content")) saw_reason = true;
      if (d.contains("tool_calls")) saw_tool = d["tool_calls"][0]["function"]["name"] == "f";
      if (!c["finish_reason"].is_null()) finish = c["finish_reason"];
    }
  }
  CHECK(done);
  CHECK(chunks >= 5);
  CHECK(saw_content && saw_tool && saw_usage && saw_reason);
  CHECK(finish == "tool_calls");
}

// ---------------------------------------------------------------- router tests

static Catalog load_catalog(const std::string& tag) {
  Catalog c(make_data_dir(tag));
  c.load_providers();
  return c;
}

TEST(test_router_switch_on_quota) {
  Catalog c = load_catalog("switch");
  Router r(c, refs({"quota", "payment", "rate", "ok-a", "ok-b"}, "sk-1"));
  std::vector<std::string> switches;
  r.on_switch = [&](const std::string& f, const std::string& t, const std::string&) {
    switches.push_back(f + ">" + t);
  };
  json req = {{"model", "auto"}, {"messages", user_msgs(1)}};
  int q0 = g_mock->hit("quota");
  ChatResult res = r.complete(req);
  CHECK(res.status == 200);
  CHECK(res.model == "mock/ok-a");
  CHECK(res.body["choices"][0]["message"]["content"] == "hello from ok-a");
  CHECK(res.body["model"] == "auto");
  CHECK(res.attempts.size() == 4);
  CHECK(r.current() == "mock/ok-a");
  CHECK(switches.size() == 1 && switches[0] == "mock/quota>mock/ok-a");
  CHECK(g_mock->last_auth == "Bearer sk-1");

  // Sticky: request berikutnya langsung ke model aktif, model habis tidak dicoba lagi.
  ChatResult res2 = r.complete(req);
  CHECK(res2.status == 200 && res2.attempts.size() == 1);
  CHECK(g_mock->hit("quota") == q0 + 1);

  auto st = r.status();
  CHECK(st[0].cooldown_left > 3000);   // kuota habis: cooldown panjang
  CHECK(st[2].cooldown_left <= 1.5);   // Retry-After: 1
  CHECK(st[3].current && st[3].ok == 2 && st[3].tokens == 14);
  fs::remove_all(c.data_dir());
}

TEST(test_router_session_continues) {
  Catalog c = load_catalog("session");
  Router r(c, refs({"quota", "echo"}));
  json history = user_msgs(6);
  ChatResult res = r.complete({{"messages", history}});
  CHECK(res.status == 200);
  // Model pengganti menerima seluruh riwayat, jadi percakapan tetap nyambung.
  CHECK(res.body["choices"][0]["message"]["content"] == "messages=" + std::to_string(history.size()));
  fs::remove_all(c.data_dir());
}

TEST(test_router_skips_empty_and_down) {
  Catalog c = load_catalog("empty");
  Router r(c, refs({"empty", "down", "ok-b"}));
  ChatResult res = r.complete({{"messages", user_msgs(1)}});
  CHECK(res.status == 200 && res.model == "mock/ok-b");
  fs::remove_all(c.data_dir());
}

TEST(test_router_tool_calls) {
  Catalog c = load_catalog("tools");
  Router r(c, refs({"tools"}));
  ChatResult res = r.complete({{"messages", user_msgs(1)}});
  CHECK(res.status == 200);
  const json& msg = res.body["choices"][0]["message"];
  CHECK(msg["tool_calls"][0]["function"]["arguments"].is_string());
  CHECK(!msg["tool_calls"][0]["id"].get<std::string>().empty());
  CHECK(msg["tool_calls"][0]["type"] == "function");
  CHECK(res.body["choices"][0]["finish_reason"] == "tool_calls");
  fs::remove_all(c.data_dir());
}

TEST(test_router_context_trim) {
  Catalog c = load_catalog("ctx");
  Router r(c, refs({"ctx"}));
  ChatResult res = r.complete({{"messages", user_msgs(12)}});
  CHECK(res.status == 200);
  CHECK(res.body["choices"][0]["message"]["content"].get<std::string>().rfind("short enough", 0) == 0);
  fs::remove_all(c.data_dir());
}

TEST(test_router_all_fail) {
  Catalog c = load_catalog("allfail");
  Router r(c, refs({"quota", "down"}));
  ChatResult res = r.complete({{"messages", user_msgs(1)}});
  CHECK(res.status == 503);
  CHECK(res.body["error"]["code"] == "pool_exhausted");
  // Semua sedang cooldown, tetapi request berikutnya tetap dicoba (bukan langsung gagal).
  int before = g_mock->hit("down");
  r.complete({{"messages", user_msgs(1)}});
  CHECK(g_mock->hit("down") == before + 1);

  Router empty(c, {});
  CHECK(empty.complete({{"messages", user_msgs(1)}}).status == 503);
  fs::remove_all(c.data_dir());
}

TEST(test_router_recovers_after_cooldown) {
  Catalog c = load_catalog("recover");
  RouterOptions o;
  o.server_cooldown_sec = 1;
  Router r(c, refs({"flaky", "ok-a"}), o);
  CHECK(r.complete({{"messages", user_msgs(1)}}).model == "mock/ok-a");
  r.set_current("mock/flaky");
  ChatResult res = r.complete({{"messages", user_msgs(1)}});
  CHECK(res.model == "mock/flaky");
  CHECK(res.body["choices"][0]["message"]["content"] == "recovered");
  fs::remove_all(c.data_dir());
}

TEST(test_router_parallel) {
  Catalog c = load_catalog("parallel");
  Router r(c, refs({"quota", "ok-a", "ok-b"}));
  std::atomic<int> ok{0};
  std::vector<std::thread> ts;
  for (int i = 0; i < 16; ++i)
    ts.emplace_back([&] {
      if (r.complete({{"messages", user_msgs(1)}}).status == 200) ok++;
    });
  for (auto& t : ts) t.join();
  CHECK(ok == 16);
  fs::remove_all(c.data_dir());
}

TEST(test_router_state_persists) {
  Catalog c = load_catalog("persist");
  RouterOptions o;
  o.state_file = (c.state_dir() / "router-state.json").string();
  {
    Router r(c, refs({"quota", "ok-a", "ok-b"}), o);
    CHECK(r.complete({{"messages", user_msgs(1)}}).model == "mock/ok-a");
  }
  CHECK(fs::exists(o.state_file));
  // Runner dijalankan ulang: model yang kuotanya habis tidak dicoba lagi dan
  // model aktif terakhir langsung dipakai.
  int before = g_mock->hit("quota");
  Router r2(c, refs({"quota", "ok-a", "ok-b"}), o);
  CHECK(r2.current() == "mock/ok-a");
  ChatResult res = r2.complete({{"messages", user_msgs(1)}});
  CHECK(res.model == "mock/ok-a" && res.attempts.size() == 1);
  CHECK(g_mock->hit("quota") == before);
  CHECK(r2.status()[0].cooldown_left > 3000);

  // File state rusak tidak boleh membuat crash.
  write_file_atomic(o.state_file, "{broken");
  Router r3(c, refs({"quota", "ok-a"}), o);
  CHECK(r3.current() == "mock/quota");
  fs::remove_all(c.data_dir());
}

// ---------------------------------------------------------------- proxy tests

TEST(test_proxy_http) {
  Catalog c = load_catalog("proxy");
  Router r(c, refs({"quota", "ok-a", "tools"}));
  ProxyServer p(r, "secret");
  int port = p.start("127.0.0.1", 28899);
  CHECK(port > 0);
  httplib::Client cli("127.0.0.1", port);
  cli.set_read_timeout(30, 0);

  auto h = cli.Get("/health");
  CHECK(h && h->status == 200);

  auto unauth = cli.Get("/v1/models");
  CHECK(unauth && unauth->status == 401);

  httplib::Headers auth = {{"Authorization", "Bearer secret"}};
  auto models = cli.Get("/v1/models", auth);
  CHECK(models && models->status == 200);
  if (models) {
    json m = json::parse(models->body);
    CHECK(m["data"].size() == 4 && m["data"][0]["id"] == "auto");
  }

  json body = {{"model", "clawpool/auto"}, {"messages", user_msgs(1)}};
  auto res = cli.Post("/v1/chat/completions", auth, body.dump(), "application/json");
  CHECK(res && res->status == 200);
  if (res) {
    CHECK(res->get_header_value("X-Claw-Model") == "mock/ok-a");
    CHECK(json::parse(res->body)["choices"][0]["message"]["content"] == "hello from ok-a");
  }

  auto bad = cli.Post("/v1/chat/completions", auth, "{nope", "application/json");
  CHECK(bad && bad->status == 400);

  // Streaming.
  body["stream"] = true;
  body["stream_options"] = {{"include_usage", true}};
  auto sres = cli.Post("/v1/chat/completions", auth, body.dump(), "application/json");
  CHECK(sres && sres->status == 200);
  if (sres) {
    CHECK(sres->get_header_value("Content-Type").find("text/event-stream") != std::string::npos);
    CHECK(sres->body.find("\"content\":\"hello from ok-a\"") != std::string::npos);
    CHECK(sres->body.find("data: [DONE]") != std::string::npos);
    CHECK(sres->body.find("\"usage\"") != std::string::npos);
  }

  // Pilih model tertentu lewat nama pool.
  json pinned = {{"model", "mock/tools"}, {"stream", true}, {"messages", user_msgs(1)}};
  auto tres = cli.Post("/v1/chat/completions", auth, pinned.dump(), "application/json");
  CHECK(tres && tres->body.find("\"tool_calls\"") != std::string::npos);
  CHECK(tres && tres->body.find("\"finish_reason\":\"tool_calls\"") != std::string::npos);

  auto st = cli.Get("/status", auth);
  CHECK(st && json::parse(st->body)["current"] == "mock/tools");

  // Port sibuk -> proxy kedua pindah ke port lain.
  ProxyServer p2(r, "");
  int port2 = p2.start("127.0.0.1", port);
  CHECK(port2 > port);
  p2.stop();
  p.stop();
  fs::remove_all(c.data_dir());
}

TEST(test_proxy_stream_error) {
  Catalog c = load_catalog("streamerr");
  Router r(c, refs({"down"}));
  ProxyServer p(r, "");
  int port = p.start("127.0.0.1", 29899);
  httplib::Client cli("127.0.0.1", port);
  json body = {{"stream", true}, {"messages", user_msgs(1)}};
  auto res = cli.Post("/v1/chat/completions", body.dump(), "application/json");
  CHECK(res && res->body.find("pool_exhausted") != std::string::npos);
  p.stop();
  fs::remove_all(c.data_dir());
}

// ---------------------------------------------------------------- scanner tests

TEST(test_discover_and_probe) {
  Catalog c = load_catalog("scan");
  const Provider* p = c.find_provider("mock");
  auto ids = discover_models(*p);
  // paid-x dibuang oleh match, tts-voice oleh exclude.
  CHECK(ids.size() == 4);
  CHECK(std::find(ids.begin(), ids.end(), "paid-x") == ids.end());
  CHECK(std::find(ids.begin(), ids.end(), "tts-voice") == ids.end());

  ScanOptions o;
  o.rate_limit_retries = 0;
  o.max_wait_sec = 1;
  auto ok = probe_model({"mock/ok-a", "mock", "ok-a", ""}, *p, o);
  CHECK(ok.passed && ok.status == 200);
  auto bad = probe_model({"mock/down", "mock", "down", ""}, *p, o);
  CHECK(!bad.passed && bad.status == 503);

  // Scan penuh: hanya model HTTP 200 yang masuk models.json.
  write_file_atomic(c.models_path(), R"({"mock/manual":"my-own-key"})");
  int passed = run_scan(c, o);
  auto models = c.load_models();
  CHECK(passed == 3);  // ok-a, ok-b, manual (down dan rate gagal)
  CHECK(models.size() == 3);
  bool has_down = false;
  std::string manual_key;
  for (const auto& m : models) {
    if (m.name == "mock/down") has_down = true;
    if (m.name == "mock/manual") manual_key = m.api_key;
  }
  CHECK(!has_down);
  CHECK(manual_key == "my-own-key");
  CHECK(fs::exists(c.state_dir() / "scan-report.json"));

  // check --prune hanya menghapus model yang rusak permanen (404, jawaban kosong);
  // model yang sedang 503 tetap disimpan.
  auto withbad = models;
  withbad.push_back({"mock/down", "mock", "down", ""});
  withbad.push_back({"mock/gone", "mock", "gone", ""});
  withbad.push_back({"mock/empty", "mock", "empty", ""});
  c.save_models(withbad);
  CHECK(run_check(c, o, true) == 3);
  auto after = c.load_models();
  CHECK(after.size() == 4);
  bool kept_down = false, kept_gone = false;
  for (const auto& m : after) {
    if (m.name == "mock/down") kept_down = true;
    if (m.name == "mock/gone" || m.name == "mock/empty") kept_gone = true;
  }
  CHECK(kept_down && !kept_gone);
  fs::remove_all(c.data_dir());
}

static ProbeResult pr(const std::string& id, bool passed, Failure f = Failure::None, double lat = 1) {
  ProbeResult r;
  r.ref = {"mock/" + id, "mock", id, ""};
  r.passed = passed;
  r.failure = passed ? "" : failure_name(f);
  r.latency = lat;
  return r;
}

TEST(test_update_merge) {
  auto existing = refs({"keep-ok", "limited", "gone", "unlisted", "untested"});
  std::vector<ProbeResult> results = {pr("keep-ok", true, Failure::None, 2), pr("limited", false, Failure::RateLimit),
                                      pr("gone", false, Failure::BadRequest), pr("unlisted", false, Failure::Server),
                                      pr("fresh", true, Failure::None, 1), pr("fresh-bad", false, Failure::Auth)};
  std::set<std::string> listed = {"mock/keep-ok", "mock/limited", "mock/gone", "mock/fresh", "mock/fresh-bad"};
  MergeOutcome m = merge_update(existing, results, listed, {"mock"});
  std::vector<std::string> names;
  for (const auto& x : m.models) names.push_back(x.name);
  // Lolos dulu (tercepat duluan), lalu yang gagal sementara, lalu yang tidak diuji.
  CHECK((names == std::vector<std::string>{"mock/fresh", "mock/keep-ok", "mock/limited", "mock/untested"}));
  CHECK(m.added == std::vector<std::string>{"mock/fresh"});
  CHECK(m.removed.size() == 2);  // gone (404), unlisted (hilang dari daftar provider)
  CHECK(m.kept_unverified == std::vector<std::string>{"mock/limited"});

  // Daftar provider gagal dibaca: model yang tidak ada di daftar tidak dibuang.
  MergeOutcome m2 = merge_update(existing, results, {}, {});
  bool has_unlisted = false;
  for (const auto& x : m2.models) has_unlisted |= x.name == "mock/unlisted";
  CHECK(has_unlisted);
  CHECK(is_permanent_failure(failure_name(Failure::Auth)));
  CHECK(!is_permanent_failure(failure_name(Failure::RateLimit)));
  CHECK(!is_permanent_failure(failure_name(Failure::Quota)));
}

TEST(test_update_offline_keeps_models) {
  // Semua provider tidak bisa dihubungi: models.json tidak boleh dikosongkan.
  fs::path d = fs::temp_directory_path() / ("claw-test-offline-" + random_hex(4));
  fs::create_directories(d);
  json prov = {{"providers",
                {{"dead", {{"chat_url", "http://127.0.0.1:1/v1/chat/completions"},
                           {"models_url", "http://127.0.0.1:1/v1/models"}}}}}};
  write_file_atomic(d / "providers.json", prov.dump());
  write_file_atomic(d / "models.json", R"({"dead/a":"","dead/b":""})");
  Catalog c(d);
  c.load_providers();
  ScanOptions o;
  o.quiet = true;
  o.rate_limit_retries = 0;
  o.max_wait_sec = 1;
  o.probe_tools = false;
  UpdateResult r = run_update(c, o);
  CHECK(!r.written);
  CHECK(!r.error.empty());
  CHECK(c.load_models().size() == 2);
  CHECK(last_update_time(c) == 0);

  // Update normal lewat mock: model baru masuk, report dan waktu update tercatat.
  Catalog m = load_catalog("update");
  write_file_atomic(m.models_path(), R"({"mock/gone":""})");
  UpdateResult u = run_update(m, o);
  CHECK(u.written);
  CHECK(u.merge.added.size() == 2);  // ok-a, ok-b
  CHECK(u.merge.removed.size() == 1 && u.merge.removed[0].first == "mock/gone");
  CHECK(last_update_time(m) > 0);
  CHECK(fs::exists(m.state_dir() / "update-report.json"));
  fs::remove_all(d);
  fs::remove_all(m.data_dir());
}

static bool param_billions_helper();
TEST(test_mdstream) {
  // enabled=false: keluaran = teks apa adanya (per karakter), tanpa ANSI.
  std::string cap;
  {
    fflush(stdout);
    int saved = dup(fileno(stdout));
    FILE* tmp = tmpfile();
    dup2(fileno(tmp), fileno(stdout));
    MarkdownStreamer md(false);
    md.feed("Halo **dunia** `kode` dan\n```py\nprint(1)\n```\n");
    md.finish();
    fflush(stdout);
    long n = ftell(tmp);
    rewind(tmp);
    cap.resize(n > 0 ? n : 0);
    if (n > 0) { size_t rd = fread(&cap[0], 1, n, tmp); cap.resize(rd); }
    dup2(saved, fileno(stdout));
    close(saved);
    fclose(tmp);
  }
  // Tanpa gaya: markah tetap muncul literal, isi utuh.
  CHECK(cap.find("Halo") != std::string::npos);
  CHECK(cap.find("dunia") != std::string::npos);
  CHECK(cap.find("print(1)") != std::string::npos);
  CHECK(cap.find("\033[") == std::string::npos);  // tidak ada ANSI saat disabled

  // enabled=true: ada kode ANSI, teks tetap ada.
  std::string cap2;
  {
    fflush(stdout);
    int saved = dup(fileno(stdout));
    FILE* tmp = tmpfile();
    dup2(fileno(tmp), fileno(stdout));
    MarkdownStreamer md(true);
    md.feed("teks **tebal** lalu ```\nkode\n``` selesai");
    md.finish();
    fflush(stdout);
    long n = ftell(tmp);
    rewind(tmp);
    if (n > 0) { cap2.resize(n); size_t rd = fread(&cap2[0], 1, n, tmp); cap2.resize(rd); }
    dup2(saved, fileno(stdout));
    close(saved);
    fclose(tmp);
  }
  CHECK(cap2.find("\033[") != std::string::npos);   // ada ANSI
  CHECK(cap2.find("tebal") != std::string::npos);
  CHECK(cap2.find("kode") != std::string::npos);
}

// Tangkap keluaran MarkdownStreamer (tanpa ANSI) untuk potongan-potongan stream.
static std::string md_capture(const std::vector<std::string>& pieces, const std::string& lead,
                              bool* shown = nullptr) {
  std::string cap;
  fflush(stdout);
  int saved = dup(fileno(stdout));
  FILE* tmp = tmpfile();
  dup2(fileno(tmp), fileno(stdout));
  MarkdownStreamer md(false);
  md.set_lead(lead);
  for (const auto& p : pieces) md.feed(p);
  if (shown) *shown = md.shown();
  md.finish();
  fflush(stdout);
  long n = ftell(tmp);
  rewind(tmp);
  if (n > 0) { cap.resize(n); size_t rd = fread(&cap[0], 1, n, tmp); cap.resize(rd); }
  dup2(saved, fileno(stdout));
  close(saved);
  fclose(tmp);
  return cap;
}

TEST(test_mdstream_spacing) {
  // Baris kosong di awal dan akhir jawaban dibuang; jarak dari prompt tetap satu baris (lead).
  CHECK(md_capture({"\n\n\n", "Halo!", " \n\n"}, "\n") == "\nHalo!");
  // Baris kosong berturut-turut antar paragraf dirapatkan menjadi satu.
  CHECK(md_capture({"satu\n\n\n\n", "dua\n  \n\n", "tiga"}, "") == "satu\n\ndua\n\ntiga");
  // Spasi di ujung baris dibuang, indentasi di awal baris tetap.
  CHECK(md_capture({"a   \n", "  b"}, "") == "a\n  b");
  // Blok kode tetap utuh, termasuk baris kosong di dalamnya (tanpa gaya, pagar ``` tidak dicetak).
  CHECK(md_capture({"```\nx\n\n\ny\n```"}, "") == "x\n\n\ny\n");
  // Jawaban yang cuma berisi spasi/baris kosong tidak mencetak apa-apa, termasuk lead.
  bool shown = true;
  CHECK(md_capture({"\n", "  \n\n"}, "\n", &shown).empty());
  CHECK(!shown);
}

TEST(test_mdstream_code_bar) {
  // Bar kiri hanya di baris kode (termasuk baris kosong di dalamnya), tidak di baris pagar.
  std::string cap;
  fflush(stdout);
  int saved = dup(fileno(stdout));
  FILE* tmp = tmpfile();
  dup2(fileno(tmp), fileno(stdout));
  MarkdownStreamer md(true);
  md.feed("Contoh:\n```\na\n\nb\n```");
  bool col0 = md.at_col0();
  md.finish();
  fflush(stdout);
  long n = ftell(tmp);
  rewind(tmp);
  if (n > 0) { cap.resize(n); size_t rd = fread(&cap[0], 1, n, tmp); cap.resize(rd); }
  dup2(saved, fileno(stdout));
  close(saved);
  fclose(tmp);
  size_t bars = 0;
  for (size_t p = cap.find("\u2502"); p != std::string::npos; p = cap.find("\u2502", p + 1)) ++bars;
  CHECK(bars == 3);
  CHECK(col0);  // pagar penutup sudah di awal baris: pemanggil tidak menambah baris baru
  CHECK(cap.find("```") == std::string::npos);
}

TEST(test_routing) {
  using K = TaskKind;
  CHECK(categorize_request("tolong tulis fungsi python untuk cek prima") == K::Coding);
  CHECK(categorize_request("debug error di script ini") == K::Coding);
  CHECK(categorize_request("desain UI halaman login dengan warna dan font") == K::Design);
  CHECK(categorize_request("analisa dan bandingkan dua strategi ini, buktikan") == K::Reasoning);
  CHECK(categorize_request("apa kabar hari ini") == K::General);

  std::vector<std::string> pool = {
      "pollinations/openai-fast", "ovh/Qwen3-Coder-30B-A3B-Instruct", "kilo/cohere/north-mini-code:free",
      "kilo/nvidia/nemotron-3-ultra-550b-a55b:free", "ovh/Qwen2.5-VL-72B-Instruct",
      "kilo/nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free", "ovh/Mistral-7B-Instruct-v0.3"};
  // Coding -> model coder menang.
  std::string c = pick_model(pool, K::Coding);
  CHECK(c == "ovh/Qwen3-Coder-30B-A3B-Instruct" || c == "kilo/cohere/north-mini-code:free");
  CHECK(score_model("ovh/Qwen3-Coder-30B-A3B-Instruct", K::Coding) >
        score_model("pollinations/openai-fast", K::Coding));
  // Vision -> model VL/omni.
  std::string v = pick_model(pool, K::Vision);
  CHECK(v == "ovh/Qwen2.5-VL-72B-Instruct" || v == "kilo/nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free");
  // Reasoning -> model reasoning atau terbesar.
  std::string r = pick_model(pool, K::Reasoning);
  CHECK(r == "kilo/nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free" ||
        r == "kilo/nvidia/nemotron-3-ultra-550b-a55b:free");
  // General -> serahkan ke router (auto).
  CHECK(pick_model(pool, K::General).empty());
  // Pool tanpa model coder khusus: coding jatuh ke instruct terbesar (>=threshold? tidak),
  // jadi kosong -> auto.
  CHECK(pick_model({"pollinations/openai-fast"}, K::Coding).empty());
  CHECK(param_billions_helper());
}

// pembungkus kecil agar param parsing teruji lewat score_model perbandingan ukuran.
static bool param_billions_helper() {
  return score_model("x-550b", TaskKind::Reasoning) > score_model("x-9b", TaskKind::Reasoning);
}

TEST(test_fstools) {
  fs::path base = fs::temp_directory_path() / ("claw-fs-" + random_hex(4));
  fs::create_directories(base);
  // write + read
  CHECK(fs_write(base, "sub/a.txt", "halo dunia").value("ok", false));
  json rd = fs_read(base, "sub/a.txt");
  CHECK(rd.value("ok", false) && rd.value("content", "") == "halo dunia" && !rd.value("truncated", true));
  // list
  json ls = fs_list(base, "sub");
  CHECK(ls.value("ok", false) && ls.value("count", 0) == 1);
  // mkdir + find
  CHECK(fs_mkdir(base, "sub/deep/nest").value("ok", false));
  CHECK(fs_write(base, "sub/deep/nest/target-file.md", "x").value("ok", false));
  json fd = fs_find(base, ".", "target");
  CHECK(fd.value("ok", false) && fd.value("count", 0) == 1);
  // move/rename
  CHECK(fs_move(base, "sub/a.txt", "sub/b.txt").value("ok", false));
  CHECK(!fs_read(base, "sub/a.txt").value("ok", true));
  CHECK(fs_read(base, "sub/b.txt").value("ok", false));
  // move into existing dir keeps name
  json mv2 = fs_move(base, "sub/b.txt", "sub/deep");
  CHECK(mv2.value("ok", false) && fs_read(base, "sub/deep/b.txt").value("ok", false));
  // errors: missing source, existing dest, control char, absolute normalize
  CHECK(!fs_read(base, "tidak-ada.txt").value("ok", true));
  CHECK(!fs_move(base, "tidak-ada", "x").value("ok", true));
  CHECK(!fs_mkdir(base, "sub").value("ok", true));  // sudah ada
  CHECK(!fs_precheck("").empty());
  CHECK(!fs_precheck(std::string("a\nb")).empty());
  CHECK(fs_precheck("folder/file.txt").empty());
  // truncation
  CHECK(fs_write(base, "big.txt", std::string(100, 'z')).value("ok", false));
  json big = fs_read(base, "big.txt", 10);
  CHECK(big.value("truncated", false) && big.value("content", "").size() == 10);
  // read a directory is an error
  CHECK(!fs_read(base, "sub").value("ok", true));
  fs::remove_all(base);
}

TEST(test_activity_and_language) {
  fs::path home = fs::temp_directory_path() / ("claw-act-" + random_hex(4));
  fs::create_directories(home);
  {
    Activity act(true, false, home, "chat");  // detailed file aktif, live mati
    act.step("Mulai", "detail baris satu\ndetail baris dua");
    act.tool("web_fetch", "example.com", "HTTP 200");
    act.turn("halo", "mock/ok-a");
    CHECK(fs::exists(act.path()));
    std::ifstream in(act.path());
    std::string all((std::istreambuf_iterator<char>(in)), {});
    CHECK(all.find("Catatan aktivitas claw") != std::string::npos);
    CHECK(all.find("web_fetch") != std::string::npos);
    CHECK(all.find("mock/ok-a") != std::string::npos);
    CHECK(all.find("    detail baris dua") != std::string::npos);  // rincian di-indent
  }
  // Umpan real-time untuk `claw watch`.
  {
    Activity act(false, true, home, "chat");
    act.live("sedang membuat file 'test.txt'");
    fs::path lp = Activity::live_path(home);
    CHECK(fs::exists(lp));
    std::ifstream in(lp);
    std::string all((std::istreambuf_iterator<char>(in)), {});
    CHECK(all.find("> sedang membuat file 'test.txt' (") != std::string::npos);
    CHECK(all.find("== sesi claw chat dimulai ==") != std::string::npos);  // penanda sesi
  }
  // Dimatikan: tidak ada berkas yang dibuat.
  {
    fs::path h2 = fs::temp_directory_path() / ("claw-act2-" + random_hex(4));
    Activity off(false, false, h2, "chat");
    off.step("apa pun");
    CHECK(!fs::exists(h2));
  }

  // Bahasa: id.jsonl bawaan + belajar dari pengalaman.
  fs::path data = fs::temp_directory_path() / ("claw-lang-" + random_hex(4));
  fs::create_directories(data / "language");
  {
    std::ofstream f(data / "language" / "id.jsonl");
    f << "{\"intent\":\"sapaan\",\"text\":\"Halo.\"}\n";
    f << "baris rusak yang harus dilewati\n";
    f << "{\"intent\":\"selesai\",\"text\":\"Beres.\"}\n";
  }
  auto ph = load_phrases(data, home);
  CHECK(ph.size() == 2);
  CHECK(learn_phrase(home, "sapaan", "Hai juga."));
  CHECK(learn_phrase(home, "sapaan", "Hai juga."));  // duplikat: tidak menggandakan
  auto ph2 = load_phrases(data, home);
  CHECK(ph2.size() == 3);
  CHECK(!style_block(ph2).empty());
  CHECK(style_block({}).empty());
  fs::remove_all(home);
  fs::remove_all(data);
}

TEST(test_policy) {
  fs::path d = fs::temp_directory_path() / ("claw-policy-" + random_hex(4));
  fs::create_directories(d);
  Policy p = load_policy(d);  // tanpa file: bawaan yang aman
  CHECK(p.browse == "ask" && !p.private_network && p.proxy_token && !p.save_conversations);
  write_file_atomic(d / "policy.json",
                    R"({"browse":"ALLOW","private_network":true,"skills":false,"update_interval_hours":0,)"
                    R"("proxy_token":"yes"})");
  p = load_policy(d);
  CHECK(p.browse == "allow" && p.private_network && !p.skills);
  CHECK(!p.humanizer && p.activity_log);  // tidak disebut: bawaan aman
  CHECK(p.update_interval_hours == 1);    // dijepit ke minimal 1 jam
  CHECK(p.proxy_token);                   // tipe salah: bawaan dipakai
  write_file_atomic(d / "policy.json", R"({"humanizer":true,"activity_log":false,"fs_access":"ALLOW"})");
  p = load_policy(d);
  CHECK(p.humanizer && !p.activity_log && p.fs_access == "allow");
  CHECK(p.chat_model == "auto" && p.humanizer_model == "auto" && p.humanizer_agents == 3);
  write_file_atomic(d / "policy.json",
                    R"({"chat_model":" ovh/a ","humanizer_model":"AUTO","humanizer_agents":40})");
  p = load_policy(d);
  CHECK(p.chat_model == "ovh/a" && p.humanizer_model == "auto" && p.humanizer_agents == 6);
  CHECK(load_policy(fs::temp_directory_path() / "nope-xyz").fs_access == "off");  // default
  write_file_atomic(d / "policy.json", R"({"fs_access":"weird"})");
  CHECK(load_policy(d).fs_access == "off");
  write_file_atomic(d / "policy.json", R"({"browse":"everything"})");
  CHECK(load_policy(d).browse == "ask");
  write_file_atomic(d / "policy.json", "{rusak");
  std::string warn;
  p = load_policy(d, &warn);
  CHECK(!warn.empty() && p.browse == "ask");
  fs::remove_all(d);
}

TEST(test_skills_and_urls) {
  fs::path root = fs::temp_directory_path() / ("claw-sk-" + random_hex(4));
  fs::create_directories(root / "skills/weather");
  fs::create_directories(root / "skills/plain");
  write_file_atomic(root / "skills/weather/SKILL.md",
                    "---\nname: weather\ndescription: \"Get current weather.\"\n---\n# Weather\nUse wttr.in\n");
  write_file_atomic(root / "skills/plain/SKILL.md", "# Plain\n\nFirst paragraph here.\n");
  auto sk = list_skills({root / "skills"});
  CHECK(sk.size() == 2);
  const SkillInfo* w = find_skill(sk, "Weather");
  CHECK(w && w->description == "Get current weather.");
  const SkillInfo* pl = find_skill(sk, "plain");
  CHECK(pl && pl->description == "First paragraph here.");
  CHECK(read_skill(*w).find("wttr.in") != std::string::npos);
  CHECK(read_skill(*w, 10).find("dipotong") != std::string::npos);
  fs::remove_all(root);

  CHECK(precheck_url("https://example.com/a?b=1").empty());
  CHECK(!precheck_url("file:///etc/passwd").empty());
  CHECK(!precheck_url("-rf").empty());
  CHECK(!precheck_url("").empty());
  CHECK(!precheck_url("http://x/\na").empty());
  CHECK(precheck_url(" https://x/ \n").empty());  // spasi di ujung dipangkas
  CHECK(url_host("https://user:pw@example.com:8443/path?q=secret") == "example.com:8443");

  std::string out;
  CHECK(run_capture({"/bin/sh", "-c", "echo hi"}, 5, out) == 0 && out == "hi\n");
  CHECK(run_capture({"/bin/sh", "-c", "sleep 5"}, 1, out) == 124);
  CHECK(run_capture({"/nonexistent/prog"}, 5, out) == 127);
}

TEST(test_project_skills) {
  fs::path root = fs::temp_directory_path() / ("claw-skills-" + random_hex(6));
  auto mk = [&](const fs::path& rel) {
    fs::create_directories(root / rel);
    write_file_atomic(root / rel / "SKILL.md", "---\nname: x\n---\n");
  };
  mk("skills/weather");
  mk("skills/group/tmux");
  mk(".agents/skills/deslop");
  mk("claude-skills/gstack");
  mk("claude-skills/gstack/qa");  // di bawah SKILL.md lain: tidak dihitung
  mk("claude-skills/gstack/node_modules/pkg");
  mk("claude-skills/humanizer");
  mk("claude-skills/.backup/humanizer.bak");      // folder berawalan titik dilewati
  mk("claude-skills/a/b/terlalu-dalam");          // lebih dari 2 tingkat
  fs::create_directories(root / "claude-skills" / "node_modules" / "dep");
  write_file_atomic(root / "claude-skills" / "node_modules" / "dep" / "SKILL.md", "x");

  auto dirs = project_skill_dirs(root);
  CHECK(dirs.size() == 3);
  CHECK(count_skills(root / "skills") == 2);
  CHECK(count_skills(root / ".agents" / "skills") == 1);
  CHECK(count_skills(root / "claude-skills") == 2);

  auto same_ws = project_skill_dirs(root, root);
  CHECK(same_ws.size() == 1);
  CHECK(same_ws[0].filename() == "claude-skills");
  CHECK(project_skill_dirs(root, fs::temp_directory_path()).size() == 3);

  fs::remove_all(root / ".agents");
  CHECK(project_skill_dirs(root).size() == 2);

  fs::path cfg = root / "openclaw.json";
  write_openclaw_config(cfg, 18899, "tok", root, project_skill_dirs(root));
  json j = json::parse(read_file(cfg));
  CHECK(j["skills"]["load"]["extraDirs"].size() == 2);
  CHECK(j["skills"]["load"]["extraDirs"][0] == fs::absolute(root / "skills").generic_string());
  write_openclaw_config(cfg, 18899, "tok", root, {});
  CHECK(!json::parse(read_file(cfg)).contains("skills"));
  fs::remove_all(root);
}

TEST(test_router_add_and_broken) {
  Catalog c = load_catalog("repair");
  // Pool awal cuma model yang kuotanya habis.
  Router r(c, refs({"quota"}, "sk-1"));
  std::vector<std::string> broken;
  r.on_broken = [&](const std::string& m, const std::string& why) {
    broken.push_back(m + ":" + why);
  };
  CHECK(r.usable() == 1);

  json req = {{"model", "auto"}, {"messages", user_msgs(1)}};
  ChatResult res = r.complete(req);
  CHECK(res.status == 503);                       // semua model habis
  CHECK(broken.size() == 1);                      // on_broken terpicu sekali
  CHECK(broken[0] == "mock/quota:token/kuota habis");
  CHECK(r.usable() == 0);                          // quota masuk cooldown panjang

  // Auto-repair menyisipkan model gratis baru ke pool yang sedang jalan.
  int added = r.add_models(c, refs({"ok-a", "ok-b"}, "sk-1"));
  CHECK(added == 2);
  CHECK(r.add_models(c, refs({"ok-a"}, "sk-1")) == 0);  // duplikat dilewati
  CHECK(r.size() == 3);
  CHECK(r.usable() == 2);

  // Percakapan lanjut: pesan berikutnya dijawab model pengganti.
  ChatResult res2 = r.complete(req);
  CHECK(res2.status == 200);
  CHECK(res2.model == "mock/ok-a");
  CHECK(res2.body["choices"][0]["message"]["content"] == "hello from ok-a");
  fs::remove_all(c.data_dir());
}

TEST(test_agents_plan) {
  std::vector<SkillInfo> sk = {{"engineering", "Rancang, tulis, dan review kode", {}},
                               {"riset", "ide untuk project, riset, atau biologi", {}},
                               {"web-research", "Riset di internet", {}},
                               {"audit-debug", "Audit dan debug project", {}},
                               {"design", "Desainer UI/UX", {}}};
  // Rencana dibungkus teks dan pagar kode, dengan skill palsu, id ganda, dan siklus.
  std::string txt = "Berikut rencananya:\n```json\n"
                    R"({"domain":"biologi","agents":[)"
                    R"({"id":"a1","role":"peneliti","task":"cari literatur enzim","kind":"reasoning","skills":["riset","palsu"],"depends_on":["a2"]},)"
                    R"({"id":"a1","role":"analis","task":"ringkas temuan","kind":"coding","depends_on":["a1","a1"]},)"
                    R"({"id":"a3","role":"x","task":"","skills":[]},)"
                    R"({"id":"a4","role":"penulis","task":"tulis laporan","depends_on":["a4","zz"]}]})"
                    "\n```\nselesai";
  std::string err;
  AgentPlan p = parse_plan(txt, sk, 5, &err);
  CHECK(err.empty());
  CHECK(p.domain == "biologi");
  CHECK(p.tasks.size() == 3);  // tugas kosong dibuang
  CHECK(p.tasks[0].skills == std::vector<std::string>{"riset"});
  CHECK(p.tasks[0].depends_on.empty());  // a2 belum disebut sebelumnya -> dibuang
  CHECK(p.tasks[0].kind == TaskKind::Reasoning);
  CHECK(p.tasks[1].id != "a1");          // id ganda diganti
  CHECK(p.tasks[1].depends_on == std::vector<std::string>{"a1"});
  CHECK(p.tasks[2].depends_on.empty());  // ke diri sendiri / tak dikenal dibuang
  auto w = plan_waves(p);
  CHECK(w.size() == 2 && w[0].size() == 2 && w[1] == std::vector<size_t>{1});
  CHECK(parse_plan(txt, sk, 1).tasks.size() == 1);  // dibatasi max_agents
  CHECK(parse_plan("tidak ada json", sk, 3, &err).tasks.empty() && !err.empty());
  CHECK(parse_plan(R"({"agents":[]})", sk, 3).tasks.empty());

  // Pemilihan skill per bidang.
  auto s1 = suggest_skills("riset struktur protein dan enzim bakteri", sk);
  CHECK(!s1.empty() && s1[0] == "riset");
  auto s2 = suggest_skills("buat aplikasi CLI python lalu debug", sk);
  CHECK(!s2.empty() && s2[0] == "engineering");
  CHECK(suggest_skills("halo apa kabar", sk).empty());
  CHECK(suggest_skills("riset protein", {}).empty());
  CHECK(guess_domain("analisis genom bakteri") == "biologi");
  CHECK(guess_domain("halo") == "umum");
  AgentPlan f = fallback_plan("bangun project web dengan python", sk);
  CHECK(f.tasks.size() == 1 && !f.tasks[0].skills.empty() && f.tasks[0].kind == TaskKind::Coding);
}

// Delegasi agen depan -> agen pekerja: parsing argumen tool delegate_task + peran default.
TEST(test_delegate) {
  std::vector<SkillInfo> sk = {{"engineering", "Rancang, tulis, dan review kode", {}},
                               {"audit-debug", "Audit dan debug project", {}},
                               {"design", "Desainer UI/UX", {}}};
  // Argumen lengkap: task, kind, role, skills (satu palsu + satu duplikat dibuang).
  DelegateRequest d = parse_delegate(
      R"({"task":"buat CLI kalkulator lalu uji","kind":"coding","role":"developer","skills":["engineering","palsu","engineering","audit-debug"]})",
      sk);
  CHECK(d.task == "buat CLI kalkulator lalu uji");
  CHECK(d.kind == TaskKind::Coding);
  CHECK(d.role == "developer");
  CHECK(d.skills == (std::vector<std::string>{"engineering", "audit-debug"}));

  // kind kosong -> General; skill dibatasi ke yang benar-benar ada.
  DelegateRequest d2 = parse_delegate(R"({"task":"ringkas dokumen","skills":["design"]})", sk);
  CHECK(d2.task == "ringkas dokumen" && d2.kind == TaskKind::General && d2.role.empty());
  CHECK(d2.skills == std::vector<std::string>{"design"});

  // Tanpa daftar skill / skill tak dikenal -> kosong (agen depan lalu memilih otomatis).
  CHECK(parse_delegate(R"({"task":"x","skills":["tak-ada"]})", sk).skills.empty());
  // task kosong / JSON rusak -> permintaan tidak sah (task kosong).
  CHECK(parse_delegate("", sk).task.empty());
  CHECK(parse_delegate("bukan json", sk).task.empty());
  CHECK(parse_delegate(R"({"kind":"coding"})", sk).task.empty());

  // Peran default per jenis tugas.
  CHECK(std::string(worker_role(TaskKind::Coding)) == "developer");
  CHECK(std::string(worker_role(TaskKind::Design)) == "desainer");
  CHECK(std::string(worker_role(TaskKind::Reasoning)) == "analis");
  CHECK(std::string(worker_role(TaskKind::General)) == "asisten");
}

// Konteks tak terbatas: batas kompaksi selalu jatuh di pesan 'user' dan tidak melewati system.
TEST(test_compaction_bounds) {
  using V = std::vector<std::string>;
  // [system, user, assistant, user, assistant, tool, assistant] — keep 6 -> mundur ke user@1.
  V r1 = {"system", "user", "assistant", "user", "assistant", "tool", "assistant"};
  CHECK(compact_keep_from(r1, 1, 6) == 1);   // size 7, size-6=1, r1[1]=="user"
  // Dua pesan system di depan (system dasar + memori) tetap dijaga.
  V r2 = {"system", "system", "user", "assistant", "user", "assistant", "tool", "assistant", "user", "assistant"};
  size_t k = compact_keep_from(r2, 2, 6);    // size 10, size-6=4, r2[4]=="user"
  CHECK(k == 4 && r2[k] == "user");
  // Tidak pernah lebih awal dari system_end walau blok terbaru bukan diawali user.
  V r3 = {"system", "assistant", "tool", "assistant"};
  CHECK(compact_keep_from(r3, 1, 6) == 1);
  // Percakapan pendek: tidak ada yang layak diringkas (hasil == system_end).
  V r4 = {"system", "user", "assistant"};
  CHECK(compact_keep_from(r4, 1, 6) == 1);
  // Mundur melewati pasangan tool ke user sebelumnya.
  V r5 = {"system", "user", "assistant", "tool", "assistant", "user", "assistant", "tool", "assistant"};
  size_t k5 = compact_keep_from(r5, 1, 4);   // size 9, size-4=5, r5[5]=="user"
  CHECK(k5 == 5 && r5[k5] == "user");
}

int main() {
  MockUpstream mock;
  g_mock = &mock;
  Log::set_console(false);

  RUN(test_classify);
  RUN(test_sanitize);
  RUN(test_trim_history);
  RUN(test_catalog_roundtrip);
  RUN(test_sse);
  RUN(test_router_switch_on_quota);
  RUN(test_router_session_continues);
  RUN(test_router_skips_empty_and_down);
  RUN(test_router_tool_calls);
  RUN(test_router_context_trim);
  RUN(test_router_all_fail);
  RUN(test_router_recovers_after_cooldown);
  RUN(test_router_parallel);
  RUN(test_router_state_persists);
  RUN(test_proxy_http);
  RUN(test_proxy_stream_error);
  RUN(test_discover_and_probe);
  RUN(test_update_merge);
  RUN(test_update_offline_keeps_models);
  RUN(test_mdstream);
  RUN(test_mdstream_spacing);
  RUN(test_mdstream_code_bar);
  RUN(test_routing);
  RUN(test_fstools);
  RUN(test_activity_and_language);
  RUN(test_policy);
  RUN(test_skills_and_urls);
  RUN(test_project_skills);
  RUN(test_router_add_and_broken);
  RUN(test_agents_plan);
  RUN(test_delegate);
  RUN(test_compaction_bounds);

  std::cout << "\n" << g_passed << " cek lolos, " << g_failed << " gagal\n";
  return g_failed == 0 ? 0 : 1;
}
