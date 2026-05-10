#pragma once
#include <Arduino.h>

struct StepperConfig {
    uint16_t speed_steps_per_second = 200;
    uint16_t rotation_degrees = 90;
    uint16_t steps_per_revolution = 2048;
    uint16_t reverse_wait_ms = 1000;
    char start_direction[17] = "clockwise";
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

struct WebConfig {
    uint8_t mode = 2; // 0=off, 1=station, 2=access point
    char ssid[33] = "VST-BASE";
    char password[65] = "";
    bool append_mac = true;
};

struct LoggingConfig {
    char post_log[33] = "/post.log";
    char image_prefix[33] = "/frame_";
};

struct FeaturesConfig {
    bool gnss_probe = true;
    bool ack_frames = true;
};

struct PowerConfig {
    uint32_t log_interval_seconds = 900;
    uint8_t deep_sleep = 2;
    uint8_t deep_sleep_start_hour = 18;
    uint8_t deep_sleep_end_hour = 6;
};

struct TimeConfig {
    uint16_t network_timeout_seconds = 10;
    bool allow_gnss_fallback = true;
};

struct ModemConfig {
    uint8_t mode = 1; // 0=no modem, 1=time only, 2=LTE-M validated
    char apn[33] = "internet.m2m";
    char lookup_primary[16] = "1.1.1.1";
    char lookup_secondary[16] = "8.8.8.8";
};

struct HealthConfig {
    uint8_t led = 1; // 0=off, 1=blink health state on status LED
};

struct BaseConfig {
    char device_name[32] = "vst-base";
    UartConfig uart;
    LoggingConfig logging;
    FeaturesConfig features;
    StepperConfig stepper;
    InferenceConfig inference;
    WebConfig web;
    PowerConfig power;
    TimeConfig time;
    ModemConfig modem;
    HealthConfig health;
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
