#include <Arduino.h>
#include <sys/time.h>
#include <time.h>

#include "driver/uart.h"
#include "esp_crc.h"
#include "esp_system.h"
#include "mbedtls/base64.h"

#include "sdcard.h"
#include "modem.h"

/* =========================================================
   BROKER UART CONFIG
   ========================================================= */
static constexpr uart_port_t BROKER_UART = UART_NUM_2;
static constexpr int BROKER_RX_PIN = 18;    // P1.4
static constexpr int BROKER_TX_PIN = 17;    // P1.3
static constexpr int BROKER_BAUD   = 921600;
static constexpr int BROKER_BUF_SZ = 4096;

/* =========================================================
   RX STATE MACHINE
   ========================================================= */
enum RxState {
    WAIT_JSON,
    WAIT_IMAGE_HEADER,
    READ_IMAGE,
    WAIT_END
};

static RxState rx_state = WAIT_JSON;

/* =========================================================
   FRAME DATA
   ========================================================= */
static String   json_buffer;
static String   image_base64;
static size_t   image_expected_len = 0;
static uint32_t image_expected_crc = 0;
static uint32_t frame_id = 0;

static char g_timestamp[32] = {0};
static uint32_t g_rx_bytes = 0;
static uint32_t g_rx_lines = 0;

struct PostResult {
    bool modem_ready = false;
    bool modem_time = false;
    bool system_time = false;
    bool gnss_ready = false;
    bool gnss_fix = false;
    bool broker_uart = false;
    bool sd_card = false;
};

static PostResult g_post;
static ModemGnssInfo g_gnss;

/* =========================================================
   UTIL
   ========================================================= */
static void wait_for_serial(uint32_t timeout_ms)
{
    uint32_t start = millis();
    while (!Serial && (millis() - start < timeout_ms)) {
        delay(10);
    }
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXTERNAL";
        case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "UNKNOWN";
    }
}

static void print_post_line(const char *name, bool pass, const char *detail = nullptr)
{
    Serial.printf("POST: %-18s [%s]", name, pass ? "PASS" : "FAIL");
    if (detail && detail[0] != '\0')
        Serial.printf(" %s", detail);
    Serial.println();
}

static void print_post_warn(const char *name, const char *detail)
{
    Serial.printf("POST: %-18s [WARN]", name);
    if (detail && detail[0] != '\0')
        Serial.printf(" %s", detail);
    Serial.println();
}

static String current_time_text()
{
    if (!g_post.system_time)
        return "unavailable";

    time_t now = time(nullptr);
    struct tm tm{};
    localtime_r(&now, &tm);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return String(buf);
}

static String make_post_summary_text()
{
    String s;
    s.reserve(900);

    s += "==================================================\n";
    s += "POST SUMMARY\n";
    s += "==================================================\n";
    s += "firmware=receiver build=2026-05-04 post-log-v2\n";
    s += "log_policy=overwrite_at_boot\n";
    s += "modem_timestamp_compact=";
    s += g_post.modem_time ? g_timestamp : "unavailable";
    s += "\n";
    s += "system_time_local=";
    s += current_time_text();
    s += "\n";
    s += "reset_reason=";
    s += reset_reason_name(esp_reset_reason());
    s += " (";
    s += (int)esp_reset_reason();
    s += ")\n";
    s += "sdk=";
    s += ESP.getSdkVersion();
    s += "\n";
    s += "chip=";
    s += ESP.getChipModel();
    s += " rev=";
    s += ESP.getChipRevision();
    s += " cores=";
    s += ESP.getChipCores();
    s += " cpu_mhz=";
    s += ESP.getCpuFreqMHz();
    s += "\n";
    s += "flash_size=";
    s += ESP.getFlashChipSize();
    s += " sketch_size=";
    s += ESP.getSketchSize();
    s += " free_sketch=";
    s += ESP.getFreeSketchSpace();
    s += "\n";
    s += "heap_free=";
    s += ESP.getFreeHeap();
    s += " heap_min=";
    s += ESP.getMinFreeHeap();
    s += " heap_size=";
    s += ESP.getHeapSize();
    s += "\n";
    s += "psram_found=";
    s += psramFound() ? "YES" : "NO";
    s += " psram_size=";
    s += ESP.getPsramSize();
    s += " psram_free=";
    s += ESP.getFreePsram();
    s += "\n";
    s += "efuse_mac=";
    char mac[16];
    snprintf(mac, sizeof(mac), "%012llX", ESP.getEfuseMac());
    s += mac;
    s += "\n";
    s += "usb_serial=COM5 baud=115200\n";
    s += "broker_uart=UART";
    s += (int)BROKER_UART;
    s += " RX=";
    s += BROKER_RX_PIN;
    s += " TX=";
    s += BROKER_TX_PIN;
    s += " baud=";
    s += BROKER_BAUD;
    s += " buffer=";
    s += BROKER_BUF_SZ;
    s += "\n";
    s += "modem_at=";
    s += g_post.modem_ready ? "PASS" : "FAIL";
    s += "\n";
    s += "modem_timestamp=";
    s += g_post.modem_time ? "PASS " : "FAIL ";
    s += g_post.modem_time ? g_timestamp : "no_valid_network_time";
    s += "\n";
    s += "system_time=";
    s += g_post.system_time ? "PASS" : "FAIL";
    s += "\n";
    s += "gnss_command=";
    s += g_post.gnss_ready ? "PASS" : "FAIL";
    s += "\n";
    s += "gnss_powered=";
    s += g_gnss.powered ? "YES" : "NO";
    s += "\n";
    s += "gnss_fix=";
    s += g_post.gnss_fix ? "YES" : "NO";
    s += "\n";
    s += "gnss_utc=";
    s += g_gnss.utc[0] ? g_gnss.utc : "unavailable";
    s += "\n";
    s += "gnss_latitude=";
    s += g_gnss.latitude[0] ? g_gnss.latitude : "unavailable";
    s += "\n";
    s += "gnss_longitude=";
    s += g_gnss.longitude[0] ? g_gnss.longitude : "unavailable";
    s += "\n";
    s += "gnss_altitude_m=";
    s += g_gnss.altitude_m[0] ? g_gnss.altitude_m : "unavailable";
    s += "\n";
    s += "gnss_speed_kph=";
    s += g_gnss.speed_kph[0] ? g_gnss.speed_kph : "unavailable";
    s += "\n";
    s += "gnss_satellites_view=";
    s += g_gnss.satellites[0] ? g_gnss.satellites : "unavailable";
    s += "\n";
    s += "gnss_raw=";
    s += g_gnss.raw[0] ? g_gnss.raw : "unavailable";
    s += "\n";
    s += "broker_uart_status=";
    s += g_post.broker_uart ? "PASS" : "FAIL";
    s += "\n";
    s += "sd_card=";
    s += g_post.sd_card ? "PASS" : "FAIL";
    s += "\n";
    s += "==================================================\n\n";

    return s;
}

static void print_system_info()
{
    Serial.println();
    Serial.println("==================================================");
    Serial.println(" POWER-ON SELF TEST: T-SIM7080G-S3 RECEIVER");
    Serial.println("==================================================");
    Serial.println("POST: firmware=receiver build=2026-05-04 debug-com5");
    Serial.printf("POST: reset_reason=%s (%d)\n",
                  reset_reason_name(esp_reset_reason()),
                  (int)esp_reset_reason());
    Serial.printf("POST: sdk=%s\n", ESP.getSdkVersion());
    Serial.printf("POST: chip=%s rev=%u cores=%u cpu=%uMHz\n",
                  ESP.getChipModel(),
                  ESP.getChipRevision(),
                  ESP.getChipCores(),
                  ESP.getCpuFreqMHz());
    Serial.printf("POST: flash_size=%u sketch_size=%u free_sketch=%u\n",
                  ESP.getFlashChipSize(),
                  ESP.getSketchSize(),
                  ESP.getFreeSketchSpace());
    Serial.printf("POST: heap_free=%u heap_min=%u heap_size=%u\n",
                  ESP.getFreeHeap(),
                  ESP.getMinFreeHeap(),
                  ESP.getHeapSize());
    Serial.printf("POST: psram_found=%s psram_size=%u psram_free=%u\n",
                  psramFound() ? "YES" : "NO",
                  ESP.getPsramSize(),
                  ESP.getFreePsram());
    Serial.printf("POST: efuse_mac=%012llX\n", ESP.getEfuseMac());
    Serial.printf("POST: usb_serial=COM5 baud=115200\n");
    Serial.printf("POST: broker_uart=UART%d RX=%d TX=%d baud=%d buffer=%d\n",
                  (int)BROKER_UART,
                  BROKER_RX_PIN,
                  BROKER_TX_PIN,
                  BROKER_BAUD,
                  BROKER_BUF_SZ);
    Serial.flush();
}

static void print_post_summary()
{
    Serial.println("==================================================");
    Serial.println(" POST SUMMARY");
    Serial.println("==================================================");
    print_post_line("modem_at", g_post.modem_ready);
    print_post_line("modem_timestamp", g_post.modem_time, g_post.modem_time ? g_timestamp : "no valid network time");
    print_post_line("system_time", g_post.system_time);
    print_post_line("gnss_command", g_post.gnss_ready, g_post.gnss_ready ? "AT+CGNSPWR/AT+CGNSINF" : "unavailable");
    if (g_post.gnss_fix)
        print_post_line("gnss_fix", true, "position fix");
    else
        print_post_warn("gnss_fix", "no fix yet");
    if (g_post.gnss_ready) {
        Serial.printf("POST: gnss_powered=%s utc=%s lat=%s lon=%s sats=%s\n",
                      g_gnss.powered ? "YES" : "NO",
                      g_gnss.utc[0] ? g_gnss.utc : "-",
                      g_gnss.latitude[0] ? g_gnss.latitude : "-",
                      g_gnss.longitude[0] ? g_gnss.longitude : "-",
                      g_gnss.satellites[0] ? g_gnss.satellites : "-");
    }
    print_post_line("broker_uart", g_post.broker_uart);
    print_post_line("sd_card", g_post.sd_card);
    Serial.printf("POST: startup_heap_free=%u\n", ESP.getFreeHeap());
    Serial.println("POST: receiver idle; waiting for broker UART traffic");
    Serial.println("==================================================");
    Serial.flush();
}

static void write_post_summary_to_sd()
{
    if (!sdcard_available())
        return;

    String summary = make_post_summary_text();
    bool ok = sdcard_write_log("/post.log", summary);
    print_post_line("sd_post_log", ok, ok ? "/post.log" : "write failed");
}

static void reset_frame()
{
    json_buffer = "";
    image_base64 = "";
    image_expected_len = 0;
    image_expected_crc = 0;
    rx_state = WAIT_JSON;
}

static bool is_digit(char c)
{
    return (c >= '0' && c <= '9');
}

static bool parse2(const char *p, int &out)
{
    if (!is_digit(p[0]) || !is_digit(p[1])) return false;
    out = (p[0] - '0') * 10 + (p[1] - '0');
    return true;
}

static bool parse4(const char *p, int &out)
{
    if (!is_digit(p[0]) || !is_digit(p[1]) ||
        !is_digit(p[2]) || !is_digit(p[3])) return false;
    out = (p[0] - '0') * 1000 +
          (p[1] - '0') * 100 +
          (p[2] - '0') * 10 +
          (p[3] - '0');
    return true;
}

/* =========================================================
   SYSTEM TIME SET
   ========================================================= */
static bool set_system_time_from_timestamp(const char *ts)
{
    if (!ts || strlen(ts) != 15 || ts[8] != '_')
        return false;

    int year, mon, day, hour, min, sec;

    if (!parse4(ts + 0, year) ||
        !parse2(ts + 4, mon)  ||
        !parse2(ts + 6, day)  ||
        !parse2(ts + 9, hour) ||
        !parse2(ts + 11, min) ||
        !parse2(ts + 13, sec))
        return false;

    struct tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon  = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min  = min;
    tm.tm_sec  = sec;
    tm.tm_isdst = -1;

    time_t t = mktime(&tm);
    if (t < 0)
        return false;

    struct timeval tv{};
    tv.tv_sec = t;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    Serial.print("TIME: system time set ");
    Serial.println(buf);

    return true;
}

/* =========================================================
   JPEG SANITY CHECK
   ========================================================= */
static bool jpeg_sanity_check(const uint8_t *buf, size_t len)
{
    if (len < 4 || buf[0] != 0xFF || buf[1] != 0xD8)
        return false;

    bool found_sos = false;
    bool found_eoi = false;

    for (size_t i = 2; i + 1 < len; i++) {
        if (buf[i] != 0xFF) continue;
        uint8_t marker = buf[i + 1];
        if (marker == 0x00) continue;
        if (marker == 0xDA) found_sos = true;
        if (marker == 0xD9) { found_eoi = true; break; }
    }

    return found_sos && found_eoi;
}

/* =========================================================
   BASE64 → JPEG
   ========================================================= */
static bool decode_base64_to_jpeg(
    const String &b64,
    uint8_t **out_buf,
    size_t *out_len
)
{
    size_t decoded_len = 0;

    int rc = mbedtls_base64_decode(
        nullptr, 0, &decoded_len,
        (const unsigned char*)b64.c_str(),
        b64.length()
    );

    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
        return false;

    uint8_t *buf = (uint8_t*)heap_caps_malloc(decoded_len, MALLOC_CAP_8BIT);
    if (!buf) return false;

    rc = mbedtls_base64_decode(
        buf, decoded_len, out_len,
        (const unsigned char*)b64.c_str(),
        b64.length()
    );

    if (rc != 0) {
        free(buf);
        return false;
    }

    *out_buf = buf;
    return true;
}

/* =========================================================
   BROKER UART INIT
   ========================================================= */
static bool broker_uart_init()
{
    Serial.println("POST: broker UART init begin");

    uart_config_t cfg {
        .baud_rate = BROKER_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB
    };

    esp_err_t err = uart_driver_install(BROKER_UART, BROKER_BUF_SZ, BROKER_BUF_SZ, 0, nullptr, 0);
    if (err != ESP_OK) {
        Serial.printf("POST: broker UART driver install FAILED err=%d\n", (int)err);
        Serial.flush();
        return false;
    }

    err = uart_param_config(BROKER_UART, &cfg);
    if (err != ESP_OK) {
        Serial.printf("POST: broker UART param config FAILED err=%d\n", (int)err);
        Serial.flush();
        return false;
    }

    err = uart_set_pin(BROKER_UART, BROKER_TX_PIN, BROKER_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        Serial.printf("POST: broker UART pin config FAILED err=%d\n", (int)err);
        Serial.flush();
        return false;
    }

    Serial.printf(
        "POST: broker UART configured RX=%d TX=%d baud=%d buffer=%d\n",
        BROKER_RX_PIN, BROKER_TX_PIN, BROKER_BAUD, BROKER_BUF_SZ
    );
    Serial.flush();
    return true;
}

static void print_idle_heartbeat()
{
    static bool serial_post_copy_printed = false;
    static uint32_t last_ms = 0;
    uint32_t now = millis();

    if (now - last_ms < 5000)
        return;

    last_ms = now;

    if (!serial_post_copy_printed) {
        serial_post_copy_printed = true;
        Serial.println();
        Serial.println("POST MONITOR COPY: delayed once so COM5 can attach");
        print_system_info();
        print_post_summary();
    }

    Serial.printf("HEARTBEAT: build=post-log-v2 ms=%lu state=%d frame=%lu rx_bytes=%lu rx_lines=%lu heap=%u modem=%s time=%s gnss=%s fix=%s uart=%s sd=%s\n",
                  (unsigned long)now,
                  (int)rx_state,
                  (unsigned long)frame_id,
                  (unsigned long)g_rx_bytes,
                  (unsigned long)g_rx_lines,
                  ESP.getFreeHeap(),
                  g_post.modem_ready ? "OK" : "NO",
                  g_post.modem_time ? g_timestamp : "NO",
                  g_post.gnss_ready ? "OK" : "NO",
                  g_post.gnss_fix ? "YES" : "NO",
                  g_post.broker_uart ? "OK" : "NO",
                  sdcard_available() ? "OK" : "NO");
    Serial.flush();
}

/* =========================================================
   SETUP
   ========================================================= */
void setup()
{
    Serial.begin(115200);
    wait_for_serial(5000);
    delay(100);

    print_system_info();

    Serial.println("POST: modem init begin");
    if (modem_init_early()) {
        g_post.modem_ready = true;
        print_post_line("modem_at", true);
        Serial.println("POST: modem timestamp begin; waiting for network time");
        if (modem_get_timestamp(g_timestamp, sizeof(g_timestamp))) {
            g_post.modem_time = true;
            Serial.printf("POST: modem_timestamp [%s]\n", g_timestamp);
            g_post.system_time = set_system_time_from_timestamp(g_timestamp);
            print_post_line("system_time", g_post.system_time);
        } else {
            print_post_line("modem_timestamp", false, "unavailable");
        }

        Serial.println("POST: GNSS probe begin; sampling up to 10 seconds for fix");
        g_post.gnss_ready = modem_gnss_probe(g_gnss, 10000);
        g_post.gnss_fix = g_gnss.fix;
        print_post_line("gnss_command", g_post.gnss_ready, g_post.gnss_ready ? "AT+CGNSPWR/AT+CGNSINF" : "unavailable");
        if (g_post.gnss_fix)
            print_post_line("gnss_fix", true, "position fix");
        else
            print_post_warn("gnss_fix", "no fix yet");
    } else {
        print_post_line("modem_at", false);
    }
    Serial.flush();

    g_post.broker_uart = broker_uart_init();
    print_post_line("broker_uart", g_post.broker_uart);

    Serial.println("POST: SD card init begin");
    g_post.sd_card = sdcard_init();
    print_post_line("sd_card", g_post.sd_card);
    write_post_summary_to_sd();

    print_post_summary();
}

/* =========================================================
   LOOP
   ========================================================= */
void loop()
{
    print_idle_heartbeat();

    uint8_t c;
    if (uart_read_bytes(BROKER_UART, &c, 1, 20 / portTICK_PERIOD_MS) <= 0)
        return;
    g_rx_bytes++;

    static String line;

    if (rx_state == READ_IMAGE) {
        image_base64 += (char)c;
        if (image_base64.length() >= image_expected_len)
            rx_state = WAIT_END;
        return;
    }

    if (c != '\n') {
        line += (char)c;
        return;
    }

    line.trim();
    g_rx_lines++;

    /* ---------- GLOBAL RESYNC ON JSON ---------- */
    if (line.startsWith("JSON ")) {
        reset_frame();

        json_buffer = line.substring(5);
        int idx = json_buffer.indexOf("\"frame\":");
        if (idx >= 0)
            frame_id = json_buffer.substring(idx + 8).toInt();

        Serial.println("🧠 INFERENCE");
        Serial.printf("Frame      : %lu\n", frame_id);
        Serial.println(json_buffer);

        rx_state = WAIT_IMAGE_HEADER;
        line = "";
        return;
    }

    if (rx_state == WAIT_IMAGE_HEADER && line.startsWith("IMAGE ")) {
        sscanf(line.c_str(), "IMAGE %zu %lx",
               &image_expected_len, &image_expected_crc);

        image_base64.reserve(image_expected_len);
        rx_state = READ_IMAGE;
    }
    else if (rx_state == WAIT_END && line == "END") {
        uint32_t crc = esp_crc32_le(
            0,
            (const uint8_t*)image_base64.c_str(),
            image_base64.length()
        );

        if (crc == image_expected_crc) {
            uint8_t *jpeg = nullptr;
            size_t jpeg_len = 0;

            if (decode_base64_to_jpeg(image_base64, &jpeg, &jpeg_len) &&
                jpeg_sanity_check(jpeg, jpeg_len) &&
                sdcard_available())
            {
                sdcard_save_jpeg(frame_id, jpeg, jpeg_len);
            }

            free(jpeg);

            String ack = "ACK " + String(frame_id) + "\n";
            uart_write_bytes(BROKER_UART, ack.c_str(), ack.length());
        }

        reset_frame();
    }

    line = "";
}
