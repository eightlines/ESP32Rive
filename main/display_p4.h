// Waveshare ESP32-P4-WIFI6-Touch-LCD-4B display: 720x720 ST7703 over 2-lane
// MIPI-DSI, RGB565, two frame buffers in PSRAM.
//
// Frames are rendered elsewhere into an ARGB8888 buffer and handed to
// display_present(), which uses the PPA (pixel-processing accelerator) to
// convert (and optionally upscale) into the back frame buffer, then flips.
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_WIDTH 720
#define DISPLAY_HEIGHT 720

// Powers the DSI PHY, initialises the ST7703 panel, PPA and backlight.
esp_err_t display_init(int backlight_percent);

// Frame protocol (three panel frame buffers, round robin):
//   display_begin_frame();                        // pick a free target buffer
//   display_blit_async(frame, w, h, y0, rows);    // PPA copy, in background
//   ... draw the next frame meanwhile ...
//   display_end_frame();                          // wait for copy, flip
//
// `frame` is premultiplied ARGB8888, src_w x src_h, stride src_w. src_w must
// divide DISPLAY_WIDTH (720 = 1:1, 360 = upscaled 2x by the PPA); y0 and rows
// are in source pixels. The target buffer holds the frame shown three flips
// ago (DISPLAY_FB_AGE), so callers copy every row that changed since then.

#define DISPLAY_FB_AGE 3

// Picks the target buffer (waits only if it could still be on screen).
void display_begin_frame(void);
// Starts converting rows [src_y0, src_y0 + rows) to RGB565 into the target.
// `frame` must not be modified until display_end_frame() returns.
esp_err_t display_blit_async(const uint32_t *frame, int src_w, int src_h, int src_y0, int rows);
// Waits for the copy started by display_blit_async(), if any.
void display_wait_blit(void);
// Waits for the copy, then shows the target from the next refresh on.
esp_err_t display_end_frame(void);
// How long the last completed PPA copy took (ms), measured from its start to
// its completion interrupt; 0 if unknown.
float display_last_copy_ms(void);

// DMA burst length of the PPA (8, 16, 32, 64 or 128 bytes; default 128).
// Shorter bursts slow the copy but may leave more memory bandwidth to the
// CPUs while it runs in the background. Waits for a running copy first.
esp_err_t display_set_ppa_burst(int bytes);
int display_ppa_burst(void);

// Whole-frame convenience: begin + one blit + end.
esp_err_t display_present(const uint32_t *argb, int src_w, int src_h);

esp_err_t display_set_backlight(int percent);

#ifdef __cplusplus
}
#endif
