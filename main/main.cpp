// Plays an embedded Rive file (chosen in menuconfig from main/assets) on the Waveshare
// ESP32-P4-WIFI6-Touch-LCD-4B 720x720 display.
//
// Per frame N:
//   core 1 (rive task):   advance Rive, record its draw calls (DisplayList),
//                         render the top half with ThorVG
//   core 0 (lane worker): render the bottom half with ThorVG, in parallel
//   PPA (background):     meanwhile, copy frame N-1's changed rows (ARGB8888,
//                         PSRAM) as RGB565 into a free panel buffer
//   then:                 flip to frame N-1, start the copy of frame N.
// Frames alternate between two ARGB8888 buffers, so the PPA can read one
// while the cores draw the other. The split between the two halves moves to
// keep both cores equally busy.
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>

#if CONFIG_RIVE_AUDIO_OUT
#include "audio_p4.h"
#endif
#include "display_p4.h"
#if CONFIG_RIVE_NET_CONTROL
#include "net_control.h"
#endif
#include "pie_selftest.h"
#include "raster_profile.h"
#include "touch_gt911.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rive_thorvg/rive_player.hpp"
#include "sdkconfig.h"

static const char* TAG = "rive";

// The .riv chosen in menuconfig, embedded under a fixed name (CMakeLists.txt).
extern const uint8_t rive_file_start[] asm("_binary_rive_file_riv_start");
extern const uint8_t rive_file_end[] asm("_binary_rive_file_riv_end");

#if CONFIG_RIVE_RENDER_HALF_RES
static constexpr int kRenderSize = DISPLAY_WIDTH / 2;
#else
static constexpr int kRenderSize = DISPLAY_WIDTH;
#endif

#if CONFIG_RIVE_DUAL_CORE
static constexpr uint32_t kLanes = 2;
#else
static constexpr uint32_t kLanes = 1;
#endif

#if CONFIG_RIVE_LANE_CULLING
static constexpr bool kCulling = true;
#else
static constexpr bool kCulling = false;
#endif

#if CONFIG_RIVE_PARTIAL_REDRAW
static constexpr bool kPartial = true;
#else
static constexpr bool kPartial = false;
#endif

// ---- Second-core lane worker --------------------------------------------

struct LaneWorker
{
  rive_thorvg::RivePlayer* player;
  uint32_t lane;
  SemaphoreHandle_t start; // rive task -> worker: render a frame
  SemaphoreHandle_t done;  // worker -> rive task: lane created / rendered
};

static void lane_worker_task(void* arg)
{
  auto* w = static_cast<LaneWorker*>(arg);
  // The lane's ThorVG canvas must be created on the thread that renders it.
  w->player->createLane(w->lane);
  xSemaphoreGive(w->done);
  while (true)
  {
    xSemaphoreTake(w->start, portMAX_DELAY);
    w->player->renderLane(w->lane);
    xSemaphoreGive(w->done);
  }
}

// One-off at boot: how fast can the CPU write and read-modify-write pixels in
// internal SRAM vs PSRAM? Gives cycles/pixel to compare with the raster time.
static void memoryBenchmark()
{
  constexpr size_t kPixels = 720 * 32; // 92 KB, bigger than L1, smaller than L2
  const uint32_t caps[2] = {MALLOC_CAP_INTERNAL, MALLOC_CAP_SPIRAM};
  const char* names[2] = {"SRAM", "PSRAM"};
  for (int i = 0; i < 2; ++i)
  {
    auto* buf = static_cast<volatile uint32_t*>(
        heap_caps_malloc(kPixels * sizeof(uint32_t), caps[i]));
    if (buf == nullptr)
    {
      ESP_LOGW(TAG, "membench: no %s buffer", names[i]);
      continue;
    }
    constexpr int kReps = 8;
    int64_t t0 = esp_timer_get_time();
    for (int r = 0; r < kReps; ++r)
    {
      for (size_t p = 0; p < kPixels; ++p)
      {
        buf[p] = 0xFF202020u + uint32_t(r);
      }
    }
    int64_t t1 = esp_timer_get_time();
    for (int r = 0; r < kReps; ++r)
    {
      for (size_t p = 0; p < kPixels; ++p)
      {
        // Simple 50% src-over blend: read, modify, write.
        uint32_t d = buf[p];
        buf[p] = ((d >> 1) & 0x7F7F7F7Fu) + 0x40404040u;
      }
    }
    int64_t t2 = esp_timer_get_time();
    const float mhz = float(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    const float n = float(kPixels) * kReps;
    ESP_LOGI(TAG, "membench %s: write %.1f cycles/px, blend %.1f cycles/px",
             names[i],
             float(t1 - t0) * mhz / n,
             float(t2 - t1) * mhz / n);
    heap_caps_free(const_cast<uint32_t*>(buf));
  }
}

static void rive_task(void*)
{
  ESP_ERROR_CHECK(display_init(CONFIG_RIVE_BACKLIGHT_PERCENT));
  ESP_LOGI(TAG, "free heap at start: internal %u (largest block %u), PSRAM %u",
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  memoryBenchmark();
  pie_selftest_run();
  // Touch is optional: without it the animation still plays.
  if (touch_gt911_start(0, 15) != ESP_OK)
  {
    ESP_LOGW(TAG, "touch unavailable");
  }
#if CONFIG_RIVE_NET_CONTROL
  // Ethernet + the command port. Not fatal: without a cable the animation
  // still plays.
  if (net_control_start(0) != ESP_OK)
  {
    ESP_LOGW(TAG, "network control unavailable");
  }
#endif
#if CONFIG_RIVE_AUDIO_OUT
  // Before the file loads, so its audio events find the engine. The audio
  // task runs on core 0 next to lane 1; it needs ~2% of a core.
  audio_start(0);
#endif

  // Render targets: two premultiplied ARGB8888 frames in PSRAM, used
  // alternately. Each keeps its contents between uses (needed for partial
  // redraw). Aligned to the L2 cache line (128 B) for the PPA's DMA.
  uint32_t* frames[2];
  for (auto& f : frames)
  {
    f = static_cast<uint32_t*>(heap_caps_aligned_calloc(
        128, kRenderSize * kRenderSize, sizeof(uint32_t), MALLOC_CAP_SPIRAM));
    if (f == nullptr)
    {
      ESP_LOGE(TAG, "out of PSRAM for the render targets");
      vTaskDelete(nullptr);
      return;
    }
  }

  static rive_thorvg::RivePlayer player(
      frames, 2, kRenderSize, kRenderSize, kLanes, kPartial);
  player.setClearColor(0xFF000000);
  player.setCulling(kCulling);
#if CONFIG_RIVE_IN_PLACE_UPDATES
  player.setInPlaceUpdates(true);
#else
  player.setInPlaceUpdates(false);
#endif
#if CONFIG_RIVE_SCRIPTING_REQUIRE_SIGNED
  player.setRequireSignedScripts(true);
#endif
  player.createLane(0); // this task (core 1) renders lane 0

  static LaneWorker worker;
  if (kLanes > 1)
  {
    worker.player = &player;
    worker.lane = 1;
    worker.start = xSemaphoreCreateBinary();
    worker.done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(
        lane_worker_task, "rive_lane1", 24 * 1024, &worker, 5, nullptr, 0);
    xSemaphoreTake(worker.done, portMAX_DELAY); // lane 1 created
  }

  const size_t rivSize = rive_file_end - rive_file_start;
  ESP_LOGI(TAG, "loading %s (%u bytes): %dx%d, %u lane(s), partial "
           "redraw %s",
           CONFIG_RIVE_EMBEDDED_FILE_NAME, unsigned(rivSize), kRenderSize,
           kRenderSize, unsigned(kLanes),
           kPartial ? "on" : "off");
  if (!player.load(rive_file_start, rivSize))
  {
    ESP_LOGE(TAG, "could not load %s", CONFIG_RIVE_EMBEDDED_FILE_NAME);
    vTaskDelete(nullptr);
    return;
  }
#if CONFIG_RIVE_AUDIO_OUT && CONFIG_RIVE_AUDIO_BUFFER_SECONDS > 0
  // Decode short sounds once now, so playing them only mixes samples.
  {
    const int64_t t0 = esp_timer_get_time();
    const size_t bytes = player.prepareAudio(
        audio_channels(), audio_sample_rate(),
        float(CONFIG_RIVE_AUDIO_BUFFER_SECONDS));
    if (bytes > 0)
    {
      ESP_LOGI(TAG, "sounds decoded in advance: %u KB in %.0f ms",
               unsigned(bytes / 1024),
               double(esp_timer_get_time() - t0) / 1000.0);
    }
  }
#endif
  ESP_LOGI(TAG, "free heap: internal %u, PSRAM %u",
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

  // Rows changed in each of the last frames, as reported by the player (for
  // a frame drawn into ARGB buffer k: changes since buffer k's previous
  // frame, two frames earlier). The panel buffer being written holds the
  // frame from DISPLAY_FB_AGE flips ago, so the copy must cover every change
  // since then: the union of the last DISPLAY_FB_AGE + 1 entries. Starts
  // "everything changed" because the panel buffers start out empty.
  constexpr int kHistory = DISPLAY_FB_AGE + 1;
  struct Rows
  {
    uint32_t y0, y1;
  };
  Rows history[kHistory];
  for (auto& h : history)
  {
    h = {0, uint32_t(kRenderSize)};
  }
  int historyPos = 0;

  int64_t last = esp_timer_get_time();
  int64_t statsStart = last;
  int frames_ = 0, rebuilds = 0, moves = 0;
  float sumAdvance = 0, sumBuild = 0, sumRender = 0, sumBlitWait = 0,
        sumVsync = 0, sumDirty = 0, sumCopied = 0, sumCopyMs = 0;
  int copies = 0;
  uint32_t sumCulled[2] = {};
  uint32_t sumEdited[2] = {}, sumReplaced[2] = {}, sumClips[2] = {};
#if CONFIG_RIVE_PPA_BURST_SWEEP
  static const int kBursts[] = {128, 64, 32, 16, 8};
  int burstIndex = 0, intervalsAtBurst = 0;
#endif
  float sumLane[2] = {}, sumApply[2] = {}, sumPrep[2] = {}, sumFill[2] = {};
  bool copyPending = false;

  while (true)
  {
    int64_t now = esp_timer_get_time();
    float dt = float(now - last) / 1e6f;
    last = now;
    if (dt > 0.1f)
    {
      dt = 0.1f; // don't jump far ahead after a stall
    }

    // Touch: forward to the Rive state machine (if the scene is one), in
    // render-frame pixels.
    touch_event_t touch;
    while (touch_next_event(&touch))
    {
      const float k = float(kRenderSize) / float(DISPLAY_WIDTH);
      const float tx = float(touch.x) * k, ty = float(touch.y) * k;
      switch (touch.type)
      {
        case TOUCH_DOWN:
          ESP_LOGI(TAG, "touch down at %u,%u", touch.x, touch.y);
          // A finger has no hover: move there first so listeners see the
          // pointer enter before the press, as they would with a mouse.
          player.pointerMove(tx, ty);
          player.pointerDown(tx, ty);
          break;
        case TOUCH_MOVE:
          player.pointerMove(tx, ty);
          break;
        case TOUCH_UP:
          ESP_LOGI(TAG, "touch up at %u,%u", touch.x, touch.y);
          player.pointerUp(tx, ty);
          // The finger is gone: leave, so the next touch enters again
          // (hover listeners, e.g. sound.riv's, fire on every tap).
          player.pointerExit(tx, ty);
          break;
      }
    }

#if CONFIG_RIVE_NET_CONTROL
    // Commands from the network ("set score 42"), between frames.
    net_control_service(
        [](const std::string& line) { return player.command(line); });
#endif
    player.advance(dt);
    player.build(); // also switches to the other ARGB buffer

    // Render all lanes: lane 1 on core 0, lane 0 here on core 1. The PPA is
    // copying the previous frame from the other ARGB buffer meanwhile.
    int64_t r0 = esp_timer_get_time();
    if (kLanes > 1)
    {
      xSemaphoreGive(worker.start);
    }
    player.renderLane(0);
    if (kLanes > 1)
    {
      xSemaphoreTake(worker.done, portMAX_DELAY);
    }
    float renderMs = float(esp_timer_get_time() - r0) / 1000.0f;

    // Show the previous frame (its copy normally finished during rendering).
    int64_t w0 = esp_timer_get_time();
    if (copyPending)
    {
      ESP_ERROR_CHECK(display_end_frame());
      float copyMs = display_last_copy_ms();
      if (copyMs > 0.0f)
      {
        sumCopyMs += copyMs;
        ++copies;
      }
    }
    float blitWaitMs = float(esp_timer_get_time() - w0) / 1000.0f;

    // Rows to copy for this frame.
    uint32_t y0 = 0, y1 = 0;
    bool dirty = player.dirtyRows(&y0, &y1);
    history[historyPos] = dirty ? Rows{y0, y1} : Rows{0, 0};
    historyPos = (historyPos + 1) % kHistory;
    uint32_t copyY0 = uint32_t(kRenderSize), copyY1 = 0;
    for (const Rows& h : history)
    {
      if (h.y1 > h.y0)
      {
        copyY0 = std::min(copyY0, h.y0);
        copyY1 = std::max(copyY1, h.y1);
      }
    }

    // Start copying this frame to a free panel buffer, in the background.
    int64_t v0 = esp_timer_get_time();
    display_begin_frame();
    float vsyncMs = float(esp_timer_get_time() - v0) / 1000.0f;
    if (copyY1 > copyY0)
    {
      ESP_ERROR_CHECK(display_blit_async(player.frame(), kRenderSize,
                                         kRenderSize, int(copyY0),
                                         int(copyY1 - copyY0)));
    }
    copyPending = true;

    // Stats for this frame (before balancing changes the lanes).
    const auto& s = player.stats();
    sumAdvance += s.advanceMs;
    sumBuild += s.buildMs;
    sumRender += renderMs;
    sumBlitWait += blitWaitMs;
    sumVsync += vsyncMs;
    sumDirty += dirty ? float(y1 - y0) / kRenderSize : 0.0f;
    sumCopied += copyY1 > copyY0 ? float(copyY1 - copyY0) / kRenderSize : 0.0f;
    for (uint32_t i = 0; i < kLanes; ++i)
    {
      const auto& l = player.laneStats(i);
      sumLane[i] += l.totalMs();
      sumApply[i] += l.applyMs;
      sumPrep[i] += l.prepMs;
      sumFill[i] += l.fillMs;
      sumCulled[i] += l.culled;
      sumEdited[i] += l.edited;
      sumReplaced[i] += l.changed;
      sumClips[i] += l.clips;
    }
    rebuilds += player.laneStats(0).rebuilt ? 1 : 0;
    ++frames_;

    // Move the split between the two halves toward balance.
    if (player.balanceLanes())
    {
      ++moves;
    }

    int64_t elapsed = esp_timer_get_time() - statsStart;
    if (elapsed >= int64_t(CONFIG_RIVE_STATS_INTERVAL_MS) * 1000)
    {
      float n = float(frames_);
      ESP_LOGI(TAG,
               "%.1f fps | advance %.1f  build %.1f  render %.1f  blit wait "
               "%.1f  vsync %.1f ms | copy %.1f ms (PPA burst %d) | dirty "
               "%.0f%%  copied %.0f%% | %" PRIu32 " draws, %d rebuilds",
               n * 1e6f / float(elapsed),
               sumAdvance / n,
               sumBuild / n,
               sumRender / n,
               sumBlitWait / n,
               sumVsync / n,
               copies ? sumCopyMs / float(copies) : 0.0f,
               display_ppa_burst(),
               100.0f * sumDirty / n,
               100.0f * sumCopied / n,
               s.draws,
               rebuilds);
      for (uint32_t i = 0; i < kLanes; ++i)
      {
        uint32_t ly0 = player.laneY0(i);
        uint32_t ly1 = i + 1 < kLanes ? player.laneY0(i + 1)
                                      : uint32_t(kRenderSize);
        ESP_LOGI(TAG, "  lane %" PRIu32 " (core %d, rows %" PRIu32 "-%" PRIu32
                 "): %.1f ms = apply %.1f + prep %.1f + fill %.1f | %.0f "
                 "draws culled, %.0f edited, %.0f replaced, %.0f clips | "
                 "background %s",
                 i, i == 0 ? 1 : 0, ly0, ly1, sumLane[i] / n, sumApply[i] / n,
                 sumPrep[i] / n, sumFill[i] / n, float(sumCulled[i]) / n,
                 float(sumEdited[i]) / n, float(sumReplaced[i]) / n,
                 float(sumClips[i]) / n,
                 player.laneStats(i).backgroundHidden ? "skipped" : "drawn");
        sumLane[i] = sumApply[i] = sumPrep[i] = sumFill[i] = 0;
        sumCulled[i] = 0;
        sumEdited[i] = sumReplaced[i] = sumClips[i] = 0;
      }
      if (moves > 0)
      {
        ESP_LOGI(TAG, "  split moved %d time(s)", moves);
      }
      raster_profile_report(TAG, frames_);
#if CONFIG_RIVE_AUDIO_OUT
      {
        const audio_stats_t a = audio_take_stats();
        ESP_LOGI(TAG, "  audio: peak %s%.0f dBFS | mix %.2f ms per %.1f ms "
                 "block (%.1f%% of a core) | stack %u of %u KB free%s",
                 a.peak > 0.0f ? "" : "< ",
                 a.peak > 0.0f ? 20.0f * log10f(a.peak) : -90.0f,
                 a.mixMs, 256.0f * 1000.0f / CONFIG_RIVE_AUDIO_SAMPLE_RATE,
                 a.cpuPercent, unsigned(a.stackFree / 1024),
                 unsigned(a.stackSize / 1024),
                 a.codec ? "" : " | no codec");
      }
#endif
      frames_ = rebuilds = moves = copies = 0;
      sumAdvance = sumBuild = sumRender = sumBlitWait = sumVsync = sumDirty =
          sumCopied = sumCopyMs = 0;
#if CONFIG_RIVE_PPA_BURST_SWEEP
      // Two intervals per burst length, then the next one.
      if (++intervalsAtBurst >= 2)
      {
        intervalsAtBurst = 0;
        burstIndex = (burstIndex + 1) % int(sizeof(kBursts) / sizeof(kBursts[0]));
        ESP_ERROR_CHECK(display_set_ppa_burst(kBursts[burstIndex]));
      }
#endif
      statsStart = esp_timer_get_time();
    }

    // Let the idle task run so the task watchdog stays fed.
    vTaskDelay(1);
  }
}

extern "C" void app_main(void)
{
  // Rive file parsing and ThorVG rasterisation recurse; give them room.
  xTaskCreatePinnedToCore(rive_task, "rive", 32 * 1024, nullptr, 5, nullptr, 1);
}
