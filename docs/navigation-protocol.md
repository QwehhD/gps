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
NAV:bearing,jarak_ke_belok,tipe_belok,jarak_total,speed,titik_garis[]
```

Rincian field:

| Field           | Tipe          | Satuan   | Keterangan                                                                 |
| --------------- | ------------- | -------- | --------------------------------------------------------------------------- |
| `bearing`       | float         | derajat  | Heading kompas saat ini, 0-359.9, 0 = utara                                 |
| `jarak_ke_belok`| float         | meter    | Jarak tersisa ke maneuver berikutnya, hitung mundur ke 0                    |
| `tipe_belok`    | enum/int      | -        | Jenis maneuver berikutnya: lurus / belok kiri / belok kanan / putar balik   |
| `jarak_total`   | float         | meter    | Sisa jarak untuk keseluruhan rute (bukan cuma sampai belokan berikutnya)    |
| `speed`         | float         | km/h     | Kecepatan saat ini                                                          |
| `titik_garis[]` | array of x,y  | meter    | Titik-titik garis skematik jalan (lihat bagian 2)                          |

Payload ini adalah **rencana desain**, bukan implementasi aktif — koneksi
BLE yang sudah berjalan di `src/main.cpp` saat ini masih memakai format
lama (`GPS:lat,lon,speed` dan `ROUTE:...`) untuk keperluan lain, dan belum
diganti ke format `NAV:` di atas. Field-field ini sengaja dibuat 1:1
dengan struct `nav_data_t` di `src/nav_sim.h` (lihat bagian 5) supaya
nanti tinggal parsing string BLE ke struct yang sama, tanpa mengubah kode
UI sama sekali.

## 2. Garis Skematik (Schematic Line)

Garis skematik **bukan peta akurat** — ini cuma ilustrasi bentuk jalan
untuk maneuver/step yang sedang berjalan saat ini, mirip yang dipakai
Beeline Moto 2. Karakteristiknya:

- **Orientasi heading-up**: "atas" pada gambar selalu berarti "ke arah
  hadap rider saat ini", bukan utara. Rider secara visual selalu berada
  di posisi tetap (biasanya bawah-tengah), dan garis digambar relatif
  terhadap posisi itu.
- **Koordinat lokal/relatif, bukan lat/lon**: titik-titik `titik_garis[]`
  dinyatakan dalam meter relatif terhadap posisi rider saat ini (x =
  lateral/kiri-kanan, y = jarak ke depan), bukan koordinat GPS absolut.
- **Diproses di app HP sebelum dikirim**: app HP yang melakukan routing
  (OpenRouteService dkk) bertanggung jawab mengubah geometri rute
  (lat/lon) menjadi titik-titik lokal heading-up ini sebelum
  dikirim lewat BLE. ESP32 tidak melakukan konversi lat/lon → lokal.
- **Hanya mencakup step yang sedang berjalan**: garis ini menggambarkan
  bentuk jalan untuk 1 maneuver ke depan saja, bukan keseluruhan rute.

## 3. Catatan: Data Cabang Jalan Lain di Persimpangan ("Junction View")

Beberapa aplikasi navigasi menampilkan bukan cuma jalur yang akan dilalui,
tapi juga cabang-cabang jalan lain di persimpangan (junction view) supaya
rider bisa mengenali persimpangan yang benar sebelum sampai di sana. Ini
**belum diputuskan** apakah akan diimplementasikan, tapi dicatat sebagai
opsi masa depan:

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
- **Speed, jarak-ke-belok, tipe belokan, dan titik garis skematik** juga
  seluruhnya digenerate software (lihat `src/nav_sim.cpp` untuk logikanya)
  — tidak ada input dari GPS, IMU, atau BLE app HP untuk field-field ini.
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
    nav_point_t schematic_points[8];       // titik garis skematik, lokal, heading-up
    uint8_t schematic_point_count;         // jumlah titik yang valid di atas
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

### Hal yang belum diimplementasikan (di luar cakupan dokumen ini)

- **IMU (MPU6050)**: belum ada di hardware, belum ada kode pembacaannya.
  Field yang nanti butuh data IMU (mis. tilt-compensated heading) belum
  ditentukan strukturnya.
- **Modul BLE app HP** (pengirim payload `NAV:` di atas): belum
  diimplementasikan. BLE server yang aktif sekarang di `src/main.cpp`
  masih untuk protokol lama (`GPS:`, `ROUTE:`), bukan untuk payload nav
  yang didokumentasikan di bagian 1.
