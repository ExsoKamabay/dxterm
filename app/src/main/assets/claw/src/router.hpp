// Router: mengirim chat completion ke model di pool, dan otomatis pindah ke
// model berikutnya saat model aktif kehabisan token/kuota, kena rate limit,
// atau error. Riwayat percakapan dikirim ulang utuh ke model pengganti, jadi
// sesi percakapan berlanjut tanpa terputus.
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "http.hpp"

namespace claw {

enum class Failure {
  None,
  RateLimit,   // 429 sementara
  Quota,       // token/kuota/kredit habis
  Auth,        // key ditolak
  Context,     // prompt terlalu panjang untuk model ini
  Server,      // 5xx / timeout upstream
  Network,     // koneksi gagal
  BadRequest,  // 4xx lain (model hilang, parameter tidak didukung)
  Empty,       // 200 tapi tanpa isi jawaban
};

const char* failure_name(Failure f);

// Klasifikasi respons upstream. Dipakai router dan scanner.
Failure classify_response(int status, const std::string& body);

// Rapikan request dari klien agar diterima sebanyak mungkin provider gratis.
json sanitize_request(const json& req, const std::string& upstream_id, int max_tokens_cap);

// Potong riwayat lama (pesan system tetap). Mengembalikan false jika tidak ada
// lagi yang bisa dipotong.
bool trim_history(json& messages);

struct RouterOptions {
  int quota_cooldown_sec = 3600;
  int rate_limit_cooldown_sec = 60;
  int server_cooldown_sec = 30;
  int bad_request_cooldown_sec = 300;
  int auth_cooldown_sec = 3600;
  int read_timeout_sec = 120;
  int total_budget_sec = 540;   // batas waktu total satu request (semua percobaan)
  int max_tokens_cap = 8192;
  int max_trims = 4;
  std::string dump_dir;         // jika diisi, request terakhir disimpan di sini
  // Jika diisi, cooldown dan model aktif disimpan ke file ini sehingga model
  // yang kuotanya habis tetap dilewati saat runner dijalankan ulang.
  std::string state_file;
};

struct ChatResult {
  int status = 0;             // status HTTP untuk klien
  json body;                  // respons (sukses) atau objek error
  std::string model;          // model yang akhirnya menjawab
  std::vector<std::string> attempts;  // jejak percobaan untuk log
};

struct ModelStatus {
  std::string name;
  bool current = false;
  double cooldown_left = 0;
  std::string last_failure;
  int ok = 0;
  int failed = 0;
  int64_t tokens = 0;
};

class Router {
 public:
  Router(const Catalog& catalog, const std::vector<ModelRef>& models, RouterOptions opt = {});

  // requested_model: "auto" atau nama model di pool (dijadikan model aktif).
  ChatResult complete(const json& request, const std::string& requested_model = "auto");

  // Hasil streaming: content dialirkan lewat on_delta saat tiba; setelah selesai
  // content berisi teks penuh dan tool_calls berisi panggilan tool (bila ada).
  struct StreamResult {
    int status = 0;
    std::string model;
    std::string content;
    json tool_calls = json::array();
    json error;
    std::vector<std::string> attempts;
  };
  using DeltaSink = std::function<void(const std::string& text)>;
  // Sama seperti complete() tetapi mengalirkan jawaban. on_delta dipanggil per
  // potongan teks; on_first_content dipanggil sekali sebelum teks pertama (untuk
  // menghentikan animasi loading). Auto-switch hanya terjadi sebelum ada teks/tool.
  StreamResult stream(const json& request, const std::string& requested_model,
                      const DeltaSink& on_delta, const std::function<void()>& on_first_content);

  std::vector<ModelStatus> status() const;
  std::vector<std::string> names() const;
  std::string current() const;
  bool set_current(const std::string& name);
  size_t size() const { return entries_.size(); }

  // Jumlah model yang tidak sedang istirahat (siap dipakai sekarang).
  size_t usable() const;

  // Tambahkan model baru ke pool saat runner berjalan (dipakai auto-repair).
  // Model yang sudah ada atau providernya tidak dikenal dilewati. Mengembalikan
  // jumlah model yang benar-benar ditambahkan. Aman dipanggil dari thread lain.
  int add_models(const Catalog& catalog, const std::vector<ModelRef>& models);

  // Dipanggil setiap kali model aktif berpindah.
  std::function<void(const std::string& from, const std::string& to, const std::string& reason)>
      on_switch;

  // Dipanggil sekali saat sebuah model token/kuotanya habis atau key-nya ditolak
  // (kandidat untuk diperbaiki lewat auto-repair). failure = nama kegagalan.
  std::function<void(const std::string& model, const std::string& failure)> on_broken;

 private:
  struct Entry {
    ModelRef ref;
    std::string chat_url;
    double cooldown_until = 0;
    Failure last_failure = Failure::None;
    int ok = 0;
    int failed = 0;
    int64_t tokens = 0;
  };

  std::vector<size_t> plan_order(size_t start, bool& has_cooling) const;
  void mark_failure(size_t idx, Failure f, const HttpResponse& res);
  void mark_success(size_t idx, const json& body, const std::string& prior_reason);
  int cooldown_for(Failure f, const HttpResponse& res) const;
  void load_state();
  void save_state() const;

  RouterOptions opt_;
  mutable std::mutex mu_;
  std::vector<Entry> entries_;
  size_t current_ = 0;
};

}  // namespace claw
