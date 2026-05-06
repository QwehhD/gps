#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <lvgl.h>
#include "../ui.h"
#include "../screens/ui_watch_digital.h"

// Forward declarations for LVGL port
extern void lv_port_disp_init(void);
extern void lv_port_tick_inc(void);
extern "C" void ui_update_gps(double lat, double lon, float speed, float heading, bool connected);

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
};

GPSData gpsData;
CompassData compassData;

// BLE variables
BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic = NULL;
bool deviceConnected = false;

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

class MyServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *pServer)
  {
    deviceConnected = true;
    Serial.println("✓ BLE Client connected");
  }
  void onDisconnect(BLEServer *pServer)
  {
    deviceConnected = false;
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
    if (value.length() > 0)
    {
      String data = String((char *)value.c_str());
      Serial.printf("📡 BLE RX (%d bytes): %s\n", value.length(), data.c_str());

      // Parse GPS: format "GPS:lat,lon,speed"
      if (data.startsWith("GPS:"))
      {
        String gpsStr = data.substring(4); // Remove "GPS:"
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
      // Parse ROUTE: format "ROUTE:lat1,lon1;lat2,lon2;..."
      else if (data.startsWith("ROUTE:"))
      {
        Serial.println("✓ Route data received");
      }
      else
      {
        Serial.printf("⚠ Unknown format: %s\n", data.c_str());
      }
    }
  }
};

// Old TFT_eSPI drawing functions removed - using LVGL UI instead
// drawDestinationArrow, drawMapView, drawRandomRoads, drawRoutePath, drawEgocentricMapView

void updateCompass()
{
  // Simulate compass heading (0.5°/100ms = 180°/minute)
  static uint32_t lastUpdate = 0;
  if (millis() - lastUpdate > 100)
  {
    lastUpdate = millis();
    compassData.heading += 0.5;
    if (compassData.heading >= 360)
      compassData.heading = 0;
  }
  
  // TODO: Enable QMC5883L when library docs available
  // For now, using simulated heading for UI testing
}

void setup()
{
  pixels.begin();
  pixels.setPixelColor(0, pixels.Color(0, 0, 0));
  pixels.show();

  Serial.begin(115200);
  delay(500);

  // Suppress BT stack debug logs
  esp_log_level_set("BT_SMP", ESP_LOG_ERROR);
  esp_log_level_set("BT_BTM", ESP_LOG_ERROR);

  Serial.println("\n=== GPS TRACKER SETUP ===");
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawCentreString("GPS TRACKER", 120, 50, 4);
  tft.drawCentreString("INITIALIZING", 120, 100, 2);

  Wire.begin(2, 1);
  Wire.setClock(50000);

  Serial.println("⚠ QMC5883L disabled (pending library documentation)");
  Serial.println("  Using simulated heading for UI testing");
  tft.drawCentreString("QMC: SIM", 120, 130, 1);

  // Init BLE with simpler config
  BLEDevice::init("GPS_Tracker_BLE");
  BLEDevice::setMTU(517); // Increase MTU for larger packets

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  // TX Characteristic (Notify)
  pTxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_TX,
      BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());

  // RX Characteristic (Write - NO response needed)
  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_RX,
      BLECharacteristic::PROPERTY_WRITE_NR);
  pRxCharacteristic->setCallbacks(new MyCharacteristicCallbacks());

  pService->start();

  // Configure advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x00);
  pAdvertising->setMaxPreferred(0x00);
  pAdvertising->setMinInterval(0x20); // 32 * 0.625ms = 20ms
  pAdvertising->setMaxInterval(0x40); // 64 * 0.625ms = 40ms

  BLEDevice::startAdvertising();

  Serial.println("✓ BLE started as 'GPS_Tracker_BLE'");

  // Initialize LVGL
  lv_init();
  lv_port_disp_init();
  ui_init();

  delay(2000);
  Serial.println("Setup complete! LVGL watch UI ready...\n");
}

void loop()
{
  // Update LVGL
  lv_timer_handler();

  // Update LVGL tick
  lv_port_tick_inc();

  // Update compass heading
  updateCompass();

  // Update UI with latest GPS data every 1 second (1000ms)
  static uint32_t lastUpdate = 0;
  if (millis() - lastUpdate >= 1000)
  {
    lastUpdate = millis();
    ui_update_gps(gpsData.currentLat, gpsData.currentLon,
                  gpsData.speed, compassData.heading, deviceConnected);
  }

  delay(5); // LVGL tick interval
}
