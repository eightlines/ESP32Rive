// Goodix GT911 touch controller on the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B.
//
// The board wires the GT911 to the shared I2C bus (board_i2c.h) without its INT
// or RESET lines, so the controller is polled. A small task reads the first
// touch point and turns it into down / move / up events in panel pixels
// (0..719), which the render loop drains with touch_next_event().
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  TOUCH_DOWN,
  TOUCH_MOVE,
  TOUCH_UP,
} touch_event_type_t;

typedef struct
{
  touch_event_type_t type;
  uint16_t x; // panel pixels
  uint16_t y;
} touch_event_t;

// Probes the GT911 (I2C address 0x5D, then 0x14), reads its configured
// resolution and starts the polling task (pinned to `core`, every
// `period_ms`). Returns an error if no controller answers; the rest of the
// app can carry on without touch.
esp_err_t touch_gt911_start(int core, uint32_t period_ms);

// Polling counters since the previous call (for the stats log): polls,
// failed reads, samples the controller marked ready, and of those, samples
// with a finger down.
typedef struct
{
  uint32_t polls;
  uint32_t errors;
  uint32_t ready;
  uint32_t points;
  esp_err_t last_error;
} touch_stats_t;
void touch_take_stats(touch_stats_t* stats);

// Next queued touch event, without waiting. False when there is none.
bool touch_next_event(touch_event_t* event);

#ifdef __cplusplus
}
#endif
