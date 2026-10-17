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

Log aplikasi firmware (`nav: ...`, `ble: ...`) keluar lewat port USB native
(`ARDUINO_USB_CDC_ON_BOOT=1`). Kalau board tersambung lewat port UART/COM
(chip CH343, `1a86:55d3` di `lsusb`), yang terlihat hanya log boot ROM.
Untuk membaca log aplikasi di port itu, build dengan:

```bash
env PLATFORMIO_BUILD_FLAGS="-DNAV_SOURCE=NAV_SOURCE_BLE -UARDUINO_USB_CDC_ON_BOOT -DARDUINO_USB_CDC_ON_BOOT=0" \
    pio run -t upload --upload-port /dev/ttyACM0
```

(gcc memperingatkan `ARDUINO_USB_CDC_ON_BOOT redefined`; nilai dari baris
perintah yang dipakai.)

## Pemakaian

```bash
.venv/bin/python ble_sender.py --synthetic                              # rute acak seperti simulator firmware
.venv/bin/python ble_sender.py --latlon                                 # rute contoh di sekitar Monas
.venv/bin/python ble_sender.py --latlon routes/kampus.geojson --noise 5 # rute sendiri + noise GPS 5 m
.venv/bin/python ble_sender.py --latlon routes/kampus.geojson --speed-kmh 15
```

Saat berjalan:

- **Enter** = jeda/lanjut (koneksi tetap tersambung, untuk menguji state
  "tidak ada sinyal" setelah 3 detik).
- **Angka + Enter** = ganti kecepatan `--latlon` (km/h), mis. `10` untuk
  melambat menjelang belokan, `0` untuk berhenti di tempat, lalu `40` lagi.
- **q + Enter** = keluar.

| Opsi | Arti |
| --- | --- |
| `--synthetic` | Rute acak langsung dalam koordinat lokal (setara `src/nav_sim.cpp`); hanya menguji transport |
| `--latlon [FILE]` | Rute lat/lon (GeoJSON atau daftar `points`, lihat di bawah) + posisi GPS simulasi; menjalankan pipeline HP (`navgeo.py`). Tanpa FILE memakai rute contoh |
| `--maneuvers ors\|geometry` | Sumber maneuver untuk `--latlon`. Bawaan: `ors` kalau berkas punya steps ORS, selain itu `geometry` (lihat "Maneuver") |
| `--speed-kmh KMH` | Kecepatan `--latlon`, 0–255 (bawaan 40; `--speed` juga diterima) |
| `--noise M` | Noise GPS `--latlon`, simpangan baku dalam meter |
| `--rate HZ` | Pesan per detik (bawaan 5) |
| `--chunk-size N` | Byte per write, header frame ikut dihitung. Bawaan: batas write tanpa respons dari link. Coba `182` (setara iOS) dan `20` (MTU minimum BLE) |
| `--drop P` | Lewati seluruh pesan dengan peluang P → firmware mencatat `lost by seq` |
| `--drop-frames P` | Lewati satu frame dengan peluang P → firmware membuang pesan itu (`dropped (missing chunk)`) |
| `--pause-cycle RUN,PAUSE` | Kirim RUN detik, jeda PAUSE detik, berulang |
| `--duration S`, `--seed N` | Berhenti setelah S detik; seed acak supaya bisa diulang |
| `--dry-run` | Tanpa BLE: hanya membangun dan menghitung frame (ringkasan dan baris status tetap dicetak) |

Kalau `--chunk-size` lebih besar dari batas write link, atau terlalu kecil
sehingga pesan terbesar (275 B) butuh lebih dari 32 frame, skrip berhenti
dengan pesan error. Catatan BlueZ: saat menyambung ulang, bleak kadang
melaporkan batas write 20 B walaupun MTU link 517; skrip lalu membaca MTU
link langsung.

Statistik di sisi ESP32 dicetak ke serial tiap 5 detik selama tersambung
(`ble: ... complete, dropped (missing chunk), ... lost by seq ...`).

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

### Maneuver

Dua sumber, dipilih dengan `--maneuvers`:

- **`ors`** (bawaan kalau berkas punya `properties.segments[].steps`):
  instruksi OpenRouteService, yang nanti dipakai app HP. Posisi step adalah
  titik rute `way_points[0]`-nya. Tipe instruksi ORS dipetakan ke empat
  maneuver firmware (`maneuvers.py`, tabelnya di
  `docs/navigation-protocol.md` bagian 1, "Maneuver dari routing"):
  left/sharp/slight/keep → kiri/kanan, u-turn → putar balik,
  depart/straight/arrive → tanpa maneuver. Bundaran ditampilkan sebagai
  belokan jalan keluarnya (lurus = tanpa maneuver), sampai firmware punya
  ikon bundaran.
- **`geometry`**: belokan dari bentuk garis, untuk gambar geojson.io yang
  tidak punya steps. Ambangnya: perubahan arah ≥ 35° antara 20 m sebelum
  dan sesudah titik; kalau lekukan di sekitarnya berjumlah ≥ 150° dalam
  ±40 m, menjadi putar balik. Jadi putar balik lewat jalan samping atau
  celah median selebar sampai ~40 m terbaca satu putar balik. Belokan lain
  tidak pernah dibuang, karena detektor hanya melihat garis rute, bukan
  jalan di sekitarnya.

Sebelum mengirim, skrip mencetak ringkasan sebagai acuan untuk mencocokkan
layar: jumlah titik, panjang, belokan dari bentuk garis, lalu (kalau ada
steps) daftar steps ORS berdampingan dengan belokan garis beserta setiap
selisihnya. Contoh untuk rute ORS
`fetch_route.py --from "-6.1790,106.8225" --to "-6.1900,106.8242" --name monas`:

```
route: routes/monas.geojson (GeoJSON FeatureCollection)
  58 points, 2361 m
  3 turns in the line (direction change over 20 m either side, sharper than 35°; u-turn: 150° or more within 40 m):
      #  direction  angle  from start  lat,lon
      1  u-turn      180°       512 m  -6.174452,106.822908
      2  left         94°      2085 m  -6.188491,106.823137
      3  right        91°      2192 m  -6.188468,106.824101
  7 ORS steps next to the turns in the line (same place: within 40 m):
      #  from start  ORS step                        display   line                  check
      1         0 m  depart, Jalan Medan Merdeka B…  -
      2       486 m  keep right                      right     u-turn 180° at 512 m  different
      3      1185 m  enter roundabout, exit 2, Jal…  -
      4      1244 m  straight, Jalan Mohammad Husn…  -
      5      2085 m  left, Jalan Sunda               left      left 94° at 2085 m    same
      6      2192 m  right, Jalan Timor              right     right 91° at 2192 m   same
      7      2361 m  arrive                          -
  1 difference:
    - at 486 m ORS says keep right (shown as right), the line makes a u-turn 180° at 512 m
  the display gets the ORS maneuvers (--maneuvers geometry: the turns in the line)
```

`angle` adalah perubahan arah di titik itu (untuk putar balik: total
lekukannya). `lat,lon` berurutan seperti Google Maps, jadi bisa ditempel ke
kotak pencarian untuk melihat lokasinya. `display` adalah yang dikirim ke
layar dengan `--maneuvers ors`; `check` membandingkannya dengan belokan
garis (`same`, `different`, `ORS only`, `geometry only`).

Jalan samping selalu kosong di `--latlon`: ORS tidak menyediakan cabang
persimpangan (bagian 13.3 dokumentasi proyek).

### Mencocokkan layar

Selama mengirim, skrip mencetak satu baris per detik:

```
  12.0 s | at   136 m | next: right in 350 m | 2226 m to go | 40 km/h
```

yaitu detik sejak mulai, posisi rider (meter dari awal rute, hasil
map-matching), maneuver berikutnya dan jaraknya persis seperti yang dikirim
ke layar, sisa rute, dan kecepatan. Setelah belokan terakhir: `no turn
ahead, end of route in … m` (layar menampilkan lurus dengan jarak ke
tujuan). Saat dijeda, baris diberi `[paused: not sent]`. Jeda dan lanjut
dicatat dengan waktunya, mis. `10.0 s | paused (link kept up), last message
0.20 s ago`.

### Uji data basi

```bash
.venv/bin/python ble_sender.py --latlon routes/monas.geojson --pause-cycle 10,10 --duration 30
```

Kirim 10 detik, jeda 10 detik dengan koneksi tetap tersambung, kirim lagi.
Serial firmware mencatat `nav: no signal (no valid message for 3 s)` dan
`nav: signal back`. Hasil 17 Okt 2026: "no signal" 3,08 s setelah pesan
terakhir, pulih 0,05 s setelah pengiriman berlanjut, tanpa reconnect
(`docs/navigation-protocol.md` bagian 1).

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
  `ORS_API_KEY=...` di `tools/ble_sender/.env` (hanya key-nya, tanpa label
  lain). Key tidak pernah dicetak.
- Berkas yang sudah ada tidak ditimpa, kecuali dengan `--force`.
- `.env` dan `routes/*.geojson` (berisi lokasi pribadi) masuk `.gitignore`.

## Test vector untuk app HP

`make_test_vectors.py` menulis `test_vectors/*.json`: input pipeline
(rute lat/lon, maneuver, posisi GPS) beserta hasilnya sampai pesan `'N'`
dan frame BLE dalam hex, untuk belok kanan, belok kiri, putar balik, rute
lurus, dekat ujung rute, dan tikungan yang disederhanakan. Port TypeScript
harus menghasilkan byte yang sama. Koordinatnya sintetis; rincian ada di
`test_vectors/README.md`.

## Modul

| Berkas | Isi |
| --- | --- |
| `navproto.py` | Encoder pesan `'N'` dan pemecah frame (cermin `src/nav_codec.c` dan `src/nav_frag.c`) |
| `navgeo.py` | **Implementasi referensi untuk app HP**: map-matching, potong jendela −12…+80 satuan, equirectangular, sederhanakan ≤ 32 titik, heading dari tangen rute, rotasi heading-up, 15 m/satuan; belokan dari bentuk garis |
| `maneuvers.py` | Maneuver dari steps ORS: pemetaan tipe instruksi, bundaran, dan pemasangan dengan belokan garis |
| `gps_sim.py` | GPS pura-pura yang bergerak di sepanjang rute, rute contoh, pembaca file rute (GeoJSON / `points`) beserta validasinya |
| `synthetic.py` | Port simulator firmware |
| `fetch_route.py` | Pengambil rute OpenRouteService ke `routes/<nama>.geojson` |
| `make_test_vectors.py` | Pembuat `test_vectors/*.json` |
| `ble_sender.py` | Skrip utama |

## Test

```bash
.venv/bin/python -m pytest
```

Test format memakai vektor byte yang sama dengan unit test C
(`test/test_nav_codec`, `test/test_nav_frag`), jadi kedua sisi dijamin
sepakat soal layout. `test_test_vectors.py` gagal kalau `test_vectors/`
tidak lagi cocok dengan pipeline Python.
