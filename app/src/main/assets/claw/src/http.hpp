// Klien HTTP(S) tipis di atas cpp-httplib.
#pragma once

#include <functional>
#include <map>
#include <string>

namespace claw {

struct HttpResponse {
  int status = 0;          // 0 berarti gagal di level jaringan
  std::string body;
  std::string error;       // pesan error jaringan bila status == 0
  std::map<std::string, std::string> headers;  // nama header huruf kecil
  double elapsed = 0.0;    // detik
};

struct HttpOptions {
  int connect_timeout_sec = 15;
  int read_timeout_sec = 180;
  std::string bearer;      // kosong berarti tanpa Authorization
};

HttpResponse http_get(const std::string& url, const HttpOptions& opt = {});
HttpResponse http_post_json(const std::string& url, const std::string& body,
                            const HttpOptions& opt = {});

// POST streaming: on_chunk dipanggil untuk setiap potongan body respons saat tiba.
// Kembalikan false dari on_chunk untuk membatalkan. HttpResponse.body kosong (isi
// dialirkan lewat callback); status/headers terisi. Dipakai chat streaming.
using ChunkSink = std::function<bool(const char* data, size_t len)>;
HttpResponse http_post_stream(const std::string& url, const std::string& body, const HttpOptions& opt,
                              const ChunkSink& on_chunk);

// Pecah URL menjadi "scheme://host:port" dan path.
bool split_url(const std::string& url, std::string& origin, std::string& path);

}  // namespace claw
