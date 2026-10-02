#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_VL53L0X.h>
#include <Audio.h>
#include <WiFi.h>
#include <math.h>

#include "ScreamPlayer.h"
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
constexpr uint32_t SENSOR_INTERVAL_MS = 250;
constexpr uint32_t ONBOARD_COLOR_INTERVAL_MS = 1000;
constexpr uint32_t WIFI_FAILURE_GRACE_MS = 3000;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
constexpr uint32_t DEFAULT_RAINBOW_STEP_MS = 100;
constexpr uint32_t FAST_RAINBOW_STEP_MS = 5;
constexpr uint8_t RAINBOW_HUE_STEP = 8;
constexpr uint32_t VU_UPDATE_INTERVAL_MS = 30;
constexpr uint32_t VU_DECAY_INTERVAL_MS = 60;
constexpr uint16_t DEBOUNCE_MS = 50;
constexpr float DISTANCE_NEAR_M = 0.10f;
constexpr float DISTANCE_FAR_M = 0.30f;
constexpr float RAINBOW_FAST_DISTANCE_M = 0.20f;
constexpr uint8_t DEFAULT_RADIO_VOLUME_PERCENT = 50;
constexpr uint16_t VU_THRESHOLDS[RING_PIXEL_COUNT] = {
    300, 500, 800, 1200, 1800, 2600, 3800, 5500, 8000, 11500, 16500, 23000};

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
bool usingBackupWiFi = false;
bool wifiAttemptInProgress = false;
bool radioConnected = false;
bool hasValidDistance = false;
bool screamTriggerArmed = true;
float currentDistanceM = 0.0f;
float smoothedDistanceM = 0.20f;
uint8_t ring1HueOffset = 0;
uint8_t ring2HueOffset = 0;
uint8_t onboardColorIndex = 0;
uint8_t radioVolumePercent = DEFAULT_RADIO_VOLUME_PERCENT;
uint32_t lastRainbowUpdateMs = 0;
uint32_t lastScreamFlashUpdateMs = 0;
uint32_t lastSensorPoll = 0;
uint32_t lastOnboardUpdate = 0;
uint32_t wifiAttemptStartedMs = 0;
uint32_t nextWifiAttemptMs = 0;
uint32_t lastVUMeterUpdateMs = 0;
uint32_t lastVUDecayMs = 0;
uint16_t lastReportedDistanceMm = 0xFFFF;
volatile uint16_t pcmPeakAmplitude = 0;
uint8_t vuLitPixels = 0;
bool screamFlashOn = false;

void audio_process_i2s(uint32_t* sample, bool* continueI2S) {
  if (continueI2S != nullptr) {
    *continueI2S = true;
  }
  if (sample == nullptr) {
    return;
  }

  const int16_t left = static_cast<int16_t>(*sample >> 16);
  const int16_t right = static_cast<int16_t>(*sample & 0xFFFF);
  const uint16_t leftAmplitude =
      static_cast<uint16_t>(abs(static_cast<int32_t>(left)));
  const uint16_t rightAmplitude =
      static_cast<uint16_t>(abs(static_cast<int32_t>(right)));
  const uint16_t amplitude = max(leftAmplitude, rightAmplitude);
  if (amplitude > pcmPeakAmplitude) {
    pcmPeakAmplitude = amplitude;
  }
}

uint32_t rainbowColor(uint8_t hue) {
  if (hue < 85) {
    return ring1.Color(255 - hue * 3, hue * 3, 0);
  }
  if (hue < 170) {
    hue -= 85;
    return ring1.Color(0, 255 - hue * 3, hue * 3);
  }
  hue -= 170;
  return ring1.Color(hue * 3, 0, 255 - hue * 3);
}

uint32_t rainbowStepIntervalMs() {
  if (!screamTriggerArmed || !hasValidDistance ||
      smoothedDistanceM >= DISTANCE_FAR_M) {
    return DEFAULT_RAINBOW_STEP_MS;
  }
  if (smoothedDistanceM <= RAINBOW_FAST_DISTANCE_M) {
    return FAST_RAINBOW_STEP_MS;
  }

  const float proportion =
      (smoothedDistanceM - RAINBOW_FAST_DISTANCE_M) /
      (DISTANCE_FAR_M - RAINBOW_FAST_DISTANCE_M);
  return FAST_RAINBOW_STEP_MS +
         static_cast<uint32_t>(proportion *
                               (DEFAULT_RAINBOW_STEP_MS -
                                FAST_RAINBOW_STEP_MS));
}

void drawRainbowRing(Adafruit_NeoPixel& ring, uint8_t hueOffset,
                     bool reverseDirection) {
  for (uint8_t index = 0; index < RING_PIXEL_COUNT; ++index) {
    const uint8_t pixelHue = reverseDirection
                                 ? hueOffset - index * 256 / RING_PIXEL_COUNT
                                 : hueOffset + index * 256 / RING_PIXEL_COUNT;
    ring.setPixelColor(index, rainbowColor(pixelHue));
  }
  ring.show();
}

void drawScreamFlash(bool on) {
  const uint32_t color = on ? ring1.Color(255, 0, 0) : 0;
  for (uint8_t index = 0; index < RING_PIXEL_COUNT; ++index) {
    ring1.setPixelColor(index, color);
    ring2.setPixelColor(index, color);
  }
  ring1.show();
  ring2.show();
}

void updateMode1Rings() {
  const uint32_t now = millis();
  if (isScreamPlaybackRunning()) {
    if (now - lastScreamFlashUpdateMs >= 100) {
      lastScreamFlashUpdateMs = now;
      screamFlashOn = !screamFlashOn;
      drawScreamFlash(screamFlashOn);
    }
    return;
  }

  if (screamFlashOn) {
    screamFlashOn = false;
    drawRainbowRing(ring1, ring1HueOffset, false);
    drawRainbowRing(ring2, ring2HueOffset, true);
    lastRainbowUpdateMs = now;
  }

  if (now - lastRainbowUpdateMs < rainbowStepIntervalMs()) {
    return;
  }
  lastRainbowUpdateMs = now;
  ring1HueOffset += RAINBOW_HUE_STEP;
  ring2HueOffset -= RAINBOW_HUE_STEP;
  drawRainbowRing(ring1, ring1HueOffset, false);
  drawRainbowRing(ring2, ring2HueOffset, true);
}

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
  drawRainbowRing(ring1, ring1HueOffset, false);
  drawRainbowRing(ring2, ring2HueOffset, true);
  Serial.println("NeoPixel rings initialized");

  onboardLed.setPixelColor(
      0, onboardLed.Color(onboardColors[0].red, onboardColors[0].green,
                           onboardColors[0].blue));
  onboardLed.show();
  Serial.println("Onboard WS2812 initialized: RED");
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
    currentDistanceM = distanceM;
    if (millimeters != lastReportedDistanceMm) {
      lastReportedDistanceMm = millimeters;
      Serial.print("Distance: ");
      Serial.print(distanceM, 3);
      Serial.print(" m (");
      Serial.print(millimeters);
      Serial.println(" mm)");
    }

    if (!hasValidDistance) {
      smoothedDistanceM = distanceM;
      hasValidDistance = true;
    } else {
      smoothedDistanceM = (smoothedDistanceM * 0.7f) + (distanceM * 0.3f);
    }
  }
}

void updateScreamPlayback() {
  if (!hasValidDistance || currentDistanceM >= DISTANCE_NEAR_M) {
    screamTriggerArmed = true;
    return;
  }

  if (!screamTriggerArmed || isScreamPlaybackRunning()) {
    return;
  }

  screamTriggerArmed = false;
  if (startScreamPlayback(audio)) {
    Serial.println("Playing proximity scream");
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
  const uint32_t now = millis();
  if (now - lastVUMeterUpdateMs < VU_UPDATE_INTERVAL_MS) {
    return;
  }
  lastVUMeterUpdateMs = now;

  const uint16_t peakAmplitude = pcmPeakAmplitude;
  pcmPeakAmplitude = 0;

  uint8_t detectedPixels = 0;
  for (uint8_t index = 0; index < RING_PIXEL_COUNT; ++index) {
    if (peakAmplitude >= VU_THRESHOLDS[index]) {
      detectedPixels = index + 1;
    }
  }

  if (detectedPixels > vuLitPixels) {
    vuLitPixels = detectedPixels;
    lastVUDecayMs = now;
  } else if (vuLitPixels > 0 && now - lastVUDecayMs >= VU_DECAY_INTERVAL_MS) {
    --vuLitPixels;
    lastVUDecayMs = now;
  }

  ring2.clear();
  for (uint8_t index = 0; index < RING_PIXEL_COUNT; ++index) {
    if (index < vuLitPixels) {
      if (index < 8) {
        ring2.setPixelColor(index, ring2.Color(0, 180, 0));
      } else {
        ring2.setPixelColor(index, ring2.Color(180, 0, 0));
      }
    }
  }
  ring2.show();
}

void attemptWiFiConnection() {
  if (wifiConnected) {
    return;
  }

  const uint32_t now = millis();
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    wifiConnected = true;
    wifiAttemptInProgress = false;
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    return;
  }

  if (wifiAttemptInProgress) {
    const uint32_t attemptDuration = now - wifiAttemptStartedMs;
    const bool reportedFailure =
        status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL;
    if ((reportedFailure && attemptDuration >= WIFI_FAILURE_GRACE_MS) ||
        attemptDuration >= WIFI_CONNECT_TIMEOUT_MS) {
      wifiAttemptInProgress = false;
      WiFi.disconnect();

      if (!usingBackupWiFi) {
        usingBackupWiFi = true;
        nextWifiAttemptMs = now;
        Serial.println("Primary WiFi unavailable; trying backup credentials");
      } else {
        nextWifiAttemptMs = now + WIFI_RETRY_INTERVAL_MS;
        Serial.println("Backup WiFi unavailable; will retry");
      }
    }
    return;
  }

  if (static_cast<int32_t>(now - nextWifiAttemptMs) < 0) {
    return;
  }

  if (!usingBackupWiFi &&
      (WIFI_SSID[0] == '\0' || WIFI_PASSWORD[0] == '\0')) {
    usingBackupWiFi = true;
    Serial.println("Primary WiFi credentials empty; using backup credentials");
  }

  WiFi.mode(WIFI_STA);
  if (usingBackupWiFi) {
    WiFi.begin(BACKUP_WIFI_SSID, BACKUP_WIFI_PASSWORD);
    Serial.println("Connecting to backup WiFi...");
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("Connecting to primary WiFi...");
  }
  wifiAttemptStartedMs = now;
  wifiAttemptInProgress = true;
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

  if (isScreamPlaybackRunning()) {
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
  audio.setVolume(map(radioVolumePercent, 0, 100, 0, 21));
  audio.loop();
  updateVUMeter();
}

void setup() {
  Serial.begin(115200);
  Serial.println("========================================");
  Serial.println("FREENOVE ESP32-S3 MAIN OPERATION");
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
  lastRainbowUpdateMs = now;
  lastOnboardUpdate = now;
}

void loop() {
  updateModeButton();
  updateVL53L0X();

  if (currentMode == Mode::Mode1) {
    updateMode1Rings();
    updateScreamPlayback();
    updateOnboardLED();
    return;
  }

  updateRadioMode();
}