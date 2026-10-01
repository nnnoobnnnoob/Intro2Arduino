#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

constexpr uint8_t RGB_LED_PIN = 48;
constexpr uint16_t PIXEL_COUNT = 1;
Adafruit_NeoPixel rgbLed(PIXEL_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

uint32_t colorWheel(uint8_t position) {
  position = 255 - position;
  if (position < 85) {
    return rgbLed.Color(255 - position * 3, 0, position * 3);
  }
  if (position < 170) {
    position -= 85;
    return rgbLed.Color(0, position * 3, 255 - position * 3);
  }
  position -= 170;
  return rgbLed.Color(position * 3, 255 - position * 3, 0);
}

void setup() {
  rgbLed.begin();
  rgbLed.setBrightness(32);
  rgbLed.show();
}

void loop() {
  static uint8_t hue = 0;
  rgbLed.setPixelColor(0, colorWheel(hue++));
  rgbLed.show();
  delay(10);
}