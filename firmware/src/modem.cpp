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
static bool g_pmu_ready = false;
static bool g_serial_ready = false;

static bool is_plausible_year(int year)
{
    return (year >= 2020 && year <= 2099);
}

static bool is_plausible_gnss_utc(const char *utc)
{
    if (!utc)
        return false;

    for (int i = 0; i < 14; i++) {
        if (utc[i] < '0' || utc[i] > '9')
            return false;
    }

    char year_buf[5] = {utc[0], utc[1], utc[2], utc[3], 0};
    return is_plausible_year(atoi(year_buf));
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
        g_pmu_ready = false;
        return false;
    }

    g_pmu_ready = true;
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

static bool read_at_prefixed_line(const char *cmd, const char *prefix, String &line, uint32_t timeout_ms)
{
    line = "";
    modem.sendAT(cmd);
    if (modem.waitResponse(timeout_ms, prefix) != 1)
        return false;

    line = modem.stream.readStringUntil('\n');
    line.trim();
    modem.waitResponse(200);
    return true;
}

static void print_at_raw_response(const char *label, const char *cmd, uint32_t timeout_ms)
{
    Serial.printf("MODEM: %s query AT%s\n", label, cmd);
    modem.sendAT(cmd);

    uint32_t start = millis();
    bool printed = false;
    while (millis() - start < timeout_ms) {
        while (modem.stream.available()) {
            String line = modem.stream.readStringUntil('\n');
            line.trim();
            if (!line.length())
                continue;

            Serial.printf("MODEM: %s rx [%s]\n", label, line.c_str());
            printed = true;
            if (line == "OK" || line == "ERROR" || line.startsWith("+CME ERROR"))
                return;
        }
        delay(10);
    }

    if (!printed)
        Serial.printf("MODEM: %s rx timeout\n", label);
}

void modem_print_sim_network_status()
{
    String line;
    if (read_at_prefixed_line("+CPIN?", "+CPIN:", line, 2000))
        Serial.printf("MODEM: SIM status +CPIN:%s\n", line.c_str());
    else {
        Serial.println("MODEM: SIM status +CPIN? unavailable");
        print_at_raw_response("SIM status raw", "+CPIN?", 3000);
        print_at_raw_response("SIM insert raw", "+CSMINS?", 3000);
    }

    if (read_at_prefixed_line("+CEREG?", "+CEREG:", line, 2000))
        Serial.printf("MODEM: registration +CEREG:%s\n", line.c_str());
    else
        Serial.println("MODEM: registration +CEREG? unavailable");

    if (read_at_prefixed_line("+CREG?", "+CREG:", line, 2000))
        Serial.printf("MODEM: registration +CREG:%s\n", line.c_str());
    else
        Serial.println("MODEM: registration +CREG? unavailable");
}

static bool modem_ping_host(const char *host)
{
    if (!host || !host[0])
        return false;

    Serial.printf("MODEM: LTE-M lookup ping host=%s\n", host);
    modem.sendAT("+SNPING4=\"", host, "\",1,16,1000");

    uint32_t start = millis();
    bool saw_success = false;
    while (millis() - start < 3000)
    {
        String line = modem.stream.readStringUntil('\n');
        line.trim();
        if (line.length() == 0)
            continue;

        Serial.printf("MODEM: ping rx %s\n", line.c_str());
        if (line.indexOf("+SNPING4:") >= 0) {
            int comma2 = line.lastIndexOf(',');
            int latency_ms = comma2 >= 0 ? line.substring(comma2 + 1).toInt() : 0;
            if (latency_ms > 0 && latency_ms < 60000) {
                saw_success = true;
            }
            continue;
        }
        if (line == "OK")
            return saw_success;
        if (line == "ERROR")
            return false;
    }

    return false;
}

static String extract_json_string_field(const String &body, const char *field)
{
    String needle = String("\"") + field + "\":\"";
    int start = body.indexOf(needle);
    if (start < 0)
        return "";

    start += needle.length();
    int end = body.indexOf('"', start);
    if (end < 0)
        return "";

    return body.substring(start, end);
}

static String extract_json_number_field(const String &body, const char *field)
{
    String needle = String("\"") + field + "\":";
    int start = body.indexOf(needle);
    if (start < 0)
        return "";

    start += needle.length();
    while (start < body.length() && body[start] == ' ')
        start++;

    int end = start;
    while (end < body.length() && body[end] >= '0' && body[end] <= '9')
        end++;

    return end > start ? body.substring(start, end) : "";
}

bool modem_init_early()
{
    static bool done = false;
    if (done) return true;
    done = true;

    if (!pmu_enable_modem_rails())
        return false;

    Serial1.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RXD, MODEM_TXD);
    g_serial_ready = true;

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

bool modem_validate_ltem(const char *apn,
                         const char *lookup_primary,
                         const char *lookup_secondary,
                         uint32_t network_timeout_ms)
{
    if (!apn || !apn[0])
        apn = "internet.m2m";

    Serial.printf("MODEM: LTE-M validation begin apn=%s lookup_primary=%s lookup_secondary=%s\n",
                  apn,
                  lookup_primary && lookup_primary[0] ? lookup_primary : "-",
                  lookup_secondary && lookup_secondary[0] ? lookup_secondary : "-");

    if (!wait_for_network_registration(network_timeout_ms)) {
        Serial.println("MODEM: LTE-M registration timeout");
        return false;
    }

    if (!modem.gprsConnect(apn)) {
        Serial.println("MODEM: LTE-M data attach FAILED");
        return false;
    }

    IPAddress local_ip = modem.localIP();
    Serial.printf("MODEM: LTE-M data attached local_ip=%s\n", local_ip.toString().c_str());

    if (local_ip == IPAddress(0, 0, 0, 0)) {
        Serial.println("MODEM: LTE-M validation FAILED; data attached but local IP is 0.0.0.0");
        return false;
    }

    if (modem_ping_host(lookup_primary)) {
        Serial.printf("MODEM: LTE-M lookup probe PASS host=%s\n", lookup_primary);
        Serial.println("MODEM: LTE-M validation PASS bearer=attached ip=assigned lookup=reachable");
        return true;
    }

    if (modem_ping_host(lookup_secondary)) {
        Serial.printf("MODEM: LTE-M lookup probe PASS host=%s\n", lookup_secondary);
        Serial.println("MODEM: LTE-M validation PASS bearer=attached ip=assigned lookup=reachable");
        return true;
    }

    Serial.println("MODEM: LTE-M lookup probe WARN; bearer attached and IP assigned, but lookup ping was blocked or timed out");
    Serial.println("MODEM: LTE-M validation PASS bearer=attached ip=assigned lookup=unreachable");
    return true;
}

bool modem_test_world_clock(uint32_t timeout_ms)
{
    static const char *host = "worldtimeapi.org";
    static const char *path = "/api/timezone/Etc/UTC";

    Serial.printf("MODEM: world clock HTTP test begin host=%s path=%s\n", host, path);

    TinyGsmClient client(modem, 1);
    if (!client.connect(host, 80, timeout_ms / 1000)) {
        client.stop();
        Serial.println("MODEM: world clock HTTP connect FAILED");
        return false;
    }

    client.print(String("GET ") + path + " HTTP/1.1\r\n" +
                 "Host: " + host + "\r\n" +
                 "User-Agent: VST-BASE/0.2\r\n" +
                 "Connection: close\r\n\r\n");

    uint32_t start = millis();
    String response;
    response.reserve(1024);
    while (millis() - start < timeout_ms) {
        while (client.available()) {
            char c = (char)client.read();
            if (response.length() < 1600)
                response += c;
        }
        if (!client.connected() && !client.available())
            break;
        delay(10);
    }
    client.stop();

    int line_end = response.indexOf('\n');
    String status_line = line_end >= 0 ? response.substring(0, line_end) : response;
    status_line.trim();
    Serial.printf("MODEM: world clock HTTP status [%s]\n", status_line.c_str());

    bool ok = status_line.indexOf(" 200 ") >= 0;
    int body_start = response.indexOf("\r\n\r\n");
    String body = body_start >= 0 ? response.substring(body_start + 4) : "";
    String utc = extract_json_string_field(body, "utc_datetime");
    String unixtime = extract_json_number_field(body, "unixtime");

    if (utc.length() || unixtime.length()) {
        Serial.printf("MODEM: world clock utc=%s unixtime=%s\n",
                      utc.length() ? utc.c_str() : "-",
                      unixtime.length() ? unixtime.c_str() : "-");
    }

    if (!ok)
        Serial.println("MODEM: world clock HTTP test FAILED");
    else
        Serial.println("MODEM: world clock HTTP test PASS");

    return ok;
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

bool modem_check_network_registered(uint32_t timeout_ms)
{
    return wait_for_network_registration(timeout_ms);
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
    bool saw_first_valid_utc = false;
    char first_valid_utc[24] = {0};

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
                if (is_plausible_gnss_utc(info.utc)) {
                    info.utc_valid = true;
                    if (!saw_first_valid_utc) {
                        snprintf(first_valid_utc, sizeof(first_valid_utc), "%s", info.utc);
                        saw_first_valid_utc = true;
                    } else if (strcmp(first_valid_utc, info.utc) != 0) {
                        info.utc_advancing = true;
                    }
                }
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

void modem_prepare_for_sleep()
{
    Serial.println("MODEM: preparing for deep sleep");

    if (g_serial_ready) {
        modem.sendAT("+CGNSPWR=0");
        modem.waitResponse(2000);
        modem.sendAT("+CPOWD=1");
        modem.waitResponse(3000);
    }

    if (g_pmu_ready) {
        PMU.disableBLDO2();
        PMU.disableDC3();
        Serial.println("MODEM: GNSS off; modem rails disabled");
    } else {
        Serial.println("MODEM: PMU was not ready; rails not changed");
    }
    Serial.flush();
}
