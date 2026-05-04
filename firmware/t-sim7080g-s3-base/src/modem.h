#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// Returns true if PMU + modem AT are ready (or at least AT works).
bool modem_init_early();

// Tries to obtain a *plausible* modem timestamp via AT+CCLK?
// Output format: "YYYYMMDD_HHMMSS"
// Returns true if a plausible network time was obtained, false if fallback was used.
bool modem_get_timestamp(char *out, size_t out_len);

struct ModemGnssInfo {
    bool command_ok = false;
    bool powered = false;
    bool fix = false;
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
