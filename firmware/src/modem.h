#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

#include "sdcard.h"

// Returns true if PMU + modem AT are ready (or at least AT works).
bool modem_init_early(bool operator_auto_select = false);

// Tries to obtain a *plausible* modem timestamp via AT+CCLK?
// Output format: "YYYYMMDD_HHMMSS"
// Returns true if a plausible network time was obtained, false if fallback was used.
bool modem_get_timestamp(char *out, size_t out_len, uint32_t network_timeout_ms = 60000);

// Checks whether the modem is currently registered on the cellular network.
bool modem_check_network_registered(uint32_t timeout_ms = 5000);

// Cheap liveness probe used before expensive diagnostics.
bool modem_at_responsive(uint32_t timeout_ms = 1000);

// Prints SIM and registration state for diagnostics.
void modem_print_sim_network_status();

// Configures the APN and validates LTE-M data with a TLS HTTP egress check.
bool modem_validate_ltem(const char *apn,
                         const char *lookup_primary,
                         const char *lookup_secondary,
                         uint32_t network_timeout_ms = 60000);

// Tries configured APN candidates in order and stores the working APN in config.apn.
bool modem_validate_ltem_apn_candidates(ModemConfig &config,
                                        uint32_t network_timeout_ms = 60000);

// TLS HTTP egress probe against the configured validation endpoint.
bool modem_test_http_egress(uint32_t timeout_ms = 15000);

// Uploads an SD-card file to Azure Blob Storage using secrets from config_secrets.h.
bool modem_upload_azure_blob_from_sd(const char *local_path,
                                     const char *blob_name,
                                     const char *apn = nullptr,
                                     uint32_t timeout_ms = 20000);

struct ModemGnssInfo {
    bool command_ok = false;
    bool powered = false;
    bool fix = false;
    bool position_valid = false;
    bool utc_valid = false;
    bool utc_advancing = false;
    uint8_t satellite_count = 0;
    char utc[24] = {0};
    char latitude[16] = {0};
    char longitude[16] = {0};
    char altitude_m[16] = {0};
    char speed_kph[16] = {0};
    char satellites[8] = {0};
    char raw[180] = {0};
};

// Powers GNSS with AT+CGNSPWR=1 and samples AT+CGNSINF.
// Returns true when the GNSS AT command path works. A position fix is optional.
bool modem_gnss_probe(ModemGnssInfo &info, uint32_t sample_ms = 5000);

// Best-effort shutdown before ESP32 deep sleep.
void modem_prepare_for_sleep();
