#include "fstools.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace claw {

namespace {

json err(const std::string& msg) { return json{{"ok", false}, {"error", msg}}; }

std::string type_of(const fs::directory_entry& e, std::error_code& ec) {
  if (e.is_symlink(ec)) return "symlink";
  if (e.is_directory(ec)) return "dir";
  if (e.is_regular_file(ec)) return "file";
  return "other";
}

}  // namespace

std::string fs_precheck(const std::string& path) {
  if (trim(path).empty()) return "path kosong";
  if (path.size() > 4096) return "path terlalu panjang";
  for (char c : path)
    if (static_cast<unsigned char>(c) < 0x20) return "path berisi karakter kontrol";
  return "";
}

fs::path fs_resolve(const fs::path& base, const std::string& path) {
  fs::path p(path);
  if (p.is_absolute()) return p.lexically_normal();
  fs::path b = base.empty() ? fs::current_path() : base;
  return (b / p).lexically_normal();
}

json fs_list(const fs::path& base, const std::string& path, size_t max_entries) {
  std::string pre = fs_precheck(path);
  if (!pre.empty()) return err(pre);
  fs::path dir = fs_resolve(base, path);
  std::error_code ec;
  if (!fs::exists(dir, ec)) return err("tidak ada: " + dir.string());
  if (!fs::is_directory(dir, ec)) return err("bukan folder: " + dir.string());
  json entries = json::array();
  size_t n = 0;
  bool more = false;
  for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (n >= max_entries) {
      more = true;
      break;
    }
    std::error_code e2;
    std::string t = type_of(*it, e2);
    json row = {{"name", it->path().filename().string()}, {"type", t}};
    if (t == "file") {
      auto sz = it->file_size(e2);
      if (!e2) row["size"] = static_cast<std::uint64_t>(sz);
    }
    entries.push_back(row);
    ++n;
  }
  std::sort(entries.begin(), entries.end(), [](const json& a, const json& b) {
    return a.value("name", "") < b.value("name", "");
  });
  return json{{"ok", true}, {"path", dir.string()}, {"count", n}, {"more", more}, {"entries", entries}};
}

json fs_find(const fs::path& base, const std::string& start, const std::string& query, size_t max_hits,
             int max_depth) {
  if (trim(query).empty()) return err("query kosong");
  std::string pre = fs_precheck(start.empty() ? "." : start);
  if (!pre.empty()) return err(pre);
  fs::path root = fs_resolve(base, start.empty() ? "." : start);
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return err("bukan folder: " + root.string());
  std::string needle = to_lower(query);
  json hits = json::array();
  bool more = false;

  // BFS manual dengan batas kedalaman, lewati symlink agar tidak berputar.
  std::vector<std::pair<fs::path, int>> stack = {{root, 0}};
  while (!stack.empty()) {
    auto [cur, depth] = stack.back();
    stack.pop_back();
    std::error_code e;
    for (fs::directory_iterator it(cur, fs::directory_options::skip_permission_denied, e), end;
         !e && it != end; it.increment(e)) {
      const fs::path& p = it->path();
      if (contains_ci(p.filename().string(), needle)) {
        hits.push_back(fs::relative(p, root, e).string());
        if (hits.size() >= max_hits) {
          more = true;
          break;
        }
      }
      std::error_code e2;
      if (depth + 1 <= max_depth && it->is_directory(e2) && !it->is_symlink(e2))
        stack.push_back({p, depth + 1});
    }
    if (more) break;
  }
  return json{{"ok", true}, {"base", root.string()}, {"count", hits.size()}, {"more", more}, {"matches", hits}};
}

json fs_read(const fs::path& base, const std::string& path, size_t max_bytes) {
  std::string pre = fs_precheck(path);
  if (!pre.empty()) return err(pre);
  fs::path f = fs_resolve(base, path);
  std::error_code ec;
  if (!fs::exists(f, ec)) return err("tidak ada: " + f.string());
  if (fs::is_directory(f, ec)) return err("ini folder, bukan berkas: " + f.string());
  std::ifstream in(f, std::ios::binary);
  if (!in) return err("tidak bisa membuka: " + f.string());
  std::string data(max_bytes + 1, '\0');
  in.read(&data[0], static_cast<std::streamsize>(max_bytes + 1));
  size_t got = static_cast<size_t>(in.gcount());
  bool truncated = got > max_bytes;
  if (truncated) got = max_bytes;
  data.resize(got);
  return json{{"ok", true}, {"path", f.string()}, {"size", got}, {"truncated", truncated}, {"content", data}};
}

json fs_write(const fs::path& base, const std::string& path, const std::string& content) {
  std::string pre = fs_precheck(path);
  if (!pre.empty()) return err(pre);
  fs::path f = fs_resolve(base, path);
  std::error_code ec;
  if (fs::is_directory(f, ec)) return err("ini folder, bukan berkas: " + f.string());
  if (f.has_parent_path()) fs::create_directories(f.parent_path(), ec);
  std::ofstream out(f, std::ios::binary | std::ios::trunc);
  if (!out) return err("tidak bisa menulis: " + f.string());
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.close();
  if (!out) return err("gagal saat menulis: " + f.string());
  return json{{"ok", true}, {"path", f.string()}, {"bytes", content.size()}};
}

json fs_move(const fs::path& base, const std::string& src, const std::string& dst) {
  std::string p1 = fs_precheck(src), p2 = fs_precheck(dst);
  if (!p1.empty()) return err(p1);
  if (!p2.empty()) return err(p2);
  fs::path s = fs_resolve(base, src), d = fs_resolve(base, dst);
  std::error_code ec;
  if (!fs::exists(s, ec)) return err("sumber tidak ada: " + s.string());
  // Jika tujuan folder yang ada, pindahkan ke dalamnya dengan nama asli.
  if (fs::is_directory(d, ec)) d = d / s.filename();
  if (fs::exists(d, ec)) return err("tujuan sudah ada: " + d.string());
  if (d.has_parent_path()) fs::create_directories(d.parent_path(), ec);
  fs::rename(s, d, ec);
  if (ec) {
    // rename lintas perangkat gagal: salin lalu hapus.
    std::error_code ec2;
    fs::copy(s, d, fs::copy_options::recursive, ec2);
    if (ec2) return err("gagal memindah: " + ec.message());
    fs::remove_all(s, ec2);
  }
  return json{{"ok", true}, {"from", s.string()}, {"to", d.string()}};
}

json fs_mkdir(const fs::path& base, const std::string& path) {
  std::string pre = fs_precheck(path);
  if (!pre.empty()) return err(pre);
  fs::path d = fs_resolve(base, path);
  std::error_code ec;
  if (fs::exists(d, ec)) return err("sudah ada: " + d.string());
  if (!fs::create_directories(d, ec)) return err("tidak bisa membuat folder: " + ec.message());
  return json{{"ok", true}, {"path", d.string()}};
}

}  // namespace claw
