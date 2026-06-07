/*
 * clawd-mood — ESP32-C3 Super Mini + ST7789 1.54" 240x240
 *
 * Pins (same as clawd-mochi):
 *   SDA=GPIO10  SCL=GPIO8  RST=GPIO2  DC=GPIO1  CS=GPIO4  BL=GPIO3
 *   VCC=3V3  GND=GND
 *
 * Arduino IDE: ESP32C3 Dev Module, USB CDC On Boot: Enabled, 160 MHz, 921600.
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

#define TFT_CS  4
#define TFT_DC  1
#define TFT_RST 2
#define TFT_BLK 3

#define DISP_W 240
#define DISP_H 240

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);

  tft.init(DISP_W, DISP_H);
  tft.setRotation(2);
  tft.fillScreen(ST77XX_BLACK);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(3);
  tft.setCursor(60, 100);
  tft.print("READY");
}

void loop() {
  delay(1000);
}
