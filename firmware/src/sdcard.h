#pragma once
#include <Arduino.h>

bool sdcard_init();
bool sdcard_available();
bool sdcard_save_jpeg(uint32_t frame_id, const uint8_t *data, size_t len);
bool sdcard_append_log(const char *path, const String &text);
bool sdcard_write_log(const char *path, const String &text);
