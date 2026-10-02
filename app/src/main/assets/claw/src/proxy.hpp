// Proxy lokal OpenAI-compatible (http://127.0.0.1:<port>/v1) di depan Router.
// OpenClaw diarahkan ke sini sebagai satu provider bernama "clawpool".
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "router.hpp"

namespace httplib {
class Server;
}

namespace claw {

// Ubah respons chat.completion utuh menjadi rangkaian event SSE
// chat.completion.chunk (termasuk tool_calls dan usage), diakhiri [DONE].
std::string completion_to_sse(const json& completion, bool include_usage);

class ProxyServer {
 public:
  ProxyServer(Router& router, std::string token);
  ~ProxyServer();

  // Mulai di thread latar. Jika port sibuk, coba hingga 20 port berikutnya.
  // Mengembalikan port yang dipakai, atau -1 jika gagal.
  int start(const std::string& host, int port);
  // Jalankan di thread saat ini (blocking) sampai stop() dipanggil.
  bool listen_blocking(const std::string& host, int port);
  void stop();
  int port() const { return port_; }
  // Jumlah request streaming yang masih diproses di thread latar.
  int inflight() const { return inflight_->load(); }

 private:
  void setup_routes();
  void reset_server();

  Router& router_;
  std::string token_;
  std::unique_ptr<httplib::Server> server_;
  std::thread thread_;
  int port_ = -1;
  std::shared_ptr<std::atomic<int>> inflight_ = std::make_shared<std::atomic<int>>(0);
};

}  // namespace claw
