// Rive audio on the board: Rive's audio engine (miniaudio, run as an
// "external" engine) mixes the sounds that audio events start; a task pulls
// the mix in small blocks and plays it through I2S -> ES8311 codec ->
// NS4150B amplifier -> the 2-pin speaker header.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Creates Rive's audio engine (must happen before a .riv is loaded, so its
// audio events find it), sets up the codec and starts the audio task on
// `core`. Without a working codec the engine still runs and its output is
// only measured, so sounds can be seen in the log.
esp_err_t audio_start(int core);

// The engine's format, for RivePlayer::prepareAudio().
uint32_t audio_channels(void);
uint32_t audio_sample_rate(void);

typedef struct
{
  float peak;        // highest |sample| since the last call (0..1)
  float mixMs;       // average time to mix one block
  float cpuPercent;  // mixing time / audio time
  uint32_t blocks;   // blocks played since the last call
  bool codec;        // false: no codec, output is discarded
  uint32_t stackFree; // audio task stack never used so far (bytes)
  uint32_t stackSize; // audio task stack size (bytes)
} audio_stats_t;

// Stats since the previous call.
audio_stats_t audio_take_stats(void);
