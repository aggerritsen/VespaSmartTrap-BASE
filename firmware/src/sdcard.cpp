//sdcard.cpp

#include "sdcard.h"
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <strings.h>
#include <time.h>

/* =============================
   SD PIN CONFIG (T-SIM7080G-S3)
   ============================= */
#define SD_CMD  39
#define SD_CLK  38
#define SD_DATA 40

static bool sd_ok = false;
static File active_jpeg_file;
static char active_jpeg_path[64] = {0};
static uint32_t active_jpeg_bytes = 0;

static const char *CONFIG_PATH = "/config.json";
static const char *DEFAULT_CONFIG =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"device_name\": \"vst-base\",\n"
    "  \"uart\": {\n"
    "    \"rx_gpio\": 16,\n"
    "    \"tx_gpio\": 17,\n"
    "    \"baud\": 921600\n"
    "  },\n"
    "  \"logging\": {\n"
    "    \"post_log\": \"/post.log\",\n"
    "    \"image_prefix\": \"/frame_\"\n"
    "  },\n"
    "  \"features\": {\n"
    "    \"gnss_probe\": true,\n"
    "    \"ack_frames\": true\n"
    "  },\n"
    "  \"stepper\": {\n"
    "    \"speed_steps_per_second\": 200,\n"
    "    \"rotation_degrees\": 90,\n"
    "    \"steps_per_revolution\": 2048,\n"
    "    \"reverse_wait_ms\": 1000,\n"
    "    \"start_direction\": \"clockwise\",\n"
    "    \"_start_direction_comment\": \"Use clockwise/cw or anti-clockwise/ccw\"\n"
    "  },\n"
    "  \"inference\": {\n"
    "    \"confidence_threshold\": 0.0,\n"
    "    \"detected_class\": -1,\n"
    "    \"occurrence\": 1\n"
    "  },\n"
    "  \"web\": {\n"
    "    \"mode\": 2,\n"
    "    \"ssid\": \"VST-BASE\",\n"
    "    \"password\": \"\",\n"
    "    \"append_mac\": true\n"
    "  },\n"
    "  \"power\": {\n"
    "    \"log_interval_seconds\": 60,\n"
    "    \"_log_interval_comment\": \"Use 60 for now; later 900 for 15 minutes or 3600 for 1 hour\"\n"
    "  }\n"
    "}\n";

static const char *sd_card_type_name(uint8_t type)
{
    switch (type) {
        case CARD_MMC: return "MMC";
        case CARD_SD: return "SDSC";
        case CARD_SDHC: return "SDHC/SDXC";
        default: return "UNKNOWN";
    }
}

static void make_jpeg_path(char *path, size_t path_len, uint32_t frame_id)
{
    time_t now = time(nullptr);
    struct tm tm{};

    if (now > 1700000000 && localtime_r(&now, &tm)) {
        snprintf(path,
                 path_len,
                 "/%04d%02d%02d_%02d%02d%02d_%06lu.jpg",
                 tm.tm_year + 1900,
                 tm.tm_mon + 1,
                 tm.tm_mday,
                 tm.tm_hour,
                 tm.tm_min,
                 tm.tm_sec,
                 (unsigned long)frame_id);
        return;
    }

    snprintf(path, path_len, "/uptime_%010lu_%06lu.jpg", millis(), (unsigned long)frame_id);
}

static bool is_anti_clockwise_direction(const char *direction)
{
    if (!direction)
        return false;

    return strcasecmp(direction, "anti-clockwise") == 0 ||
           strcasecmp(direction, "anticlockwise") == 0 ||
           strcasecmp(direction, "counter-clockwise") == 0 ||
           strcasecmp(direction, "counterclockwise") == 0 ||
           strcasecmp(direction, "ccw") == 0 ||
           strcasecmp(direction, "anti-clokckwise") == 0;
}

static bool is_clockwise_direction(const char *direction)
{
    if (!direction)
        return false;

    return strcasecmp(direction, "clockwise") == 0 ||
           strcasecmp(direction, "cw") == 0;
}

/* =============================
   INIT
   ============================= */
bool sdcard_init()
{
    Serial.println("SD: initializing SD_MMC with custom pins");
    Serial.printf("SD: CLK=%d CMD=%d DATA=%d mode=1-bit\n", SD_CLK, SD_CMD, SD_DATA);

    SD_MMC.setPins(SD_CLK, SD_CMD, SD_DATA);

    if (!SD_MMC.begin("/sdcard", true))
    {
        Serial.println("SD: mount FAILED");
        sd_ok = false;
        return false;
    }

    uint64_t size = SD_MMC.cardSize();
    uint64_t used = SD_MMC.usedBytes();
    uint8_t type = SD_MMC.cardType();

    Serial.printf("SD: mounted OK\n");
    Serial.printf("SD: card_type=%s (%u)\n", sd_card_type_name(type), type);
    Serial.printf("SD: size_mb=%llu\n", size / (1024 * 1024));
    Serial.printf("SD: used_bytes=%llu total_bytes=%llu\n", used, size);
    Serial.printf("SD: free_bytes=%llu\n", size > used ? (size - used) : 0);

    sd_ok = true;
    return true;
}

bool sdcard_available()
{
    return sd_ok;
}

bool sdcard_ensure_config()
{
    if (!sd_ok)
        return false;

    if (SD_MMC.exists(CONFIG_PATH))
    {
        File f = SD_MMC.open(CONFIG_PATH, FILE_READ);
        if (!f)
        {
            Serial.printf("SD: config open FAILED path=%s\n", CONFIG_PATH);
            return false;
        }

        size_t size = f.size();
        f.close();

        if (size == 0)
        {
            Serial.printf("SD: config empty path=%s\n", CONFIG_PATH);
            return false;
        }

        Serial.printf("SD: config found path=%s bytes=%u\n", CONFIG_PATH, (unsigned)size);
        return true;
    }

    File f = SD_MMC.open(CONFIG_PATH, FILE_WRITE);
    if (!f)
    {
        Serial.printf("SD: config create FAILED path=%s\n", CONFIG_PATH);
        return false;
    }

    size_t expected = strlen(DEFAULT_CONFIG);
    size_t written = f.print(DEFAULT_CONFIG);
    f.close();

    if (written != expected)
    {
        Serial.printf("SD: config write incomplete path=%s written=%u expected=%u\n",
                      CONFIG_PATH,
                      (unsigned)written,
                      (unsigned)expected);
        return false;
    }

    Serial.printf("SD: config created path=%s bytes=%u\n", CONFIG_PATH, (unsigned)written);
    return true;
}

bool sdcard_load_config(BaseConfig &config)
{
    config = BaseConfig{};

    if (!sd_ok)
        return false;

    File f = SD_MMC.open(CONFIG_PATH, FILE_READ);
    if (!f)
    {
        Serial.printf("SD: config load FAILED path=%s\n", CONFIG_PATH);
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err)
    {
        Serial.printf("SD: config parse FAILED path=%s error=%s; using defaults\n",
                      CONFIG_PATH,
                      err.c_str());
        return false;
    }

    const char *device_name = doc["device_name"] | config.device_name;
    strlcpy(config.device_name, device_name, sizeof(config.device_name));

    JsonObject uart = doc["uart"];
    config.uart.rx_gpio = uart["rx_gpio"] | config.uart.rx_gpio;
    config.uart.tx_gpio = uart["tx_gpio"] | config.uart.tx_gpio;
    config.uart.baud = uart["baud"] | config.uart.baud;

    if (config.uart.baud == 0)
        config.uart.baud = 921600;

    JsonObject stepper = doc["stepper"];
    config.stepper.speed_steps_per_second =
        stepper["speed_steps_per_second"] | config.stepper.speed_steps_per_second;
    config.stepper.rotation_degrees =
        stepper["rotation_degrees"] | config.stepper.rotation_degrees;
    config.stepper.steps_per_revolution =
        stepper["steps_per_revolution"] | config.stepper.steps_per_revolution;
    config.stepper.reverse_wait_ms =
        stepper["reverse_wait_ms"] | config.stepper.reverse_wait_ms;
    const char *start_direction = stepper["start_direction"] | config.stepper.start_direction;
    strlcpy(config.stepper.start_direction,
            is_anti_clockwise_direction(start_direction) && !is_clockwise_direction(start_direction) ? "anti-clockwise" : "clockwise",
            sizeof(config.stepper.start_direction));

    if (config.stepper.speed_steps_per_second == 0)
        config.stepper.speed_steps_per_second = 200;
    if (config.stepper.steps_per_revolution == 0)
        config.stepper.steps_per_revolution = 2048;
    if (config.stepper.reverse_wait_ms == 0)
        config.stepper.reverse_wait_ms = 1000;

    JsonObject inference = doc["inference"];
    config.inference.confidence_threshold =
        inference["confidence_threshold"] | config.inference.confidence_threshold;
    config.inference.detected_class =
        inference["detected_class"] | config.inference.detected_class;
    config.inference.occurrence =
        inference["occurrence"] | (inference["occurence"] | config.inference.occurrence);

    if (config.inference.confidence_threshold < 0.0f)
        config.inference.confidence_threshold = 0.0f;
    if (config.inference.confidence_threshold > 1.0f)
        config.inference.confidence_threshold = 1.0f;
    if (config.inference.detected_class < -1)
        config.inference.detected_class = -1;
    if (config.inference.detected_class > 255)
        config.inference.detected_class = 255;
    if (config.inference.occurrence == 0)
        config.inference.occurrence = 1;

    JsonObject web = doc["web"];
    config.web.mode = web["mode"] | config.web.mode;
    const char *web_ssid = web["ssid"] | config.web.ssid;
    const char *web_password = web["password"] | config.web.password;
    strlcpy(config.web.ssid, web_ssid, sizeof(config.web.ssid));
    strlcpy(config.web.password, web_password, sizeof(config.web.password));
    config.web.append_mac = web["append_mac"] | config.web.append_mac;
    if (config.web.mode > 2)
        config.web.mode = 0;

    JsonObject power = doc["power"];
    config.power.log_interval_seconds =
        power["log_interval_seconds"] | config.power.log_interval_seconds;
    if (config.power.log_interval_seconds == 0)
        config.power.log_interval_seconds = 60;
    if (config.power.log_interval_seconds > 86400)
        config.power.log_interval_seconds = 86400;

    Serial.printf("SD: config loaded device=%s uart_rx=%u uart_tx=%u uart_baud=%lu stepper_speed=%u stepper_rotation_deg=%u stepper_steps_per_rev=%u stepper_wait_ms=%u stepper_start_direction=%s inference_conf_threshold=%.3f inference_detected_class=%d inference_occurrence=%u web_mode=%u web_ssid=%s power_log_interval_seconds=%lu\n",
                  config.device_name,
                  config.uart.rx_gpio,
                  config.uart.tx_gpio,
                  (unsigned long)config.uart.baud,
                  config.stepper.speed_steps_per_second,
                  config.stepper.rotation_degrees,
                  config.stepper.steps_per_revolution,
                  config.stepper.reverse_wait_ms,
                  config.stepper.start_direction,
                  config.inference.confidence_threshold,
                  config.inference.detected_class,
                  config.inference.occurrence,
                  config.web.mode,
                  config.web.ssid,
                  (unsigned long)config.power.log_interval_seconds);

    return true;
}

bool sdcard_append_log(const char *path, const String &text)
{
    if (!sd_ok || !path || !path[0])
        return false;

    File f = SD_MMC.open(path, FILE_APPEND);
    if (!f)
    {
        Serial.printf("SD: log open FAILED path=%s\n", path);
        return false;
    }

    size_t written = f.print(text);
    f.close();

    if (written != text.length())
    {
        Serial.printf("SD: log write incomplete path=%s written=%u expected=%u\n",
                      path,
                      (unsigned)written,
                      (unsigned)text.length());
        return false;
    }

    Serial.printf("SD: log appended path=%s bytes=%u\n", path, (unsigned)written);
    return true;
}

bool sdcard_write_log(const char *path, const String &text)
{
    if (!sd_ok || !path || !path[0])
        return false;

    if (SD_MMC.exists(path))
        SD_MMC.remove(path);

    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f)
    {
        Serial.printf("SD: log open FAILED path=%s\n", path);
        return false;
    }

    size_t written = f.print(text);
    f.close();

    if (written != text.length())
    {
        Serial.printf("SD: log write incomplete path=%s written=%u expected=%u\n",
                      path,
                      (unsigned)written,
                      (unsigned)text.length());
        return false;
    }

    Serial.printf("SD: log written path=%s bytes=%u\n", path, (unsigned)written);
    return true;
}

/* =============================
   SAVE JPEG
   ============================= */
bool sdcard_save_jpeg(uint32_t frame_id, const uint8_t *data, size_t len, char *out_path, size_t out_path_len)
{
    if (!sd_ok)
        return false;

    char path[64];
    make_jpeg_path(path, sizeof(path), frame_id);
    if (out_path && out_path_len > 0) {
        strlcpy(out_path, path, out_path_len);
    }

    if (SD_MMC.exists(path))
        SD_MMC.remove(path);

    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f)
    {
        Serial.printf("SD: open FAILED path=%s\n", path);
        return false;
    }

    size_t written = f.write(data, len);
    f.close();

    if (written != len)
    {
        Serial.printf("SD: write incomplete written=%u expected=%u\n", written, len);
        return false;
    }

    Serial.printf("SD: jpeg saved path=%s bytes=%u\n", path, len);
    return true;
}

bool sdcard_begin_jpeg(uint32_t frame_id)
{
    if (!sd_ok)
        return false;

    sdcard_abort_jpeg();

    make_jpeg_path(active_jpeg_path, sizeof(active_jpeg_path), frame_id);
    if (SD_MMC.exists(active_jpeg_path))
        SD_MMC.remove(active_jpeg_path);

    active_jpeg_file = SD_MMC.open(active_jpeg_path, FILE_WRITE);
    active_jpeg_bytes = 0;

    if (!active_jpeg_file)
    {
        Serial.printf("SD: jpeg stream open FAILED path=%s\n", active_jpeg_path);
        active_jpeg_path[0] = '\0';
        return false;
    }

    Serial.printf("SD: jpeg stream begin path=%s\n", active_jpeg_path);
    return true;
}

bool sdcard_write_jpeg_chunk(const uint8_t *data, size_t len)
{
    if (!sd_ok || !active_jpeg_file || !data || len == 0)
        return false;

    size_t written = active_jpeg_file.write(data, len);
    if (written != len)
    {
        Serial.printf("SD: jpeg stream write incomplete path=%s written=%u expected=%u\n",
                      active_jpeg_path,
                      (unsigned)written,
                      (unsigned)len);
        return false;
    }

    active_jpeg_bytes += (uint32_t)written;
    return true;
}

bool sdcard_finish_jpeg()
{
    if (!active_jpeg_file)
        return false;

    active_jpeg_file.close();
    Serial.printf("SD: jpeg stream saved path=%s bytes=%lu\n",
                  active_jpeg_path,
                  (unsigned long)active_jpeg_bytes);
    active_jpeg_path[0] = '\0';
    active_jpeg_bytes = 0;
    return true;
}

void sdcard_abort_jpeg()
{
    if (active_jpeg_file)
    {
        active_jpeg_file.close();
        Serial.printf("SD: jpeg stream aborted path=%s bytes=%lu\n",
                      active_jpeg_path,
                      (unsigned long)active_jpeg_bytes);
    }

    active_jpeg_path[0] = '\0';
    active_jpeg_bytes = 0;
}
