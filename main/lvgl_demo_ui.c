/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "lvgl.h"
#include <stdbool.h>
#include <stdio.h>

static lv_obj_t *lbl_hr;
static lv_obj_t *lbl_spo2;
static lv_obj_t *lbl_gsr;
static lv_obj_t *lbl_status;
static lv_obj_t *lbl_result;

void example_lvgl_demo_ui(lv_display_t *disp) {
  lv_obj_t *scr = lv_display_get_screen_active(disp);

  const lv_font_t *font_mono = &lv_font_unscii_8;

  // Row 0 (y=0):  Heart rate
  lbl_hr = lv_label_create(scr);
  lv_obj_set_style_text_font(lbl_hr, font_mono, 0);
  lv_label_set_text(lbl_hr, "HR  : --- BPM");
  lv_obj_set_pos(lbl_hr, 0, 0);

  // Row 1 (y=10): SpO2
  lbl_spo2 = lv_label_create(scr);
  lv_obj_set_style_text_font(lbl_spo2, font_mono, 0);
  lv_label_set_text(lbl_spo2, "SpO2: --- %");
  lv_obj_set_pos(lbl_spo2, 0, 10);

  // Row 2 (y=20): GSR
  lbl_gsr = lv_label_create(scr);
  lv_obj_set_style_text_font(lbl_gsr, font_mono, 0);
  lv_label_set_text(lbl_gsr, "GSR : --- uS");
  lv_obj_set_pos(lbl_gsr, 0, 20);

  // Row 3 (y=30): Pulse + Contact
  lbl_status = lv_label_create(scr);
  lv_obj_set_style_text_font(lbl_status, font_mono, 0);
  lv_label_set_text(lbl_status, "PLACE YOUR HAND");
  lv_obj_set_pos(lbl_status, 0, 30);

  // Row 4 (y=42): Lie evaluation result - default (14px) font, centered
  lbl_result = lv_label_create(scr);
  lv_label_set_text(lbl_result, "---");
  lv_obj_set_width(lbl_result, 128);
  lv_obj_set_style_text_align(lbl_result, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(lbl_result, 0, 42);
}

void ldah_ui_update(uint8_t heart_rate, uint8_t spo2, uint16_t gsr, bool pulse,
                    bool contact, uint8_t lie_eval) {
  char buf[32];

  snprintf(buf, sizeof(buf), "HR  :%3u BPM  %s", heart_rate, pulse ? "<3" : "");
  lv_label_set_text(lbl_hr, buf);

  snprintf(buf, sizeof(buf), "SpO2:%3u %%", spo2);
  lv_label_set_text(lbl_spo2, buf);

  snprintf(buf, sizeof(buf), "GSR :%3u uS", gsr);
  lv_label_set_text(lbl_gsr, buf);

  lv_label_set_text(lbl_status, contact ? "" : "PLACE YOUR HAND");

  static const char *const lie_strs[] = {"---", "LIE", "TRUTH", "INCONCLUSIVE"};
  lv_label_set_text(lbl_result, lie_strs[lie_eval & 0x03]);
}
