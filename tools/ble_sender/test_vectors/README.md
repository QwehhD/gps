# Test vector pipeline lat/lon → pesan `'N'`

Acuan untuk port pipeline `navgeo.py` + `navproto.py` ke app HP
(TypeScript): untuk input yang sama, port harus menghasilkan pesan dan
frame yang **identik byte per byte**.

Berkas di sini dibuat oleh `../make_test_vectors.py`, jangan diedit
manual. `pytest` gagal kalau isinya tidak cocok lagi dengan kode Python
(`test_the_files_are_up_to_date`). Setelah pipeline Python diubah dengan
sengaja, buat ulang dengan:

```bash
.venv/bin/python make_test_vectors.py          # tulis ulang
.venv/bin/python make_test_vectors.py --check  # hanya cek, keluar 1 kalau basi
```

Semua koordinat **sintetis**, berupa meter di sekitar titik asal
(−7.0, 110.0), bukan rute pribadi. Test `test_only_synthetic_coordinates`
menolak titik yang lebih dari 5 km dari titik asal.

## Kasus

| Berkas | Isi |
| --- | --- |
| `right_turn.json` | Arah 30°, belok kanan ke 120° 180 m di depan; fix 3 m di kiri jalan |
| `left_turn.json` | Arah 200°, belok kiri ke 110° 120 m di depan |
| `u_turn.json` | Arah 350°, putar balik lewat median 12 m 100 m di depan |
| `straight.json` | Satu ruas lurus 45°, tanpa maneuver: jarak ke ujung rute; fix 5 m di kanan jalan |
| `near_end.json` | 30 m sebelum ujung, setelah belokan terakhir, dengan posisi cocok sebelumnya; nomor urut 255 |
| `curve_simplified.json` | Tikungan panjang dengan > 32 titik di jendela, jadi jendela disederhanakan (25 titik) |

## Isi satu berkas

- `config`: konstanta pipeline (15 m per satuan, jendela −12…+80 satuan,
  chord heading ±3 satuan, maks. 32 titik, kuantisasi 1/16, meter per
  derajat).
- `input`:
  - `route_latlon`: polyline `[lat, lon]` (**latitude dulu**, beda dengan
    GeoJSON).
  - `turns`: maneuver yang dipakai, masing-masing `index` (titik rute,
    yaitu `way_points[0]` step ORS), `s_m` (jarak tempuh di titik itu), dan
    `maneuver`.
  - `fix_latlon`: posisi GPS.
  - `previous_s_m`: hasil map-matching update sebelumnya, atau `null`.
  - `speed_kmh`, `seq`: kecepatan dan nomor urut pesan.
- `expected`:
  - Nilai antara untuk mencari letak selisih: `route_length_m`,
    `matched_s_m`, `offset_m`, `heading_deg`, `distance_to_turn_m`,
    `total_distance_m`, dan `window_units` (titik jendela heading-up dalam
    satuan, sebelum kuantisasi).
  - `message_hex`: pesan `'N'` lengkap.
  - `frames_hex`: frame BLE untuk write 20, 182, dan 514 byte.

## Catatan untuk port

- Bandingkan `message_hex` dan `frames_hex` persis. Nilai antara cukup
  dibandingkan dengan toleransi ±1e-9, karena `sin`/`cos`/`atan2` antar
  bahasa bisa berbeda di bit terakhir. Generator menolak kasus yang nilai
  terkuantisasinya jatuh di batas pembulatan (`.5`), jadi selisih sekecil
  itu tidak bisa mengubah byte.
- Pembulatan di encoder adalah **setengah menjauhi nol** (seperti `roundf`
  di C): `Math.sign(v) * Math.floor(Math.abs(v) + 0.5)`. Jangan pakai
  `Math.round`, yang membulatkan −2,5 ke −2.
- Angka di JSON ditulis lengkap (bentuk terpendek yang kembali ke double
  yang sama), jadi `JSON.parse` menghasilkan double yang sama persis
  dengan Python.
- Urutan langkahnya mengikuti `Navigator.update()` di `navgeo.py`:
  map-matching (cari dekat posisi sebelumnya), jendela, proyeksi
  equirectangular di sekitar titik rider, penyederhanaan Douglas-Peucker
  (toleransi 0,5 m, dikali 1,5 sampai ≤ 32 titik, titik rider selalu
  dipertahankan), heading dari chord ±45 m, rotasi heading-up, lalu dibagi
  15.
