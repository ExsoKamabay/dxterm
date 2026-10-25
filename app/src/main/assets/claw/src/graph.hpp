// Graph 3D agen humanizer: `run_mission` menulis state (manager + agen + ketergantungan
// + status hidup) ke home/.claw/graph.json; `cmd_watch` membacanya dan menggambar graph
// 3D animatif bergaya cyberpunk di terminal. File ini berisi model data, I/O JSON, dan
// matematika proyeksi/tata-letak yang MURNI (tanpa terminal) sehingga bisa diuji; render
// ANSI penuh ada di render_graph(), dipanggil loop cmd_watch.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "util.hpp"

namespace claw {

// Status sebuah node agen sepanjang misi.
enum class NodeState { Idle, Planning, Running, Done, Failed, Replaced };

const char* node_state_name(NodeState s);
NodeState node_state_from(const std::string& s);

struct GraphNode {
  std::string id;       // "mgr", "a1", "a2", ...
  std::string label;    // peran, mis. "developer"
  std::string model;    // model yang dipakai (boleh kosong)
  NodeState state = NodeState::Idle;
  int wave = 0;         // kedalaman ketergantungan (manager = -1)
  bool manager = false;
};

struct GraphState {
  std::string goal;
  std::string domain;
  std::vector<GraphNode> nodes;
  // (from, to): "to" bergantung pada "from" (panah mengalir from -> to).
  std::vector<std::pair<std::string, std::string>> edges;

  GraphNode* find(const std::string& id);
  void set_state(const std::string& id, NodeState s);
};

// Lokasi dan I/O graph.json (aman gagal; tidak pernah melempar ke pemanggil).
fs::path graph_path(const fs::path& home);
void write_graph(const fs::path& home, const GraphState& g);
bool read_graph(const fs::path& home, GraphState& g);

// ---- tata letak 2D (graf tautan, gaya Maltego) ----
struct P2 {
  double x = 0, y = 0;  // koordinat layar pecahan
  bool visible = true;
};

// Tata letak radial 2D: manager di pusat kanvas, agen pada cincin elips per-wave
// (wave lebih dalam = cincin lebih luar), disebar merata pada sudut. Mengembalikan satu
// posisi layar per node (indeks selaras g.nodes). Murni, tanpa rotasi atau animasi.
std::vector<P2> layout_graph(const GraphState& g, int cols, int rows);

// ---- renderer ANSI penuh ----
// Gambar satu frame graph (cols x rows) pada waktu t (detik) untuk parallax/animasi.
// ticker = baris aktivitas live.log (pita bawah). activity = peta id-node -> detik sejak
// aktivitas terakhirnya (kecil = baru saja aktif), dipakai memicu "ping" (cincin melebar)
// dan menyalakan marker agar graph tampak hidup mengikuti kerja agen. Mengembalikan string
// ANSI siap cetak (termasuk pindah kursor ke home); truecolor, latar gelap, gaya Maltego.
std::string render_graph(const GraphState& g, int cols, int rows, double t,
                         const std::vector<std::string>& ticker,
                         const std::map<std::string, double>& activity = {});

}  // namespace claw
