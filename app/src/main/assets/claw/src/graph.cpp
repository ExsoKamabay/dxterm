#include "graph.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "json.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace claw {

using json = nlohmann::json;

const char* node_state_name(NodeState s) {
  switch (s) {
    case NodeState::Planning: return "planning";
    case NodeState::Running: return "running";
    case NodeState::Done: return "done";
    case NodeState::Failed: return "failed";
    case NodeState::Replaced: return "replaced";
    case NodeState::Idle: default: return "idle";
  }
}

NodeState node_state_from(const std::string& s) {
  if (s == "planning") return NodeState::Planning;
  if (s == "running") return NodeState::Running;
  if (s == "done") return NodeState::Done;
  if (s == "failed") return NodeState::Failed;
  if (s == "replaced") return NodeState::Replaced;
  return NodeState::Idle;
}

GraphNode* GraphState::find(const std::string& id) {
  for (auto& n : nodes)
    if (n.id == id) return &n;
  return nullptr;
}

void GraphState::set_state(const std::string& id, NodeState s) {
  if (GraphNode* n = find(id)) n->state = s;
}

fs::path graph_path(const fs::path& home) { return home / ".claw" / "graph.json"; }

void write_graph(const fs::path& home, const GraphState& g) {
  try {
    std::error_code ec;
    fs::create_directories(home / ".claw", ec);
    json jn = json::array();
    for (const auto& n : g.nodes)
      jn.push_back({{"id", n.id},
                    {"label", n.label},
                    {"model", n.model},
                    {"state", node_state_name(n.state)},
                    {"wave", n.wave},
                    {"manager", n.manager}});
    json je = json::array();
    for (const auto& e : g.edges) je.push_back({{"from", e.first}, {"to", e.second}});
    json j = {{"goal", g.goal}, {"domain", g.domain}, {"nodes", jn}, {"edges", je}};
    write_file_atomic(graph_path(home), j.dump());
  } catch (...) {
    // State graph hanya untuk visual; kegagalan menulis tidak boleh mengganggu misi.
  }
}

bool read_graph(const fs::path& home, GraphState& g) {
  try {
    fs::path p = graph_path(home);
    std::error_code ec;
    if (!fs::exists(p, ec)) return false;
    json j = json::parse(read_file(p), nullptr, false);
    if (!j.is_object()) return false;
    g = GraphState{};
    g.goal = j.value("goal", std::string());
    g.domain = j.value("domain", std::string());
    if (j.contains("nodes") && j["nodes"].is_array())
      for (const auto& n : j["nodes"]) {
        GraphNode nd;
        nd.id = n.value("id", std::string());
        nd.label = n.value("label", std::string());
        nd.model = n.value("model", std::string());
        nd.state = node_state_from(n.value("state", std::string("idle")));
        nd.wave = n.value("wave", 0);
        nd.manager = n.value("manager", false);
        if (!nd.id.empty()) g.nodes.push_back(nd);
      }
    if (j.contains("edges") && j["edges"].is_array())
      for (const auto& e : j["edges"]) {
        std::string f = e.value("from", std::string()), t = e.value("to", std::string());
        if (!f.empty() && !t.empty()) g.edges.emplace_back(f, t);
      }
    return true;
  } catch (...) {
    return false;
  }
}

// ---- tata letak radial 2D (hub manager di pusat, agen tersebar penuh 360°) ----
std::vector<P2> layout_graph(const GraphState& g, int cols, int rows) {
  std::vector<P2> out(g.nodes.size());
  double cx = cols / 2.0, cy = rows / 2.0;
  // Agen (bukan manager) menurut urutan muncul; sebar merata satu lingkaran penuh supaya
  // spoke manager->agen jatuh ke arah mata-angin (garis ─/│ bersih), radius tumbuh per-wave.
  std::vector<size_t> agents;
  for (size_t i = 0; i < g.nodes.size(); ++i)
    if (!g.nodes[i].manager) agents.push_back(i);
  int na = static_cast<int>(agents.size());
  double rx0 = cols * 0.17, rxw = cols * 0.15;  // elips: lebar > tinggi (aspek terminal)
  double ry0 = rows * 0.22, ryw = rows * 0.17;
  for (int k = 0; k < na; ++k) {
    size_t i = agents[k];
    double ang = (na > 0 ? 2.0 * M_PI * k / na : 0.0) - M_PI / 2.0;  // agen pertama di atas
    double rx = rx0 + rxw * g.nodes[i].wave;
    double ry = ry0 + ryw * g.nodes[i].wave;
    out[i] = P2{cx + rx * std::cos(ang), cy + ry * std::sin(ang), true};
  }
  for (size_t i = 0; i < g.nodes.size(); ++i)
    if (g.nodes[i].manager) out[i] = P2{cx, cy, true};
  return out;
}

// ---- renderer ANSI gaya Maltego ----
namespace {

struct Cell {
  std::string glyph;  // kosong = belum diisi
  int r = 0, g = 0, b = 0;
  bool bold = false;
};

struct Canvas {
  int w, h;
  std::vector<Cell> cells;
  Canvas(int cols, int rows) : w(cols), h(rows), cells(cols * rows) {}
  Cell& at(int x, int y) { return cells[y * w + x]; }
  bool in(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
  void put(int x, int y, const std::string& gl, int r, int g, int b, bool bold) {
    if (!in(x, y)) return;
    Cell& c = at(x, y);
    c.glyph = gl;
    c.r = r;
    c.g = g;
    c.b = b;
    c.bold = bold;
  }
};

// Warna status bergaya neon (cyberpunk) di atas latar gelap. Marker membawa warna penuh;
// edge, glow, dan label memakai turunannya.
void state_color(NodeState s, int& r, int& g, int& b) {
  switch (s) {
    case NodeState::Running: r = 0; g = 229; b = 255; break;    // cyan neon
    case NodeState::Done: r = 57; g = 255; b = 150; break;      // hijau neon
    case NodeState::Failed: r = 255; g = 60; b = 110; break;    // merah/pink neon
    case NodeState::Replaced: r = 255; g = 180; b = 60; break;  // amber neon
    case NodeState::Planning: r = 170; g = 120; b = 255; break; // ungu neon
    case NodeState::Idle: default: r = 110; g = 125; b = 165; break;  // baja biru
  }
}

// Glyph entitas per status (terbaca sekilas tanpa warna).
// Icon entitas per agen, dipilih dari kata kunci perannya (gaya entitas Maltego). Warna
// marker tetap dari status; icon menandakan JENIS agen agar bisa dibedakan sekilas.
std::string role_glyph(const GraphNode& n) {
  if (n.manager) return "◆";  // ◆ manager
  std::string s = to_lower(n.label);
  auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
  if (has("peneliti") || has("riset") || has("research") || has("analis")) return "◈";  // ◈ peneliti
  if (has("develop") || has("engineer") || has("coder") || has("program")) return "▣";  // ▣ developer
  if (has("audit") || has("security") || has("keamanan") || has("qa")) return "▲";       // ▲ auditor
  if (has("penulis") || has("writer") || has("docs") || has("tulis")) return "◇";        // ◇ penulis
  if (has("desain") || has("design")) return "❖";                                        // ❖ desainer
  return "●";  // ● umum
}

std::string clip_w(std::string s, int w) {
  if (w <= 0) return std::string();
  if (static_cast<int>(s.size()) > w) s = s.substr(0, w);
  return s;
}

void put_ascii(Canvas& cv, int x, int y, const std::string& s, int r, int g, int b, bool bold) {
  for (size_t i = 0; i < s.size(); ++i) cv.put(x + static_cast<int>(i), y, std::string(1, s[i]), r, g, b, bold);
}

// Konektor garis lurus dengan glyph mengikuti kemiringan (─ │ ╱ ╲), tidak menimpa sel padat.
void draw_edge(Canvas& cv, const std::vector<char>& solid, int x0, int y0, int x1, int y1, int r, int g,
               int b) {
  int ddx = x1 - x0, ddy = y1 - y0;
  std::string gl = (ddy == 0)   ? "─"                            // ─
                   : (ddx == 0) ? "│"                            // │
                   : ((ddx > 0) == (ddy > 0)) ? "╲" : "╱";  // ╲ / ╱
  int dx = std::abs(ddx), dy = std::abs(ddy);
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx - dy, x = x0, y = y0, guard = 0;
  while (guard++ < 4000) {
    if (cv.in(x, y) && cv.at(x, y).glyph.empty() &&
        !(x >= 0 && y >= 0 && x < cv.w && y < cv.h && solid[static_cast<size_t>(y) * cv.w + x]))
      cv.put(x, y, gl, r, g, b, false);
    if (x == x1 && y == y1) break;
    int e2 = 2 * err;
    if (e2 > -dy) { err -= dy; x += sx; }
    if (e2 < dx) { err += dx; y += sy; }
  }
}

}  // namespace

std::string render_graph(const GraphState& g, int cols, int rows, double t,
                         const std::vector<std::string>& ticker,
                         const std::map<std::string, double>& activity) {
  if (cols < 20) cols = 20;
  if (rows < 9) rows = 9;
  // Tata ruang: header, body (graph kiri | panel aktivitas kanan), legenda, tujuan, footer.
  int canvas_h = rows - 4;
  if (canvas_h < 3) canvas_h = 3;
  // Panel aktivitas di kanan bila layar cukup lebar; sisanya untuk graph.
  int panel_w = 0;
  if (cols >= 48) {
    panel_w = std::max(20, std::min(34, cols * 2 / 5));
    if (cols - panel_w - 1 < 20) panel_w = 0;  // graph butuh ruang minimal
  }
  int graph_w = cols - (panel_w ? panel_w + 1 : 0);
  Canvas cv(graph_w, canvas_h);

  std::vector<P2> pos = layout_graph(g, graph_w, canvas_h);
  // Parallax: tiap node mengambang halus; cincin lebih luar (wave lebih dalam) bergerak lebih
  // besar dan beda fase, manager hampir diam sebagai jangkar -> kesan kedalaman yang hidup.
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    if (!pos[i].visible) continue;
    double depth = g.nodes[i].manager ? 0.0 : (g.nodes[i].wave + 1);
    double ph = 0.7 * static_cast<double>(i);
    pos[i].x += (0.7 + 0.6 * depth) * std::sin(t * 0.45 + ph);
    pos[i].y += (0.45 + 0.4 * depth) * std::sin(t * 0.33 + ph * 1.27);
  }
  std::vector<int> ncx(g.nodes.size()), ncy(g.nodes.size());
  std::vector<char> solid(static_cast<size_t>(graph_w) * canvas_h, 0);
  auto mark_solid = [&](int x, int y) {
    if (cv.in(x, y)) solid[static_cast<size_t>(y) * graph_w + x] = 1;
  };
  auto is_solid = [&](int x, int y) {
    return !cv.in(x, y) || solid[static_cast<size_t>(y) * graph_w + x];
  };
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    ncx[i] = static_cast<int>(std::lround(pos[i].x));
    ncy[i] = static_cast<int>(std::lround(pos[i].y));
    mark_solid(ncx[i], ncy[i]);  // cadangkan sel marker agar garis/label tidak menimpanya
  }

  auto idx_of = [&](const std::string& id) -> int {
    for (size_t i = 0; i < g.nodes.size(); ++i)
      if (g.nodes[i].id == id) return static_cast<int>(i);
    return -1;
  };

  // Denyut per node: "ping" saat ada aktivitas baru (memudar ~1.1 dtk), plus napas halus untuk
  // agen yang sedang berjalan. Nilai 0..1 dipakai menyalakan marker.
  auto node_pulse = [&](size_t i) -> double {
    double p = 0.0;
    auto it = activity.find(g.nodes[i].id);
    if (it != activity.end() && it->second >= 0.0 && it->second < 1.1) p = 1.0 - it->second / 1.1;
    if (g.nodes[i].state == NodeState::Running)
      p = std::max(p, 0.30 + 0.20 * (0.5 + 0.5 * std::sin(t * 3.0 + 0.6 * static_cast<double>(i))));
    return p;
  };

  // 1) Konektor (di belakang marker): garis abu-abu baja + kepala panah ke anak.
  for (size_t ei = 0; ei < g.edges.size(); ++ei) {
    int a = idx_of(g.edges[ei].first), c = idx_of(g.edges[ei].second);
    if (a < 0 || c < 0) continue;
    bool active = g.nodes[c].state == NodeState::Running;
    // Edge ke agen yang jalan menyala cyan; sisanya ungu redup (jejak neon).
    int er = active ? 40 : 66, eg = active ? 190 : 60, eb = active ? 230 : 120;
    draw_edge(cv, solid, ncx[a], ncy[a], ncx[c], ncy[c], er, eg, eb);
    // Kepala panah tepat sebelum node anak (menunjuk ke dalam anak).
    int adx = (ncx[c] > ncx[a]) - (ncx[c] < ncx[a]);
    int ady = (ncy[c] > ncy[a]) - (ncy[c] < ncy[a]);
    int ax = ncx[c] - adx, ay = ncy[c] - ady;
    std::string head = (std::abs(ncx[c] - ncx[a]) >= std::abs(ncy[c] - ncy[a]))
                           ? (adx >= 0 ? "▸" : "◂")   // ▸ ◂
                           : (ady >= 0 ? "▾" : "▴");  // ▾ ▴
    if (cv.in(ax, ay) && !is_solid(ax, ay)) cv.put(ax, ay, head, er, eg, eb, false);
    // Titik aliran bergerak pada tautan ke agen yang sedang jalan (satu-satunya animasi).
    if (active) {
      double f = std::fmod(t * 0.7 + 0.19 * ei, 1.0);
      int fx = static_cast<int>(std::lround(ncx[a] + (ncx[c] - ncx[a]) * f));
      int fy = static_cast<int>(std::lround(ncy[a] + (ncy[c] - ncy[a]) * f));
      if (cv.in(fx, fy) && !is_solid(fx, fy) && !(fx == ax && fy == ay))
        cv.put(fx, fy, "∙", 160, 245, 255, true);  // kilau aliran data
    }
  }

  // 2) Label (teks terang, hindari tabrakan). Prioritas: manager -> yang lain.
  std::vector<size_t> lorder(g.nodes.size());
  for (size_t i = 0; i < lorder.size(); ++i) lorder[i] = i;
  std::sort(lorder.begin(), lorder.end(),
            [&](size_t a, size_t b) { return g.nodes[a].manager > g.nodes[b].manager; });
  for (size_t i : lorder) {
    const GraphNode& n = g.nodes[i];
    std::string lab = clip_w(n.id + (n.label.empty() ? "" : " " + n.label), 18);
    int len = static_cast<int>(lab.size());
    int cx = ncx[i], cy = ncy[i];
    const int cand[6][2] = {{cx + 2, cy},     {cx - 1 - len, cy},     {cx + 2, cy + 1},
                            {cx + 2, cy - 1}, {cx - 1 - len, cy + 1}, {cx - 1 - len, cy - 1}};
    int lx = -1, ly = -1;
    for (auto& p : cand) {
      bool ok = cv.in(p[0], p[1]) && cv.in(p[0] + len - 1, p[1]);
      for (int k = 0; ok && k < len; ++k)
        if (is_solid(p[0] + k, p[1])) ok = false;
      if (ok) { lx = p[0]; ly = p[1]; break; }
    }
    if (lx < 0) continue;  // tak ada ruang: lewati label, marker tetap tergambar
    bool mgr = n.manager;
    put_ascii(cv, lx, ly, lab, mgr ? 235 : 202, mgr ? 235 : 208, mgr ? 245 : 222, mgr);
    for (int k = 0; k < len; ++k) mark_solid(lx + k, ly);
  }

  // 3) Ripple: cincin melebar dari node yang baru saja aktif -> graph bereaksi tiap aktivitas.
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    auto it = activity.find(g.nodes[i].id);
    if (it == activity.end() || it->second < 0.0 || it->second >= 0.95) continue;
    double age = it->second;
    int rr = 1 + static_cast<int>(std::lround(age * 7.0));  // radius tumbuh seiring umur
    double fade = 1.0 - age / 0.95;
    int r, gg, b;
    state_color(g.nodes[i].state, r, gg, b);
    if (g.nodes[i].manager) { r = 255; gg = 60; b = 220; }
    double k = 0.25 + 0.45 * fade;
    int cr = static_cast<int>(r * k), cg = static_cast<int>(gg * k), cb = static_cast<int>(b * k);
    int d = static_cast<int>(std::lround(rr * 0.71));
    const int ring[8][2] = {{rr, 0}, {-rr, 0}, {0, rr}, {0, -rr}, {d, d}, {d, -d}, {-d, d}, {-d, -d}};
    for (auto& o : ring) {
      int x = ncx[i] + o[0], y = ncy[i] + o[1];
      if (cv.in(x, y) && cv.at(x, y).glyph.empty() && !is_solid(x, y))
        cv.put(x, y, "·", cr, cg, cb, false);  // '·'
    }
  }

  // 3b) Glow neon: halo redup berdenyut di sekitar manager & agen yang sedang jalan.
  {
    static const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    for (size_t i = 0; i < g.nodes.size(); ++i) {
      const GraphNode& n = g.nodes[i];
      if (!pos[i].visible || !(n.manager || n.state == NodeState::Running)) continue;
      int r, gg, b;
      state_color(n.state, r, gg, b);
      if (n.manager) { r = 255; gg = 60; b = 220; }
      double pulse = 0.5 + 0.5 * std::sin(t * 3.2 + 0.7 * static_cast<double>(i));
      double gi = 0.18 + 0.14 * pulse;
      int hr = static_cast<int>(r * gi), hg = static_cast<int>(gg * gi), hb = static_cast<int>(b * gi);
      for (int k = 0; k < 8; ++k) {
        int x = ncx[i] + dx[k], y = ncy[i] + dy[k];
        if (cv.in(x, y) && cv.at(x, y).glyph.empty() && !is_solid(x, y))
          cv.put(x, y, (k < 4 ? "·" : "∙"), hr, hg, hb, false);
      }
    }
  }

  // 4) Marker entitas di atas semuanya; menyala sesuai denyut (aktivitas/napas).
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    const GraphNode& n = g.nodes[i];
    int r, gg, b;
    state_color(n.state, r, gg, b);
    if (n.manager) { r = 255; gg = 60; b = 220; }
    double pz = node_pulse(i);  // 0..1
    r = static_cast<int>(r + (255 - r) * 0.6 * pz);
    gg = static_cast<int>(gg + (255 - gg) * 0.6 * pz);
    b = static_cast<int>(b + (255 - b) * 0.6 * pz);
    cv.put(ncx[i], ncy[i], role_glyph(n), r, gg, b, true);
  }

  // ---- komposisi ----
  std::ostringstream out;
  out << "\033[H";  // kursor ke kiri atas (tanpa clear penuh -> minim kedip)

  // Tally status agen (non-manager): dipakai header dan persentase progres live.
  int a_done = 0, a_run = 0, a_fail = 0, a_repl = 0, a_tot = 0;
  for (const auto& n : g.nodes) {
    if (n.manager) continue;
    ++a_tot;
    switch (n.state) {
      case NodeState::Done: ++a_done; break;
      case NodeState::Running: ++a_run; break;
      case NodeState::Failed: ++a_fail; break;
      case NodeState::Replaced: ++a_repl; break;
      default: break;  // Planning / Idle
    }
  }
  // Progres project yang hidup: agen tuntas (selesai/gagal) dihitung penuh, yang sedang jalan
  // atau sedang diganti dihitung setengah, perencanaan/idle nol. render_graph dipanggil ~11x/detik
  // atas graph.json yang diperbarui run_mission tiap status agen berubah, jadi nilainya bergerak
  // mengikuti kerja. 0 agen (fase perencanaan) → 0%.
  int pct = 0;
  if (a_tot > 0) {
    double prog = (a_done + a_fail) + 0.5 * (a_run + a_repl);
    pct = static_cast<int>(prog / a_tot * 100.0 + 0.5);
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
  }

  // Header.
  {
    std::string title = " claw · a-work · graf agen";
    std::ostringstream rs;
    rs << a_run << " jalan  " << a_done << " selesai  " << a_fail << " gagal  · " << a_tot << " agen ";
    std::string right = rs.str();
    int pad = cols - static_cast<int>(title.size()) - static_cast<int>(right.size()) - 1;
    if (pad < 1) pad = 1;
    out << "\033[48;2;14;10;26m\033[1m\033[38;2;0;229;255m" << clip_w(title, cols)
        << "\033[38;2;150;165;205m" << std::string(pad, ' ') << right << "\033[0m\033[K\n";
  }

  // Serialisasi satu baris kanvas graph (lebar graph_w) menjadi string ANSI.
  auto graph_row = [&](int y) -> std::string {
    std::ostringstream line;
    bool color_on = false;
    int cr = -1, cg = -1, cb = -1;
    bool cbold = false;
    for (int x = 0; x < graph_w; ++x) {
      const Cell& c = cv.at(x, y);
      if (c.glyph.empty()) {
        if (color_on) { line << "\033[0m"; color_on = false; cr = cg = cb = -1; cbold = false; }
        line << ' ';
      } else {
        if (!color_on || c.r != cr || c.g != cg || c.b != cb || c.bold != cbold) {
          line << "\033[0m";
          if (c.bold) line << "\033[1m";
          line << "\033[38;2;" << c.r << ";" << c.g << ";" << c.b << "m";
          color_on = true;
          cr = c.r; cg = c.g; cb = c.b; cbold = c.bold;
        }
        line << c.glyph;
      }
    }
    if (color_on) line << "\033[0m";
    return line.str();
  };

  // Panel aktivitas (kanan): judul + umpan live.log, terbaru di bawah, berwarna per agen.
  std::vector<std::string> panel(canvas_h);
  if (panel_w) {
    auto pad_to = [](std::string v, int w) {
      if (static_cast<int>(v.size()) > w) v = v.substr(0, w);
      else v += std::string(w - static_cast<int>(v.size()), ' ');
      return v;
    };
    // Judul panel: bar HUD neon (latar gelap, teks cyan).
    panel[0] = std::string("\033[48;2;20;16;34m\033[1m\033[38;2;0;229;255m") + pad_to(" AKTIVITAS", panel_w) +
               "\033[0m";
    int avail = canvas_h - 1, n = static_cast<int>(ticker.size());
    int start = n > avail ? n - avail : 0;
    for (int r = 0; r < avail; ++r) {
      int fi = start + r;
      if (fi >= n) { panel[1 + r] = std::string(panel_w, ' '); continue; }
      const std::string& raw = ticker[fi];
      int cr = 130, cg = 140, cb = 170;  // bawaan: baja (langkah non-agen: rencana/ringkasan)
      size_t ap = raw.find("agen ");
      if (ap != std::string::npos) {
        size_t s = ap + 5, e = s;
        while (e < raw.size()) {
          char c = raw[e];
          if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ++e; else break;
        }
        if (e > s) {
          std::string id = raw.substr(s, e - s);
          for (const auto& nd : g.nodes)
            if (nd.id == id) { state_color(nd.state, cr, cg, cb); break; }
        }
      }
      std::string txt = raw;
      if (txt.rfind("> ", 0) == 0) txt = txt.substr(2);
      size_t tp = txt.rfind(" (");
      if (tp != std::string::npos && !txt.empty() && txt.back() == ')') txt = txt.substr(0, tp);
      bool newest = (fi == n - 1);
      // Terbaru: warna agen penuh + bold + penanda ">". Lama: diredupkan dengan lantai warna agar
      // tetap mudah dibaca (bukan sekadar *3/5 yang bisa terlalu gelap).
      int tr = newest ? cr : std::min(255, cr / 2 + 55);
      int tg = newest ? cg : std::min(255, cg / 2 + 55);
      int tb = newest ? cb : std::min(255, cb / 2 + 60);
      panel[1 + r] = std::string("\033[38;2;") + std::to_string(tr) + ";" + std::to_string(tg) + ";" +
                     std::to_string(tb) + "m" + (newest ? "\033[1m" : "") +
                     pad_to((newest ? "> " : "  ") + txt, panel_w) + "\033[0m";
    }
  }

  // Body: graph (kiri) + pemisah + panel (kanan).
  for (int y = 0; y < canvas_h; ++y) {
    out << graph_row(y);
    if (panel_w) out << "\033[38;2;110;80;160m│\033[0m" << panel[y];
    out << "\033[K\n";
  }

  // Legenda status (titik berwarna).
  {
    std::ostringstream lg;
    lg << " \033[38;2;255;60;220m◆\033[38;2;150;155;170m manager  ";
    struct L { NodeState s; const char* gl; const char* name; };
    static const L items[] = {{NodeState::Running, "●", "jalan"},   {NodeState::Done, "●", "selesai"},
                              {NodeState::Replaced, "●", "ganti"}, {NodeState::Failed, "●", "gagal"},
                              {NodeState::Idle, "○", "idle"}};
    for (const auto& it : items) {
      int r, g2, b;
      state_color(it.s, r, g2, b);
      lg << "\033[38;2;" << r << ";" << g2 << ";" << b << "m" << it.gl << "\033[38;2;150;155;170m " << it.name
         << "  ";
    }
    out << lg.str() << "\033[0m\033[K\n";
  }

  // Baris tujuan dengan bar progres live di depannya. Untuk layar sempit tanpa panel, aktivitas
  // terakhir disertakan di belakang tujuan.
  {
    // Bar progres: lebar tetap, isi neon sesuai pct (hijau penuh saat 100%, cyan saat berjalan).
    // Glyph blok █/░ berukuran 3 byte; karena clip_w di berkas ini berbasis byte, bar dioutput
    // langsung dan lebar KOLOM-nya dihitung manual, lalu tujuan di-clip ke sisa kolom.
    const int barw = 12;
    int fill = (pct * barw + 50) / 100;
    if (fill < 0) fill = 0;
    if (fill > barw) fill = barw;
    const char* fill_col = (pct >= 100) ? "\033[38;2;57;255;150m" : "\033[38;2;0;229;255m";
    const char* num_col = (pct >= 100) ? "\033[1m\033[38;2;57;255;150m" : "\033[1m\033[38;2;120;245;255m";
    std::string ps = std::to_string(pct) + "%";
    std::ostringstream bar;
    bar << " \033[38;2;120;110;160m[" << fill_col;
    for (int k = 0; k < fill; ++k) bar << "█";
    bar << "\033[38;2;60;64;90m";
    for (int k = fill; k < barw; ++k) bar << "░";
    bar << "\033[38;2;120;110;160m]\033[0m " << num_col << ps << "\033[0m  ";
    //           " [" + isi(barw) + "] "  + ps        + "  "
    int bar_cols = 1 + 1 + barw + 1 + 1 + static_cast<int>(ps.size()) + 2;

    std::string line = g.goal;
    if (!panel_w) {
      std::string last = ticker.empty() ? "" : ticker.back();
      if (last.rfind("> ", 0) == 0) last = last.substr(2);
      size_t tp = last.rfind(" (");
      if (tp != std::string::npos && !last.empty() && last.back() == ')') last = last.substr(0, tp);
      if (!last.empty()) line += "  │  " + last;
    }
    out << bar.str() << "\033[38;2;150;160;185m" << clip_w(line, std::max(0, cols - bar_cols))
        << "\033[0m\033[K\n";
  }

  // Footer.
  out << "\033[2m Ctrl+C untuk keluar · graf agen a-work · bidang: "
      << clip_w(g.domain.empty() ? "umum" : g.domain, 20) << "\033[0m\033[K";
  return out.str();
}

}  // namespace claw
