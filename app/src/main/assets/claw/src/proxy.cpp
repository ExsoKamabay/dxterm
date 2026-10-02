#include "proxy.hpp"

#include <condition_variable>

#include "httplib.h"

namespace claw {

std::string completion_to_sse(const json& completion, bool include_usage) {
  std::string out;
  auto emit = [&](const json& chunk) { out += "data: " + chunk.dump() + "\n\n"; };

  std::string id = completion.value("id", "chatcmpl-" + random_hex(12));
  int64_t created = completion.value("created", unix_time());
  std::string model = completion.value("model", "auto");
  auto base = [&]() {
    return json{{"id", id}, {"object", "chat.completion.chunk"}, {"created", created},
                {"model", model}};
  };

  const json choices = completion.value("choices", json::array());
  for (size_t ci = 0; ci < choices.size(); ++ci) {
    const json& ch = choices[ci];
    const json msg = ch.value("message", json::object());
    int index = ch.value("index", static_cast<int>(ci));

    json c = base();
    c["choices"] = json::array({{{"index", index},
                                 {"delta", {{"role", "assistant"}, {"content", ""}}},
                                 {"finish_reason", nullptr}}});
    emit(c);

    if (msg.contains("reasoning_content") && msg["reasoning_content"].is_string() &&
        !msg["reasoning_content"].get<std::string>().empty()) {
      json r = base();
      r["choices"] = json::array({{{"index", index},
                                   {"delta", {{"reasoning_content", msg["reasoning_content"]}}},
                                   {"finish_reason", nullptr}}});
      emit(r);
    }
    if (msg.contains("content") && msg["content"].is_string() &&
        !msg["content"].get<std::string>().empty()) {
      json t = base();
      t["choices"] = json::array({{{"index", index},
                                   {"delta", {{"content", msg["content"]}}},
                                   {"finish_reason", nullptr}}});
      emit(t);
    }
    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
      int ti = 0;
      for (const auto& tc : msg["tool_calls"]) {
        json fn = tc.value("function", json::object());
        json d = {{"tool_calls",
                   json::array({{{"index", ti},
                                 {"id", tc.value("id", "call_" + random_hex(8))},
                                 {"type", "function"},
                                 {"function",
                                  {{"name", fn.value("name", "")},
                                   {"arguments", fn.value("arguments", "{}")}}}}})}};
        json t = base();
        t["choices"] = json::array({{{"index", index}, {"delta", d}, {"finish_reason", nullptr}}});
        emit(t);
        ++ti;
      }
    }
    json f = base();
    f["choices"] = json::array({{{"index", index},
                                 {"delta", json::object()},
                                 {"finish_reason", ch.value("finish_reason", "stop")}}});
    emit(f);
  }
  if (include_usage && completion.contains("usage")) {
    json u = base();
    u["choices"] = json::array();
    u["usage"] = completion["usage"];
    emit(u);
  }
  out += "data: [DONE]\n\n";
  return out;
}

ProxyServer::ProxyServer(Router& router, std::string token)
    : router_(router), token_(std::move(token)) {
  reset_server();
}

void ProxyServer::reset_server() {
  // httplib menandai Server sebagai tidak terpakai setelah bind gagal, jadi
  // setiap percobaan port butuh objek Server baru.
  server_ = std::make_unique<httplib::Server>();
  setup_routes();
}

ProxyServer::~ProxyServer() { stop(); }

namespace {

struct PendingResult {
  std::mutex mu;
  std::condition_variable cv;
  bool ready = false;
  ChatResult result;
};

json error_body(const std::string& msg, const std::string& type) {
  return {{"error", {{"message", msg}, {"type", type}}}};
}

}  // namespace

void ProxyServer::setup_routes() {
  auto& srv = *server_;
  // Default httplib memasang SO_REUSEPORT sehingga dua proxy bisa berbagi port
  // dan request terbagi acak. Proxy harus memiliki port-nya sendiri.
  srv.set_socket_options([](socket_t sock) {
#ifdef _WIN32
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes),
               sizeof(yes));
#else
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
  });
  srv.set_read_timeout(300, 0);
  srv.set_write_timeout(300, 0);
  srv.set_payload_max_length(64 * 1024 * 1024);

  auto authorized = [this](const httplib::Request& req, httplib::Response& res) {
    if (token_.empty()) return true;
    std::string h = req.get_header_value("Authorization");
    if (h == "Bearer " + token_) return true;
    res.status = 401;
    res.set_content(error_body("invalid proxy token", "authentication_error").dump(),
                    "application/json");
    return false;
  };

  srv.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(R"({"ok":true})", "application/json");
  });

  auto models_handler = [this, authorized](const httplib::Request& req, httplib::Response& res) {
    if (!authorized(req, res)) return;
    json data = json::array();
    data.push_back({{"id", "auto"}, {"object", "model"}, {"created", 0}, {"owned_by", "clawpool"}});
    for (const auto& n : router_.names())
      data.push_back({{"id", n}, {"object", "model"}, {"created", 0}, {"owned_by", "clawpool"}});
    res.set_content(json{{"object", "list"}, {"data", data}}.dump(), "application/json");
  };
  srv.Get("/v1/models", models_handler);
  srv.Get("/models", models_handler);

  srv.Get("/status", [this, authorized](const httplib::Request& req, httplib::Response& res) {
    if (!authorized(req, res)) return;
    json arr = json::array();
    for (const auto& s : router_.status())
      arr.push_back({{"model", s.name},
                     {"current", s.current},
                     {"cooldown_left_sec", static_cast<int>(s.cooldown_left)},
                     {"last_failure", s.last_failure},
                     {"ok", s.ok},
                     {"failed", s.failed},
                     {"tokens", s.tokens}});
    res.set_content(json{{"current", router_.current()}, {"models", arr}}.dump(2),
                    "application/json");
  });

  auto chat_handler = [this, authorized](const httplib::Request& req, httplib::Response& res) {
    if (!authorized(req, res)) return;
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object() || !body.contains("messages") ||
        !body["messages"].is_array()) {
      res.status = 400;
      res.set_content(error_body("body harus JSON dengan field messages[]", "invalid_request_error").dump(),
                      "application/json");
      return;
    }
    std::string model = body.value("model", "auto");
    // Model di luar pool (mis. "clawpool/auto" atau nama lama) diperlakukan sebagai auto.
    bool known = model == "auto";
    for (const auto& n : router_.names())
      if (n == model) known = true;
    if (!known) model = "auto";

    bool stream = body.value("stream", false);
    bool include_usage = body.contains("stream_options") && body["stream_options"].is_object() &&
                         body["stream_options"].value("include_usage", false);

    if (!stream) {
      ChatResult r = router_.complete(body, model);
      res.status = r.status;
      if (!r.model.empty()) res.set_header("X-Claw-Model", r.model);
      res.set_content(r.body.dump(), "application/json");
      return;
    }

    // Streaming: router berjalan di thread terpisah, sementara klien dikirimi
    // komentar keep-alive agar koneksi tidak dianggap mati selama menunggu.
    auto pending = std::make_shared<PendingResult>();
    Router* router = &router_;
    auto inflight = inflight_;
    inflight->fetch_add(1);
    std::thread([pending, router, body, model, inflight] {
      ChatResult r = router->complete(body, model);
      inflight->fetch_sub(1);
      std::lock_guard<std::mutex> lk(pending->mu);
      pending->result = std::move(r);
      pending->ready = true;
      pending->cv.notify_all();
    }).detach();

    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider(
        "text/event-stream", [pending, include_usage](size_t, httplib::DataSink& sink) {
          std::unique_lock<std::mutex> lk(pending->mu);
          if (!pending->cv.wait_for(lk, std::chrono::seconds(5), [&] { return pending->ready; })) {
            lk.unlock();
            static const std::string ka = ": keep-alive\n\n";
            return sink.write(ka.data(), ka.size());
          }
          ChatResult r = pending->result;
          lk.unlock();
          std::string payload;
          if (r.status == 200) {
            payload = completion_to_sse(r.body, include_usage);
          } else {
            payload = "data: " + r.body.dump() + "\n\n";
          }
          if (!sink.write(payload.data(), payload.size())) return false;
          sink.done();
          return true;
        });
  };
  srv.Post("/v1/chat/completions", chat_handler);
  srv.Post("/chat/completions", chat_handler);
}

int ProxyServer::start(const std::string& host, int port) {
  for (int p = port; p < port + 20 && p <= 65535; ++p) {
    if (server_->bind_to_port(host, p)) {
      port_ = p;
      thread_ = std::thread([this] { server_->listen_after_bind(); });
      server_->wait_until_ready();
      return p;
    }
    Log::debug("port " + std::to_string(p) + " sibuk, mencoba port berikutnya");
    reset_server();
  }
  return -1;
}

bool ProxyServer::listen_blocking(const std::string& host, int port) {
  if (!server_->bind_to_port(host, port)) return false;
  port_ = port;
  return server_->listen_after_bind();
}

void ProxyServer::stop() {
  if (server_) server_->stop();
  if (thread_.joinable()) thread_.join();
}

}  // namespace claw
