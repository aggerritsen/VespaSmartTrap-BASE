#pragma once

#include <Arduino.h>
#include "sdcard.h"
#include "modem.h"

struct Gv2UartStats {
    uint32_t bytes = 0;
    uint32_t state_frames = 0;
    uint32_t jpeg_frames = 0;
    uint32_t error_frames = 0;
    uint32_t jpeg_invalid = 0;
    uint32_t last_frame_len = 0;
    uint8_t last_state = 0;
    uint8_t last_error_code = 0;
    uint8_t last_error_detail = 0;
    uint32_t last_error_counter = 0;
    uint8_t last_class_idx = 0;
    uint8_t last_conf_u8 = 0;
    uint16_t last_bbox_x = 0;
    uint16_t last_bbox_y = 0;
    uint16_t last_bbox_w = 0;
    uint16_t last_bbox_h = 0;
};

bool gv2_uart_init(const UartConfig &config);
void gv2_uart_set_log_context(const BaseConfig *config, const ModemGnssInfo *gnss);
void gv2_power_on();
void gv2_prepare_for_sleep(const UartConfig &config);
void gv2_uart_poll();
const Gv2UartStats &gv2_uart_stats();
