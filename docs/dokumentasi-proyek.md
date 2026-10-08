# Butamap GPS — Dokumentasi Proyek & Persiapan Data Asli

Status per 8 Oktober 2026: `main` (`9fb50aa`, fase 1 dan 2 jalur data asli:
format biner `'N'`, fragmentasi BLE, penerima BLE, state "tidak ada sinyal",
pengirim uji di laptop) ditambah kompas opsional (`ENABLE_COMPASS`) dan
periode refresh 30 ms.

Dokumen ini berdiri sendiri: ditulis supaya bisa dibaca tanpa membuka repo,
sebagai bahan untuk merencanakan peralihan dari data simulasi ke data asli
(GPS, kompas, dan app HP lewat BLE). Bagian yang **sudah jalan** dan yang
masih **usulan** dibedakan dengan jelas. Detail sejarah desain protokol ada
di `docs/navigation-protocol.md`.

---

## 1. Ringkasan

Butamap GPS adalah layar navigasi turn-by-turn untuk dashboard motor: modul
ESP32-S3 dengan layar bulat 240×240 (GC9A01). Rencananya app HP melakukan
routing dan mengirim data navigasi lewat BLE; ESP32 hanya menampilkan.

Bawaan firmware masih **data simulasi** (`NAV_SOURCE_SIM`): simulator di
ESP32 membuat rute acak tanpa ujung dan "mengendarai" rider di
sepanjangnya, sehingga seluruh tampilan bisa diuji tanpa sensor atau app.
Jalur data asli sudah terbukti end-to-end dengan `NAV_SOURCE_BLE`: pesan
biner `'N'` dipecah menjadi frame BLE, dirakit ulang dan didekode di ESP32,
lalu ditampilkan; tanpa data valid > 3 detik atau saat link putus, layar
masuk state "tidak ada sinyal". Pengirimnya untuk sementara skrip Python di
laptop (`tools/ble_sender/`), termasuk implementasi referensi konversi
lat/lon untuk app HP. Yang belum ada: app HP itu sendiri.

## 2. Status singkat

| Bagian | Status |
| --- | --- |
| Layar navigasi (rute, jalan samping, panah, panel jarak, busur progres) | Selesai, diuji di board |
| Animasi halus (easing, rute menerus, belokan berputar mulus) | Selesai |
| Performa render (refresh 30 ms → ±33 fps, rata-rata 12–13 ms per frame) | Selesai, diukur di board: frame terberat ≤ 25,3 ms, tidak ada frame yang melewati periode (bagian 9) |
| Simulator data dummy (rute acak, jalan samping, belokan) | Selesai |
| Kontrak data `nav_data_t` (antarmuka sumber data → UI) | Selesai |
| Pilihan sumber data `NAV_SOURCE` (SIM / LOOPBACK / BLE) | Selesai, tanpa fallback runtime ke simulator (bagian 6) |
| Format payload navigasi lewat BLE (pesan biner `'N'`) | Selesai: encoder/decoder defensif `src/nav_codec.c` (bagian 13.4) |
| Fragmentasi BLE (frame `0xCE`, rakit ulang per nomor pesan) | Selesai: `src/nav_frag.c`, jalan dari write 20 B (MTU minimum) sampai 514 B (bagian 13.4) |
| Unit test host (`pio test -e native`) | Selesai: 51 test (codec, fragmentasi, simulator lewat codec); juga lolos saat dijalankan manual dengan ASan/UBSan |
| Mode loopback (simulator → encode → frame 20 B → rakit → decode → layar) | Selesai, diuji di board: 0 error, tampilan sama dengan SIM |
| BLE server (Nordic UART Service, Bluedroid) | Jalan: frame navigasi + `GPS:lat,lon,speed` lama; diuji dari laptop di 514/182/20 B per write |
| Pengisian `nav_data_t` dari data asli (`NAV_SOURCE_BLE`) | Selesai: task BLE → queue FreeRTOS panjang 1 → `loop()`; `ble_connected` diisi; paket hilang dihitung dari nomor urut |
| State "tidak ada sinyal" (> 3 detik tanpa data valid / link putus) | Selesai: rute diredupkan, panel menampilkan ikon sinyal dicoret dan `--` |
| Pengirim uji di laptop (`tools/ble_sender/`, Python + bleak) | Selesai: mode `--synthetic` dan `--latlon` (pipeline bagian 13.2), 28 test pytest |
| App HP (routing, konversi koordinat, kirim BLE) | **Belum ada** — `tools/ble_sender/navgeo.py` adalah referensinya |
| Kompas QMC5883L | **Belum terpasang** (board hanya ESP32-S3 + layar). Kode opsional lewat `-DENABLE_COMPASS=1` (bawaan 0): probe sekali saat boot, tidak di-poll kalau tidak merespons. Hanya dipakai layar debug lama (bagian 13.6) |
| IMU (MPU6050) | Belum ada di hardware |

## 3. Hardware

| Komponen | Detail |
| --- | --- |
| Board | ESP32-S3 DevKitC-1, chip ESP32-S3 (QFN56) rev 0.2, flash 16 MB (quad), PSRAM 8 MB embedded |
| USB | Port USB-Serial/JTAG bawaan, muncul sebagai `/dev/ttyACM*` |
| Layar | GC9A01, bulat, 240×240, SPI |
| Kompas | Belum terpasang. Kode mendukung QMC5883L di I2C alamat `0x0D` (opsional, `ENABLE_COMPASS`) |
| LED | NeoPixel di GPIO 48 (dimatikan saat boot) |

Sambungan layar (SPI2/FSPI, 80 MHz):

| Pin layar | GPIO |
| --- | --- |
| SCL / SCK | 9 |
| SDA / MOSI | 8 |
| CS | 13 |
| DC | 14 |
| RST | 21 |
| BL / BLK | ke 3V3 (kode tidak mengatur backlight) |
| VCC / GND | 3V3 / GND |

Kompas (kalau nanti dipasang): SDA = GPIO 2, SCL = GPIO 1, clock I2C 50 kHz.

Catatan upload: setelah upload, board kadang tertahan di mode download
(layar tetap di teks hijau pembuka atau hitam). Tekan tombol **RST** atau
cabut-colok USB. Port bisa berganti antara `ttyACM0` dan `ttyACM1`.

## 4. Software & build

- PlatformIO, platform `espressif32`, framework Arduino (berbasis ESP-IDF 4.4).
- **LVGL dikunci ke 9.6.0** (`platformio.ini`): versi 9.5 ±3 ms lebih lambat
  per frame, dan versi yang tidak dikunci pernah membuat dua folder build
  memakai versi berbeda.
- TFT_eSPI 2.5.43 hanya untuk inisialisasi panel dan teks pembuka di
  `setup()`. Setelah itu panel dikendalikan langsung lewat `spi_master`
  ESP-IDF (bagian 9).
- Konfigurasi LVGL: `include/lv_conf.h` (salinan identik di root
  `lv_conf.h`), heap LVGL 64 KB, font Montserrat 14 dan 36 aktif.
- Kompilasi `-O2` (menggantikan `-Os` bawaan) karena render LVGL software.
- Pemakaian memori (`NAV_SOURCE_SIM`): RAM 78% (±255 KB dari 320 KB;
  termasuk buffer layar penuh 115 KB), flash 43,7%. `NAV_SOURCE_BLE`: RAM
  77,1%, flash ±44%.
- Stack BLE: **Bluedroid** (library `BLEDevice` bawaan Arduino-ESP32 2.0.17;
  NimBLE tidak aktif). Heap internal bebas saat runtime di `NAV_SOURCE_BLE`
  tanpa kompas: ±45,4 KB saat tersambung dan menerima data, minimum tercatat
  ±44 KB, blok terbesar ±36,9 KB. Dengan `ENABLE_COMPASS=1` driver I2C ikut
  aktif dan memakai ±2,8 KB lagi. (Pengukuran fase 2 mendapat minimum ±35 KB
  saat kode kompas lama masih mem-poll sensor yang tidak ada.) Task Bluedroid
  (BTC) hanya punya stack 3 KB, jadi semua struct besar di jalur terima
  `static`.
- Pilihan saat build (bisa diubah di header-nya atau lewat `-D…` di
  `build_flags`): `NAV_SOURCE` (`src/nav_source.h`), `ENABLE_COMPASS`
  (`src/hw_config.h`, bawaan 0), `LV_PORT_PERF_LOG` (`src/lv_port_disp.cpp`).

Build, upload, dan test:

```bash
pio run                                   # build firmware
pio run -t upload --upload-port /dev/ttyACM0
pio device monitor -b 115200              # log serial
pio test -e native                        # unit test di host (gcc + Unity)
cd tools/ble_sender && .venv/bin/python -m pytest   # test pengirim Python
```

`pio run` hanya membangun firmware (`default_envs`). Env `native` khusus
test host; env board memakai `test_ignore = *`, jadi `pio test` tanpa
`-e native` tidak menjalankan apa pun dan tidak meng-upload ke board.

Sumber data per build tanpa mengubah berkas (memicu build ulang penuh):

```bash
env PLATFORMIO_BUILD_FLAGS="-DNAV_SOURCE=NAV_SOURCE_LOOPBACK" pio run -t upload --upload-port /dev/ttyACM0
```

Log performa: ubah `LV_PORT_PERF_LOG` di `src/lv_port_disp.cpp` menjadi `1`,
atau per build dengan `-DLV_PORT_PERF_LOG=1`. Tiap 2 detik dicetak ke serial:

```
[perf] 33.3 fps | longest gap 33.7 ms | frame avg 12.5 ms, max 24.3 ms (render only max 23.6, DMA wait max 5.9) | 0 over 30 ms | flush 0.3 ms | 46962 px/frame
```

"Frame" dihitung dari mulai render sampai frame diantrekan ke DMA, termasuk
menunggu DMA frame sebelumnya; "render only" adalah frame tanpa tunggu itu;
"over 30 ms" adalah jumlah frame yang lebih lama dari periode refresh.

## 5. Struktur kode

| Berkas | Isi |
| --- | --- |
| `src/main.cpp` | `setup()`/`loop()`, BLE server (frame navigasi + `GPS:`), pembacaan QMC5883L (`NAV_SOURCE_BLE` + `ENABLE_COMPASS=1`), deteksi data basi, jalur loopback |
| `src/nav_source.h` | Pilihan sumber data `NAV_SOURCE` (SIM / LOOPBACK / BLE) |
| `src/hw_config.h` | Hardware opsional: `ENABLE_COMPASS` (bawaan 0, board belum punya sensor) |
| `src/nav_sim.h` | **Kontrak data** `nav_data_t` + API simulator |
| `src/nav_sim.cpp` | Simulator rute dummy |
| `src/nav_codec.h/.c` | Encoder/decoder biner pesan `'N'` (C murni, tanpa Arduino/LVGL) |
| `src/nav_frag.h/.c` | Fragmentasi BLE: pemecah frame dan perakit ulang berbuffer tetap (C murni) |
| `src/nav_rx.h/.cpp` | Penerima BLE: rakit → decode → queue FreeRTOS panjang 1; hitung paket hilang; log statistik + heap |
| `test/test_nav_codec/` | Unit test codec: round-trip, nilai batas, paket rusak, korupsi acak |
| `test/test_nav_frag/` | Unit test fragmentasi: urutan normal, potongan hilang/dobel, pesan menyela, header rusak, fuzz |
| `test/test_nav_loopback/` | Simulator lewat codec, dibandingkan dengan keluaran simulator langsung |
| `tools/ble_sender/` | Pengirim uji BLE di laptop (Python + bleak) dan pipeline lat/lon referensi untuk app HP; lihat README-nya |
| `src/lv_port_disp.cpp` | Port layar LVGL: mode DIRECT, kirim frame lewat DMA `spi_master`, tick dari `millis()`, log performa |
| `src/ui_init.cpp` | Memilih layar awal; fungsi `ui_update_*` yang dipanggil `main.cpp` |
| `screens/ui_nav_display.c/.h` | Layar navigasi (semua fitur di bagian 7–8) |
| `screens/ui_nav_raster.c/.h` | Rasterizer kecil untuk lapisan peta (garis anti-aliasing langsung ke buffer) |
| `screens/ui_gps_tracker.c/.h` | Layar debug lama (teks lat/lon, kompas X/Y/Z, jarum heading) |
| `docs/navigation-protocol.md` | Catatan desain protokol, jendela rute, junction view, performa |
| `screens/ui_*` lain, `components/`, `images/`, `fonts/` | Sisa contoh SquareLine (tema smartwatch); hanya fontnya yang dipakai |

Antarmuka yang dipanggil `main.cpp` (di `ui_init.cpp`):

```c
void ui_update_nav_display(const nav_data_t *nav); // tiap loop(): layar navigasi
void ui_update_nav_signal(bool has_signal);        // tiap loop(): data masih berlaku?
void ui_update_nav_info(const nav_data_t *nav);    // tiap 200 ms: layar debug
void ui_update_nav_heading(float bearing_deg);     // jarum heading layar debug
void ui_update_gps(double lat, double lon, float speed, float heading,
                   int16_t x, int16_t y, int16_t z, bool connected); // label layar debug
```

`UI_START_DEBUG_SCREEN` di `src/ui_init.cpp`: `0` = layar navigasi
(bawaan), `1` = layar debug lama.

## 6. Alur data saat ini

Sumber data dipilih saat kompilasi dengan `NAV_SOURCE` di
`src/nav_source.h` (menggantikan `USE_DUMMY_DATA`):

```
NAV_SOURCE_SIM (bawaan):

  loop() ─► nav_sim_update(millis()) ─► nav_data_t ─► ui_update_nav_display()
                                                   └► ui_update_nav_info() (debug, 200 ms)

NAV_SOURCE_LOOPBACK (uji format biner + fragmentasi tanpa radio):

  loop() ─► nav_sim_update() ─► nav_encode() ─► frame 20 B ─► nav_frag_push() ─► nav_decode() ─► (sama seperti SIM)
            serial tiap 5 detik: "loopback: … packets (…/s), avg … B, max … B (… frames of 20 B), errors …"

NAV_SOURCE_BLE:

  task BLE:  write RX ─► byte 0 = 0xCE? ─► nav_rx_on_frame() ─► nav_frag_push() ─► nav_decode()
                                                        └► xQueueOverwrite(queue panjang 1)
  loop():    xQueuePeek() ─► umur data ≤ 3 s dan link tersambung?
                 ├► ui_update_nav_signal(ya/tidak)
                 └► ui_update_nav_display(data terbaru, ble_connected diisi)
             updateCompass() (hanya ENABLE_COMPASS=1 dan sensor menjawab) ─► ui_update_gps() (layar debug saja)

Semua mode:

  BLE "GPS:lat,lon,speed" ─► gpsData (lat/lon/speed) ─► hanya label layar debug
  BLE frame 0xCE di SIM/LOOPBACK ─► diabaikan
```

Task BLE tidak pernah menyentuh LVGL; hanya `loop()` yang memanggil UI. Saat
link putus, queue dikosongkan dan perakit ulang direset, jadi data lama tidak
pernah tampak berlaku setelah tersambung lagi.

**Tidak ada fallback otomatis ke simulator saat runtime**: rute palsu di
layar saat koneksi putus berbahaya di jalan. Nilai `NAV_SOURCE` yang salah
ketik gagal dikompilasi (`#error`).

Layar navigasi **hanya** membaca `nav_data_t`. Mengganti sumber data cukup
dengan mengisi struct yang sama dari sumber lain, tanpa menyentuh kode UI.

## 7. Kontrak data `nav_data_t`

Didefinisikan di `src/nav_sim.h`:

```c
typedef enum {
    NAV_MANEUVER_STRAIGHT = 0,
    NAV_MANEUVER_TURN_LEFT,
    NAV_MANEUVER_TURN_RIGHT,
    NAV_MANEUVER_U_TURN,
    NAV_MANEUVER_COUNT
} nav_maneuver_t;

typedef struct { float x; float y; } nav_point_t;

#define NAV_SCHEMATIC_MAX_POINTS 32
#define NAV_SIDE_ROAD_MAX_POINTS 4
#define NAV_SIDE_ROADS_MAX 8

typedef struct {
    nav_point_t points[NAV_SIDE_ROAD_MAX_POINTS]; // titik 0 di rute
    uint8_t point_count;
} nav_side_road_t;

typedef struct {
    float bearing_deg;              // 0-359.9, heading kompas
    float distance_to_turn_m;       // meter ke maneuver berikutnya
    nav_maneuver_t maneuver;        // maneuver berikutnya
    float total_distance_m;         // sisa jarak seluruh rute
    float speed_kmh;                // kecepatan saat ini
    nav_point_t schematic_points[NAV_SCHEMATIC_MAX_POINTS]; // garis rute
    uint8_t schematic_point_count;
    nav_side_road_t side_roads[NAV_SIDE_ROADS_MAX];         // jalan samping
    uint8_t side_road_count;
    bool ble_connected;             // link app HP tersambung (NAV_SOURCE_BLE)
} nav_data_t;
```

### Field dan pemakaiannya

| Field | Dipakai layar navigasi? | Arti |
| --- | --- | --- |
| `schematic_points`, `schematic_point_count` | Ya | Garis rute di sekitar rider (2–32 titik) |
| `side_roads`, `side_road_count` | Ya | Cabang jalan di sekitar rute (0–8 polyline, 2–4 titik) |
| `maneuver` | Ya | Ikon di panel bawah |
| `distance_to_turn_m` | Ya | Angka jarak + busur progres |
| `bearing_deg` | Tidak (hanya jarum di layar debug) | Heading kompas, 0 = utara. Tidak ikut pesan `'N'` |
| `speed_kmh` | Tidak (hanya layar debug) | Kecepatan |
| `total_distance_m` | Tidak | Sisa jarak rute |
| `ble_connected` | Tidak (layar memakai `ui_update_nav_signal`) | Status link app HP, diisi `loop()` di `NAV_SOURCE_BLE`. Tidak ikut pesan `'N'` |

### Sistem koordinat garis rute dan jalan samping

- **Lokal dan heading-up**, bukan lat/lon: rider di titik asal `(0,0)`,
  `+y` = arah hadap rider (ke depan), `+x` = kanan rider.
- **Satuan skematik**, bukan meter. Layar menggambar **4 px per satuan**.
  Data dummy memakai 15 m jalan per satuan (jadi ±33 satuan ≈ 500 m terlihat
  di depan rider). Skalanya bebas dipilih sumber data selama konsisten.
- Di layar, titik asal berada di piksel `(120, 132)`, di tengah panah rider
  (ujung panah di y = 118).
  Layar bulat berjari-jari 120 px, jadi yang terlihat kira-kira 33 satuan ke
  depan, ±30 satuan ke samping, dan ±5–15 satuan ke belakang sebelum
  tertutup panel bawah.

### Aturan yang diharapkan UI dari data

- **Rider harus berada di garis rute** (di atau sangat dekat titik asal). UI
  menjangkarkan garis pada titik rute yang paling dekat dengan `(0,0)`.
- Garis rute sebaiknya mencakup **sedikit di belakang rider (±12 satuan)
  dan jauh ke depan (±80 satuan)**, melewati tepi layar, supaya garis tidak
  terlihat berhenti di tengah dan belokan berikutnya sudah terlihat.
- Maksimal 32 titik: pakai titik sudut saja (polyline yang sudah
  disederhanakan). UI sendiri melakukan resampling dan penghalusan.
- Jalan samping: titik pertama tepat di garis rute (titik persimpangan),
  titik terakhir di ujung jauh. Panjang wajar 6–14 satuan.
- `schematic_point_count < 2` = tidak ada rute (garis disembunyikan).
- Data boleh dikirim dengan frekuensi berapa pun; UI menghaluskan sendiri
  (bagian 8). Data yang lebih sering (5–10 Hz) terlihat paling mulus.

## 8. Layar navigasi — fitur

Semua digambar dari kode (tanpa bitmap), layar heading-up 240×240:

- **Garis rute**: putih tebal (8 px) dengan tepi abu tipis (2 px) di kedua
  sisi, sambungan membulat. Rute ditampilkan sebagai jendela menerus, jadi
  belokan kedua dan ketiga sudah terlihat sebelum tiba.
- **Jalan samping**: sepasang garis tepi tipis abu-kebiruan (2 px, berjarak
  8 px) yang mengikuti bengkok polyline. Warnanya memudar satu tingkat per
  segmen (sampai 3 tingkat), ujung terjauh ±70% lebih redup.
  Tepi rute **dibuka** di tempat jalan samping bertemu (mulut persimpangan),
  sehingga tampak sebagai satu jaringan jalan.
- **Panah rider 3D**: chevron putih dengan sisi kiri terang dan kanan teduh,
  ketebalan di bawahnya, dan bayangan lembut; sedikit di bawah tengah layar.
- **Panel bawah berbentuk kubah**: ikon maneuver (lurus / kiri / kanan /
  putar balik) dan jarak ke maneuver. Jarak dibulatkan per 10 m (≥ 100 m)
  atau 5 m (< 100 m); mulai ±1 km ditampilkan dalam km satu desimal
  (`1.5 km`). Angka Montserrat 36 Medium, satuan Montserrat Bold 20.
- **Busur progres** di tepi bawah: terisi dari kiri ke kanan selama 500 m
  terakhir sebelum belokan (`PROGRESS_RANGE_M`), kosong kalau masih jauh.
- **State "tidak ada sinyal"** (`ui_nav_display_set_signal(false)`, dipanggil
  saat > 3 detik tanpa pesan valid atau link putus): rute dan jalan samping
  memudar ke ±30% kecerahan (warna dicampur ke latar di rasterizer, ±0,4 s),
  ikon diganti batang sinyal abu yang dicoret oranye, jarak diganti `--`
  abu, satuan dikosongkan, busur progres langsung kosong. Rute terakhir tetap
  terlihat redup supaya rider tahu posisinya, tapi panah belok dan jarak
  lama tidak pernah tampil seolah masih berlaku. Saat data kembali, peta
  memudar terang lagi dan bergeser mulus ke posisi baru. Tidak ada tugas
  gambar LVGL tambahan per frame: ikon di-cache (digambar ulang hanya saat
  state berganti), teks hanya berubah saat state berganti, dan setelah
  pudar selesai layar diam tidak digambar ulang sama sekali.

Kehalusan gerak (dirancang supaya data patah-patah, misalnya BLE 1×/detik,
tetap terlihat mulus):

- `ui_nav_display_set()` hanya menyimpan target; timer LVGL 20 ms
  menggerakkan tampilan ke target dengan easing (`EASE_RATE` 10/detik,
  ±100 ms jeda).
- Garis rute di-resample menjadi 73 titik berjarak 1,25 satuan, dihitung
  dari titik rute terdekat dengan rider (8 di belakang, 65 di depan). Titik
  ke-i selalu "posisi yang sama relatif terhadap rider", jadi setiap
  perubahan data menjadi gerakan mulus, termasuk saat rute berputar di
  belokan.
- Jalan samping dipasangkan dengan yang tampil sebelumnya (titik awal dalam
  `3 + 0,15 × jarak-dari-rider` satuan dan arah segmen pertama mirip); yang
  baru muncul dengan fade ±200 ms, yang hilang memudar keluar.
- Ikon maneuver dan teks jarak berganti langsung saat datanya berubah.
- Posisi jalan samping digambar sub-piksel (tanpa loncatan 1 px).

## 9. Pipeline render & performa

Diukur di board: frame yang bergerak butuh rata-rata **12–13 ms** dan paling
berat ±24 ms; refresh tiap **30 ms** → **±33 fps**. Yang membuatnya cukup
cepat:

- **Lapisan peta digambar sendiri** (`ui_nav_raster.c`): latar, jalan
  samping, tepi/inti rute, dan mulut persimpangan ditulis langsung ke buffer
  dengan segmen anti-aliasing berujung bulat yang hanya mengunjungi piksel
  di sekitar garis. Renderer garis LVGL makan ±0,1–0,3 ms per garis di
  ESP32-S3; dengan rasterizer ini seluruh peta ±4–8 ms.
- **LVGL mode DIRECT** dengan satu buffer layar penuh (RGB565 byte-terbalik,
  tanpa tukar byte): area apa pun dirender dalam satu putaran.
- **Kirim ke layar lewat `spi_master` ESP-IDF, sepenuhnya asinkron**: satu
  frame = antrean transaksi DMA (alamat, lalu potongan ≤ 64 baris dengan
  RAMWR / RAMWR_CONTINUE; satu transaksi DMA ESP32-S3 maksimal 32 KB).
- **Panah dan ikon di-cache** sebagai canvas; ikon digambar ulang hanya saat
  maneuver berganti.
- **Refresh 30 ms** (di atas frame terberat termasuk tunggu DMA; bagian 13.8)
  dan `loop()` tidur 1 ms: periode yang lebih pendek dari waktu frame
  membuat jarak antar-frame berselang-seling dan terasa patah-patah.

Pelajaran yang terbukti lewat pengukuran (berguna saat menambah fitur):

- Biaya LVGL terutama **per tugas gambar dan per putaran render**, bukan per
  piksel. Menambah elemen yang digambar LVGL tiap frame cepat menghabiskan
  jatah 30 ms; lebih baik lewat rasterizer atau cache gambar.
- Karena biaya per putaran itu, **buffer parsial lebih lambat** walaupun DMA
  bisa berjalan bersamaan: callback peta jalan sekali per potongan
  (diuji 8 Okt 2026, tabel di bawah).
- Jangan pakai DMA TFT_eSPI di board ini: versi 2.5.43 crash di ESP32-S3,
  dan mencampurnya dengan DMA driver IDF membuat panel mengabaikan frame.
- `esp_lcd` IDF 4.4 hanya mengizinkan satu transfer berjalan sekaligus, jadi
  tidak dipakai.

**Frame terberat (diukur 8 Okt 2026).** Log performa kini juga mencetak
frame terberat, render saja, dan tunggu DMA; sebelumnya hanya rata-rata, dan
angka "17 ms" di catatan lama adalah rata-rata itu. Tiga skenario yang sama
diukur berulang (frame / render saja, ms; "lewat" = frame lebih lama dari
periode refresh):

| Skenario | Fase 2: 25 ms, poll kompas yang tidak ada | 25 ms, tanpa kompas | **30 ms, tanpa kompas (dipakai)** | Eksperimen: 30 ms, 2 buffer parsial |
| --- | --- | --- | --- | --- |
| LOOPBACK, simulator C, 60 detik | 27,5 / 22,9 | 27,9 / 23,5 (18 lewat) | **24,3 / 23,6 (0 lewat)** | 31,8 / 31,8 (1 lewat) |
| BLE, data terus-menerus, 60 detik | 29,3 / 23,7 | 28,1 / 19,7 (25 lewat) | **19,7 / 19,7 (0 lewat)** | 29,6 / 29,5 (0 lewat) |
| BLE, jeda/lanjut 8/6 detik, ±32 detik | 27,9 / 19,6 | 29,1 / 20,1 (23 lewat) | **25,3 / 20,5 (0 lewat)** | 29,1 / 29,1 (0 lewat) |
| Rata-rata frame | 12–14 | 12,7–13,8 | **12,4–13,2** | 20,0–20,9 |
| Tunggu DMA terberat | ±9,8 | 9,4–10,1 | **2,0–6,7** | 0–1,5 |
| Heap internal minimum (BLE) | ±35 KB | 44,2 KB | **44,0 KB** | 101,2 KB |

Kolom pertama diukur dengan instrumentasi sementara di fase 2 (cetak per
frame berat), kolom lain dengan log performa permanen; jumlah "lewat" kolom
pertama tidak sebanding sehingga tidak ditulis. Membuang poll kompas tidak
mengubah frame terberat secara berarti. Menaikkan periode ke 30 ms
menghilangkan frame yang melewati periode, karena frame berikutnya tidak lagi
mulai sebelum DMA frame sebelumnya selesai.

Eksperimen dua buffer parsial (2 × 60 baris = 2 × 28,8 KB menggantikan
115 KB, mode `PARTIAL`): DMA memang berjalan bersamaan dengan render, dan
RAM yang bebas bertambah ±57 KB. Tapi frame jadi jauh lebih berat (rata-rata
±20 ms, terberat ±30–32 ms), karena layar dirender dalam potongan 60 baris
dan seluruh callback gambar (raster peta + kubah) jalan lagi untuk setiap
potongan. Tidak lebih baik dari periode 30 ms, jadi tidak dipakai.

Rute yang diredupkan (state "tidak ada sinyal") memakai warna redup yang
dihitung sekali per panggilan gambar (`lv_color_mix` ke warna latar), bukan
opacity per piksel. Uji A/B dengan LOOPBACK (deterministik), peta dipaksa
redup terus vs normal: rata-rata 12,4 vs 12,5 ms, terberat 24,3 vs 24,3 ms,
jadi biayanya tidak terukur.

Catatan berikut menjelaskan pola tunggu DMA yang diukur di fase 2 (periode
25 ms). Selisih antara frame dan render saja adalah **menunggu DMA frame
sebelumnya** (sampai ±9 ms). Buffer layar hanya satu, jadi LVGL harus menunggu frame sebelumnya
selesai terkirim sebelum menggambar lagi. Satu frame yang mengubah rute dan
busur sekaligus mengirim baris 0–236 (±11,4 ms di 80 MHz). Kalau render
(13–20 ms di adegan berat: banyak jalan samping, rute berkelok) ditambah DMA
melewati 25 ms, frame berikutnya mulai sebelum DMA selesai dan ikut
menunggu. Akibatnya jeda antar-frame sesekali 35–40 ms dan fps turun ke
±38. Render murni terberat (22,9 / 23,7 ms) terjadi di frame pergantian
manuver: ikon, label, peta, dan busur menjadi 3 area terpisah, dan tiap area
menjalankan seluruh callback gambar (raster + kubah) sekali lagi.

Ini sudah terjadi sebelum fase 2 (baris baseline). Untuk state "tidak ada
sinyal", dua hal dibuat supaya transisinya tidak menambah frame berat
(diukur: tanpa keduanya, frame pertama transisi butuh 27–31 ms render murni
karena 4 pass, dan ±26 frame ≥ 25 ms di skenario jeda/lanjut):

- Saat state berganti, seluruh layar di-invalidate sekali, jadi peta, ikon,
  label, dan busur digambar dalam **satu pass**, bukan empat.
- Busur progres langsung loncat ke nilai barunya (bersama ikon dan teks)
  alih-alih beranimasi; kalau tidak, area busur ikut digambar dan dikirim
  di setiap frame selama peta memudar.

Satu tambahan sementara masih tersisa: ±0,2 detik setelah data kembali,
jalan samping lama yang memudar keluar dan yang baru memudar masuk
tergambar bersamaan (sampai 14), sehingga raster ±2 ms lebih berat. Keputusan
untuk pipeline-nya (periode 30 ms) dicatat di bagian 13.8.

## 10. Simulator data dummy (`src/nav_sim.cpp`)

- Rute dunia acak tanpa ujung: ruas lurus 12–26 satuan, lalu belok
  kiri/kanan 55–100° (masing-masing 38%), lurus di perempatan (16%), atau
  putar balik (8%). Sudut belokan dipotong 2 satuan (chamfer).
- Jalan samping: cabang di persimpangan (arah yang tidak diambil rute) dan
  jalan kecil memotong ruas; polyline 4 titik, ±setengahnya melengkung
  sampai ±50°.
- Rider "dikendarai" di sepanjang rute dengan kecepatan acak 0–120 km/h
  (minimal 5 m/s); arah hadap mengikuti jalan lewat chord ±3 satuan,
  sehingga tampilan berputar halus di belokan. Skala 15 m per satuan.
- Setiap update mengirim jendela rute 12 satuan di belakang sampai 80 di
  depan rider (maks. 32 titik) dan jalan samping dalam jendela itu
  (maks. 8).
- `bearing_deg` masih sapuan waktu 0–360° per 13 detik (untuk jarum layar
  debug), tidak terkait arah rute.

## 11. Layar debug lama (`screens/ui_gps_tracker.c`)

Teks status BLE, lat/lon, kecepatan, heading, nilai mentah kompas X/Y/Z,
jarum heading berputar, label maneuver, jarak, dan pratinjau garis skematik
kecil. Aktif dengan `UI_START_DEBUG_SCREEN 1`. Berguna untuk memeriksa
sensor dan data BLE mentah.

## 12. Riwayat pencapaian

| Tanggal | Pencapaian |
| --- | --- |
| Apr–Mei 2026 | Proyek awal, desain SquareLine, pembacaan kompas berpindah HMC → QMC5883L (berhasil), monitor heading dan X/Y/Z di layar |
| 9 Sep 2026 | Simulator data dummy dan widget pratinjau navigasi |
| 4 Okt 2026 | Build diperbaiki (LVGL tidak menemukan `lv_conf.h`) |
| 5 Okt 2026 | Layar navigasi turn-by-turn (rute, panel, ikon, jarak, busur progres); tick LVGL diperbaiki; panel kubah; belokan berganti mulus; rute menerus dengan beberapa belokan sekaligus; jalan samping; panah 3D; font lebih tebal; rambu batas kecepatan dihapus |
| 5 Okt 2026 | Performa: DMA, refresh 25 ms, `-O2` → 40 fps stabil; layar yang tertahan di teks pembuka diperbaiki |
| 6 Okt 2026 | Jalan samping dua garis, bisa bengkok, menyatu dengan rute bertepi abu (mulut persimpangan); LVGL dikunci 9.6.0; mode DIRECT + DMA `spi_master`; rasterizer peta → frame terberat 17 ms |
| 8 Okt 2026 | Fase 1 jalur data asli: format biner pesan `'N'` (encoder/decoder defensif), 31 unit test host, `NAV_SOURCE` menggantikan `USE_DUMMY_DATA`, mode loopback diuji di board (0 error, 40 fps, sama dengan SIM) |
| 8 Okt 2026 | Fase 2 jalur data asli: fragmentasi BLE (frame `0xCE`, jalan di write 20 B), penerima BLE dengan queue FreeRTOS, state "tidak ada sinyal", pengirim uji Python (`--synthetic`, `--latlon`) diuji end-to-end lewat radio di 514/182/20 B per write; log performa kini mencatat frame terberat |
| 8 Okt 2026 | Kompas jadi opsional (`ENABLE_COMPASS`, bawaan 0; board belum punya sensor); refresh 25 → 30 ms setelah pengukuran frame terberat; eksperimen buffer parsial ditolak |

## 13. Persiapan data asli

### 13.1 Pembagian tugas

| App HP (belum ada) | ESP32 (firmware ini) |
| --- | --- |
| GPS posisi & course, routing (ORS / Mapbox) | Menerima data lewat BLE |
| Menentukan maneuver berikutnya dan jaraknya | Mengisi `nav_data_t` |
| Mengubah geometri rute (lat/lon) ke koordinat lokal heading-up | Menampilkan dan menghaluskan gerak |
| Mengambil jalan samping dari data persimpangan | (opsional) membaca kompas |
| Mengirim update beberapa kali per detik | Menangani putus koneksi / data basi |

ESP32 **tidak** melakukan konversi lat/lon → lokal; semua geometri datang
siap pakai dari HP.

### 13.2 Konversi koordinat di app HP (usulan)

Untuk setiap update:

1. Cocokkan posisi GPS rider ke garis rute (map matching / proyeksi ke
   polyline) → titik rider `P0` di rute dan jarak tempuhnya `s0`.
2. Ambil potongan rute dari `s0 − 12` sampai `s0 + 80` satuan (×15 m bila
   skala dummy dipakai), sederhanakan menjadi ≤ 32 titik sudut.
3. Ubah setiap titik ke meter lokal di sekitar `P0` (cukup aproksimasi
   equirectangular untuk jarak beberapa km):

   ```
   east  = (lon - lon0) * cos(lat0) * 111320      // meter
   north = (lat - lat0) * 110540                  // meter
   ```

4. Putar ke heading-up dengan heading rider `h` (derajat, searah jarum jam
   dari utara):

   ```
   x = east * cos(h) - north * sin(h)   // kanan rider
   y = east * sin(h) + north * cos(h)   // depan rider
   ```

   Contoh cek: menghadap timur (h = 90°), titik di utara (north > 0)
   menjadi x < 0 (kiri) ✓.
5. Bagi dengan skala meter per satuan (mis. 15).

`h` sebaiknya arah rute di posisi rider (tangen polyline, dihaluskan),
bukan course GPS mentah yang berisik di kecepatan rendah. Simulator memakai
chord ±3 satuan (±45 m) dan hasilnya halus.

### 13.3 Jalan samping dari data routing

- **OpenRouteService**: per step hanya `bearing_before`/`bearing_after`
  jalur rider sendiri; tidak ada cabang lain.
- **Mapbox Directions**: `steps[].intersections[]` berisi `location`,
  `bearings[]`, `entry[]`, `in`, `out` — semua jalan yang bertemu di
  persimpangan. Setiap bearing selain `in`/`out` bisa dijadikan jalan
  samping: titik awal = lokasi persimpangan, arah = bearing, panjang tetap
  8–14 satuan (geometri jalan aslinya tidak tersedia, jadi lurus).
- Jalan samping bengkok hanya mungkin bila geometri jalan diambil dari
  sumber peta (mis. OSM) — opsional.

### 13.4 Transport BLE

Yang sudah ada di `src/main.cpp`:

- Nama perangkat `GPS_Tracker_BLE`, Nordic UART Service
  `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`.
- RX (HP → ESP32): `6E400002-…`, write without response.
- TX (ESP32 → HP): `6E400003-…`, notify (belum dipakai).
- MTU diminta 517 (payload maks. 514 byte per write); laptop dengan BlueZ
  mendapat 517. iOS biasanya menegosiasikan MTU lebih kecil (±185, payload
  ±182 byte), dan BLE hanya menjamin 20 byte per write — keduanya ditangani
  fragmentasi di bawah.
- Stack: Bluedroid (library `BLEDevice` Arduino-ESP32 2.0.17).
- Yang diterima: frame navigasi (byte pertama `0xCE`, di bawah) dan pesan
  teks lama `GPS:lat,lon,speed` (mengisi lat/lon/speed untuk layar debug).
  `ROUTE:` hanya dicatat ke log. Frame biner tidak pernah dicetak ke serial.
- **Aturan pesan teks: setiap pesan teks wajib diawali tag ASCII** (huruf
  besar diikuti titik dua, seperti `GPS:` dan `ROUTE:`), supaya byte
  pertamanya tidak pernah `0xCE`. Penerima memilah pesan hanya dari byte
  pertama, dan `0xCE` juga lead byte UTF-8 yang sah (huruf Yunani
  U+0380–U+03BF, mis. `Ω` = `CE A9`), jadi teks bebas yang dimulai dengan
  karakter seperti itu akan masuk jalur frame navigasi (lalu ditolak sebagai
  header rusak atau potongan yang tak pernah lengkap) dan tidak pernah sampai
  ke pemroses teks.

**Format biner pesan `'N'`** — sudah diimplementasikan di
`src/nav_codec.h/.c`, satu pesan = satu jendela rute lengkap,
little-endian:

| Offset | Ukuran | Field |
| --- | --- | --- |
| 0 | 1 | Tipe pesan (`'N'` = 0x4E) |
| 1 | 1 | Nomor urut (wrap 0–255; untuk deteksi pesan hilang/berurutan) |
| 2 | 1 | `maneuver` (0–3) |
| 3 | 1 | Flags: bit 0 = rute valid, wajib 1 tepat saat N ≥ 2; bit 1–7 cadangan (dikirim 0, diabaikan penerima) |
| 4 | 2 | `distance_to_turn_m` (uint16, meter) |
| 6 | 2 | `total_distance_m` (uint16, ×10 m) |
| 8 | 1 | `speed_kmh` (uint8) |
| 9 | 1 | Jumlah titik rute N (0–32) |
| 10 | 4·N | Titik rute: `int16 x`, `int16 y` (1/16 satuan) |
| … | 1 | Jumlah jalan samping M (0–8) |
| … | per jalan: 1 + 4·K | K titik (2–4), lalu K × (`int16 x`, `int16 y`) |

- **Kuantisasi koordinat 1/16 satuan**: dibulatkan ke terdekat, jadi error
  maksimal **1/32 satuan ≈ 0,125 px** (4 px per satuan). Rentang `int16`
  = ±2048 satuan. Jarak dibulatkan ke 1 m (sisa rute ke 10 m), kecepatan
  ke 1 km/h.
- **Ukuran**: 10 + 4·N + 1 + Σ(1 + 4·K) byte. Minimal 11 (tanpa rute),
  maksimal **275** (32 titik + 8 jalan × 4 titik). Data simulator: rata-rata
  ±171–179 byte, terbesar 199 byte (diukur di board; host: rata-rata
  176,4, maksimal 195 dari 40.000 paket). Format teks `NAV:…` rencana awal
  (±750–1000 karakter, harus dipecah) ditinggalkan.
- **MTU**: diselesaikan lewat fragmentasi (di bawah): satu pesan dikirim
  dalam 1 frame di MTU 517, 2 frame di iOS (182 byte), sampai 18 frame di
  MTU minimum (20 byte).
- **Encoder** menjepit nilai di luar rentang (negatif → 0, terlalu besar →
  maksimum field, NaN → 0), melewati jalan samping < 2 titik, dan tidak
  pernah menulis paket yang ditolak decodernya sendiri.
- **Decoder** memvalidasi seluruh paket sebelum menulis apa pun: tipe
  `'N'`, panjang total harus pas (kurang = `TRUNCATED`, lebih =
  `LENGTH`), maneuver 0–3, N ≤ 32, M ≤ 8, K 2–4, bit rute valid cocok
  dengan N. Paket rusak ditolak dengan kode error dan `nav_data_t` tujuan
  tidak berubah. `bearing_deg` dan `ble_connected` tidak dikirim (diisi
  0/`false`). Tidak ada checksum: link layer BLE sudah punya CRC.
- Pesan `'N'` tidak dikirim mentah lewat BLE; ia selalu dibungkus frame.

**Fragmentasi (transport BLE)** — `src/nav_frag.h/.c`, `nav_encode`/
`nav_decode` tidak berubah. Setiap pesan dikirim dalam satu atau beberapa
frame, satu frame = satu write:

| Offset | Ukuran | Field |
| --- | --- | --- |
| 0 | 1 | Tipe frame `0xCE` (bit tertinggi menyala, jadi tidak pernah sama dengan awal pesan teks ASCII seperti `GPS:`; `0x80 \| 'N'`) |
| 1 | 1 | Nomor pesan (pengirim memakai nomor urut `'N'` yang sama) |
| 2 | 1 | Indeks potongan, 0 … jumlah − 1 |
| 3 | 1 | Jumlah potongan, 1–32 |
| 4 | 1+ | Potongan isi pesan berikutnya |

- Header 4 byte, jadi write 20 byte membawa 16 byte isi: pesan terbesar
  (275 byte) butuh 18 frame; batas 32 memberi ruang tanpa melonggarkan
  validasi.
- Penerima merakit ulang **berurutan** ke buffer tetap 275 byte. BLE
  mengirim write berurutan atau tidak sama sekali, jadi celah berarti
  potongan hilang: pesan itu dibuang, sisa frame-nya diabaikan, pesan
  berikutnya mulai bersih. Potongan pesan baru yang datang sebelum pesan lama
  lengkap juga membuang pesan lama. Tidak ada retransmit (pesan berikutnya
  menyusul ±200 ms kemudian).
- Potongan dobel diabaikan, dan pesan yang sudah lengkap tidak pernah
  dikirim ke decoder dua kali.
- Header yang tidak masuk akal dibuang dan dihitung: tipe salah, tanpa isi,
  jumlah 0 atau > 32, indeks ≥ jumlah, jumlah berubah di tengah pesan, atau
  pesan yang tumbuh melebihi 275 byte.
- Statistik di serial tiap 5 detik selama tersambung: pesan lengkap, pesan
  dibuang karena potongan hilang, frame rusak, potongan dobel, error decode,
  pesan hilang menurut nomor urut, dan heap internal. Contoh nyata (pengirim
  membuang 2% frame di write 20 byte): `complete 100, dropped 20 (missing
  chunk)`, dan `lost by seq` naik 20.

Diuji lewat radio dari laptop (BlueZ, bleak): write 514, 182, dan 20 byte
masing-masing ±75 pesan, semua lengkap, 0 hilang. `--drop 0.1` (pengirim
melewati 9 dari 125 pesan) → firmware mencatat tepat 9 hilang menurut nomor
urut.

Rincian kode error dan aturan encoder ada di `docs/navigation-protocol.md`
bagian 1.

### 13.5 Pekerjaan firmware untuk data asli

Fase 1 (selesai): codec biner `'N'` + unit test host + mode loopback, dan
`USE_DUMMY_DATA` diganti pilihan compile-time `NAV_SOURCE`
(`SIM` / `LOOPBACK` / `BLE`). **Diputuskan tidak ada pilihan runtime ke
dummy** (mis. saat belum ada koneksi BLE): rute palsu di layar saat
koneksi putus berbahaya di jalan.

Fase 2 (selesai):

1. Frame navigasi di callback BLE (`MyCharacteristicCallbacks::onWrite`):
   byte pertama `0xCE` → `nav_rx_on_frame()` → rakit ulang → `nav_decode()`;
   `GPS:` tetap untuk layar debug, `ROUTE:` tetap hanya log.
2. **Pengaman antar-task**: callback BLE (task Bluedroid) menulis hasil
   decode ke queue FreeRTOS panjang 1 (`xQueueOverwrite`); `loop()`
   mengambil salinan terbaru (`xQueuePeek`) lalu memanggil
   `ui_update_nav_display()` tiap loop. Task BLE tidak menyentuh LVGL.
3. Data basi: > 3 detik tanpa pesan valid (`NAV_RX_STALE_MS`) atau link
   putus → `ui_update_nav_signal(false)` (bagian 8). Saat link putus queue
   dikosongkan dan perakit ulang direset. `ble_connected` diisi. Paket
   hilang dihitung dari nomor urut dan dicatat ke serial.
4. Pengirim uji di laptop (`tools/ble_sender/`, Python + bleak), termasuk
   implementasi referensi konversi koordinat bagian 13.2 (`navgeo.py`,
   dengan test arah: menghadap timur, titik di utara → x < 0).

Berikutnya (di luar fase 2): app HP, pemisahan pesan geometri/pose, zoom
mengikuti kecepatan, IMU.

### 13.6 Heading

Layar navigasi tidak memakai `bearing_deg`; orientasi heading-up sudah ada
di koordinat lokal yang dikirim HP.

**Kompas belum terpasang** dan belum akan dipasang dalam waktu dekat. Kodenya
opsional lewat `ENABLE_COMPASS` (`src/hw_config.h`, bawaan 0):

- `0`: tidak ada I2C sama sekali (`Wire` tidak diinisialisasi).
- `1` (hanya berlaku di `NAV_SOURCE_BLE`): sensor di-probe sekali saat boot.
  Kalau tidak menjawab, ditandai tidak ada dan tidak pernah di-poll; log
  hanya satu baris (`⚠ QMC5883L not responding: compass marked absent, not
  polled`) ditambah satu error I2C dari core Arduino saat probe. Di
  SIM/LOOPBACK flag ini tidak berpengaruh (heading dari simulator).

Fitur yang bergantung pada heading kompas: **hanya layar debug lama**
(`UI_START_DEBUG_SCREEN 1`): label heading `H: …°` dan nilai mentah X/Y/Z
(lewat `ui_update_gps()` di `NAV_SOURCE_BLE`). Tanpa kompas keduanya tetap
0. Jarum heading di layar debug diberi `bearing_deg` simulator di
SIM/LOOPBACK, dan di BLE memang tidak pernah diberi data. **Belum ada sumber
heading lain dari HP**: pesan `'N'` tidak membawa heading (sudah tersirat di
geometri heading-up), dan `GPS:` hanya lat/lon/kecepatan. Sesuai keputusan,
tidak dibuat logika baru; kalau layar debug butuh heading tanpa kompas,
perlu diputuskan dulu (mis. field heading di pesan, atau dihitung dari dua
posisi `GPS:`).

Kalau nanti dipasang: pembacaannya masih `atan2(y, x)` mentah tanpa
kalibrasi hard/soft-iron dan tanpa kompensasi kemiringan (belum ada IMU),
jadi belum layak untuk orientasi peta. Ia baru berguna untuk memutar peta di
antara dua update HP saat motor diam atau kecepatan rendah (course GPS
tidak andal di bawah ±5 km/h).

### 13.7 Frekuensi update

UI menghaluskan dengan konstanta waktu ±100 ms. Pada 1 Hz gerak tetap halus
tapi tertinggal ±1 detik dari posisi sebenarnya; 5–10 Hz memberi tampilan
paling responsif. Ukuran pesan biner (±280 byte × 10 Hz ≈ 2,8 KB/s) masih
jauh di bawah kapasitas BLE.

### 13.8 Keputusan yang masih terbuka

- Platform app HP (Android / iOS / Flutter). Batas MTU **tidak lagi**
  menentukan: fragmentasi jalan di MTU berapa pun (bagian 13.4).
- Pemisahan pesan geometri (rute, jarang) dan pose (posisi/heading, sering)
  tetap jadi **opsi optimasi untuk nanti**: mengurangi byte per update
  (sekarang ±170–200 byte × 5 Hz), tapi butuh state di ESP32 untuk
  menggabungkannya. Belum perlu selama satu pesan penuh cukup.
- Penyedia routing: ORS (tanpa cabang persimpangan) atau Mapbox (dengan
  `intersections`).
- Skala meter per satuan: tetap (mis. 15 m) atau zoom mengikuti kecepatan.
- Sumber heading untuk konversi: arah rute, course GPS, atau kompas HP.
- Jenis maneuver tambahan: belok sedikit, bundaran, tiba di tujuan, keluar
  jalan tol — perlu ikon dan nilai enum baru.
- Sinyal GPS hilang di sisi HP (link BLE masih tersambung): HP sebaiknya
  berhenti mengirim (ESP32 masuk "tidak ada sinyal" setelah 3 detik) atau
  diberi flag tersendiri. Tampilan untuk link putus / data basi sudah ada.
- **Diputuskan (8 Okt 2026): periode refresh 30 ms** (±33 fps). Alasannya:
  - Buffer ganda penuh tidak diambil karena RAM tidak cukup: butuh ±115 KB
    lagi, sedangkan heap internal minimum yang terukur di fase 2 hanya
    ±35 KB.
  - Render saja sudah ±23,7 ms di frame terberat, jadi target 25 ms tetap
    mepet.

  Hasil pengukuran: tidak ada frame yang melewati 30 ms, frame terberat
  ≤ 25,3 ms (bagian 9). Eksperimen dua buffer parsial (¼ layar) diukur dan
  ditolak: RAM bebas +57 KB, tapi frame rata-rata ±20 ms dan terberat
  ±30–32 ms karena callback peta jalan sekali per potongan. Yang masih bisa
  dicoba nanti kalau perlu fps lebih tinggi: memangkas pass ganda di frame
  pergantian manuver (render saja terberat ±23,7 ms terjadi di sana).
- Informasi tambahan di layar (nama jalan, ETA, sisa jarak total) — perlu
  ruang di panel dan biaya render (bagian 9).
- Nasib pesan lama `GPS:` dan `ROUTE:`.

## 14. Batasan dan masalah yang diketahui

- Ada satu jeda ±55 ms sekali, ±9 detik setelah boot (kemungkinan pemuatan
  pertama sesuatu, mis. glyph font); setelah itu tidak berulang.
- RAM 78% terpakai (buffer layar penuh 115 KB).
- Frame terberat ±24–25 ms dengan refresh 30 ms (bagian 9). Jeda antar-frame
  sesekali masih 40–55 ms (juga di LOOPBACK, tanpa BLE dan tanpa kompas);
  penyebabnya belum diselidiki.
- Kompas belum terpasang; layar debug lama menampilkan heading dan X/Y/Z 0
  (bagian 13.6).
- Pesan serial dari task BLE dan `loop()` kadang tercampur/terpotong
  (mis. `✓ BLE Client c`), karena keduanya mencetak ke USB-CDC bersamaan.
- Sisa peringatan kompilasi: `lv_obj_remove_flag` di
  `screens/ui_nav_display.c` sudah deprecated di LVGL 9.6 (diganti setter
  `lv_obj_set_<flag>()`), `LV_COLOR_DEPTH` di `lv_conf.h` (diganti
  `LV_COLOR_FORMAT_DEFAULT`), dan opsi `src_filter` di `platformio.ini`.
  Semuanya tidak memengaruhi hasil build.

## 15. Aturan kerja repo (ringkas)

- Commit dilakukan pemilik project sendiri, banyak commit kecil (satu
  perubahan per commit), pesan commit bahasa Inggris gaya conventional
  commit (`feat(ui): …`, `fix(display): …`, `docs: …`).
- Perubahan dari luar folder utama diserahkan sebagai satu patch relatif ke
  `main`, diterapkan dengan `git apply`, lalu dibagi ke commit.
- Sebelum menyarankan commit, build dan test harus lolos (`pio run`,
  `pio test -e native`, dan `pytest` di `tools/ble_sender`; cek kode
  keluarnya).
