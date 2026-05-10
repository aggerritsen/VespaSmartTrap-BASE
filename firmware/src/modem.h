#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// Returns true if PMU + modem AT are ready (or at least AT works).
bool modem_init_early();

// Tries to obtain a *plausible* modem timestamp via AT+CCLK?
// Output format: "YYYYMMDD_HHMMSS"
// Returns true if a plausible network time was obtained, false if fallback was used.
bool modem_get_timestamp(char *out, size_t out_len, uint32_t network_timeout_ms = 60000);

// Checks whether the modem is currently registered on the cellular network.
bool modem_check_network_registered(uint32_t timeout_ms = 5000);

// Prints SIM and registration state for diagnostics.
void modem_print_sim_network_status();

// Configures the APN and validates LTE-M data by pinging either lookup host.
bool modem_validate_ltem(const char *apn,
                         const char *lookup_primary,
                         const char *lookup_secondary,
                         uint32_t network_timeout_ms = 60000);

// No-credential public HTTP GET to prove DNS + TCP + HTTP egress.
bool modem_test_world_clock(uint32_t timeout_ms = 15000);

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
