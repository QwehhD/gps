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
.venv/bin/python ble_sender.py --synthetic                              # rute acak seperti simulator firmware
.venv/bin/python ble_sender.py --latlon                                 # rute contoh di sekitar Monas
.venv/bin/python ble_sender.py --latlon routes/kampus.geojson --noise 5 # rute sendiri + noise GPS 5 m
```

Saat berjalan: **Enter** = jeda/lanjut (koneksi tetap tersambung, untuk
menguji state "tidak ada sinyal" setelah 3 detik), **q + Enter** = keluar.

| Opsi | Arti |
| --- | --- |
| `--synthetic` | Rute acak langsung dalam koordinat lokal (setara `src/nav_sim.cpp`); hanya menguji transport |
| `--latlon [FILE]` | Rute lat/lon (GeoJSON atau daftar `points`, lihat di bawah) + posisi GPS simulasi; menjalankan pipeline HP (`navgeo.py`). Tanpa FILE memakai rute contoh |
| `--rate HZ` | Pesan per detik (bawaan 5) |
| `--chunk-size N` | Byte per write, header frame ikut dihitung. Bawaan: batas write tanpa respons dari link. Coba `182` (setara iOS) dan `20` (MTU minimum BLE) |
| `--drop P` | Lewati seluruh pesan dengan peluang P → firmware mencatat `lost by seq` |
| `--drop-frames P` | Lewati satu frame dengan peluang P → firmware membuang pesan itu (`dropped (missing chunk)`) |
| `--pause-cycle RUN,PAUSE` | Kirim RUN detik, jeda PAUSE detik, berulang |
| `--speed KMH`, `--noise M` | Kecepatan dan noise GPS untuk `--latlon` |
| `--duration S`, `--seed N` | Berhenti setelah S detik; seed acak supaya bisa diulang |
| `--dry-run` | Tanpa BLE: hanya membangun dan menghitung frame |

### File rute untuk `--latlon`

- **GeoJSON**: geometry `LineString` polos, `Feature` berisi `LineString`,
  atau `FeatureCollection` — yang dipakai `LineString` pertama, fitur lain
  seperti marker `Point` dilewati. Jadi respons ORS dari `fetch_route.py`
  maupun garis yang digambar di [geojson.io](https://geojson.io) bisa
  langsung dipakai. Kalau ada lebih dari satu `LineString`, ringkasan
  menyebut "first of N LineStrings"; `MultiLineString` tidak diterima.
- **Daftar titik**: `{"points": [[lat, lon], ...]}`.

Koordinat GeoJSON berurutan **`[longitude, latitude]`**, kebalikan dari
Google Maps. Kalau ada latitude di luar −90…90, berkas ditolak dengan pesan
bahwa urutannya kemungkinan tertukar, beserta nomor dan isi titik pertama
yang salah. Hal yang sama berlaku untuk daftar `points` (urutannya
`[lat, lon]`).

Sebelum mulai mengirim, skrip mencetak ringkasan rute sebagai acuan untuk
mencocokkan layar: jumlah titik, panjang total, dan setiap belokan yang
dideteksi dari geometri rute (`navgeo.py`, sama dengan yang dikirim ke
layar). Contoh untuk rute ORS
`fetch_route.py --from "-6.1790,106.8225" --to "-6.1900,106.8242" --name monas`:

```
route: routes/monas.geojson (GeoJSON FeatureCollection)
  58 points, 2361 m
  3 turns (direction change over 20 m either side, sharper than 35°; u-turn: 150° or more within 40 m):
      #  direction  angle  from start  lat,lon
      1  u-turn      180°       512 m  -6.174452,106.822908
      2  left         94°      2085 m  -6.188491,106.823137
      3  right        91°      2192 m  -6.188468,106.824101
```

`angle` adalah perubahan arah antara 20 m sebelum dan 20 m sesudah titik
belok. Kalau lekukan di sekitarnya berjumlah ≥ 150° dalam ±40 m, belokan itu
menjadi `u-turn` dan `angle`-nya adalah total lekukan tersebut. Jadi putar
balik lewat jalan samping (belokan pertama di atas; ORS sendiri hanya
menyebutnya "keep right") atau lewat celah median selebar sampai ~40 m
terbaca satu `u-turn`, bukan belok kanan. Belokan lain tidak pernah dibuang,
karena detektor hanya melihat garis rute, bukan jalan di sekitarnya.
`lat,lon` berurutan seperti Google Maps, jadi bisa ditempel ke kotak
pencarian untuk melihat lokasinya.

Kalau `--chunk-size` lebih besar dari batas write link, atau terlalu kecil
sehingga pesan terbesar (275 B) butuh lebih dari 32 frame, skrip berhenti
dengan pesan error. Catatan BlueZ: saat menyambung ulang, bleak kadang
melaporkan batas write 20 B walaupun MTU link 517; skrip lalu membaca MTU
link langsung.

Statistik di sisi ESP32 dicetak ke serial tiap 5 detik selama tersambung
(`ble: ... complete, dropped (missing chunk), ... lost by seq ...`).

## Rute dari OpenRouteService

`fetch_route.py` mengambil rute mobil dari OpenRouteService (Directions V2,
profile `driving-car`) **sekali**, lalu menyimpan respons GeoJSON-nya apa
adanya ke `routes/<nama>.geojson`. Uji `--latlon` berikutnya memakai berkas
itu, jadi bisa diulang tanpa internet dan tanpa memakai kuota.

```bash
.venv/bin/python fetch_route.py --from "-7.9666,112.6326" --to "-7.9826,112.6308" --name kampus
.venv/bin/python ble_sender.py --latlon routes/kampus.geojson
```

- Titik ditulis **`lat,lon`** (latitude dulu), sama dengan urutan yang muncul
  saat klik kanan di Google Maps; spasi setelah koma boleh. Script yang
  membaliknya ke `lon,lat` untuk ORS.
- Titik yang tampak tertukar (latitude di luar −90…90) ditolak beserta urutan
  yang benar.
- API key diambil dari environment variable `ORS_API_KEY`, atau dari baris
  `ORS_API_KEY=...` di `tools/ble_sender/.env`. Key tidak pernah dicetak.
- Berkas yang sudah ada tidak ditimpa, kecuali dengan `--force`.
- `.env` dan `routes/*.geojson` (berisi lokasi pribadi) masuk `.gitignore`.

## Modul

| Berkas | Isi |
| --- | --- |
| `navproto.py` | Encoder pesan `'N'` dan pemecah frame (cermin `src/nav_codec.c` dan `src/nav_frag.c`) |
| `navgeo.py` | **Implementasi referensi untuk app HP**: map-matching, potong jendela −12…+80 satuan, equirectangular, sederhanakan ≤ 32 titik, heading dari tangen rute, rotasi heading-up, 15 m/satuan |
| `gps_sim.py` | GPS pura-pura yang bergerak di sepanjang rute, rute contoh, pembaca file rute (GeoJSON / `points`) beserta validasinya |
| `synthetic.py` | Port simulator firmware |
| `fetch_route.py` | Pengambil rute OpenRouteService ke `routes/<nama>.geojson` |
| `ble_sender.py` | Skrip utama |

## Test

```bash
.venv/bin/python -m pytest
```

Test format memakai vektor byte yang sama dengan unit test C
(`test/test_nav_codec`, `test/test_nav_frag`), jadi kedua sisi dijamin
sepakat soal layout.
