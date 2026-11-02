#include "audio_p4.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rive/audio/audio_engine.hpp"
#include "sdkconfig.h"

#include "board_i2c.h"

static const char* TAG = "audio";

namespace
{
constexpr uint32_t kChannels = 2;
// 256 frames = 5.3 ms at 48 kHz: small enough that a sound starts promptly
// after its event, large enough that the per-block overhead stays low.
constexpr uint32_t kBlockFrames = 256;
constexpr uint32_t kRate = CONFIG_RIVE_AUDIO_SAMPLE_RATE;

// miniaudio's mixing graph keeps conversion and resampling buffers on the
// stack (several KB per node level), and decoders (dr_mp3 etc.) add more:
// 6 KB overflowed on the first sound. The stats line reports the margin.
constexpr uint32_t kStackBytes = 32 * 1024;

rive::rcp<rive::AudioEngine> s_engine;
TaskHandle_t s_task;
esp_codec_dev_handle_t s_codec;

portMUX_TYPE s_statsLock = portMUX_INITIALIZER_UNLOCKED;
float s_peak;
int64_t s_mixUs;
uint32_t s_blocks;

bool codecInit()
{
  i2c_master_bus_handle_t bus = board_i2c_bus();
  if (bus == nullptr)
  {
    return false;
  }

  i2s_chan_handle_t tx = nullptr;
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chan.dma_desc_num = 4;
  chan.dma_frame_num = kBlockFrames;
  chan.auto_clear = true; // play silence, not the last block, on an underrun
  if (i2s_new_channel(&chan, &tx, nullptr) != ESP_OK)
  {
    ESP_LOGE(TAG, "no free I2S controller");
    return false;
  }
  i2s_std_config_t std = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = gpio_num_t(CONFIG_RIVE_AUDIO_PIN_MCLK),
              .bclk = gpio_num_t(CONFIG_RIVE_AUDIO_PIN_BCLK),
              .ws = gpio_num_t(CONFIG_RIVE_AUDIO_PIN_WS),
              .dout = gpio_num_t(CONFIG_RIVE_AUDIO_PIN_DOUT),
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {},
          },
  };
  std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (i2s_channel_init_std_mode(tx, &std) != ESP_OK)
  {
    ESP_LOGE(TAG, "I2S init failed");
    return false;
  }

  audio_codec_i2s_cfg_t i2sCfg = {};
  i2sCfg.port = 0;
  i2sCfg.tx_handle = tx;
  const audio_codec_data_if_t* dataIf = audio_codec_new_i2s_data(&i2sCfg);

  audio_codec_i2c_cfg_t i2cCfg = {};
  i2cCfg.addr = ES8311_CODEC_DEFAULT_ADDR; // 0x30 (8-bit) = 0x18
  i2cCfg.bus_handle = bus;
  const audio_codec_ctrl_if_t* ctrlIf = audio_codec_new_i2c_ctrl(&i2cCfg);
  const audio_codec_gpio_if_t* gpioIf = audio_codec_new_gpio();
  if (dataIf == nullptr || ctrlIf == nullptr || gpioIf == nullptr)
  {
    ESP_LOGE(TAG, "no ES8311 at I2C 0x18");
    return false;
  }

  es8311_codec_cfg_t es = {};
  es.ctrl_if = ctrlIf;
  es.gpio_if = gpioIf;
  es.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
  es.pa_pin = CONFIG_RIVE_AUDIO_PIN_PA;
  es.use_mclk = true;
  es.hw_gain.pa_voltage = 5.0f;
  es.hw_gain.codec_dac_voltage = 3.3f;
  const audio_codec_if_t* codecIf = es8311_codec_new(&es);
  if (codecIf == nullptr)
  {
    ESP_LOGE(TAG, "ES8311 init failed");
    return false;
  }

  esp_codec_dev_cfg_t devCfg = {};
  devCfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
  devCfg.codec_if = codecIf;
  devCfg.data_if = dataIf;
  s_codec = esp_codec_dev_new(&devCfg);
  esp_codec_dev_sample_info_t fs = {};
  fs.bits_per_sample = 16;
  fs.channel = kChannels;
  fs.sample_rate = kRate;
  if (s_codec == nullptr || esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK)
  {
    ESP_LOGE(TAG, "codec open failed");
    s_codec = nullptr;
    return false;
  }
  esp_codec_dev_set_out_vol(s_codec, CONFIG_RIVE_AUDIO_VOLUME);
  return true;
}

void audioTask(void*)
{
  static float mix[kBlockFrames * kChannels];
  static int16_t pcm[kBlockFrames * kChannels];
  const TickType_t blockTicks =
      std::max<TickType_t>(1, pdMS_TO_TICKS(kBlockFrames * 1000 / kRate));
  TickType_t wake = xTaskGetTickCount();
  for (;;)
  {
    const int64_t t0 = esp_timer_get_time();
    std::memset(mix, 0, sizeof(mix));
    uint64_t got = 0;
    s_engine->readAudioFrames(mix, kBlockFrames, &got);
    float peak = 0.0f;
    for (uint32_t i = 0; i < kBlockFrames; ++i)
    {
      // The ES8311 has one DAC (the speaker is mono): send the mono mix on
      // both slots.
      float v = 0.5f * (mix[2 * i] + mix[2 * i + 1]);
      v = std::min(1.0f, std::max(-1.0f, v));
      peak = std::max(peak, std::fabs(v));
      const int16_t s = int16_t(std::lrintf(v * 32767.0f));
      pcm[2 * i] = s;
      pcm[2 * i + 1] = s;
    }
    const int64_t t1 = esp_timer_get_time();

    taskENTER_CRITICAL(&s_statsLock);
    s_peak = std::max(s_peak, peak);
    s_mixUs += t1 - t0;
    s_blocks++;
    taskEXIT_CRITICAL(&s_statsLock);

    if (s_codec != nullptr)
    {
      // Blocks while the I2S DMA queue is full, which paces this loop.
      esp_codec_dev_write(s_codec, pcm, sizeof(pcm));
    }
    else
    {
      // No codec: keep the engine's clock running in real time.
      vTaskDelayUntil(&wake, blockTicks);
    }
  }
}
} // namespace

esp_err_t audio_start(int core)
{
  s_engine = rive::AudioEngine::MakeAndStore(kChannels, kRate);
  if (s_engine == nullptr)
  {
    ESP_LOGE(TAG, "Rive audio engine could not be created");
    return ESP_FAIL;
  }
  const bool codec = codecInit();
  ESP_LOGI(TAG, "%s, %u Hz, %u-frame blocks, volume %d%%",
           codec ? "ES8311 ready" : "no codec, output measured only",
           unsigned(kRate), unsigned(kBlockFrames), CONFIG_RIVE_AUDIO_VOLUME);
  // Above the render tasks (5) so a block is never late while a frame
  // renders; it only runs for ~2% of the time.
  if (xTaskCreatePinnedToCore(audioTask, "audio", kStackBytes, nullptr, 7,
                              &s_task, core) != pdPASS)
  {
    return ESP_ERR_NO_MEM;
  }
  return codec ? ESP_OK : ESP_ERR_NOT_FOUND;
}

audio_stats_t audio_take_stats(void)
{
  audio_stats_t st = {};
  taskENTER_CRITICAL(&s_statsLock);
  st.peak = s_peak;
  st.blocks = s_blocks;
  const int64_t us = s_mixUs;
  s_peak = 0.0f;
  s_blocks = 0;
  s_mixUs = 0;
  taskEXIT_CRITICAL(&s_statsLock);
  st.codec = s_codec != nullptr;
  st.stackSize = kStackBytes;
  st.stackFree = s_task ? uint32_t(uxTaskGetStackHighWaterMark(s_task)) : 0;
  if (st.blocks > 0)
  {
    st.mixMs = float(us) / 1000.0f / st.blocks;
    const float blockMs = 1000.0f * kBlockFrames / kRate;
    st.cpuPercent = 100.0f * st.mixMs / blockMs;
  }
  return st;
}
