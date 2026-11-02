// Boot self-test and benchmark for the PIE (ESP32-P4 SIMD) gradient span in
// components/thorvg/fast/sw_fill_fast.inc.
//
// The PIE code can't be run anywhere but on the chip, so before it is used
// it must reproduce ThorVG's own loop exactly, on random tables, positions,
// lengths and alignments. Only then is tvgPieEnabled set. The timings show
// what each version costs per pixel.
#include "pie_selftest.h"

#include "sdkconfig.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
extern bool tvgPieEnabled;
void tvgPreNormalSpanRef(uint32_t* dst, const uint32_t* ctable, int32_t t2,
                         int32_t inc2, uint32_t len);
void tvgPreNormalSpanC(uint32_t* dst, const uint32_t* ctable, int32_t t2,
                       int32_t inc2, uint32_t len);
#if CONFIG_RIVE_THORVG_FAST_GRADIENT
void tvgPreNormalSpanPie(uint32_t* dst, const uint32_t* ctable, int32_t t2,
                         int32_t inc2, uint32_t len);
#endif
}

namespace
{
const char* TAG = "pie";

// Test data comes from xorshift32, seeded once from the hardware RNG:
// esp_random() waits on the entropy source on every call (tens of us on the
// P4), far too slow for the ~700k values the test needs.
uint32_t g_rng = 1;
uint32_t fastRandom()
{
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

// A premultiplied ARGB pixel: every colour channel <= alpha.
uint32_t randomPremultiplied()
{
  uint32_t r = fastRandom();
  uint32_t a = r >> 24;
  uint32_t c0 = (r & 0xFF) * a / 255;
  uint32_t c1 = ((r >> 8) & 0xFF) * a / 255;
  uint32_t c2 = ((r >> 16) & 0xFF) * a / 255;
  return (a << 24) | (c2 << 16) | (c1 << 8) | c0;
}

using SpanFn = void (*)(uint32_t*, const uint32_t*, int32_t, int32_t, uint32_t);

// Cycles per pixel for one 720-pixel span, best of a few runs.
float cyclesPerPixel(SpanFn fn, uint32_t* dst, const uint32_t* table)
{
  constexpr uint32_t kLen = 720;
  uint32_t best = UINT32_MAX;
  for (int run = 0; run < 8; ++run)
  {
    uint32_t t0 = esp_cpu_get_cycle_count();
    fn(dst, table, 0, (1023 << 8) / kLen, kLen);
    uint32_t c = esp_cpu_get_cycle_count() - t0;
    if (c < best)
    {
      best = c;
    }
  }
  return float(best) / float(kLen);
}
} // namespace

void pie_selftest_run()
{
#if CONFIG_RIVE_THORVG_FAST_GRADIENT && CONFIG_RIVE_PIE_GRADIENT
  constexpr uint32_t kMax = 800;
  auto* table = static_cast<uint32_t*>(
      heap_caps_aligned_alloc(16, 1024 * 4, MALLOC_CAP_DEFAULT));
  auto* base = static_cast<uint32_t*>(
      heap_caps_aligned_alloc(16, (kMax + 8) * 4, MALLOC_CAP_DEFAULT));
  auto* want = static_cast<uint32_t*>(
      heap_caps_aligned_alloc(16, (kMax + 8) * 4, MALLOC_CAP_DEFAULT));
  auto* gotC = static_cast<uint32_t*>(
      heap_caps_aligned_alloc(16, (kMax + 8) * 4, MALLOC_CAP_DEFAULT));
  auto* gotPie = static_cast<uint32_t*>(
      heap_caps_aligned_alloc(16, (kMax + 8) * 4, MALLOC_CAP_DEFAULT));
  if (!table || !base || !want || !gotC || !gotPie)
  {
    ESP_LOGE(TAG, "self-test: out of memory, PIE gradient spans stay off");
    heap_caps_free(table);
    heap_caps_free(base);
    heap_caps_free(want);
    heap_caps_free(gotC);
    heap_caps_free(gotPie);
    return;
  }

  g_rng = esp_random() | 1; // xorshift state must be non-zero
  uint32_t badC = 0, badPie = 0, firstBad = 0, firstWant = 0, firstGot = 0;
  constexpr int kTrials = 400;
  for (int trial = 0; trial < kTrials; ++trial)
  {
    if ((trial & 31) == 31)
    {
      vTaskDelay(1); // let the idle task run (task watchdog)
    }
    for (int i = 0; i < 1024; ++i)
    {
      // Mostly translucent, with some fully opaque / transparent entries.
      uint32_t p = randomPremultiplied();
      if ((i & 31) == 0) p |= 0xFF000000;
      if ((i & 63) == 1) p = 0;
      table[i] = p;
    }
    for (uint32_t i = 0; i < kMax + 8; ++i)
    {
      base[i] = randomPremultiplied();
    }
    const uint32_t offset = fastRandom() % 4; // dst alignment
    const uint32_t len = fastRandom() % kMax;
    // Positions from well below to well above the table (clamping), steps
    // of either sign.
    const int32_t t2 = int32_t(fastRandom() % (2048 << 8)) - (512 << 8);
    const int32_t inc2 = int32_t(fastRandom() % 1024) - 512;

    std::memcpy(want, base, (kMax + 8) * 4);
    std::memcpy(gotC, base, (kMax + 8) * 4);
    std::memcpy(gotPie, base, (kMax + 8) * 4);
    tvgPreNormalSpanRef(want + offset, table, t2, inc2, len);
    tvgPreNormalSpanC(gotC + offset, table, t2, inc2, len);
    tvgPreNormalSpanPie(gotPie + offset, table, t2, inc2, len);
    for (uint32_t i = 0; i < kMax + 8; ++i)
    {
      if (gotC[i] != want[i])
      {
        ++badC;
      }
      if (gotPie[i] != want[i])
      {
        if (badPie == 0)
        {
          firstBad = i;
          firstWant = want[i];
          firstGot = gotPie[i];
        }
        ++badPie;
      }
    }
  }

  for (int i = 0; i < 1024; ++i)
  {
    table[i] = randomPremultiplied();
  }
  const float ref = cyclesPerPixel(tvgPreNormalSpanRef, base, table);
  const float c = cyclesPerPixel(tvgPreNormalSpanC, base, table);
  const float pie = cyclesPerPixel(tvgPreNormalSpanPie, base, table);

  tvgPieEnabled = badPie == 0 && badC == 0;
  ESP_LOGI(TAG,
           "translucent gradient span self-test (%d random spans): C %s, PIE "
           "%s | cycles/px: ThorVG loop %.1f, C %.1f, PIE %.1f | PIE spans %s",
           kTrials, badC ? "MISMATCH" : "ok", badPie ? "MISMATCH" : "ok", ref,
           c, pie, tvgPieEnabled ? "ON" : "OFF");
  if (badC)
  {
    ESP_LOGE(TAG, "C span differs from ThorVG's loop in %" PRIu32 " pixels",
             badC);
  }
  if (badPie)
  {
    ESP_LOGW(TAG,
             "PIE span differs in %" PRIu32 " pixels, first at %" PRIu32
             ": want %08" PRIx32 " got %08" PRIx32,
             badPie, firstBad, firstWant, firstGot);
  }
  heap_caps_free(table);
  heap_caps_free(base);
  heap_caps_free(want);
  heap_caps_free(gotC);
  heap_caps_free(gotPie);
#endif
}
