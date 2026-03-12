/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/lock.h>
#include <sys/param.h>
#include <unistd.h>

#if CONFIG_EXAMPLE_LCD_CONTROLLER_SH1107
#include "esp_lcd_sh1107.h"
#else
#include "esp_lcd_panel_vendor.h"
#endif

static const char *TAG = "ldah-rx";

#define I2C_BUS_PORT 0

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your
/// LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ (400 * 1000)
#define EXAMPLE_PIN_NUM_SDA 21
#define EXAMPLE_PIN_NUM_SCL 22
#define EXAMPLE_PIN_NUM_RST -1
#define EXAMPLE_I2C_HW_ADDR 0x3C

// The pixel number in horizontal and vertical
#if CONFIG_EXAMPLE_LCD_CONTROLLER_SSD1306
#define EXAMPLE_LCD_H_RES 128
#define EXAMPLE_LCD_V_RES CONFIG_EXAMPLE_SSD1306_HEIGHT
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_SH1107
#define EXAMPLE_LCD_H_RES 64
#define EXAMPLE_LCD_V_RES 128
#endif
// Bit number used to represent command and parameter
#define EXAMPLE_LCD_CMD_BITS 8
#define EXAMPLE_LCD_PARAM_BITS 8

#define EXAMPLE_LVGL_TICK_PERIOD_MS 5
#define EXAMPLE_LVGL_TASK_STACK_SIZE (4 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY 2
#define EXAMPLE_LVGL_PALETTE_SIZE 8
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ

// To use LV_COLOR_FORMAT_I1, we need an extra buffer to hold the converted data
static uint8_t oled_buffer[EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES / 8];
// LVGL library is not thread-safe, this example will call LVGL APIs from
// different tasks, so use a mutex to protect it
static _lock_t lvgl_api_lock;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// UART2 payload receiver (from NUCLEO-L432KC)
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define LDAH_UART_PORT UART_NUM_2
#define LDAH_UART_BAUD 115200
#define LDAH_UART_TXD_PIN 17
#define LDAH_UART_RXD_PIN 16
#define LDAH_UART_BUF_SIZE 256

/*
 * Payload bit layout (32-bit unsigned integer, little-endian byte order):
 *
 *   MSB
 *   [31:28] sync nibble (4 bits, always 0xA)
 *   [27:20] heart rate  (8 bits, BPM)
 *   [19:13] SpO2        (7 bits, %)
 *   [12: 4] GSR         (9 bits, uS)
 *   [    3] pulse       (1 bit, unit impulse train)
 *   [    2] contact     (1 bit, 1=finger detected)
 *   [ 1: 0] lie eval    (2 bits: 00=undef, 01=lie, 10=truth, 11=inconclusive)
 *   LSB
 */
#define PAYLOAD_SYNC 0xA
#define PAYLOAD_HR(p) (((p) >> 20) & 0xFF)
#define PAYLOAD_SPO2(p) (((p) >> 13) & 0x7F)
#define PAYLOAD_GSR(p) (((p) >> 4) & 0x1FF)
#define PAYLOAD_PULSE(p) (((p) >> 3) & 0x01)
#define PAYLOAD_CONTACT(p) (((p) >> 2) & 0x01)
#define PAYLOAD_LIE(p) (((p) >> 0) & 0x03)

extern void example_lvgl_demo_ui(lv_disp_t *disp);
extern void ldah_ui_update(uint8_t hr, uint8_t spo2, uint16_t gsr, bool pulse,
                           bool contact, uint8_t lie_eval);

static bool
example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t io_panel,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx) {
  lv_display_t *disp = (lv_display_t *)user_ctx;
  lv_display_flush_ready(disp);
  return false;
}

static void example_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                                  uint8_t *px_map) {
  esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);

  // This is necessary because LVGL reserves 2 x 4 bytes in the buffer, as these
  // are assumed to be used as a palette. Skip the palette here More information
  // about the monochrome, please refer to
  // https://docs.lvgl.io/9.2/porting/display.html#monochrome-displays
  px_map += EXAMPLE_LVGL_PALETTE_SIZE;

  uint16_t hor_res = lv_display_get_physical_horizontal_resolution(disp);
  int x1 = area->x1;
  int x2 = area->x2;
  int y1 = area->y1;
  int y2 = area->y2;

  for (int y = y1; y <= y2; y++) {
    for (int x = x1; x <= x2; x++) {
      /* The order of bits is MSB first
                  MSB           LSB
         bits      7 6 5 4 3 2 1 0
         pixels    0 1 2 3 4 5 6 7
                  Left         Right
      */
      bool chroma_color =
          (px_map[(hor_res >> 3) * y + (x >> 3)] & 1 << (7 - x % 8));

      /* Write to the buffer as required for the display.
       * It writes only 1-bit for monochrome displays mapped vertically.*/
      uint8_t *buf = oled_buffer + hor_res * (y >> 3) + (x);
      if (chroma_color) {
        (*buf) &= ~(1 << (y % 8));
      } else {
        (*buf) |= (1 << (y % 8));
      }
    }
  }
  // pass the draw buffer to the driver
  esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2 + 1, y2 + 1, oled_buffer);
}

static void example_increase_lvgl_tick(void *arg) {
  /* Tell LVGL how many milliseconds has elapsed */
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static void example_lvgl_port_task(void *arg) {
  ESP_LOGI(TAG, "Starting LVGL task");
  uint32_t time_till_next_ms = 0;
  while (1) {
    _lock_acquire(&lvgl_api_lock);
    time_till_next_ms = lv_timer_handler();
    _lock_release(&lvgl_api_lock);
    // in case of triggering a task watch dog time out
    time_till_next_ms = MAX(time_till_next_ms, EXAMPLE_LVGL_TASK_MIN_DELAY_MS);
    // in case of lvgl display not ready yet
    time_till_next_ms = MIN(time_till_next_ms, EXAMPLE_LVGL_TASK_MAX_DELAY_MS);
    usleep(1000 * time_till_next_ms);
  }
}

static void ldah_uart_init(void) {
  const uart_config_t cfg = {
      .baud_rate = LDAH_UART_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  ESP_ERROR_CHECK(uart_param_config(LDAH_UART_PORT, &cfg));
  ESP_ERROR_CHECK(uart_set_pin(LDAH_UART_PORT, LDAH_UART_TXD_PIN,
                               LDAH_UART_RXD_PIN, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE));
  ESP_ERROR_CHECK(
      uart_driver_install(LDAH_UART_PORT, LDAH_UART_BUF_SIZE, 0, 0, NULL, 0));
}

static void ldah_uart_task(void *arg) {
  static const char *const lie_strs[] = {"  UNDEF", "    LIE", "  TRUTH",
                                         "INCONCL"};
  uint8_t win[4] = {0};
  int pos = 0;

  uart_flush(LDAH_UART_PORT);

  while (1) {
    uint8_t byte;
    int n = uart_read_bytes(LDAH_UART_PORT, &byte, 1, pdMS_TO_TICKS(2000));
    if (n != 1)
      continue;

    win[pos++] = byte;
    if (pos < 4)
      continue;

    // Reconstruct little-endian uint32 (NUCLEO ARM is LE-native)
    uint32_t p = (uint32_t)win[0] | ((uint32_t)win[1] << 8) |
                 ((uint32_t)win[2] << 16) | ((uint32_t)win[3] << 24);

    // Validate: sync nibble [31:28] must be 0xA
    if ((p >> 28) != PAYLOAD_SYNC) {
      // Misaligned - slide window by 1 byte to resync
      win[0] = win[1];
      win[1] = win[2];
      win[2] = win[3];
      pos = 3;
      continue;
    }

    // Valid frame - reset window for next frame
    pos = 0;

    ESP_LOGI(TAG,
             "RX payload: 0x%08" PRIx32
             " | HR=%3u SPO2=%3u GSR=%3u Pulse=%u Contact=%u Lie=%s",
             p, PAYLOAD_HR(p), PAYLOAD_SPO2(p), PAYLOAD_GSR(p),
             PAYLOAD_PULSE(p), PAYLOAD_CONTACT(p), lie_strs[PAYLOAD_LIE(p)]);

    _lock_acquire(&lvgl_api_lock);
    ldah_ui_update(PAYLOAD_HR(p), PAYLOAD_SPO2(p), PAYLOAD_GSR(p),
                   PAYLOAD_PULSE(p), PAYLOAD_CONTACT(p), PAYLOAD_LIE(p));
    _lock_release(&lvgl_api_lock);
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "Initialize I2C bus");
  i2c_master_bus_handle_t i2c_bus = NULL;
  i2c_master_bus_config_t bus_config = {
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .i2c_port = I2C_BUS_PORT,
      .sda_io_num = EXAMPLE_PIN_NUM_SDA,
      .scl_io_num = EXAMPLE_PIN_NUM_SCL,
      .flags.enable_internal_pullup = true,
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));

  ESP_LOGI(TAG, "Install panel IO");
  esp_lcd_panel_io_handle_t io_handle = NULL;
  esp_lcd_panel_io_i2c_config_t io_config = {
      .dev_addr = EXAMPLE_I2C_HW_ADDR,
      .scl_speed_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
      .control_phase_bytes = 1,               // According to SSD1306 datasheet
      .lcd_cmd_bits = EXAMPLE_LCD_CMD_BITS,   // According to SSD1306 datasheet
      .lcd_param_bits = EXAMPLE_LCD_CMD_BITS, // According to SSD1306 datasheet
#if CONFIG_EXAMPLE_LCD_CONTROLLER_SSD1306
      .dc_bit_offset = 6, // According to SSD1306 datasheet
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_SH1107
      .dc_bit_offset = 0, // According to SH1107 datasheet
      .flags =
          {
              .disable_control_phase = 1,
          }
#endif
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &io_handle));

  ESP_LOGI(TAG, "Install SSD1306 panel driver");
  esp_lcd_panel_handle_t panel_handle = NULL;
  esp_lcd_panel_dev_config_t panel_config = {
      .bits_per_pixel = 1,
      .reset_gpio_num = EXAMPLE_PIN_NUM_RST,
  };
#if CONFIG_EXAMPLE_LCD_CONTROLLER_SSD1306
  esp_lcd_panel_ssd1306_config_t ssd1306_config = {
      .height = EXAMPLE_LCD_V_RES,
  };
  panel_config.vendor_config = &ssd1306_config;
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &panel_handle));
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_SH1107
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_sh1107(io_handle, &panel_config, &panel_handle));
#endif

  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

#if CONFIG_EXAMPLE_LCD_CONTROLLER_SH1107
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
#endif

  ESP_LOGI(TAG, "Initialize LVGL");
  lv_init();
  // create a lvgl display
  lv_display_t *display =
      lv_display_create(EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
  // associate the i2c panel handle to the display
  lv_display_set_user_data(display, panel_handle);
  // create draw buffer
  void *buf = NULL;
  ESP_LOGI(TAG, "Allocate separate LVGL draw buffers");
  // LVGL reserves 2 x 4 bytes in the buffer, as these are assumed to be used as
  // a palette.
  size_t draw_buffer_sz =
      EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES / 8 + EXAMPLE_LVGL_PALETTE_SIZE;
  buf = heap_caps_calloc(1, draw_buffer_sz,
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  assert(buf);

  // LVGL9 suooprt new monochromatic format.
  lv_display_set_color_format(display, LV_COLOR_FORMAT_I1);
  // initialize LVGL draw buffers
  lv_display_set_buffers(display, buf, NULL, draw_buffer_sz,
                         LV_DISPLAY_RENDER_MODE_FULL);
  // set the callback which can copy the rendered image to an area of the
  // display
  lv_display_set_flush_cb(display, example_lvgl_flush_cb);

  ESP_LOGI(
      TAG,
      "Register io panel event callback for LVGL flush ready notification");
  const esp_lcd_panel_io_callbacks_t cbs = {
      .on_color_trans_done = example_notify_lvgl_flush_ready,
  };
  /* Register done callback */
  esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display);

  ESP_LOGI(TAG, "Use esp_timer as LVGL tick timer");
  const esp_timer_create_args_t lvgl_tick_timer_args = {
      .callback = &example_increase_lvgl_tick, .name = "lvgl_tick"};
  esp_timer_handle_t lvgl_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer,
                                           EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

  ESP_LOGI(TAG, "Create LVGL task");
  xTaskCreate(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE,
              NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);

  ESP_LOGI(TAG, "Initialize UART2 (LDAH payload receiver)");
  ldah_uart_init();
  xTaskCreate(ldah_uart_task, "ldah_uart", 4096, NULL, 3, NULL);

  ESP_LOGI(TAG, "Display lie detector UI");
  // Lock the mutex due to the LVGL APIs are not thread-safe
  _lock_acquire(&lvgl_api_lock);
  example_lvgl_demo_ui(display);
  _lock_release(&lvgl_api_lock);
}
