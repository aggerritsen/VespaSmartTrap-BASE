#include "uart.h"

#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "version.h"
#include "sdcard.h"
#include "stepper.h"
#include "web.h"

#ifndef GV2_UART_RX_GPIO_CFG
  #define GV2_UART_RX_GPIO_CFG 16
#endif
#ifndef GV2_UART_TX_GPIO_CFG
  #define GV2_UART_TX_GPIO_CFG 17
#endif
#ifndef GV2_UART_BAUD_CFG
  #define GV2_UART_BAUD_CFG 921600
#endif

static HardwareSerial Gv2Serial(2);

static UartConfig active_config;
static constexpr uint32_t MAX_JPEG_LEN = 512UL * 1024UL;
static constexpr size_t GV2_UART_RX_BUFFER_SIZE = 32UL * 1024UL;
static constexpr size_t GV2_UART_READ_CHUNK_SIZE = 2048;
static constexpr const char *FRAME_LOG_PATH = "/frames.log";

static const uint8_t JPEG_MAGIC[4] = {'V', 'S', 'T', 'J'};
static const uint8_t STATE_MAGIC[4] = {'V', 'S', 'T', 'S'};
static const uint8_t ERROR_MAGIC[4] = {'V', 'S', 'T', 'E'};
static constexpr int JPEG_HEADER_LEN = 1 + 1 + 1 + 2 + 2 + 2 + 2 + 4 + 4;
static constexpr int ERROR_PAYLOAD_LEN = 1 + 1 + 4;

struct JpegRxState {
    uint8_t magic_window[4] = {0, 0, 0, 0};
    uint8_t magic_filled = 0;
    bool receiving_jpeg = false;
    bool waiting_jpeg_header = false;
    uint32_t jpeg_remaining = 0;
    uint8_t *jpeg_buf = nullptr;
    uint32_t jpeg_offset = 0;
    uint8_t frame_state = 0;
    uint8_t frame_class_idx = 0;
    uint8_t frame_conf_u8 = 0;
    uint16_t frame_bbox_x = 0;
    uint16_t frame_bbox_y = 0;
    uint16_t frame_bbox_w = 0;
    uint16_t frame_bbox_h = 0;
    uint32_t frame_len = 0;
    uint32_t frame_crc32 = 0;
    bool frame_has_crc32 = false;
    uint8_t legacy_prefix[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t legacy_prefix_len = 0;
    uint32_t image_counter = 0;
    uint16_t detection_streak = 0;
};

struct StateRxState {
    uint8_t magic_window[4] = {0, 0, 0, 0};
    uint8_t magic_filled = 0;
};

struct ErrorRxState {
    uint8_t magic_window[4] = {0, 0, 0, 0};
    uint8_t magic_filled = 0;
    bool waiting_error_payload = false;
};

static JpegRxState jpeg_rx;
static StateRxState state_rx;
static ErrorRxState error_rx;
static Gv2UartStats stats;
static const BaseConfig *log_config = nullptr;
static const ModemGnssInfo *log_gnss = nullptr;

static void shift_magic_window(uint8_t *window, uint8_t *filled, uint8_t value)
{
    if (*filled < 4) {
        window[(*filled)++] = value;
        return;
    }

    window[0] = window[1];
    window[1] = window[2];
    window[2] = window[3];
    window[3] = value;
}

static bool magic_matches(const uint8_t *window, uint8_t filled, const uint8_t *magic)
{
    if (filled < 4)
        return false;

    return memcmp(window, magic, 4) == 0;
}

static bool read_u32_le(uint32_t &out)
{
    uint8_t b[4];
    for (uint8_t i = 0; i < sizeof(b); i++) {
        int v = Gv2Serial.read();
        if (v < 0)
            return false;
        b[i] = (uint8_t)v;
    }

    out = ((uint32_t)b[0]) |
          ((uint32_t)b[1] << 8) |
          ((uint32_t)b[2] << 16) |
          ((uint32_t)b[3] << 24);
    return true;
}

static bool read_u16_le(uint16_t &out)
{
    int lo = Gv2Serial.read();
    int hi = Gv2Serial.read();
    if (lo < 0 || hi < 0)
        return false;

    out = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    return true;
}

static void u16_to_le(uint16_t v, uint8_t *out)
{
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
}

static bool crc_field_looks_like_jpeg_prefix(uint32_t value)
{
    uint8_t b0 = (uint8_t)(value & 0xFF);
    uint8_t b1 = (uint8_t)((value >> 8) & 0xFF);
    uint8_t b2 = (uint8_t)((value >> 16) & 0xFF);
    uint8_t b3 = (uint8_t)((value >> 24) & 0xFF);

    if (b0 != 0xFF || b1 != 0xD8 || b2 != 0xFF)
        return false;

    return b3 == 0xE0 || b3 == 0xE1 || b3 == 0xDB || b3 == 0xEE || b3 == 0xFE;
}

static uint32_t vst_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            uint32_t mask = -(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static void abort_active_jpeg()
{
    if (jpeg_rx.jpeg_buf) {
        free(jpeg_rx.jpeg_buf);
        jpeg_rx.jpeg_buf = nullptr;
    }

    jpeg_rx.receiving_jpeg = false;
    jpeg_rx.waiting_jpeg_header = false;
    jpeg_rx.jpeg_remaining = 0;
    jpeg_rx.jpeg_offset = 0;
    jpeg_rx.legacy_prefix_len = 0;
}

static void reset_uart_sync_windows()
{
    memset(jpeg_rx.magic_window, 0, sizeof(jpeg_rx.magic_window));
    jpeg_rx.magic_filled = 0;
    memset(state_rx.magic_window, 0, sizeof(state_rx.magic_window));
    state_rx.magic_filled = 0;
    memset(error_rx.magic_window, 0, sizeof(error_rx.magic_window));
    error_rx.magic_filled = 0;
    error_rx.waiting_error_payload = false;
    jpeg_rx.waiting_jpeg_header = false;
    jpeg_rx.legacy_prefix_len = 0;
}

static uint32_t flush_gv2_uart_until_quiet(uint32_t quiet_ms, uint32_t max_ms)
{
    uint32_t discarded = 0;
    uint32_t start_ms = millis();
    uint32_t last_byte_ms = millis();

    while (millis() - start_ms < max_ms) {
        int available = Gv2Serial.available();
        if (available > 0) {
            while (Gv2Serial.available() > 0) {
                Gv2Serial.read();
                discarded++;
            }
            last_byte_ms = millis();
            continue;
        }

        if (millis() - last_byte_ms >= quiet_ms)
            break;

        delay(1);
    }

    reset_uart_sync_windows();
    return discarded;
}

static size_t jpeg_payload_len(const uint8_t *buf, size_t declared_len)
{
    if (!buf || declared_len < 4)
        return 0;

    if (buf[0] != 0xFF || buf[1] != 0xD8)
        return 0;

    for (size_t i = declared_len - 2; i > 1; i--) {
        if (buf[i] == 0xFF && buf[i + 1] == 0xD9)
            return i + 2;
    }

    return 0;
}

static bool jpeg_sanity_check(const uint8_t *buf, size_t len)
{
    if (!buf || len < 6)
        return false;

    if (buf[0] != 0xFF || buf[1] != 0xD8)
        return false;
    if (buf[len - 2] != 0xFF || buf[len - 1] != 0xD9)
        return false;

    size_t i = 2;
    while (i + 1 < len) {
        if (buf[i] != 0xFF)
            return false;

        while (i < len && buf[i] == 0xFF)
            i++;
        if (i >= len)
            return false;

        uint8_t marker = buf[i++];
        if (marker == 0x00)
            return false;

        if (marker == 0xDA) {
            if (i + 2 > len)
                return false;
            uint16_t segment_len = ((uint16_t)buf[i] << 8) | buf[i + 1];
            return segment_len >= 2 && i + segment_len <= len - 2;
        }

        if (marker == 0xD9)
            return false;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
            continue;

        if (i + 2 > len)
            return false;
        uint16_t segment_len = ((uint16_t)buf[i] << 8) | buf[i + 1];
        if (segment_len < 2 || i + segment_len > len)
            return false;

        i += segment_len;
    }

    return false;
}

static void start_jpeg_frame()
{
    jpeg_rx.image_counter++;
    stats.last_frame_len = jpeg_rx.frame_len;
    stats.last_state = jpeg_rx.frame_state;
    stats.last_class_idx = jpeg_rx.frame_class_idx;
    stats.last_conf_u8 = jpeg_rx.frame_conf_u8;
    stats.last_bbox_x = jpeg_rx.frame_bbox_x;
    stats.last_bbox_y = jpeg_rx.frame_bbox_y;
    stats.last_bbox_w = jpeg_rx.frame_bbox_w;
    stats.last_bbox_h = jpeg_rx.frame_bbox_h;

    jpeg_rx.receiving_jpeg = true;
    jpeg_rx.jpeg_remaining = jpeg_rx.frame_len - jpeg_rx.legacy_prefix_len;
    jpeg_rx.jpeg_offset = 0;
    jpeg_rx.jpeg_buf = (uint8_t *)heap_caps_malloc(jpeg_rx.frame_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_rx.jpeg_buf)
        jpeg_rx.jpeg_buf = (uint8_t *)heap_caps_malloc(jpeg_rx.frame_len, MALLOC_CAP_8BIT);

    if (!jpeg_rx.jpeg_buf) {
        stats.jpeg_invalid++;
        Serial.printf("GV2: jpeg ram alloc FAILED len=%lu\n", (unsigned long)jpeg_rx.frame_len);
        abort_active_jpeg();
        return;
    }

    if (jpeg_rx.legacy_prefix_len > 0) {
        memcpy(jpeg_rx.jpeg_buf, jpeg_rx.legacy_prefix, jpeg_rx.legacy_prefix_len);
        jpeg_rx.jpeg_offset = jpeg_rx.legacy_prefix_len;
    }

    Serial.printf("GV2: jpeg rx begin #%lu len=%lu crc32=%08lX protocol=%s ram=YES\n",
                  (unsigned long)jpeg_rx.image_counter,
                  (unsigned long)jpeg_rx.frame_len,
                  (unsigned long)jpeg_rx.frame_crc32,
                  jpeg_rx.frame_has_crc32 ? "CRC32" : "LEGACY");
}

void gv2_uart_set_log_context(const BaseConfig *config, const ModemGnssInfo *gnss)
{
    log_config = config;
    log_gnss = gnss;
}

static String current_timestamp()
{
    time_t now = time(nullptr);
    struct tm tm{};
    char buf[32];

    if (now > 1700000000 && localtime_r(&now, &tm)) {
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
        return String(buf);
    }

    snprintf(buf, sizeof(buf), "uptime_ms_%lu", (unsigned long)millis());
    return String(buf);
}

static void append_json_field(String &s, const char *name, const char *value)
{
    s += "\"";
    s += name;
    s += "\":\"";
    if (value) {
        for (const char *p = value; *p; p++) {
            if (*p == '"' || *p == '\\')
                s += '\\';
            s += *p;
        }
    }
    s += "\"";
}

static bool frame_matches_class_and_confidence(bool valid)
{
    const BaseConfig *cfg = log_config;
    InferenceConfig inference = cfg ? cfg->inference : InferenceConfig{};
    float confidence = (float)jpeg_rx.frame_conf_u8 / 255.0f;
    bool class_matches = inference.detected_class < 0 ||
        jpeg_rx.frame_class_idx == (uint8_t)inference.detected_class;
    bool confidence_matches = confidence >= inference.confidence_threshold;

    return valid && class_matches && confidence_matches;
}

static uint16_t configured_occurrence()
{
    const BaseConfig *cfg = log_config;
    uint16_t occurrence = cfg ? cfg->inference.occurrence : InferenceConfig{}.occurrence;
    return occurrence == 0 ? 1 : occurrence;
}

static const char *gv2_error_name(uint8_t code, uint8_t detail)
{
    if (code == 3 && detail == 1)
        return "camera_missing";
    if (code == 3 && detail == 2)
        return "camera_datapath_failed";
    if (code == 3)
        return "camera_error";
    return "unknown";
}

static void append_gv2_error_log(uint8_t code, uint8_t detail, uint32_t counter)
{
    const BaseConfig *cfg = log_config;

    String s;
    s.reserve(280);
    s += "{";
    append_json_field(s, "timestamp", current_timestamp().c_str());
    s += ",";
    append_json_field(s, "device_name", cfg ? cfg->device_name : "vst-base");
    s += ",";
    append_json_field(s, "event", "gv2_error");
    s += ",";
    append_json_field(s, "name", gv2_error_name(code, detail));
    s += ",\"code\":";
    s += (unsigned)code;
    s += ",\"detail\":";
    s += (unsigned)detail;
    s += ",\"counter\":";
    s += (unsigned long)counter;
    s += "}\n";

    sdcard_append_log(FRAME_LOG_PATH, s);
}

static bool read_error_payload()
{
    if (Gv2Serial.available() < ERROR_PAYLOAD_LEN)
        return false;

    int code = Gv2Serial.read();
    int detail = Gv2Serial.read();
    uint32_t counter = 0;
    if (code < 0 || detail < 0 || !read_u32_le(counter))
        return false;

    error_rx.waiting_error_payload = false;
    stats.bytes += ERROR_PAYLOAD_LEN;
    stats.error_frames++;
    stats.last_error_code = (uint8_t)code;
    stats.last_error_detail = (uint8_t)detail;
    stats.last_error_counter = counter;
    stats.last_state = (uint8_t)code;

    Serial.printf("GV2: error code=%u detail=%u name=%s counter=%lu\n",
                  (unsigned)stats.last_error_code,
                  (unsigned)stats.last_error_detail,
                  gv2_error_name(stats.last_error_code, stats.last_error_detail),
                  (unsigned long)stats.last_error_counter);
    append_gv2_error_log(stats.last_error_code, stats.last_error_detail, stats.last_error_counter);
    return true;
}

static void append_frame_log(bool saved, bool valid, bool filter_match, bool detection_match, bool actuated, bool crc_ok, uint32_t crc_actual, size_t payload_len, const char *filename)
{
    const BaseConfig *cfg = log_config;
    const ModemGnssInfo *gnss = log_gnss;
    StepperConfig stepper = cfg ? cfg->stepper : StepperConfig{};
    InferenceConfig inference = cfg ? cfg->inference : InferenceConfig{};
    float confidence = (float)jpeg_rx.frame_conf_u8 / 255.0f;

    String s;
    s.reserve(900);
    s += "{";
    append_json_field(s, "timestamp", current_timestamp().c_str());
    s += ",";
    append_json_field(s, "device_name", cfg ? cfg->device_name : "vst-base");
    s += ",";
    append_json_field(s, "software_name", VST_BASE_SOFTWARE_NAME);
    s += ",";
    append_json_field(s, "software_version", VST_BASE_SOFTWARE_VERSION);
    s += ",";
    append_json_field(s, "cpu_make", "Espressif");
    s += ",";
    append_json_field(s, "cpu_model", ESP.getChipModel());
    s += ",\"gps\":{\"fix\":";
    s += (gnss && gnss->fix) ? "true" : "false";
    s += ",";
    append_json_field(s, "utc", gnss ? gnss->utc : "");
    s += ",";
    append_json_field(s, "latitude", gnss ? gnss->latitude : "");
    s += ",";
    append_json_field(s, "longitude", gnss ? gnss->longitude : "");
    s += ",";
    append_json_field(s, "satellites", gnss ? gnss->satellites : "");
    s += "},\"inference\":{\"state\":";
    s += (unsigned)jpeg_rx.frame_state;
    s += ",\"class_idx\":";
    s += (unsigned)jpeg_rx.frame_class_idx;
    s += ",\"confidence_u8\":";
    s += (unsigned)jpeg_rx.frame_conf_u8;
    s += ",\"confidence\":";
    s += String(confidence, 3);
    s += ",\"confidence_threshold\":";
    s += String(inference.confidence_threshold, 3);
    s += ",\"detected_class\":";
    s += inference.detected_class;
    s += ",\"occurrence_required\":";
    s += configured_occurrence();
    s += ",\"occurrence_count\":";
    s += (unsigned)jpeg_rx.detection_streak;
    s += ",\"filter_match\":";
    s += filter_match ? "true" : "false";
    s += ",\"detection_match\":";
    s += detection_match ? "true" : "false";
    s += "},\"box\":{\"x\":";
    s += (unsigned)jpeg_rx.frame_bbox_x;
    s += ",\"y\":";
    s += (unsigned)jpeg_rx.frame_bbox_y;
    s += ",\"w\":";
    s += (unsigned)jpeg_rx.frame_bbox_w;
    s += ",\"h\":";
    s += (unsigned)jpeg_rx.frame_bbox_h;
    s += "},\"jpeg\":{\"len\":";
    s += (unsigned)payload_len;
    s += ",\"declared_len\":";
    s += (unsigned long)jpeg_rx.frame_len;
    s += ",\"crc_rx\":\"";
    char hex[16];
    snprintf(hex, sizeof(hex), "%08lX", (unsigned long)jpeg_rx.frame_crc32);
    s += hex;
    s += "\",\"crc_calc\":\"";
    snprintf(hex, sizeof(hex), "%08lX", (unsigned long)crc_actual);
    s += hex;
    s += "\",\"crc_ok\":";
    s += crc_ok ? "true" : "false";
    s += ",";
    append_json_field(s, "filename", filename ? filename : "");
    s += ",\"saved\":";
    s += saved ? "true" : "false";
    s += ",\"valid\":";
    s += valid ? "true" : "false";
    s += "},\"actuation\":{\"angle_degrees\":";
    s += (unsigned)stepper.rotation_degrees;
    s += ",\"speed_steps_per_second\":";
    s += (unsigned)stepper.speed_steps_per_second;
    s += ",\"wait_ms\":";
    s += (unsigned)stepper.reverse_wait_ms;
    s += ",\"start_direction\":\"";
    s += stepper.start_direction;
    s += "\"";
    s += ",\"eligible\":";
    s += detection_match ? "true" : "false";
    s += ",\"activated\":";
    s += actuated ? "true" : "false";
    s += "}}\n";

    sdcard_append_log(FRAME_LOG_PATH, s);
}

static bool read_jpeg_header()
{
    if (Gv2Serial.available() < JPEG_HEADER_LEN)
        return false;

    int st = Gv2Serial.read();
    int cls = Gv2Serial.read();
    int conf = Gv2Serial.read();
    uint16_t bbox_x = 0, bbox_y = 0, bbox_w = 0, bbox_h = 0;
    uint32_t len = 0;
    uint32_t crc32 = 0;

    if (st < 0 || cls < 0 || conf < 0 ||
        !read_u16_le(bbox_x) ||
        !read_u16_le(bbox_y) ||
        !read_u16_le(bbox_w) ||
        !read_u16_le(bbox_h) ||
        !read_u32_le(len) ||
        !read_u32_le(crc32)) {
        return false;
    }

    jpeg_rx.waiting_jpeg_header = false;
    jpeg_rx.frame_state = (uint8_t)st;
    jpeg_rx.frame_class_idx = (uint8_t)cls;
    jpeg_rx.frame_conf_u8 = (uint8_t)conf;
    jpeg_rx.frame_bbox_x = bbox_x;
    jpeg_rx.frame_bbox_y = bbox_y;
    jpeg_rx.frame_bbox_w = bbox_w;
    jpeg_rx.frame_bbox_h = bbox_h;
    jpeg_rx.frame_len = len;
    jpeg_rx.frame_crc32 = crc32;
    jpeg_rx.frame_has_crc32 = true;
    jpeg_rx.legacy_prefix_len = 0;

    if (len == 0 || len > MAX_JPEG_LEN) {
        uint8_t prefix[8];
        u16_to_le(bbox_x, prefix + 0);
        u16_to_le(bbox_y, prefix + 2);
        u16_to_le(bbox_w, prefix + 4);
        u16_to_le(bbox_h, prefix + 6);

        jpeg_rx.frame_bbox_x = 0;
        jpeg_rx.frame_bbox_y = 0;
        jpeg_rx.frame_bbox_w = 0;
        jpeg_rx.frame_bbox_h = 0;
        jpeg_rx.frame_len = ((uint32_t)prefix[0]) |
                            ((uint32_t)prefix[1] << 8) |
                            ((uint32_t)prefix[2] << 16) |
                            ((uint32_t)prefix[3] << 24);
        jpeg_rx.frame_crc32 = ((uint32_t)prefix[4]) |
                              ((uint32_t)prefix[5] << 8) |
                              ((uint32_t)prefix[6] << 16) |
                              ((uint32_t)prefix[7] << 24);
        memcpy(jpeg_rx.legacy_prefix, &len, 4);
        memcpy(jpeg_rx.legacy_prefix + 4, &crc32, 4);
        jpeg_rx.legacy_prefix_len = 8;
    }

    if (crc_field_looks_like_jpeg_prefix(jpeg_rx.frame_crc32)) {
        jpeg_rx.frame_has_crc32 = false;
        jpeg_rx.legacy_prefix[0] = (uint8_t)(jpeg_rx.frame_crc32 & 0xFF);
        jpeg_rx.legacy_prefix[1] = (uint8_t)((jpeg_rx.frame_crc32 >> 8) & 0xFF);
        jpeg_rx.legacy_prefix[2] = (uint8_t)((jpeg_rx.frame_crc32 >> 16) & 0xFF);
        jpeg_rx.legacy_prefix[3] = (uint8_t)((jpeg_rx.frame_crc32 >> 24) & 0xFF);
        jpeg_rx.legacy_prefix_len = 4;
        jpeg_rx.frame_crc32 = 0;
    }

    if (jpeg_rx.frame_len == 0 || jpeg_rx.frame_len > MAX_JPEG_LEN || jpeg_rx.legacy_prefix_len > jpeg_rx.frame_len) {
        stats.jpeg_invalid++;
        Serial.printf("GV2: invalid jpeg len=%lu\n", (unsigned long)jpeg_rx.frame_len);
        return true;
    }

    start_jpeg_frame();
    return true;
}

bool gv2_uart_init(const UartConfig &config)
{
    active_config = config;
    if (active_config.baud == 0)
        active_config.baud = GV2_UART_BAUD_CFG;

    size_t rx_buffer = Gv2Serial.setRxBufferSize(GV2_UART_RX_BUFFER_SIZE);
    Gv2Serial.begin(active_config.baud, SERIAL_8N1, active_config.rx_gpio, active_config.tx_gpio);
    Gv2Serial.setRxTimeout(1);
    Serial.printf("POST: gv2_uart=Serial2 RX=%d TX=%d baud=%lu rx_buffer=%u protocol=VSTJ/VSTS/VSTE+CRC32\n",
                  active_config.rx_gpio,
                  active_config.tx_gpio,
                  (unsigned long)active_config.baud,
                  (unsigned)rx_buffer);
    return true;
}

void gv2_uart_poll()
{
    while (Gv2Serial.available() > 0) {
        if (error_rx.waiting_error_payload) {
            if (!read_error_payload())
                break;
            continue;
        }

        if (jpeg_rx.waiting_jpeg_header) {
            if (!read_jpeg_header())
                break;
            continue;
        }

        if (jpeg_rx.receiving_jpeg) {
            static uint8_t buf[GV2_UART_READ_CHUNK_SIZE];
            int available = Gv2Serial.available();
            if (available <= 0)
                break;

            uint32_t to_read = (uint32_t)available;
            if (to_read > jpeg_rx.jpeg_remaining)
                to_read = jpeg_rx.jpeg_remaining;
            if (to_read > sizeof(buf))
                to_read = sizeof(buf);

            size_t n = Gv2Serial.readBytes(buf, (size_t)to_read);
            if (n == 0)
                break;

            stats.bytes += (uint32_t)n;
            memcpy(jpeg_rx.jpeg_buf + jpeg_rx.jpeg_offset, buf, n);
            jpeg_rx.jpeg_offset += (uint32_t)n;

            jpeg_rx.jpeg_remaining -= (uint32_t)n;
            if (jpeg_rx.jpeg_remaining == 0) {
                size_t payload_len = jpeg_payload_len(jpeg_rx.jpeg_buf, jpeg_rx.frame_len);
                uint32_t crc_actual = payload_len > 0
                    ? vst_crc32_update(0, jpeg_rx.jpeg_buf, payload_len)
                    : 0;
                bool crc_ok = !jpeg_rx.frame_has_crc32 ||
                    (payload_len == jpeg_rx.frame_len && crc_actual == jpeg_rx.frame_crc32);
                bool valid = jpeg_rx.frame_has_crc32 && crc_ok && jpeg_sanity_check(jpeg_rx.jpeg_buf, payload_len);
                bool filter_match = frame_matches_class_and_confidence(valid);
                if (filter_match) {
                    if (jpeg_rx.detection_streak < UINT16_MAX)
                        jpeg_rx.detection_streak++;
                } else {
                    jpeg_rx.detection_streak = 0;
                }

                uint16_t occurrence = configured_occurrence();
                bool detection_match = filter_match && jpeg_rx.detection_streak >= occurrence;
                bool actuated = false;
                char filename[64] = {0};
                bool saved = false;

                if (detection_match) {
                    actuated = stepper_run_configured_cycle();
                    uint32_t discarded = flush_gv2_uart_until_quiet(100, 1200);
                    if (discarded > 0) {
                        Serial.printf("GV2: resync after actuator discarded=%lu bytes\n",
                                      (unsigned long)discarded);
                    }

                    saved = sdcard_save_jpeg(jpeg_rx.image_counter,
                                             jpeg_rx.jpeg_buf,
                                             payload_len,
                                             filename,
                                             sizeof(filename));
                }

                if (!valid)
                    stats.jpeg_invalid++;

                append_frame_log(saved, valid, filter_match, detection_match, actuated, crc_ok, crc_actual, payload_len, saved ? filename : "");

                WebFrameInfo web_info;
                web_info.frame_id = jpeg_rx.image_counter;
                web_info.state = jpeg_rx.frame_state;
                web_info.class_idx = jpeg_rx.frame_class_idx;
                web_info.confidence_u8 = jpeg_rx.frame_conf_u8;
                web_info.bbox_x = jpeg_rx.frame_bbox_x;
                web_info.bbox_y = jpeg_rx.frame_bbox_y;
                web_info.bbox_w = jpeg_rx.frame_bbox_w;
                web_info.bbox_h = jpeg_rx.frame_bbox_h;
                web_info.jpeg_len = (uint32_t)payload_len;
                web_info.crc_rx = jpeg_rx.frame_crc32;
                web_info.crc_calc = crc_actual;
                web_info.crc_ok = crc_ok;
                web_info.valid = valid;
                web_info.filter_match = filter_match;
                web_info.detection_match = detection_match;
                web_info.saved = saved;
                web_info.actuated = actuated;
                web_info.occurrence_count = jpeg_rx.detection_streak;
                web_info.occurrence_required = occurrence;
                if (valid)
                    web_publish_frame(jpeg_rx.jpeg_buf, payload_len, web_info);

                jpeg_rx.receiving_jpeg = false;
                stats.jpeg_frames++;
                Serial.printf("GV2: jpeg complete #%lu len=%lu jpeg_len=%u crc_rx=%08lX crc_calc=%08lX crc_ok=%s protocol=%s state=%u class=%u conf_u8=%u conf=%.3f box=[%u,%u,%u,%u] ram_valid=%s filter_match=%s occurrence=%u/%u detection_match=%s saved=%s file=%s actuator=%s soi=%02X%02X eoi=%02X%02X\n",
                              (unsigned long)jpeg_rx.image_counter,
                              (unsigned long)jpeg_rx.frame_len,
                              (unsigned)payload_len,
                              (unsigned long)jpeg_rx.frame_crc32,
                              (unsigned long)crc_actual,
                              jpeg_rx.frame_has_crc32 ? (crc_ok ? "YES" : "NO") : "SKIP",
                              jpeg_rx.frame_has_crc32 ? "CRC32" : "LEGACY",
                              jpeg_rx.frame_state,
                              jpeg_rx.frame_class_idx,
                              jpeg_rx.frame_conf_u8,
                              (float)jpeg_rx.frame_conf_u8 / 255.0f,
                              jpeg_rx.frame_bbox_x,
                              jpeg_rx.frame_bbox_y,
                              jpeg_rx.frame_bbox_w,
                              jpeg_rx.frame_bbox_h,
                              valid ? "YES" : "NO",
                              filter_match ? "YES" : "NO",
                              jpeg_rx.detection_streak,
                              occurrence,
                              detection_match ? "YES" : "NO",
                              saved ? "YES" : "NO",
                              saved ? filename : "",
                              detection_match ? (actuated ? "activated" : "failed") : "skipped",
                              jpeg_rx.frame_len >= 2 ? jpeg_rx.jpeg_buf[0] : 0,
                              jpeg_rx.frame_len >= 2 ? jpeg_rx.jpeg_buf[1] : 0,
                              payload_len >= 2 ? jpeg_rx.jpeg_buf[payload_len - 2] : 0,
                              payload_len >= 2 ? jpeg_rx.jpeg_buf[payload_len - 1] : 0);
                if (detection_match)
                    jpeg_rx.detection_streak = 0;
                free(jpeg_rx.jpeg_buf);
                jpeg_rx.jpeg_buf = nullptr;
                jpeg_rx.jpeg_offset = 0;
            }
            continue;
        }

        int b = Gv2Serial.read();
        if (b < 0)
            break;

        uint8_t value = (uint8_t)b;
        stats.bytes++;

        shift_magic_window(state_rx.magic_window, &state_rx.magic_filled, value);
        if (magic_matches(state_rx.magic_window, state_rx.magic_filled, STATE_MAGIC)) {
            if (Gv2Serial.available() >= 1) {
                int state = Gv2Serial.read();
                if (state >= 0) {
                    stats.bytes++;
                    stats.state_frames++;
                    stats.last_state = (uint8_t)state;
                }
            }
        }

        shift_magic_window(error_rx.magic_window, &error_rx.magic_filled, value);
        if (magic_matches(error_rx.magic_window, error_rx.magic_filled, ERROR_MAGIC)) {
            error_rx.waiting_error_payload = true;
            (void)read_error_payload();
            continue;
        }

        shift_magic_window(jpeg_rx.magic_window, &jpeg_rx.magic_filled, value);
        if (!magic_matches(jpeg_rx.magic_window, jpeg_rx.magic_filled, JPEG_MAGIC))
            continue;

        jpeg_rx.waiting_jpeg_header = true;
        (void)read_jpeg_header();
    }
}

const Gv2UartStats &gv2_uart_stats()
{
    return stats;
}
