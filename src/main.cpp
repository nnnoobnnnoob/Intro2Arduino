#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_VL53L0X.h>
#include <Audio.h>
#include <WiFi.h>
#include <math.h>

#include "secrets.h"

constexpr uint8_t BUTTON_PIN = 47;
constexpr uint8_t RING1_PIN = 4;
constexpr uint8_t RING2_PIN = 5;
constexpr uint8_t I2C_SDA_PIN = 8;
constexpr uint8_t I2C_SCL_PIN = 9;
constexpr uint8_t I2S_DIN_PIN = 12;
constexpr uint8_t I2S_BCLK_PIN = 13;
constexpr uint8_t I2S_LRC_PIN = 14;
constexpr uint8_t ONBOARD_LED_PIN = 48;
constexpr uint16_t RING_PIXEL_COUNT = 12;
constexpr uint32_t RING_STEP_MS = 120;
constexpr uint32_t SENSOR_INTERVAL_MS = 250;
constexpr uint32_t ONBOARD_COLOR_INTERVAL_MS = 1000;
constexpr uint16_t DEBOUNCE_MS = 50;
constexpr float DISTANCE_NEAR_M = 0.10f;
constexpr float DISTANCE_FAR_M = 0.30f;
constexpr uint8_t DEFAULT_RADIO_VOLUME_PERCENT = 50;

enum class Mode {
  Mode1,
  Mode2,
};

Adafruit_NeoPixel ring1(RING_PIXEL_COUNT, RING1_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel ring2(RING_PIXEL_COUNT, RING2_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel onboardLed(1, ONBOARD_LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_VL53L0X distanceSensor;
Audio audio;

Mode currentMode = Mode::Mode1;
bool sensorReady = false;
bool mode2RadioRunning = false;
bool wifiConnected = false;
bool radioConnected = false;
bool hasValidDistance = false;
float smoothedDistanceM = 0.20f;
uint16_t ring1Position = 0;
uint16_t ring2Position = RING_PIXEL_COUNT - 1;
uint8_t onboardColorIndex = 0;
uint8_t radioVolumePercent = DEFAULT_RADIO_VOLUME_PERCENT;
uint32_t lastRing1Update = 0;
uint32_t lastRing2Update = 0;
uint32_t lastSensorPoll = 0;
uint32_t lastOnboardUpdate = 0;
uint32_t lastWifiAttemptMs = 0;
uint32_t lastVUMeterUpdateMs = 0;

struct OnboardColor {
  const char* name;
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

const OnboardColor onboardColors[] = {
    {"RED", 255, 0, 0},       {"GREEN", 0, 255, 0},
    {"BLUE", 0, 0, 255},      {"YELLOW", 255, 255, 0},
    {"CYAN", 0, 255, 255},    {"MAGENTA", 255, 0, 255},
    {"WHITE", 255, 255, 255}, {"OFF", 0, 0, 0},
};
constexpr size_t ONBOARD_COLOR_COUNT = sizeof(onboardColors) / sizeof(onboardColors[0]);

void drawMode1Ring1() {
  ring1.clear();
  ring1.setPixelColor(ring1Position, ring1.Color(0, 0, 180));
  ring1.show();
}

void drawMode1Ring2() {
  ring2.clear();
  ring2.setPixelColor(ring2Position, ring2.Color(0, 180, 0));
  ring2.show();
}

void setupNeoPixels() {
  ring1.begin();
  ring2.begin();
  onboardLed.begin();
  ring1.setBrightness(48);
  ring2.setBrightness(48);
  onboardLed.setBrightness(32);
  ring1.clear();
  ring2.clear();
  onboardLed.clear();
  drawMode1Ring1();
  drawMode1Ring2();
  Serial.println("NeoPixel rings initialized");

  onboardLed.setPixelColor(
      0, onboardLed.Color(onboardColors[0].red, onboardColors[0].green,
                           onboardColors[0].blue));
  onboardLed.show();
  Serial.println("Onboard WS2812 initialized: RED");
}

void updateMode1Ring1() {
  const uint32_t now = millis();
  if (now - lastRing1Update >= RING_STEP_MS) {
    lastRing1Update = now;
    ring1Position = (ring1Position + 1) % RING_PIXEL_COUNT;
    drawMode1Ring1();
  }
}

void updateMode1Ring2() {
  const uint32_t now = millis();
  if (now - lastRing2Update >= RING_STEP_MS) {
    lastRing2Update = now;
    ring2Position = (ring2Position + RING_PIXEL_COUNT - 1) % RING_PIXEL_COUNT;
    drawMode1Ring2();
  }
}

void setupVL53L0X() {
  if (!Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN)) {
    Serial.println("VL53L0X initialization failed: I2C bus could not start");
    return;
  }

  sensorReady = distanceSensor.begin(0x29, false, &Wire);
  if (!sensorReady) {
    Serial.println("VL53L0X initialization failed: check power, GND, SDA, and SCL");
    return;
  }

  if (!distanceSensor.startRangeContinuous(SENSOR_INTERVAL_MS)) {
    sensorReady = false;
    Serial.println("VL53L0X initialization failed: continuous ranging could not start");
    return;
  }

  lastSensorPoll = millis();
  Serial.println("VL53L0X initialized");
}

void updateVL53L0X() {
  if (!sensorReady) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastSensorPoll < 5) {
    return;
  }
  lastSensorPoll = now;

  if (!distanceSensor.isRangeComplete()) {
    return;
  }

  const uint16_t millimeters = distanceSensor.readRangeResult();
  if (distanceSensor.readRangeStatus() == 0 && millimeters != 0xFFFF) {
    const float distanceM = millimeters / 1000.0f;
    if (!hasValidDistance) {
      smoothedDistanceM = distanceM;
      hasValidDistance = true;
    } else {
      smoothedDistanceM = (smoothedDistanceM * 0.7f) + (distanceM * 0.3f);
    }
    if (currentMode == Mode::Mode2) {
      Serial.print("VL53L0X: ");
      Serial.print(smoothedDistanceM, 3);
      Serial.println(" m");
    }
  }
}

void updateOnboardLED() {
  const uint32_t now = millis();
  if (now - lastOnboardUpdate < ONBOARD_COLOR_INTERVAL_MS) {
    return;
  }
  lastOnboardUpdate = now;
  onboardColorIndex = (onboardColorIndex + 1) % ONBOARD_COLOR_COUNT;

  const OnboardColor& color = onboardColors[onboardColorIndex];
  onboardLed.setPixelColor(0, onboardLed.Color(color.red, color.green, color.blue));
  onboardLed.show();
  Serial.print("Onboard LED: ");
  Serial.println(color.name);
}

void updateModeButton() {
  static int lastRawState = HIGH;
  static uint32_t lastDebounceMs = 0;
  static int debouncedState = HIGH;

  const int rawState = digitalRead(BUTTON_PIN);

  if (rawState != lastRawState) {
    lastRawState = rawState;
    lastDebounceMs = millis();
  }

  if ((millis() - lastDebounceMs) > DEBOUNCE_MS) {
    if (rawState != debouncedState) {
      debouncedState = rawState;
      if (debouncedState == LOW && currentMode == Mode::Mode1) {
        currentMode = Mode::Mode2;
        Serial.println("Mode switch: MODE 1 -> MODE 2");
        ring1.clear();
        ring2.clear();
        ring1.show();
        ring2.show();
      }
    }
  }
}

float mapDistanceToVolumePercent(float distanceM) {
  if (!isfinite(distanceM)) {
    return DEFAULT_RADIO_VOLUME_PERCENT;
  }

  if (distanceM <= DISTANCE_NEAR_M) {
    return 100.0f;
  }

  if (distanceM >= DISTANCE_FAR_M) {
    return 0.0f;
  }

  const float slope = (100.0f - 0.0f) / (DISTANCE_NEAR_M - DISTANCE_FAR_M);
  const float volume = slope * (distanceM - DISTANCE_FAR_M);
  return constrain(volume, 0.0f, 100.0f);
}

void drawVolumeBar(uint8_t volumePercentValue) {
  ring1.clear();

  uint8_t litPixels = (volumePercentValue * RING_PIXEL_COUNT + 99) / 100;
  if (litPixels == 0) {
    litPixels = 1;
  }

  for (uint8_t index = 0; index < RING_PIXEL_COUNT; ++index) {
    if (index < litPixels) {
      ring1.setPixelColor(index, ring1.Color(0, 0, 180));
    }
  }
  ring1.show();
}

void updateVUMeter() {
  static uint8_t vuLevel = 1;
  const uint32_t now = millis();
  if (now - lastVUMeterUpdateMs < 80) {
    return;
  }
  lastVUMeterUpdateMs = now;

  vuLevel = (vuLevel + 1) % (RING_PIXEL_COUNT + 1);
  if (vuLevel == 0) {
    vuLevel = 1;
  }

  ring2.clear();
  for (uint8_t index = 0; index < RING_PIXEL_COUNT; ++index) {
    if (index < vuLevel) {
      ring2.setPixelColor(index, ring2.Color(0, 180, 0));
    }
  }
  ring2.show();
}

void attemptWiFiConnection() {
  if (wifiConnected) {
    return;
  }

  if (lastWifiAttemptMs == 0 || (millis() - lastWifiAttemptMs) >= 5000) {
    lastWifiAttemptMs = millis();
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("Connecting to WiFi...");
  }

  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    return;
  }

  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL ||
      status == WL_DISCONNECTED) {
    Serial.print("WiFi connection failed (status: ");
    Serial.print(status);
    Serial.println("). Retrying...");
  }
}

void startRadioStream() {
  if (radioConnected) {
    return;
  }

  Serial.println("Starting internet radio stream...");
  audio.setPinout(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DIN_PIN);
  audio.setVolume(map(radioVolumePercent, 0, 100, 0, 21));
  audio.connecttohost(RADIO_STREAM_URL);
  radioConnected = true;
}

void updateRadioMode() {
  if (currentMode != Mode::Mode2) {
    return;
  }

  if (!wifiConnected) {
    attemptWiFiConnection();
    return;
  }

  if (!radioConnected) {
    startRadioStream();
    return;
  }

  if (hasValidDistance) {
    const float computedVolume = mapDistanceToVolumePercent(smoothedDistanceM);
    radioVolumePercent = static_cast<uint8_t>(constrain(computedVolume, 0.0f, 100.0f));
  } else {
    radioVolumePercent = DEFAULT_RADIO_VOLUME_PERCENT;
  }

  drawVolumeBar(radioVolumePercent);
  updateVUMeter();
  audio.setVolume(map(radioVolumePercent, 0, 100, 0, 21));
  audio.loop();
}

void setup() {
  Serial.begin(115200);
  Serial.println("========================================");
  Serial.println("FREENOVE ESP32-S3 HARDWARE TEST");
  Serial.println("========================================");
  Serial.println("NeoPixel Ring 1: GPIO 4");
  Serial.println("NeoPixel Ring 2: GPIO 5");
  Serial.println("VL53L0X SDA: GPIO 8");
  Serial.println("VL53L0X SCL: GPIO 9");
  Serial.println("MAX98357A DIN: GPIO 12");
  Serial.println("MAX98357A BCLK: GPIO 13");
  Serial.println("MAX98357A LRC: GPIO 14");
  Serial.println("Onboard WS2812: GPIO 48");
  Serial.println("========================================");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  setupNeoPixels();
  setupVL53L0X();

  const uint32_t now = millis();
  lastRing1Update = now;
  lastRing2Update = now;
  lastOnboardUpdate = now;
}

void loop() {
  updateModeButton();
  updateVL53L0X();

  if (currentMode == Mode::Mode1) {
    updateMode1Ring1();
    updateMode1Ring2();
    updateOnboardLED();
    return;
  }

  updateRadioMode();
}