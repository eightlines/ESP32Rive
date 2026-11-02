// Goodix GT911 touch controller, polled over I2C. See touch_gt911.h.
//
// GT911 registers (16-bit addresses, big-endian on the wire):
//   0x8140  product ID, ASCII ("911")
//   0x8048  configured X resolution (little-endian u16), 0x804A: Y
//   0x814E  status: bit 7 = new data ready, bits 0-3 = number of points
//   0x814F  point 1: track id, x (u16 LE), y (u16 LE), size (u16 LE), -
// After reading a ready status, 0 must be written back to 0x814E, or the
// controller stops updating.
#include "touch_gt911.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "display_p4.h"

#define TOUCH_I2C_SDA 7
#define TOUCH_I2C_SCL 8
#define TOUCH_I2C_HZ 400000
#define TOUCH_TIMEOUT_MS 20

#define GT911_REG_CONFIG_XRES 0x8048
#define GT911_REG_PRODUCT_ID 0x8140
#define GT911_REG_STATUS 0x814E
#define GT911_REG_POINT1 0x814F

static const char* TAG = "touch";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static QueueHandle_t s_events;
static uint32_t s_period_ms;
static uint16_t s_res_x = DISPLAY_WIDTH, s_res_y = DISPLAY_HEIGHT;

static esp_err_t gt911_read(uint16_t reg, uint8_t* data, size_t len)
{
  const uint8_t addr[2] = {reg >> 8, reg & 0xFF};
  return i2c_master_transmit_receive(s_dev, addr, sizeof(addr), data, len,
                                     TOUCH_TIMEOUT_MS);
}

static esp_err_t gt911_write_u8(uint16_t reg, uint8_t value)
{
  const uint8_t buf[3] = {reg >> 8, reg & 0xFF, value};
  return i2c_master_transmit(s_dev, buf, sizeof(buf), TOUCH_TIMEOUT_MS);
}

static void push(touch_event_type_t type, uint16_t x, uint16_t y)
{
  touch_event_t e = {.type = type, .x = x, .y = y};
  if (xQueueSend(s_events, &e, 0) != pdTRUE && type != TOUCH_MOVE)
  {
    // Full (render loop stalled): drop the oldest so down / up survive.
    touch_event_t old;
    xQueueReceive(s_events, &old, 0);
    xQueueSend(s_events, &e, 0);
  }
}

static void touch_task(void* arg)
{
  (void)arg;
  bool down = false;
  uint16_t last_x = 0, last_y = 0;
  uint32_t errors = 0;
  TickType_t wake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(s_period_ms) ? pdMS_TO_TICKS(s_period_ms) : 1;
  while (true)
  {
    vTaskDelayUntil(&wake, period);
    uint8_t status = 0;
    if (gt911_read(GT911_REG_STATUS, &status, 1) != ESP_OK)
    {
      if (++errors == 10)
      {
        ESP_LOGW(TAG, "GT911 not answering (10 failed reads)");
      }
      continue;
    }
    errors = 0;
    if ((status & 0x80) == 0)
    {
      continue; // no new sample yet
    }
    const int points = status & 0x0F;
    if (points > 0 && points <= 5)
    {
      uint8_t p[8];
      if (gt911_read(GT911_REG_POINT1, p, sizeof(p)) == ESP_OK)
      {
        uint32_t x = (uint32_t)(p[1] | (p[2] << 8));
        uint32_t y = (uint32_t)(p[3] | (p[4] << 8));
        // Scale from the controller's configured resolution to the panel.
        x = x * DISPLAY_WIDTH / s_res_x;
        y = y * DISPLAY_HEIGHT / s_res_y;
        if (x >= DISPLAY_WIDTH) x = DISPLAY_WIDTH - 1;
        if (y >= DISPLAY_HEIGHT) y = DISPLAY_HEIGHT - 1;
        if (!down)
        {
          push(TOUCH_DOWN, (uint16_t)x, (uint16_t)y);
          down = true;
        }
        else if (x != last_x || y != last_y)
        {
          push(TOUCH_MOVE, (uint16_t)x, (uint16_t)y);
        }
        last_x = (uint16_t)x;
        last_y = (uint16_t)y;
      }
    }
    else if (points == 0 && down)
    {
      push(TOUCH_UP, last_x, last_y);
      down = false;
    }
    gt911_write_u8(GT911_REG_STATUS, 0); // ready for the next sample
  }
}

esp_err_t touch_gt911_start(int core, uint32_t period_ms)
{
  const i2c_master_bus_config_t bus_cfg = {
      .i2c_port = -1, // any free controller
      .sda_io_num = TOUCH_I2C_SDA,
      .scl_io_num = TOUCH_I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
    return err;
  }
  uint16_t address = 0;
  if (i2c_master_probe(s_bus, 0x5D, 50) == ESP_OK)
  {
    address = 0x5D;
  }
  else if (i2c_master_probe(s_bus, 0x14, 50) == ESP_OK)
  {
    address = 0x14;
  }
  else
  {
    ESP_LOGE(TAG, "no GT911 at 0x5D or 0x14");
    i2c_del_master_bus(s_bus);
    s_bus = NULL;
    return ESP_ERR_NOT_FOUND;
  }
  const i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = address,
      .scl_speed_hz = TOUCH_I2C_HZ,
  };
  err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
  if (err != ESP_OK)
  {
    return err;
  }

  char id[5] = {0};
  uint8_t res[4] = {0};
  if (gt911_read(GT911_REG_PRODUCT_ID, (uint8_t*)id, 4) == ESP_OK &&
      gt911_read(GT911_REG_CONFIG_XRES, res, sizeof(res)) == ESP_OK)
  {
    const uint16_t rx = (uint16_t)(res[0] | (res[1] << 8));
    const uint16_t ry = (uint16_t)(res[2] | (res[3] << 8));
    if (rx > 0 && ry > 0)
    {
      s_res_x = rx;
      s_res_y = ry;
    }
  }
  ESP_LOGI(TAG, "GT911 (id \"%s\") at 0x%02X, %ux%u, polled every %u ms", id,
           address, s_res_x, s_res_y, (unsigned)period_ms);
  gt911_write_u8(GT911_REG_STATUS, 0);

  s_period_ms = period_ms;
  s_events = xQueueCreate(16, sizeof(touch_event_t));
  // Above the render tasks' priority so polling stays regular; each poll is
  // one short I2C transfer (~0.1 ms), mostly spent waiting on the bus.
  if (xTaskCreatePinnedToCore(touch_task, "touch", 3072, NULL, 6, NULL, core) !=
      pdPASS)
  {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

bool touch_next_event(touch_event_t* event)
{
  return s_events != NULL && xQueueReceive(s_events, event, 0) == pdTRUE;
}
