//sdcard.cpp

#include "sdcard.h"
#include "config_example_generated.h"
#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <esp_log.h>
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
static char configured_image_prefix[33] = "/frame_";
static uint32_t active_jpeg_bytes = 0;
static uint32_t last_mount_attempt_ms = 0;
static uint32_t last_sd_failure_log_ms = 0;
static constexpr uint32_t SD_REMOUNT_BACKOFF_MS = 5000;

static const char *CONFIG_PATH = "/config.json";
static const char *sd_card_type_name(uint8_t type)
{
    switch (type) {
        case CARD_MMC: return "MMC";
        case CARD_SD: return "SDSC";
        case CARD_SDHC: return "SDHC/SDXC";
        default: return "UNKNOWN";
    }
}

static void sdcard_set_driver_log_level(esp_log_level_t level)
{
    esp_log_level_set("sdmmc_req", level);
    esp_log_level_set("diskio_sdmmc", level);
    esp_log_level_set("sdmmc_common", level);
    esp_log_level_set("vfs_fat_sdmmc", level);
}

class SdDriverLogSilencer {
public:
    SdDriverLogSilencer()
    {
        sdcard_set_driver_log_level(ESP_LOG_NONE);
    }

    ~SdDriverLogSilencer()
    {
        sdcard_set_driver_log_level(ESP_LOG_ERROR);
    }
};

static void sdcard_mark_failed(const char *reason)
{
    sdcard_abort_jpeg();
    sd_ok = false;

    uint32_t now = millis();
    if (last_sd_failure_log_ms == 0 || now - last_sd_failure_log_ms > SD_REMOUNT_BACKOFF_MS) {
        Serial.printf("SD: marked unavailable reason=%s; will retry mount\n", reason ? reason : "io_failed");
        last_sd_failure_log_ms = now;
    }

    {
        SdDriverLogSilencer quiet;
        SD_MMC.end();
    }
}

static bool sdcard_try_mount(bool verbose)
{
    uint32_t now = millis();
    if (last_mount_attempt_ms != 0 && now - last_mount_attempt_ms < SD_REMOUNT_BACKOFF_MS)
        return false;
    last_mount_attempt_ms = now;

    if (verbose) {
        Serial.println("SD: initializing SD_MMC with custom pins");
        Serial.printf("SD: CLK=%d CMD=%d DATA=%d mode=1-bit\n", SD_CLK, SD_CMD, SD_DATA);
    }

    bool mounted = false;
    {
        SdDriverLogSilencer quiet;
        SD_MMC.setPins(SD_CLK, SD_CMD, SD_DATA);
        mounted = SD_MMC.begin("/sdcard", true);
    }

    if (!mounted)
    {
        if (verbose)
            Serial.println("SD: mount FAILED");
        sd_ok = false;
        return false;
    }

    uint64_t size = SD_MMC.cardSize();
    uint64_t used = SD_MMC.usedBytes();
    uint8_t type = SD_MMC.cardType();

    if (size == 0) {
        if (verbose)
            Serial.println("SD: mount FAILED card size is 0");
        {
            SdDriverLogSilencer quiet;
            SD_MMC.end();
        }
        sd_ok = false;
        return false;
    }

    if (verbose || !sd_ok) {
        Serial.printf("SD: mounted OK\n");
        Serial.printf("SD: card_type=%s (%u)\n", sd_card_type_name(type), type);
        Serial.printf("SD: size_mb=%llu\n", size / (1024 * 1024));
        Serial.printf("SD: used_bytes=%llu total_bytes=%llu\n", used, size);
        Serial.printf("SD: free_bytes=%llu\n", size > used ? (size - used) : 0);
    }

    sd_ok = true;
    return true;
}

static bool sdcard_ensure_ready()
{
    if (sd_ok)
        return true;

    return sdcard_try_mount(false);
}

static JsonObject ensure_object(JsonDocument &doc, const char *name, bool &changed)
{
    JsonVariant v = doc[name];
    if (v.is<JsonObject>())
        return v.as<JsonObject>();

    changed = true;
    return doc[name].to<JsonObject>();
}

static bool merge_missing_config(JsonVariant target, JsonVariantConst defaults)
{
    if (!defaults.is<JsonObjectConst>())
        return false;

    bool changed = false;
    JsonObject target_obj = target.as<JsonObject>();
    for (JsonPairConst kv : defaults.as<JsonObjectConst>()) {
        JsonVariant target_value = target_obj[kv.key()];
        JsonVariantConst default_value = kv.value();
        if (target_value.isNull()) {
            target_obj[kv.key()].set(default_value);
            changed = true;
        } else if (target_value.is<JsonObject>() && default_value.is<JsonObjectConst>()) {
            changed |= merge_missing_config(target_value, default_value);
        }
    }
    return changed;
}

static bool profile_identity_matches(JsonObject profile, JsonObjectConst defaults)
{
    const char *supplier = profile["supplier"] | "";
    const char *apn = profile["apn"] | "";
    const char *default_supplier = defaults["supplier"] | "";
    const char *default_apn = defaults["apn"] | "";

    if (supplier[0] && default_supplier[0] && strcasecmp(supplier, default_supplier) == 0)
        return true;
    if (supplier[0] || default_supplier[0])
        return false;
    if (apn[0] && default_apn[0] && strcasecmp(apn, default_apn) == 0)
        return true;

    return false;
}

static bool merge_profile_prefix_array(JsonObject profile, JsonObjectConst defaults, const char *name)
{
    JsonArrayConst default_prefixes = defaults[name];
    if (default_prefixes.isNull() || default_prefixes.size() == 0)
        return false;

    JsonArray prefixes = profile[name];
    if (!prefixes.isNull() && prefixes.size() > 0)
        return false;

    profile[name].set(default_prefixes);
    return true;
}

static bool merge_profile_value_default(JsonObject profile, JsonObjectConst defaults, const char *name)
{
    if (!profile[name].isNull() || defaults[name].isNull())
        return false;

    profile[name].set(defaults[name]);
    return true;
}

static bool merge_sim_profile_prefix_defaults(JsonDocument &doc, JsonDocument &defaults)
{
    JsonArray profiles = doc["modem"]["sim_profiles"];
    JsonArrayConst default_profiles = defaults["modem"]["sim_profiles"];
    if (profiles.isNull() || default_profiles.isNull())
        return false;

    bool changed = false;
    for (JsonObject profile : profiles) {
        for (JsonObjectConst default_profile : default_profiles) {
            if (!profile_identity_matches(profile, default_profile))
                continue;

            changed |= merge_profile_prefix_array(profile, default_profile, "imsi_prefixes");
            changed |= merge_profile_prefix_array(profile, default_profile, "iccid_prefixes");
            changed |= merge_profile_value_default(profile, default_profile, "direct_sms");
            break;
        }
    }

    return changed;
}

static bool remove_key(JsonObject obj, const char *name)
{
    if (obj[name].isNull())
        return false;

    obj.remove(name);
    return true;
}

static bool remove_legacy_config_fields(JsonDocument &doc)
{
    bool changed = false;
    JsonObject root = doc.as<JsonObject>();

    changed |= remove_key(root, "gv2");
    changed |= remove_key(root, "_comment");

    JsonObject stepper = root["stepper"];
    if (!stepper.isNull())
        changed |= remove_key(stepper, "_start_direction_comment");

    JsonObject power = root["power"];
    if (!power.isNull())
        changed |= remove_key(power, "_log_interval_comment");

    JsonObject modem = root["modem"];
    if (!modem.isNull())
        changed |= remove_key(modem, "apn_choices");

    return changed;
}

static bool ensure_config_defaults(JsonDocument &doc)
{
    bool changed = false;
    JsonDocument defaults;
    DeserializationError err = deserializeJson(defaults, CONFIG_EXAMPLE_JSON);
    if (err) {
        Serial.printf("SD: embedded config.example parse FAILED error=%s\n", err.c_str());
        return changed;
    }

    if (!doc.is<JsonObject>()) {
        Serial.println("SD: config defaults skipped; root is not a JSON object");
        return changed;
    }

    JsonArray sms_recipients = doc["sms"]["recipients"];
    bool sms_recipients_present = !sms_recipients.isNull() && sms_recipients.size() > 0;

    changed |= merge_missing_config(doc.as<JsonVariant>(), defaults.as<JsonVariantConst>());
    changed |= merge_sim_profile_prefix_defaults(doc, defaults);
    if (sms_recipients_present && !doc["sms"]["enabled"].as<bool>()) {
        doc["sms"]["enabled"] = true;
        changed = true;
    }

    return changed;
}

static String strip_json_trailing_commas(const String &text)
{
    String cleaned;
    cleaned.reserve(text.length());

    for (size_t i = 0; i < text.length(); i++) {
        char c = text[i];
        if (c != ',') {
            cleaned += c;
            continue;
        }

        size_t j = i + 1;
        while (j < text.length()) {
            char n = text[j];
            if (n != ' ' && n != '\t' && n != '\r' && n != '\n')
                break;
            j++;
        }

        if (j < text.length() && (text[j] == '}' || text[j] == ']'))
            continue;

        cleaned += c;
    }

    return cleaned;
}

static DeserializationError parse_config_text(JsonDocument &doc, const String &text, bool &repaired)
{
    repaired = false;
    DeserializationError err = deserializeJson(doc, text);
    if (!err)
        return err;

    String cleaned = strip_json_trailing_commas(text);
    if (cleaned == text)
        return err;

    doc.clear();
    DeserializationError repaired_err = deserializeJson(doc, cleaned);
    if (!repaired_err)
        repaired = true;

    return repaired_err;
}

static void make_jpeg_path(char *path, size_t path_len, uint32_t frame_id)
{
    time_t now = time(nullptr);
    struct tm tm{};

    if (now > 1700000000 && localtime_r(&now, &tm)) {
        snprintf(path,
                 path_len,
                 "%s%04d%02d%02d_%02d%02d%02d_%06lu.jpg",
                 configured_image_prefix,
                 tm.tm_year + 1900,
                 tm.tm_mon + 1,
                 tm.tm_mday,
                 tm.tm_hour,
                 tm.tm_min,
                 tm.tm_sec,
                 (unsigned long)frame_id);
        return;
    }

    snprintf(path, path_len, "%suptime_%010lu_%06lu.jpg", configured_image_prefix, millis(), (unsigned long)frame_id);
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

static bool modem_candidate_exists(const ModemConfig &modem, const char *apn)
{
    if (!apn || !apn[0])
        return false;

    for (uint8_t i = 0; i < modem.apn_candidate_count; i++) {
        if (strcasecmp(modem.apn_candidates[i].apn, apn) == 0)
            return true;
    }

    return false;
}

static void modem_add_apn_candidate(ModemConfig &modem, const char *supplier, const char *apn)
{
    if (modem.apn_candidate_count >= ModemConfig::MAX_APN_CANDIDATES)
        return;

    ModemConfig::ApnCandidate &candidate = modem.apn_candidates[modem.apn_candidate_count++];
    String supplier_text = supplier && supplier[0] ? supplier : "unknown";
    String apn_text = apn ? apn : "";
    supplier_text.trim();
    apn_text.trim();
    strlcpy(candidate.supplier, supplier_text.length() ? supplier_text.c_str() : "unknown", sizeof(candidate.supplier));
    strlcpy(candidate.apn, apn_text.c_str(), sizeof(candidate.apn));
}

static void modem_add_sim_prefix(char prefixes[][13], uint8_t &count, const char *prefix)
{
    if (count >= ModemConfig::MAX_SIM_PREFIXES || !prefix || !prefix[0])
        return;

    String text = prefix;
    text.trim();
    if (!text.length())
        return;

    strlcpy(prefixes[count++], text.c_str(), 13);
}

static void modem_load_default_apn_candidates(ModemConfig &modem)
{
    modem.apn_candidate_count = 0;
    modem_add_apn_candidate(modem, "Onomondo", "onomondo");
    modem_add_apn_candidate(modem, "KPNThings", "internet.m2m");
    modem_add_apn_candidate(modem, "Wireless Logic Benelux", "");
    modem_add_apn_candidate(modem, "ThingsData/Tele2 2G-4G", "m2m.tele2.com");
    modem_add_apn_candidate(modem, "ThingsData/Tele2 5G", "iot.tele2.com");
}

static void modem_prefer_configured_apn(ModemConfig &modem)
{
    if (!modem.apn[0])
        return;

    for (uint8_t i = 0; i < modem.apn_candidate_count; i++) {
        if (strcasecmp(modem.apn_candidates[i].apn, modem.apn) != 0)
            continue;

        if (i == 0)
            return;

        ModemConfig::ApnCandidate preferred = modem.apn_candidates[i];
        for (uint8_t j = i; j > 0; j--)
            modem.apn_candidates[j] = modem.apn_candidates[j - 1];
        modem.apn_candidates[0] = preferred;
        return;
    }
}

/* =============================
   INIT
   ============================= */
bool sdcard_init()
{
    return sdcard_try_mount(true);
}

bool sdcard_available()
{
    return sd_ok;
}

bool sdcard_ensure_config()
{
    if (!sdcard_ensure_ready())
        return false;

    if (SD_MMC.exists(CONFIG_PATH))
    {
        File f;
        {
            SdDriverLogSilencer quiet;
            f = SD_MMC.open(CONFIG_PATH, FILE_READ);
        }
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
        {
            SdDriverLogSilencer quiet;
            f = SD_MMC.open(CONFIG_PATH, FILE_READ);
        }
        if (!f)
        {
            Serial.printf("SD: config reopen FAILED path=%s\n", CONFIG_PATH);
            return false;
        }

        String text = f.readString();
        f.close();
        JsonDocument doc;
        bool parse_repaired = false;
        DeserializationError err = parse_config_text(doc, text, parse_repaired);
        if (err)
        {
            Serial.printf("SD: config parse FAILED path=%s error=%s\n",
                          CONFIG_PATH,
                          err.c_str());
            return false;
        }

        bool changed = parse_repaired;
        changed |= ensure_config_defaults(doc);
        if (!changed)
            return true;

        if (SD_MMC.exists(CONFIG_PATH))
            SD_MMC.remove(CONFIG_PATH);

        {
            SdDriverLogSilencer quiet;
            f = SD_MMC.open(CONFIG_PATH, FILE_WRITE);
        }
        if (!f)
        {
            Serial.printf("SD: config update open FAILED path=%s\n", CONFIG_PATH);
            return false;
        }

        size_t written = 0;
        {
            SdDriverLogSilencer quiet;
            written = serializeJsonPretty(doc, f);
        }
        f.println();
        f.close();
        Serial.printf("SD: config updated with defaults path=%s bytes=%u\n",
                      CONFIG_PATH,
                      (unsigned)written);
        return true;
    }

    File f;
    {
        SdDriverLogSilencer quiet;
        f = SD_MMC.open(CONFIG_PATH, FILE_WRITE);
    }
    if (!f)
    {
        Serial.printf("SD: config create FAILED path=%s\n", CONFIG_PATH);
        return false;
    }

    size_t expected = strlen(CONFIG_EXAMPLE_JSON);
    size_t written = 0;
    {
        SdDriverLogSilencer quiet;
        written = f.print(CONFIG_EXAMPLE_JSON);
    }
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

    if (!sdcard_ensure_ready())
        return false;

    File f;
    {
        SdDriverLogSilencer quiet;
        f = SD_MMC.open(CONFIG_PATH, FILE_READ);
    }
    if (!f)
    {
        Serial.printf("SD: config load FAILED path=%s\n", CONFIG_PATH);
        return false;
    }

    String text = f.readString();
    f.close();
    JsonDocument doc;
    bool parse_repaired = false;
    DeserializationError err = parse_config_text(doc, text, parse_repaired);

    if (err)
    {
        Serial.printf("SD: config parse FAILED path=%s error=%s; using defaults\n",
                      CONFIG_PATH,
                      err.c_str());
        return false;
    }

    const char *device_name = doc["device_name"] | config.device_name;
    strlcpy(config.device_name, device_name, sizeof(config.device_name));

    JsonObject logging = doc["logging"];
    const char *post_log = logging["post_log"] | config.logging.post_log;
    const char *image_prefix = logging["image_prefix"] | config.logging.image_prefix;
    strlcpy(config.logging.post_log, post_log, sizeof(config.logging.post_log));
    strlcpy(config.logging.image_prefix, image_prefix, sizeof(config.logging.image_prefix));
    if (!config.logging.post_log[0] || config.logging.post_log[0] != '/')
        strlcpy(config.logging.post_log, "/post.log", sizeof(config.logging.post_log));
    if (!config.logging.image_prefix[0] || config.logging.image_prefix[0] != '/')
        strlcpy(config.logging.image_prefix, "/frame_", sizeof(config.logging.image_prefix));
    strlcpy(configured_image_prefix, config.logging.image_prefix, sizeof(configured_image_prefix));

    JsonObject features = doc["features"];
    config.features.gnss_probe = features["gnss_probe"] | config.features.gnss_probe;
    config.features.ack_frames = features["ack_frames"] | config.features.ack_frames;

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
    config.stepper.post_test_enabled =
        stepper["post_test_enabled"] | config.stepper.post_test_enabled;
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

    JsonObject time = doc["time"];
    config.time.network_timeout_seconds =
        time["network_timeout_seconds"] | config.time.network_timeout_seconds;
    config.time.allow_gnss_fallback =
        time["allow_gnss_fallback"] | config.time.allow_gnss_fallback;
    if (config.time.network_timeout_seconds == 0)
        config.time.network_timeout_seconds = 1;
    if (config.time.network_timeout_seconds > 300)
        config.time.network_timeout_seconds = 300;

    JsonObject modem = doc["modem"];
    config.modem.mode = modem["mode"] | config.modem.mode;
    const char *apn = modem["apn"] | config.modem.apn;
    config.modem.apn_autodetect = modem["apn_autodetect"] | config.modem.apn_autodetect;
    config.modem.apn_test_all = modem["apn_test_all"] | config.modem.apn_test_all;
    config.modem.validate_http_egress = modem["validate_http_egress"] | config.modem.validate_http_egress;
    config.modem.operator_auto_select = modem["operator_auto_select"] | config.modem.operator_auto_select;
    config.modem.keep_alive_after_post = modem["keep_alive_after_post"] | config.modem.keep_alive_after_post;
    config.modem.wake_for_runtime_sms = modem["wake_for_runtime_sms"] | config.modem.wake_for_runtime_sms;
    const char *lookup_primary = modem["lookup_primary"] | config.modem.lookup_primary;
    const char *lookup_secondary = modem["lookup_secondary"] | config.modem.lookup_secondary;
    String modem_apn = apn;
    modem_apn.trim();
    strlcpy(config.modem.apn, modem_apn.c_str(), sizeof(config.modem.apn));
    strlcpy(config.modem.lookup_primary, lookup_primary, sizeof(config.modem.lookup_primary));
    strlcpy(config.modem.lookup_secondary, lookup_secondary, sizeof(config.modem.lookup_secondary));
    if (config.modem.mode > 2)
        config.modem.mode = 0;
    if (!config.modem.apn[0])
        strlcpy(config.modem.apn, "internet.m2m", sizeof(config.modem.apn));
    if (!config.modem.lookup_primary[0])
        strlcpy(config.modem.lookup_primary, "1.1.1.1", sizeof(config.modem.lookup_primary));
    if (!config.modem.lookup_secondary[0])
        strlcpy(config.modem.lookup_secondary, "8.8.8.8", sizeof(config.modem.lookup_secondary));

    JsonArray apn_candidates = modem["apn_candidates"];
    config.modem.apn_candidate_count = 0;
    if (!apn_candidates.isNull()) {
        for (JsonVariant candidate_value : apn_candidates) {
            if (config.modem.apn_candidate_count >= ModemConfig::MAX_APN_CANDIDATES)
                break;

            JsonObject candidate = candidate_value.as<JsonObject>();
            if (candidate.isNull())
                continue;

            const char *supplier = candidate["supplier"] | "unknown";
            const char *candidate_apn = candidate["apn"] | "";
            modem_add_apn_candidate(config.modem, supplier, candidate_apn);
        }
    }
    if (config.modem.apn_candidate_count == 0)
        modem_load_default_apn_candidates(config.modem);

    JsonArray sim_profiles = modem["sim_profiles"];
    config.modem.sim_profile_count = 0;
    if (!sim_profiles.isNull()) {
        for (JsonVariant profile_value : sim_profiles) {
            if (config.modem.sim_profile_count >= ModemConfig::MAX_SIM_PROFILES)
                break;

            JsonObject profile_json = profile_value.as<JsonObject>();
            if (profile_json.isNull())
                continue;

            ModemConfig::SimProfile &profile = config.modem.sim_profiles[config.modem.sim_profile_count];
            const char *supplier = profile_json["supplier"] | "unknown";
            const char *profile_apn = profile_json["apn"] | "";
            String supplier_text = supplier && supplier[0] ? supplier : "unknown";
            String apn_text = profile_apn ? profile_apn : "";
            supplier_text.trim();
            apn_text.trim();
            strlcpy(profile.supplier, supplier_text.length() ? supplier_text.c_str() : "unknown", sizeof(profile.supplier));
            strlcpy(profile.apn, apn_text.c_str(), sizeof(profile.apn));
            profile.direct_sms = profile_json["direct_sms"] | profile.direct_sms;

            JsonArray imsi_prefixes = profile_json["imsi_prefixes"];
            if (!imsi_prefixes.isNull()) {
                for (JsonVariant prefix_value : imsi_prefixes) {
                    if (profile.imsi_prefix_count >= ModemConfig::MAX_SIM_PREFIXES)
                        break;
                    const char *prefix = prefix_value | "";
                    if (!prefix || !prefix[0])
                        continue;
                    String text = prefix;
                    text.trim();
                    if (text.length())
                        strlcpy(profile.imsi_prefixes[profile.imsi_prefix_count++], text.c_str(), sizeof(profile.imsi_prefixes[0]));
                }
            }

            JsonArray iccid_prefixes = profile_json["iccid_prefixes"];
            if (!iccid_prefixes.isNull()) {
                for (JsonVariant prefix_value : iccid_prefixes) {
                    if (profile.iccid_prefix_count >= ModemConfig::MAX_SIM_PREFIXES)
                        break;
                    const char *prefix = prefix_value | "";
                    modem_add_sim_prefix(profile.iccid_prefixes, profile.iccid_prefix_count, prefix);
                }
            }

            if (profile.apn[0] && (profile.imsi_prefix_count > 0 || profile.iccid_prefix_count > 0))
                config.modem.sim_profile_count++;
        }
    }
    if (config.modem.apn[0] && !modem_candidate_exists(config.modem, config.modem.apn)) {
        for (int i = (int)min<uint8_t>(config.modem.apn_candidate_count, ModemConfig::MAX_APN_CANDIDATES - 1); i > 0; i--)
            config.modem.apn_candidates[i] = config.modem.apn_candidates[i - 1];
        config.modem.apn_candidate_count = min<uint8_t>(config.modem.apn_candidate_count + 1, ModemConfig::MAX_APN_CANDIDATES);
        strlcpy(config.modem.apn_candidates[0].supplier, "configured", sizeof(config.modem.apn_candidates[0].supplier));
        strlcpy(config.modem.apn_candidates[0].apn, config.modem.apn, sizeof(config.modem.apn_candidates[0].apn));
    } else if (config.modem.apn_autodetect) {
        modem_prefer_configured_apn(config.modem);
    }

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
    if (!power["deep_sleep"].isNull())
        config.power.deep_sleep = power["deep_sleep"].as<uint8_t>();
    else
        config.power.deep_sleep = power["deep_sleep_enabled"] | config.power.deep_sleep;
    config.power.deep_sleep_start_hour =
        power["deep_sleep_start_hour"] | config.power.deep_sleep_start_hour;
    config.power.deep_sleep_end_hour =
        power["deep_sleep_end_hour"] | config.power.deep_sleep_end_hour;
    const char *reboot_cron = power["reboot_cron"] | config.power.reboot_cron;
    strlcpy(config.power.reboot_cron, reboot_cron, sizeof(config.power.reboot_cron));
    config.power.reboot_after_deep_sleep_wakeup =
        power["reboot_after_deep_sleep_wakeup"] | config.power.reboot_after_deep_sleep_wakeup;
    if (config.power.log_interval_seconds == 0)
        config.power.log_interval_seconds = 900;
    if (config.power.log_interval_seconds > 86400)
        config.power.log_interval_seconds = 86400;
    if (config.power.deep_sleep > 2)
        config.power.deep_sleep = 0;
    if (config.power.deep_sleep_start_hour > 23)
        config.power.deep_sleep_start_hour = 18;
    if (config.power.deep_sleep_end_hour > 23)
        config.power.deep_sleep_end_hour = 6;
    if (config.power.deep_sleep_start_hour == config.power.deep_sleep_end_hour)
        config.power.deep_sleep = 0;

    JsonObject health = doc["health"];
    config.health.led = health["led"] | config.health.led;
    config.health.led = config.health.led ? 1 : 0;

    JsonObject azure = doc["azure"];
    config.azure.max_uploads_per_boot =
        azure["max_uploads_per_boot"] | config.azure.max_uploads_per_boot;
    if (config.azure.max_uploads_per_boot > 20)
        config.azure.max_uploads_per_boot = 20;

    JsonObject sms = doc["sms"];
    config.sms.enabled = sms["enabled"] | config.sms.enabled;
    config.sms.post_test_enabled = sms["post_test_enabled"] | config.sms.post_test_enabled;
    config.sms.runtime_settle_ms = sms["runtime_settle_ms"] | config.sms.runtime_settle_ms;
    if (config.sms.runtime_settle_ms > 10000)
        config.sms.runtime_settle_ms = 10000;
    config.sms.runtime_delay_after_detection_seconds =
        sms["runtime_delay_after_detection_seconds"] | config.sms.runtime_delay_after_detection_seconds;
    if (config.sms.runtime_delay_after_detection_seconds > 3600)
        config.sms.runtime_delay_after_detection_seconds = 3600;
    config.sms.runtime_submit_timeout_ms =
        sms["runtime_submit_timeout_ms"] | config.sms.runtime_submit_timeout_ms;
    if (config.sms.runtime_submit_timeout_ms < 30000)
        config.sms.runtime_submit_timeout_ms = 30000;
    if (config.sms.runtime_submit_timeout_ms > 120000)
        config.sms.runtime_submit_timeout_ms = 120000;
    config.sms.cooldown_minutes = sms["cooldown_minutes"] | config.sms.cooldown_minutes;
    if (config.sms.cooldown_minutes > 10080)
        config.sms.cooldown_minutes = 10080;
    config.sms.failure_cooldown_seconds =
        sms["failure_cooldown_seconds"] | config.sms.failure_cooldown_seconds;
    if (config.sms.failure_cooldown_seconds > 86400)
        config.sms.failure_cooldown_seconds = 86400;
    config.sms.recipient_count = 0;
    JsonArray recipients = sms["recipients"];
    if (!recipients.isNull()) {
        for (JsonVariant recipient_value : recipients) {
            if (config.sms.recipient_count >= SmsConfig::MAX_RECIPIENTS)
                break;

            JsonObject recipient_json = recipient_value.as<JsonObject>();
            if (recipient_json.isNull())
                continue;

            const char *number = recipient_json["number"] | "";
            String number_text = number ? number : "";
            number_text.trim();
            if (!number_text.length())
                continue;

            const char *name = recipient_json["name"] | "";
            String name_text = name ? name : "";
            name_text.trim();

            SmsConfig::Recipient &recipient = config.sms.recipients[config.sms.recipient_count++];
            strlcpy(recipient.name, name_text.c_str(), sizeof(recipient.name));
            strlcpy(recipient.number, number_text.c_str(), sizeof(recipient.number));
        }
    }

    Serial.printf("SD: config loaded device=%s post_log=%s image_prefix=%s gnss_probe=%s ack_frames=%s uart_rx=%u uart_tx=%u uart_baud=%lu stepper_speed=%u stepper_rotation_deg=%u stepper_steps_per_rev=%u stepper_wait_ms=%u stepper_start_direction=%s stepper_post_test=%s inference_conf_threshold=%.3f inference_detected_class=%d inference_occurrence=%u web_mode=%u web_ssid=%s power_log_interval_seconds=%lu power_deep_sleep_mode=%u power_sleep_window=%02u:00-%02u:00 power_reboot_cron=\"%s\" power_reboot_after_deep_sleep_wakeup=%s health_led=%u azure_max_uploads_per_boot=%u sms_enabled=%s sms_post_test=%s sms_runtime_settle_ms=%u sms_runtime_delay_after_detection_seconds=%u sms_runtime_submit_timeout_ms=%lu sms_cooldown_minutes=%lu sms_recipients=%u sms_failure_cooldown_seconds=%lu time_network_timeout_seconds=%u time_gnss_fallback=%s modem_mode=%u modem_apn=%s modem_direct_sms=%s modem_apn_autodetect=%s modem_apn_test_all=%s modem_validate_http_egress=%s modem_operator_auto_select=%s modem_apn_candidates=%u modem_sim_profiles=%u modem_lookup_primary=%s modem_lookup_secondary=%s\n",
                  config.device_name,
                  config.logging.post_log,
                  config.logging.image_prefix,
                  config.features.gnss_probe ? "YES" : "NO",
                  config.features.ack_frames ? "YES" : "NO",
                  config.uart.rx_gpio,
                  config.uart.tx_gpio,
                  (unsigned long)config.uart.baud,
                  config.stepper.speed_steps_per_second,
                  config.stepper.rotation_degrees,
                  config.stepper.steps_per_revolution,
                  config.stepper.reverse_wait_ms,
                  config.stepper.start_direction,
                  config.stepper.post_test_enabled ? "YES" : "NO",
                  config.inference.confidence_threshold,
                  config.inference.detected_class,
                  config.inference.occurrence,
                  config.web.mode,
                  config.web.ssid,
                  (unsigned long)config.power.log_interval_seconds,
                  config.power.deep_sleep,
                  config.power.deep_sleep_start_hour,
                  config.power.deep_sleep_end_hour,
                  config.power.reboot_cron,
                  config.power.reboot_after_deep_sleep_wakeup ? "YES" : "NO",
                  config.health.led,
                  config.azure.max_uploads_per_boot,
                  config.sms.enabled ? "YES" : "NO",
                  config.sms.post_test_enabled ? "YES" : "NO",
                  config.sms.runtime_settle_ms,
                  config.sms.runtime_delay_after_detection_seconds,
                  (unsigned long)config.sms.runtime_submit_timeout_ms,
                  (unsigned long)config.sms.cooldown_minutes,
                  config.sms.recipient_count,
                  (unsigned long)config.sms.failure_cooldown_seconds,
                  config.time.network_timeout_seconds,
                  config.time.allow_gnss_fallback ? "YES" : "NO",
                  config.modem.mode,
                  config.modem.apn,
                  config.modem.direct_sms ? "YES" : "NO",
                  config.modem.apn_autodetect ? "YES" : "NO",
                  config.modem.apn_test_all ? "YES" : "NO",
                  config.modem.validate_http_egress ? "YES" : "NO",
                  config.modem.operator_auto_select ? "YES" : "NO",
                  config.modem.apn_candidate_count,
                  config.modem.sim_profile_count,
                  config.modem.lookup_primary,
                  config.modem.lookup_secondary);
    for (uint8_t i = 0; i < config.modem.apn_candidate_count; i++) {
        Serial.printf("SD: modem APN candidate #%u supplier=%s apn=%s%s\n",
                      (unsigned)(i + 1),
                      config.modem.apn_candidates[i].supplier,
                      config.modem.apn_candidates[i].apn[0] ? config.modem.apn_candidates[i].apn : "-",
                      config.modem.apn_candidates[i].apn[0] ? "" : " pending");
    }
    for (uint8_t i = 0; i < config.modem.sim_profile_count; i++) {
        Serial.printf("SD: modem SIM profile #%u supplier=%s apn=%s direct_sms=%s imsi_prefixes=%u iccid_prefixes=%u\n",
                      (unsigned)(i + 1),
                      config.modem.sim_profiles[i].supplier,
                      config.modem.sim_profiles[i].apn[0] ? config.modem.sim_profiles[i].apn : "-",
                      config.modem.sim_profiles[i].direct_sms ? "YES" : "NO",
                      config.modem.sim_profiles[i].imsi_prefix_count,
                      config.modem.sim_profiles[i].iccid_prefix_count);
    }

    return true;
}

bool sdcard_append_log(const char *path, const String &text)
{
    if (!path || !path[0] || !sdcard_ensure_ready())
        return false;

    File f;
    {
        SdDriverLogSilencer quiet;
        f = SD_MMC.open(path, FILE_APPEND);
    }
    if (!f)
    {
        Serial.printf("SD: log open FAILED path=%s\n", path);
        sdcard_mark_failed("log_open_failed");
        return false;
    }

    size_t written = 0;
    {
        SdDriverLogSilencer quiet;
        written = f.print(text);
    }
    f.close();

    if (written != text.length())
    {
        Serial.printf("SD: log write incomplete path=%s written=%u expected=%u\n",
                      path,
                      (unsigned)written,
                      (unsigned)text.length());
        sdcard_mark_failed("log_write_incomplete");
        return false;
    }

    Serial.printf("SD: log appended path=%s bytes=%u\n", path, (unsigned)written);
    return true;
}

bool sdcard_write_log(const char *path, const String &text)
{
    if (!path || !path[0] || !sdcard_ensure_ready())
        return false;

    {
        SdDriverLogSilencer quiet;
        if (SD_MMC.exists(path))
            SD_MMC.remove(path);
    }

    File f;
    {
        SdDriverLogSilencer quiet;
        f = SD_MMC.open(path, FILE_WRITE);
    }
    if (!f)
    {
        Serial.printf("SD: log open FAILED path=%s\n", path);
        sdcard_mark_failed("log_open_failed");
        return false;
    }

    size_t written = 0;
    {
        SdDriverLogSilencer quiet;
        written = f.print(text);
    }
    f.close();

    if (written != text.length())
    {
        Serial.printf("SD: log write incomplete path=%s written=%u expected=%u\n",
                      path,
                      (unsigned)written,
                      (unsigned)text.length());
        sdcard_mark_failed("log_write_incomplete");
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
    if (!sdcard_ensure_ready())
        return false;

    char path[64];
    make_jpeg_path(path, sizeof(path), frame_id);
    if (out_path && out_path_len > 0) {
        strlcpy(out_path, path, out_path_len);
    }

    {
        SdDriverLogSilencer quiet;
        if (SD_MMC.exists(path))
            SD_MMC.remove(path);
    }

    File f;
    {
        SdDriverLogSilencer quiet;
        f = SD_MMC.open(path, FILE_WRITE);
    }
    if (!f)
    {
        Serial.printf("SD: open FAILED path=%s\n", path);
        sdcard_mark_failed("jpeg_open_failed");
        return false;
    }

    size_t written = 0;
    {
        SdDriverLogSilencer quiet;
        written = f.write(data, len);
    }
    f.close();

    if (written != len)
    {
        Serial.printf("SD: write incomplete written=%u expected=%u\n", written, len);
        sdcard_mark_failed("jpeg_write_incomplete");
        return false;
    }

    Serial.printf("SD: jpeg saved path=%s bytes=%u\n", path, len);
    return true;
}

bool sdcard_begin_jpeg(uint32_t frame_id)
{
    if (!sdcard_ensure_ready())
        return false;

    sdcard_abort_jpeg();

    make_jpeg_path(active_jpeg_path, sizeof(active_jpeg_path), frame_id);
    {
        SdDriverLogSilencer quiet;
        if (SD_MMC.exists(active_jpeg_path))
            SD_MMC.remove(active_jpeg_path);
    }

    {
        SdDriverLogSilencer quiet;
        active_jpeg_file = SD_MMC.open(active_jpeg_path, FILE_WRITE);
    }
    active_jpeg_bytes = 0;

    if (!active_jpeg_file)
    {
        Serial.printf("SD: jpeg stream open FAILED path=%s\n", active_jpeg_path);
        active_jpeg_path[0] = '\0';
        sdcard_mark_failed("jpeg_stream_open_failed");
        return false;
    }

    Serial.printf("SD: jpeg stream begin path=%s\n", active_jpeg_path);
    return true;
}

bool sdcard_write_jpeg_chunk(const uint8_t *data, size_t len)
{
    if (!sd_ok || !active_jpeg_file || !data || len == 0)
        return false;

    size_t written = 0;
    {
        SdDriverLogSilencer quiet;
        written = active_jpeg_file.write(data, len);
    }
    if (written != len)
    {
        Serial.printf("SD: jpeg stream write incomplete path=%s written=%u expected=%u\n",
                      active_jpeg_path,
                      (unsigned)written,
                      (unsigned)len);
        sdcard_mark_failed("jpeg_stream_write_incomplete");
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
