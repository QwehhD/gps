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
extern "C" void ui_update_gps(double lat, double lon, float speed, float heading, int16_t x, int16_t y, int16_t z, bool connected);

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

  // Initialize LVGL
  lv_init();
  lv_port_disp_init();
  ui_init();

  Serial.println("✓ Setup complete!");
}

void loop()
{
  lv_timer_handler();
  lv_port_tick_inc();

  updateCompass();

  static uint32_t lastUpdate = 0;
  if (millis() - lastUpdate >= 500)
  {
    lastUpdate = millis();
    ui_update_gps(gpsData.currentLat, gpsData.currentLon,
                  gpsData.speed, compassData.heading, 
                  compassData.x, compassData.y, compassData.z, deviceConnected);
  }

  delay(5);
}