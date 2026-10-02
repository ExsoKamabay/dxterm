// Catatan aktivitas: apa pun yang claw kerjakan ditulis rinci ke satu berkas
// activity(<tanggal waktu>).txt di HOME terminal, sesuai permintaan pengguna.
// Satu berkas per sesi claw. Dimatikan lewat policy (activity_log = false).
#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "util.hpp"

namespace claw {

class Activity {
 public:
  // detailed: tulis berkas rinci activity(<waktu>).txt (default TIDAK untuk chat,
  // hanya bila diperintah). live: tulis umpan real-time ~/.claw/live.log yang
  // diikuti `claw watch` (dipakai saat humanizer aktif). command ikut ke judul.
  Activity(bool detailed, bool live, const fs::path& home, const std::string& command);

  // Satu langkah aktivitas. judul singkat, rincian bebas (boleh banyak baris).
  void step(const std::string& judul, const std::string& rincian = "");
  // Catat pemakaian tool oleh model beserti hasil ringkas.
  void tool(const std::string& nama, const std::string& argumen, const std::string& hasil);
  // Catat giliran percakapan (pertanyaan pengguna + model yang menjawab).
  void turn(const std::string& permintaan, const std::string& model);

  // Satu baris umpan real-time untuk `claw watch`, format:
  //   > <deskripsi> (YYYY-MM-DD HH:MM:SS)
  // Ditulis ke berkas umpan bersama (HOME/.claw/live.log) yang diikuti oleh monitor.
  void live(const std::string& desc);

  // Nyalakan/matikan penulisan berkas rinci di tengah sesi (perintah /activity).
  void set_detailed(bool on);

  bool active() const { return detailed_; }
  bool live_active() const { return live_; }
  fs::path path() const { return path_; }

  // Lokasi berkas umpan real-time untuk HOME tertentu (dipakai `claw watch`).
  static fs::path live_path(const fs::path& home);

 private:
  void ensure_header();
  void write(const std::string& block);

  bool detailed_;
  bool live_;
  fs::path home_;
  std::string command_;
  fs::path path_;
  bool header_done_ = false;
  int n_ = 0;
  std::shared_ptr<std::mutex> mu_ = std::make_shared<std::mutex>();
};

}  // namespace claw
