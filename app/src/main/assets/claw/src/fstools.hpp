// Alat pengelolaan berkas/folder untuk `claw chat`, dipakai model lewat tool call
// saat izin fs_access di policy != "off". Semua bekerja di dalam sandbox guest,
// sebagai user Linux yang sedang menjalankan claw. Setiap fungsi mengembalikan
// objek JSON {ok, ...} dan tidak pernah melempar.
#pragma once

#include <string>

#include "json.hpp"
#include "util.hpp"

namespace claw {

using json = nlohmann::json;

// Tolak path kosong, berisi karakter kontrol, atau melewati batas panjang.
// Mengembalikan alasan penolakan, atau "" bila boleh.
std::string fs_precheck(const std::string& path);

// Daftar isi folder (read-only). Mengembalikan {ok, path, entries:[{name,type,size}]}.
json fs_list(const fs::path& base, const std::string& path, size_t max_entries = 300);

// Cari berkas/folder yang namanya memuat query (read-only, tidak mengikuti symlink,
// dibatasi kedalaman dan jumlah). {ok, matches:[relpath...]}.
json fs_find(const fs::path& base, const std::string& start, const std::string& query,
             size_t max_hits = 100, int max_depth = 8);

// Baca isi berkas teks (read-only), dipotong sampai max_bytes. {ok, path, size, truncated, content}.
json fs_read(const fs::path& base, const std::string& path, size_t max_bytes = 64 * 1024);

// Tulis (buat/timpa) berkas teks. {ok, path, bytes}.
json fs_write(const fs::path& base, const std::string& path, const std::string& content);

// Pindah atau ubah nama. {ok, from, to}.
json fs_move(const fs::path& base, const std::string& src, const std::string& dst);

// Buat folder (beserta induknya). {ok, path}.
json fs_mkdir(const fs::path& base, const std::string& path);

// Resolusi path relatif terhadap base (HOME/cwd guest). Path absolut dipakai apa
// adanya. Dipublikasikan untuk pengujian.
fs::path fs_resolve(const fs::path& base, const std::string& path);

}  // namespace claw
