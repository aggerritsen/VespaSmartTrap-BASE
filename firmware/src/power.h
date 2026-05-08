#pragma once
#include <Arduino.h>
#include <math.h>

#include "sdcard.h"

struct PowerSnapshot {
    bool ok = false;
    uint32_t uptime_ms = 0;
    bool battery_present = false;
    bool vbus_good = false;
    bool vbus_in = false;
    bool batfet_open = false;
    bool charging = false;
    bool discharging = false;
    bool standby = false;
    bool thermal_regulation = false;
    bool current_limit = false;
    uint8_t charger_status = 0;
    uint16_t battery_mv = 0;
    int battery_percent = -1;
    uint16_t vbus_mv = 0;
    uint16_t vsys_mv = 0;
    float pmu_temp_c = NAN;
};

bool power_init(const PowerConfig &config);
bool power_read_snapshot(PowerSnapshot &snapshot);
void power_print_snapshot(const PowerSnapshot &snapshot);
void power_log_snapshot_if_due(const PowerSnapshot &snapshot);
void power_poll();
