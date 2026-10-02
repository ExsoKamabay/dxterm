// Scanner: menemukan model gratis di setiap provider, menguji tiap model dengan
// request chat sungguhan, dan hanya menyimpan model yang menjawab HTTP 200.
#pragma once

#include <set>
#include <utility>

#include <string>
#include <vector>

#include "catalog.hpp"

namespace claw {

struct ProbeResult {
  ModelRef ref;
  int status = 0;
  std::string failure;   // kosong jika lolos
  std::string error;     // cuplikan pesan error
  double latency = 0;
  bool tools_ok = false;
  bool passed = false;
  bool rate_limited = false;  // ditunda ke putaran ulang
  int retry_after = 0;
};

struct ScanOptions {
  bool probe_tools = true;
  std::vector<std::string> only_providers;  // kosong = semua
  int rate_limit_retries = 2;
  int max_wait_sec = 40;
  // Putaran pertama: model yang kena rate limit tidak ditunggu, tapi diuji
  // ulang setelah semua model lain selesai.
  bool defer_rate_limited = false;
  // Tanpa cetak ke stdout (dipakai update otomatis di latar belakang).
  bool quiet = false;
};

// Ambil daftar kandidat model dari provider (tanpa menguji).
std::vector<std::string> discover_models(const Provider& p, std::string* error = nullptr);

// Uji satu model. Dipakai scan dan check.
ProbeResult probe_model(const ModelRef& ref, const Provider& p, const ScanOptions& opt);

// Kegagalan yang berarti model memang tidak bisa dipakai lagi (key ditolak,
// model tidak dikenal provider, jawaban kosong). Rate limit, kuota, server error,
// dan jaringan dianggap sementara: model seperti itu tidak dibuang.
bool is_permanent_failure(const std::string& failure);

struct MergeOutcome {
  std::vector<ModelRef> models;                                // daftar baru, urut prioritas
  std::vector<std::string> added;                              // model baru yang lolos
  std::vector<std::pair<std::string, std::string>> removed;    // nama + alasan
  std::vector<std::string> kept_unverified;                    // gagal sementara, tetap disimpan
};

// Gabungkan hasil uji dengan daftar lama. Model yang lolos diurutkan (tool calling
// dulu, lalu tercepat). Model lama yang gagal hanya dibuang kalau gagalnya
// permanen, atau kalau daftar model provider berhasil dibaca dan model itu sudah
// tidak ada di sana. `listed` berisi nama model yang masih ditawarkan provider;
// `listed_providers` berisi provider yang daftarnya berhasil dibaca.
MergeOutcome merge_update(const std::vector<ModelRef>& existing, const std::vector<ProbeResult>& results,
                          const std::set<std::string>& listed, const std::set<std::string>& listed_providers);

struct UpdateResult {
  bool written = false;      // models.json diperbarui
  int tested = 0;
  int passed = 0;
  MergeOutcome merge;
  std::string error;         // alasan models.json tidak disentuh
};

// Perbarui daftar model: cari model gratis di semua provider, uji semuanya, lalu
// gabungkan dengan aman ke models.json. Daftar lama tidak pernah dikosongkan:
// kalau tidak ada satu pun model yang lolos (misalnya sedang offline), file tidak
// ditulis. Hanya satu update yang berjalan pada satu waktu, juga antar proses.
UpdateResult run_update(Catalog& catalog, const ScanOptions& opt);

// Waktu (unix) update terakhir yang berhasil, 0 bila belum pernah.
int64_t last_update_time(const Catalog& catalog);

// Scan penuh: sama dengan run_update, dengan keluaran ke terminal.
// Mengembalikan jumlah model yang lolos.
int run_scan(Catalog& catalog, const ScanOptions& opt);

// Uji ulang isi models.json. Jika prune = true, model yang gagal permanen dihapus;
// model yang hanya kena rate limit atau gangguan sementara tetap disimpan.
// Mengembalikan jumlah model yang gagal.
int run_check(Catalog& catalog, const ScanOptions& opt, bool prune);

}  // namespace claw
