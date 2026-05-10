#include <Arduino.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_sleep.h"
#include "esp_system.h"

#include "sdcard.h"
#include "modem.h"
#include "power.h"
#include "stepper.h"
#include "uart.h"
#include "version.h"
#include "web.h"

static char g_timestamp[32] = {0};

struct PostResult {
    bool modem_ready = false;
    bool modem_network = false;
    bool modem_ltem = false;
    bool modem_http = false;
    bool modem_time = false;
    bool gnss_time = false;
    bool system_time = false;
    bool gnss_ready = false;
    bool gnss_fix = false;
    bool gv2_uart = false;
    bool sd_card = false;
    bool sd_config = false;
    bool power = false;
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

static bool current_local_time(struct tm &tm)
{
    if (!g_post.system_time)
        return false;

    time_t now = time(nullptr);
    if (now <= 1700000000)
        return false;

    return localtime_r(&now, &tm) != nullptr;
}

static bool is_hour_in_sleep_window(uint8_t hour, const PowerConfig &config)
{
    uint8_t start = config.deep_sleep_start_hour;
    uint8_t end = config.deep_sleep_end_hour;

    if (start < end)
        return hour >= start && hour < end;

    return hour >= start || hour < end;
}

static uint32_t seconds_until_sleep_window_end(const struct tm &tm, const PowerConfig &config)
{
    int now_seconds = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    int end_seconds = config.deep_sleep_end_hour * 3600;
    int delta = end_seconds - now_seconds;

    if (delta <= 0)
        delta += 24 * 3600;

    return (uint32_t)delta;
}

static const char *deep_sleep_reason()
{
    switch (g_config.power.deep_sleep) {
        case 1: return "scheduled_night_window";
        case 2: return "scheduled_battery_only";
        default: return "disabled";
    }
}

static bool deep_sleep_power_condition_allows(const char *phase)
{
    if (g_config.power.deep_sleep != 2)
        return true;

    static uint32_t last_loop_check_ms = 0;
    if (phase && strcmp(phase, "loop") == 0) {
        uint32_t now_ms = millis();
        uint32_t interval_ms = g_config.power.log_interval_seconds * 1000UL;
        if (interval_ms == 0)
            interval_ms = 60000UL;
        if (last_loop_check_ms != 0 && now_ms - last_loop_check_ms < interval_ms)
            return false;
        last_loop_check_ms = now_ms;
    }

    PowerSnapshot snapshot;
    if (!power_read_snapshot(snapshot)) {
        Serial.printf("POWER: sleep schedule skipped phase=%s reason=power_snapshot_unavailable mode=2\n", phase);
        return false;
    }

    bool external_power_present = snapshot.vbus_good || snapshot.vbus_in || snapshot.vbus_mv > 4200;
    bool battery_only_discharge = snapshot.battery_present && snapshot.discharging && !external_power_present;

    if (!battery_only_discharge) {
        Serial.printf("POWER: sleep schedule skipped phase=%s reason=external_power_or_not_discharging mode=2 battery_present=%s discharging=%s vbus_good=%s vbus_in=%s vbus_mv=%u\n",
                      phase,
                      snapshot.battery_present ? "YES" : "NO",
                      snapshot.discharging ? "YES" : "NO",
                      snapshot.vbus_good ? "YES" : "NO",
                      snapshot.vbus_in ? "YES" : "NO",
                      (unsigned)snapshot.vbus_mv);
        return false;
    }

    Serial.printf("POWER: sleep mode 2 allowed phase=%s battery_present=%s discharging=%s vbus_good=%s vbus_in=%s vbus_mv=%u\n",
                  phase,
                  snapshot.battery_present ? "YES" : "NO",
                  snapshot.discharging ? "YES" : "NO",
                  snapshot.vbus_good ? "YES" : "NO",
                  snapshot.vbus_in ? "YES" : "NO",
                  (unsigned)snapshot.vbus_mv);
    return true;
}

static void enter_deep_sleep_until(uint32_t sleep_seconds)
{
    if (sleep_seconds < 60)
        sleep_seconds = 60;

    const char *reason = deep_sleep_reason();
    Serial.printf("POWER: entering deep sleep for %lu seconds reason=%s\n",
                  (unsigned long)sleep_seconds,
                  reason);
    sdcard_append_log("/power.log", String("{\"event\":\"deep_sleep_enter\",\"seconds\":") +
                                      String((unsigned long)sleep_seconds) +
                                      String(",\"reason\":\"") +
                                      String(reason) +
                                      String("\"}\n"));
    gv2_prepare_for_sleep(g_config.uart);
    modem_prepare_for_sleep();
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_seconds * 1000000ULL);
    esp_deep_sleep_start();
}

static void sleep_if_in_configured_window(const char *phase)
{
    if (!g_config.power.deep_sleep)
        return;

    struct tm tm{};
    if (!current_local_time(tm)) {
        static uint32_t last_loop_no_time_log_ms = 0;
        if (phase && strcmp(phase, "loop") == 0) {
            uint32_t now_ms = millis();
            uint32_t interval_ms = g_config.power.log_interval_seconds * 1000UL;
            if (interval_ms == 0)
                interval_ms = 60000UL;
            if (last_loop_no_time_log_ms != 0 && now_ms - last_loop_no_time_log_ms < interval_ms)
                return;
            last_loop_no_time_log_ms = now_ms;
        }
        Serial.printf("POWER: sleep schedule skipped phase=%s reason=no_valid_system_time\n", phase);
        return;
    }

    if (!is_hour_in_sleep_window((uint8_t)tm.tm_hour, g_config.power))
        return;

    if (!deep_sleep_power_condition_allows(phase))
        return;

    uint32_t sleep_seconds = seconds_until_sleep_window_end(tm, g_config.power);
    Serial.printf("POWER: sleep window active phase=%s mode=%u now=%02d:%02d:%02d window=%02u:00-%02u:00\n",
                  phase,
                  g_config.power.deep_sleep,
                  tm.tm_hour,
                  tm.tm_min,
                  tm.tm_sec,
                  g_config.power.deep_sleep_start_hour,
                  g_config.power.deep_sleep_end_hour);
    enter_deep_sleep_until(sleep_seconds);
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
    s += "post_log=";
    s += g_config.logging.post_log;
    s += "\n";
    s += "image_prefix=";
    s += g_config.logging.image_prefix;
    s += "\n";
    s += "feature_gnss_probe=";
    s += g_config.features.gnss_probe ? "YES" : "NO";
    s += "\n";
    s += "feature_ack_frames=";
    s += g_config.features.ack_frames ? "YES" : "NO";
    s += "\n";
    s += "log_policy=overwrite_at_boot\n";
    s += "timestamp_compact=";
    s += (g_post.modem_time || g_post.gnss_time) ? g_timestamp : "unavailable";
    s += "\n";
    s += "timestamp_source=";
    s += g_post.modem_time ? "modem_network" : (g_post.gnss_time ? "gnss_utc" : "unavailable");
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
    s += " protocol=VSTJ/VSTS/VSTH/VSTE+CRC32";
    s += "\n";
    s += "modem_at=";
    s += g_post.modem_ready ? "PASS" : "FAIL";
    s += "\n";
    s += "modem_mode=";
    s += g_config.modem.mode;
    s += "\n";
    s += "modem_apn=";
    s += g_config.modem.apn;
    s += "\n";
    s += "modem_ltem=";
    s += g_post.modem_ltem ? "PASS" : (g_config.modem.mode == 2 ? "FAIL" : "SKIPPED");
    s += "\n";
    s += "modem_http_world_clock=";
    s += g_post.modem_http ? "PASS" : (g_config.modem.mode == 2 ? "FAIL" : "SKIPPED");
    s += "\n";
    s += "modem_lookup_primary=";
    s += g_config.modem.lookup_primary;
    s += "\n";
    s += "modem_lookup_secondary=";
    s += g_config.modem.lookup_secondary;
    s += "\n";
    s += "modem_timestamp=";
    s += g_post.modem_time ? "PASS " : "FAIL ";
    s += g_post.modem_time ? g_timestamp : "no_valid_network_time";
    s += "\n";
    s += "cellular_time_status=";
    s += g_post.modem_time ? "network_time_valid" : "network_registration_timeout_or_missing_sim";
    s += "\n";
    s += "gnss_timestamp=";
    s += g_post.gnss_time ? "PASS " : "FAIL ";
    s += g_post.gnss_time ? g_timestamp : "unavailable";
    s += "\n";
    s += "gnss_time_status=";
    if (g_post.gnss_time)
        s += "trusted_position_time";
    else if (!g_post.gnss_ready)
        s += "gnss_command_unavailable";
    else if (!g_gnss.position_valid)
        s += "invalid_position_or_missing_gnss_antenna";
    else
        s += "not_used";
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
    s += "gnss_raw_fix=";
    s += g_gnss.fix ? "YES" : "NO";
    s += "\n";
    s += "gnss_position_valid=";
    s += g_gnss.position_valid ? "YES" : "NO";
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
    s += "gnss_satellite_count=";
    s += g_gnss.satellite_count;
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
    s += "stepper_start_direction=";
    s += g_config.stepper.start_direction;
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
    s += "power_log_interval_seconds=";
    s += g_config.power.log_interval_seconds;
    s += "\n";
    s += "health_led=";
    s += g_config.health.led;
    s += "\n";
    s += "power_deep_sleep=";
    s += g_config.power.deep_sleep;
    s += "\n";
    s += "power_deep_sleep_window=";
    if (g_config.power.deep_sleep_start_hour < 10)
        s += "0";
    s += g_config.power.deep_sleep_start_hour;
    s += ":00-";
    if (g_config.power.deep_sleep_end_hour < 10)
        s += "0";
    s += g_config.power.deep_sleep_end_hour;
    s += ":00\n";
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
    Serial.printf("POST: post_log          [%s]\n", g_config.logging.post_log);
    Serial.printf("POST: image_prefix      [%s]\n", g_config.logging.image_prefix);
    Serial.printf("POST: features          [gnss_probe=%s ack_frames=%s]\n",
                  g_config.features.gnss_probe ? "YES" : "NO",
                  g_config.features.ack_frames ? "YES" : "NO");
    Serial.printf("POST: stepper_wait_ms   [%u]\n", g_config.stepper.reverse_wait_ms);
    Serial.printf("POST: stepper_direction [%s]\n", g_config.stepper.start_direction);
    Serial.printf("POST: power_log_every  [%lu seconds]\n", (unsigned long)g_config.power.log_interval_seconds);
    Serial.printf("POST: health_led       [%u]\n", g_config.health.led);
    Serial.printf("POST: modem_mode      [%u] apn=[%s] lookup=[%s,%s]\n",
                  g_config.modem.mode,
                  g_config.modem.apn,
                  g_config.modem.lookup_primary,
                  g_config.modem.lookup_secondary);
    Serial.printf("POST: deep_sleep       [%u %02u:00-%02u:00]\n",
                  g_config.power.deep_sleep,
                  g_config.power.deep_sleep_start_hour,
                  g_config.power.deep_sleep_end_hour);
    Serial.printf("POST: inference_filter  [class=%d confidence>=%.3f occurrence=%u]\n",
                  g_config.inference.detected_class,
                  g_config.inference.confidence_threshold,
                  g_config.inference.occurrence);
    print_post_line("modem_at", g_post.modem_ready);
    if (g_config.modem.mode == 2)
        print_post_line("modem_ltem", g_post.modem_ltem, g_post.modem_ltem ? "bearer/ip validated" : "attach failed");
    else
        print_post_warn("modem_ltem", g_config.modem.mode == 0 ? "skipped mode=0" : "skipped mode=1");
    if (g_config.modem.mode == 2)
        print_post_line("modem_http", g_post.modem_http, g_post.modem_http ? "world clock OK" : "world clock failed");
    print_post_line("modem_timestamp", g_post.modem_time, g_post.modem_time ? g_timestamp : "no valid network time");
    print_post_line("gnss_timestamp", g_post.gnss_time, g_post.gnss_time ? g_timestamp : "unavailable");
    print_post_line("system_time", g_post.system_time);
    print_post_line("gnss_command", g_post.gnss_ready, g_post.gnss_ready ? "AT+CGNSPWR/AT+CGNSINF" : "unavailable");
    if (g_post.gnss_fix)
        print_post_line("gnss_fix", true, "position fix");
    else
        print_post_warn("gnss_fix", g_gnss.fix ? "raw fix rejected" : "no fix yet");
    if (g_post.gnss_ready) {
        Serial.printf("POST: gnss_powered=%s raw_fix=%s valid=%s utc=%s utc_advancing=%s lat=%s lon=%s sats=%s\n",
                      g_gnss.powered ? "YES" : "NO",
                      g_gnss.fix ? "YES" : "NO",
                      g_gnss.position_valid ? "YES" : "NO",
                      g_gnss.utc[0] ? g_gnss.utc : "-",
                      g_gnss.utc_advancing ? "YES" : "NO",
                      g_gnss.latitude[0] ? g_gnss.latitude : "-",
                      g_gnss.longitude[0] ? g_gnss.longitude : "-",
                      g_gnss.satellites[0] ? g_gnss.satellites : "-");
    }
    print_post_line("gv2_uart", g_post.gv2_uart);
    print_post_line("sd_card", g_post.sd_card);
    print_post_line("sd_config", g_post.sd_config, g_post.sd_config ? "/config.json" : "unavailable");
    print_post_line("power_monitor", g_post.power, g_post.power ? "/power.log" : "unavailable");
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
    bool ok = sdcard_write_log(g_config.logging.post_log, summary);
    print_post_line("sd_post_log", ok, ok ? g_config.logging.post_log : "write failed");
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

static bool gnss_utc_to_timestamp(const char *utc, char *out, size_t out_len)
{
    if (!utc || !out || out_len < 16)
        return false;

    if (strlen(utc) < 14)
        return false;

    for (uint8_t i = 0; i < 14; i++) {
        if (!is_digit(utc[i]))
            return false;
    }

    int year = 0;
    if (!parse4(utc, year) || year < 2020 || year > 2099)
        return false;

    snprintf(out, out_len, "%.8s_%.6s", utc, utc + 8);
    return true;
}

static bool apply_gnss_time_if_usable(const char *phase, bool post_style_log)
{
    if (!g_config.time.allow_gnss_fallback || g_post.system_time)
        return g_post.system_time;

    if (!g_config.features.gnss_probe)
        return false;

    if (g_config.modem.mode == 0 || !g_post.modem_ready)
        return false;

    Serial.printf("%s: GNSS time fallback probe begin\n", phase);
    g_post.gnss_ready = modem_gnss_probe(g_gnss, 10000);
    g_post.gnss_fix = g_gnss.position_valid;

    if (post_style_log) {
        print_post_line("gnss_command", g_post.gnss_ready, g_post.gnss_ready ? "AT+CGNSPWR/AT+CGNSINF" : "unavailable");
        if (g_post.gnss_fix)
            print_post_line("gnss_fix", true, "position fix");
        else
            print_post_warn("gnss_fix", g_gnss.fix ? "raw fix rejected" : "no fix yet");
    }

    char gnss_timestamp[32] = {0};
    bool gnss_time_usable = (g_gnss.position_valid || g_gnss.utc_advancing) &&
                            gnss_utc_to_timestamp(g_gnss.utc, gnss_timestamp, sizeof(gnss_timestamp));
    if (!gnss_time_usable) {
        const char *reason = g_gnss.utc_valid ? "GNSS UTC stale/no fix" : "trusted GNSS time unavailable";
        if (post_style_log)
            print_post_warn("gnss_timestamp", reason);
        else
            Serial.printf("%s: GNSS timestamp unavailable reason=%s\n", phase, reason);
        return false;
    }

    strlcpy(g_timestamp, gnss_timestamp, sizeof(g_timestamp));
    g_post.gnss_time = true;
    g_post.system_time = set_system_time_from_timestamp(g_timestamp);
    if (post_style_log)
        print_post_line("gnss_timestamp", g_post.system_time, g_post.system_time ? g_timestamp : "invalid");
    else
        Serial.printf("%s: GNSS timestamp recovered [%s]\n", phase, g_timestamp);
    return g_post.system_time;
}

struct HealthState {
    bool has_error = false;
    bool no_uart_comm = true;
    bool no_inference_detection = true;
    bool gv2_camera_error = false;
    bool no_sim_connect = false;
    bool no_sd = true;
    bool low_power = false;
    bool power_valid = false;
    PowerSnapshot power;
    uint32_t last_diagnosis_ms = 0;
    uint32_t last_bytes = 0;
    uint32_t last_jpegs = 0;
    uint32_t last_state_frames = 0;
    uint32_t last_heartbeat_frames = 0;
    uint32_t last_error_frames = 0;
};

static HealthState g_health;

static const char *yn(bool value)
{
    return value ? "YES" : "NO";
}

static void append_json_string(String &s, const char *name, const char *value)
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

static void append_json_bool(String &s, const char *name, bool value)
{
    s += "\"";
    s += name;
    s += "\":";
    s += value ? "true" : "false";
}

static bool modem_health_ok()
{
    if (g_config.modem.mode == 0)
        return true;
    if (!g_post.modem_ready)
        return false;
    if (g_config.modem.mode == 1)
        return g_post.modem_network && g_post.modem_time && g_post.system_time;
    return g_post.modem_network && g_post.modem_ltem;
}

static void refresh_modem_health()
{
    if (g_config.modem.mode == 0)
        return;

    if (!g_post.modem_ready) {
        Serial.println("MODEM: health recovery init begin");
        g_post.modem_ready = modem_init_early();
        if (!g_post.modem_ready)
            return;
    }

    uint32_t network_timeout_ms = (uint32_t)g_config.time.network_timeout_seconds * 1000UL;
    if (network_timeout_ms == 0)
        network_timeout_ms = 10000UL;

    bool registered = modem_check_network_registered(min<uint32_t>(network_timeout_ms, 5000UL));
    if (!registered) {
        Serial.println("MODEM: health network registration timeout");
        modem_print_sim_network_status();
        if (g_post.modem_network) {
            Serial.println("MODEM: health network registration LOST");
            g_post.modem_time = false;
        }
        g_post.modem_network = false;
        if (!g_post.system_time)
            apply_gnss_time_if_usable("HEALTH", false);
        return;
    }

    bool network_recovered = !g_post.modem_network;
    if (network_recovered)
        Serial.println("MODEM: health network registration OK");
    g_post.modem_network = true;

    if (g_config.modem.mode == 1 && (network_recovered || !g_post.modem_time || !g_post.system_time)) {
        Serial.println("MODEM: health timestamp retry begin");
        if (modem_get_timestamp(g_timestamp, sizeof(g_timestamp), network_timeout_ms)) {
            g_post.modem_time = true;
            g_post.system_time = set_system_time_from_timestamp(g_timestamp);
            Serial.printf("MODEM: health timestamp recovered [%s]\n", g_timestamp);
        } else if (network_recovered) {
            g_post.modem_time = false;
            Serial.println("MODEM: health timestamp recovery FAILED");
        }
    } else if (g_config.modem.mode == 2 && !g_post.modem_ltem) {
        Serial.println("MODEM: health LTE-M retry begin");
        g_post.modem_ltem = modem_validate_ltem(g_config.modem.apn,
                                                g_config.modem.lookup_primary,
                                                g_config.modem.lookup_secondary,
                                                network_timeout_ms);
    }

    if (!g_post.system_time)
        apply_gnss_time_if_usable("HEALTH", false);
}

static void append_health_errors_json(String &s)
{
    bool first = true;
    auto add_error = [&](const char *name) {
        if (!first)
            s += ",";
        s += "\"";
        for (const char *p = name; *p; p++) {
            if (*p == '"' || *p == '\\')
                s += '\\';
            s += *p;
        }
        s += "\"";
        first = false;
    };

    s += "\"errors\":[";
    if (g_health.no_uart_comm)
        add_error("no_uart_comm");
    if (g_health.no_inference_detection)
        add_error("no_inference_detection");
    if (g_health.gv2_camera_error)
        add_error("gv2_camera_error");
    if (g_health.no_sim_connect)
        add_error("no_sim_connect");
    if (g_health.no_sd)
        add_error("no_sd");
    if (g_health.low_power)
        add_error("low_power");
    s += "]";
}

static String make_health_log_line(uint32_t now,
                                   uint32_t delta_bytes,
                                   uint32_t delta_jpegs,
                                   uint32_t delta_state,
                                   uint32_t delta_heartbeats,
                                   uint32_t delta_errors,
                                   const Gv2UartStats &uart_stats)
{
    String s;
    s.reserve(1500);
    s += "{";
    append_json_string(s, "type", "health");
    s += ",";
    append_json_string(s, "timestamp", (g_post.modem_time || g_post.gnss_time) ? g_timestamp : "");
    s += ",\"uptime_ms\":";
    s += (unsigned long)now;
    s += ",";
    append_json_string(s, "state", g_health.has_error ? "ERROR" : "OK");
    s += ",";
    append_health_errors_json(s);
    s += ",\"delta\":{\"gv2_bytes\":";
    s += (unsigned long)delta_bytes;
    s += ",\"gv2_jpegs\":";
    s += (unsigned long)delta_jpegs;
    s += ",\"gv2_state\":";
    s += (unsigned long)delta_state;
    s += ",\"gv2_heartbeats\":";
    s += (unsigned long)delta_heartbeats;
    s += ",\"gv2_errors\":";
    s += (unsigned long)delta_errors;
    s += "},\"gv2\":{\"bytes\":";
    s += (unsigned long)uart_stats.bytes;
    s += ",\"jpegs\":";
    s += (unsigned long)uart_stats.jpeg_frames;
    s += ",\"state_frames\":";
    s += (unsigned long)uart_stats.state_frames;
    s += ",\"heartbeats\":";
    s += (unsigned long)uart_stats.heartbeat_frames;
    s += ",\"heartbeat_status\":";
    s += (unsigned)uart_stats.last_heartbeat_status;
    s += ",\"heartbeat_counter\":";
    s += (unsigned long)uart_stats.last_heartbeat_counter;
    s += ",\"errors\":";
    s += (unsigned long)uart_stats.error_frames;
    s += ",\"last_error_code\":";
    s += (unsigned)uart_stats.last_error_code;
    s += ",\"last_error_detail\":";
    s += (unsigned)uart_stats.last_error_detail;
    s += "},\"modem\":{";
    append_json_bool(s, "ok", modem_health_ok());
    s += ",\"mode\":";
    s += (unsigned)g_config.modem.mode;
    s += ",";
    append_json_bool(s, "at", g_post.modem_ready);
    s += ",";
    append_json_bool(s, "network", g_post.modem_network);
    s += ",";
    append_json_bool(s, "ltem", g_post.modem_ltem);
    s += ",";
    append_json_bool(s, "time", g_post.modem_time);
    s += "},\"time\":{";
    append_json_bool(s, "system", g_post.system_time);
    s += ",";
    append_json_string(s, "compact", (g_post.modem_time || g_post.gnss_time) ? g_timestamp : "");
    s += "},\"gnss\":{";
    append_json_bool(s, "ok", g_post.gnss_ready);
    s += ",";
    append_json_bool(s, "fix", g_post.gnss_fix);
    s += ",";
    append_json_bool(s, "utc_advancing", g_gnss.utc_advancing);
    s += ",";
    append_json_string(s, "utc", g_gnss.utc);
    s += "},\"io\":{";
    append_json_bool(s, "uart", g_post.gv2_uart);
    s += ",";
    append_json_bool(s, "sd", sdcard_available());
    s += ",\"health_led\":";
    s += (unsigned)g_config.health.led;
    s += "},\"power\":{";
    append_json_bool(s, "valid", g_health.power_valid);
    s += ",";
    append_json_bool(s, "low_power", g_health.low_power);
    s += ",\"battery_percent\":";
    s += g_health.power_valid ? g_health.power.battery_percent : -1;
    s += ",\"battery_mv\":";
    s += g_health.power_valid ? (unsigned)g_health.power.battery_mv : 0;
    s += ",\"vbus_mv\":";
    s += g_health.power_valid ? (unsigned)g_health.power.vbus_mv : 0;
    s += "},\"heap_free\":";
    s += ESP.getFreeHeap();
    s += "}\n";
    return s;
}

static bool diagnose_and_print_health()
{
    static bool serial_post_copy_printed = false;
    static uint32_t last_ms = 0;
    static uint32_t diagnosis_count = 0;
    uint32_t now = millis();
    constexpr uint32_t interval_ms = 60000UL;

    if (last_ms == 0) {
        last_ms = now;
        return false;
    }

    if (now - last_ms < interval_ms)
        return false;

    last_ms = now;
    diagnosis_count++;

    if (!serial_post_copy_printed && diagnosis_count > 1) {
        serial_post_copy_printed = true;
        Serial.println();
        Serial.println("POST MONITOR COPY: delayed once so COM5 can attach");
        print_system_info();
        print_post_summary();
    }

    const Gv2UartStats &uart_stats = gv2_uart_stats();
    uint32_t delta_bytes = uart_stats.bytes - g_health.last_bytes;
    uint32_t delta_jpegs = uart_stats.jpeg_frames - g_health.last_jpegs;
    uint32_t delta_state = uart_stats.state_frames - g_health.last_state_frames;
    uint32_t delta_heartbeats = uart_stats.heartbeat_frames - g_health.last_heartbeat_frames;
    uint32_t delta_errors = uart_stats.error_frames - g_health.last_error_frames;

    if (uart_stats.camera_error_active && delta_errors == 0 && (delta_jpegs > 0 || delta_state > 0 || delta_heartbeats > 0)) {
        gv2_uart_clear_camera_error();
        Serial.println("HEALTH: gv2_camera_error cleared by recovered GV2 traffic");
    }

    refresh_modem_health();

    g_health.no_uart_comm = !g_post.gv2_uart || delta_bytes == 0;
    g_health.no_inference_detection = !g_health.no_uart_comm &&
                                      delta_jpegs == 0 &&
                                      delta_heartbeats == 0 &&
                                      delta_state == 0;
    g_health.gv2_camera_error = uart_stats.camera_error_active;
    g_health.no_sim_connect = !modem_health_ok();
    g_health.no_sd = !sdcard_available();
    g_health.power_valid = power_read_snapshot(g_health.power);
    g_health.low_power = false;
    if (g_health.power_valid) {
        bool external_power_present = g_health.power.vbus_good ||
                                      g_health.power.vbus_in ||
                                      g_health.power.vbus_mv > 4200;
        g_health.low_power = g_health.power.battery_present &&
                             !external_power_present &&
                             ((g_health.power.battery_percent >= 0 && g_health.power.battery_percent <= 20) ||
                              (g_health.power.battery_mv > 0 && g_health.power.battery_mv < 3500));
    }

    g_health.has_error = g_health.no_uart_comm ||
                         g_health.no_inference_detection ||
                         g_health.gv2_camera_error ||
                         g_health.no_sim_connect ||
                         g_health.no_sd ||
                         g_health.low_power;
    g_health.last_diagnosis_ms = now;
    g_health.last_bytes = uart_stats.bytes;
    g_health.last_jpegs = uart_stats.jpeg_frames;
    g_health.last_state_frames = uart_stats.state_frames;
    g_health.last_heartbeat_frames = uart_stats.heartbeat_frames;
    g_health.last_error_frames = uart_stats.error_frames;

    Serial.printf("HEALTH: ms=%lu state=%s errors=%s%s%s%s%s%s gv2_delta_bytes=%lu gv2_delta_jpegs=%lu gv2_delta_state=%lu gv2_delta_heartbeats=%lu gv2_delta_errors=%lu battery_pct=%d battery_mv=%u vbus_mv=%u\n",
                  (unsigned long)now,
                  g_health.has_error ? "ERROR" : "OK",
                  g_health.no_uart_comm ? " no_uart_comm" : "",
                  g_health.no_inference_detection ? " no_inference_detection" : "",
                  g_health.gv2_camera_error ? " gv2_camera_error" : "",
                  g_health.no_sim_connect ? " no_sim_connect" : "",
                  g_health.no_sd ? " no_sd" : "",
                  g_health.low_power ? " low_power" : "",
                  (unsigned long)delta_bytes,
                  (unsigned long)delta_jpegs,
                  (unsigned long)delta_state,
                  (unsigned long)delta_heartbeats,
                  (unsigned long)delta_errors,
                  g_health.power_valid ? g_health.power.battery_percent : -1,
                  g_health.power_valid ? (unsigned)g_health.power.battery_mv : 0,
                  g_health.power_valid ? (unsigned)g_health.power.vbus_mv : 0);

    Serial.printf("HEARTBEAT: build=post-log-v2 ms=%lu gv2_bytes=%lu gv2_jpegs=%lu gv2_state=%lu gv2_heartbeats=%lu gv2_heartbeat_status=%u/%lu gv2_errors=%lu gv2_last_error=%u/%u heap=%u modem=%s time=%s gnss=%s fix=%s uart=%s sd=%s health=%s low_power=%s\n",
                  (unsigned long)now,
                  (unsigned long)uart_stats.bytes,
                  (unsigned long)uart_stats.jpeg_frames,
                  (unsigned long)uart_stats.state_frames,
                  (unsigned long)uart_stats.heartbeat_frames,
                  (unsigned)uart_stats.last_heartbeat_status,
                  (unsigned long)uart_stats.last_heartbeat_counter,
                  (unsigned long)uart_stats.error_frames,
                  (unsigned)uart_stats.last_error_code,
                  (unsigned)uart_stats.last_error_detail,
                  ESP.getFreeHeap(),
                  modem_health_ok() ? "OK" : "NO",
                  (g_post.modem_time || g_post.gnss_time) ? g_timestamp : "NO",
                  g_post.gnss_ready ? "OK" : "NO",
                  g_post.gnss_fix ? "YES" : "NO",
                  g_post.gv2_uart ? "OK" : "NO",
                  sdcard_available() ? "OK" : "NO",
                  g_health.has_error ? "ERROR" : "OK",
                  yn(g_health.low_power));
    sdcard_append_log("/health.log",
                      make_health_log_line(now,
                                           delta_bytes,
                                           delta_jpegs,
                                           delta_state,
                                           delta_heartbeats,
                                           delta_errors,
                                           uart_stats));
    Serial.flush();
    return true;
}

static void print_power_after_heartbeat()
{
    power_log_snapshot_when_due();
}

static void poll_status_led()
{
    static bool led_on = false;
    static uint32_t cycle_start_ms = 0;
    static bool last_error_state = false;

    if (g_config.health.led == 0) {
        if (led_on) {
            led_on = false;
            stepper_set_status_led(false);
        }
        return;
    }

    uint32_t now = millis();
    bool error_state = g_health.has_error;
    if (cycle_start_ms == 0 || error_state != last_error_state || now - cycle_start_ms >= 10000UL) {
        cycle_start_ms = now;
        last_error_state = error_state;
    }

    uint32_t phase = now - cycle_start_ms;
    bool desired = false;

    if (!error_state) {
        desired = phase < 500UL;
    } else {
        desired = (phase < 120UL) ||
                  (phase >= 240UL && phase < 360UL) ||
                  (phase >= 480UL && phase < 600UL);
    }

    if (desired != led_on) {
        led_on = desired;
        stepper_set_status_led(led_on);
    }
}

/* =========================================================
   SETUP
   ========================================================= */
void setup()
{
    Serial.begin(115200);
    wait_for_serial(5000);
    delay(100);
    gv2_power_on();

    print_system_info();

    Serial.println("POST: SD card init begin");
    g_post.sd_card = sdcard_init();
    print_post_line("sd_card", g_post.sd_card);
    g_post.sd_config = sdcard_ensure_config();
    print_post_line("sd_config", g_post.sd_config, g_post.sd_config ? "/config.json" : "unavailable");
    bool config_loaded = sdcard_load_config(g_config);
    print_post_line("config_load", config_loaded, config_loaded ? "loaded" : "defaults");

    if (g_config.modem.mode == 0) {
        Serial.println("POST: modem skipped by config mode=0 (no SIM/no modem)");
        print_post_warn("modem_at", "skipped mode=0");
        print_post_warn("modem_timestamp", "skipped mode=0");
        print_post_warn("gnss_command", "skipped mode=0");
    } else {
        Serial.printf("POST: modem init begin mode=%u\n", g_config.modem.mode);
    }

    if (g_config.modem.mode != 0 && modem_init_early()) {
        g_post.modem_ready = true;
        print_post_line("modem_at", true);

        uint32_t network_timeout_ms = (uint32_t)g_config.time.network_timeout_seconds * 1000UL;
        if (g_config.modem.mode == 2) {
            Serial.println("POST: LTE-M validation begin");
            g_post.modem_ltem = modem_validate_ltem(g_config.modem.apn,
                                                    g_config.modem.lookup_primary,
                                                    g_config.modem.lookup_secondary,
                                                    network_timeout_ms);
            g_post.modem_network = g_post.modem_ltem;
            print_post_line("modem_ltem", g_post.modem_ltem, g_post.modem_ltem ? "bearer/ip validated" : "attach failed");
            if (g_post.modem_ltem) {
                g_post.modem_http = modem_test_world_clock();
                print_post_line("modem_http", g_post.modem_http, g_post.modem_http ? "world clock OK" : "world clock failed");
            }
        }

        Serial.println("POST: modem timestamp begin; waiting for network time");
        if (modem_get_timestamp(g_timestamp, sizeof(g_timestamp), network_timeout_ms)) {
            g_post.modem_network = true;
            g_post.modem_time = true;
            Serial.printf("POST: modem_timestamp [%s]\n", g_timestamp);
            g_post.system_time = set_system_time_from_timestamp(g_timestamp);
            print_post_line("system_time", g_post.system_time);
        } else {
            print_post_line("modem_timestamp", false, "unavailable");
        }

        bool should_probe_gnss = g_config.features.gnss_probe &&
                                 (g_config.modem.mode == 2 ||
                                  (!g_post.system_time && g_config.time.allow_gnss_fallback));
        if (should_probe_gnss) {
            Serial.println("POST: GNSS probe begin; sampling up to 10 seconds for fix");
            g_post.gnss_ready = modem_gnss_probe(g_gnss, 10000);
            g_post.gnss_fix = g_gnss.position_valid;
            print_post_line("gnss_command", g_post.gnss_ready, g_post.gnss_ready ? "AT+CGNSPWR/AT+CGNSINF" : "unavailable");
            if (g_post.gnss_fix)
                print_post_line("gnss_fix", true, "position fix");
            else
                print_post_warn("gnss_fix", g_gnss.fix ? "raw fix rejected" : "no fix yet");
        } else {
            print_post_warn("gnss_command", "skipped mode=1");
        }

        if (!g_post.system_time && g_config.time.allow_gnss_fallback) {
            char gnss_timestamp[32] = {0};
            bool gnss_time_usable = (g_gnss.position_valid || g_gnss.utc_advancing) &&
                                    gnss_utc_to_timestamp(g_gnss.utc, gnss_timestamp, sizeof(gnss_timestamp));
            if (gnss_time_usable) {
                strlcpy(g_timestamp, gnss_timestamp, sizeof(g_timestamp));
                g_post.gnss_time = true;
                g_post.system_time = set_system_time_from_timestamp(g_timestamp);
                print_post_line("gnss_timestamp", g_post.system_time, g_post.system_time ? g_timestamp : "invalid");
            } else {
                print_post_warn("gnss_timestamp", g_gnss.utc_valid ? "GNSS UTC stale/no fix" : "trusted GNSS time unavailable");
            }
        }
    } else if (g_config.modem.mode != 0) {
        print_post_line("modem_at", false);
    }
    Serial.flush();

    g_post.power = power_init(g_config.power);
    print_post_line("power_monitor", g_post.power, g_post.power ? "/power.log" : "unavailable");

    sleep_if_in_configured_window("post_time_sync");

    bool web_started = web_init(g_config.web);
    print_post_line("web_service", web_started, web_started ? "http port 80" : "disabled/unavailable");

    stepper_init(g_config.stepper);
    stepper_run_post_test_cycle();

    g_post.gv2_uart = gv2_uart_init(g_config.uart);
    gv2_uart_set_log_context(&g_config, &g_gnss);
    print_post_line("gv2_uart", g_post.gv2_uart);

    write_post_summary_to_sd();
    print_post_summary();
}

/* =========================================================
   LOOP
   ========================================================= */
void loop()
{
    sleep_if_in_configured_window("loop");
    gv2_uart_poll();
    web_loop();
    poll_status_led();
    if (diagnose_and_print_health())
        print_power_after_heartbeat();
}
