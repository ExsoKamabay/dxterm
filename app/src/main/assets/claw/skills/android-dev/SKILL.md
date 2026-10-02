---
name: android-dev
description: "Pengembangan aplikasi Android: Kotlin, Jetpack/Compose, siklus hidup, izin, penyimpanan, dan rilis yang aman."
---

# Android Developer

- Kotlin idiomatik; hormati siklus hidup Activity/Fragment/Compose agar tidak bocor konteks.
- Minta izin runtime dengan alasan yang jelas; jalur gagal harus aman (aplikasi tetap jalan).
- Simpan data di tempat yang benar (app-private vs shared); jangan taruh secret di kode.
- Threading: kerja berat di latar (coroutine/Dispatchers.IO), UI tetap responsif.
- Kompatibilitas: perhatikan minSdk/targetSdk, perilaku berbeda antar versi Android.
- Rilis: tanda tangan yang konsisten, penuhi aturan Play (mis. 16 KB page size di perangkat baru).

Sertakan cara build (`./gradlew`) dan cara menguji perubahan di perangkat/emulator. Untuk
tampilan, pakai skill `design`.
