//sdcard.cpp

#include "sdcard.h"
#include <SD_MMC.h>

/* =============================
   SD PIN CONFIG (T-SIM7080G-S3)
   ============================= */
#define SD_CMD  39
#define SD_CLK  38
#define SD_DATA 40

static bool sd_ok = false;

static const char *sd_card_type_name(uint8_t type)
{
    switch (type) {
        case CARD_MMC: return "MMC";
        case CARD_SD: return "SDSC";
        case CARD_SDHC: return "SDHC/SDXC";
        default: return "UNKNOWN";
    }
}

/* =============================
   INIT
   ============================= */
bool sdcard_init()
{
    Serial.println("SD: initializing SD_MMC with custom pins");
    Serial.printf("SD: CLK=%d CMD=%d DATA=%d mode=1-bit\n", SD_CLK, SD_CMD, SD_DATA);

    SD_MMC.setPins(SD_CLK, SD_CMD, SD_DATA);

    if (!SD_MMC.begin("/sdcard", true))
    {
        Serial.println("SD: mount FAILED");
        sd_ok = false;
        return false;
    }

    uint64_t size = SD_MMC.cardSize();
    uint64_t used = SD_MMC.usedBytes();
    uint8_t type = SD_MMC.cardType();

    Serial.printf("SD: mounted OK\n");
    Serial.printf("SD: card_type=%s (%u)\n", sd_card_type_name(type), type);
    Serial.printf("SD: size_mb=%llu\n", size / (1024 * 1024));
    Serial.printf("SD: used_bytes=%llu total_bytes=%llu\n", used, size);
    Serial.printf("SD: free_bytes=%llu\n", size > used ? (size - used) : 0);

    sd_ok = true;
    return true;
}

bool sdcard_available()
{
    return sd_ok;
}

bool sdcard_append_log(const char *path, const String &text)
{
    if (!sd_ok || !path || !path[0])
        return false;

    File f = SD_MMC.open(path, FILE_APPEND);
    if (!f)
    {
        Serial.printf("SD: log open FAILED path=%s\n", path);
        return false;
    }

    size_t written = f.print(text);
    f.close();

    if (written != text.length())
    {
        Serial.printf("SD: log write incomplete path=%s written=%u expected=%u\n",
                      path,
                      (unsigned)written,
                      (unsigned)text.length());
        return false;
    }

    Serial.printf("SD: log appended path=%s bytes=%u\n", path, (unsigned)written);
    return true;
}

bool sdcard_write_log(const char *path, const String &text)
{
    if (!sd_ok || !path || !path[0])
        return false;

    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f)
    {
        Serial.printf("SD: log open FAILED path=%s\n", path);
        return false;
    }

    size_t written = f.print(text);
    f.close();

    if (written != text.length())
    {
        Serial.printf("SD: log write incomplete path=%s written=%u expected=%u\n",
                      path,
                      (unsigned)written,
                      (unsigned)text.length());
        return false;
    }

    Serial.printf("SD: log written path=%s bytes=%u\n", path, (unsigned)written);
    return true;
}

/* =============================
   SAVE JPEG
   ============================= */
bool sdcard_save_jpeg(uint32_t frame_id, const uint8_t *data, size_t len)
{
    if (!sd_ok)
        return false;

    char path[32];
    snprintf(path, sizeof(path), "/frame_%06lu.jpg", frame_id);

    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f)
    {
        Serial.printf("SD: open FAILED path=%s\n", path);
        return false;
    }

    size_t written = f.write(data, len);
    f.close();

    if (written != len)
    {
        Serial.printf("SD: write incomplete written=%u expected=%u\n", written, len);
        return false;
    }

    Serial.printf("SD: jpeg saved path=%s bytes=%u\n", path, len);
    return true;
}
