#include <Arduino.h>

#include "ScreamPlayer.h"
//#include "scream.h"
#include "sweep.h"

namespace {

constexpr int8_t I2S_DIN_PIN = 12;
constexpr int8_t I2S_BCLK_PIN = 13;
constexpr int8_t I2S_LRC_PIN = 14;
constexpr uint32_t SCREAM_SAMPLE_RATE = 16000;
constexpr size_t FRAMES_PER_BUFFER = 256;
constexpr uint32_t I2S_DRAIN_DELAY_MS = 100;

volatile bool playbackRunning = false;
Audio* activeAudio = nullptr;

void screamPlaybackTask(void* parameter) {
  (void)parameter;
  bool succeeded = false;
  uint32_t stereoFrames[FRAMES_PER_BUFFER];
  Audio* const audio = activeAudio;

  if (audio == nullptr ||
      !audio->setPinout(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DIN_PIN) ||
      i2s_set_clk(static_cast<i2s_port_t>(audio->getI2sPort()),
                  SCREAM_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT,
                  I2S_CHANNEL_STEREO) != ESP_OK ||
      i2s_zero_dma_buffer(static_cast<i2s_port_t>(audio->getI2sPort())) !=
          ESP_OK) {
    Serial.println("Scream playback failed: legacy I2S configuration failed");
  } else {
    succeeded = true;
    size_t sampleIndex = 0;
    while (sampleIndex < audioDataLength && succeeded) {
      const size_t remainingSamples = audioDataLength - sampleIndex;
      const size_t frames = remainingSamples < FRAMES_PER_BUFFER
                                ? remainingSamples
                                : FRAMES_PER_BUFFER;
      for (size_t frame = 0; frame < frames; ++frame) {
        const uint16_t sample =
            static_cast<uint16_t>(audioData[sampleIndex + frame]);
        stereoFrames[frame] =
            (static_cast<uint32_t>(sample) << 16) | sample;
      }

      const size_t bytesToWrite = frames * sizeof(stereoFrames[0]);
      size_t bytesWritten = 0;
      while (bytesWritten < bytesToWrite) {
        size_t written = 0;
        const esp_err_t result =
            i2s_write(static_cast<i2s_port_t>(audio->getI2sPort()),
                      reinterpret_cast<const char*>(stereoFrames) + bytesWritten,
                      bytesToWrite - bytesWritten, &written, 100);
        if (result != ESP_OK) {
            Serial.println("Scream playback failed while writing I2S samples");
            succeeded = false;
        } else if (written == 0) {
          vTaskDelay(1);
        } else {
          bytesWritten += written;
        }
      }
      sampleIndex += frames;
    }

    if (succeeded) {
      const i2s_port_t port = static_cast<i2s_port_t>(audio->getI2sPort());
      vTaskDelay(pdMS_TO_TICKS(I2S_DRAIN_DELAY_MS));
      if (i2s_zero_dma_buffer(port) != ESP_OK) {
        Serial.println("Scream playback failed: could not clear I2S DMA buffer");
        succeeded = false;
      }
    }
  }

  playbackRunning = false;
  activeAudio = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

bool startScreamPlayback(Audio& audio) {
  if (playbackRunning) {
    return false;
  }

  activeAudio = &audio;
  playbackRunning = true;
  if (xTaskCreate(screamPlaybackTask, "screamPlayback", 4096, nullptr, 1,
                  nullptr) != pdPASS) {
    playbackRunning = false;
    activeAudio = nullptr;
    Serial.println("Scream playback failed: could not create audio task");
    return false;
  }
  return true;
}

bool isScreamPlaybackRunning() {
  return playbackRunning;
}
