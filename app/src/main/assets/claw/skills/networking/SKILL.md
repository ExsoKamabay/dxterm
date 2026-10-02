---
name: networking
description: "Kerja jaringan dengan modul Python scapy: menyusun/membedah paket, membaca pcap, dan belajar protokol — untuk jaringan/lab milik pengguna atau yang berwenang."
---

# Networking dengan scapy

scapy adalah pustaka Python untuk menyusun, membedah, mengirim, dan menangkap paket
jaringan. Skill ini dipakai untuk tugas networking: membangun/parse paket, menganalisis
berkas `.pcap`, memahami protokol, dan menguji jaringan sendiri.

## Batas penggunaan — WAJIB
Gunakan HANYA pada jaringan yang pengguna miliki atau berwenang untuk diuji (jaringan sendiri,
lab, atau engagement resmi). Jangan memindai, membanjiri, menyadap, atau menyerang jaringan
milik orang lain. Kalau kepemilikan/izin tidak jelas, tanya pengguna dulu.

## Lingkungan
scapy DI-BUNDLE bersama aplikasi (murni Python, tanpa dependensi tambahan) di
`/opt/claw/pytools`. Pakai lewat PYTHONPATH — tidak perlu pip/apt/jaringan:

```sh
export PYTHONPATH=/opt/claw/pytools
python3 -c "import scapy; from scapy.all import IP, TCP; print('scapy', scapy.__version__)"
```

Kalau `import scapy` gagal (bundel belum diekstrak), jalankan sekali (cepat, tanpa jaringan):

```sh
sh /opt/claw/data/scripts/tools-setup.sh
```

Jika `python3` sendiri belum ada di distro, pasang dulu: `sudo apt-get install -y python3`.

Saat pertama mengimpor, scapy menulis satu baris peringatan
`Cannot use an RTNETLINK socket ([Errno 13] Permission denied)`. Itu WAJAR di guest ini:
kernel menolak socket AF_NETLINK, jadi scapy tidak bisa mendaftar interface/route dan
menonaktifkannya. Impor tetap berhasil dan crafting/pcap tetap jalan; peringatan itu bukan
kegagalan.

## Yang bisa & tidak bisa di terminal ini
- **Bisa tanpa root:** menyusun paket (`IP()/TCP()/...`), membedah/`show()`, membaca &
  menulis pcap (`rdpcap`/`wrpcap`), menganalisis lalu lintas yang sudah direkam, dan
  perhitungan/konversi protokol.
- **Terbatas:** menangkap langsung (`sniff`) dan mengirim paket mentah (`send`/`sendp`,
  `sr*`) butuh hak raw socket (CAP_NET_RAW) yang biasanya TIDAK ada di guest tanpa root —
  operasi ini akan gagal dengan permission error. Untuk analisis, kerjakan pada pcap.

## Contoh
Menyusun dan membedah paket (tanpa root):

```python
from scapy.all import IP, TCP, Ether, rdpcap
pkt = IP(dst="10.0.0.5")/TCP(dport=80, flags="S")
pkt.show()                 # struktur paket
print(bytes(pkt).hex())    # byte mentah

# analisis rekaman lalu lintas milik/izin pengguna
for p in rdpcap("/path/ke/rekaman.pcap")[:10]:
    if p.haslayer(TCP):
        print(p[IP].src, "->", p[IP].dst, p[TCP].dport)
```

Untuk menjalankan skrip, tulis ke berkas lalu jalankan dengan
`PYTHONPATH=/opt/claw/pytools python3 skrip.py`.
Untuk debug error runtime pakai skill `audit-debug`; untuk perintah shell umum pakai `shell`.
