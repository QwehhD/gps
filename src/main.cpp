#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include "../ui.h"
#include "../screens/ui_watch_digital.h"
#include "nav_sim.h"
#include "nav_source.h"
#include "nav_codec.h"
#include "nav_frag.h"
#include "nav_rx.h"

// Forward declarations for LVGL port
extern void lv_port_disp_init(void);
extern "C" void ui_update_gps(double lat, double lon, float speed, float heading, int16_t x, int16_t y, int16_t z, bool connected);
extern "C" void ui_update_nav_heading(float bearing_deg);
extern "C" void ui_update_nav_info(const nav_data_t *nav);
extern "C" void ui_update_nav_display(const nav_data_t *nav);
extern "C" void ui_update_nav_signal(bool has_signal);

TFT_eSPI tft = TFT_eSPI();
Adafruit_NeoPixel pixels(1, 48, NEO_GRB + NEO_KHZ800);

struct GPSData
{
  double currentLat = -6.2088;
  double currentLon = 106.8456;
  double destLat = -6.1944;
  double destLon = 106.8294;
  float speed = 25.0;
  bool hasData = true;
};

struct CompassData
{
  float heading = 0.0;
  int16_t x = 0;
  int16_t y = 0;
  int16_t z = 0;
};

GPSData gpsData;
CompassData compassData;

// BLE variables
BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic = NULL;
volatile bool deviceConnected = false; // set by the BLE task, read by loop()

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

class MyServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *pServer)
  {
    deviceConnected = true;
#if NAV_SOURCE == NAV_SOURCE_BLE
    nav_rx_on_connect();
#endif
    Serial.println("✓ BLE Client connected");
  }
  void onDisconnect(BLEServer *pServer)
  {
    deviceConnected = false;
#if NAV_SOURCE == NAV_SOURCE_BLE
    nav_rx_on_disconnect();
#endif
    Serial.println("⚠ BLE Client disconnected");
    pServer->getAdvertising()->start();
  }

  uint32_t onPassKeyRequest()
  {
    Serial.println("⚠ Passkey request (returning 0)");
    return 0;
  }

  void onPassKeyNotify(uint32_t passKey) {}
  bool onSecurityRequest() { return false; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {}
};

class MyCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic)
  {
    std::string value = pCharacteristic->getValue();
    const uint8_t *bytes = (const uint8_t *)value.data();
    if (value.length() > 0 && bytes[0] == NAV_FRAG_TYPE)
    {
      // Binary nav frames (nav_frag.h), several per message: never printed.
      // Only NAV_SOURCE_BLE shows them; the simulated sources ignore them.
#if NAV_SOURCE == NAV_SOURCE_BLE
      nav_rx_on_frame(bytes, value.length());
#endif
      return;
    }
    if (value.length() > 0)
    {
      String data = String((char *)value.c_str());
      Serial.printf("📡 BLE RX (%d bytes): %s\n", value.length(), data.c_str());

      if (data.startsWith("GPS:"))
      {
        String gpsStr = data.substring(4); 
        int comma1 = gpsStr.indexOf(',');
        int comma2 = gpsStr.lastIndexOf(',');

        if (comma1 > 0 && comma2 > comma1)
        {
          double lat = gpsStr.substring(0, comma1).toDouble();
          double lon = gpsStr.substring(comma1 + 1, comma2).toDouble();
          float speed = gpsStr.substring(comma2 + 1).toFloat();

          gpsData.currentLat = lat;
          gpsData.currentLon = lon;
          gpsData.speed = speed;
          gpsData.hasData = true;

          Serial.printf("✓ GPS Updated: Lat=%.6f Lon=%.6f Speed=%.1f\n", lat, lon, speed);
        }
      }
      else if (data.startsWith("ROUTE:"))
      {
        Serial.println("✓ Route data received");
      }
    }
  }
};

// QMC5883L polling. Left completely untouched, but only built for
// NAV_SOURCE_BLE (see nav_source.h) — the simulated sources take the heading
// from the simulator instead, with zero I2C traffic to the sensor.
#if NAV_SOURCE == NAV_SOURCE_BLE
void updateCompass()
{
  static uint32_t lastUpdate = 0;
  if (millis() - lastUpdate > 100)
  {
    lastUpdate = millis();
    
    Wire.beginTransmission(0x0D);  
    Wire.write(0x00);              
    if (Wire.endTransmission(false) == 0)
    {
      if (Wire.requestFrom(0x0D, 6) == 6)
      {
        int16_t x = (int16_t)(Wire.read() | (Wire.read() << 8));
        int16_t y = (int16_t)(Wire.read() | (Wire.read() << 8));
        int16_t z = (int16_t)(Wire.read() | (Wire.read() << 8));
        
        // Simpan ke struct
        compassData.x = x;
        compassData.y = y;
        compassData.z = z;
        
        // Hanya update jika data tidak nol semua (mencegah glitch UI)
        if (x != 0 || y != 0) {
          float h = atan2((float)y, (float)x) * 180.0 / M_PI;
          if (h < 0) h += 360.0;
          compassData.heading = h;
          Serial.printf("QMC5883L: X=%d Y=%d Z=%d heading=%.1f°\n", x, y, z, compassData.heading);
        } else {
          // Jika masih nol, pancing lagi Continuous Mode-nya
          Wire.beginTransmission(0x0D);
          Wire.write(0x09);
          Wire.write(0x1D);
          Wire.endTransmission();
        }
      }
    }
  }
}
#endif // NAV_SOURCE == NAV_SOURCE_BLE

#if NAV_SOURCE == NAV_SOURCE_LOOPBACK
// Sends the simulator output through the BLE wire format (encode, split
// into frames of the smallest BLE write, reassemble, decode) so the display
// shows exactly what would survive the trip from the phone. Logs the packet
// sizes every few seconds.
static const nav_data_t *nav_loopback(const nav_data_t *sim)
{
  static uint8_t packet[NAV_MSG_MAX_SIZE];
  static nav_frag_t frag; // zeroed, same as nav_frag_init()
  static nav_data_t decoded;
  static uint8_t tx_seq = 0;
  static uint8_t max_frames = 0;
  static uint32_t packets = 0;
  static uint32_t errors = 0;
  static uint64_t total_bytes = 0;
  static size_t max_bytes = 0;
  static uint32_t last_log_ms = millis();
  static uint32_t last_log_packets = 0;

  size_t len = nav_encode(sim, packet, sizeof(packet), tx_seq);
  uint8_t frames = nav_frag_count(len, NAV_FRAG_MIN_FRAME);
  const uint8_t *msg = nullptr;
  size_t msg_len = 0;
  for (uint8_t i = 0; i < frames; i++)
  {
    uint8_t frame[NAV_FRAG_MIN_FRAME];
    size_t frame_len = nav_frag_build(packet, len, tx_seq, NAV_FRAG_MIN_FRAME, i, frame, sizeof(frame));
    nav_frag_push(&frag, frame, frame_len, &msg, &msg_len);
  }
  if (frames > max_frames)
  {
    max_frames = frames;
  }
  uint8_t rx_seq = 0;
  nav_decode_status_t status = msg != nullptr ? nav_decode(msg, msg_len, &decoded, &rx_seq)
                                              : NAV_DECODE_ERR_TRUNCATED;
  if (status != NAV_DECODE_OK || rx_seq != tx_seq)
  {
    // decoded keeps the last good packet.
    errors++;
    Serial.printf("loopback: decode failed: %s (%u bytes, seq %u/%u)\n",
                  nav_decode_status_str(status), (unsigned)len, (unsigned)tx_seq, (unsigned)rx_seq);
  }
  tx_seq++;
  packets++;
  total_bytes += len;
  if (len > max_bytes)
  {
    max_bytes = len;
  }

  uint32_t now = millis();
  if (now - last_log_ms >= 5000)
  {
    Serial.printf("loopback: %lu packets (%lu/s), avg %.1f B, max %u B (%u frames of %u B), errors %lu\n",
                  (unsigned long)packets,
                  (unsigned long)((packets - last_log_packets) * 1000UL / (now - last_log_ms)),
                  (double)total_bytes / packets, (unsigned)max_bytes, (unsigned)max_frames,
                  (unsigned)NAV_FRAG_MIN_FRAME, (unsigned long)errors);
    last_log_ms = now;
    last_log_packets = packets;
  }
  return &decoded;
}
#endif // NAV_SOURCE == NAV_SOURCE_LOOPBACK

void setup()
{
  pixels.begin();
  pixels.setPixelColor(0, pixels.Color(0, 0, 0));
  pixels.show();

  Serial.begin(115200);
  delay(500);

  esp_log_level_set("BT_SMP", ESP_LOG_ERROR);
  esp_log_level_set("BT_BTM", ESP_LOG_ERROR);

  Serial.println("\n=== GPS TRACKER SETUP ===");

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawCentreString("GPS TRACKER", 120, 50, 4);

#if NAV_SOURCE != NAV_SOURCE_BLE
  // Simulated sources: no I2C bus, no QMC5883L access at all — everything the
  // UI shows (heading included) comes from nav_sim instead.
#if NAV_SOURCE == NAV_SOURCE_LOOPBACK
  Serial.println("NAV_SOURCE=LOOPBACK: skipping QMC5883L init, simulated nav data through the codec");
  tft.drawCentreString("LOOPBACK MODE", 120, 130, 2);
#else
  Serial.println("NAV_SOURCE=SIM: skipping QMC5883L init, simulating nav data");
  tft.drawCentreString("DUMMY DATA MODE", 120, 130, 2);
#endif
  nav_sim_init();
#else
  Wire.begin(2, 1);
  Wire.setClock(50000);

  // Initialize QMC5883L dengan prosedur Reset
  Wire.beginTransmission(0x0D);
  Wire.write(0x0B);
  if (Wire.endTransmission(false) == 0 && Wire.requestFrom(0x0D, 1) == 1)
  {
    byte chipID = Wire.read();
    if (chipID == 0xFF)
    {
      Serial.printf("✓ QMC5883L detected (ChipID: 0x%02X)\n", chipID);

      // 1. SOFT RESET
      Wire.beginTransmission(0x0D);
      Wire.write(0x0A);
      Wire.write(0x80); // Set Soft Reset
      Wire.endTransmission();
      delay(100);

      // 2. SET/RESET PERIOD (Penting untuk memulai pengukuran)
      Wire.beginTransmission(0x0D);
      Wire.write(0x0B);
      Wire.write(0x01);
      Wire.endTransmission();

      // 3. KONFIGURASI OPERASI (Continuous Mode)
      Wire.beginTransmission(0x0D);
      Wire.write(0x09);
      Wire.write(0x1D); // 200Hz, 8G range, 512 oversampling, Continuous mode
      Wire.endTransmission();

      tft.drawCentreString("QMC OK", 120, 130, 2);
    }
    else
    {
      tft.drawCentreString("QMC UNKNOWN", 120, 130, 1);
    }
  }
  else
  {
    Serial.println("⚠ QMC5883L not responding");
    tft.drawCentreString("QMC ERR", 120, 130, 1);
  }
#endif // NAV_SOURCE != NAV_SOURCE_BLE

#if NAV_SOURCE == NAV_SOURCE_BLE
  nav_rx_init(); // the queue must exist before the first BLE callback
#endif

  // BLE Setup (Tetap sesuai aslinya)
  BLEDevice::init("GPS_Tracker_BLE");
  BLEDevice::setMTU(517); 
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_TX, BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());
  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_RX, BLECharacteristic::PROPERTY_WRITE_NR);
  pRxCharacteristic->setCallbacks(new MyCharacteristicCallbacks());
  pService->start();
  BLEDevice::startAdvertising();
  Serial.printf("BLE stack: %s, internal heap free %u B after BLE init\n",
#if defined(CONFIG_BT_NIMBLE_ENABLED)
                "NimBLE",
#elif defined(CONFIG_BT_BLUEDROID_ENABLED)
                "Bluedroid",
#else
                "unknown",
#endif
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  // Initialize LVGL
  lv_init();
  lv_port_disp_init();
  ui_init();
#if NAV_SOURCE == NAV_SOURCE_BLE
  ui_update_nav_signal(false); // nothing received yet
#endif

  Serial.println("✓ Setup complete!");
}

void loop()
{
  lv_timer_handler();

#if NAV_SOURCE != NAV_SOURCE_BLE
  // Advance the simulator every iteration and push the heading and the
  // navigation display data right away so they move smoothly (~30fps+); the
  // debug screen's text/schematic updates are throttled below since they
  // don't need frame-rate refresh.
  nav_sim_update(millis());
  const nav_data_t *nav = nav_sim_get_data();
#if NAV_SOURCE == NAV_SOURCE_LOOPBACK
  // bearing_deg is not on the wire, so the debug screen's needle stays at 0.
  nav = nav_loopback(nav);
#endif
  ui_update_nav_heading(nav->bearing_deg);
  ui_update_nav_display(nav);

  static uint32_t lastNavInfoUpdate = 0;
  if (millis() - lastNavInfoUpdate >= 200)
  {
    lastNavInfoUpdate = millis();
    ui_update_nav_info(nav);
    // GPS status label still reflects the real (already-implemented) BLE
    // link; nav->ble_connected is a separate placeholder for the future
    // nav phone-app protocol, not implemented yet (see docs).
    ui_update_gps(gpsData.currentLat, gpsData.currentLon,
                  nav->speed_kmh, nav->bearing_deg,
                  0, 0, 0, deviceConnected);
  }
#else
  updateCompass();

  static uint32_t lastUpdate = 0;
  if (millis() - lastUpdate >= 500)
  {
    lastUpdate = millis();
    ui_update_gps(gpsData.currentLat, gpsData.currentLon,
                  gpsData.speed, compassData.heading,
                  compassData.x, compassData.y, compassData.z, deviceConnected);
  }

  // Newest nav data from the BLE task. With the link down, or no valid
  // message for NAV_RX_STALE_MS, the display switches to "no signal" rather
  // than showing old guidance as if it still held. There is no fallback to
  // the simulator.
  static nav_data_t nav;
  uint32_t age_ms = 0;
  bool have = nav_rx_latest(&nav, &age_ms);
  bool connected = deviceConnected;
  bool fresh = have && connected && age_ms <= NAV_RX_STALE_MS;
  ui_update_nav_signal(fresh);
  if (have)
  {
    nav.ble_connected = connected;
    ui_update_nav_display(&nav);
  }

  static bool was_fresh = false;
  if (fresh != was_fresh)
  {
    was_fresh = fresh;
    if (fresh)
    {
      Serial.println("nav: signal back");
    }
    else
    {
      Serial.printf("nav: no signal (%s)\n",
                    !connected ? "link down" : !have ? "no data yet" : "no valid message for 3 s");
    }
  }
  nav_rx_log_stats(connected);
#endif // NAV_SOURCE != NAV_SOURCE_BLE

  // Short sleep: LVGL's timers only run when loop() comes back here, so a
  // longer delay would make the display refresh drift by up to that much.
  delay(1);
}