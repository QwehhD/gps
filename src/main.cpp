#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Adafruit_NeoPixel.h>

TFT_eSPI tft = TFT_eSPI();
Adafruit_NeoPixel pixels(1, 48, NEO_GRB + NEO_KHZ800);

void setup()
{
  pixels.begin();
  pixels.setPixelColor(0, pixels.Color(0, 0, 0));
  pixels.show();

  Serial.begin(115200);
  delay(500);

  Serial.println("\n=== ESP32 S3 GC9A01 START ===");
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());

  Serial.println("Initializing display...");
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawCentreString("ESP32 S3", 120, 100, 4);
  tft.drawCentreString("OK!", 120, 140, 4);
  
  Serial.println("Setup complete!");
}

void loop()
{
  // Simple display test
  tft.fillScreen(TFT_BLACK);
  
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(4);
  
  static int count = 0;
  tft.drawCentreString("Test", 120, 80, 4);
  tft.drawCentreString(String(count++).c_str(), 120, 140, 4);
  
  delay(1000);
}
