#pragma once
#include <Arduino.h>

struct StepperConfig {
    uint16_t speed_steps_per_second = 200;
    uint16_t rotation_degrees = 90;
    uint16_t steps_per_revolution = 2048;
    uint16_t reverse_wait_ms = 1000;
};

struct UartConfig {
    uint8_t rx_gpio = 16;
    uint8_t tx_gpio = 17;
    uint32_t baud = 921600;
};

struct InferenceConfig {
    float confidence_threshold = 0.0f;
    int16_t detected_class = -1;
    uint16_t occurrence = 1;
};

struct BaseConfig {
    char device_name[32] = "vst-base";
    UartConfig uart;
    StepperConfig stepper;
    InferenceConfig inference;
};

bool sdcard_init();
bool sdcard_available();
bool sdcard_ensure_config();
bool sdcard_load_config(BaseConfig &config);
bool sdcard_save_jpeg(uint32_t frame_id, const uint8_t *data, size_t len, char *out_path = nullptr, size_t out_path_len = 0);
bool sdcard_begin_jpeg(uint32_t frame_id);
bool sdcard_write_jpeg_chunk(const uint8_t *data, size_t len);
bool sdcard_finish_jpeg();
void sdcard_abort_jpeg();
bool sdcard_append_log(const char *path, const String &text);
bool sdcard_write_log(const char *path, const String &text);
