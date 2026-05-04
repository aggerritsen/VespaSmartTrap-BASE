#pragma once

#include <Arduino.h>
#include "sdcard.h"

struct WebFrameInfo {
    uint32_t frame_id = 0;
    uint8_t state = 0;
    uint8_t class_idx = 0;
    uint8_t confidence_u8 = 0;
    uint16_t bbox_x = 0;
    uint16_t bbox_y = 0;
    uint16_t bbox_w = 0;
    uint16_t bbox_h = 0;
    uint32_t jpeg_len = 0;
    uint32_t crc_rx = 0;
    uint32_t crc_calc = 0;
    bool crc_ok = false;
    bool valid = false;
    bool filter_match = false;
    bool detection_match = false;
    bool saved = false;
    bool actuated = false;
    uint16_t occurrence_count = 0;
    uint16_t occurrence_required = 1;
};

bool web_init(const WebConfig &config);
void web_loop();
void web_publish_frame(const uint8_t *jpeg, size_t jpeg_len, const WebFrameInfo &info);

