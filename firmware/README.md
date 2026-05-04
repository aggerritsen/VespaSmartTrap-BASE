# T-SIM7080G-S3 Base Firmware

This PlatformIO project contains the owned receiver firmware for the VST-BASE T-SIM7080G-S3 base unit.

The firmware receives framed inference metadata and JPEG images from the Grove Vision AI V2 stick-on module over the custom PCB UART link. It performs boot diagnostics, initializes the modem and SD card, writes a POST log, validates incoming image frames, saves valid JPEGs, and acknowledges completed frames.

## Hardware Role

```text
Grove Vision AI V2 stick-on module
        |
        | UART2, 921600 baud, custom PCB
        v
T-SIM7080G-S3 base firmware
        |
        +-- SIM7080 modem time
        +-- SIM7080 GNSS probe
        +-- SD-MMC image and POST logging
        +-- USB serial POST/heartbeat monitor
```

## Board Pinout

![LILYGO T-SIM7080G-S3 Pinout](../docs/hardware/Lilygo_T_SIM7080G_S3_PINOUT.jpg)

For more board details, see the [LilyGO T-SIM7080G repository](https://github.com/Xinyuan-LilyGO/LilyGo-T-SIM7080G).

## Pins And Ports

| Function | Pin / Port | Notes |
| --- | --- | --- |
| USB serial monitor | COM5, 115200 baud | POST and heartbeat output |
| PlatformIO monitor | COM3, 115200 baud | Configured in `platformio.ini` |
| GV2 UART RX | GPIO 18, UART2 RX | Receives data from stick-on module |
| GV2 UART TX | GPIO 17, UART2 TX | Sends ACK lines back |
| GV2 UART baud | 921600 | 4096-byte RX/TX buffers |
| Modem RX | GPIO 4, Serial1 RX | SIM7080 AT interface |
| Modem TX | GPIO 5, Serial1 TX | SIM7080 AT interface |
| Modem PWRKEY | GPIO 41 | Pulsed if AT does not respond |
| PMU SDA | GPIO 15 | AXP2101 I2C |
| PMU SCL | GPIO 7 | AXP2101 I2C |
| SD CMD | GPIO 39 | SD-MMC 1-bit mode |
| SD CLK | GPIO 38 | SD-MMC 1-bit mode |
| SD DATA | GPIO 40 | SD-MMC 1-bit mode |

## Boot Flow

On startup, `setup()` performs:

1. Starts USB serial and prints system information.
2. Enables modem rails through the AXP2101 PMU.
3. Probes the SIM7080 with AT commands.
4. Enables modem network time commands and waits for registration.
5. Reads `AT+CCLK?` into `YYYYMMDD_HHMMSS` format and sets system time if valid.
6. Powers GNSS with `AT+CGNSPWR=1` and samples `AT+CGNSINF` for up to 10 seconds.
7. Initializes UART2 for the GV2 link.
8. Initializes SD-MMC with custom T-SIM7080G-S3 pins.
9. Writes `/post.log` to the SD card when the card is available.
10. Prints a POST summary and enters receive mode.

Every 5 seconds the loop prints a heartbeat with frame state, byte/line counters, heap, modem, time, GNSS, UART, and SD status.

## UART Protocol

The receiver expects newline-delimited ASCII control lines and a fixed-length Base64 image payload.

```text
JSON {"frame":123,"label":"hornet","score":0.94}
IMAGE <base64_length> <crc32_hex>
<base64 JPEG payload, exactly base64_length bytes>
END
```

The receiver replies only after a valid frame reaches `END`:

```text
ACK <frame_id>
```

The `frame_id` is extracted with a simple search for the `"frame":` field in the JSON text. The JSON is currently stored and printed as text; it is not parsed with ArduinoJson.

## Receive State Machine

| State | Responsibility |
| --- | --- |
| `WAIT_JSON` | Wait for a line beginning with `JSON ` |
| `WAIT_IMAGE_HEADER` | Read `IMAGE <length> <crc>` |
| `READ_IMAGE` | Accumulate exactly the declared Base64 payload length |
| `WAIT_END` | Wait for `END`, validate, save, ACK, then reset |

A new `JSON ` line globally resynchronizes the receiver and starts a fresh frame.

## Validation

Before saving an image, the firmware checks:

- CRC32 over the Base64 payload using `esp_crc32_le`.
- Base64 decode using mbedTLS.
- JPEG markers: SOI `FFD8`, SOS `FFDA`, and EOI `FFD9`.
- SD card availability.

Invalid frames are discarded and do not receive an ACK.

## SD Card Output

The firmware writes:

```text
/post.log
/frame_000123.jpg
```

`/post.log` is overwritten at boot. JPEG filenames use the frame number from the JSON metadata.

## Build And Flash

```powershell
cd firmware
.\run.ps1
```

`run.ps1` uploads to COM5 by default, then opens the serial monitor on COM5 at 115200 baud unless different ports are passed:

```powershell
.\run.ps1 -UploadPort COM5 -MonitorPort COM5
```

## Source Files

| File | Responsibility |
| --- | --- |
| `src/main.cpp` | POST reporting, UART2 setup, receive state machine, CRC/Base64/JPEG validation, ACKs, heartbeat |
| `src/modem.cpp` | AXP2101 rails, SIM7080 AT readiness, network time, GNSS probe |
| `src/modem.h` | Modem API and GNSS data structure |
| `src/sdcard.cpp` | SD-MMC custom pin setup, `/post.log`, JPEG writing |
| `src/sdcard.h` | SD card API |
| `platformio.ini` | ESP32-S3 build, serial ports, PSRAM, 16 MB flash, dependencies |
| `huge_app.csv` | Partition table |

## Dependencies

- Arduino ESP32 core
- TinyGSM for SIM7080 AT support
- XPowersLib for AXP2101 PMU control
- SD_MMC from the ESP32 Arduino core
- mbedTLS Base64
- ESP-IDF UART and CRC helpers through the Arduino build

`ArduinoJson` is listed in `platformio.ini` but is not currently used by `src/`.
