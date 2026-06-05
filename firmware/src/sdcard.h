#pragma once
#include <Arduino.h>

struct StepperConfig {
    uint16_t speed_steps_per_second = 400;
    uint16_t rotation_degrees = 90;
    uint16_t steps_per_revolution = 2048;
    uint16_t reverse_wait_ms = 1000;
    char start_direction[17] = "ccw";
    bool post_test_enabled = false;
};

struct UartConfig {
    uint8_t rx_gpio = 16;
    uint8_t tx_gpio = 17;
    uint32_t baud = 921600;
};

struct InferenceConfig {
    static constexpr uint8_t MAX_CLASS_NAMES = 8;

    struct ClassName {
        uint8_t class_idx;
        char short_name[9];
        char display_name[33];
    };

    float confidence_threshold = 0.89f;
    float doubtful_confidence_threshold = 0.70f;
    int16_t detected_class = 3;
    uint16_t occurrence = 3;
    bool upload_doubtful_to_azure = true;
    uint8_t class_name_count = 4;
    ClassName class_names[MAX_CLASS_NAMES] = {
        {0, "amel", "Apis mellifera"},
        {1, "vcra", "Vespa crabro"},
        {2, "vesp", "Vespula sp."},
        {3, "vvel", "Vespa velutina"},
    };
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
    char reboot_cron[64] = "";
    bool reboot_after_deep_sleep_wakeup = false;
};

struct TimeConfig {
    uint16_t network_timeout_seconds = 10;
    bool allow_gnss_fallback = true;
};

struct ModemConfig {
    static constexpr uint8_t MAX_APN_CANDIDATES = 6;
    static constexpr uint8_t MAX_SIM_PROFILES = 6;
    static constexpr uint8_t MAX_SIM_PREFIXES = 4;

    struct ApnCandidate {
        char supplier[33] = "";
        char apn[33] = "";
    };

    struct SimProfile {
        char supplier[33] = "";
        char apn[33] = "";
        bool direct_sms = true;
        char imsi_prefixes[MAX_SIM_PREFIXES][9] = {};
        char iccid_prefixes[MAX_SIM_PREFIXES][13] = {};
        uint8_t imsi_prefix_count = 0;
        uint8_t iccid_prefix_count = 0;
    };

    uint8_t mode = 0; // 0=no modem, 1=time only, 2=LTE-M validated
    char apn[33] = "internet.m2m";
    bool apn_autodetect = true;
    bool apn_test_all = false;
    bool validate_http_egress = false;
    bool operator_auto_select = false;
    bool direct_sms = true;
    bool keep_alive_after_post = false;
    bool wake_for_runtime_sms = true;
    uint8_t apn_candidate_count = 0;
    ApnCandidate apn_candidates[MAX_APN_CANDIDATES];
    uint8_t sim_profile_count = 0;
    SimProfile sim_profiles[MAX_SIM_PROFILES];
    char lookup_primary[16] = "1.1.1.1";
    char lookup_secondary[16] = "8.8.8.8";
};

struct HealthConfig {
    uint8_t led = 1; // 0=off, 1=blink health state on status LED
};

struct AzureConfig {
    uint32_t cooldown_minutes = 15;
    uint32_t failure_cooldown_seconds = 120;
    uint16_t runtime_connect_timeout_seconds = 20;
};

struct SmsConfig {
    static constexpr uint8_t MAX_RECIPIENTS = 5;

    struct Recipient {
        char name[33] = "";
        char number[24] = "";
    };

    bool enabled = false;
    bool post_test_enabled = false;
    uint16_t runtime_settle_ms = 2000;
    uint16_t runtime_delay_after_detection_seconds = 0;
    uint32_t runtime_submit_timeout_ms = 60000;
    uint32_t cooldown_minutes = 15;
    uint32_t failure_cooldown_seconds = 900;
    uint8_t recipient_count = 0;
    Recipient recipients[MAX_RECIPIENTS];
};

struct BaseConfig {
    char device_name[32] = "vst-base-001";
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
    AzureConfig azure;
    SmsConfig sms;
};

bool sdcard_init();
bool sdcard_available();
bool sdcard_ensure_config();
bool sdcard_load_config(BaseConfig &config);
bool sdcard_save_jpeg(uint32_t frame_id,
                      const uint8_t *data,
                      size_t len,
                      char *out_path = nullptr,
                      size_t out_path_len = 0,
                      int16_t class_idx = -1,
                      float confidence = -1.0f);
bool sdcard_begin_jpeg(uint32_t frame_id);
bool sdcard_write_jpeg_chunk(const uint8_t *data, size_t len);
bool sdcard_finish_jpeg();
void sdcard_abort_jpeg();
bool sdcard_append_log(const char *path, const String &text);
bool sdcard_write_log(const char *path, const String &text);
