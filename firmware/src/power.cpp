#include "power.h"

#include <Wire.h>
#include <math.h>
#include <time.h>

#ifndef XPOWERS_CHIP_AXP2101
#define XPOWERS_CHIP_AXP2101
#endif
#include <XPowersLib.h>

static constexpr int PMU_I2C_SDA = 15;
static constexpr int PMU_I2C_SCL = 7;
static constexpr const char *POWER_LOG_PATH = "/power.log";

static XPowersPMU PMU;
static PowerConfig g_power_config;
static bool g_power_ready = false;
static uint32_t g_last_log_ms = 0;

static const char *charger_status_name(uint8_t status)
{
    switch (status) {
        case XPOWERS_AXP2101_CHG_TRI_STATE: return "tri_charge";
        case XPOWERS_AXP2101_CHG_PRE_STATE: return "pre_charge";
        case XPOWERS_AXP2101_CHG_CC_STATE: return "constant_current";
        case XPOWERS_AXP2101_CHG_CV_STATE: return "constant_voltage";
        case XPOWERS_AXP2101_CHG_DONE_STATE: return "done";
        case XPOWERS_AXP2101_CHG_STOP_STATE: return "stopped";
        default: return "unknown";
    }
}

static const char *current_direction_name(const PowerSnapshot &snapshot)
{
    if (snapshot.charging)
        return "charge";
    if (snapshot.discharging)
        return "discharge";
    if (snapshot.standby)
        return "standby";
    return "unknown";
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

static String make_power_log_line(const PowerSnapshot &snapshot)
{
    String s;
    s.reserve(640);
    s += "{";
    append_json_string(s, "timestamp", current_timestamp().c_str());
    s += ",\"uptime_ms\":";
    s += (unsigned long)snapshot.uptime_ms;
    s += ",\"battery\":{\"present\":";
    s += snapshot.battery_present ? "true" : "false";
    s += ",\"mv\":";
    s += (unsigned)snapshot.battery_mv;
    s += ",\"percent\":";
    s += snapshot.battery_percent;
    s += ",\"batfet_open\":";
    s += snapshot.batfet_open ? "true" : "false";
    s += "},\"input\":{\"vbus_good\":";
    s += snapshot.vbus_good ? "true" : "false";
    s += ",\"vbus_in\":";
    s += snapshot.vbus_in ? "true" : "false";
    s += ",\"vbus_mv\":";
    s += (unsigned)snapshot.vbus_mv;
    s += "},\"system\":{\"vsys_mv\":";
    s += (unsigned)snapshot.vsys_mv;
    s += ",\"pmu_temp_c\":";
    if (isnan(snapshot.pmu_temp_c))
        s += "null";
    else
        s += String(snapshot.pmu_temp_c, 2);
    s += ",\"thermal_regulation\":";
    s += snapshot.thermal_regulation ? "true" : "false";
    s += ",\"current_limit\":";
    s += snapshot.current_limit ? "true" : "false";
    s += "},\"charger\":{\"direction\":\"";
    s += current_direction_name(snapshot);
    s += "\",\"status\":\"";
    s += charger_status_name(snapshot.charger_status);
    s += "\",\"status_code\":";
    s += (unsigned)snapshot.charger_status;
    s += "}}\n";
    return s;
}

bool power_init(const PowerConfig &config)
{
    g_power_config = config;
    if (g_power_config.log_interval_seconds == 0)
        g_power_config.log_interval_seconds = 60;

    Wire.begin(PMU_I2C_SDA, PMU_I2C_SCL);
    Wire.setClock(400000);

    if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, PMU_I2C_SDA, PMU_I2C_SCL)) {
        Serial.println("POWER: PMU init FAILED");
        g_power_ready = false;
        return false;
    }

    PMU.enableBattVoltageMeasure();
    PMU.enableVbusVoltageMeasure();
    PMU.enableSystemVoltageMeasure();
    PMU.enableTemperatureMeasure();
    PMU.enableBattDetection();
    PMU.disableTSPinMeasure();

    g_power_ready = true;
    uint32_t interval_ms = g_power_config.log_interval_seconds * 1000UL;
    g_last_log_ms = millis() - interval_ms;

    Serial.printf("POWER: PMU telemetry init OK log_interval=%lu seconds\n",
                  (unsigned long)g_power_config.log_interval_seconds);
    return true;
}

bool power_read_snapshot(PowerSnapshot &snapshot)
{
    snapshot = PowerSnapshot{};
    snapshot.uptime_ms = millis();

    if (!g_power_ready)
        return false;

    snapshot.ok = true;
    snapshot.battery_present = PMU.isBatteryConnect();
    snapshot.vbus_good = PMU.isVbusGood();
    snapshot.vbus_in = PMU.isVbusIn();
    snapshot.batfet_open = PMU.getBatfetState();
    snapshot.charging = PMU.isCharging();
    snapshot.discharging = PMU.isDischarge();
    snapshot.standby = PMU.isStandby();
    snapshot.thermal_regulation = PMU.getThermalRegulationStatus();
    snapshot.current_limit = PMU.getCurrentLimitStatus();
    snapshot.charger_status = (uint8_t)PMU.getChargerStatus();
    snapshot.battery_mv = PMU.getBattVoltage();
    snapshot.battery_percent = PMU.getBatteryPercent();
    snapshot.vbus_mv = PMU.getVbusVoltage();
    snapshot.vsys_mv = PMU.getSystemVoltage();
    snapshot.pmu_temp_c = PMU.getTemperature();

    return true;
}

void power_print_snapshot(const PowerSnapshot &snapshot)
{
    Serial.printf("POWER: bat=%dmV soc=%d%% vbus=%umV vbus_good=%s vsys=%umV direction=%s charge=%s pmu_temp=%.2fC thermal=%s current_limit=%s\n",
                  (int)snapshot.battery_mv,
                  snapshot.battery_percent,
                  (unsigned)snapshot.vbus_mv,
                  snapshot.vbus_good ? "YES" : "NO",
                  (unsigned)snapshot.vsys_mv,
                  current_direction_name(snapshot),
                  charger_status_name(snapshot.charger_status),
                  snapshot.pmu_temp_c,
                  snapshot.thermal_regulation ? "YES" : "NO",
                  snapshot.current_limit ? "YES" : "NO");
    Serial.flush();
}

void power_log_snapshot_if_due(const PowerSnapshot &snapshot)
{
    uint32_t now = millis();
    uint32_t interval_ms = g_power_config.log_interval_seconds * 1000UL;
    if (now - g_last_log_ms >= interval_ms) {
        g_last_log_ms = now;
        sdcard_append_log(POWER_LOG_PATH, make_power_log_line(snapshot));
    }
}

void power_poll()
{
    if (!g_power_ready)
        return;

    PowerSnapshot snapshot;
    if (!power_read_snapshot(snapshot))
        return;

    power_print_snapshot(snapshot);
    power_log_snapshot_if_due(snapshot);
}
