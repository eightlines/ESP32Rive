#include "board_i2c.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BOARD_I2C_SDA 7
#define BOARD_I2C_SCL 8

static const char* TAG = "i2c";
static i2c_master_bus_handle_t s_bus;

i2c_master_bus_handle_t board_i2c_bus(void)
{
  static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
  static SemaphoreHandle_t once;
  taskENTER_CRITICAL(&lock);
  if (once == NULL)
  {
    static StaticSemaphore_t storage;
    once = xSemaphoreCreateMutexStatic(&storage);
  }
  taskEXIT_CRITICAL(&lock);

  xSemaphoreTake(once, portMAX_DELAY);
  if (s_bus == NULL)
  {
    const i2c_master_bus_config_t cfg = {
        .i2c_port = -1, // any free controller
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "bus init failed: %s", esp_err_to_name(err));
      s_bus = NULL;
    }
  }
  xSemaphoreGive(once);
  return s_bus;
}
