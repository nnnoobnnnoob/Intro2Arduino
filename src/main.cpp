#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_VL53L0X.h>
#include <ESP_I2S.h>
#include <math.h>

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
constexpr uint32_t AUDIO_SAMPLE_RATE = 44100;
constexpr uint32_t AUDIO_SAMPLE_COUNT = AUDIO_SAMPLE_RATE;
constexpr uint32_t AUDIO_DURATION_US = 1000000;
constexpr float TONE_FREQUENCY_HZ = 1000.0f;

Adafruit_NeoPixel ring1(RING_PIXEL_COUNT, RING1_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel ring2(RING_PIXEL_COUNT, RING2_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel onboardLed(1, ONBOARD_LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_VL53L0X distanceSensor;
I2SClass i2s;

bool sensorReady = false;
bool i2sReady = false;
uint16_t ring1Position = 0;
uint16_t ring2Position = RING_PIXEL_COUNT - 1;
uint8_t onboardColorIndex = 0;
uint32_t lastRing1Update = 0;
uint32_t lastRing2Update = 0;
uint32_t lastSensorPoll = 0;
uint32_t lastOnboardUpdate = 0;

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

void drawRing1() {
  ring1.clear();
  ring1.setPixelColor(ring1Position, ring1.Color(0, 0, 180));
  ring1.show();
}

void drawRing2() {
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
  drawRing1();
  drawRing2();
  Serial.println("NeoPixel rings initialized");

  onboardLed.setPixelColor(
      0, onboardLed.Color(onboardColors[0].red, onboardColors[0].green,
                           onboardColors[0].blue));
  onboardLed.show();
  Serial.println("Onboard WS2812 initialized: RED");
}

void updateRing1() {
  const uint32_t now = millis();
  if (now - lastRing1Update >= RING_STEP_MS) {
    lastRing1Update = now;
    ring1Position = (ring1Position + 1) % RING_PIXEL_COUNT;
    drawRing1();
  }
}

void updateRing2() {
  const uint32_t now = millis();
  if (now - lastRing2Update >= RING_STEP_MS) {
    lastRing2Update = now;
    ring2Position = (ring2Position + RING_PIXEL_COUNT - 1) % RING_PIXEL_COUNT;
    drawRing2();
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
    Serial.print("VL53L0X: ");
    Serial.print(millimeters / 1000.0f, 3);
    Serial.println(" m");
  } else {
    Serial.print("VL53L0X: invalid reading (range status ");
    Serial.print(distanceSensor.readRangeStatus());
    Serial.println(")");
  }
}

void setupI2S() {
  i2s.setPins(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DIN_PIN, -1);
  i2sReady = i2s.begin(I2S_MODE_STD, AUDIO_SAMPLE_RATE,
                       I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO,
                       I2S_STD_SLOT_BOTH);
  if (i2sReady) {
    Serial.println("MAX98357A I2S initialized");
  } else {
    Serial.println("MAX98357A I2S initialization failed");
  }
}

void stopTone() {
  i2s.end();
  Serial.println("Audio test complete");
}

void audioTestTask(void* parameter) {
  (void)parameter;
  constexpr size_t BUFFER_SAMPLES = 256;
  int16_t samples[BUFFER_SAMPLES];
  uint32_t sampleIndex = 0;
  const uint32_t toneStartUs = micros();

  while (sampleIndex < AUDIO_SAMPLE_COUNT) {
    const size_t samplesInBuffer =
        min(BUFFER_SAMPLES, static_cast<size_t>(AUDIO_SAMPLE_COUNT - sampleIndex));
    for (size_t index = 0; index < samplesInBuffer; ++index) {
      const float phase = 2.0f * PI * TONE_FREQUENCY_HZ * sampleIndex / AUDIO_SAMPLE_RATE;
      samples[index] = static_cast<int16_t>(sinf(phase) * 12000.0f);
      ++sampleIndex;
    }

    const size_t bytesToWrite = samplesInBuffer * sizeof(samples[0]);
    size_t bytesWritten = 0;
    while (bytesWritten < bytesToWrite) {
      const size_t written = i2s.write(
          reinterpret_cast<const uint8_t*>(samples) + bytesWritten,
          bytesToWrite - bytesWritten);
      if (written == 0) {
        if (i2s.lastError() != 0) {
          Serial.println("Audio test failed while writing I2S samples");
          stopTone();
          vTaskDelete(nullptr);
          return;
        }
        vTaskDelay(1);
      } else {
        bytesWritten += written;
      }
    }
  }

  while (static_cast<uint32_t>(micros() - toneStartUs) < AUDIO_DURATION_US) {
    const uint32_t elapsedUs = static_cast<uint32_t>(micros() - toneStartUs);
    const uint32_t remainingUs = AUDIO_DURATION_US - elapsedUs;
    if (remainingUs > 2000) {
      vTaskDelay(pdMS_TO_TICKS((remainingUs - 1000) / 1000));
    } else {
      delayMicroseconds(remainingUs);
    }
  }

  stopTone();
  vTaskDelete(nullptr);
}

void startTone() {
  Serial.println("Starting 1 kHz audio test");
  if (!i2sReady) {
    Serial.println("Audio test skipped: I2S initialization failed");
    Serial.println("Audio test complete");
    return;
  }

  if (xTaskCreate(audioTestTask, "audioTest", 4096, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("Audio test failed: could not create audio task");
    stopTone();
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

  setupNeoPixels();
  setupVL53L0X();
  setupI2S();

  const uint32_t now = millis();
  lastRing1Update = now;
  lastRing2Update = now;
  lastOnboardUpdate = now;
  startTone();
}

void loop() {
  updateRing1();
  updateRing2();
  updateVL53L0X();
  updateOnboardLED();
}