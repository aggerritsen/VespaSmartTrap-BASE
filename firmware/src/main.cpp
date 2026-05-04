#include <Arduino.h>
#include <sys/time.h>
#include <time.h>

#include "esp_system.h"

#include "sdcard.h"
#include "modem.h"
#include "stepper.h"
#include "uart.h"
#include "version.h"
#include "web.h"

static char g_timestamp[32] = {0};

struct PostResult {
    bool modem_ready = false;
    bool modem_time = false;
    bool system_time = false;
    bool gnss_ready = false;
    bool gnss_fix = false;
    bool gv2_uart = false;
    bool sd_card = false;
    bool sd_config = false;
};

static PostResult g_post;
static ModemGnssInfo g_gnss;
static BaseConfig g_config;

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
    s += "software_name=";
    s += VST_BASE_SOFTWARE_NAME;
    s += "\n";
    s += "software_version=";
    s += VST_BASE_SOFTWARE_VERSION;
    s += "\n";
    s += "device_name=";
    s += g_config.device_name;
    s += "\n";
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
    s += "gv2_uart=Serial2";
    s += " RX=";
    s += g_config.uart.rx_gpio;
    s += " TX=";
    s += g_config.uart.tx_gpio;
    s += " baud=";
    s += g_config.uart.baud;
    s += " protocol=VSTJ/VSTS+CRC32";
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
    s += "gv2_uart_status=";
    s += g_post.gv2_uart ? "PASS" : "FAIL";
    s += "\n";
    s += "sd_card=";
    s += g_post.sd_card ? "PASS" : "FAIL";
    s += "\n";
    s += "sd_config=";
    s += g_post.sd_config ? "PASS /config.json" : "FAIL";
    s += "\n";
    s += "stepper_speed_steps_per_second=";
    s += g_config.stepper.speed_steps_per_second;
    s += "\n";
    s += "stepper_rotation_degrees=";
    s += g_config.stepper.rotation_degrees;
    s += "\n";
    s += "stepper_reverse_wait_ms=";
    s += g_config.stepper.reverse_wait_ms;
    s += "\n";
    s += "stepper_steps_per_revolution=";
    s += g_config.stepper.steps_per_revolution;
    s += "\n";
    s += "inference_confidence_threshold=";
    s += String(g_config.inference.confidence_threshold, 3);
    s += "\n";
    s += "inference_detected_class=";
    s += g_config.inference.detected_class;
    s += "\n";
    s += "inference_occurrence=";
    s += g_config.inference.occurrence;
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
    Serial.printf("POST: firmware=receiver version=%s build=2026-05-04 debug-com5\n", VST_BASE_SOFTWARE_VERSION);
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
    Serial.flush();
}

static void print_post_summary()
{
    Serial.println("==================================================");
    Serial.println(" POST SUMMARY");
    Serial.println("==================================================");
    Serial.printf("POST: software_version  [%s]\n", VST_BASE_SOFTWARE_VERSION);
    Serial.printf("POST: device_name       [%s]\n", g_config.device_name);
    Serial.printf("POST: stepper_wait_ms   [%u]\n", g_config.stepper.reverse_wait_ms);
    Serial.printf("POST: inference_filter  [class=%d confidence>=%.3f occurrence=%u]\n",
                  g_config.inference.detected_class,
                  g_config.inference.confidence_threshold,
                  g_config.inference.occurrence);
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
    print_post_line("gv2_uart", g_post.gv2_uart);
    print_post_line("sd_card", g_post.sd_card);
    print_post_line("sd_config", g_post.sd_config, g_post.sd_config ? "/config.json" : "unavailable");
    Serial.printf("POST: startup_heap_free=%u\n", ESP.getFreeHeap());
    Serial.println("POST: receiver idle; waiting for GV2 UART traffic");
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

    const Gv2UartStats &uart_stats = gv2_uart_stats();
    Serial.printf("HEARTBEAT: build=post-log-v2 ms=%lu gv2_bytes=%lu gv2_jpegs=%lu gv2_state=%lu heap=%u modem=%s time=%s gnss=%s fix=%s uart=%s sd=%s\n",
                  (unsigned long)now,
                  (unsigned long)uart_stats.bytes,
                  (unsigned long)uart_stats.jpeg_frames,
                  (unsigned long)uart_stats.state_frames,
                  ESP.getFreeHeap(),
                  g_post.modem_ready ? "OK" : "NO",
                  g_post.modem_time ? g_timestamp : "NO",
                  g_post.gnss_ready ? "OK" : "NO",
                  g_post.gnss_fix ? "YES" : "NO",
                  g_post.gv2_uart ? "OK" : "NO",
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

    Serial.println("POST: SD card init begin");
    g_post.sd_card = sdcard_init();
    print_post_line("sd_card", g_post.sd_card);
    g_post.sd_config = sdcard_ensure_config();
    print_post_line("sd_config", g_post.sd_config, g_post.sd_config ? "/config.json" : "unavailable");
    bool config_loaded = sdcard_load_config(g_config);
    print_post_line("config_load", config_loaded, config_loaded ? "loaded" : "defaults");

    bool web_started = web_init(g_config.web);
    print_post_line("web_service", web_started, web_started ? "http port 80" : "disabled/unavailable");

    g_post.gv2_uart = gv2_uart_init(g_config.uart);
    gv2_uart_set_log_context(&g_config, &g_gnss);
    print_post_line("gv2_uart", g_post.gv2_uart);

    stepper_init(g_config.stepper);
    write_post_summary_to_sd();

    print_post_summary();
    stepper_run_post_test_cycle();
}

/* =========================================================
   LOOP
   ========================================================= */
void loop()
{
    gv2_uart_poll();
    web_loop();
    print_idle_heartbeat();
}
