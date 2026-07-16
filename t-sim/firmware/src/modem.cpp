// modem.cpp

#include "modem.h"

#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <Wire.h>
#include <FS.h>
#include <SD_MMC.h>

#if __has_include("config_secrets.h")
#include "config_secrets.h"
#endif

#ifndef AZURE_BLOB_HOST
#define AZURE_BLOB_HOST ""
#endif
#ifndef AZURE_BLOB_CONTAINER
#define AZURE_BLOB_CONTAINER ""
#endif
#ifndef AZURE_BLOB_SAS
#define AZURE_BLOB_SAS ""
#endif

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
static bool g_modem_initialized = false;
static constexpr const char *MODEM_HEALTH_LOG_PATH = "/health.log";
static char g_modem_device_name[32] = "vst-base";

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

    Serial.println("MODEM: modem rails reset begin");
    PMU.disableBLDO2();
    PMU.disableDC3();
    delay(2000);
    Serial.println("MODEM: modem rails reset power-up");

    PMU.setDC3Voltage(3000);
    PMU.enableDC3();
    Serial.println("MODEM: DCDC3 enabled at 3000mV");

    PMU.setBLDO2Voltage(3300);
    PMU.enableBLDO2();
    Serial.println("MODEM: BLDO2 enabled at 3300mV");

    PMU.disableTSPinMeasure();
    delay(2500);

    return true;
}

static bool wait_for_at_ready(uint32_t timeout_ms)
{
    uint32_t start = millis();
    uint32_t last_progress_ms = start;
    bool pwrkey_pulsed = false;
    static constexpr uint32_t FIRST_PWRKEY_DELAY_MS = 20000;

    while (millis() - start < timeout_ms)
    {
        if (modem.testAT(1000))
            return true;

        uint32_t now = millis();
        if (now - last_progress_ms >= 5000) {
            Serial.printf("\nMODEM: AT probe waiting elapsed_ms=%lu timeout_ms=%lu\n",
                          (unsigned long)(now - start),
                          (unsigned long)timeout_ms);
            last_progress_ms = now;
        }

        if (!pwrkey_pulsed && now - start >= FIRST_PWRKEY_DELAY_MS) {
            Serial.println("MODEM: AT not ready after boot window, pulsing PWRKEY");
            pwrkey_pulse();
            pwrkey_pulsed = true;
            last_progress_ms = millis();
        }

        delay(200);
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

static void append_apn_probe_log(const char *event,
                                 uint8_t attempt,
                                 const char *supplier,
                                 const char *apn,
                                 const char *result,
                                 const char *detail)
{
    String s;
    s.reserve(320);
    s += "{";
    append_json_string(s, "type", "modem_apn_probe");
    s += ",";
    append_json_string(s, "device_name", g_modem_device_name);
    s += ",";
    append_json_string(s, "event", event ? event : "");
    s += ",\"uptime_ms\":";
    s += (unsigned long)millis();
    s += ",\"attempt\":";
    s += (unsigned)attempt;
    s += ",";
    append_json_string(s, "supplier", supplier ? supplier : "");
    s += ",";
    append_json_string(s, "apn", apn ? apn : "");
    s += ",";
    append_json_string(s, "result", result ? result : "");
    s += ",";
    append_json_string(s, "detail", detail ? detail : "");
    s += "}\n";

    sdcard_append_log(MODEM_HEALTH_LOG_PATH, s);
}

void modem_set_device_name(const char *device_name)
{
    if (!device_name || !device_name[0])
        strlcpy(g_modem_device_name, "vst-base", sizeof(g_modem_device_name));
    else
        strlcpy(g_modem_device_name, device_name, sizeof(g_modem_device_name));
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

static bool activate_app_pdp_context(const char *apn, char *out, size_t out_len, uint32_t timeout_ms);

bool modem_at_responsive(uint32_t timeout_ms)
{
    if (!g_serial_ready)
        return false;
    return modem.testAT(timeout_ms);
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

static bool send_at_expect_ok(const char *cmd, uint32_t timeout_ms)
{
    Serial.printf("MODEM: command AT%s\n", cmd);
    modem.sendAT(cmd);
    int response = modem.waitResponse(timeout_ms);
    Serial.printf("MODEM: command AT%s result=%s\n", cmd, response == 1 ? "OK" : "FAIL");
    delay(100);
    return response == 1;
}

static void log_operator_after_registration(const char *context)
{
    String cops;
    if (!read_at_prefixed_line("+COPS?", "+COPS:", cops, 5000)) {
        Serial.printf("MODEM: operator after registration unavailable context=%s\n",
                      context && context[0] ? context : "-");
        return;
    }

    cops.trim();
    Serial.printf("MODEM: operator after registration context=%s +COPS:%s\n",
                  context && context[0] ? context : "-",
                  cops.c_str());
}

static int timeout_ms_to_connect_seconds(uint32_t timeout_ms)
{
    uint32_t seconds = (timeout_ms + 999UL) / 1000UL;
    if (seconds == 0)
        seconds = 1;
    if (seconds > 30)
        seconds = 30;
    return (int)seconds;
}

static bool is_sim_ready_for_network()
{
    String sim;
    if (!read_at_prefixed_line("+CPIN?", "+CPIN:", sim, 2000)) {
        Serial.println("MODEM: SIM status before operator selection unavailable; skipping operator selection change");
        return false;
    }

    sim.trim();
    Serial.printf("MODEM: SIM status before operator selection +CPIN:%s\n", sim.c_str());
    if (sim == "READY")
        return true;

    Serial.println("MODEM: SIM not ready; skipping operator selection change");
    return false;
}

static bool set_pdp_context_ip(const char *apn)
{
    if (!apn || !apn[0])
        return false;

    Serial.printf("MODEM: PDP context define cid=1 type=IP apn=%s\n", apn);
    modem.sendAT("+CGDCONT=1,\"IP\",\"", apn, "\"");
    int response = modem.waitResponse(5000);
    Serial.printf("MODEM: PDP context define result=%s\n", response == 1 ? "OK" : "FAIL");
    delay(100);
    return response == 1;
}

static void deactivate_pdp_context()
{
    Serial.println("MODEM: PDP context deactivate cid=1");
    modem.sendAT("+CGACT=0,1");
    int response = modem.waitResponse(10000);
    Serial.printf("MODEM: PDP context deactivate result=%s\n", response == 1 ? "OK" : "FAIL");
    delay(100);
}

static void deactivate_app_pdp_context()
{
    Serial.println("MODEM: APP PDP deactivate id=0");
    modem.sendAT("+CNACT=0,0");
    int response = modem.waitResponse(2000);
    Serial.printf("MODEM: APP PDP deactivate result=%s\n", response == 1 ? "OK" : "FAIL");
    delay(100);
}

static void reset_bearer_for_apn_probe(uint8_t attempt, const char *supplier, const char *apn)
{
    Serial.printf("MODEM: APN probe reset bearer attempt=%u supplier=%s apn=%s\n",
                  (unsigned)attempt,
                  supplier && supplier[0] ? supplier : "configured",
                  apn && apn[0] ? apn : "-");
    deactivate_app_pdp_context();
    deactivate_pdp_context();
}

static bool is_digits_only(const String &value)
{
    if (!value.length())
        return false;

    for (int i = 0; i < value.length(); i++) {
        if (!isdigit((unsigned char)value[i]))
            return false;
    }
    return true;
}

static bool read_sim_identity_value(const char *cmd, const char *label, String &value, uint32_t timeout_ms)
{
    value = "";
    modem.sendAT(cmd);
    uint32_t start = millis();

    while (millis() - start < timeout_ms) {
        while (modem.stream.available()) {
            String line = modem.stream.readStringUntil('\n');
            line.trim();
            if (!line.length())
                continue;

            if (line == "OK")
                return value.length() > 0;
            if (line == "ERROR" || line.startsWith("+CME ERROR")) {
                Serial.printf("MODEM: SIM identity %s unavailable line=[%s]\n", label, line.c_str());
                modem.waitResponse(100);
                return false;
            }

            int colon = line.indexOf(':');
            if (colon >= 0) {
                line = line.substring(colon + 1);
                line.trim();
            }
            line.replace("\"", "");
            line.trim();

            if (is_digits_only(line)) {
                value = line;
                continue;
            }
        }
        delay(10);
    }

    if (value.length()) {
        modem.waitResponse(200);
        return true;
    }

    Serial.printf("MODEM: SIM identity %s unavailable reason=timeout\n", label);
    return false;
}

static bool wait_for_sim_ready_for_identity(uint32_t timeout_ms)
{
    uint32_t start = millis();
    String line;
    while (millis() - start < timeout_ms) {
        if (read_at_prefixed_line("+CPIN?", "+CPIN:", line, 1000)) {
            line.trim();
            line.replace(" ", "");
            Serial.printf("MODEM: SIM identity CPIN=%s\n", line.c_str());
            if (line.indexOf("READY") >= 0)
                return true;
        }
        delay(250);
    }

    Serial.println("MODEM: SIM identity CPIN not ready before identity query");
    return false;
}

static bool read_sim_identity_value_retry(const char *cmd,
                                          const char *label,
                                          String &value,
                                          uint8_t attempts,
                                          uint32_t timeout_ms)
{
    for (uint8_t attempt = 1; attempt <= attempts; attempt++) {
        if (read_sim_identity_value(cmd, label, value, timeout_ms))
            return true;

        if (attempt < attempts) {
            Serial.printf("MODEM: SIM identity %s retry attempt=%u/%u\n",
                          label,
                          (unsigned)(attempt + 1),
                          (unsigned)attempts);
            delay(500);
        }
    }
    return false;
}

template <size_t N>
static bool starts_with_config_prefixes(const String &value, const char (&prefixes)[ModemConfig::MAX_SIM_PREFIXES][N], uint8_t prefix_count)
{
    for (uint8_t i = 0; i < prefix_count && i < ModemConfig::MAX_SIM_PREFIXES; i++) {
        if (prefixes[i][0] && value.startsWith(prefixes[i]))
            return true;
    }
    return false;
}

static int find_apn_candidate_index(const ModemConfig &config, const char *supplier_fragment, const char *apn)
{
    for (uint8_t i = 0; i < config.apn_candidate_count; i++) {
        const ModemConfig::ApnCandidate &candidate = config.apn_candidates[i];
        if (apn && apn[0] && strcasecmp(candidate.apn, apn) == 0)
            return i;

        if (supplier_fragment && supplier_fragment[0]) {
            String supplier = candidate.supplier;
            String fragment = supplier_fragment;
            supplier.toLowerCase();
            fragment.toLowerCase();
            if (supplier.indexOf(fragment) >= 0)
                return i;
        }
    }
    return -1;
}

static void move_apn_candidate_to_front(ModemConfig &config, uint8_t index)
{
    if (index == 0 || index >= config.apn_candidate_count)
        return;

    ModemConfig::ApnCandidate selected = config.apn_candidates[index];
    for (int i = index; i > 0; i--)
        config.apn_candidates[i] = config.apn_candidates[i - 1];
    config.apn_candidates[0] = selected;
}

static bool prefer_apn_candidate_by_sim_identity(ModemConfig &config)
{
    String imsi;
    String iccid;
    wait_for_sim_ready_for_identity(5000);
    bool have_imsi = read_sim_identity_value_retry("+CIMI", "IMSI", imsi, 3, 3000);
    bool have_iccid = read_sim_identity_value_retry("+CCID", "ICCID", iccid, 3, 3000);

    Serial.printf("MODEM: SIM identity imsi=%s iccid=%s\n",
                  have_imsi ? imsi.c_str() : "-",
                  have_iccid ? iccid.c_str() : "-");

    if (!have_imsi && !have_iccid) {
        Serial.println("MODEM: SIM identity APN preference skipped reason=identity_unavailable");
        return false;
    }

    const ModemConfig::SimProfile *matched_profile = nullptr;
    const char *matched_by = nullptr;
    for (uint8_t i = 0; i < config.sim_profile_count; i++) {
        const ModemConfig::SimProfile &profile = config.sim_profiles[i];
        if (have_imsi && starts_with_config_prefixes(imsi, profile.imsi_prefixes, profile.imsi_prefix_count)) {
            matched_profile = &profile;
            matched_by = "imsi";
            break;
        }
        if (have_iccid && starts_with_config_prefixes(iccid, profile.iccid_prefixes, profile.iccid_prefix_count)) {
            matched_profile = &profile;
            matched_by = "iccid";
            break;
        }
    }

    if (!matched_profile) {
        Serial.println("MODEM: SIM identity APN preference no known profile; using configured candidate order");
        return false;
    }

    int index = find_apn_candidate_index(config, matched_profile->supplier, matched_profile->apn);
    if (index < 0) {
        Serial.printf("MODEM: SIM identity profile=%s preferred_apn=%s match=%s not present in candidates\n",
                      matched_profile->supplier,
                      matched_profile->apn,
                      matched_by);
        return false;
    }

    move_apn_candidate_to_front(config, (uint8_t)index);
    strlcpy(config.apn, config.apn_candidates[0].apn, sizeof(config.apn));
    config.direct_sms = matched_profile->direct_sms;
    Serial.printf("MODEM: SIM identity selected APN profile=%s supplier=%s apn=%s direct_sms=%s match=%s candidate_index=%d\n",
                  matched_profile->supplier,
                  config.apn_candidates[0].supplier,
                  config.apn,
                  config.direct_sms ? "YES" : "NO",
                  matched_by,
                  index + 1);
    return true;
}

static bool ensure_automatic_operator_selection(bool allow_change)
{
    String cops;
    if (!read_at_prefixed_line("+COPS?", "+COPS:", cops, 5000)) {
        Serial.println("MODEM: operator mode read unavailable; leaving operator selection unchanged");
        return false;
    }

    Serial.printf("MODEM: operator +COPS:%s\n", cops.c_str());
    cops.trim();
    if (cops.startsWith("0")) {
        Serial.println("MODEM: operator selection already automatic");
        return true;
    }

    if (!allow_change) {
        Serial.println("MODEM: operator selection is not automatic; skipping AT+COPS=0 because modem.operator_auto_select=false");
        return false;
    }

    if (!is_sim_ready_for_network())
        return false;

    Serial.println("MODEM: operator selection is not automatic; requesting AT+COPS=0");
    return send_at_expect_ok("+COPS=0", 15000);
}

static bool activate_pdp_context()
{
    Serial.println("MODEM: PDP context activate cid=1");
    modem.sendAT("+CGACT=1,1");
    int response = modem.waitResponse(30000);
    Serial.printf("MODEM: PDP context activate result=%s\n", response == 1 ? "OK" : "FAIL");
    delay(100);
    return response == 1;
}

static bool read_pdp_address(char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return false;

    out[0] = '\0';
    Serial.println("MODEM: PDP address query cid=1");
    modem.sendAT("+CGPADDR=1");

    uint32_t start = millis();
    while (millis() - start < 5000) {
        while (modem.stream.available()) {
            String line = modem.stream.readStringUntil('\n');
            line.trim();
            if (!line.length())
                continue;

            Serial.printf("MODEM: PDP address rx [%s]\n", line.c_str());
            if (line.startsWith("+CGPADDR:")) {
                int comma = line.indexOf(',');
                if (comma >= 0) {
                    String ip = line.substring(comma + 1);
                    ip.trim();
                    ip.replace("\"", "");
                    strlcpy(out, ip.c_str(), out_len);
                }
                modem.waitResponse(500);
                bool ok = out[0] &&
                          strcmp(out, "0.0.0.0") != 0 &&
                          strcmp(out, "0") != 0;
                Serial.printf("MODEM: PDP address result=%s ip=%s\n", ok ? "OK" : "FAIL", out[0] ? out : "-");
                return ok;
            }
            if (line == "ERROR" || line.startsWith("+CME ERROR")) {
                Serial.println("MODEM: PDP address query FAILED");
                return false;
            }
        }
        delay(10);
    }

    Serial.println("MODEM: PDP address query timeout");
    return false;
}

static bool read_app_pdp_address(char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return false;

    out[0] = '\0';
    Serial.println("MODEM: APP PDP address query id=0");
    modem.sendAT("+CNACT?");

    uint32_t start = millis();
    while (millis() - start < 5000) {
        while (modem.stream.available()) {
            String line = modem.stream.readStringUntil('\n');
            line.trim();
            if (!line.length())
                continue;

            Serial.printf("MODEM: APP PDP address rx [%s]\n", line.c_str());
            if (line.startsWith("+APP PDP:") && line.indexOf("DEACTIVE") >= 0) {
                Serial.println("MODEM: APP PDP address result=FAIL reason=deactive_urc");
                return false;
            }
            if (line.startsWith("+CNACT:")) {
                String fields = line.substring(strlen("+CNACT:"));
                fields.trim();
                String id = csv_field(fields, 0);
                String active = csv_field(fields, 1);
                String ip = csv_field(fields, 2);
                id.trim();
                active.trim();
                ip.trim();
                ip.replace("\"", "");
                if (id == "0" && active == "1") {
                    strlcpy(out, ip.c_str(), out_len);
                }
            }

            if (line == "OK") {
                bool ok = out[0] &&
                          strcmp(out, "0.0.0.0") != 0 &&
                          strcmp(out, "0") != 0;
                Serial.printf("MODEM: APP PDP address result=%s ip=%s\n", ok ? "OK" : "FAIL", out[0] ? out : "-");
                return ok;
            }

            if (line == "ERROR" || line.startsWith("+CME ERROR")) {
                Serial.println("MODEM: APP PDP address query FAILED");
                return false;
            }
        }
        delay(10);
    }

    Serial.println("MODEM: APP PDP address query timeout");
    return false;
}

static bool wait_for_app_pdp_address(char *out, size_t out_len, uint32_t timeout_ms)
{
    uint32_t start = millis();
    do {
        if (read_app_pdp_address(out, out_len))
            return true;
        delay(1000);
    } while (millis() - start < timeout_ms);

    return false;
}

static bool activate_app_pdp_context(const char *apn, char *out, size_t out_len, uint32_t timeout_ms)
{
    if (!apn || !apn[0] || !out || out_len == 0)
        return false;

    out[0] = '\0';
    deactivate_app_pdp_context();

    if (!send_at_expect_ok("+CGATT=1", timeout_ms < 60000 ? timeout_ms : 60000)) {
        Serial.println("MODEM: APP PDP packet attach FAILED");
        return false;
    }
    modem.sendAT("+CGATT?");
    modem.waitResponse(3000);
    modem.sendAT("+CGNAPN");
    modem.waitResponse(3000);

    Serial.printf("MODEM: APP PDP configure id=0 pdp=IP apn=%s\n", apn);
    modem.sendAT("+CNCFG=0,1,\"", apn, "\"");
    int config_response = modem.waitResponse(5000);
    Serial.printf("MODEM: APP PDP configure result=%s\n", config_response == 1 ? "OK" : "FAIL");
    if (config_response != 1)
        return false;

    uint32_t app_timeout = timeout_ms < 15000 ? timeout_ms : 15000;

    Serial.println("MODEM: APP PDP activate id=0 action=1");
    modem.sendAT("+CNACT=0,1");
    int activate_response = modem.waitResponse(app_timeout);
    Serial.printf("MODEM: APP PDP activate action=1 result=%s\n", activate_response == 1 ? "OK" : "FAIL");
    if (wait_for_app_pdp_address(out, out_len, app_timeout))
        return true;

    Serial.println("MODEM: APP PDP auto-active retry id=0 action=2");
    modem.sendAT("+CNACT=0,2");
    int auto_response = modem.waitResponse(app_timeout);
    Serial.printf("MODEM: APP PDP activate action=2 result=%s\n", auto_response == 1 ? "OK" : "FAIL");
    if (wait_for_app_pdp_address(out, out_len, app_timeout))
        return true;

    Serial.println("MODEM: APP PDP activate FAILED reason=no_app_ip");
    return false;
}

static uint32_t remaining_ms(uint32_t start_ms, uint32_t timeout_ms)
{
    uint32_t elapsed = millis() - start_ms;
    return elapsed >= timeout_ms ? 0 : timeout_ms - elapsed;
}

static bool activate_app_pdp_context_bounded(const char *apn,
                                             uint32_t timeout_ms,
                                             char *out_ip = nullptr,
                                             size_t out_ip_len = 0)
{
    if (!apn || !apn[0])
        return false;

    char ip[32] = {0};
    uint32_t start = millis();
    timeout_ms = timeout_ms < 5000 ? 5000 : timeout_ms;

    deactivate_app_pdp_context();
    uint32_t left = remaining_ms(start, timeout_ms);
    if (left == 0)
        return false;

    if (!send_at_expect_ok("+CGATT=1", left)) {
        Serial.println("MODEM: bounded APP PDP packet attach FAILED");
        return false;
    }

    left = remaining_ms(start, timeout_ms);
    if (left == 0)
        return false;

    Serial.printf("MODEM: bounded APP PDP configure id=0 pdp=IP apn=%s\n", apn);
    modem.sendAT("+CNCFG=0,1,\"", apn, "\"");
    int config_response = modem.waitResponse(left < 5000 ? left : 5000);
    Serial.printf("MODEM: bounded APP PDP configure result=%s\n", config_response == 1 ? "OK" : "FAIL");
    if (config_response != 1)
        return false;

    left = remaining_ms(start, timeout_ms);
    if (left == 0)
        return false;

    Serial.printf("MODEM: bounded APP PDP activate id=0 action=1 timeout_ms=%lu\n", (unsigned long)left);
    modem.sendAT("+CNACT=0,1");
    int activate_response = modem.waitResponse(left, GF(AT_NL "+APP PDP: 0,ACTIVE"), GF(AT_NL "+APP PDP: 0,DEACTIVE"));
    modem.waitResponse(500);
    Serial.printf("MODEM: bounded APP PDP activate action=1 result=%s\n", activate_response == 1 ? "OK" : "FAIL");
    if (activate_response != 1)
        return false;

    left = remaining_ms(start, timeout_ms);
    if (left == 0)
        return false;

    if (!wait_for_app_pdp_address(ip, sizeof(ip), left))
        return false;

    if (out_ip && out_ip_len > 0)
        strlcpy(out_ip, ip, out_ip_len);
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
            if (line == "OK" || line == "ERROR" || line.startsWith("+CME ERROR") || line.startsWith("+CMS ERROR"))
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
    print_at_raw_response("module identity raw", "I", 3000);
    print_at_raw_response("verbose errors raw", "+CMEE?", 3000);
    print_at_raw_response("functionality raw", "+CFUN?", 3000);

    if (read_at_prefixed_line("+CPIN?", "+CPIN:", line, 2000)) {
        Serial.printf("MODEM: SIM status +CPIN:%s\n", line.c_str());
        print_at_raw_response("SIM ICCID raw", "+CCID", 3000);
        print_at_raw_response("SIM IMSI raw", "+CIMI", 3000);
    } else {
        Serial.println("MODEM: SIM status +CPIN? unavailable");
        print_at_raw_response("SIM status raw", "+CPIN?", 3000);
        print_at_raw_response("SIM insert raw", "+CSMINS?", 3000);
        print_at_raw_response("SIM ICCID raw", "+CCID", 3000);
        print_at_raw_response("SIM IMSI raw", "+CIMI", 3000);
    }

    print_at_raw_response("signal raw", "+CSQ", 3000);
    print_at_raw_response("operator raw", "+COPS?", 3000);
    print_at_raw_response("EPS registration raw", "+CEREG?", 3000);
    print_at_raw_response("GPRS registration raw", "+CGREG?", 3000);

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

bool modem_init_early(bool operator_auto_select)
{
    if (g_modem_initialized && g_serial_ready && modem.testAT(1000))
        return true;

    if (g_serial_ready)
        Serial1.end();
    g_serial_ready = false;
    g_modem_initialized = false;

    if (!pmu_enable_modem_rails())
        return false;

    Serial1.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RXD, MODEM_TXD);
    g_serial_ready = true;

    Serial.println("MODEM: Serial1 begin baud=115200 RX=4 TX=5");
    Serial.println("MODEM: probing AT");
    if (!wait_for_at_ready(30000))
    {
        Serial.println("\nMODEM: AT probe FAILED");
        return false;
    }
    Serial.println("\nMODEM: AT ready");

    modem.sendAT("+CMEE=2");
    modem.waitResponse(2000);
    Serial.println("MODEM: CMEE verbose errors enabled");

    modem.sendAT("+CFUN=1");
    modem.waitResponse(5000);
    Serial.println("MODEM: CFUN full functionality requested");

    modem.sendAT("+CLTS=1");
    modem.waitResponse(2000);
    Serial.println("MODEM: CLTS command sent");

    modem.sendAT("+CTZR=1");
    modem.waitResponse(2000);
    Serial.println("MODEM: CTZR command sent");

    modem.sendAT("+CEREG=2");
    modem.waitResponse(2000);
    modem.sendAT("+CREG=2");
    modem.waitResponse(2000);
    modem.sendAT("+CGREG=2");
    modem.waitResponse(2000);
    Serial.println("MODEM: registration detail reporting enabled");

    ensure_automatic_operator_selection(operator_auto_select);

    g_modem_initialized = true;
    return true;
}

static bool modem_validate_ltem_attempt(const char *supplier,
                                        const char *apn,
                                        const char *lookup_primary,
                                        const char *lookup_secondary,
                                        uint32_t network_timeout_ms,
                                        uint8_t attempt,
                                        bool require_registration,
                                        bool validate_http_egress,
                                        bool force_clean_bearer)
{
    (void)lookup_primary;
    (void)lookup_secondary;

    if (!apn || !apn[0])
        apn = "internet.m2m";

    Serial.printf("MODEM: LTE-M APN probe begin attempt=%u supplier=%s apn=%s validation=%s\n",
                  (unsigned)attempt,
                  supplier && supplier[0] ? supplier : "configured",
                  apn,
                  validate_http_egress ? "http_egress" : "bearer_ip");
    append_apn_probe_log("begin", attempt, supplier, apn, "running", "lte_m_validation_begin");

    if (force_clean_bearer)
        reset_bearer_for_apn_probe(attempt, supplier, apn);

    bool data_active = modem.isGprsConnected();
    Serial.printf("MODEM: TinyGSM data active before APN configure attempt=%u active=%s\n",
                  (unsigned)attempt,
                  data_active ? "YES" : "NO");

    if (!data_active) {
        if (!set_pdp_context_ip(apn)) {
            Serial.printf("MODEM: LTE-M APN probe FAIL attempt=%u supplier=%s apn=%s reason=pdp_context_define_failed\n",
                          (unsigned)attempt,
                          supplier && supplier[0] ? supplier : "configured",
                          apn);
            append_apn_probe_log("end", attempt, supplier, apn, "fail", "pdp_context_define_failed");
            return false;
        }
    } else {
        Serial.println("MODEM: APN configure skipped because TinyGSM data is already active");
    }

    if (require_registration && !wait_for_network_registration(network_timeout_ms)) {
        Serial.printf("MODEM: LTE-M APN probe FAIL attempt=%u supplier=%s apn=%s reason=registration_timeout\n",
                      (unsigned)attempt,
                      supplier && supplier[0] ? supplier : "configured",
                      apn);
        append_apn_probe_log("end", attempt, supplier, apn, "fail", "registration_timeout");
        return false;
    }

    Serial.printf("MODEM: data active before connect attempt=%u active=%s\n",
                  (unsigned)attempt,
                  data_active ? "YES" : "NO");

    if (!data_active) {
        char app_ip[32] = {0};
        Serial.printf("MODEM: bounded APP PDP connect begin attempt=%u supplier=%s apn=%s timeout_ms=%lu\n",
                      (unsigned)attempt,
                      supplier && supplier[0] ? supplier : "configured",
                      apn,
                      (unsigned long)network_timeout_ms);
        if (!activate_app_pdp_context_bounded(apn, network_timeout_ms, app_ip, sizeof(app_ip))) {
            Serial.printf("MODEM: LTE-M APN probe FAIL attempt=%u supplier=%s apn=%s reason=app_pdp_activate_failed\n",
                          (unsigned)attempt,
                          supplier && supplier[0] ? supplier : "configured",
                          apn);
            print_at_raw_response("APP PDP after bounded connect fail", "+CNACT?", 3000);
            append_apn_probe_log("end", attempt, supplier, apn, "fail", "app_pdp_activate_failed");
            return false;
        }

        Serial.printf("MODEM: LTE-M APN probe attached attempt=%u supplier=%s apn=%s local_ip=%s source=APP_PDP\n",
                      (unsigned)attempt,
                      supplier && supplier[0] ? supplier : "configured",
                      apn,
                      app_ip[0] ? app_ip : "-");

        if (!app_ip[0] || strcmp(app_ip, "0.0.0.0") == 0 || strcmp(app_ip, "0") == 0) {
            Serial.printf("MODEM: LTE-M APN probe FAIL attempt=%u supplier=%s apn=%s reason=zero_local_ip\n",
                      (unsigned)attempt,
                      supplier && supplier[0] ? supplier : "configured",
                      apn);
            print_at_raw_response("APP PDP after zero local IP", "+CNACT?", 3000);
            append_apn_probe_log("end", attempt, supplier, apn, "fail", "zero_local_ip");
            return false;
        }
    } else {
        IPAddress local_ip = modem.localIP();
        char app_ip[32] = {0};
        strlcpy(app_ip, local_ip.toString().c_str(), sizeof(app_ip));
        Serial.printf("MODEM: LTE-M APN probe already attached attempt=%u supplier=%s apn=%s local_ip=%s source=TinyGSM\n",
                      (unsigned)attempt,
                      supplier && supplier[0] ? supplier : "configured",
                      apn,
                      app_ip[0] ? app_ip : "-");
    }

    if (!validate_http_egress) {
        Serial.printf("MODEM: LTE-M APN probe PASS attempt=%u supplier=%s apn=%s validation=bearer_ip\n",
                      (unsigned)attempt,
                      supplier && supplier[0] ? supplier : "configured",
                      apn);
        append_apn_probe_log("end", attempt, supplier, apn, "pass", "bearer_ip");
        return true;
    }

    Serial.printf("MODEM: LTE-M APN probe HTTP validation begin attempt=%u supplier=%s apn=%s\n",
                  (unsigned)attempt,
                  supplier && supplier[0] ? supplier : "configured",
                  apn);
    if (modem_test_http_egress(20000)) {
        Serial.printf("MODEM: LTE-M APN probe PASS attempt=%u supplier=%s apn=%s validation=http_egress\n",
                      (unsigned)attempt,
                      supplier && supplier[0] ? supplier : "configured",
                      apn);
        append_apn_probe_log("end", attempt, supplier, apn, "pass", "bearer_ip_http_egress");
        return true;
    }

    Serial.printf("MODEM: LTE-M APN probe FAIL attempt=%u supplier=%s apn=%s reason=http_egress_failed\n",
                  (unsigned)attempt,
                  supplier && supplier[0] ? supplier : "configured",
                  apn);
    append_apn_probe_log("end", attempt, supplier, apn, "fail", "bearer_ip_ok_http_egress_failed");
    return false;
}

bool modem_validate_ltem(const char *apn,
                         const char *lookup_primary,
                         const char *lookup_secondary,
                         uint32_t network_timeout_ms)
{
    return modem_validate_ltem_attempt("configured",
                                       apn,
                                       lookup_primary,
                                       lookup_secondary,
                                       network_timeout_ms,
                                       1,
                                       true,
                                       false,
                                       false);
}

bool modem_validate_ltem_apn_candidates(ModemConfig &config, uint32_t network_timeout_ms)
{
    if (!config.apn_autodetect) {
        Serial.printf("MODEM: APN autodetect disabled; using configured apn=%s\n", config.apn);
        return modem_validate_ltem_attempt("configured",
                                           config.apn,
                                           config.lookup_primary,
                                           config.lookup_secondary,
                                           network_timeout_ms,
                                           1,
                                           true,
                                           config.validate_http_egress,
                                           false);
    }

    Serial.printf("MODEM: APN autodetect begin candidates=%u configured_apn=%s\n",
                  (unsigned)config.apn_candidate_count,
                  config.apn);
    append_apn_probe_log("autodetect_begin", 0, "all", config.apn, "running", "candidate_scan");
    if (config.apn_test_all)
        Serial.println("MODEM: APN test-all enabled; all usable APN candidates will be tested");

    bool identity_preferred = prefer_apn_candidate_by_sim_identity(config);

    Serial.println("MODEM: APN autodetect waiting for network registration before APN attach tests");
    set_pdp_context_ip(config.apn);
    if (!wait_for_network_registration(network_timeout_ms)) {
        Serial.println("MODEM: APN autodetect FAILED reason=registration_timeout; APN candidates not tried");
        modem_print_sim_network_status();
        append_apn_probe_log("autodetect_end", 0, "all", "", "fail", "registration_timeout_before_apn");
        return false;
    }
    Serial.println("MODEM: APN autodetect network registered; starting APN attach tests");
    log_operator_after_registration("apn_autodetect");
    if (!identity_preferred) {
        Serial.println("MODEM: SIM identity retry after registration before APN probing");
        identity_preferred = prefer_apn_candidate_by_sim_identity(config);
    }

    uint8_t attempted = 0;
    bool selected = false;
    char selected_supplier[33] = {0};
    char selected_apn[33] = {0};
    for (uint8_t i = 0; i < config.apn_candidate_count; i++) {
        const ModemConfig::ApnCandidate &candidate = config.apn_candidates[i];
        if (!candidate.apn[0]) {
            Serial.printf("MODEM: LTE-M APN probe skip supplier=%s reason=apn_pending\n",
                          candidate.supplier[0] ? candidate.supplier : "unknown");
            append_apn_probe_log("skip", i + 1, candidate.supplier, candidate.apn, "skipped", "apn_pending");
            continue;
        }

        attempted++;
        if (modem_validate_ltem_attempt(candidate.supplier,
                                        candidate.apn,
                                        config.lookup_primary,
                                        config.lookup_secondary,
                                        network_timeout_ms,
                                        attempted,
                                        false,
                                        config.validate_http_egress,
                                        true)) {
            if (!selected) {
                selected = true;
                strlcpy(selected_supplier, candidate.supplier[0] ? candidate.supplier : "unknown", sizeof(selected_supplier));
                strlcpy(selected_apn, candidate.apn, sizeof(selected_apn));
                strlcpy(config.apn, selected_apn, sizeof(config.apn));
                Serial.printf("MODEM: APN autodetect selected supplier=%s apn=%s attempt=%u\n",
                              selected_supplier,
                              config.apn,
                              (unsigned)attempted);
                append_apn_probe_log("select", attempted, selected_supplier, config.apn, "pass", "selected");
            }
            if (!config.apn_test_all) {
                append_apn_probe_log("autodetect_end", attempted, selected_supplier, config.apn, "pass", "selected");
                return true;
            }
        }
    }

    if (selected) {
        strlcpy(config.apn, selected_apn, sizeof(config.apn));
        Serial.printf("MODEM: APN test-all complete selected supplier=%s apn=%s attempted=%u\n",
                      selected_supplier,
                      config.apn,
                      (unsigned)attempted);
        append_apn_probe_log("autodetect_end", attempted, selected_supplier, config.apn, "pass", "test_all_complete");
        return true;
    }

    Serial.printf("MODEM: APN autodetect FAILED attempted=%u candidates=%u\n",
                  (unsigned)attempted,
                  (unsigned)config.apn_candidate_count);
    append_apn_probe_log("autodetect_end", attempted, "all", "", "fail", attempted ? "all_candidates_failed" : "no_usable_apn_candidates");
    return false;
}

bool modem_test_http_egress(uint32_t timeout_ms)
{
    static const char *host = "prod.dm.kpnthings.com";
    static const char *path = "/ingestion/ip/senml/v1";
    static const char *body = "[]";
    static const char *dummy_token = "0000000000000000000000000000000000000000000000000000000000000000";

    Serial.printf("MODEM: HTTP egress test begin method=TinyGSM_TLS host=%s path=%s\n", host, path);

    TinyGsmClientSecure client(modem, 1);
    client.setTimeout(timeout_ms);
    int connect_timeout_s = timeout_ms_to_connect_seconds(timeout_ms);
    Serial.printf("MODEM: HTTP egress connect timeout_s=%d\n", connect_timeout_s);
    if (!client.connect(host, 443, connect_timeout_s)) {
        Serial.println("MODEM: HTTP egress connect FAILED");
        print_at_raw_response("HTTP egress APP PDP raw", "+CNACT?", 3000);
        return false;
    }
    Serial.println("MODEM: HTTP egress connect OK");

    String request;
    request.reserve(256);
    request += "POST ";
    request += path;
    request += " HTTP/1.1\r\nHost: ";
    request += host;
    request += "\r\nThings-Message-Token: ";
    request += dummy_token;
    request += "\r\nContent-Type: application/json\r\nContent-Length: ";
    request += strlen(body);
    request += "\r\nConnection: close\r\n\r\n";
    request += body;
    size_t written = client.print(request);
    client.flush();
    Serial.printf("MODEM: HTTP egress request written bytes=%u\n", (unsigned)written);
    if (written == 0) {
        client.stop();
        Serial.println("MODEM: HTTP egress request write FAILED");
        return false;
    }

    uint32_t start = millis();
    bool saw_closed = false;
    uint32_t closed_ms = 0;
    int http_status = -1;
    String status_line;
    while (millis() - start < timeout_ms) {
        while (client.available()) {
            status_line = client.readStringUntil('\n');
            status_line.trim();
            if (status_line.length() == 0)
                continue;

            Serial.printf("MODEM: HTTP egress status line [%s]\n", status_line.c_str());
            int marker = status_line.indexOf("HTTP/");
            if (marker >= 0 && status_line.length() >= marker + 12) {
                int first_space = status_line.indexOf(' ', marker);
                if (first_space >= 0)
                    http_status = status_line.substring(first_space + 1, first_space + 4).toInt();
            }

            client.stop();
            bool ok = http_status >= 100 && http_status < 600;
            Serial.printf("MODEM: HTTP egress status=%d result=%s\n",
                          http_status,
                          ok ? "PASS" : "FAIL");
            return ok;
        }

        if (!client.connected() && !saw_closed) {
            saw_closed = true;
            closed_ms = millis();
            Serial.println("MODEM: HTTP egress socket closed before status; polling for buffered data");
        } else if (saw_closed && millis() - closed_ms >= 1500UL) {
            client.stop();
            Serial.println("MODEM: HTTP egress no status after close; connect+request_write treated as PASS");
            return true;
        }
        delay(10);
    }

    client.stop();
    Serial.println("MODEM: HTTP egress no status received; connect+request_write treated as PASS");
    return true;
}

static bool modem_upload_azure_blob_from_sd_impl(const char *local_path,
                                                 const char *blob_name,
                                                 const char *apn,
                                                 uint32_t timeout_ms,
                                                 uint32_t data_connect_timeout_ms,
                                                 const char *content_type)
{
    if (!AZURE_BLOB_HOST[0] || !AZURE_BLOB_CONTAINER[0] || !AZURE_BLOB_SAS[0]) {
        Serial.println("AZURE: upload skipped; AZURE_BLOB_* secrets are not configured");
        return false;
    }
    if (!local_path || !local_path[0] || !blob_name || !blob_name[0]) {
        Serial.println("AZURE: upload FAILED reason=missing_path_or_blob_name");
        return false;
    }

    Serial.println("AZURE: upload begin method=VSTComm");
    Serial.printf("AZURE: open SD path=%s\n", local_path);
    File file = SD_MMC.open(local_path, "r");
    if (!file) {
        Serial.println("AZURE: upload FAILED reason=sd_file_open_failed");
        return false;
    }

    size_t file_size = file.size();
    Serial.printf("AZURE: file_size=%u blob=%s host=%s container=%s sas_len=%u\n",
                  (unsigned)file_size,
                  blob_name,
                  AZURE_BLOB_HOST,
                  AZURE_BLOB_CONTAINER,
                  (unsigned)strlen(AZURE_BLOB_SAS));
    if (file_size == 0) {
        file.close();
        Serial.println("AZURE: upload FAILED reason=empty_file");
        return false;
    }

    if (!modem.isGprsConnected()) {
        if (!apn || !apn[0]) {
            file.close();
            Serial.println("AZURE: upload FAILED reason=no_active_ltem_data");
            return false;
        }

        if (data_connect_timeout_ms > 0) {
            Serial.printf("AZURE: no data active, bounded APP PDP connect apn=%s timeout_ms=%lu\n",
                          apn,
                          (unsigned long)data_connect_timeout_ms);
        } else {
            Serial.printf("AZURE: no data active, gprsConnect apn=%s\n", apn);
        }
        bool connected = data_connect_timeout_ms > 0
            ? activate_app_pdp_context_bounded(apn, data_connect_timeout_ms)
            : modem.gprsConnect(apn);
        if (!connected) {
            file.close();
            Serial.println("AZURE: upload FAILED reason=data_connect_failed");
            return false;
        }
        Serial.println("AZURE: data active after connect");
    } else {
        Serial.println("AZURE: data already active");
    }

    String path;
    path.reserve(strlen(AZURE_BLOB_CONTAINER) + strlen(blob_name) + strlen(AZURE_BLOB_SAS) + 8);
    path += "/";
    path += AZURE_BLOB_CONTAINER;
    path += "/";
    path += blob_name;
    path += "?";
    path += AZURE_BLOB_SAS;

    String headers;
    headers.reserve(path.length() + 220);
    headers += "PUT ";
    headers += path;
    headers += " HTTP/1.1\r\nHost: ";
    headers += AZURE_BLOB_HOST;
    headers += "\r\nx-ms-blob-type: BlockBlob\r\n";
    headers += "x-ms-version: 2020-10-02\r\n";
    headers += "Content-Length: ";
    headers += String(file_size);
    headers += "\r\nContent-Type: ";
    headers += (content_type && content_type[0]) ? content_type : "application/octet-stream";
    headers += "\r\nConnection: close\r\n\r\n";

    Serial.printf("AZURE: HTTP PUT path=/%s/%s?<sas-redacted>\n", AZURE_BLOB_CONTAINER, blob_name);
    Serial.printf("AZURE: connect host=%s port=443 method=TinyGSM_default_secure_client\n", AZURE_BLOB_HOST);
    TinyGsmClientSecure client(modem);
    client.setTimeout(timeout_ms);
    bool connected = client.connect(AZURE_BLOB_HOST, 443);
    Serial.printf("AZURE: connect result=%s\n", connected ? "OK" : "FAIL");
    if (!connected) {
        client.stop();
        file.close();
        Serial.println("AZURE: upload FAILED reason=tls_connect_failed");
        return false;
    }

    size_t header_written = client.print(headers);
    Serial.printf("AZURE: headers_written=%u\n", (unsigned)header_written);
    if (header_written == 0) {
        client.stop();
        file.close();
        Serial.println("AZURE: upload FAILED reason=header_write_failed");
        return false;
    }

    uint8_t buffer[1024];
    size_t total_sent = 0;
    while (file.available()) {
        int n = file.read(buffer, sizeof(buffer));
        if (n <= 0)
            break;
        size_t written = client.write(buffer, (size_t)n);
        total_sent += written;
        if (written != (size_t)n) {
            Serial.printf("AZURE: body chunk short write requested=%d written=%u\n", n, (unsigned)written);
            break;
        }
    }
    file.close();
    client.flush();
    Serial.printf("AZURE: body_written=%u expected=%u\n", (unsigned)total_sent, (unsigned)file_size);

    if (total_sent != file_size) {
        client.stop();
        Serial.println("AZURE: upload FAILED reason=body_write_incomplete");
        return false;
    }

    String response;
    response.reserve(512);
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        while (client.available()) {
            char c = (char)client.read();
            if (response.length() < 700)
                response += c;
        }
        if (!client.connected() && !client.available())
            break;
        delay(10);
    }
    client.stop();

    if (response.length() == 0) {
        Serial.println("AZURE: no HTTP response received; connect+all_bytes_written treated as PASS");
        return true;
    }

    int status = -1;
    int marker = response.indexOf("HTTP/");
    if (marker >= 0) {
        int first_space = response.indexOf(' ', marker);
        if (first_space >= 0)
            status = response.substring(first_space + 1, first_space + 4).toInt();
    }
    Serial.printf("AZURE: HTTP status=%d response_bytes=%u\n", status, (unsigned)response.length());

    if (status == 201 || status == 202) {
        Serial.println("AZURE: upload PASS");
        return true;
    }
    if (response.indexOf("AuthenticationFailed") >= 0)
        Serial.println("AZURE: upload FAILED reason=authentication_failed");
    else if (response.indexOf("ContainerNotFound") >= 0)
        Serial.println("AZURE: upload FAILED reason=container_not_found");
    else
    Serial.println("AZURE: upload FAILED reason=http_status_or_unknown_response");
    return false;
}

bool modem_upload_azure_blob_from_sd(const char *local_path,
                                     const char *blob_name,
                                     const char *apn,
                                     uint32_t timeout_ms,
                                     const char *content_type)
{
    return modem_upload_azure_blob_from_sd_impl(local_path,
                                                blob_name,
                                                apn,
                                                timeout_ms,
                                                timeout_ms,
                                                content_type);
}

bool modem_upload_azure_blob_from_sd_runtime(const char *local_path,
                                             const char *blob_name,
                                             const char *apn,
                                             bool operator_auto_select,
                                             bool wake_if_needed,
                                             uint32_t timeout_ms,
                                             bool power_down_after,
                                             uint32_t data_connect_timeout_ms,
                                             const char *content_type)
{
    bool woke_for_upload = false;
    if (!g_serial_ready || !modem.testAT(1000)) {
        if (!wake_if_needed) {
            Serial.println("AZURE: runtime upload skipped reason=modem_off_wake_disabled");
            return false;
        }

        Serial.println("AZURE: runtime upload wake begin");
        if (!modem_init_early(operator_auto_select)) {
            Serial.println("AZURE: runtime upload wake FAIL");
            return false;
        }
        woke_for_upload = true;
        Serial.println("AZURE: runtime upload wake OK");
    }

    Serial.println("AZURE: runtime upload registration check begin");
    bool registered = wait_for_network_registration(15000);
    Serial.printf("AZURE: runtime upload registration check result=%s\n", registered ? "OK" : "FAIL");
    if (!registered) {
        if (woke_for_upload || power_down_after)
            modem_power_down_runtime("runtime_azure_registration_failed");
        return false;
    }

    bool ok = modem_upload_azure_blob_from_sd_impl(local_path,
                                                  blob_name,
                                                  apn,
                                                  timeout_ms,
                                                  data_connect_timeout_ms,
                                                  content_type);
    if (woke_for_upload || power_down_after)
        modem_power_down_runtime(ok ? "runtime_azure_done" : "runtime_azure_failed");
    return ok;
}

static int wait_sms_final_response_raw(uint32_t timeout_ms)
{
    uint32_t start_ms = millis();
    String line;
    line.reserve(96);
    bool saw_cmgs = false;

    while (millis() - start_ms < timeout_ms) {
        while (modem.stream.available() > 0) {
            char c = (char)modem.stream.read();
            if (c == '\r')
                continue;

            if (c == '\n') {
                line.trim();
                if (line.length() > 0) {
                    Serial.printf("MODEM: SMS final rx [%s]\n", line.c_str());
                    if (line.startsWith("+CMGS:"))
                        saw_cmgs = true;
                    if (line == "OK")
                        return 1;
                    if (line.indexOf("ERROR") >= 0)
                        return -1;
                }
                line = "";
                continue;
            }

            if (line.length() < 180) {
                if ((uint8_t)c >= 0x20 && (uint8_t)c <= 0x7E)
                    line += c;
                else {
                    char escaped[6];
                    snprintf(escaped, sizeof(escaped), "<%02X>", (unsigned)(uint8_t)c);
                    line += escaped;
                }
            }
        }
        delay(5);
    }

    line.trim();
    if (line.length() > 0)
        Serial.printf("MODEM: SMS final rx partial [%s]\n", line.c_str());
    Serial.printf("MODEM: SMS final response timeout saw_cmgs=%s\n", saw_cmgs ? "YES" : "NO");
    return saw_cmgs ? 1 : 0;
}

bool modem_send_sms_text(const char *number, const char *message, uint32_t submit_timeout_ms)
{
    if (!number || !number[0] || !message || !message[0]) {
        Serial.println("MODEM: SMS skipped reason=missing_number_or_message");
        return false;
    }

    if (!g_serial_ready || !modem.testAT(1000)) {
        Serial.printf("MODEM: SMS send FAIL number=%s reason=modem_not_ready\n", number);
        return false;
    }

    Serial.printf("MODEM: SMS send begin number=%s chars=%u\n",
                  number,
                  (unsigned)strlen(message));

    modem.sendAT("+CMEE=2");
    modem.waitResponse(2000);

    Serial.println("MODEM: SMS set text mode AT+CMGF=1");
    modem.sendAT("+CMGF=1");
    if (modem.waitResponse(5000) != 1) {
        Serial.printf("MODEM: SMS send FAIL number=%s reason=cmgf_failed\n", number);
        return false;
    }

    Serial.println("MODEM: SMS set charset AT+CSCS=\"GSM\"");
    modem.sendAT("+CSCS=\"GSM\"");
    if (modem.waitResponse(5000) != 1) {
        Serial.printf("MODEM: SMS send FAIL number=%s reason=cscs_failed\n", number);
        return false;
    }

    Serial.printf("MODEM: SMS request prompt number=%s timeout_ms=15000\n", number);
    modem.sendAT("+CMGS=\"", number, "\"");
    if (modem.waitResponse(15000, ">") != 1) {
        Serial.printf("MODEM: SMS send FAIL number=%s reason=prompt_timeout\n", number);
        modem.stream.write((char)0x1B);
        modem.stream.flush();
        modem.waitResponse(1000);
        return false;
    }

    Serial.println("MODEM: SMS prompt OK; writing body");
    modem.stream.print(message);
    modem.stream.write((char)0x1A);
    modem.stream.flush();

    if (submit_timeout_ms < 30000)
        submit_timeout_ms = 30000;
    if (submit_timeout_ms > 120000)
        submit_timeout_ms = 120000;

    Serial.printf("MODEM: SMS waiting final response number=%s timeout_ms=%lu\n",
                  number,
                  (unsigned long)submit_timeout_ms);
    int response = wait_sms_final_response_raw(submit_timeout_ms);
    bool ok = response == 1;
    Serial.printf("MODEM: SMS send %s number=%s response=%d\n", ok ? "PASS" : "FAIL", number, response);
    return ok;
}

bool modem_send_sms_text_runtime(const char *number,
                                 const char *message,
                                 const char *apn,
                                 bool operator_auto_select,
                                 bool wake_if_needed,
                                 uint32_t submit_timeout_ms,
                                 bool power_down_after)
{
    bool woke_for_sms = false;
    if (!g_serial_ready || !modem.testAT(1000)) {
        if (!wake_if_needed) {
            Serial.println("MODEM: SMS runtime skipped reason=modem_off_wake_disabled");
            return false;
        }

        Serial.println("MODEM: SMS runtime wake begin");
        if (!modem_init_early(operator_auto_select)) {
            Serial.println("MODEM: SMS runtime wake FAIL");
            return false;
        }
        woke_for_sms = true;
        Serial.println("MODEM: SMS runtime wake OK");
    }

    Serial.println("MODEM: SMS runtime registration check begin");
    bool registered = wait_for_network_registration(10000);
    Serial.printf("MODEM: SMS runtime registration check result=%s\n", registered ? "OK" : "FAIL");

    bool ok = modem_send_sms_text(number, message, submit_timeout_ms);
    (void)apn;
    if (woke_for_sms && power_down_after)
        modem_power_down_runtime(ok ? "runtime_sms_done" : "runtime_sms_failed");
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
    log_operator_after_registration("timestamp");

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

void modem_power_down_runtime(const char *reason)
{
    (void)reason;

    if (g_serial_ready) {
        modem.sendAT("+CGNSPWR=0");
        modem.waitResponse(1000);
        modem.sendAT("+CPOWD=1");
        modem.waitResponse(3000);
        Serial1.end();
        g_serial_ready = false;
    }

    if (g_pmu_ready) {
        PMU.disableBLDO2();
        PMU.disableDC3();
    }

    g_modem_initialized = false;
}

void modem_prepare_for_sleep()
{
    Serial.println("MODEM: preparing for deep sleep");

    if (g_serial_ready) {
        modem.sendAT("+CGNSPWR=0");
        modem.waitResponse(2000);
        modem.sendAT("+CPOWD=1");
        modem.waitResponse(3000);
        Serial1.end();
        g_serial_ready = false;
    }

    if (g_pmu_ready) {
        PMU.disableBLDO2();
        PMU.disableDC3();
        Serial.println("MODEM: GNSS off; modem rails disabled");
    } else {
        Serial.println("MODEM: PMU was not ready; rails not changed");
    }
    g_modem_initialized = false;
    Serial.flush();
}
