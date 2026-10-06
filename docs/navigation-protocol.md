# Protokol Navigasi (Rencana BLE + Mode Dummy Data)

Dokumen ini mencatat desain protokol navigasi untuk dashboard motor —
supaya keputusan desain (format payload, orientasi garis skematik, opsi
junction view) tidak hilang begitu implementasi sensor/BLE beneran mulai
dikerjakan. Sebagian besar yang didokumentasikan di sini **belum
diimplementasikan**; yang jalan sekarang hanya mode simulasi software
(lihat bagian "Mode Dummy Data Saat Ini").

## 1. Format Payload BLE (Rencana, Belum Diimplementasikan)

App HP (belum ada) akan melakukan routing (via OpenRouteService atau
sejenisnya), lalu mengirim hasilnya ke ESP32-S3 lewat BLE dalam bentuk
string terstruktur:

```
NAV:bearing,jarak_ke_belok,tipe_belok,jarak_total,speed,titik_garis[],jalan_samping[]
```

Rincian field:

| Field           | Tipe          | Satuan   | Keterangan                                                                 |
| --------------- | ------------- | -------- | --------------------------------------------------------------------------- |
| `bearing`       | float         | derajat  | Heading kompas saat ini, 0-359.9, 0 = utara                                 |
| `jarak_ke_belok`| float         | meter    | Jarak tersisa ke maneuver berikutnya, hitung mundur ke 0                    |
| `tipe_belok`    | enum/int      | -        | Jenis maneuver berikutnya: lurus / belok kiri / belok kanan / putar balik   |
| `jarak_total`   | float         | meter    | Sisa jarak untuk keseluruhan rute (bukan cuma sampai belokan berikutnya)    |
| `speed`         | float         | km/h     | Kecepatan saat ini                                                          |
| `titik_garis[]` | array of x,y  | satuan   | Titik-titik garis rute di sekitar rider, maks. 32 (lihat bagian 2)          |
| `jalan_samping[]` | array of segmen x,y→x,y | satuan | Cabang jalan di sekitar rute, maks. 8 (lihat bagian 3)       |

Payload ini adalah **rencana desain**, bukan implementasi aktif — koneksi
BLE yang sudah berjalan di `src/main.cpp` saat ini masih memakai format
lama (`GPS:lat,lon,speed` dan `ROUTE:...`) untuk keperluan lain, dan belum
diganti ke format `NAV:` di atas. Field-field ini sengaja dibuat 1:1
dengan struct `nav_data_t` di `src/nav_sim.h` (lihat bagian 5) supaya
nanti tinggal parsing string BLE ke struct yang sama, tanpa mengubah kode
UI sama sekali.

## 2. Garis Skematik (Schematic Line)

Garis skematik **bukan peta akurat** — ini cuma ilustrasi bentuk rute di
sekitar rider, mirip yang dipakai Beeline Moto 2. Karakteristiknya:

- **Orientasi heading-up**: "atas" pada gambar selalu berarti "ke arah
  hadap rider saat ini", bukan utara. Rider secara visual selalu berada
  di posisi tetap (biasanya bawah-tengah), dan garis digambar relatif
  terhadap posisi itu.
- **Koordinat lokal/relatif, bukan lat/lon**: titik-titik `titik_garis[]`
  dinyatakan relatif terhadap posisi rider saat ini (x = lateral/kiri-kanan,
  y = jarak ke depan), dengan rider di (0,0) dan berada di garis rute. Satuannya
  "satuan skematik", bukan meter: layar menggambar 4 px per satuan, dan
  data dummy memakai ~15 m jalan per satuan supaya beberapa belokan muat di
  layar. Skala ini bebas dipilih app HP selama konsisten.
- **Diproses di app HP sebelum dikirim**: app HP yang melakukan routing
  (OpenRouteService dkk) bertanggung jawab mengubah geometri rute
  (lat/lon) menjadi titik-titik lokal heading-up ini sebelum
  dikirim lewat BLE. ESP32 tidak melakukan konversi lat/lon → lokal.
- **Mencakup jendela di sekitar rider, bukan cuma 1 maneuver**: garis ini
  memuat rute dari sedikit di belakang rider (~12 satuan) sampai jauh ke
  depan (~80 satuan, melewati tepi layar), jadi belokan kedua dan ketiga
  sudah terlihat sebelum tiba, dan melewati persimpangan tidak mengganti
  gambar. App HP cukup mengirim ulang jendela ini setiap kali posisi
  berubah.

## 3. Catatan: Data Cabang Jalan Lain di Persimpangan ("Junction View")

Beberapa aplikasi navigasi menampilkan bukan cuma jalur yang akan dilalui,
tapi juga cabang-cabang jalan lain di persimpangan (junction view) supaya
rider bisa mengenali persimpangan yang benar sebelum sampai di sana. Layar
sudah mendukungnya lewat `side_roads` (`jalan_samping[]`): tiap cabang
berupa satu segmen lurus dalam koordinat yang sama dengan garis rute,
dengan `from` di rute dan `to` di ujung jauh (digambar memudar ke arah
`to`). Saat ini hanya data dummy yang mengisinya; untuk data asli, sumber
datanya masih perlu dipilih:

- **OpenRouteService** (routing API yang direncanakan dipakai): per step,
  cuma menyediakan `bearing_before` dan `bearing_after` — yaitu bearing
  masuk dan keluar untuk jalur yang **dilalui rider sendiri**. Tidak ada
  informasi bearing cabang-cabang lain di persimpangan tersebut.
- **Mapbox Directions API**: sebagai alternatif/pelengkap, punya field
  `intersections` per step yang berisi bearing **semua** jalan yang
  bertemu di satu persimpangan (bukan cuma jalur yang dilalui). Kalau
  nanti mau fitur junction view yang lebih akurat (menampilkan semua
  cabang jalan, bukan cuma satu panah), field ini yang perlu dipakai,
  kemungkinan besar berdampingan dengan atau menggantikan ORS.

## 4. Mode Dummy Data Saat Ini: 100% Simulasi Software

Firmware saat ini berjalan dalam **mode dummy data penuh**
(`USE_DUMMY_DATA 1` di `src/nav_sim.h`), yang berarti:

- **Tidak ada pembacaan QMC5883L sama sekali.** Kode inisialisasi dan
  polling QMC5883L (soft reset `0x80`→`0x0A`, set/reset period
  `0x01`→`0x0B`, continuous mode `0x1D`→`0x09`) masih ada di
  `src/main.cpp`, tapi seluruhnya dibungkus `#if !USE_DUMMY_DATA` — jadi
  tidak pernah dipanggil selama flag dummy aktif. Tidak ada transaksi I2C
  apa pun ke alamat sensor ini dalam mode dummy.
- **Bearing/heading 100% simulasi**, berupa sapuan halus 0°→360° berulang
  (~13 detik per putaran), murni fungsi waktu — bukan pembacaan kompas
  fisik dalam bentuk apa pun, bukan cuma bearing tujuan.
- **Speed, jarak-ke-belok, tipe belokan, titik garis skematik, dan jalan
  samping** juga seluruhnya digenerate software (lihat `src/nav_sim.cpp`
  untuk logikanya) — tidak ada input dari GPS, IMU, atau BLE app HP untuk
  field-field ini. Simulator membuat rute acak tanpa ujung (ruas lurus
  12–26 satuan, lalu belok kiri/kanan 55–100°, lurus di perempatan, atau
  sesekali putar balik, plus cabang jalan di persimpangan dan jalan kecil
  yang memotong ruas), lalu "mengendarai" rider di sepanjangnya dengan
  arah hadap mengikuti jalan.
- Tujuannya murni **validasi UI & rendering LVGL** di layar fisik,
  terisolasi total dari status/ketersediaan hardware sensor.

Untuk beralih ke data sensor/BLE asli nanti, cukup ubah
`#define USE_DUMMY_DATA` di `src/nav_sim.h` menjadi `0`, lalu sambungkan
sumber data asli (baca `nav_data_t` dari hasil parsing payload `NAV:` di
atas, atau dari pembacaan sensor langsung) — kode UI di
`screens/ui_gps_tracker.c` tidak perlu diubah karena sudah membaca dari
struct `nav_data_t` yang sama, bukan langsung dari nav_sim.

## 5. Struct/Field Mock Data (`nav_data_t`)

Didefinisikan di `src/nav_sim.h`. Ini adalah kontrak data antara sumber
data (simulator sekarang, sensor+BLE nanti) dan kode UI — mengganti
sumber data cukup dengan mengisi struct ini dengan cara berbeda, tanpa
menyentuh kode UI:

```c
typedef struct {
    float bearing_deg;                    // 0-359.9, heading kompas
    float distance_to_turn_m;             // hitung mundur ke maneuver berikutnya
    nav_maneuver_t maneuver;               // STRAIGHT / TURN_LEFT / TURN_RIGHT / U_TURN
    float total_distance_m;                // sisa jarak keseluruhan rute
    float speed_kmh;                       // kecepatan saat ini
    nav_point_t schematic_points[32];      // garis rute, lokal, heading-up, rider di (0,0)
    uint8_t schematic_point_count;         // jumlah titik yang valid di atas
    nav_side_road_t side_roads[8];         // cabang jalan: segmen from (di rute) -> to
    uint8_t side_road_count;               // jumlah cabang yang valid di atas
    bool ble_connected;                    // placeholder, selalu false di mode dummy
} nav_data_t;
```

Sumber data saat ini (`src/nav_sim.c/.cpp`) mengisi struct ini dengan nilai
simulasi. Modul ini sengaja dipisah total dari kode UI (`ui_init.cpp`,
`screens/ui_gps_tracker.c`) supaya penggantinya nanti — baik itu hasil
parsing BLE `NAV:` payload, atau pembacaan sensor langsung — tinggal
mengisi `nav_data_t` yang sama lewat fungsi `nav_sim_get_data()`-nya
sendiri (atau fungsi pengganti dengan nama lain), tanpa mengubah satu pun
baris kode di layer UI.

## 6. Layar Navigasi (`screens/ui_nav_display.c`)

Layar default sekarang adalah tampilan turn-by-turn untuk layar bulat
240x240, seluruhnya digambar lewat draw callback LVGL (bukan gambar/bitmap)
dan hanya membaca `nav_data_t`:

- **Garis rute** (`schematic_points`): putih tebal, heading-up, rider selalu
  di panah tengah-bawah. Rider berada di titik asal (0,0) dan harus terletak
  di garis rute. Skala 4 px per satuan; titik terakhir sebaiknya jauh di
  luar layar supaya garis tidak terlihat berhenti di tengah.
- **Jalan samping** (`side_roads`): digambar sebagai sepasang garis tepi
  tipis abu-abu kebiruan (2 px, berjarak 7 px, kira-kira selebar rute),
  memudar dari rute ke ujung jauhnya. Muncul dan hilang dengan fade ~200 ms
  saat masuk/keluar jendela data.
- **Panah rider 3D**: sisi kiri terang dan sisi kanan teduh (cahaya dari
  kiri), sisi bawah diberi ketebalan yang lebih gelap, plus bayangan lembut.
- **Panel bawah berbentuk kubah** (lingkaran besar berpusat di bawah layar):
  berisi ikon maneuver (lurus / kiri / kanan / putar balik) dan
  `distance_to_turn_m` (Montserrat 36 Medium, satuan Montserrat Bold 20).
  Jarak dibulatkan per 10 m (>= 100 m) atau 5 m (< 100 m), dan ditampilkan
  dalam km dengan satu desimal mulai ~1 km.
- **Busur progres** di tepi bawah: terisi dari kiri ke kanan dalam 500 m
  terakhir sebelum belokan (`PROGRESS_RANGE_M`), kosong kalau masih jauh.

Gerakan dibuat halus di sisi UI, jadi data boleh datang patah-patah
(mis. BLE 1x per detik):

- `ui_nav_display_set()` hanya menyimpan target. Timer LVGL 20 ms
  menggerakkan garis rute, jalan samping, dan busur progres ke target
  dengan easing. `main.cpp` memanggilnya tiap `loop()` lewat
  `ui_update_nav_display()`.
- Garis rute diubah menjadi 73 titik berjarak sama (1,25 satuan = 5 px),
  dihitung dari titik rute yang paling dekat dengan rider: 8 di belakang
  panah, sisanya di depan. Titik ke-i selalu berarti "posisi yang sama
  relatif terhadap rider", jadi perubahan data menjadi gerakan mulus, bukan
  gambar ulang. Jaraknya cukup rapat sehingga sudut tetap bulat dan tidak
  bergoyang saat titik-titik bergeser sepanjang jalan.
- Jalan samping dipasangkan dengan yang tampil sebelumnya (titik awal
  berdekatan dan arahnya mirip), jadi ikut bergeser mulus; yang tidak punya
  pasangan muncul dengan fade-in, yang hilang dari data memudar keluar.
- Render LVGL memakai mode parsial dan jam dari `millis()`: hanya area yang
  posisi pikselnya benar-benar berubah yang digambar ulang dan dikirim ke
  layar.

### Performa di ESP32-S3 (diukur di board)

Frame yang bergerak butuh ±16–19 ms. Angka ini bisa dilihat sendiri dengan
mengubah `LV_PORT_PERF_LOG` di `src/lv_port_disp.cpp` menjadi `1`: FPS,
jeda terlama antar-frame, dan waktu render dicetak ke serial tiap 2 detik.
Yang membuatnya cukup cepat untuk 40 fps stabil:

- **DMA lewat `esp_lcd` (ESP-IDF) dengan dua buffer 240x68**: LVGL
  menggambar potongan berikutnya sementara potongan sebelumnya dikirim
  lewat SPI. TFT_eSPI hanya dipakai di `setup()` untuk inisialisasi panel
  dan teks pembuka; setelah itu perintah alamat dan piksel sama-sama lewat
  `esp_lcd`. DMA milik TFT_eSPI sendiri sempat dicoba dan tidak bisa
  dipakai di board ini: versi 2.5.43 crash di callback akhir transfernya
  di ESP32-S3, dan setelah crash itu diakali, panel mengabaikan semua frame
  karena tulisan register langsung TFT_eSPI bercampur dengan transfer DMA
  driver IDF (layar tertahan di teks pembuka).
- **Refresh tiap 25 ms**, sedikit di atas waktu render terburuk. Periode yang
  lebih pendek dari waktu render membuat jarak antar-frame berselang-seling
  satu/dua periode dan terasa patah-patah. `loop()` hanya tidur 1 ms supaya
  timer LVGL tidak bergeser.
- **Garis rute disederhanakan** (Douglas–Peucker, 0,75 px) sebelum digambar,
  dan ujung bulat hanya dipakai di tempat yang terlihat: LVGL menggambar
  setiap ujung bulat sebagai lingkaran tersendiri yang lebih mahal dari
  garisnya.
- **`-O2`** menggantikan `-Os` bawaan framework (di `platformio.ini`).
- Karena data dummy berupa rute menerus, belokan berikutnya sudah terlihat
  sebelum tiba, dan saat rider melewati belokan tampilan ikut berputar di
  sekitar panah (heading-up) tanpa ada garis yang diganti.

Layar debug lama (lat/lon, kompas X/Y/Z, jarum heading) masih ada: ubah
`UI_START_DEBUG_SCREEN` di `src/ui_init.cpp` menjadi `1` untuk memakainya.

### Hal yang belum diimplementasikan (di luar cakupan dokumen ini)

- **IMU (MPU6050)**: belum ada di hardware, belum ada kode pembacaannya.
  Field yang nanti butuh data IMU (mis. tilt-compensated heading) belum
  ditentukan strukturnya.
- **Modul BLE app HP** (pengirim payload `NAV:` di atas): belum
  diimplementasikan. BLE server yang aktif sekarang di `src/main.cpp`
  masih untuk protokol lama (`GPS:`, `ROUTE:`), bukan untuk payload nav
  yang didokumentasikan di bagian 1.
