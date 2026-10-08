# Pengirim BLE uji (pengganti app HP)

Skrip Python untuk Linux (BlueZ) yang mengirim data navigasi ke ESP32 lewat
BLE, supaya jalur data asli firmware (`NAV_SOURCE_BLE`) bisa diuji sebelum
app HP ada. Format pesan dan frame dijelaskan di
`docs/navigation-protocol.md` bagian 1.

## Persiapan

```bash
cd tools/ble_sender
python -m venv .venv
.venv/bin/pip install -r requirements.txt
```

Firmware harus dibuild dengan `NAV_SOURCE_BLE`, misalnya:

```bash
env PLATFORMIO_BUILD_FLAGS="-DNAV_SOURCE=NAV_SOURCE_BLE" pio run -t upload --upload-port /dev/ttyACM0
```

## Pemakaian

```bash
.venv/bin/python ble_sender.py --synthetic                  # rute acak seperti simulator firmware
.venv/bin/python ble_sender.py --latlon                     # rute contoh di sekitar Monas
.venv/bin/python ble_sender.py --latlon rute.json --noise 5 # rute sendiri + noise GPS 5 m
```

Saat berjalan: **Enter** = jeda/lanjut (koneksi tetap tersambung, untuk
menguji state "tidak ada sinyal" setelah 3 detik), **q + Enter** = keluar.

| Opsi | Arti |
| --- | --- |
| `--synthetic` | Rute acak langsung dalam koordinat lokal (setara `src/nav_sim.cpp`); hanya menguji transport |
| `--latlon [FILE]` | Rute lat/lon + posisi GPS simulasi; menjalankan pipeline HP (`navgeo.py`). Tanpa FILE memakai rute contoh |
| `--rate HZ` | Pesan per detik (bawaan 5) |
| `--chunk-size N` | Byte per write, header frame ikut dihitung. Bawaan: batas write tanpa respons dari link. Coba `182` (setara iOS) dan `20` (MTU minimum BLE) |
| `--drop P` | Lewati seluruh pesan dengan peluang P → firmware mencatat `lost by seq` |
| `--drop-frames P` | Lewati satu frame dengan peluang P → firmware membuang pesan itu (`dropped (missing chunk)`) |
| `--pause-cycle RUN,PAUSE` | Kirim RUN detik, jeda PAUSE detik, berulang |
| `--speed KMH`, `--noise M` | Kecepatan dan noise GPS untuk `--latlon` |
| `--duration S`, `--seed N` | Berhenti setelah S detik; seed acak supaya bisa diulang |
| `--dry-run` | Tanpa BLE: hanya membangun dan menghitung frame |

File rute: `{"points": [[lat, lon], ...]}` atau GeoJSON `LineString`
(koordinat GeoJSON berurutan `[lon, lat]`).

Kalau `--chunk-size` lebih besar dari batas write link, atau terlalu kecil
sehingga pesan terbesar (275 B) butuh lebih dari 32 frame, skrip berhenti
dengan pesan error. Catatan BlueZ: saat menyambung ulang, bleak kadang
melaporkan batas write 20 B walaupun MTU link 517; skrip lalu membaca MTU
link langsung.

Statistik di sisi ESP32 dicetak ke serial tiap 5 detik selama tersambung
(`ble: ... complete, dropped (missing chunk), ... lost by seq ...`).

## Modul

| Berkas | Isi |
| --- | --- |
| `navproto.py` | Encoder pesan `'N'` dan pemecah frame (cermin `src/nav_codec.c` dan `src/nav_frag.c`) |
| `navgeo.py` | **Implementasi referensi untuk app HP**: map-matching, potong jendela −12…+80 satuan, equirectangular, sederhanakan ≤ 32 titik, heading dari tangen rute, rotasi heading-up, 15 m/satuan |
| `gps_sim.py` | GPS pura-pura yang bergerak di sepanjang rute, rute contoh, pembaca file rute |
| `synthetic.py` | Port simulator firmware |
| `ble_sender.py` | Skrip utama |

## Test

```bash
.venv/bin/python -m pytest
```

Test format memakai vektor byte yang sama dengan unit test C
(`test/test_nav_codec`, `test/test_nav_frag`), jadi kedua sisi dijamin
sepakat soal layout.
