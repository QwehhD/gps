# Protokol Navigasi (BLE + Mode Sumber Data)

Dokumen ini mencatat desain protokol navigasi untuk dashboard motor —
supaya keputusan desain (format payload, orientasi garis skematik, opsi
junction view) tidak hilang begitu implementasi sensor/BLE beneran mulai
dikerjakan. Yang sudah jalan: mode simulasi software, **format pesan biner
`'N'`**, **fragmentasi BLE**, penerima di ESP32 dengan state "tidak ada
sinyal", dan pengirim uji di laptop (`tools/ble_sender/`) yang juga memuat
implementasi referensi konversi lat/lon untuk app HP. Yang belum: app HP
itu sendiri (lihat bagian "Mode Sumber Data").

## 1. Format Payload BLE: pesan biner `'N'`

App HP (belum ada) melakukan routing (via OpenRouteService atau
sejenisnya), mengubah geometri rute ke koordinat lokal heading-up, lalu
mengirim hasilnya ke ESP32-S3 lewat BLE. **Satu pesan = satu jendela rute
lengkap**, cukup untuk mengisi seluruh `nav_data_t` yang dibaca layar.
Diimplementasikan di `src/nav_codec.h/.c` (C murni, tanpa Arduino/LVGL).

Rencana awal berupa teks (`NAV:bearing,jarak,tipe,total,speed,titik[],jalan[]`)
ditinggalkan: dengan 32 titik rute dan 8 jalan samping panjangnya
±750–1000 karakter dan harus dipecah ke beberapa write, sedangkan versi
biner maksimal 275 byte dan muat satu write di MTU 517.

Little-endian:

| Offset | Ukuran | Field |
| --- | --- | --- |
| 0 | 1 | Tipe pesan, `'N'` (0x4E) |
| 1 | 1 | Nomor urut, wrap 255 → 0 (deteksi pesan hilang/tertukar) |
| 2 | 1 | `maneuver` (`nav_maneuver_t`, 0–3) |
| 3 | 1 | Flags: bit 0 = rute valid (wajib 1 tepat saat N ≥ 2), bit 1–7 cadangan (dikirim 0, diabaikan penerima) |
| 4 | 2 | `distance_to_turn_m`, uint16, meter |
| 6 | 2 | `total_distance_m`, uint16, ×10 m |
| 8 | 1 | `speed_kmh`, uint8 |
| 9 | 1 | Jumlah titik rute N (0–32) |
| 10 | 4·N | Titik rute: `int16 x`, `int16 y`, dalam 1/16 satuan |
| … | 1 | Jumlah jalan samping M (0–8) |
| … | per jalan: 1 + 4·K | Jumlah titik K (2–4), lalu K × (`int16 x`, `int16 y`) |

Panjang paket = 10 + 4·N + 1 + Σ(1 + 4·K): minimal 11 byte (tanpa rute),
maksimal 275 byte (32 titik + 8 jalan × 4 titik). Data simulator rata-rata
±171–179 byte, terbesar 199 byte (diukur di board dan di host).

**Kuantisasi.** Koordinat dibulatkan ke 1/16 satuan terdekat, jadi error
maksimal 1/32 satuan = **0,125 px** di layar (4 px per satuan). Rentang
`int16` mencakup ±2048 satuan, jauh di atas jendela rute (−12…+80).
Jarak dibulatkan ke 1 m (sisa rute ke 10 m), kecepatan ke 1 km/h.

**Encoder** (`nav_encode`) tidak pernah menulis paket yang ditolak
decodernya sendiri: nilai di luar rentang dijepit (jarak/kecepatan negatif
→ 0, terlalu besar → maksimum field, koordinat → rentang `int16`, NaN →
0), jumlah titik dijepit ke maksimum, dan jalan samping dengan < 2 titik
dilewati (layar juga mengabaikannya). Mengembalikan 0 kalau buffer kurang
atau `maneuver` bukan nilai enum.

**Decoder** (`nav_decode`) memvalidasi seluruh paket sebelum menulis
apa pun, jadi paket rusak tidak pernah mengubah `nav_data_t` tujuan dan
tidak pernah membaca di luar buffer. Paket ditolak dengan kode error bila:

| Kode | Sebab |
| --- | --- |
| `NAV_DECODE_ERR_TYPE` | Byte pertama bukan `'N'` |
| `NAV_DECODE_ERR_TRUNCATED` | Paket berakhir sebelum semua field yang disebut jumlahnya |
| `NAV_DECODE_ERR_LENGTH` | Ada byte sisa setelah field terakhir (panjang harus pas) |
| `NAV_DECODE_ERR_MANEUVER` | `maneuver` ≥ 4 |
| `NAV_DECODE_ERR_FLAGS` | Bit rute valid tidak cocok dengan N ≥ 2 |
| `NAV_DECODE_ERR_ROUTE_COUNT` | N > 32 |
| `NAV_DECODE_ERR_SIDE_COUNT` | M > 8 |
| `NAV_DECODE_ERR_SIDE_POINTS` | K di luar 2–4 |
| `NAV_DECODE_ERR_ARG` | Pointer `NULL` |

`nav_decode_status_str()` memberi nama singkat untuk log. Tidak ada
checksum: link layer BLE sudah punya CRC. `bearing_deg` dan
`ble_connected` tidak ikut dikirim; decoder mengisinya 0/`false` untuk
diisi pemanggil.

### Transport BLE: frame

Pesan `'N'` tidak dikirim mentah. Pesan terbesar (275 byte) tidak muat satu
write di iOS (±182 byte) atau di MTU minimum BLE (20 byte), jadi setiap
pesan dipecah menjadi satu atau beberapa **frame**, satu frame = satu write
ke RX `6E400002-…` (write without response). `nav_encode`/`nav_decode` tidak
tahu soal frame; pemecah dan perakitnya ada di `src/nav_frag.h/.c`:

| Offset | Ukuran | Field |
| --- | --- | --- |
| 0 | 1 | Tipe frame `0xCE` |
| 1 | 1 | Nomor pesan (pengirim memakai nomor urut `'N'` yang sama) |
| 2 | 1 | Indeks potongan, 0 … jumlah − 1 |
| 3 | 1 | Jumlah potongan, 1–32 |
| 4 | 1+ | Potongan isi pesan berikutnya |

Isi pesan = sambungan potongan sesuai urutan indeks. `0xCE` (= `0x80 | 'N'`)
punya bit tertinggi menyala, jadi tidak pernah menjadi byte pertama pesan
teks ASCII; `onWrite` membedakan keduanya dari byte pertama. Header 4 byte:
write 20 byte membawa 16 byte isi (pesan terbesar = 18 frame), write 182
byte → 2 frame, MTU 517 → 1 frame.

**Aturan pesan teks: setiap pesan teks wajib diawali tag ASCII** (huruf
besar diikuti titik dua, seperti `GPS:` dan `ROUTE:`), supaya byte
pertamanya tidak pernah `0xCE`. `0xCE` juga lead byte UTF-8 yang sah (huruf
Yunani U+0380–U+03BF, mis. `Ω` = `CE A9`): teks bebas yang dimulai dengan
karakter seperti itu akan masuk jalur frame navigasi (lalu ditolak sebagai
header rusak atau potongan yang tak pernah lengkap) dan tidak pernah sampai
ke pemroses teks.

Perakitan di penerima (`nav_frag_push`), buffer tetap 275 byte, berurutan:

- Write BLE sampai berurutan atau tidak sama sekali, jadi indeks yang
  melompat berarti potongan hilang: pesan dibuang, sisa frame-nya
  diabaikan. Potongan pertama pesan baru sebelum pesan lama lengkap juga
  membuang pesan lama. **Tidak ada retransmit.**
- Potongan dobel diabaikan; pesan yang sudah lengkap tidak pernah
  dikeluarkan dua kali.
- Header yang tidak masuk akal → frame dibuang (`NAV_FRAG_BAD`): tipe
  salah, tanpa isi, jumlah 0 atau > 32, indeks ≥ jumlah, jumlah berubah di
  tengah pesan, pesan yang tumbuh melebihi 275 byte.
- `nav_frag_reset()` saat link tersambung/putus; statistik (lengkap,
  dibuang, frame rusak, dobel) tetap.

Diuji di host (`test/test_nav_frag`, 20 test termasuk fuzz 300.000 frame
acak, juga di bawah ASan/UBSan) dan lewat radio di write 514, 182, dan 20
byte. Vektor byte yang sama dicek oleh unit test C dan pytest pengirim.

### Penerimaan di ESP32 (`NAV_SOURCE_BLE`)

`src/nav_rx.cpp`, stack BLE **Bluedroid** (library `BLEDevice` Arduino-ESP32
2.0.17):

```
task BLE:  onWrite ─► byte 0 = 0xCE ─► nav_frag_push ─► nav_decode ─► xQueueOverwrite (queue panjang 1)
loop():    xQueuePeek ─► umur ≤ 3 s && tersambung ─► ui_update_nav_signal() + ui_update_nav_display()
```

- Task BLE tidak menyentuh LVGL. Struct besar di jalur terima `static`
  karena stack task Bluedroid (BTC) hanya 3 KB.
- Paket hilang dihitung dari selisih nomor urut `'N'` (mencakup pesan yang
  dibuang perakit karena potongan hilang dan pesan yang tidak pernah tiba).
- Saat link putus, queue dikosongkan dan perakit direset; layar langsung
  masuk "tidak ada sinyal". Tanpa pesan valid > 3 detik juga begitu
  (`NAV_RX_STALE_MS`). **Tidak ada fallback ke simulator.**
- Serial tiap 5 detik selama tersambung, contoh:
  `ble: connected | msgs 165 (4.2/s) | frames: complete 165, dropped 17 (missing chunk), bad 0, dup 0 | decode errors 0 | lost by seq 21 | internal heap free 42972 B (min 35272, largest 32756)`.
- Heap internal bebas saat runtime: ±46,7 KB sebelum tersambung, ±42,8 KB
  saat menerima, minimum tercatat ±35 KB.

Pesan teks lama tetap diterima: `GPS:lat,lon,speed` mengisi label layar
debug, `ROUTE:` hanya dicatat ke log. Di `NAV_SOURCE_SIM`/`LOOPBACK`, frame
`0xCE` diabaikan.

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
berupa polyline pendek (maks. 4 titik, jadi bisa bengkok) dalam koordinat
yang sama dengan garis rute, dengan titik pertama di rute dan titik
terakhir di ujung jauh (digambar memudar ke arah ujung itu). Saat ini hanya
data dummy yang mengisinya; untuk data asli, sumber datanya masih perlu
dipilih:

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

## 4. Mode Sumber Data (`NAV_SOURCE`)

Sumber data layar navigasi dipilih saat kompilasi lewat `NAV_SOURCE` di
`src/nav_source.h` (menggantikan `USE_DUMMY_DATA`):

| Nilai | Isi |
| --- | --- |
| `NAV_SOURCE_SIM` (bawaan) | Simulator langsung ke UI |
| `NAV_SOURCE_LOOPBACK` | Simulator → `nav_encode()` → frame 20 byte → `nav_frag_push()` → `nav_decode()` → UI. Membuktikan format biner dan fragmentasi tanpa radio; ukuran paket rata-rata/maksimum dicatat ke serial tiap 5 detik |
| `NAV_SOURCE_BLE` | Data asli dari HP (sementara pengirim uji di laptop) lewat BLE, bagian 1 |

Ganti bawaannya di `src/nav_source.h`, atau per build lewat
`-DNAV_SOURCE=NAV_SOURCE_LOOPBACK` di `build_flags`. Nama yang salah ketik
gagal dikompilasi (`#error`). **Tidak ada fallback otomatis ke simulator
saat runtime**: rute palsu di layar saat koneksi putus berbahaya di jalan.

Pada `NAV_SOURCE_SIM` dan `NAV_SOURCE_LOOPBACK`, data 100% simulasi software:

- **Tidak ada pembacaan QMC5883L sama sekali.** Kode inisialisasi dan
  polling QMC5883L (soft reset `0x80`→`0x0A`, set/reset period
  `0x01`→`0x0B`, continuous mode `0x1D`→`0x09`) masih ada di
  `src/main.cpp`, tapi hanya dikompilasi untuk `NAV_SOURCE_BLE` dengan
  `ENABLE_COMPASS=1` (`src/hw_config.h`, bawaan 0; board belum punya
  sensornya) — jadi tidak pernah dipanggil di mode simulasi. Tidak ada
  transaksi I2C apa pun ke alamat sensor ini dalam mode simulasi.
- **Bearing/heading 100% simulasi**, berupa sapuan halus 0°→360° berulang
  (~13 detik per putaran), murni fungsi waktu — bukan pembacaan kompas
  fisik dalam bentuk apa pun, bukan cuma bearing tujuan. Bearing tidak
  ikut pesan `'N'`, jadi di `NAV_SOURCE_LOOPBACK` jarum layar debug diam
  di 0 (layar navigasi tidak memakainya).
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

Mode loopback sudah diuji di board: 0 paket gagal, ±400–500 paket/detik
(satu per `loop()`), fps penuh dengan waktu render dan jumlah piksel per
frame yang sama dengan `NAV_SOURCE_SIM` (simulatornya deterministik, jadi
rutenya identik). Test host `test/test_nav_loopback` menjalankan 40.000
update simulator lewat codec dan memastikan selisihnya tidak melewati
batas kuantisasi.

Di `NAV_SOURCE_BLE`, `nav_data_t` diisi dari `nav_decode()` atas pesan
`'N'` yang dirakit dari frame BLE (bagian 1). Kode gambar UI tidak perlu
diubah karena sudah membaca dari struct `nav_data_t` yang sama; tambahannya
hanya `ui_update_nav_signal()` untuk state "tidak ada sinyal" (bagian 6).
Untuk menguji tanpa app HP, pakai `tools/ble_sender/` (lihat README-nya).

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
    nav_side_road_t side_roads[8];         // cabang jalan: polyline maks. 4 titik, titik 0 di rute
    uint8_t side_road_count;               // jumlah cabang yang valid di atas
    bool ble_connected;                    // link app HP tersambung (NAV_SOURCE_BLE); false di mode simulasi
} nav_data_t;
```

Sumber data saat ini (`src/nav_sim.cpp`) mengisi struct ini dengan nilai
simulasi. Modul ini sengaja dipisah total dari kode UI (`ui_init.cpp`,
`screens/ui_gps_tracker.c`) supaya penggantinya — `nav_decode()` atas
pesan BLE `'N'` (bagian 1), atau pembacaan sensor langsung — tinggal
mengisi `nav_data_t` yang sama, tanpa mengubah satu pun baris kode di
layer UI.

## 6. Layar Navigasi (`screens/ui_nav_display.c`)

Layar default sekarang adalah tampilan turn-by-turn untuk layar bulat
240x240, seluruhnya digambar lewat draw callback LVGL (bukan gambar/bitmap)
dan hanya membaca `nav_data_t`:

- **Garis rute** (`schematic_points`): putih tebal dengan tepi abu tipis di
  kedua sisinya, heading-up, rider selalu di panah tengah-bawah. Rider
  berada di titik asal (0,0) dan harus terletak di garis rute. Skala 4 px
  per satuan; titik terakhir sebaiknya jauh di luar layar supaya garis
  tidak terlihat berhenti di tengah.
- **Jalan samping** (`side_roads`): digambar sebagai sepasang garis tepi
  tipis abu-abu kebiruan (2 px, berjarak 8 px) yang mengikuti bengkoknya
  polyline dan memudar satu tingkat per segmen dari rute ke ujung jauhnya.
  Warnanya sama dengan tepi rute, dan tepi rute dibuka di tempat jalan
  samping bertemu (mulut persimpangan), jadi dua garis jalan samping
  menyambung ke tepi rute seperti satu jaringan jalan. Pangkalnya ditarik
  4,5 px masuk ke bawah rute, supaya tetap menyatu walaupun sudut rute di
  persimpangan dipotong. Muncul dan hilang dengan fade ~200 ms saat
  masuk/keluar jendela data.
- **Panah rider 3D**: sisi kiri terang dan sisi kanan teduh (cahaya dari
  kiri), sisi bawah diberi ketebalan yang lebih gelap, plus bayangan lembut.
- **Panel bawah berbentuk kubah** (lingkaran besar berpusat di bawah layar):
  berisi ikon maneuver (lurus / kiri / kanan / putar balik) dan
  `distance_to_turn_m` (Montserrat 36 Medium, satuan Montserrat Bold 20).
  Jarak dibulatkan per 10 m (>= 100 m) atau 5 m (< 100 m), dan ditampilkan
  dalam km dengan satu desimal mulai ~1 km.
- **Busur progres** di tepi bawah: terisi dari kiri ke kanan dalam 500 m
  terakhir sebelum belokan (`PROGRESS_RANGE_M`), kosong kalau masih jauh.
- **State "tidak ada sinyal"** (`ui_nav_display_set_signal(false)`; dipakai
  di `NAV_SOURCE_BLE` saat link putus atau > 3 detik tanpa pesan valid):
  peta memudar ke ±30% kecerahan lewat rasterizer (warna dicampur ke latar,
  ±0,4 detik), ikon diganti batang sinyal abu yang dicoret oranye (di-cache
  seperti ikon maneuver), jarak menjadi `--` abu, satuan kosong, busur
  progres langsung kosong. Rute terakhir tetap terlihat redup, tapi panah
  belok dan jarak lama tidak pernah tampil seolah masih berlaku. Saat data
  kembali, peta memudar terang lagi dan bergeser mulus ke posisi baru. Saat
  state berganti, seluruh layar digambar ulang dalam satu pass (bukan empat
  area terpisah) dan busur tidak beranimasi, supaya frame transisi tidak
  lebih berat dari frame biasa; setelah pudar selesai layar diam tidak
  digambar ulang.

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
- Karena data dummy berupa rute menerus, belokan berikutnya sudah terlihat
  sebelum tiba, dan saat rider melewati belokan tampilan ikut berputar di
  sekitar panah (heading-up) tanpa ada garis yang diganti.

### Performa di ESP32-S3 (diukur di board)

Frame yang bergerak butuh rata-rata ±12–13 ms dan paling berat ±24 ms, jadi
refresh tiap 30 ms memberi ±33 fps tanpa frame yang melewati periode. Angka
ini bisa dilihat sendiri dengan mengubah `LV_PORT_PERF_LOG` di
`src/lv_port_disp.cpp` menjadi `1` (atau `-DLV_PORT_PERF_LOG=1`): FPS, jeda
terlama antar-frame, frame rata-rata dan **terberat**, render saja, tunggu
DMA, jumlah frame yang melewati periode, serta piksel per frame dicetak ke
serial tiap 2 detik. Dengan periode 25 ms dulu, frame tunggal bisa
mencapai 27–29 ms karena menunggu DMA frame sebelumnya (satu buffer layar);
pengukuran lengkap dan eksperimen buffer parsial yang ditolak ada di
`docs/dokumentasi-proyek.md` bagian 9 dan 13.8. Hal-hal yang membuatnya
cukup cepat, semuanya diukur:

- **Lapisan peta digambar sendiri** (`screens/ui_nav_raster.c`): latar,
  jalan samping, tepi dan inti rute, serta mulut persimpangan ditulis
  langsung ke buffer layer dengan rasterizer kecil (segmen anti-aliasing
  berujung bulat, hanya mengunjungi piksel di sekitar garis, jarak dihitung
  bertahap per piksel). Renderer garis LVGL makan ±0,1–0,3 ms per garis di
  ESP32-S3, terlalu mahal untuk peta berisi puluhan garis; dengan
  rasterizer ini seluruh peta ±4–8 ms. Layar LVGL dibuat transparan supaya
  tidak ada yang digambar di bawah peta; kubah, busur, panah, ikon, dan
  teks tetap digambar LVGL di atasnya.
- **LVGL dikunci ke 9.6.0** di `platformio.ini`: 9.5 sekitar 3 ms lebih
  lambat per frame, dan versi yang tidak dikunci sempat membuat build di
  dua folder berbeda versi.
- **Biaya LVGL sisanya adalah per tugas gambar dan per putaran render**,
  bukan per piksel, karena LVGL menjalankan ulang seluruh draw callback
  untuk tiap putaran. Karena itu:
  - **Mode render DIRECT dengan satu buffer layar penuh** (format
    RGB565_SWAPPED, jadi tidak perlu tukar byte): area apa pun dirender
    dalam satu putaran, langsung di posisinya, dan beberapa area dalam satu
    frame tidak saling menunggu transfer.
  - **Panah 3D dan ikon belok di-cache** sebagai canvas (gambar) dan hanya
    disalin tiap frame; ikon digambar ulang hanya saat maneuver berganti.
  - **Bentuk di luar area yang sedang dirender dilewati** sebelum tugas
    LVGL dibuat.
- **Garis rute disederhanakan** (Douglas–Peucker, 0,75 px) sebelum
  digambar, jadi lebih sedikit segmen yang saling tumpang di sambungan.
- **Pengiriman lewat `spi_master` ESP-IDF, sepenuhnya di latar belakang**:
  satu frame dikirim sebagai antrean transaksi (alamat, lalu potongan ≤64
  baris dengan RAMWR / RAMWR_CONTINUE, karena satu transaksi DMA ESP32-S3
  maksimal 32 KB). TFT_eSPI hanya dipakai di `setup()` untuk inisialisasi
  panel dan teks pembuka. DMA TFT_eSPI tidak dipakai: versi 2.5.43 crash di
  callback akhir transfernya di ESP32-S3, dan mencampur tulisan register
  TFT_eSPI dengan DMA driver IDF membuat panel mengabaikan semua frame.
  `esp_lcd` juga tidak dipakai: di IDF 4.4 ia hanya mengizinkan satu
  transfer berjalan, jadi frame yang lebih besar dari satu transfer harus
  ditunggu.
- **Refresh tiap 30 ms**, di atas frame terberat termasuk tunggu DMA
  (dinaikkan dari 25 ms pada 8 Okt 2026). Periode yang lebih pendek dari
  waktu frame membuat jarak antar-frame berselang-seling satu/dua periode
  dan terasa patah-patah. `loop()` hanya tidur 1 ms supaya timer LVGL tidak
  bergeser.
- **`-O2`** menggantikan `-Os` bawaan framework (di `platformio.ini`).

Layar debug lama (lat/lon, kompas X/Y/Z, jarum heading) masih ada: ubah
`UI_START_DEBUG_SCREEN` di `src/ui_init.cpp` menjadi `1` untuk memakainya.

### Hal yang belum diimplementasikan (di luar cakupan dokumen ini)

- **IMU (MPU6050)**: belum ada di hardware, belum ada kode pembacaannya.
- **Kompas QMC5883L**: belum terpasang. Kodenya opsional (`ENABLE_COMPASS`)
  dan hanya dipakai layar debug lama; tidak ada sumber heading lain dari HP.
  Field yang nanti butuh data IMU (mis. tilt-compensated heading) belum
  ditentukan strukturnya.
- **App HP**: belum. Pengirim uji di laptop (`tools/ble_sender/`) mengisi
  perannya untuk pengujian; `navgeo.py` di sana adalah implementasi
  referensi konversi lat/lon → jendela heading-up yang nanti diporting ke
  app.
- **Pemisahan pesan geometri/pose** dan **zoom mengikuti kecepatan**: opsi
  untuk nanti.
