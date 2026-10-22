#pragma once

#include <stdint.h>

#include <Audio.h>

bool startScreamPlayback(Audio& audio);
bool isScreamPlaybackRunning();
uint8_t screamPlaybackLevel();
