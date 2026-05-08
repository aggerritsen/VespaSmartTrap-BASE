// modem.cpp

#include "modem.h"

#include <math.h>
#include <stdlib.h>
#include <Wire.h>

// XPowers
#ifndef XPOWERS_CHIP_AXP2101
#define XPOWERS_CHIP_AXP2101
#endif
#include <XPowersLib.h>

// TinyGSM
#ifndef TINY_GSM_MODEM_SIM7080
#define TINY_GSM_MODEM_SIM7080
#endif
#ifndef TINY_GSM_RX_BUFFER
#define TINY_GSM_RX_BUFFER 1024
#endif
#include <TinyGsmClient.h>

// -------- Board wiring (T-SIM7080G-S3) --------
static constexpr int PMU_I2C_SDA = 15;
static constexpr int PMU_I2C_SCL = 7;

static constexpr int MODEM_RXD   = 4;
static constexpr int MODEM_TXD   = 5;
static constexpr int MODEM_PWR   = 41;

static constexpr uint32_t MODEM_BAUD = 115200;
// ---------------------------------------------

static XPowersPMU PMU;
static TinyGsm modem(Serial1);

static bool is_plausible_year(int year)
{
    return (year >= 2020 && year <= 2099);
}

static void pwrkey_pulse()
{
    pinMode(MODEM_PWR, OUTPUT);
    digitalWrite(MODEM_PWR, LOW);
    delay(100);
    digitalWrite(MODEM_PWR, HIGH);
    delay(1000);
    digitalWrite(MODEM_PWR, LOW);
}

static bool pmu_enable_modem_rails()
{
    Wire.begin(PMU_I2C_SDA, PMU_I2C_SCL);
    Wire.setClock(400000);

    if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, PMU_I2C_SDA, PMU_I2C_SCL))
    {
        Serial.println("MODEM: PMU init FAILED");
        return false;
    }

    Serial.println("MODEM: PMU init OK");
    PMU.setDC3Voltage(3000);
    PMU.enableDC3();
    Serial.println("MODEM: DCDC3 enabled at 3000mV");

    PMU.setBLDO2Voltage(3300);
    PMU.enableBLDO2();
    Serial.println("MODEM: BLDO2 enabled at 3300mV");

    PMU.disableTSPinMeasure();
    delay(100);

    return true;
}

static bool wait_for_at_ready(uint32_t timeout_ms)
{
    uint32_t start = millis();
    int retry = 0;

    while (millis() - start < timeout_ms)
    {
        if (modem.testAT(1000))
            return true;

        delay(200);

        if (++retry > 15)
        {
            Serial.println("MODEM: AT not ready, pulsing PWRKEY");
            pwrkey_pulse();
            retry = 0;
        }
    }
    return false;
}

static bool is_registered_line(const String &line)
{
    int comma = line.lastIndexOf(',');
    if (comma < 0) return false;

    String stat = line.substring(comma + 1);
    stat.trim();
    return (stat == "1" || stat == "5");
}

static String csv_field(const String &csv, int index)
{
    int start = 0;
    int current = 0;

    for (int i = 0; i <= csv.length(); i++)
    {
        if (i == csv.length() || csv[i] == ',')
        {
            if (current == index)
                return csv.substring(start, i);
            start = i + 1;
            current++;
        }
    }

    return "";
}

static void copy_field(char *dst, size_t dst_len, const String &value)
{
    if (!dst || dst_len == 0)
        return;

    snprintf(dst, dst_len, "%s", value.c_str());
}

static bool has_nonzero_position(const char *latitude, const char *longitude)
{
    if (!latitude || !longitude || !latitude[0] || !longitude[0])
        return false;

    double lat = atof(latitude);
    double lon = atof(longitude);
    return fabs(lat) > 0.000001 || fabs(lon) > 0.000001;
}

static bool parse_gnss_info_line(const String &line_in, ModemGnssInfo &info)
{
    String line = line_in;
    line.trim();

    int prefix = line.indexOf("+CGNSINF:");
    if (prefix >= 0)
        line = line.substring(prefix + 9);
    line.trim();

    copy_field(info.raw, sizeof(info.raw), line);

    String run = csv_field(line, 0);
    String fix = csv_field(line, 1);
    run.trim();
    fix.trim();

    info.powered = (run == "1");
    info.fix = (fix == "1");
    copy_field(info.utc, sizeof(info.utc), csv_field(line, 2));
    copy_field(info.latitude, sizeof(info.latitude), csv_field(line, 3));
    copy_field(info.longitude, sizeof(info.longitude), csv_field(line, 4));
    copy_field(info.altitude_m, sizeof(info.altitude_m), csv_field(line, 5));
    copy_field(info.speed_kph, sizeof(info.speed_kph), csv_field(line, 6));
    copy_field(info.satellites, sizeof(info.satellites), csv_field(line, 14));
    info.satellite_count = (uint8_t)constrain(info.satellites[0] ? atoi(info.satellites) : 0, 0, 255);
    info.position_valid = info.powered &&
                          info.fix &&
                          info.satellite_count >= 3 &&
                          has_nonzero_position(info.latitude, info.longitude);

    return info.powered;
}

static bool wait_for_network_registration(uint32_t timeout_ms)
{
    uint32_t start = millis();

    while (millis() - start < timeout_ms)
    {
        modem.sendAT("+CEREG?");
        if (modem.waitResponse(2000, "+CEREG:") == 1)
        {
            String line = modem.stream.readStringUntil('\n');
            line.trim();
            if (is_registered_line(line))
                return true;
        }

        modem.sendAT("+CREG?");
        if (modem.waitResponse(2000, "+CREG:") == 1)
        {
            String line = modem.stream.readStringUntil('\n');
            line.trim();
            if (is_registered_line(line))
                return true;
        }

        delay(1000);
    }
    return false;
}

bool modem_init_early()
{
    static bool done = false;
    if (done) return true;
    done = true;

    if (!pmu_enable_modem_rails())
        return false;

    Serial1.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RXD, MODEM_TXD);

    Serial.println("MODEM: Serial1 begin baud=115200 RX=4 TX=5");
    Serial.print("MODEM: probing AT");
    if (!wait_for_at_ready(30000))
    {
        Serial.println("\nMODEM: AT probe FAILED");
        return false;
    }
    Serial.println("\nMODEM: AT ready");

    modem.sendAT("+CLTS=1");
    modem.waitResponse(2000);
    Serial.println("MODEM: CLTS command sent");

    modem.sendAT("+CTZR=1");
    modem.waitResponse(2000);
    Serial.println("MODEM: CTZR command sent");

    return true;
}

bool modem_get_timestamp(char *out, size_t out_len, uint32_t network_timeout_ms)
{
    if (!out || out_len < 17)
        return false;

    Serial.println("MODEM: waiting for network registration");
    if (!wait_for_network_registration(network_timeout_ms))
    {
        Serial.println("MODEM: network registration timeout");
        return false;
    }
    Serial.println("MODEM: network registered");

    for (int attempt = 0; attempt < 10; attempt++)
    {
        modem.sendAT("+CCLK?");
        if (modem.waitResponse(3000, "+CCLK:") == 1)
        {
            String line = modem.stream.readStringUntil('\n');
            line.trim();

            int q1 = line.indexOf('\"');
            int q2 = line.indexOf('\"', q1 + 1);
            if (q1 < 0 || q2 < 0) continue;

            String dt = line.substring(q1 + 1, q2);
            if (dt.length() < 17) continue;

            int year = 2000 + dt.substring(0, 2).toInt();
            if (!is_plausible_year(year)) continue;

            snprintf(out, out_len,
                     "%04d%02d%02d_%02d%02d%02d",
                     year,
                     dt.substring(3,5).toInt(),
                     dt.substring(6,8).toInt(),
                     dt.substring(9,11).toInt(),
                     dt.substring(12,14).toInt(),
                     dt.substring(15,17).toInt());

            return true;
        }
        delay(1000);
    }

    return false;
}

bool modem_gnss_probe(ModemGnssInfo &info, uint32_t sample_ms)
{
    info = ModemGnssInfo{};

    Serial.println("GNSS: enabling with AT+CGNSPWR=1");
    modem.sendAT("+CGNSPWR=1");
    if (modem.waitResponse(5000) != 1)
    {
        Serial.println("GNSS: CGNSPWR command FAILED");
        return false;
    }

    info.command_ok = true;
    uint32_t start = millis();
    bool saw_powered_response = false;

    while (millis() - start < sample_ms)
    {
        modem.sendAT("+CGNSINF");
        if (modem.waitResponse(3000, "+CGNSINF:") == 1)
        {
            String line = modem.stream.readStringUntil('\n');
            line.trim();

            if (parse_gnss_info_line(line, info))
            {
                saw_powered_response = true;
                Serial.printf("GNSS: powered=%s fix_raw=%s valid=%s utc=%s lat=%s lon=%s sats=%s\n",
                              info.powered ? "YES" : "NO",
                              info.fix ? "YES" : "NO",
                              info.position_valid ? "YES" : "NO",
                              info.utc[0] ? info.utc : "-",
                              info.latitude[0] ? info.latitude : "-",
                              info.longitude[0] ? info.longitude : "-",
                              info.satellites[0] ? info.satellites : "-");

                if (info.position_valid)
                    return true;
            }
        }

        delay(1000);
    }

    if (saw_powered_response)
    {
        Serial.println("GNSS: probe timeout without confirmed fix");
        return true;
    }

    Serial.println("GNSS: no usable CGNSINF response");
    return info.command_ok;
}
