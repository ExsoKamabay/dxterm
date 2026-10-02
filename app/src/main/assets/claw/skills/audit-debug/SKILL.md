---
name: audit-debug
description: "Audit dan debug project tingkat lanjut: temukan akar masalah bug, ukur kualitas, dan usulkan perbaikan bertahap yang aman."
---

# Audit & Debug

## Debug berbasis bukti
1. Reproduksi dulu: langkah pasti yang memicu masalah.
2. Kumpulkan fakta: pesan error, stack trace, log, versi, dan input.
3. Bentuk hipotesis, uji satu per satu dari yang paling mungkin.
4. Perbaiki akarnya, lalu buktikan dengan reproduksi yang sama sekarang lolos.

## Audit kualitas
- Correctness: kasus tepi, kondisi balapan, penanganan error.
- Kinerja: N+1, alokasi berlebih, loop panas, I/O yang bisa dibatch.
- Kejelasan: nama yang menyesatkan, fungsi terlalu besar, duplikasi.
- Ketahanan: input tak valid, kegagalan jaringan, timeout, retry.

Sampaikan temuan berurut dari yang paling berdampak. Untuk tiap temuan: apa masalahnya,
skenario yang membuat gagal, dan perbaikan yang disarankan. Jangan mengklaim sudah menguji
kalau kamu tidak menjalankannya.
