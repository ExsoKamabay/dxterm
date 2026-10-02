// claw: runner OpenClaw dengan pool model AI gratis dan auto-switch.
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <atomic>
#include <mutex>
#include <thread>

#include "activity.hpp"
#include "agents.hpp"
#include "browse.hpp"
#include "catalog.hpp"
#include "fstools.hpp"
#include "language.hpp"
#include "launcher.hpp"
#include "lineedit.hpp"
#include "mdstream.hpp"
#include "policy.hpp"
#include "route.hpp"
#include "proxy.hpp"
#include "repair.hpp"
#include "router.hpp"
#include "scanner.hpp"
#include "skills.hpp"

#ifndef _WIN32
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <chrono>
#include <ctime>
#include <fstream>

#ifndef CLAW_VERSION
#define CLAW_VERSION "dev"
#endif

using namespace claw;

namespace {

const char* kUsage = R"(claw - runner OpenClaw dengan pool model AI gratis + auto-switch

Pemakaian:
  claw [opsi] [perintah] [argumen]

Perintah:
  run [-- argumen openclaw]   Jalankan proxy lalu buka OpenClaw (default: `openclaw chat`)
  ask "<pesan>"               Satu giliran agent OpenClaw (openclaw agent exec)
  chat                        Ngobrol dan memberi perintah ke model langsung di terminal.
                              Model hanya memakai akses yang diizinkan di xset > claw
  humanizer ["<tujuan>"]      Mode riset otonom multi-agen: tujuan dipecah jadi beberapa tugas,
                              tiap tugas dikerjakan agen dengan skill yang dipilih sendiri, lalu
                              tim lanjut ke frontier penelitian berikutnya tanpa henti sampai
                              Humanizer dimatikan di xset atau Ctrl+C (aktifkan di xset > claw)
  serve                       Jalankan proxy OpenAI-compatible saja (foreground)
  update [--no-tools]         Perbarui daftar model: cari model gratis baru, uji,
                              buang yang rusak permanen (juga jalan otomatis)
  scan [--provider NAMA]...   Sama dengan update, bisa dibatasi per provider
       [--no-tools]           Lewati uji tool calling
  check [--prune]             Uji ulang models.json (--prune: hapus yang rusak permanen)
       [--provider NAMA]...   Batasi uji ulang ke provider tertentu (boleh diulang)
  browse URL [--select CSS]   Ambil isi halaman web lewat Scrapling
       [--max N] [--json]     (--setup: pasang Scrapling dari PyPI)
  skills                      Tampilkan skill yang bisa dipakai model
  lang [--learn TEKS]         Kalimat gaya bahasa Indonesia (id.jsonl); --learn menambah
       [--intent NAMA]        satu kalimat ke yang dipelajari
  watch (alias: monitor)      Tampilkan laporan aktivitas claw secara real-time (TUI)
  policy                      Tampilkan izin yang berlaku (diatur dari xset > claw)
  repair [--max N] [--tools]  Cari model gratis baru lewat Scrapling saat token
       [--no-leads]           habis/rusak, uji, dan tambahkan yang HTTP 200
  models                      Tampilkan daftar model di models.json
  doctor                      Periksa lingkungan (OpenClaw, Node, data, jaringan)
  version                     Tampilkan versi

Opsi:
  --data DIR       Folder data (berisi providers.json dan models.json)
  --port N         Port proxy (default 18899, otomatis cari port kosong)
  --workspace DIR  Workspace agent OpenClaw untuk `run` (default: direktori kerja)
  -v, --verbose    Log detail setiap percobaan model

Tanpa perintah, claw menjalankan `run`.
)";

struct Args {
  std::string command = "run";
  std::string data_dir;
  int port = 18899;
  std::string workspace;
  bool verbose = false;
  std::vector<std::string> rest;
};

Args parse_args(int argc, char** argv) {
  Args a;
  bool have_cmd = false;
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    auto need = [&](const std::string& flag) -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(flag + " butuh nilai");
      return argv[++i];
    };
    if (s == "--") {
      // Semua sesudah "--" diteruskan apa adanya (mis. ke openclaw).
      for (int j = i + 1; j < argc; ++j) a.rest.push_back(argv[j]);
      break;
    } else if (s == "--data") {
      a.data_dir = need(s);
    } else if (s == "--port") {
      std::string v = need(s);
      try {
        a.port = std::stoi(v);
      } catch (...) {
        throw std::runtime_error("--port harus angka: " + v);
      }
      if (a.port < 1 || a.port > 65535) throw std::runtime_error("--port di luar rentang 1-65535");
    } else if (s == "--workspace") {
      a.workspace = need(s);
    } else if (s == "-v" || s == "--verbose") {
      a.verbose = true;
    } else if (!have_cmd && (s == "-h" || s == "--help" || s == "help")) {
      a.command = "help";
      have_cmd = true;
    } else if (!have_cmd && (s == "-V" || s == "--version")) {
      a.command = "version";
      have_cmd = true;
    } else if (!have_cmd && s.rfind("-", 0) != 0) {
      a.command = s;
      have_cmd = true;
    } else if (!have_cmd) {
      throw std::runtime_error("opsi tidak dikenal: " + s);
    } else {
      a.rest.push_back(s);
    }
  }
  return a;
}

bool has_flag(const std::vector<std::string>& rest, const std::string& flag) {
  for (const auto& r : rest)
    if (r == flag) return true;
  return false;
}

std::vector<std::string> flag_values(const std::vector<std::string>& rest, const std::string& flag) {
  std::vector<std::string> out;
  for (size_t i = 0; i + 1 < rest.size(); ++i)
    if (rest[i] == flag) out.push_back(rest[i + 1]);
  return out;
}

// Pastikan pool tidak kosong; jika kosong jalankan scan otomatis.
std::vector<ModelRef> ensure_models(Catalog& catalog) {
  auto models = catalog.load_models();
  if (!models.empty()) return models;
  std::cout << "models.json masih kosong, menjalankan scan model gratis dulu...\n";
  ScanOptions opt;
  run_scan(catalog, opt);
  models = catalog.load_models();
  if (models.empty())
    throw std::runtime_error("tidak ada model yang lolos uji. Periksa koneksi internet lalu jalankan `claw scan`.");
  return models;
}

Policy g_policy;
// Thread latar (auto-update/auto-repair) yang masih jalan saat claw selesai.
std::atomic<int> g_background{0};

// state: nama berkas state router. chat dan humanizer memakai berkas sendiri supaya model
// aktif dan cooldown satu fitur tidak mengganggu fitur lain.
RouterOptions router_options(const Catalog& catalog, const std::string& state = "router-state.json") {
  RouterOptions ro;
  ro.state_file = (catalog.state_dir() / state).string();
  // Request terakhir (berisi isi percakapan) hanya disimpan bila pengguna mengizinkan.
  if (!get_env("CLAW_DUMP_REQUESTS").empty()) {
    if (g_policy.save_conversations)
      ro.dump_dir = catalog.state_dir().string();
    else
      Log::warn("CLAW_DUMP_REQUESTS diabaikan: menyimpan isi percakapan dimatikan di xset > claw");
  }
  return ro;
}

// Perbarui daftar model di latar belakang bila sudah waktunya. Model baru langsung
// masuk ke pool yang sedang berjalan (kalau ada).
void maybe_auto_update(const Catalog& catalog, Router* live) {
  if (!g_policy.auto_update) return;
  int64_t last = last_update_time(catalog);
  int64_t due = static_cast<int64_t>(g_policy.update_interval_hours) * 3600;
  if (last > 0 && unix_time() - last < due) return;
  Log::note("auto-update: mulai memperbarui daftar model di latar belakang");
  auto cat = std::make_shared<Catalog>(catalog);
  ++g_background;
  std::thread([cat, live] {
    try {
      ScanOptions so;
      so.quiet = true;
      UpdateResult r = run_update(*cat, so);
      if (!r.written) {
        Log::note("auto-update: " + r.error);
      } else {
        if (live && !r.merge.added.empty()) {
          std::vector<ModelRef> fresh;
          for (const auto& m : r.merge.models)
            if (std::find(r.merge.added.begin(), r.merge.added.end(), m.name) != r.merge.added.end())
              fresh.push_back(m);
          int n = live->add_models(*cat, fresh);
          if (n > 0) Log::note("auto-update: " + std::to_string(n) + " model baru langsung dipakai");
        }
      }
    } catch (const std::exception& e) {
      Log::note("auto-update error: " + std::string(e.what()));
    }
    --g_background;
  }).detach();
}

// Pasang auto-repair: saat sebuah model kehabisan token/key ditolak, jalankan
// pencarian model gratis baru di thread latar dan sisipkan ke pool. Dijalankan
// paling cepat tiap 10 menit dan hanya satu proses pada satu waktu.
// Matikan dengan CLAW_AUTO_REPAIR=0.
void attach_autorepair(Router& router, Catalog& catalog) {
  if (get_env("CLAW_AUTO_REPAIR") == "0") return;
  if (scrapling_python(catalog.data_dir().parent_path()).empty()) return;  // belum di-setup
  struct State {
    std::atomic<bool> busy{false};
    std::atomic<double> last{0};
  };
  auto st = std::make_shared<State>();
  Catalog* cat = &catalog;
  Router* rt = &router;
  router.on_broken = [st, cat, rt](const std::string& model, const std::string& why) {
    double t = now_seconds();
    if (st->busy.load()) return;
    if (t - st->last.load() < 600) return;  // debounce 10 menit
    bool expected = false;
    if (!st->busy.compare_exchange_strong(expected, true)) return;
    st->last.store(t);
    Log::info("auto-repair terpicu oleh " + model + " (" + why + ")");
    ++g_background;
    std::thread([st, cat, rt] {
      try {
        RepairOptions ro;
        ro.quiet = true;
        RepairResult r = run_repair(*cat, ro, rt);
        if (r.added > 0)
          Log::info("auto-repair menambah " + std::to_string(r.added) + " model gratis baru ke pool");
        else if (!r.error.empty())
          Log::warn("auto-repair gagal: " + r.error);
      } catch (const std::exception& e) {
        Log::warn("auto-repair error: " + std::string(e.what()));
      }
      st->busy.store(false);
      --g_background;
    }).detach();
  };
}

int cmd_repair(Catalog& catalog, const Args& a) {
  if (scrapling_python(catalog.data_dir().parent_path()).empty()) {
    std::cerr << "Scrapling belum di-setup. Jalankan:\n"
                 "  python3 -m venv scrapling/.venv\n"
                 "  scrapling/.venv/bin/pip install -e 'scrapling[ai]'\n";
    return 1;
  }
  RepairOptions ro;
  ro.probe_tools = has_flag(a.rest, "--tools");
  ro.leads = !has_flag(a.rest, "--no-leads");
  auto vals = flag_values(a.rest, "--max");
  if (!vals.empty()) {
    try {
      ro.max_test = std::max(1, std::stoi(vals.front()));
    } catch (...) {
    }
  }
  RepairResult r = run_repair(catalog, ro, nullptr);
  if (!r.error.empty()) {
    std::cerr << "repair gagal: " << r.error << "\n";
    return 1;
  }
  std::cout << "\nringkasan: " << r.discovered << " kandidat ditemukan, " << r.tested
            << " diuji, " << r.added << " ditambahkan.\n";
  if (!r.new_providers.empty()) {
    std::cout << "provider baru: ";
    for (const auto& p : r.new_providers) std::cout << p << " ";
    std::cout << "\n";
  }
  if (!r.leads.empty()) {
    std::cout << "petunjuk situs open source (periksa manual, tambahkan ke data/repair-sources.json):\n";
    for (const auto& l : r.leads) std::cout << "  " << l << "\n";
  }
  return 0;
}

int cmd_models(Catalog& catalog) {
  auto models = catalog.load_models();
  std::cout << models.size() << " model di " << catalog.models_path().string() << ":\n";
  int i = 1;
  for (const auto& m : models)
    std::cout << "  " << i++ << ". " << m.name << (m.api_key.empty() ? "" : "  [key]") << "\n";
  return 0;
}

int cmd_serve(Catalog& catalog, const Args& a) {
  auto models = ensure_models(catalog);
  // Router di heap dan sengaja tidak dihapus: thread streaming yang mungkin
  // masih berjalan saat Ctrl+C tetap aman memakainya sampai proses keluar.
  Router& router = *new Router(catalog, models, router_options(catalog));
  attach_autorepair(router, catalog);
  maybe_auto_update(catalog, &router);
  std::string token = get_env("CLAW_PROXY_TOKEN");
  if (token.empty() && g_policy.proxy_token) {
    // Tanpa token, aplikasi lain di perangkat yang sama bisa ikut memakai proxy ini.
    token = "claw-" + random_hex(16);
    fs::path tf = catalog.state_dir() / "proxy-token";
    write_file_atomic(tf, token + "\n");
#ifndef _WIN32
    chmod(tf.c_str(), 0600);
#endif
    std::cout << "token proxy (Authorization: Bearer ...): " << token << "\n"
              << "tersimpan juga di " << tf.string() << ". Matikan kewajiban token di xset > claw.\n";
  }
  ProxyServer proxy(router, token);
  std::cout << "proxy OpenAI-compatible aktif di http://127.0.0.1:" << a.port << "/v1 dengan "
            << router.size() << " model" << (token.empty() ? "" : " (butuh token)") << "\n"
            << "model: \"auto\" (auto-switch) atau salah satu nama di models.json. Ctrl+C untuk berhenti.\n";
  static ProxyServer* g_proxy = &proxy;
  std::signal(SIGINT, [](int) { g_proxy->stop(); });
  std::signal(SIGTERM, [](int) { g_proxy->stop(); });
  if (!proxy.listen_blocking("127.0.0.1", a.port)) {
    std::cerr << "gagal membuka port " << a.port << " (sudah dipakai?)\n";
    return 1;
  }
  return 0;
}

void print_status(Router& router) {
  for (const auto& s : router.status()) {
    std::cout << (s.current ? " * " : "   ") << s.name;
    if (s.cooldown_left > 0) std::cout << "  [cooldown " << static_cast<int>(s.cooldown_left) << "s]";
    if (!s.last_failure.empty()) std::cout << "  (" << s.last_failure << ")";
    if (s.ok || s.failed) std::cout << "  ok=" << s.ok << " gagal=" << s.failed << " token=" << s.tokens;
    std::cout << "\n";
  }
}

// ---- chat dengan tool: web_fetch (Scrapling) dan use_skill ----

// Satu kunci untuk layar dan pertanyaan izin: agen humanizer yang jalan bersamaan tidak
// boleh saling menimpa baris, dan hanya satu yang boleh bertanya ke pengguna pada satu waktu.
std::mutex g_io;

// Minta `claw humanizer` berhenti dari loop never-ending: diset oleh Ctrl+C (SIGINT) di mode
// otonom. Stop kedua — humanizer dimatikan di xset — dideteksi dengan membaca ulang policy.json.
std::atomic<bool> g_humanizer_stop{false};

// Jawaban "a" (izinkan untuk sesi ini). Dibagi oleh semua salinan ChatTools dalam satu sesi,
// jadi agen humanizer tidak menanyakan hal yang sama berulang kali. Dibaca/ditulis di bawah g_io.
struct Grants {
  bool browse = false;
  bool fs = false;
};

struct ChatTools {
  fs::path project_root;
  std::vector<SkillInfo> skills;
  bool browse_on = false;
  bool skills_on = false;
  std::shared_ptr<Grants> grants = std::make_shared<Grants>();
  std::string lang_block;      // contoh kalimat dari id.jsonl untuk membentuk nada
  std::string fs_access = "off";  // off/ask/allow: kelola berkas & terminal
  fs::path fs_base;            // basis path relatif (HOME guest)
  std::string agent;           // label agen humanizer ("" untuk chat), tampil di pertanyaan izin
  bool can_delegate = false;   // agen depan `claw chat`: boleh melimpahkan kerja berat ke pekerja
  // Dipasang di jalur interaktif (chat + pekerja delegasi): tiap tool memanggil ini dengan
  // deskripsi ramah ("sedang membuat berkas 'X'", "menjalankan perintah: …") supaya pengguna
  // melihat APA yang sedang dikerjakan selama menunggu — bukan hanya spinner. Kosong = diam.
  std::function<void(const std::string&)> on_activity;
  // Agen di bawah `claw humanizer`: bawa doktrin riset (ENGINEERING.md + siklus.txt). Diisi
  // oleh cmd_humanizer; agen chat biasa membiarkannya kosong.
  bool research_mode = false;
  std::string doctrine;        // blok doktrin riset ringkas untuk agen pekerja humanizer
};

json tool_specs(const ChatTools& t, bool enabled) {
  json tools = json::array();
  if (!enabled) return tools;
  if (t.browse_on)
    tools.push_back({{"type", "function"},
                     {"function",
                      {{"name", "web_fetch"},
                       {"description",
                        "Open a public web page (http/https) with Scrapling and return its title, text "
                        "and links. Use it for current information or when the user gives a URL."},
                       {"parameters",
                        {{"type", "object"},
                         {"properties",
                          {{"url", {{"type", "string"}, {"description", "Full http(s) URL"}}},
                           {"selector",
                            {{"type", "string"},
                             {"description", "Optional CSS selector to extract only matching elements"}}},
                           {"max_chars", {{"type", "integer"}, {"description", "Max text length (200-20000)"}}}}},
                         {"required", json::array({"url"})}}}}}});
  if (t.browse_on && !t.can_delegate)
    tools.push_back(
        {{"type", "function"},
         {"function",
          {{"name", "web_request"},
           {"description",
            "Interact with a web page or API: follow a link, submit a form, or call an API with GET or "
            "POST/PUT/DELETE. Use it to act on the web for the USER'S OWN authorised purposes (e.g. sign "
            "up for a free service the user wants). Respect each site's terms; you cannot solve CAPTCHAs "
            "or read verification emails, so say so when a flow needs them. Returns status, response "
            "headers (incl. set-cookie) and body."},
           {"parameters",
            {{"type", "object"},
             {"properties",
              {{"url", {{"type", "string"}, {"description", "Full http(s) URL"}}},
               {"method", {{"type", "string"}, {"enum", json::array({"GET", "POST", "PUT", "DELETE", "PATCH"})}}},
               {"body", {{"type", "string"}, {"description", "Request body: form 'k=v&k2=v2' or a JSON string"}}},
               {"content_type", {{"type", "string"}, {"enum", json::array({"form", "json"})}}},
               {"headers", {{"type", "object"}, {"description", "Extra request headers, e.g. Cookie/Authorization"}}}}},
             {"required", json::array({"url"})}}}}}});
  if (t.browse_on && !t.can_delegate)
    tools.push_back(
        {{"type", "function"},
         {"function",
          {{"name", "download_file"},
           {"description",
            "Download a file from a public http(s) URL to a path in the workspace, e.g. a library, "
            "dataset or asset needed to build the project."},
           {"parameters",
            {{"type", "object"},
             {"properties",
              {{"url", {{"type", "string"}, {"description", "Full http(s) URL of the file"}}},
               {"path", {{"type", "string"}, {"description", "Destination path (relative to the working dir)"}}}}},
             {"required", json::array({"url", "path"})}}}}}});
  if (t.fs_access != "off") {
    auto strp = [](const char* name, const char* desc) {
      return json{{"type", "string"}, {"description", desc}};
      (void)name;
    };
    auto fn = [](const std::string& name, const std::string& desc, const json& props,
                 const json& required) {
      return json{{"type", "function"},
                  {"function",
                   {{"name", name},
                    {"description", desc},
                    {"parameters", {{"type", "object"}, {"properties", props}, {"required", required}}}}}};
    };
    tools.push_back(fn("list_dir", "List the contents of a folder in the guest.",
                       {{"path", strp("path", "Folder path (relative to the working dir, or absolute)")}},
                       json::array({"path"})));
    tools.push_back(fn("find_files", "Search for files/folders whose name contains a query.",
                       {{"query", strp("query", "Text to look for in names")},
                        {"path", strp("path", "Folder to search under (default: working dir)")}},
                       json::array({"query"})));
    tools.push_back(fn("read_file", "Read a text file in the guest.",
                       {{"path", strp("path", "File path")}}, json::array({"path"})));
    // Tool yang MENGUBAH sesuatu hanya untuk pekerja; agen depan (can_delegate) tetap baca-saja
    // dan melimpahkan perubahan/eksekusi lewat delegate_task.
    if (!t.can_delegate) {
      tools.push_back(fn("run_shell", "Run a shell command in the guest terminal and return its output.",
                         {{"command", strp("command", "The /bin/sh command line to run")}},
                         json::array({"command"})));
      tools.push_back(fn("write_file", "Create or overwrite a text file with the given content.",
                         {{"path", strp("path", "File path")},
                          {"content", strp("content", "Full new file content")}},
                         json::array({"path", "content"})));
      tools.push_back(fn("move_path", "Move or rename a file or folder.",
                         {{"src", strp("src", "Source path")}, {"dst", strp("dst", "Destination path")}},
                         json::array({"src", "dst"})));
      tools.push_back(fn("make_dir", "Create a folder (with parents).",
                         {{"path", strp("path", "Folder path")}}, json::array({"path"})));
    }
  }
  if (t.can_delegate) {
    // Agen depan melimpahkan pekerjaan nyata ke agen pekerja (akses berkas/terminal penuh,
    // keahlian engineering). kind memilih spesialis + model; skills dimuat pekerja.
    json props = {{"task",
                   {{"type", "string"},
                    {"description", "The self-contained job for the worker to carry out and deliver"}}},
                  {"kind",
                   {{"type", "string"},
                    {"enum", json::array({"coding", "reasoning", "design", "general"})},
                    {"description", "Kind of work, so the right specialist and model take it"}}},
                  {"role",
                   {{"type", "string"},
                    {"description", "Optional short role for the worker, e.g. 'developer', 'auditor'"}}}};
    if (t.skills_on && !t.skills.empty()) {
      json names = json::array();
      for (const auto& sk : t.skills) names.push_back(sk.name);
      props["skills"] = {{"type", "array"},
                         {"items", {{"type", "string"}, {"enum", names}}},
                         {"description", "Skills the worker should load for this task"}};
    }
    tools.push_back(
        {{"type", "function"},
         {"function",
          {{"name", "delegate_task"},
           {"description",
            "Hand a piece of real work to your specialist worker agent: building or changing a project, "
            "writing/refactoring/optimising code, running builds/tests/commands, debugging, auditing, "
            "migrating, or any multi-step engineering task. The worker has full file and terminal access "
            "and professional engineering skills, works on its own until done, and reports back what it "
            "did (file paths, results). Use it instead of doing heavy work yourself; keep answering and "
            "reviewing directly for quick questions. When it reports back, explain the result to the user "
            "in plain language and review it against what they asked."},
           {"parameters",
            {{"type", "object"}, {"properties", props}, {"required", json::array({"task"})}}}}}});
  }
  if (t.skills_on && !t.skills.empty()) {
    json names = json::array();
    for (const auto& sk : t.skills) names.push_back(sk.name);
    tools.push_back({{"type", "function"},
                     {"function",
                      {{"name", "use_skill"},
                       {"description",
                        "Load the full instructions of one skill from the skill list. Only call it when the "
                        "skill clearly matches what the user is asking."},
                       {"parameters",
                        {{"type", "object"},
                         {"properties", {{"name", {{"type", "string"}, {"enum", names}}}}},
                         {"required", json::array({"name"})}}}}}});
  }
  return tools;
}

// Piagam kemampuan profesional yang dibawa SETIAP model claw ke pekerjaannya
// (mencakup 38 kemampuan: reasoning, planning, analysis, ... autonomous execution).
// Ditulis sebagai cara kerja, bukan sekadar daftar, supaya benar-benar mengubah perilaku.
const char* capabilities_charter() {
  return "\n\nProfessional competencies — bring all of these to bear as the task needs:"
         "\n- Think first: reason step by step, analyse the problem, research what you don't know, and "
         "decompose a big goal into ordered steps before acting (agentic planning)."
         "\n- Design before building: choose a sound architecture and system design; prefer proven "
         "libraries and patterns over reinventing them, and justify key choices briefly."
         "\n- Build well: implement cleanly, integrate the pieces, migrate and modernise legacy code, "
         "refactor for clarity, and optimise only real bottlenecks — profile and benchmark before you do."
         "\n- Find and fix: debug and troubleshoot by reproducing the problem and finding the ROOT cause "
         "before patching; use static and dynamic analysis, and reverse-engineer unknown behaviour when "
         "needed."
         "\n- Prove it: review code critically, run a security audit for the user's OWN or clearly "
         "authorised code only, then test, validate and verify against the real requirements. Keep a "
         "verification loop — run it, read the ACTUAL output, and self-correct until it truly works."
         "\n- Operate autonomously: call your tools deliberately, remember earlier context and results, "
         "reflect on whether the outcome is actually right, correct yourself, document what matters, and "
         "carry the task through to a verified finish. Automate, deploy and monitor when asked. Never "
         "claim something works unless you checked it."
         "\n\nOperating discipline (hold to these on every task):"
         "\n- OBSERVE before assuming; UNDERSTAND before modifying; find the ROOT CAUSE before patching; "
         "USE TOOLS before guessing; TEST before claiming success; CORRECT yourself when evidence "
         "disagrees; PRESERVE stable, unrelated code; keep the user informed; finish the task, not just "
         "the first step."
         "\n- Match effort to difficulty (keep it proportional). If the task is easy and well understood "
         "and can be done on THIS system, just do it now — do not browse the web, do not load a skill, and "
         "do not over-plan for a small job. Reserve web research and loading a skill for work that is "
         "genuinely unfamiliar or complex; then research the references properly before you build."
         "\n- Source of truth, in order: actual source code > runtime behaviour > test results > "
         "build/compiler output > logs > project docs > official dependency docs > assumptions. Do not "
         "treat an assumption as a fact; mark what is CONFIRMED vs LIKELY vs UNKNOWN and never promote "
         "UNKNOWN to CONFIRMED without evidence."
         "\n- Make the SMALLEST CORRECT CHANGE that fixes the root cause — not the smallest patch that "
         "hides a symptom, and not a rewrite for taste. Don't add dependencies or abstractions without a "
         "real need, and don't leave dead code."
         "\n- Interpret tool results, don't just check exit codes: read the output, warnings and real "
         "behaviour. NEVER fake success — if something is implemented but not runtime-tested, say exactly "
         "that; state uncertainty plainly instead of inventing confidence."
         "\n- Security & pentest work only inside an AUTHORISED scope: the user's own code and systems, "
         "localhost, labs/CTF, or an explicitly permitted target. Default to NON-DESTRUCTIVE actions "
         "(never delete data, take accounts, DoS, or exfiltrate real data); use least privilege; prove a "
         "finding with the smallest safe evidence; never expose full secrets (redact); and give evidence "
         "for every finding rather than guessing. Load the `security-audit`/`audit-debug` skills for the "
         "detailed methodology when a task calls for it.";
}

// Siapa yang memakai prompt: Chat = agen depan yang diajak bicara manusia (`claw chat`),
// Agent = agen pekerja yang bekerja sendiri sampai tugasnya selesai (pekerja chat / tim humanizer).
enum class PromptMode { Chat, Agent };

std::string system_prompt(const ChatTools& t, PromptMode mode = PromptMode::Chat) {
  std::string p;
  if (mode == PromptMode::Agent)
    p = "You are the WORKER agent of a claw team in the drac-Xterm Linux terminal — the hands-on project "
        "assistant. You receive one self-contained task from the team lead. Work on it on your own until "
        "it is done: plan briefly, use your tools, check the real results, fix what fails, and do not "
        "wait for the user between steps. You act as a professional across reasoning, debugging, "
        "auditing, refactoring, engineering, architecture and system design, analysis, planning, "
        "optimisation, integration, implementation and modernisation. Your final message is your report "
        "to the team lead: what you did, what you found or produced (file paths, commands, sources), and "
        "anything left open. Two hard rules: never exceed the tool permissions you are given, and never "
        "claim to have done something you did not actually do."
        "\n\nBefore you start, think hard at an exceptional, professional level (IQ-200 caliber): "
        "brainstorm several strong, non-obvious ideas or approaches for your task (for building, "
        "research or biology work alike), reason from first principles, weigh the trade-offs honestly, "
        "and commit to the best one — then raise your own bar and refine it. If you need a library, "
        "documentation or a file to do the work, research it with web_fetch/web_request and fetch it "
        "with download_file when those tools are available.";
  else
    p = "You are claw, the FRONT of a two-agent team the user talks to in the drac-Xterm Linux terminal. "
        "Your job is to understand what the user really means, hold a natural, easy conversation, answer "
        "questions, explain things clearly, and review code or data when asked. Read intent from their "
        "words even when the phrasing is casual or incomplete; if a request is genuinely ambiguous, ask "
        "one short question instead of guessing. Be fast: for anything you can answer or review directly, "
        "do it now.\n\nWhen the request needs real work done — building or changing a project, writing, "
        "refactoring or optimising code, running builds/tests/commands, debugging, auditing, migrating, "
        "or any multi-step engineering task — hand it to your worker with delegate_task instead of doing "
        "it yourself. Pick the kind of work and the skills that fit, so the right specialist and model "
        "take it; you may delegate again to correct or extend the result. Match effort to the request: "
        "when it is simple and clearly doable on this Linux system, delegate it straight away (do NOT "
        "browse the web or pick a skill for an easy job, and do not stall) — the worker just carries it "
        "out. Only for genuinely complex or unfamiliar work should the worker research references on the "
        "web and load the fitting skill first. When the worker reports back, "
        "do NOT paste its raw report: explain in plain language what was done and what changed (state any "
        "file paths), and review it against what the user asked — flag anything wrong, risky or "
        "incomplete. You keep read access (list_dir/find_files/read_file, web_fetch) to inspect and "
        "review; the worker owns writing files and running the terminal. What the team may touch (web, "
        "files, terminal, skills) is decided by the user's settings in xset > claw; if a tool is missing "
        "or refused, say so plainly instead of working around it.";
  p += "\n\nResponse style: answer directly and concisely. Do NOT mention which model "
       "you are, do NOT narrate background steps or how you did something, and do NOT show tool/plumbing "
       "details. When you create or change a file, just state its path and briefly explain what the file "
       "is or contains. Always put code (and any file contents you show) inside Markdown fenced code "
       "blocks with a language tag, e.g. ```python.";
  if (t.browse_on)
    p += " You can read web pages with web_fetch and interact with sites/APIs with web_request (follow "
         "links, submit forms, call APIs, carry cookies) for the user's own authorised purposes; cite the "
         "URLs you used. Respect each site's terms, and note that you cannot solve CAPTCHAs or read "
         "verification emails. Only public http(s) sites are reachable.";
  if (t.skills_on && !t.skills.empty()) {
    p += "\n\nSkills you can load with use_skill (load one only when it fits the request):";
    for (const auto& sk : t.skills) p += "\n- " + sk.name + ": " + sk.description;
  }
  if (mode == PromptMode::Agent && t.fs_access != "off")
    p += "\n\nWhen your task is to build something: scaffold a real project on disk, write the code, run "
         "builds/tests in the terminal, read the errors and iterate until it works. Then actively DEBUG it "
         "(run it, read the errors, fix the root cause) and AUDIT it (correctness, security, quality), "
         "loading the `audit-debug` and `security-audit` skills with use_skill when they are listed, and "
         "report what you checked. Build security tools only for the user's OWN code and systems or "
         "clearly authorised targets (CTF/labs), and never touch systems the user does not own.";
  if (t.fs_access != "off") {
    p += "\n\nYou can manage files and run the terminal in the guest Linux (as the user running claw):"
         " list_dir and find_files to explore, read_file to read, and run_shell/write_file/move_path/"
         "make_dir to change things. Prefer paths relative to the working directory. Explain what you "
         "will do before changing or running anything, keep commands minimal, and never destroy data "
         "the user did not ask you to. Report the real result, including failures.";
  } else if (mode == PromptMode::Chat) {
    // Agen depan, akses berkas/terminal MATI: JANGAN memberi perintah manual (itu membuat pekerja
    // terasa tidak bekerja). Arahkan pengguna mengaktifkan izinnya — sekali klik lalu pekerja jalan.
    p += "\n\nThe team currently has NO file or terminal access — 'Kelola berkas & terminal' is OFF in "
         "xset > AI (claw). So your worker cannot create/change files or run commands yet. For any "
         "request that needs those actions (make/edit a file, run a command, build, install, scan, "
         "test), do NOT hand the user manual shell commands to run themselves. Instead reply in one "
         "clear line: enable 'Kelola berkas & terminal' in xset > AI (claw) (set it to 'izinkan' or "
         "'tanya'), then ask again — the worker will carry it out. You can still chat, explain, review "
         "pasted code, and answer questions normally.";
  } else {
    p += "\n\nYou have no file or terminal access in this run; you can only reason and answer. If the "
         "task needs files or the terminal, say so plainly in your report: it needs 'Kelola berkas & "
         "terminal' enabled in xset > AI (claw).";
  }
  p += "\n\nWork at an expert, top-tier level.";
  p += capabilities_charter();
  if (!t.lang_block.empty()) p += "\n\n" + t.lang_block;
  return p;
}

// Tanya pengguna. always (boleh nullptr) diisi true bila menjawab "a". Pemanggil memegang g_io.
bool ask_permission_locked(const std::string& what, bool* always) {
#ifndef _WIN32
  if (!isatty(STDIN_FILENO)) return false;  // tidak ada orang yang bisa ditanya
#endif
  std::cout << "\n[izin] " << what << "\n       izinkan? [y = ya, a = selalu di sesi ini, N = tolak] "
            << std::flush;
  std::string ans;
  if (!std::getline(std::cin, ans)) return false;
  ans = to_lower(trim(ans));
  if (ans == "a" && always) {
    *always = true;
    return true;
  }
  return ans == "y" || ans == "ya" || ans == "a";
}

// Izin yang bisa diberikan untuk seluruh sesi (flag = &Grants::browse atau &Grants::fs).
bool permit(ChatTools& t, const std::string& what, bool Grants::*flag) {
  std::lock_guard<std::mutex> lk(g_io);
  if ((*t.grants).*flag) return true;
  std::string w = t.agent.empty() ? what : "[" + t.agent + "] " + what;
  return ask_permission_locked(w, &((*t.grants).*flag));
}

// Izin sekali pakai (operasi yang selalu dikonfirmasi di mode "ask").
bool permit_once(ChatTools& t, const std::string& what) {
  std::lock_guard<std::mutex> lk(g_io);
  std::string w = t.agent.empty() ? what : "[" + t.agent + "] " + what;
  return ask_permission_locked(w, nullptr);
}

std::string run_tool(ChatTools& t, Activity& act, const std::string& name, const std::string& raw_args) {
  json args = json::parse(raw_args.empty() ? "{}" : raw_args, nullptr, false);
  if (!args.is_object()) args = json::object();

  // Umumkan langkah yang sedang dikerjakan: SELALU ke umpan live (`claw watch`) dan, di jalur
  // interaktif yang memasang on_activity (chat + pekerja delegasi), juga ke layar agar pengguna
  // tahu apa yang sedang dilakukan saat menunggu.
  auto announce = [&](const std::string& desc) {
    act.live(desc);
    if (t.on_activity && !desc.empty()) t.on_activity(desc);
  };

  if (name == "web_fetch") {
    std::string url = args.value("url", "");
    if (!t.browse_on) return R"({"ok":false,"error":"browsing dimatikan oleh pengguna"})";
    std::string pre = precheck_url(url);
    if (!pre.empty()) return json({{"ok", false}, {"error", pre}}).dump();
    if (g_policy.browse == "ask" && !permit(t, "model ingin membuka " + url, &Grants::browse)) {
      Log::note("chat: web_fetch ditolak pengguna (" + url_host(url) + ")");
      return R"({"ok":false,"error":"pengguna menolak membuka halaman ini","hint":"the user refused this; do not ask again for the same action, continue without it or explain what is blocked"})";
    }
    int max_chars = 6000;
    if (args.contains("max_chars") && args["max_chars"].is_number_integer())
      max_chars = std::clamp(args["max_chars"].get<int>(), 200, 20000);
    std::string sel = args.value("selector", "");
    announce("sedang membuka halaman " + url_host(url));
    BrowseResult br = web_fetch(t.project_root, url, sel, max_chars, g_policy.private_network);
    int st = br.data.value("status", 0);
    Log::note("chat: web_fetch " + (g_policy.save_conversations ? url : url_host(url)) + " -> " +
              (br.ok ? "HTTP " + std::to_string(st) : "gagal: " + br.error));
    act.tool("web_fetch", (g_policy.save_conversations ? url : url_host(url)) + (sel.empty() ? "" : "  sel=" + sel),
             (br.ok ? "HTTP " + std::to_string(st) + ", " + std::to_string(br.data.value("text", std::string()).size()) +
                          " karakter teks diambil"
                    : "gagal: " + br.error));
    if (!br.ok && br.data.empty()) return json({{"ok", false}, {"error", br.error}}).dump();
    json d = br.data;
    if (d.contains("links") && d["links"].is_array() && d["links"].size() > 12) {
      json l = json::array();
      for (size_t i = 0; i < 12; ++i) l.push_back(d["links"][i]);
      d["links"] = l;
    }
    return d.dump();
  }

  if (name == "web_request") {
    if (!t.browse_on) return R"({"ok":false,"error":"browsing dimatikan oleh pengguna"})";
    std::string url = args.value("url", "");
    std::string method = args.value("method", "GET");
    std::string m = to_lower(method);
    std::string pre = precheck_url(url);
    if (!pre.empty()) return json({{"ok", false}, {"error", pre}}).dump();
    bool mutating = m != "get";
    // GET: perlakukan seperti web_fetch (mode "ask" menghormati "a"). Metode yang mengubah
    // (POST/PUT/DELETE/PATCH) selalu dikonfirmasi di mode "ask", tidak memakai izin sesi GET.
    if (g_policy.browse == "ask") {
      bool ok;
      if (mutating)
        ok = permit_once(t, "model ingin " + method + " ke " + url_host(url));
      else
        ok = permit(t, "model ingin membuka " + url, &Grants::browse);
      if (!ok) {
        act.tool("web_request", method + " " + url_host(url), "ditolak pengguna");
        return R"({"ok":false,"error":"pengguna menolak permintaan ini","hint":"the user refused this; do not ask again for the same action, continue without it or explain what is blocked"})";
      }
    }
    std::vector<std::pair<std::string, std::string>> headers;
    if (args.contains("headers") && args["headers"].is_object())
      for (auto it = args["headers"].begin(); it != args["headers"].end(); ++it)
        if (it.value().is_string()) headers.emplace_back(it.key(), it.value().get<std::string>());
    std::string body = args.value("body", "");
    std::string ctype = args.value("content_type", "form");
    announce("sedang " + method + " ke " + url_host(url));
    BrowseResult br = web_request(t.project_root, url, method, body, ctype, headers, 8000,
                                  g_policy.private_network);
    int st = br.data.value("status", 0);
    Log::note("chat: web_request " + method + " " + url_host(url) + " -> " +
              (br.ok ? "HTTP " + std::to_string(st) : "gagal: " + br.error));
    act.tool("web_request", method + " " + url_host(url),
             br.ok ? "HTTP " + std::to_string(st) : "gagal: " + br.error);
    if (br.data.empty()) return json({{"ok", false}, {"error", br.error}}).dump();
    return br.data.dump();
  }

  if (name == "download_file") {
    if (!t.browse_on) return R"({"ok":false,"error":"browsing dimatikan oleh pengguna"})";
    std::string url = args.value("url", "");
    std::string path = args.value("path", "");
    std::string pre = precheck_url(url);
    if (!pre.empty()) return json({{"ok", false}, {"error", pre}}).dump();
    if (trim(path).empty()) return R"({"ok":false,"error":"path tujuan kosong"})";
    fs::path dest = fs_resolve(t.fs_base, path);
    std::string nm = dest.filename().string();
    std::string dir = dest.has_parent_path() ? dest.parent_path().string() : std::string("/");
    // Mengunduh menulis berkas + memakai jaringan: konfirmasi di mode "ask".
    if (g_policy.browse == "ask") {
      if (!permit_once(t, "model ingin mengunduh " + url_host(url) + " ke '" + dest.string() + "'")) {
        act.tool("download_file", url_host(url) + " -> " + dest.string(), "ditolak pengguna");
        return R"({"ok":false,"error":"pengguna menolak unduhan ini","hint":"the user refused this; do not ask again for the same action, continue without it or explain what is blocked"})";
      }
    }
    announce("sedang mengunduh berkas '" + nm + "' lokasi '" + dir + "'");
    BrowseResult br = web_download(t.project_root, url, dest.string(), g_policy.private_network);
    int st = br.data.value("status", 0);
    Log::note("chat: download_file " + url_host(url) + " -> " +
              (br.ok ? "OK " + std::to_string(br.data.value("bytes", 0)) + "B" : "gagal: " + br.error));
    act.tool("download_file", url_host(url) + " -> " + dest.string(),
             br.ok ? "OK " + std::to_string(br.data.value("bytes", 0)) + " byte (HTTP " + std::to_string(st) + ")"
                   : "gagal: " + br.error);
    if (br.data.empty()) return json({{"ok", false}, {"error", br.error}}).dump();
    return br.data.dump();
  }

  if (name == "use_skill") {
    std::string sk = args.value("name", "");
    if (!t.skills_on) return R"({"ok":false,"error":"skill dimatikan oleh pengguna"})";
    const SkillInfo* info = find_skill(t.skills, sk);
    if (!info) return json({{"ok", false}, {"error", "skill tidak ada: " + sk}}).dump();
    announce("sedang memakai skill " + info->name);
    Log::note("chat: skill " + info->name);
    act.tool("use_skill", info->name, "memuat instruksi skill " + info->name);
    try {
      return json({{"ok", true}, {"skill", info->name}, {"instructions", read_skill(*info)}}).dump();
    } catch (const std::exception& e) {
      return json({{"ok", false}, {"error", e.what()}}).dump();
    }
  }
  // ---- pengelolaan berkas & terminal (izin fs_access) ----
  static const std::set<std::string> kFsTools = {"list_dir", "find_files", "read_file",
                                                 "run_shell", "write_file", "move_path", "make_dir"};
  if (kFsTools.count(name)) {
    if (t.fs_access == "off")
      return R"J({"ok":false,"error":"pengelolaan berkas/terminal dimatikan (xset > AI (claw))"})J";
    bool read_only = name == "list_dir" || name == "find_files" || name == "read_file";
    if (!read_only) {
      std::string what;
      if (name == "run_shell")
        what = "model ingin menjalankan di terminal: " + args.value("command", "");
      else if (name == "write_file")
        what = "model ingin menulis berkas: " + args.value("path", "");
      else if (name == "move_path")
        what = "model ingin memindah/ubah nama: " + args.value("src", "") + " -> " + args.value("dst", "");
      else if (name == "make_dir")
        what = "model ingin membuat folder: " + args.value("path", "");
      bool ok = t.fs_access == "allow" || permit(t, what, &Grants::fs);
      if (!ok) {
        act.tool(name, args.dump(), "ditolak pengguna");
        return R"({"ok":false,"error":"pengguna menolak operasi ini","hint":"the user refused this; do not ask again for the same action, continue without it or explain what is blocked"})";
      }
    }
    json result;
    std::string shown;
    {
      auto loc = [&](const std::string& pth) {
        fs::path r = fs_resolve(t.fs_base, pth);
        std::string dir = r.has_parent_path() ? r.parent_path().string() : std::string("/");
        return std::make_pair(r.filename().string(), dir);
      };
      std::string lv;
      if (name == "list_dir") {
        auto d = loc(args.value("path", "."));
        lv = "sedang menelusuri folder '" + d.first + "' lokasi '" + d.second + "'";
      } else if (name == "find_files") {
        lv = "sedang mencari '" + args.value("query", "") + "'";
      } else if (name == "read_file") {
        auto d = loc(args.value("path", ""));
        lv = "sedang membaca berkas '" + d.first + "' lokasi '" + d.second + "'";
      } else if (name == "write_file") {
        auto d = loc(args.value("path", ""));
        lv = "sedang membuat berkas '" + d.first + "' lokasi '" + d.second + "'";
      } else if (name == "move_path") {
        auto ss = loc(args.value("src", ""));
        auto dd = loc(args.value("dst", ""));
        lv = "sedang memindah '" + ss.first + "' ke '" + dd.first + "' lokasi '" + dd.second + "'";
      } else if (name == "make_dir") {
        auto d = loc(args.value("path", ""));
        lv = "sedang membuat folder '" + d.first + "' lokasi '" + d.second + "'";
      } else if (name == "run_shell") {
        lv = "sedang menjalankan perintah: " + args.value("command", "");
      }
      announce(lv);
    }
    if (name == "list_dir") {
      result = fs_list(t.fs_base, args.value("path", "."));
      shown = "list_dir " + args.value("path", ".");
    } else if (name == "find_files") {
      result = fs_find(t.fs_base, args.value("path", "."), args.value("query", ""));
      shown = "find_files " + args.value("query", "");
    } else if (name == "read_file") {
      result = fs_read(t.fs_base, args.value("path", ""));
      shown = "read_file " + args.value("path", "");
    } else if (name == "write_file") {
      result = fs_write(t.fs_base, args.value("path", ""), args.value("content", ""));
      shown = "write_file " + args.value("path", "");
    } else if (name == "move_path") {
      result = fs_move(t.fs_base, args.value("src", ""), args.value("dst", ""));
      shown = "move_path " + args.value("src", "") + " -> " + args.value("dst", "");
    } else if (name == "make_dir") {
      result = fs_mkdir(t.fs_base, args.value("path", ""));
      shown = "make_dir " + args.value("path", "");
    } else if (name == "run_shell") {
      std::string cmd = args.value("command", "");
      shown = "run_shell " + cmd;
      if (trim(cmd).empty()) {
        result = json{{"ok", false}, {"error", "perintah kosong"}};
      } else {
        std::string out;
        // PATH dipastikan (subproses tidak selalu mewarisinya) supaya perintah eksternal seperti
        // `ls` ketemu, dan stderr digabung ke stdout (2>&1) agar model melihat pesan error.
        std::string wrapped =
            "export PATH=\"${PATH:-/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin}\"; "
            "{ " + cmd + " ; } 2>&1";
        int rc = run_capture({"/bin/sh", "-c", wrapped}, 30, out);
        if (out.size() > 8000) out = out.substr(0, 8000) + "\n[... dipotong]";
        result = json{{"ok", rc == 0}, {"exit", rc == 124 ? -1 : rc},
                      {"timed_out", rc == 124}, {"output", out}};
      }
    }
    bool rok = result.value("ok", false);
    if (!read_only && !rok) {
      std::string why = result.value("error", std::string());
      if (why.empty() && result.contains("exit")) {
        why = "exit " + std::to_string(result.value("exit", 0));
        std::string o = trim(result.value("output", std::string()));
        if (!o.empty()) why += " - " + (o.size() > 200 ? o.substr(0, 200) : o);
      }
      Log::note("chat: " + name + " gagal: " + why);
    }
    act.tool(name, shown, rok ? "ok" : "gagal: " + result.value("error", std::string()));
    Log::note("chat: " + name + " -> " + (rok ? "ok" : "gagal"));
    return result.dump();
  }

  return json({{"ok", false}, {"error", "tool tidak dikenal: " + name}}).dump();
}

// Animasi loading di thread terpisah selama menunggu byte pertama dari model.
struct Spinner {
  std::atomic<bool> stop{false};
  std::atomic<bool> shown{false};
  std::mutex io;
  std::thread th;
  void start() {
    stop = false;
    shown = false;
    th = std::thread([this] {
      const char frames[] = {'|', '/', '-', '\\'};
      // Jeda kecil: respons instan tidak sempat menampilkan spinner.
      for (int k = 0; k < 2 && !stop.load(); ++k)
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
      int i = 0;
      while (!stop.load()) {
        {
          std::lock_guard<std::mutex> lk(io);
          if (stop.load()) break;
          std::cout << "\r\033[36m" << frames[i % 4] << "\033[0m " << std::flush;
          shown = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(110));
        ++i;
      }
    });
  }
  void hide() {
    std::lock_guard<std::mutex> lk(io);
    if (!stop.exchange(true) && shown.load()) std::cout << "\r\033[K" << std::flush;
  }
  void join() {
    if (th.joinable()) th.join();
  }
  // Selalu hentikan + gabungkan thread agar tidak pernah std::terminate walau ada jalur keluar
  // tak terduga (mencegah resource leak / crash).
  ~Spinner() {
    stop = true;
    if (th.joinable()) th.join();
  }
};

fs::path mission_path(const fs::path& home) { return home / ".claw" / "mission.json"; }

fs::path home_path() { return get_env("HOME").empty() ? home_dir() : fs::path(get_env("HOME")); }

// Model yang dipilih pengguna untuk satu fitur (xset > claw). "" berarti pilih otomatis,
// juga bila model itu sudah tidak ada di pool.
std::string preferred_model(const Router& router, const std::string& want, const char* feature) {
  if (want.empty() || want == "auto") return "";
  auto names = router.names();
  if (std::find(names.begin(), names.end(), want) != names.end()) return want;
  Log::warn(std::string(feature) + ": model '" + want + "' tidak ada di pool, memakai pilihan otomatis");
  return "";
}

// Model yang paling cocok untuk jenis tugas di antara yang tidak sedang cooldown ("" = auto).
std::string route_for(const Router& router, TaskKind kind) {
  std::vector<std::string> usable;
  for (const auto& s : router.status())
    if (s.cooldown_left <= 0) usable.push_back(s.name);
  return pick_model(usable, kind);
}

ChatTools make_tools(const Catalog& catalog, const fs::path& home) {
  ChatTools tools;
  tools.project_root = catalog.data_dir().parent_path();
  tools.browse_on = g_policy.browse != "off" && !scrapling_python(tools.project_root).empty();
  tools.skills_on = g_policy.skills;
  if (tools.skills_on) tools.skills = list_skills(project_skill_dirs(tools.project_root));
  tools.fs_access = g_policy.fs_access;
  tools.fs_base = fs::current_path();
  tools.lang_block = style_block(load_phrases(catalog.data_dir(), home));
  return tools;
}

// Hasil satu agen pekerja: laporan akhir, model yang dipakai, berapa langkah tool.
struct AgentOutcome {
  bool ok = false;
  std::string report;
  std::string model;
  int tools_used = 0;
};

// Agen pekerja (didefinisikan setelah cmd_chat; dipakai lebih dulu oleh run_delegate).
AgentOutcome run_agent(Router& router, const ChatTools& base, Activity& act, const std::string& goal,
                       const AgentTask& task, const std::string& deps, const std::string& fixed);

// Agen depan `claw chat` melimpahkan satu pekerjaan ke agen pekerja. Pekerja memakai izin
// (Grants) yang sama tetapi punya tool penuh (tulis/jalankan) dan tidak boleh mendelegasikan
// lagi. Mengembalikan laporan pekerja sebagai hasil tool, agar agen depan menjelaskan/mereview.
std::string run_delegate(Router& router, ChatTools& front, Activity& act, const std::string& goal,
                         const std::string& raw_args, const std::string& fixed) {
  DelegateRequest d = parse_delegate(raw_args, front.skills_on ? front.skills : std::vector<SkillInfo>{});
  if (d.task.empty()) return "delegate_task: butuh 'task' (deskripsi pekerjaan untuk pekerja)";

  ChatTools worker = front;   // Grants dibagi; tool penuh, tanpa delegasi berlapis
  worker.can_delegate = false;
  if (worker.skills_on && d.skills.empty())
    d.skills = suggest_skills(goal + " " + d.task, worker.skills, 2);

  AgentTask task;
  task.id = "w";
  task.role = d.role.empty() ? worker_role(d.kind) : d.role;
  task.task = d.task;
  task.kind = d.kind;
  task.skills = d.skills;

  {
    std::lock_guard<std::mutex> lk(g_io);
    std::cout << "\033[2m  · pekerja " << task.role << " mulai\033[0m\n" << std::flush;
  }
  Spinner spin;
  spin.start();
  // Tampilkan tiap langkah pekerja saat terjadi (membuat berkas, menulis kode, menjalankan
  // perintah), lalu lanjutkan spinner untuk fase berpikir berikutnya. Spinner di-join sebelum
  // start ulang agar tidak pernah menimpa thread yang masih hidup.
  worker.on_activity = [&spin](const std::string& desc) {
    spin.hide();
    spin.join();
    {
      std::lock_guard<std::mutex> lk(g_io);
      std::cout << "\033[2m  · " << desc << "\033[0m\n" << std::flush;
    }
    spin.start();
  };
  AgentOutcome r;
  try {
    r = run_agent(router, worker, act, goal, task, "", fixed);
  } catch (const std::exception& e) {
    r.report = std::string("gagal: ") + e.what();
  }
  spin.hide();
  spin.join();
  {
    std::lock_guard<std::mutex> lk(g_io);
    std::cout << "\033[2m  · asisten " << task.role << ": " << (r.ok ? "selesai" : "gagal") << " · "
              << r.tools_used << " langkah" << (r.model.empty() ? "" : " · " + r.model) << "\033[0m\n"
              << std::flush;
  }
  return std::string(r.ok ? "STATUS: done" : "STATUS: failed") + "\nWORKER ROLE: " + task.role +
         (r.model.empty() ? "" : "\nMODEL: " + r.model) + "\nREPORT:\n" + r.report;
}

// Konteks tak terbatas: ukuran percakapan (tanpa pesan system) dan kompaksi riwayat.
// Didefinisikan setelah quiet_call/clip; dipakai lebih dulu oleh cmd_chat.
size_t convo_chars(const json& history);
void compact_history(Router& router, json& history, const std::string& model);

// Jam sekarang "HH:MM:SS" untuk header prompt chat.
std::string now_hms() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[16];
  std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
  return std::string(buf);
}

// Warna waktu prompt (dipakai chat_prompt DAN jam hidup lineedit, harus sama agar mulus).
const char* kChatTimeColor = "\033[38;5;179m";  // amber lembut

// Lebar tampilan (kolom) teks polos UTF-8: 1 kolom per titik-kode.
int disp_cols(const std::string& s) {
  int n = 0;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    ++n;
  }
  return n;
}

// Lebar terminal sekarang (kolom); 80 bila tidak diketahui.
int term_cols_now() {
#ifndef _WIN32
  struct winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
#endif
  return 80;
}

// Potong string UTF-8 sampai maksimal `cols` kolom, tambahkan '…' bila terpotong.
std::string clip_cols(const std::string& s, int cols) {
  if (disp_cols(s) <= cols) return s;
  if (cols <= 1) return "…";
  std::string out;
  int n = 0;
  for (size_t i = 0; i < s.size() && n < cols - 1;) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t adv = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    out += s.substr(i, adv);
    i += adv;
    ++n;
  }
  out += "…";
  return out;
}

// Prompt input `claw chat` dua-baris bergaya (warna modern), meniru estetika prompt terminal:
//   ┌─[assistant (model)]─[HH:MM:SS]
//   └──╼ > <input user berwarna>
// Nama model dipangkas agar header muat satu baris fisik (max_cols) sehingga jam hidup bisa
// memposisikan diri; *time_col (opsional) diisi kolom tempat waktu mulai di baris header.
std::string chat_prompt(const std::string& model_in, int max_cols, int* time_col) {
  const char* kFrame = "\033[38;5;240m";   // abu: garis bingkai
  const char* kName = "\033[1;38;5;213m";   // magenta tebal: label assistant
  const char* kModel = "\033[38;5;81m";     // cyan: nama model
  const char* kArrow = "\033[1;38;5;84m";   // hijau terang: tanda '>'
  const char* R = "\033[0m";
  std::string model = model_in.empty() ? "auto" : model_in;
  // Bagian tetap header (kolom): "┌─[" + "assistant " + "(" + ")" + "]─[" + "HH:MM:SS" + "]".
  int fixed = disp_cols("┌─[") + disp_cols("assistant ") + 2 + disp_cols("]─[") + 8 + 1;
  model = clip_cols(model, std::max(4, max_cols - fixed));
  if (time_col)
    *time_col = disp_cols("┌─[") + disp_cols("assistant ") + 1 + disp_cols(model) + 1 + disp_cols("]─[");
  std::string p = "\n";
  p += kFrame; p += "┌─["; p += R;
  p += kName; p += "assistant "; p += R;
  p += kModel; p += "(" + model + ")"; p += R;
  p += kFrame; p += "]─["; p += R;
  p += kChatTimeColor; p += now_hms(); p += R;
  p += kFrame; p += "]"; p += R;
  p += "\n";
  p += kFrame; p += "└──╼ "; p += R;
  p += kArrow; p += "> "; p += R;
  return p;
}

// `claw chat`: manusia mengobrol dan memberi perintah, model menjawab satu permintaan
// per giliran. Model dan router milik chat sendiri, terpisah dari humanizer.
int cmd_chat(Catalog& catalog) {
  auto models = ensure_models(catalog);
  Log::set_console(false);  // jejak router/gagal cukup di state/claw.log, layar chat tetap bersih
  Router& router = *new Router(catalog, models, router_options(catalog, "router-state-chat.json"));
  attach_autorepair(router, catalog);
  maybe_auto_update(catalog, &router);

  fs::path home = home_path();
  ChatTools tools = make_tools(catalog, home);
  tools.can_delegate = true;  // agen depan: obrol/review sendiri, kerja berat -> agen pekerja
  // Tampilkan langkah baca-saja agen depan (membaca berkas, menelusuri, membuka halaman) ke layar
  // saat menunggu, bukan cuma spinner. Spinner sudah disembunyikan sebelum tool jalan, jadi aman
  // mencetak baris. (Kerja pekerja delegasi punya penampil sendiri di run_delegate.)
  tools.on_activity = [](const std::string& d) {
    std::lock_guard<std::mutex> lk(g_io);
    std::cout << "\033[2m  · " << d << "\033[0m\n" << std::flush;
  };
  std::string fixed = preferred_model(router, g_policy.chat_model, "chat");

  // Berkas activity TIDAK dibuat kecuali diminta (/activity on). Chat tidak menulis umpan live.
  bool save_activity = false;
  Activity act(save_activity, false, home, "chat");

  auto fresh_history = [&] { return json::array({{{"role", "system"}, {"content", system_prompt(tools)}}}); };
  json history = fresh_history();

  std::cout << "claw chat · " << router.size() << " model · model chat: " << (fixed.empty() ? "auto" : fixed)
            << " · 2 agen (obrolan + pekerja)\nperintah: /models /use /skills /activity /reset /exit\n";

  std::vector<std::string> input_history;  // riwayat baris input (panah atas/bawah)
  std::string line, last_route;
  const int kMaxToolRounds = 8;

  while (true) {
    // Prompt dua-baris bergaya: header [assistant (model aktif)]─[jam realtime], lalu baris input.
    // Model aktif = pilihan tetap user, atau model terakhir yang menjawab, atau "auto".
    std::string active_model = !fixed.empty() ? fixed : (last_route.empty() ? std::string("auto") : last_route);
    int mc = term_cols_now();
    std::string prompt = chat_prompt(active_model, mc, nullptr);
    // Jam header memakai waktu LOKAL terkini setiap kali prompt digambar (akurat saat pengguna
    // hendak mengetik). Jam per-detik SENGAJA TIDAK dijalankan: keluaran tiap detik akan memaku
    // tampilan terminal ke bawah sehingga layar tidak bisa di-scroll ke atas untuk membaca riwayat.
    // Scroll bebas lebih penting daripada detik yang berdetak, jadi tidak ada LiveClock di sini.
    // Warna identitas: teks user cyan (dibedakan dari respons model & kode).
    LineStatus st = read_line(prompt, input_history, line, "\033[38;5;51m", nullptr);
    if (st == LineStatus::Eof) break;
    if (st == LineStatus::Interrupt) continue;  // Ctrl+C membatalkan baris, bukan keluar
    line = trim(line);
    if (line.empty()) continue;
    if (line == "/exit" || line == "/quit") break;
    if (line == "/reset") {
      history = fresh_history();
      std::cout << "riwayat percakapan dihapus\n";
      continue;
    }
    if (line == "/models" || line == "/status") {
      print_status(router);
      continue;
    }
    if (line == "/skills") {
      if (!tools.skills_on) std::cout << "skill dimatikan (xset > claw)\n";
      for (const auto& sk : tools.skills) std::cout << "  " << sk.name << ": " << sk.description << "\n";
      continue;
    }
    if (line == "/activity" || line.rfind("/activity ", 0) == 0) {
      std::string arg = trim(line.size() > 9 ? line.substr(9) : "");
      save_activity = (arg != "off");
      act.set_detailed(save_activity);
      std::cout << (save_activity ? "catatan aktivitas AKTIF: " + act.path().string() : std::string("catatan aktivitas dimatikan"))
                << "\n";
      continue;
    }
    if (line.rfind("/use ", 0) == 0) {
      std::string name = trim(line.substr(5));
      if (name == "auto") {
        fixed.clear();
        std::cout << "model chat: auto\n";
      } else if (router.set_current(name)) {
        fixed = name;
        std::cout << "model chat: " << name << "\n";
      } else {
        std::cout << "model tidak ada di pool: " << name << "\n";
      }
      continue;
    }

    // Konteks tak terbatas: bila percakapan sudah besar, ringkas giliran lama menjadi satu
    // memori sebelum giliran ini, supaya obrolan bisa berlanjut tanpa batas dan tanpa
    // menabrak batas konteks model. Dilakukan SEBELUM turn_start dicatat agar pembatalan
    // giliran yang gagal tetap menunjuk indeks yang benar.
    const size_t kCompactChars = 24000;
    if (convo_chars(history) > kCompactChars) {
      std::cout << "\033[2m· merapikan memori percakapan…\033[0m" << std::flush;
      compact_history(router, history, fixed);
      std::cout << "\r\033[K" << std::flush;
    }

    size_t turn_start = history.size();
    std::string pick = fixed;
    if (pick.empty()) {
      TaskKind kind = categorize_request(line);
      pick = route_for(router, kind);
      if (!pick.empty() && pick != last_route) {
        Log::note("chat: model untuk tugas " + std::string(task_name(kind)) + ": " + pick);
        last_route = pick;
      }
    }
    history.push_back({{"role", "user"}, {"content", line}});

    for (int round = 0; round < kMaxToolRounds; ++round) {
      std::string requested = !fixed.empty() ? fixed : (round == 0 && !pick.empty()) ? pick : std::string("auto");
      json req = {{"model", requested}, {"messages", history}};
      json specs = tool_specs(tools, round < kMaxToolRounds - 1);
      if (!specs.empty()) req["tools"] = specs;

      Spinner spin;
      spin.start();
      MarkdownStreamer md(true);
      md.set_base("\033[38;5;252m");  // warna dasar prosa respons model (abu terang)
      // Satu baris kosong sebelum jawaban, hanya bila model benar-benar menulis teks. Putaran
      // tool yang cuma mengirim baris kosong tidak lagi meninggalkan ruang kosong di layar.
      md.set_lead("\n");
      auto on_delta = [&](const std::string& piece) { md.feed(piece); };
      auto on_first = [&]() { spin.hide(); };
      Router::StreamResult sr = router.stream(req, requested, on_delta, on_first);
      spin.hide();
      spin.join();

      if (sr.status != 200) {
        // Model bisa gagal setelah sempat menulis sebagian jawaban: tutup gayanya dan pindah
        // baris dulu, agar pesan gagal tidak menempel di teks itu dengan warnanya.
        bool close_line = md.shown() && !md.at_col0();
        md.finish();
        if (close_line) std::cout << "\n";
        std::cout << "gagal: " << sr.error.value("message", "error") << "\n";
        while (history.size() > turn_start) history.erase(history.size() - 1);
        break;
      }

      if (!sr.tool_calls.empty()) {
        // Teks pengantar model sebelum memanggil tool ditutup dengan baris baru, agar keluaran
        // berikutnya (status pekerja, jawaban akhir) mulai di baris sendiri.
        if (md.shown()) {
          bool col0 = md.at_col0();
          md.finish();
          if (!col0) std::cout << "\n";
        }
        // Putaran tool: jalankan diam-diam (jejak ke log, bukan ke layar chat).
        json am = {{"role", "assistant"},
                   {"content", sr.content.empty() ? json(nullptr) : json(sr.content)},
                   {"tool_calls", sr.tool_calls}};
        history.push_back(am);
        for (const auto& tc : sr.tool_calls) {
          json fn = tc.value("function", json::object());
          std::string tname = fn.value("name", "");
          std::string args = fn.contains("arguments") && fn["arguments"].is_string()
                                 ? fn["arguments"].get<std::string>()
                                 : fn.value("arguments", json::object()).dump();
          // delegate_task menjalankan agen pekerja (kerja nyata); tool lain berjalan seperti biasa.
          std::string result = tname == "delegate_task"
                                   ? run_delegate(router, tools, act, line, args, fixed)
                                   : run_tool(tools, act, tname, args);
          history.push_back({{"role", "tool"}, {"tool_call_id", tc.value("id", "")}, {"content", result}});
        }
        continue;
      }

      bool close_line = md.shown() && !md.at_col0();
      md.finish();
      if (close_line) std::cout << "\n";
      history.push_back({{"role", "assistant"}, {"content", sr.content}});
      act.turn(line, sr.model);
      if (save_activity)
        act.step("Jawaban model", sr.content.size() > 500 ? sr.content.substr(0, 500) + " [...]" : sr.content);
      break;
    }
  }
  // Keluar dari chat TIDAK menulis apa pun ke berkas activity (tidak membuat/menyentuh
  // berkas activity setelah pengguna keluar dari obrolan).
  return 0;
}

// ---- humanizer: mode otonom multi-agen ----

std::string clip(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n) + "\n[... dipotong]" : s; }

// Satu jawaban lengkap tanpa ditampilkan ke layar (perencana dan agen).
Router::StreamResult quiet_call(Router& router, const json& req, const std::string& model) {
  return router.stream(req, model, [](const std::string&) {}, [] {});
}

// Panjang percakapan (tanpa pesan system), untuk memutuskan kapan perlu dikompaksi.
size_t convo_chars(const json& history) {
  size_t n = 0;
  if (!history.is_array()) return 0;
  for (const auto& m : history) {
    if (m.value("role", "") == "system") continue;
    if (m.contains("content") && m["content"].is_string()) n += m["content"].get<std::string>().size();
    if (m.contains("tool_calls")) n += m["tool_calls"].dump().size();
  }
  return n;
}

// Kompaksi riwayat agar percakapan tak terbatas: ringkas giliran lama menjadi satu memori
// system, sisakan giliran terbaru apa adanya. Bila peringkasan gagal, jatuh ke trim_history
// sebagai jaring pengaman sehingga ukuran tetap terkendali.
void compact_history(Router& router, json& history, const std::string& model) {
  if (!history.is_array() || history.size() < 6) return;
  std::vector<std::string> roles;
  for (const auto& m : history) roles.push_back(m.value("role", ""));
  size_t system_end = 0;
  while (system_end < roles.size() && roles[system_end] == "system") ++system_end;
  size_t keep_from = compact_keep_from(roles, system_end, 6);
  if (keep_from <= system_end) return;  // belum ada giliran cukup lama untuk diringkas

  std::string transcript;
  for (size_t i = system_end; i < keep_from; ++i) {
    std::string content = history[i].contains("content") && history[i]["content"].is_string()
                              ? history[i]["content"].get<std::string>()
                              : "";
    if (history[i].contains("tool_calls")) content += " [memakai tool]";
    if (trim(content).empty()) continue;
    transcript += roles[i] + ": " + content + "\n";
  }
  if (transcript.size() < 400) return;

  std::string prev_summary;
  if (system_end >= 2 && roles[1] == "system") prev_summary = history[1].value("content", "");

  std::string sm = model.empty() ? "auto" : model;
  json sreq = {{"model", sm},
               {"messages",
                json::array({{{"role", "system"},
                              {"content",
                               "Ringkas percakapan berikut menjadi memori yang padat dan faktual agar "
                               "obrolan bisa dilanjutkan tanpa kehilangan konteks. Sertakan: tujuan "
                               "pengguna, keputusan yang diambil, berkas yang dibuat/diubah beserta "
                               "path, fakta penting, dan hal yang masih terbuka. Poin-poin singkat, "
                               "jangan mengarang."}},
                             {{"role", "user"},
                              {"content", (prev_summary.empty() ? "" : "Memori sebelumnya:\n" + prev_summary + "\n\n") +
                                              "Percakapan untuk diringkas:\n" + clip(transcript, 12000)}}})}};
  Router::StreamResult sr = quiet_call(router, sreq, sm);

  if (sr.status == 200 && !trim(sr.content).empty()) {
    json rebuilt = json::array();
    rebuilt.push_back(history[0]);
    rebuilt.push_back({{"role", "system"},
                       {"content", "Memori percakapan sejauh ini (ringkasan otomatis, konteks tak "
                                   "terbatas):\n" + trim(sr.content)}});
    for (size_t i = keep_from; i < history.size(); ++i) rebuilt.push_back(history[i]);
    history = std::move(rebuilt);
    Log::note("chat: memori percakapan dikompaksi (konteks tak terbatas)");
  } else {
    // Peringkasan gagal (mis. semua model sibuk): pangkas separuh terlama sebagai cadangan.
    trim_history(history);
    Log::warn("chat: peringkasan gagal, riwayat dipangkas sebagai cadangan");
  }
}

void say(const std::string& s) {
  std::lock_guard<std::mutex> lk(g_io);
  std::cout << s << std::flush;
}

// Satu agen: memuat skill-nya, lalu bekerja dengan tool sampai memberi laporan akhir.
AgentOutcome run_agent(Router& router, const ChatTools& base, Activity& act, const std::string& goal,
                       const AgentTask& task, const std::string& deps, const std::string& fixed) {
  ChatTools t = base;  // salinan milik agen; Grants tetap dibagi
  t.agent = task.id + " " + task.role;
  std::string sys = system_prompt(t, PromptMode::Agent);
  // Agen pekerja humanizer membawa distilasi doktrin riset (evidence-first, anti-halusinasi,
  // loop verifikasi). Lead memegang doktrin penuh; pekerja cukup versi padat agar tetap cepat.
  if (t.research_mode && !t.doctrine.empty()) sys += "\n\n" + t.doctrine;
  for (const auto& name : task.skills) {
    const SkillInfo* si = find_skill(t.skills, name);
    if (!si) continue;
    try {
      sys += "\n\n## Skill loaded for this task: " + si->name + "\n" + read_skill(*si, 6000);
    } catch (const std::exception&) {
    }
  }
  std::string user = "Team goal: " + goal + "\n\nYour task (" + task.id + ", role: " + task.role + "):\n" + task.task;
  if (!deps.empty()) user += "\n\nResults from agents you depend on:\n" + deps;
  json history = json::array({{{"role", "system"}, {"content", sys}}, {{"role", "user"}, {"content", user}}});

  std::string pick = fixed.empty() ? route_for(router, task.kind) : fixed;
  AgentOutcome out;
  const int kRounds = 16;
  for (int round = 0; round < kRounds; ++round) {
    std::string requested = !fixed.empty() ? fixed : (round == 0 && !pick.empty()) ? pick : std::string("auto");
    json req = {{"model", requested}, {"messages", history}};
    json specs = tool_specs(t, round < kRounds - 1);
    if (!specs.empty()) req["tools"] = specs;
    Router::StreamResult sr = quiet_call(router, req, requested);
    if (sr.status != 200) {
      out.report = "gagal: " + sr.error.value("message", std::string("error"));
      return out;
    }
    out.model = sr.model;
    if (!sr.tool_calls.empty()) {
      history.push_back({{"role", "assistant"},
                         {"content", sr.content.empty() ? json(nullptr) : json(sr.content)},
                         {"tool_calls", sr.tool_calls}});
      for (const auto& tc : sr.tool_calls) {
        json fn = tc.value("function", json::object());
        std::string args = fn.contains("arguments") && fn["arguments"].is_string()
                               ? fn["arguments"].get<std::string>()
                               : fn.value("arguments", json::object()).dump();
        std::string result = run_tool(t, act, fn.value("name", ""), args);
        ++out.tools_used;
        history.push_back({{"role", "tool"}, {"tool_call_id", tc.value("id", "")}, {"content", result}});
      }
      continue;
    }
    out.ok = true;
    out.report = sr.content;
    return out;
  }
  out.report = "agen berhenti setelah " + std::to_string(kRounds) + " langkah tanpa laporan akhir";
  return out;
}

std::string planner_prompt(const ChatTools& t, int max_agents) {
  std::string p =
      "You are the lead of a team of autonomous AI agents in a Linux terminal. Plan at an exceptional, "
      "professional level (IQ-200 caliber): think from first principles, decompose the goal cleanly, "
      "and design a plan a top expert would be proud of. Split the user's goal into at most " +
      std::to_string(max_agents) + " tasks, one per agent. Use fewer agents for small "
      "goals (one is fine). Give each agent a clear role, a self-contained task, the kind of work "
      "(coding, reasoning, design or general) and the skills it should load from the list below. Use "
      "depends_on only when an agent needs another agent's result; agents without dependencies run at "
      "the same time.\n\nAgents can: ";
  p += t.browse_on ? "browse the web; " : "NOT browse the web; ";
  p += t.fs_access != "off" ? "read/write files and run the terminal." : "NOT touch files or the terminal.";
  if (t.skills_on && !t.skills.empty()) {
    p += "\n\nSkills:";
    for (const auto& sk : t.skills) p += "\n- " + sk.name + ": " + sk.description;
  } else {
    p += "\n\nNo skills are available; use an empty skills list.";
  }
  p += "\n\nReply with ONLY this JSON, no other text:\n"
       "{\"domain\":\"<field, e.g. software development, research, biology>\",\"agents\":[{\"id\":\"a1\","
       "\"role\":\"<short role>\",\"task\":\"<what this agent must do and deliver>\",\"kind\":\"coding\","
       "\"skills\":[\"<skill name>\"],\"depends_on\":[]}]}";
  return p;
}

// ---- doktrin riset humanizer (khusus `claw humanizer`) ----
// Dimuat dari data/humanizer/ yang dibundel bersama aplikasi: ENGINEERING.md (kerangka kerja
// evidence-driven), siklus.txt (visi riset never-ending), skill.txt (pool 620 kapabilitas).
// Lead (planner/summary/frontier) memakai versi penuh; agen pekerja memakai distilasi ringkas
// (research_mode) supaya tetap cepat dan muat di konteks model — capability routing, ENGINEERING §25.
struct HumanizerDoctrine {
  std::string engineering, siklus, pool;
  bool any() const { return !engineering.empty() || !siklus.empty() || !pool.empty(); }
};

HumanizerDoctrine load_humanizer_doctrine(const fs::path& data_dir) {
  HumanizerDoctrine d;
  auto rd = [&](const char* f, size_t cap) -> std::string {
    std::error_code ec;
    fs::path p = data_dir / "humanizer" / f;
    if (!fs::exists(p, ec)) return "";
    try { return clip(read_file(p), cap); } catch (...) { return ""; }
  };
  d.engineering = rd("ENGINEERING.md", 16000);
  d.siklus = rd("siklus.txt", 16000);
  d.pool = rd("skill.txt", 9000);
  return d;
}

// Header doktrin penuh untuk agen-lead (satu panggilan per misi/siklus, jadi biaya token wajar).
std::string lead_doctrine_header(const HumanizerDoctrine& d) {
  std::string h;
  if (!d.engineering.empty())
    h += "=== OPERATING DOCTRINE (ENGINEERING) — follow this ===\n" + d.engineering + "\n\n";
  if (!d.siklus.empty())
    h += "=== RESEARCH VISION (SIKLUS) — follow this ===\n" + d.siklus + "\n\n";
  if (!d.pool.empty())
    h += "=== CAPABILITY POOL — pick the minimum sufficient set for the task, do NOT run all ===\n" +
         d.pool + "\n\n";
  return h;
}

// Distilasi ringkas doktrin untuk tiap agen pekerja humanizer (dipakai tiap ronde → wajib padat).
const char* humanizer_worker_doctrine() {
  return "Research operating mode — you are part of the claw humanizer research team. Work "
         "evidence-first: separate FACT / EVIDENCE / ASSUMPTION / HYPOTHESIS / INFERENCE / UNKNOWN, and "
         "never promote an assumption to a fact. Source of truth in order: source code > runtime "
         "behaviour > test results > build/compiler output > logs > docs > assumptions. Debug by OBSERVE "
         "→ REPRODUCE → ISOLATE → HYPOTHESIS → TEST → ROOT CAUSE → MINIMAL FIX → RETEST; optimise only "
         "after you MEASURE a real bottleneck. Keep a verification loop: run it, read the ACTUAL output, "
         "self-correct until it truly works. Anti-hallucination: never invent a source, measurement, "
         "tool output, benchmark, log, file content or test result — if you did not run or verify it, "
         "say UNVERIFIED or INSUFFICIENT EVIDENCE. Stay within your task's scope, and report the "
         "objective, the evidence, the results, the limitations, and what still remains open.";
}

// Konteks yang dibawa sepanjang sesi humanizer: doktrin + cara mengetahui harus berhenti.
struct HumanizerCtx {
  HumanizerDoctrine doc;
  std::function<bool()> should_stop;   // true → hentikan (Ctrl+C atau humanizer dimatikan di xset)
};

// Satu misi: rencanakan, jalankan agen per gelombang, lalu rangkum untuk pengguna. Mengembalikan
// laporan akhir (dipakai untuk menurunkan frontier penelitian berikutnya di loop never-ending).
std::string run_mission(Router& router, ChatTools& tools, Activity& act, const fs::path& home,
                        const std::string& goal, const std::string& fixed, const HumanizerCtx& hz) {
  act.live("misi baru: " + (goal.size() > 80 ? goal.substr(0, 80) + "…" : goal));
  std::string lead = fixed.empty() ? route_for(router, TaskKind::Reasoning) : fixed;
  if (lead.empty()) lead = "auto";

  std::string prev;
  {
    std::error_code ec;
    if (fs::exists(mission_path(home), ec)) {
      json m = json::parse(read_file(mission_path(home)), nullptr, false);
      if (m.is_object() && m.contains("goal"))
        prev = "Previous mission goal: " + m.value("goal", std::string()) + "\nIts result:\n" +
               clip(m.value("report", std::string()), 1500);
    }
  }

  // 1. Rencana. Lead membawa doktrin riset penuh (ENGINEERING + siklus + pool kapabilitas).
  say("\nmerencanakan...\n");
  act.live("merencanakan tugas dan memilih skill");
  std::string user = "Goal: " + goal + (prev.empty() ? "" : "\n\n" + prev);
  std::string plan_sys = lead_doctrine_header(hz.doc) + planner_prompt(tools, g_policy.humanizer_agents);
  json req = {{"model", lead},
              {"messages", json::array({{{"role", "system"}, {"content", plan_sys}},
                                        {{"role", "user"}, {"content", user}}})}};
  Router::StreamResult sr = quiet_call(router, req, lead);
  std::string perr;
  AgentPlan plan;
  if (sr.status == 200)
    plan = parse_plan(sr.content, tools.skills_on ? tools.skills : std::vector<SkillInfo>{},
                      g_policy.humanizer_agents, &perr);
  else
    perr = sr.error.value("message", std::string("error"));
  if (plan.tasks.empty()) {
    Log::note("humanizer: rencana cadangan dipakai (" + perr + ")");
    plan = fallback_plan(goal, tools.skills_on ? tools.skills : std::vector<SkillInfo>{});
  }
  // Agen yang belum diberi skill: pilihkan dari bidang tugasnya.
  if (tools.skills_on)
    for (auto& t : plan.tasks)
      if (t.skills.empty()) t.skills = suggest_skills(goal + " " + t.task, tools.skills, 2);

  std::string shown = "rencana · bidang: " + plan.domain + " · " + std::to_string(plan.tasks.size()) + " agen\n";
  for (const auto& t : plan.tasks) {
    shown += "  " + t.id + " " + t.role;
    if (!t.skills.empty()) {
      shown += " [";
      for (size_t i = 0; i < t.skills.size(); ++i) shown += (i ? ", " : "") + t.skills[i];
      shown += "]";
    }
    if (!t.depends_on.empty()) {
      shown += " (menunggu";
      for (const auto& d : t.depends_on) shown += " " + d;
      shown += ")";
    }
    shown += "\n      " + (t.task.size() > 160 ? t.task.substr(0, 160) + "…" : t.task) + "\n";
  }
  say(shown);
  act.live("rencana: " + std::to_string(plan.tasks.size()) + " agen, bidang " + plan.domain);

  // 2. Eksekusi per gelombang; agen dalam satu gelombang berjalan bersamaan.
  std::vector<AgentOutcome> results(plan.tasks.size());
  std::map<std::string, size_t> index;
  for (size_t i = 0; i < plan.tasks.size(); ++i) index[plan.tasks[i].id] = i;
  for (const auto& wave : plan_waves(plan)) {
    // Berhenti responsif bila humanizer dimatikan (xset) atau Ctrl+C di tengah misi: jangan
    // luncurkan gelombang berikutnya. Gelombang yang sedang jalan tetap diselesaikan agar aman.
    if (hz.should_stop()) {
      say("\n(dihentikan: humanizer dimatikan atau Ctrl+C — melewati sisa gelombang)\n");
      break;
    }
    std::vector<std::thread> workers;
    for (size_t i : wave) {
      const AgentTask& task = plan.tasks[i];
      std::string deps;
      for (const auto& d : task.depends_on) {
        const AgentTask& dt = plan.tasks[index[d]];
        deps += "### " + dt.id + " (" + dt.role + ")\n" + clip(results[index[d]].report, 4000) + "\n\n";
      }
      say("[" + task.id + "] mulai: " + task.role + "\n");
      act.live("agen " + task.id + " (" + task.role + ") mulai");
      workers.emplace_back([&, i, deps] {
        try {
          results[i] = run_agent(router, tools, act, goal, plan.tasks[i], deps, fixed);
        } catch (const std::exception& e) {
          results[i].report = std::string("gagal: ") + e.what();
        }
        const auto& r = results[i];
        say("[" + plan.tasks[i].id + "] " + (r.ok ? "selesai" : "gagal") + " · " + std::to_string(r.tools_used) +
            " langkah" + (r.model.empty() ? "" : " · " + r.model) + "\n");
        act.live("agen " + plan.tasks[i].id + (r.ok ? " selesai" : " gagal"));
      });
    }
    for (auto& w : workers) w.join();
  }

  // 3. Ringkasan untuk pengguna.
  std::string report;
  MarkdownStreamer md(true);
  if (plan.tasks.size() == 1) {
    report = results[0].report;
    say("\n");
    std::lock_guard<std::mutex> lk(g_io);
    md.feed(report);
    md.finish();
    std::cout << "\n";
  } else {
    std::string reports;
    for (size_t i = 0; i < plan.tasks.size(); ++i)
      reports += "### " + plan.tasks[i].id + " (" + plan.tasks[i].role + (results[i].ok ? "" : ", FAILED") + ")\n" +
                 clip(results[i].report, 6000) + "\n\n";
    json sreq = {{"model", lead},
                 {"messages",
                  json::array({{{"role", "system"},
                                {"content", "You lead a team of AI agents. Write the final answer to the user from "
                                            "the agents' reports: what was done, the results (files, findings, "
                                            "sources) and what is still open. Answer in the user's language, be "
                                            "concise, and be honest about any agent that failed. Finish with a short "
                                            "CONTINUOUS RESEARCH STATE: current knowledge, unresolved questions, and "
                                            "the most valuable next step — research never reaches a final state." +
                                                (tools.lang_block.empty() ? "" : "\n\n" + tools.lang_block)}},
                               {{"role", "user"}, {"content", "Goal: " + goal + "\n\nReports:\n" + reports}}})}};
    Spinner spin;
    spin.start();
    Router::StreamResult fr =
        router.stream(sreq, lead, [&](const std::string& p) { md.feed(p); },
                      [&] {
                        spin.hide();
                        std::cout << "\n";
                      });
    spin.hide();
    spin.join();
    if (fr.status == 200) {
      md.finish();
      std::cout << "\n";
      report = fr.content;
    } else {
      report = reports;  // ringkasan gagal: tampilkan laporan mentah agen
      std::cout << "\n" << reports;
    }
  }
  act.live("misi selesai");
  try {
    std::error_code ec;
    fs::create_directories(home / ".claw", ec);
    write_file_atomic(mission_path(home),
                      json({{"goal", goal}, {"domain", plan.domain}, {"report", report}, {"time", unix_time()}}).dump());
  } catch (...) {
  }
  return report;
}

// Turunkan objective penelitian berikutnya dari hasil misi terakhir (siklus.txt: CONTINUOUS
// RESEARCH STATE + RESOURCE DISCIPLINE). Kembalikan satu baris tujuan; "" bila sudah tuntas/gagal.
std::string next_frontier(Router& router, const ChatTools& tools, const std::string& prev_goal,
                          const std::string& prev_report, const HumanizerCtx& hz, const std::string& fixed) {
  std::string sys =
      "You are the strategy lead of a never-ending autonomous research team (the claw humanizer). A "
      "research objective was just completed. Following the CONTINUOUS RESEARCH STATE and RESOURCE "
      "DISCIPLINE principles, choose the single most valuable NEXT objective: what is still unknown, "
      "what can be verified or reproduced, what can be made faster/safer/more efficient, or a new "
      "question the result opened. Keep it concrete and achievable as ONE bounded mission. Reply with "
      "ONLY the next objective as one short line in the user's language — no preamble, no quotes, no "
      "explanation. If the work is genuinely complete and no worthwhile next step exists, reply exactly "
      "DONE.";
  if (!hz.doc.siklus.empty()) sys += "\n\nResearch vision to follow:\n" + clip(hz.doc.siklus, 6000);
  if (!tools.lang_block.empty()) sys += "\n\n" + tools.lang_block;
  std::string user = "Completed objective:\n" + prev_goal + "\n\nResult summary:\n" + clip(prev_report, 4000);
  std::string lead = fixed.empty() ? route_for(router, TaskKind::Reasoning) : fixed;
  if (lead.empty()) lead = "auto";
  json req = {{"model", lead},
              {"messages", json::array({{{"role", "system"}, {"content", sys}},
                                        {{"role", "user"}, {"content", user}}})}};
  Router::StreamResult sr = quiet_call(router, req, lead);
  if (sr.status != 200) return "";
  std::string next = trim(sr.content);
  // Ambil baris pertama (model kadang menambah pengantar meski diminta satu baris). next sudah
  // di-trim, jadi baris pertama pasti berisi bila ada.
  size_t nl = next.find('\n');
  if (nl != std::string::npos) next = trim(next.substr(0, nl));
  if (next.empty() || to_lower(next) == "done") return "";
  if (next.size() > 400) next = next.substr(0, 400);
  return next;
}

// `claw humanizer`: mode otonom. Tujuan dikerjakan tim agen dengan model dan router
// milik humanizer sendiri, terpisah dari `claw chat`.
int cmd_humanizer(Catalog& catalog, const Args& a) {
  if (!g_policy.humanizer) {
    std::cerr << "humanizer dimatikan. Aktifkan di xset > AI (claw) > Humanizer.\n";
    return 1;
  }
  auto models = ensure_models(catalog);
  Log::set_console(false);
  Router& router = *new Router(catalog, models, router_options(catalog, "router-state-humanizer.json"));
  attach_autorepair(router, catalog);
  maybe_auto_update(catalog, &router);

  fs::path home = home_path();
  ChatTools tools = make_tools(catalog, home);
  std::string fixed = preferred_model(router, g_policy.humanizer_model, "humanizer");
  Activity act(false, true, home, "humanizer");

  // Doktrin riset humanizer (dibundel di data/humanizer/): lead (planner/summary/frontier) memakai
  // penuh; tiap agen pekerja memakai distilasi ringkas lewat research_mode.
  HumanizerCtx hz;
  hz.doc = load_humanizer_doctrine(catalog.data_dir());
  tools.research_mode = true;
  tools.doctrine = humanizer_worker_doctrine();
  // Dua kondisi berhenti: humanizer dimatikan di xset (baca ulang policy.json — aplikasi menulis
  // ulang berkas itu setiap setelan berubah) ATAU Ctrl+C.
  auto humanizer_off = [&]() { return !load_policy(catalog.data_dir()).humanizer; };
  hz.should_stop = [&]() { return g_humanizer_stop.load() || humanizer_off(); };

  // Ctrl+C/SIGTERM menghentikan loop never-ending dengan rapi (tidak mematikan proses di tengah
  // penulisan berkas). Di prompt read_line (raw mode) Ctrl+C tetap dibaca sebagai pembatal baris.
  g_humanizer_stop = false;
  std::signal(SIGINT, [](int) { g_humanizer_stop = true; });
  std::signal(SIGTERM, [](int) { g_humanizer_stop = true; });

  std::cout << "claw humanizer · mode otonom · " << router.size() << " model · model humanizer: "
            << (fixed.empty() ? "auto" : fixed) << " · maks " << g_policy.humanizer_agents << " agen\n"
            << "izin: web " << (tools.browse_on ? g_policy.browse : "off") << ", berkas & terminal "
            << tools.fs_access << ", skill " << (tools.skills_on ? "aktif" : "mati")
            << ", doktrin " << (hz.doc.any() ? "termuat" : "bawaan") << "\n"
            << "riset berlanjut tiap siklus (frontier berikutnya otomatis). berhenti: matikan Humanizer "
               "di xset atau Ctrl+C.\n";
  // Buka tab monitor aktivitas real-time (aplikasi menafsirkan OSC ini).
  std::cout << "\033]5391;DRACX;monitor\033\\" << std::flush;

  // Loop riset never-ending dari satu seed: kerjakan misi, turunkan frontier berikutnya, ulangi.
  // Berhenti hanya saat should_stop() (xset off / Ctrl+C) atau strategi menyatakan tuntas (DONE).
  auto run_forever = [&](const std::string& seed) {
    std::string current = trim(seed);
    std::string last_report;
    int cycle = 0;
    while (!current.empty() && !hz.should_stop()) {
      ++cycle;
      say("\n\033[1m=== siklus riset #" + std::to_string(cycle) + " ===\033[0m\ntujuan: " + current + "\n");
      last_report = run_mission(router, tools, act, home, current, fixed, hz);
      if (hz.should_stop()) break;
      say("\nmenentukan frontier penelitian berikutnya...\n");
      std::string nxt = next_frontier(router, tools, current, last_report, hz, fixed);
      if (nxt.empty()) {
        say("strategi menilai riset untuk seed ini sudah tuntas (tidak ada langkah berikut yang berarti).\n");
        break;
      }
      current = nxt;
      // Jeda singkat yang bisa disela; cek stop (membaca policy.json) tiap ~200ms.
      for (int k = 0; k < 15 && !hz.should_stop(); ++k)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  };

  std::string goal;
  for (const auto& r : a.rest) goal += (goal.empty() ? "" : " ") + r;
  if (!trim(goal).empty()) {
    run_forever(trim(goal));
    std::cout << (humanizer_off() ? "\nhumanizer dimatikan di xset — berhenti.\n" : "\nselesai.\n");
    return 0;
  }
  std::cout << "tulis tujuan, tim agen meneliti terus sampai kamu berhenti. perintah: /models /skills "
               "/reset /exit\n";
  std::vector<std::string> input_history;
  std::string line;
  while (true) {
    LineStatus st = read_line("\ntujuan> ", input_history, line);
    if (st == LineStatus::Eof) break;
    if (st == LineStatus::Interrupt) continue;
    line = trim(line);
    if (line.empty()) continue;
    if (line == "/exit" || line == "/quit") break;
    if (line == "/reset") {
      std::error_code ec;
      fs::remove(mission_path(home), ec);
      std::cout << "misi sebelumnya dilupakan\n";
      continue;
    }
    if (line == "/models" || line == "/status") {
      print_status(router);
      continue;
    }
    if (line == "/skills") {
      if (!tools.skills_on) std::cout << "skill dimatikan (xset > claw)\n";
      for (const auto& sk : tools.skills) std::cout << "  " << sk.name << ": " << sk.description << "\n";
      continue;
    }
    run_forever(line);
    if (humanizer_off()) {
      std::cout << "\nhumanizer dimatikan di xset — keluar.\n";
      break;
    }
    // Ctrl+C atau riset tuntas: kembali ke prompt untuk tujuan baru (sesi humanizer tetap hidup).
    g_humanizer_stop = false;
    std::cout << "\nsiap untuk tujuan berikutnya (atau /exit).\n";
  }
  return 0;
}

struct Session {
  std::unique_ptr<Router> router;
  std::unique_ptr<ProxyServer> proxy;
  fs::path config_path;
  fs::path state_dir;
  int port = -1;
};

Session start_session(Catalog& catalog, const Args& a, const fs::path& workspace) {
  Session s;
  auto models = ensure_models(catalog);
  s.router = std::make_unique<Router>(catalog, models, router_options(catalog));
  attach_autorepair(*s.router, catalog);
  // Tanpa pool hidup: router milik sesi ini bisa sudah dihapus saat update selesai.
  maybe_auto_update(catalog, nullptr);
  std::string token = "claw-" + random_hex(16);
  s.proxy = std::make_unique<ProxyServer>(*s.router, token);
  s.port = s.proxy->start("127.0.0.1", a.port);
  if (s.port < 0) throw std::runtime_error("tidak ada port kosong mulai dari " + std::to_string(a.port));

  s.state_dir = catalog.state_dir() / "openclaw";
  s.config_path = s.state_dir / "openclaw.json";
  std::error_code ec;
  fs::create_directories(s.state_dir, ec);
  write_openclaw_config(s.config_path, s.port, token, workspace,
                        project_skill_dirs(catalog.data_dir().parent_path(), workspace));
  // Isolasi: OpenClaw memakai state & config milik runner, ~/.openclaw tidak disentuh.
  set_env("OPENCLAW_STATE_DIR", s.state_dir.string());
  set_env("OPENCLAW_CONFIG_PATH", s.config_path.string());
  return s;
}

int cmd_run(Catalog& catalog, const Args& a, bool ask) {
  OpenClawCommand oc = find_openclaw();
  if (!oc.found) {
    std::cerr << "OpenClaw tidak ditemukan. Pasang dengan installer resmi "
                 "(https://openclaw.ai) atau set OPENCLAW_BIN ke file openclaw.\n"
                 "Sementara itu kamu tetap bisa memakai `claw chat` atau `claw serve`.\n";
    return 1;
  }
  fs::path workspace = a.workspace.empty() ? fs::current_path() : fs::absolute(a.workspace);

  Log::set_file(catalog.state_dir() / "proxy.log");
  if (!ask) Log::set_console(false);  // jangan tumpuk output TUI OpenClaw
  Session s = start_session(catalog, a, workspace);

  std::vector<std::string> argv = oc.prefix;
  if (ask) {
    if (a.rest.empty()) throw std::runtime_error("pemakaian: claw ask \"<pesan>\"");
    std::string msg;
    for (const auto& r : a.rest) msg += (msg.empty() ? "" : " ") + r;
    for (const auto& x : {std::string("agent"), std::string("exec"), std::string("--config"),
                          s.config_path.string(), std::string("--model"), std::string("clawpool/auto"),
                          std::string("--cwd"), workspace.string(), msg})
      argv.push_back(x);
  } else if (a.rest.empty()) {
    argv.push_back("chat");
  } else {
    argv.insert(argv.end(), a.rest.begin(), a.rest.end());
  }

  std::cerr << "[claw] proxy 127.0.0.1:" << s.port << " | " << s.router->size()
            << " model | model awal: " << s.router->current() << "\n"
            << "[claw] OpenClaw (" << oc.source << ") | log: "
            << (catalog.state_dir() / "proxy.log").string() << "\n";
  int rc = run_command(argv);
  s.proxy->stop();
  // Request yang masih berjalan di thread latar tetap memakai router; biarkan
  // router hidup sampai proses selesai daripada menghapusnya di tengah jalan.
  if (s.proxy->inflight() > 0) (void)s.router.release();
  std::cerr << "[claw] selesai (exit " << rc << "). Model terakhir: " << s.router->current() << "\n";
  return rc;
}

int cmd_update(Catalog& catalog, const Args& a) {
  ScanOptions opt;
  opt.probe_tools = !has_flag(a.rest, "--no-tools");
  UpdateResult r = run_update(catalog, opt);
  std::cout << "\n";
  if (!r.written) {
    std::cout << "update: " << r.error << "\n";
    return 1;
  }
  for (const auto& n : r.merge.added) std::cout << "  + " << n << "\n";
  for (const auto& [n, why] : r.merge.removed) std::cout << "  - " << n << "  (" << why << ")\n";
  for (const auto& n : r.merge.kept_unverified) std::cout << "  ~ " << n << "  (gagal sementara, tetap disimpan)\n";
  std::cout << r.passed << "/" << r.tested << " model lolos HTTP 200. models.json sekarang berisi "
            << r.merge.models.size() << " model (" << r.merge.added.size() << " baru, " << r.merge.removed.size()
            << " dibuang).\n";
  return 0;
}

int cmd_browse(Catalog& catalog, const Args& a) {
  fs::path root = catalog.data_dir().parent_path();
  if (has_flag(a.rest, "--setup")) return browse_setup(root, has_flag(a.rest, "--yes"));
  std::string url;
  for (size_t i = 0; i < a.rest.size(); ++i) {
    const std::string& x = a.rest[i];
    if (x == "--select" || x == "--max" || x == "--method" || x == "--body" || x == "--body-type" ||
        x == "--header") {
      ++i;
      continue;
    }
    if (x.rfind("--", 0) == 0) continue;
    url = x;
    break;
  }
  if (url.empty()) {
    std::cerr << "pemakaian: claw browse URL [--select CSS] [--max N] [--json] [--allow-private]\n"
                 "           claw browse URL --method POST --body \"k=v&k2=v2\" [--body-type json]\n"
                 "                          [--header \"K: V\"]...\n"
                 "           claw browse --setup [--yes]\n";
    return 2;
  }
  auto sel = flag_values(a.rest, "--select");
  int max_chars = 6000;
  auto mx = flag_values(a.rest, "--max");
  if (!mx.empty()) {
    try {
      max_chars = std::clamp(std::stoi(mx.front()), 200, 20000);
    } catch (...) {
    }
  }
  bool allow_private = has_flag(a.rest, "--allow-private") || g_policy.private_network;
  auto methods = flag_values(a.rest, "--method");
  BrowseResult r;
  if (!methods.empty()) {
    auto data = flag_values(a.rest, "--body");
    auto dtype = flag_values(a.rest, "--body-type");
    std::vector<std::pair<std::string, std::string>> headers;
    for (const auto& h : flag_values(a.rest, "--header")) {
      auto c = h.find(':');
      if (c != std::string::npos) headers.emplace_back(trim(h.substr(0, c)), trim(h.substr(c + 1)));
    }
    r = web_request(root, url, methods.front(), data.empty() ? "" : data.front(),
                    dtype.empty() ? "form" : dtype.front(), headers, max_chars, allow_private);
  } else {
    r = web_fetch(root, url, sel.empty() ? "" : sel.front(), max_chars, allow_private);
  }
  Log::note("browse: " + url_host(url) + " -> " + (r.ok ? "OK" : "gagal: " + r.error));
  {
    fs::path home = get_env("HOME").empty() ? home_dir() : fs::path(get_env("HOME"));
    Activity act(false, false, home, "browse");
    act.step("Membuka halaman web",
             "url    : " + (g_policy.save_conversations ? url : url_host(url)) + "\n" +
                 "hasil  : " + (r.ok ? "HTTP " + std::to_string(r.data.value("status", 0)) : "gagal: " + r.error));
  }
  if (has_flag(a.rest, "--json")) {
    std::cout << (r.data.empty() ? json({{"ok", false}, {"error", r.error}}) : r.data).dump(2) << "\n";
    return r.ok ? 0 : 1;
  }
  if (!r.ok) {
    std::cerr << "browse gagal: " << r.error << "\n";
    return 1;
  }
  const json& d = r.data;
  std::cout << "URL    : " << d.value("final_url", url) << "\n"
            << "status : " << d.value("status", 0) << "\n";
  if (!d.value("title", "").empty()) std::cout << "judul  : " << d.value("title", "") << "\n";
  std::cout << "\n" << d.value("text", "") << (d.value("truncated", false) ? "\n[... dipotong]" : "") << "\n";
  if (d.contains("links") && d["links"].is_array() && !d["links"].empty()) {
    std::cout << "\ntautan:\n";
    int n = 0;
    for (const auto& l : d["links"]) {
      if (++n > 10) break;
      std::cout << "  " << l.value("text", "") << "  " << l.value("url", "") << "\n";
    }
  }
  return 0;
}

int cmd_skills(Catalog& catalog) {
  auto skills = list_skills(project_skill_dirs(catalog.data_dir().parent_path()));
  std::cout << skills.size() << " skill" << (g_policy.skills ? "" : " (dimatikan di xset > claw)") << ":\n";
  for (const auto& s : skills) std::cout << "  " << s.name << ": " << s.description << "\n";
  return 0;
}

int cmd_policy(Catalog& catalog) {
  fs::path f = catalog.data_dir() / "policy.json";
  std::error_code ec;
  std::cout << "sumber             : " << (fs::exists(f, ec) ? f.string() : "bawaan (policy.json belum ada)") << "\n"
            << "browsing web       : " << g_policy.browse << "\n"
            << "jaringan lokal     : " << (g_policy.private_network ? "boleh" : "diblokir") << "\n"
            << "skill              : " << (g_policy.skills ? "aktif" : "mati") << "\n"
            << "auto-update model  : "
            << (g_policy.auto_update ? "aktif, tiap " + std::to_string(g_policy.update_interval_hours) + " jam"
                                     : std::string("mati"))
            << "\n"
            << "token proxy        : " << (g_policy.proxy_token ? "wajib" : "tidak wajib") << "\n"
            << "simpan percakapan  : " << (g_policy.save_conversations ? "boleh" : "tidak") << "\n"
            << "model chat         : " << g_policy.chat_model << "\n"
            << "humanizer          : " << (g_policy.humanizer ? "aktif" : "mati (default)") << ", model "
            << g_policy.humanizer_model << ", maks " << g_policy.humanizer_agents << " agen\n"
            << "berkas & terminal  : " << g_policy.fs_access << "\n"
            << "catatan aktivitas  : " << (g_policy.activity_log ? "aktif" : "mati") << "\n";
  int64_t last = last_update_time(catalog);
  std::cout << "update terakhir    : "
            << (last ? std::to_string((unix_time() - last) / 3600) + " jam lalu" : std::string("belum pernah"))
            << "\n";
  return 0;
}

int cmd_lang(Catalog& catalog, const Args& a) {
  fs::path home = get_env("HOME").empty() ? home_dir() : fs::path(get_env("HOME"));
  auto learn = flag_values(a.rest, "--learn");
  auto intent = flag_values(a.rest, "--intent");
  if (!learn.empty()) {
    if (learn_phrase(home, intent.empty() ? "umum" : intent.front(), learn.front())) {
      std::cout << "disimpan ke " << (home / ".claw" / "learned-id.jsonl").string() << "\n";
      return 0;
    }
    std::cerr << "gagal menyimpan kalimat\n";
    return 1;
  }
  auto phrases = load_phrases(catalog.data_dir(), home);
  std::cout << phrases.size() << " kalimat (bawaan + yang dipelajari):\n";
  std::string cur;
  for (const auto& ph : phrases) {
    if (ph.intent != cur) {
      cur = ph.intent;
      std::cout << "\n[" << cur << "]\n";
    }
    std::cout << "  " << ph.text << "\n";
  }
  return 0;
}

namespace {
std::atomic<bool> g_watch_stop{false};
}

// TUI laporan aktivitas real-time: mengikuti HOME/.claw/live.log dan menggambar
// ulang saat ada aktivitas baru. Ctrl+C untuk keluar.
int cmd_watch(Catalog& catalog) {
  (void)catalog;
  fs::path home = get_env("HOME").empty() ? home_dir() : fs::path(get_env("HOME"));
  fs::path lp = Activity::live_path(home);
  std::signal(SIGINT, [](int) { g_watch_stop = true; });
  std::signal(SIGTERM, [](int) { g_watch_stop = true; });
  std::cout << "\033[?1049h\033[?25l" << std::flush;  // layar alternatif + sembunyikan kursor

  const char* kSpin = "|/-\\";
  int tick = 0;
  size_t last_count = SIZE_MAX;

  auto fit = [](std::string s, int w) {
    if (w <= 1) return std::string();
    if (static_cast<int>(s.size()) > w) s = s.substr(0, w - 1) + "\u2026";
    return s;
  };

  while (!g_watch_stop) {
    int cols = 80, rows = 24;
#ifndef _WIN32
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
      cols = ws.ws_col;
      rows = ws.ws_row;
    }
#endif
    std::vector<std::string> lines;
    {
      std::ifstream in(lp);
      std::string l;
      while (std::getline(in, l))
        if (!trim(l).empty()) lines.push_back(l);
    }
    std::string out = "\033[2J\033[H";
    // Header bar (ungu), judul + indikator LIVE + spinner + jumlah.
    std::string spin(1, kSpin[tick % 4]);
    std::string head = " claw \u00b7 laporan aktivitas real-time";
    std::string right = std::string("\u25cf LIVE ") + spin + "  " +
                        std::to_string(lines.size()) + " aktivitas ";
    int pad = cols - static_cast<int>(head.size()) - static_cast<int>(right.size()) - 1;
    if (pad < 1) pad = 1;
    out += "\033[48;5;54m\033[97m" + fit(head, cols) + std::string(pad, ' ') + right + "\033[0m\n\n";

    int body = rows - 4;
    if (body < 1) body = 1;
    if (lines.empty()) {
      out += "  \033[2mmenunggu aktivitas... jalankan `claw humanizer` di tab lain.\033[0m\n";
    } else {
      size_t start = lines.size() > static_cast<size_t>(body) ? lines.size() - body : 0;
      for (size_t i = start; i < lines.size(); ++i) {
        std::string ln = lines[i];
        // Baris penanda sesi ("== ... ==") dibuat menonjol.
        if (ln.rfind("> ==", 0) == 0) {
          out += "  \033[38;5;141m" + fit(ln.substr(2), cols - 2) + "\033[0m\n";
          continue;
        }
        // Pisahkan waktu " (....)" di ujung agar bisa diredupkan.
        std::string body_txt = ln, tail;
        size_t tp = ln.rfind(" (");
        if (tp != std::string::npos && ln.back() == ')') {
          body_txt = ln.substr(0, tp);
          tail = ln.substr(tp);
        }
        std::string colored = "\033[38;5;213m>\033[0m ";  // ">" aksen
        std::string rest = body_txt.size() > 2 ? body_txt.substr(2) : body_txt;  // buang "> "
        colored += rest;
        if (!tail.empty()) colored += "\033[2m" + tail + "\033[0m";
        out += "  " + fit(colored, cols + 40) + "\n";  // +40: kompensasi kode ANSI (perkiraan)
      }
    }
    // Footer di baris terakhir.
    out += "\033[" + std::to_string(rows) + ";1H\033[2m Ctrl+C untuk keluar \u00b7 mengikuti " +
           fit(lp.string(), cols - 24) + "\033[0m";
    if (lines.size() != last_count) {
      last_count = lines.size();
    }
    std::cout << out << std::flush;

    for (int i = 0; i < 4 && !g_watch_stop; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ++tick;
  }
  std::cout << "\033[?25h\033[?1049l" << std::flush;  // pulihkan layar + kursor
  // Minta aplikasi mematikan humanizer di xset; itu akan menutup tab ini dan menyimpan
  // catatan aktivitas (MainActivity.onHumanizerChanged). Aman diabaikan di terminal lain.
  std::cout << "\033]5391;DRACX;humanizer-off\033\\" << std::flush;
  std::cout << "monitor aktivitas ditutup, humanizer dimatikan.\n";
  return 0;
}

int cmd_doctor(Catalog& catalog) {
  int problems = 0;
  std::cout << "data dir      : " << catalog.data_dir().string() << "\n";
  std::cout << "providers     : " << catalog.providers().size() << "\n";
  try {
    auto m = catalog.load_models();
    std::cout << "models.json   : " << m.size() << " model\n";
    if (m.empty()) {
      std::cout << "  -> jalankan `claw scan`\n";
      ++problems;
    }
  } catch (const std::exception& e) {
    std::cout << "models.json   : ERROR " << e.what() << "\n";
    ++problems;
  }
  OpenClawCommand oc = find_openclaw();
  if (oc.found) {
    std::cout << "openclaw      : " << oc.prefix.back() << " (" << oc.source << ")\n";
    std::vector<std::string> v = oc.prefix;
    v.push_back("--version");
    int rc = run_command(v);
    if (rc != 0) {
      std::cout << "  -> openclaw --version gagal (exit " << rc << ")\n";
      ++problems;
    }
  } else {
    std::cout << "openclaw      : TIDAK DITEMUKAN\n";
    ++problems;
  }
  fs::path project_root = catalog.data_dir().parent_path();
  auto skill_dirs = project_skill_dirs(project_root);
  if (skill_dirs.empty()) std::cout << "skills        : tidak ada folder skill di " << project_root.string() << "\n";
  for (const auto& d : skill_dirs)
    std::cout << "skills        : " << count_skills(d) << " di " << fs::relative(d, project_root).generic_string() << "\n";
  fs::path py = scrapling_python(project_root);
  std::cout << "auto-repair   : "
            << (py.empty() ? "Scrapling belum di-setup (jalankan `claw repair` untuk petunjuk)"
                           : "siap (Scrapling di scrapling/.venv)")
            << "\n";
  fs::path tpy = tools_python(project_root);
  std::error_code tec;
  bool tools_ready = fs::exists(project_root / "tools" / ".ready", tec);
  std::cout << "alat networking/ai-red-team : "
            << (tpy.empty()
                    ? "belum di-setup (jalankan `sh /opt/claw/data/scripts/tools-setup.sh`)"
                    : (tools_ready ? "siap (scapy + pyrit di tools/.venv)"
                                   : "sebagian siap (scapy di tools/.venv; pyrit menyusul)"))
            << "\n";
  for (const auto& p : catalog.providers()) {
    if (!p.enabled || p.models_url.empty()) continue;
    HttpOptions ho;
    ho.read_timeout_sec = 20;
    ho.bearer = p.api_key;
    auto r = http_get(p.models_url, ho);
    std::cout << "jaringan      : " << p.name << " -> "
              << (r.status ? "HTTP " + std::to_string(r.status) : "gagal: " + r.error) << "\n";
  }
  std::cout << (problems ? "\nada " + std::to_string(problems) + " masalah\n" : "\nsemua beres\n");
  return problems ? 1 : 0;
}

}  // namespace

// Keluar tanpa menunggu thread latar. Destruktor statis tidak dijalankan kalau
// masih ada thread yang memakai logger/router; models.json tetap utuh karena
// setiap penulisan bersifat atomik.
int finish(int rc) {
  if (g_background.load() > 0) {
    Log::note("keluar saat tugas latar belakang masih jalan; dilanjutkan lain kali");
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(rc);
  }
  return rc;
}

int main(int argc, char** argv) {
  try {
    Args a = parse_args(argc, argv);
    Log::set_verbose(a.verbose);
    if (a.command == "help") {
      std::cout << kUsage;
      return 0;
    }
    if (a.command == "version") {
      std::cout << "claw " << CLAW_VERSION << "\n";
      return 0;
    }
    if (a.command == "watch" || a.command == "monitor") {
      Catalog empty_cat(resolve_data_dir(a.data_dir));  // watch hanya butuh HOME, bukan providers
      return finish(cmd_watch(empty_cat));
    }
    Catalog catalog(resolve_data_dir(a.data_dir));
    catalog.load_providers();
    std::string pw;
    g_policy = load_policy(catalog.data_dir(), &pw);
    // `run` menulis log ke proxy.log sendiri; perintah lain ke claw.log.
    if (a.command != "run" && a.command != "ask") Log::set_file(catalog.state_dir() / "claw.log");
    if (!pw.empty()) Log::warn(pw);

    if (a.command == "scan") {
      ScanOptions opt;
      opt.probe_tools = !has_flag(a.rest, "--no-tools");
      opt.only_providers = flag_values(a.rest, "--provider");
      return run_scan(catalog, opt) > 0 ? 0 : 1;
    }
    if (a.command == "update") return finish(cmd_update(catalog, a));
    if (a.command == "browse") return finish(cmd_browse(catalog, a));
    if (a.command == "skills") return finish(cmd_skills(catalog));
    if (a.command == "policy") return finish(cmd_policy(catalog));
    if (a.command == "lang") return finish(cmd_lang(catalog, a));
    if (a.command == "check") {
      ScanOptions opt;
      opt.probe_tools = false;
      opt.only_providers = flag_values(a.rest, "--provider");
      return run_check(catalog, opt, has_flag(a.rest, "--prune")) == 0 ? 0 : 1;
    }
    if (a.command == "repair") return cmd_repair(catalog, a);
    if (a.command == "models") return cmd_models(catalog);
    if (a.command == "serve") return finish(cmd_serve(catalog, a));
    if (a.command == "chat") return finish(cmd_chat(catalog));
    if (a.command == "humanizer") return finish(cmd_humanizer(catalog, a));
    if (a.command == "run") return finish(cmd_run(catalog, a, false));
    if (a.command == "ask") return finish(cmd_run(catalog, a, true));
    if (a.command == "doctor") return cmd_doctor(catalog);
    std::cerr << "perintah tidak dikenal: " << a.command << "\n\n" << kUsage;
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "claw: " << e.what() << "\n";
    return 1;
  }
}
