// Izin yang diatur pengguna untuk apa yang boleh dilakukan model (data/policy.json).
//
// Di drac-Xterm file ini ditulis oleh aplikasi setiap terminal dibuka. Modul xset
// "claw" tidak lagi menampilkan toggle per-fitur: model berjalan dengan kemampuan
// penuh sebagai default dracxterm. File yang tidak ada atau rusak memakai nilai
// bawaan di bawah, yang kini mengikuti default penuh itu (proxy_token tetap wajib).
#pragma once

#include <string>

#include "util.hpp"

namespace claw {

struct Policy {
  // Tool web_fetch: "off" (tidak ditawarkan ke model), "ask" (tanya dulu setiap
  // kali), "allow" (langsung jalan). Default dracxterm: langsung jalan.
  std::string browse = "allow";
  // Boleh membuka alamat lokal (127.0.0.1, 192.168.x.x, 10.x.x.x, ...)?
  bool private_network = true;
  // Model boleh memilih dan memuat skill?
  bool skills = true;
  // Daftar model diperbarui otomatis di latar belakang.
  bool auto_update = true;
  int update_interval_hours = 24;
  // `claw serve` wajib memakai token (aplikasi lain di perangkat tidak bisa ikut memakai proxy).
  bool proxy_token = true;
  // Isi percakapan boleh disimpan ke disk (CLAW_DUMP_REQUESTS).
  bool save_conversations = false;
  // Humanizer: mode otonom multi-agen (`claw humanizer`), terpisah dari `claw chat`.
  // Saat false perintah itu menolak jalan. Tetap tunduk pada izin browse/fs_access/
  // skill di atas: tidak menambah akses. Default dracxterm: aktif.
  bool humanizer = true;
  // Model untuk tiap fitur, dijalankan terpisah (router dan state sendiri-sendiri).
  // "auto" = pilih otomatis dari pool sesuai jenis tugas.
  std::string chat_model = "auto";
  std::string humanizer_model = "auto";
  // Jumlah agen maksimal yang boleh dibuat humanizer untuk satu tujuan (1-6).
  int humanizer_agents = 3;
  // Pengelolaan berkas/folder dan terminal oleh model: "off" (tidak ada tool),
  // "ask" (operasi yang mengubah/menjalankan shell dikonfirmasi tiap kali),
  // "allow" (langsung jalan). Operasi baca (list/cari/baca) tersedia begitu != off.
  // Semua di dalam sandbox guest, sebagai user Linux. Default dracxterm: langsung jalan.
  std::string fs_access = "allow";
  // Tulis catatan aktivitas ke berkas activity-<waktu>.txt di HOME terminal.
  bool activity_log = true;
};

// Baca policy.json di data_dir. Nilai yang hilang atau salah tipe memakai bawaan.
Policy load_policy(const fs::path& data_dir, std::string* warning = nullptr);

}  // namespace claw
