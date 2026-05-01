#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <Adafruit_HMC5883_U.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Forward declarations
void updateCompass();
void drawRandomRoads(int centerX, int centerY, float heading);
void drawRoutePath(int centerX, int centerY, float heading);
void drawEgocentricMapView();
void setup();
void loop();

TFT_eSPI tft = TFT_eSPI();
Adafruit_NeoPixel pixels(1, 48, NEO_GRB + NEO_KHZ800);
Adafruit_HMC5883_Unified mag = Adafruit_HMC5883_Unified(12345);

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

struct RoutePoint
{
  double lat;
  double lon;
};

struct RouteData
{
  RoutePoint waypoints[20];
  int pointCount = 0;
  bool hasRoute = false;
};

GPSData gpsData;
CompassData compassData;
RouteData routeData;

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
};

class MyCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic)
  {
    std::string value = pCharacteristic->getValue();
    if (value.length() > 0)
    {
      String data = String((char *)value.c_str());

      if (data.startsWith("GPS:"))
      {
        // Format: GPS:lat,lon,speed
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

          Serial.printf("GPS via BLE: %.4f, %.4f | Speed: %.1f km/h\n", lat, lon, speed);
        }
      }
      else if (data.startsWith("ROUTE:"))
      {
        // Format: ROUTE:lat1,lon1;lat2,lon2;lat3,lon3
        String routeStr = data.substring(6);
        routeData.pointCount = 0;

        int start = 0;
        while (start < routeStr.length() && routeData.pointCount < 20)
        {
          int semicolon = routeStr.indexOf(';', start);
          if (semicolon == -1)
            semicolon = routeStr.length();

          String point = routeStr.substring(start, semicolon);
          int comma = point.indexOf(',');

          if (comma > 0)
          {
            routeData.waypoints[routeData.pointCount].lat = point.substring(0, comma).toDouble();
            routeData.waypoints[routeData.pointCount].lon = point.substring(comma + 1).toDouble();
            routeData.pointCount++;
          }

          start = semicolon + 1;
        }

        routeData.hasRoute = (routeData.pointCount > 0);
        Serial.printf("Route received: %d waypoints\n", routeData.pointCount);
      }
    }
  }
};

void drawRandomRoads(int centerX, int centerY, float heading)
{
  // Draw random road lines for visual reference (jalan acak tipis)
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);

  float cosH = cos(heading * PI / 180);
  float sinH = sin(heading * PI / 180);

  // Horizontal roads (berubah sesuai heading)
  for (int i = -80; i <= 80; i += 30)
  {
    int x1 = centerX + (int)(i * cosH - 80 * sinH);
    int y1 = centerY + (int)(i * sinH + 80 * cosH);
    int x2 = centerX + (int)(i * cosH + 80 * sinH);
    int y2 = centerY + (int)(i * sinH - 80 * cosH);

    tft.drawLine(x1, y1, x2, y2, TFT_DARKGREY);
  }

  // Vertical roads
  for (int i = -80; i <= 80; i += 30)
  {
    int x1 = centerX + (int)(-80 * cosH - i * sinH);
    int y1 = centerY + (int)(-80 * sinH + i * cosH);
    int x2 = centerX + (int)(80 * cosH - i * sinH);
    int y2 = centerY + (int)(80 * sinH + i * cosH);

    tft.drawLine(x1, y1, x2, y2, TFT_DARKGREY);
  }
}

void drawRoutePath(int centerX, int centerY, float heading)
{
  if (!routeData.hasRoute || routeData.pointCount == 0)
    return;

  float cosH = cos(heading * PI / 180);
  float sinH = sin(heading * PI / 180);

  for (int i = 0; i < routeData.pointCount - 1; i++)
  {
    double dLat1 = routeData.waypoints[i].lat - gpsData.currentLat;
    double dLon1 = routeData.waypoints[i].lon - gpsData.currentLon;
    double dLat2 = routeData.waypoints[i + 1].lat - gpsData.currentLat;
    double dLon2 = routeData.waypoints[i + 1].lon - gpsData.currentLon;

    // Convert to screen coordinates (simplified)
    int x1 = centerX + (int)(dLat1 * 10000 * cosH - dLon1 * 10000 * sinH);
    int y1 = centerY + (int)(dLat1 * 10000 * sinH + dLon1 * 10000 * cosH);
    int x2 = centerX + (int)(dLat2 * 10000 * cosH - dLon2 * 10000 * sinH);
    int y2 = centerY + (int)(dLat2 * 10000 * sinH + dLon2 * 10000 * cosH);

    tft.drawLine(x1, y1, x2, y2, TFT_WHITE);
    tft.drawLine(x1 - 1, y1, x2 - 1, y2, TFT_WHITE);
    tft.drawLine(x1 + 1, y1, x2 + 1, y2, TFT_WHITE);
  }
}

void drawEgocentricMapView()
{
  tft.fillScreen(TFT_BLACK);

  double dLat = gpsData.destLat - gpsData.currentLat;
  double dLon = gpsData.destLon - gpsData.currentLon;
  double distance = sqrt(dLat * dLat + dLon * dLon) * 111000;

  String distStr;
  if (distance < 1000)
  {
    distStr = String((int)distance) + "m";
  }
  else
  {
    distStr = String(distance / 1000, 1) + "km";
  }

  int centerX = 120, centerY = 120;

  // Draw random road grid
  drawRandomRoads(centerX, centerY, compassData.heading);

  // Draw main route if available
  drawRoutePath(centerX, centerY, compassData.heading);

  // Draw central compass point
  tft.fillCircle(centerX, centerY, 4, TFT_CYAN);
  tft.drawCircle(centerX, centerY, 10, TFT_CYAN);

  // Draw direction pointer (always at top - egocentric!)
  tft.fillTriangle(centerX - 6, centerY - 60, centerX + 6, centerY - 60, centerX, centerY - 45, TFT_RED);

  // Draw distance at top
  tft.fillRoundRect(20, 6, 200, 44, 10, TFT_DARKGREY);
  tft.fillRoundRect(22, 8, 196, 40, 9, TFT_BLACK);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawCentreString("TARGET", 120, 10, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString(distStr, 120, 22, 8);

  // Status bar at bottom
  tft.fillRoundRect(18, 200, 204, 28, 10, TFT_DARKGREY);
  tft.fillRoundRect(20, 202, 200, 24, 9, TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(String((int)gpsData.speed) + " km/h", 28, 207, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawCentreString(String((int)compassData.heading) + String((char)247), 120, 208, 2);
  tft.setTextColor(deviceConnected ? TFT_CYAN : TFT_DARKGREY, TFT_BLACK);
  tft.drawRightString(deviceConnected ? "BLE OK" : "BLE --", 220, 208, 2);
}

void setup()
{
  pixels.begin();
  pixels.setPixelColor(0, pixels.Color(0, 0, 0));
  pixels.show();

  Serial.begin(115200);
  delay(500);

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

  // HMC5883L init attempt (non-blocking)
  if (mag.begin())
  {
    Serial.println("✓ HMC5883L detected - using real compass");
    tft.drawCentreString("Compass OK", 120, 130, 2);
  }
  else
  {
    Serial.println("⚠ HMC5883L not responding - using simulated heading");
    Serial.println("  Check: GPIO2(SDA), GPIO1(SCL), GND, 3.3V");
    Serial.println("  Add 10k pull-up resistors if needed");
    tft.drawCentreString("No compass (sim)", 120, 130, 1);
  }

  // Init BLE
  BLEDevice::init("GPS_Tracker_BLE");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_TX,
      BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_RX,
      BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setCallbacks(new MyCharacteristicCallbacks());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x0);
  BLEDevice::startAdvertising();

  Serial.println("✓ BLE started as 'GPS_Tracker_BLE'");

  delay(2000);
  Serial.println("Setup complete! Awaiting GPS data via BLE...\n");
}

void loop()
{
  // BLE data already handled in callback
  updateCompass();
  drawEgocentricMapView();
  delay(100);
}

void updateCompass()
{
  // Try real compass, fallback to simulated
  sensors_event_t event;
  static bool useRealCompass = true;
  static int skipErrorCount = 0;
  
  if (useRealCompass)
  {
    // Try reading from HMC5883L (but don't spam errors)
    if (mag.getEvent(&event))
    {
      if (abs(event.magnetic.x) > 0.5 || abs(event.magnetic.y) > 0.5)
      {
        float h = atan2(event.magnetic.y, event.magnetic.x);
        if (h < 0) h += 2 * PI;
        compassData.heading = h * 180 / M_PI;
        skipErrorCount = 0;
        return;
      }
    }
    
    skipErrorCount++;
    if (skipErrorCount > 5)
    {
      // Sensor not responding, switch to simulated
      useRealCompass = false;
      Serial.println("Switching to simulated compass (sensor not responding)");
    }
  }
  
  // Simulated compass (rotate slowly)
  compassData.heading += 0.3;
  if (compassData.heading >= 360)
    compassData.heading = 0;
}
