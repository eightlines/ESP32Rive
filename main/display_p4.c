#include "display_p4.h"

#include <stdbool.h>

#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7703.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display";

// Board wiring (from the Waveshare BSP for this board)
#define LCD_RST_GPIO 27
#define LCD_BACKLIGHT_GPIO 26
#define LCD_BACKLIGHT_LEDC_TIMER LEDC_TIMER_1
#define LCD_BACKLIGHT_LEDC_CH LEDC_CHANNEL_1
#define DSI_LANES 2
#define DSI_LANE_MBPS 480
#define DSI_PHY_LDO_CHAN 3 // VO3 -> VDD_MIPI_DPHY
#define DSI_PHY_LDO_MV 2500
#define LCD_LDO_CHAN 4 // VO4 3.3 V rail, enabled by the BSP before the panel
#define LCD_LDO_MV 3300

#define FB_BYTES (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2)

// Three panel frame buffers, used round robin: one on screen, one that was
// on screen until the last flip (may still be scanned out until the next
// refresh completes), and one free to be written while the next frame is
// being drawn.
#define NUM_FBS 3

static esp_lcd_panel_handle_t s_panel;
static void *s_fb[NUM_FBS];
static int s_shown = 0;   // buffer most recently flipped to
static int s_target = 1;  // buffer being written for the next flip
// Refreshes completed (counted in the DSI ISR), and the count at the moment
// each buffer was flipped to.
static volatile uint32_t s_refreshes;
static uint32_t s_flip_at[NUM_FBS];
static SemaphoreHandle_t s_refresh_sem;
static ppa_client_handle_t s_ppa;
static SemaphoreHandle_t s_ppa_done;
static bool s_ppa_busy;
static int64_t s_ppa_start_us;
static volatile int64_t s_ppa_end_us;
static int s_ppa_burst = 128;

// Called from the DSI ISR when a full frame has been sent to the panel. A
// buffer that was replaced by a flip is no longer read once the refresh that
// was in progress at flip time has completed.
static IRAM_ATTR bool on_refresh_complete(esp_lcd_panel_handle_t panel,
                                          esp_lcd_dpi_panel_event_data_t *edata,
                                          void *user_ctx)
{
    s_refreshes = s_refreshes + 1;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_refresh_sem, &woken);
    return woken == pdTRUE;
}

// Called (from the PPA ISR) when a non-blocking copy has finished.
static IRAM_ATTR bool on_ppa_done(ppa_client_handle_t client, ppa_event_data_t *event_data, void *user_data)
{
    s_ppa_end_us = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_ppa_done, &woken);
    return woken == pdTRUE;
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LCD_BACKLIGHT_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
    const ledc_channel_config_t ch = {
        .gpio_num = LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_BACKLIGHT_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 1,
    };
    return ledc_channel_config(&ch);
}

esp_err_t display_set_backlight(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (1023 * percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_LEDC_CH, duty), TAG, "duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_LEDC_CH);
}

esp_err_t display_init(int backlight_percent)
{
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init");

    // Power rails: MIPI D-PHY supply and the 3.3 V LCD rail.
    static esp_ldo_channel_handle_t phy_ldo, lcd_ldo;
    const esp_ldo_channel_config_t phy_ldo_cfg = {
        .chan_id = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&phy_ldo_cfg, &phy_ldo), TAG, "DSI PHY LDO");
    const esp_ldo_channel_config_t lcd_ldo_cfg = {
        .chan_id = LCD_LDO_CHAN,
        .voltage_mv = LCD_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&lcd_ldo_cfg, &lcd_ldo), TAG, "LCD LDO");

    esp_lcd_dsi_bus_handle_t dsi_bus;
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
        .phy_clk_src = 0, // default
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), TAG, "DSI bus");

    esp_lcd_panel_io_handle_t io;
    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io), TAG, "DBI io");

    esp_lcd_dpi_panel_config_t dpi_cfg = ST7703_720_720_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi_cfg.num_fbs = NUM_FBS;

    st7703_vendor_config_t vendor_cfg = {
        .flags = {
            .use_mipi_interface = 1,
        },
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .bits_per_pixel = 16,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .reset_gpio_num = LCD_RST_GPIO,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7703(io, &dev_cfg, &s_panel), TAG, "ST7703 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    // DPI panels are always on; ignore "not supported".
    esp_lcd_panel_disp_on_off(s_panel, true);

    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, NUM_FBS, &s_fb[0], &s_fb[1], &s_fb[2]), TAG, "frame buffers");

    s_refresh_sem = xSemaphoreCreateBinary();
    s_ppa_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_refresh_sem && s_ppa_done, ESP_ERR_NO_MEM, TAG, "semaphore");
    // `on_refresh_done` exists in every ESP-IDF 5.5.x release; later releases
    // keep it as a deprecated alias (same union member) of on_frame_buf_complete.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    const esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_refresh_done = on_refresh_complete,
    };
#pragma GCC diagnostic pop
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL), TAG, "callbacks");

    ESP_RETURN_ON_ERROR(display_set_ppa_burst(128), TAG, "PPA client");

    ESP_RETURN_ON_ERROR(display_set_backlight(backlight_percent), TAG, "backlight");
    ESP_LOGI(TAG, "720x720 ST7703 ready, frame buffers %p %p %p", s_fb[0], s_fb[1], s_fb[2]);
    return ESP_OK;
}

void display_begin_frame(void)
{
    // Round robin: the target is the buffer that was replaced two flips ago.
    // It is free once the refresh in progress at the *previous* flip (which
    // replaced the buffer before it) has completed. Frames take far longer
    // than a refresh, so this normally doesn't wait.
    s_target = (s_shown + 1) % NUM_FBS;
    const int previous = (s_shown + NUM_FBS - 1) % NUM_FBS;
    while ((int32_t)(s_refreshes - s_flip_at[previous]) <= 0) {
        xSemaphoreTake(s_refresh_sem, pdMS_TO_TICKS(20));
    }
}

esp_err_t display_blit_async(const uint32_t *frame, int src_w, int src_h, int src_y0, int rows)
{
    if (rows <= 0) {
        return ESP_OK;
    }
    // The PPA reads rows [src_y0, src_y0 + rows) of the ARGB8888 frame,
    // scales them by DISPLAY_WIDTH / src_w (1 or 2) and writes RGB565 into
    // the target frame buffer, in the background.
    const uint32_t scale = DISPLAY_WIDTH / (uint32_t)src_w;
    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = frame,
            .pic_w = (uint32_t)src_w,
            .pic_h = (uint32_t)src_h,
            .block_w = (uint32_t)src_w,
            .block_h = (uint32_t)rows,
            .block_offset_x = 0,
            .block_offset_y = (uint32_t)src_y0,
            .srm_cm = PPA_SRM_COLOR_MODE_ARGB8888,
        },
        .out = {
            .buffer = s_fb[s_target],
            .buffer_size = FB_BYTES,
            .pic_w = DISPLAY_WIDTH,
            .pic_h = DISPLAY_HEIGHT,
            .block_offset_x = 0,
            .block_offset_y = (uint32_t)src_y0 * scale,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)scale,
        .scale_y = (float)scale,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    xSemaphoreTake(s_ppa_done, 0); // drop any stale completion
    s_ppa_start_us = esp_timer_get_time();
    s_ppa_end_us = 0;
    ESP_RETURN_ON_ERROR(ppa_do_scale_rotate_mirror(s_ppa, &op), TAG, "PPA");
    s_ppa_busy = true;
    return ESP_OK;
}

void display_wait_blit(void)
{
    if (s_ppa_busy) {
        xSemaphoreTake(s_ppa_done, portMAX_DELAY);
        s_ppa_busy = false;
    }
}

float display_last_copy_ms(void)
{
    if (s_ppa_end_us == 0 || s_ppa_end_us < s_ppa_start_us) {
        return 0.0f;
    }
    return (float)(s_ppa_end_us - s_ppa_start_us) / 1000.0f;
}

esp_err_t display_set_ppa_burst(int bytes)
{
    display_wait_blit();
    if (s_ppa) {
        ESP_RETURN_ON_ERROR(ppa_unregister_client(s_ppa), TAG, "PPA unregister");
        s_ppa = NULL;
    }
    // Map bytes to the enum constants: in ESP-IDF 5.5.2 these are indices
    // (1..5), in later releases byte counts, so never cast the number.
    ppa_data_burst_length_t burst;
    switch (bytes) {
    case 8:  burst = PPA_DATA_BURST_LENGTH_8; break;
    case 16: burst = PPA_DATA_BURST_LENGTH_16; break;
    case 32: burst = PPA_DATA_BURST_LENGTH_32; break;
    case 64: burst = PPA_DATA_BURST_LENGTH_64; break;
    default: burst = PPA_DATA_BURST_LENGTH_128; bytes = 128; break;
    }
    const ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = burst,
    };
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_cfg, &s_ppa), TAG, "PPA client");
    const ppa_event_callbacks_t ppa_cbs = {
        .on_trans_done = on_ppa_done,
    };
    ESP_RETURN_ON_ERROR(ppa_client_register_event_callbacks(s_ppa, &ppa_cbs), TAG, "PPA callbacks");
    s_ppa_burst = bytes;
    return ESP_OK;
}

int display_ppa_burst(void)
{
    return s_ppa_burst;
}

esp_err_t display_end_frame(void)
{
    display_wait_blit();
    // The target is one of the panel's own frame buffers: no copy, it just
    // becomes the buffer scanned out from the next refresh on.
    s_flip_at[s_target] = s_refreshes;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, s_fb[s_target]), TAG, "flip");
    s_shown = s_target;
    return ESP_OK;
}

esp_err_t display_present(const uint32_t *argb, int src_w, int src_h)
{
    display_begin_frame();
    ESP_RETURN_ON_ERROR(display_blit_async(argb, src_w, src_h, 0, src_h), TAG, "PPA");
    return display_end_frame();
}
