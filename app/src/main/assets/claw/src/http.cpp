#include "http.hpp"

#include "httplib.h"
#include "util.hpp"

namespace claw {

bool split_url(const std::string& url, std::string& origin, std::string& path) {
  auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) return false;
  auto path_start = url.find('/', scheme_end + 3);
  if (path_start == std::string::npos) {
    origin = url;
    path = "/";
  } else {
    origin = url.substr(0, path_start);
    path = url.substr(path_start);
  }
  return !origin.empty();
}

namespace {

HttpResponse perform(const std::string& method, const std::string& url, const std::string& body,
                     const HttpOptions& opt) {
  HttpResponse out;
  std::string origin, path;
  if (!split_url(url, origin, path)) {
    out.error = "URL tidak valid: " + url;
    return out;
  }
  double start = now_seconds();
  try {
    httplib::Client cli(origin);
    if (!cli.is_valid()) {
      out.error = "klien HTTP tidak valid untuk " + origin;
      return out;
    }
    cli.set_connection_timeout(opt.connect_timeout_sec, 0);
    cli.set_read_timeout(opt.read_timeout_sec, 0);
    cli.set_write_timeout(60, 0);
    cli.set_follow_location(true);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    cli.enable_server_certificate_verification(true);
#endif
    httplib::Headers headers = {{"Accept", "application/json"},
                                {"User-Agent", "claw-runner/1.0"}};
    if (!opt.bearer.empty()) headers.emplace("Authorization", "Bearer " + opt.bearer);

    httplib::Result res = method == "GET"
                              ? cli.Get(path, headers)
                              : cli.Post(path, headers, body, "application/json");
    out.elapsed = now_seconds() - start;
    if (!res) {
      out.error = httplib::to_string(res.error());
      return out;
    }
    out.status = res->status;
    out.body = res->body;
    for (const auto& h : res->headers) out.headers[to_lower(h.first)] = h.second;
  } catch (const std::exception& e) {
    out.elapsed = now_seconds() - start;
    out.error = e.what();
  }
  return out;
}

}  // namespace

HttpResponse http_get(const std::string& url, const HttpOptions& opt) {
  return perform("GET", url, "", opt);
}

HttpResponse http_post_json(const std::string& url, const std::string& body,
                            const HttpOptions& opt) {
  return perform("POST", url, body, opt);
}

HttpResponse http_post_stream(const std::string& url, const std::string& body, const HttpOptions& opt,
                              const ChunkSink& on_chunk) {
  HttpResponse out;
  std::string origin, path;
  if (!split_url(url, origin, path)) {
    out.error = "URL tidak valid: " + url;
    return out;
  }
  double start = now_seconds();
  try {
    httplib::Client cli(origin);
    if (!cli.is_valid()) {
      out.error = "klien HTTP tidak valid untuk " + origin;
      return out;
    }
    cli.set_connection_timeout(opt.connect_timeout_sec, 0);
    cli.set_read_timeout(opt.read_timeout_sec, 0);
    cli.set_write_timeout(60, 0);
    cli.set_follow_location(true);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    cli.enable_server_certificate_verification(true);
#endif
    httplib::Headers headers = {{"Accept", "text/event-stream"},
                                {"User-Agent", "claw-runner/1.0"}};
    if (!opt.bearer.empty()) headers.emplace("Authorization", "Bearer " + opt.bearer);

    // Sink membungkus callback pengguna; false membatalkan transfer.
    auto receiver = [&](const char* data, size_t len) -> bool { return on_chunk(data, len); };
    httplib::Result res = cli.Post(path, headers, body, "application/json", receiver);
    out.elapsed = now_seconds() - start;
    if (!res) {
      out.error = httplib::to_string(res.error());
      return out;
    }
    out.status = res->status;
    for (const auto& h : res->headers) out.headers[to_lower(h.first)] = h.second;
  } catch (const std::exception& e) {
    out.elapsed = now_seconds() - start;
    out.error = e.what();
  }
  return out;
}

}  // namespace claw
