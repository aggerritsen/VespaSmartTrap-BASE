# VST-BASE

VST-BASE is the base-unit firmware, hardware integration reference, model tooling, and validation workspace for the Vespa Smart Trap project. The base unit is built around a LilyGO T-SIM7080G-S3 and receives inference metadata and JPEG frames from a Grove Vision AI V2 stick-on module through a custom PCB.

The root project is licensed under the GNU General Public License version 3.0. Keep source, build scripts, technical notes, and hardware references auditable and reproducible so that other developers can inspect, modify, rebuild, and redistribute the work under the repository's GNU license terms.

## Functionality

The current base firmware provides the following operational features:

- Power-on self test (POST) covering CPU, flash, heap, PSRAM, reset reason, SD card, configuration, modem, GNSS command path, UART, power telemetry, and actuator readiness.
- SD-MMC storage for configuration, boot reports, frame logs, power logs, and selected JPEG captures.
- SIM7080 modem initialization using TinyGSM, including AT readiness checks and network-time acquisition.
- GNSS probing through the SIM7080 command interface, with optional GNSS UTC fallback when network time is unavailable and a trusted position is present.
- Binary UART receiver for the Grove Vision AI V2 module using `VSTS` state frames and `VSTJ` JPEG frames with metadata and CRC32 validation.
- Inference filtering by class, confidence threshold, and consecutive occurrence count before actuation and image persistence.
- TB6612FNG stepper output for a configurable actuator cycle, including a boot-time POST cycle and a detection-triggered cycle.
- WiFi web view in access-point or station mode, serving the latest verified frame and inference metadata over HTTP.
- JSON Lines frame logging with timestamp, GNSS data, inference result, bounding box, CRC status, actuator result, saved filename, and firmware identity.
- Power telemetry logging to `/power.log` at a configurable interval.
- Helper scripts for building and flashing the Grove Vision AI V2 firmware image and model.
- Desktop image-viewer tooling and sample images for visual validation work.

## System Architecture

```text
T-SIM7080G-S3 base unit
        |
        +-- ESP32-S3 receiver firmware
        +-- SIM7080 modem time and GNSS probe
        +-- SD-MMC configuration, logs, and JPEG storage
        +-- WiFi HTTP image and state view
        +-- TB6612FNG stepper actuator output
        +-- USB serial POST and heartbeat monitor
        |
        +-- custom PCB interconnect
              |
              +-- Grove Vision AI V2 stick-on module
                    +-- camera
                    +-- YOLO11 object-detection model
                    +-- UART inference metadata and JPEG output
```

The T-SIM7080G-S3 is the base controller. The Grove Vision AI V2 is treated as an attached vision module, not as the system broker. The custom PCB provides the electrical and mechanical interface between the base, vision module, and actuator hardware references.

## Technical Specifications

| Area | Specification |
| --- | --- |
| Base board | LilyGO T-SIM7080G-S3, built as `esp32-s3-devkitc-1` |
| Framework | PlatformIO, Arduino ESP32 core |
| Firmware version | `0.2.0` in `firmware/src/version.h` |
| Flash | 16 MB, `huge_app.csv` partition table |
| PSRAM | Enabled, OPI/QIO memory configuration |
| USB serial | COM5 default, 115200 baud monitor |
| Upload | COM5 default, 921600 baud upload |
| GV2 UART | Serial2, GPIO 16 RX, GPIO 17 TX, 921600 baud default |
| GV2 protocol | `VSTS` state frames, `VSTJ` JPEG frames, CRC32 over JPEG payload |
| Modem UART | Serial1, GPIO 4 RX, GPIO 5 TX |
| Modem power key | GPIO 41 |
| PMU | AXP2101 over I2C, SDA GPIO 15, SCL GPIO 7 |
| SD-MMC | 1-bit mode, CMD GPIO 39, CLK GPIO 38, DATA GPIO 40 |
| Actuator driver | TB6612FNG stepper output |
| Stepper pins | PWMA GPIO 9, AIN2 GPIO 10, AIN1 GPIO 11, BIN2 GPIO 12, BIN1 GPIO 13, PWMB GPIO 14 |
| Status output | Right LED / actuator-active signal on GPIO 3 / D0 |
| Web service | HTTP port 80, endpoints `/`, `/state.json`, `/frame.jpg` |
| Model target | Grove Vision AI V2 |
| Current model path | `external/gv2-firmware/model_zoo/tflm_yolo11_od/` |
| Current model file | `yolo11n_vespa_2026-02v1_allpxNULL_full_integer_quant_vela.tflite` |
| Model flash offset | `0xB7B000` |
| Model classes | `Apis mellifera`, `Vespa crabro`, `Vespa velutina` |

## Repository Layout

```text
firmware/                     PlatformIO firmware for the T-SIM7080G-S3 base
firmware/src/                 Receiver firmware modules
firmware/platformio.ini       ESP32-S3 build, upload, monitor, and library config
firmware/huge_app.csv         16 MB flash partition table
docs/                         Architecture, protocol, hardware, GNSS, SD, and bring-up notes
docs/hardware/                Datasheets, pinouts, photos, and hardware references
external/gv2-firmware/        Submodule: Grove Vision AI V2 firmware fork and model zoo
external/t-sim-motor-shield/  Submodule: custom PCB / motor shield reference
tools/image-viewer/           Desktop random image viewer and sample images
tools/receiver/               Receiver helper scripts
tools/Himax_AI_web_toolkit/   Local copy of the Himax web flashing toolkit
build_gv2_image.ps1           GV2 firmware image build helper
flash_gv2.ps1                 GV2 firmware and model flash helper
```

Submodules are intentionally kept under `external/` to make ownership and licensing boundaries clear.

## Firmware Boot Flow

At startup the receiver:

1. Starts USB serial and prints system information.
2. Initializes the SD card, creates `/config.json` when missing, and loads runtime configuration.
3. Enables modem rails and probes SIM7080 AT readiness.
4. Attempts to obtain network time and set system time.
5. Powers and probes GNSS, optionally using trusted GNSS UTC as a fallback time source.
6. Starts the WiFi web service when enabled.
7. Initializes power telemetry, GV2 UART, and the stepper actuator.
8. Writes `/post.log` to SD when available.
9. Prints the POST summary to serial and runs the configured actuator POST cycle.
10. Enters receive mode and prints a heartbeat every 5 seconds.

## UART Protocol

The base expects binary frames from the current GV2 firmware:

```text
State frame:
VSTS + state_u8

JPEG frame:
VSTJ + state_u8 + class_idx_u8 + conf_u8
     + bbox_x_u16_le + bbox_y_u16_le + bbox_w_u16_le + bbox_h_u16_le
     + jpeg_len_u32_le + crc32_u32_le + jpeg_bytes
```

The JPEG payload length is the trimmed JPEG length through the actual `FFD9` marker. CRC32 is calculated over exactly those JPEG bytes. The receiver only treats a frame as complete after the declared payload has arrived, CRC32 matches, and the JPEG structure is valid.

## Detection And Actuation

The receiver evaluates each valid JPEG frame against the configured inference filter:

- `confidence_threshold`: required confidence as `0.0` to `1.0`.
- `detected_class`: target class index, or `-1` to accept any class.
- `occurrence`: number of consecutive matching frames required.

When the occurrence threshold is reached, the firmware runs the configured stepper actuator cycle, flushes and resynchronizes the UART receive path after the blocking actuator movement, and then saves the triggering JPEG when the SD card is available. The occurrence counter resets after a completed actuator event.

## SD Card Files

The firmware creates or writes these files on the SD card:

```text
/config.json                  Runtime configuration, created if missing
/post.log                     Boot POST summary, overwritten each boot
/frames.log                   JSON Lines frame and detection log
/power.log                    JSON Lines power telemetry log
/YYYYMMDD_HHMMSS_000123.jpg   Saved detection JPEGs when time is known
/uptime_0000000000_000123.jpg Fallback JPEG naming when time is unavailable
```

`/frames.log` includes device identity, firmware version, GNSS fields, inference state, bounding box, filter result, CRC result, actuator result, and saved filename.

## Configuration

Configuration is read from `/config.json` on the SD card. If it does not exist, the firmware writes a default file. Key fields include:

```json
{
  "schema_version": 1,
  "device_name": "vst-base-001",
  "uart": {
    "baud": 921600
  },
  "time": {
    "network_timeout_seconds": 10,
    "allow_gnss_fallback": true
  },
  "stepper": {
    "speed_steps_per_second": 400,
    "rotation_degrees": 90,
    "steps_per_revolution": 2048,
    "reverse_wait_ms": 1000,
    "start_direction": "ccw"
  },
  "inference": {
    "confidence_threshold": 0.89,
    "detected_class": 3,
    "occurrence": 3
  },
  "power": {
    "log_interval_seconds": 60
  },
  "web": {
    "mode": 2,
    "ssid": "VST-BASE",
    "password": "",
    "append_mac": true
  }
}
```

`web.mode` values are `0` for disabled, `1` for WiFi station mode, and `2` for access-point mode. `stepper.start_direction` accepts common clockwise and counter-clockwise forms such as `cw`, `clockwise`, `ccw`, and `anti-clockwise`.

## Build And Flash The Base Firmware

Install PlatformIO, connect the T-SIM7080G-S3, then run:

```powershell
cd firmware
.\run.ps1
```

The helper uploads to COM5 and opens the serial monitor on COM5 at 115200 baud by default. Override the ports when needed:

```powershell
.\run.ps1 -UploadPort COM5 -MonitorPort COM5
```

The underlying PlatformIO environment is `t-sim7080g-s3`.

## Build And Flash The GV2 Firmware

Initialize submodules before building GV2 assets:

```powershell
git submodule update --init --recursive
```

Build the GV2 firmware image:

```powershell
.\build_gv2_image.ps1
```

Flash the GV2 firmware image and model:

```powershell
.\flash_gv2.ps1 -Port COM7
```

The flash helper uses `external/gv2-firmware/xmodem/xmodem_send.py` and expects the generated image at:

```text
external/gv2-firmware/we2_image_gen_local/output_case1_sec_wlcsp/output.img
```

Large model binaries should remain in the GV2 firmware submodule unless they are intentionally copied into this repository for a release package.

## Dependencies

Base firmware dependencies are declared in `firmware/platformio.ini`:

- Arduino ESP32 core through PlatformIO `espressif32`.
- TinyGSM from `https://github.com/vshymanskyy/TinyGSM.git`.
- XPowersLib by `lewisxhe` for AXP2101 PMU support.
- ArduinoJson `^7.0.0`.
- ESP32 Arduino built-ins such as `SD_MMC`, `WiFi`, and `WebServer`.

GV2 firmware dependencies are managed inside the `external/gv2-firmware` submodule.

## External Repositories

```text
external/gv2-firmware
  url:    https://github.com/marcory-hub/Seeed_Grove_Vision_AI_Module_V2.git
  branch: yolo11-vespa

external/t-sim-motor-shield
  url:    https://github.com/aggerritsen/T-SIMMotorShield.git
  branch: master
```

Clone with submodules:

```powershell
git clone --recurse-submodules <repo-url>
```

Or initialize them after cloning:

```powershell
git submodule update --init --recursive
```

## Public Development And GNU Licensing

This repository is licensed under the GNU General Public License version 3.0. See `LICENSE` for the full license text. For public releases, keep these practices in place:

- Preserve license notices in source files and third-party materials.
- Keep submodules and bundled tools in clearly separated directories so their upstream licenses remain identifiable.
- Document generated firmware, model binaries, and hardware artifacts well enough that users can rebuild or replace them.
- Avoid committing private credentials, SIM/APN secrets, WiFi passwords, certificates, or personal device identifiers.
- Prefer reproducible build commands and versioned configuration over local-only IDE state.

Third-party submodules and bundled tools may have their own licenses. Review those license files before redistributing combined firmware images, model packages, or hardware packages.

## Documentation Index

- `firmware/README.md`: detailed receiver firmware behavior, pins, protocol, validation, and source-file map.
- `docs/architecture.md`: high-level hardware and firmware architecture.
- `docs/receiver-post.md`: POST behavior.
- `docs/sd-card-layout.md`: SD output notes.
- `docs/gv2-model-flashing.md`: GV2 firmware and model flashing notes.
- `docs/custom-pcb-bringup.md`: custom PCB bring-up guidance.
- `docs/gnss.md`: GNSS notes.
- `docs/hardware/`: pinouts, datasheets, board photos, and hardware references.

## Development Notes

- Keep base application changes in `firmware/`.
- Keep GV2 firmware and model work in `external/gv2-firmware/`.
- Keep custom PCB and motor shield reference work in `external/t-sim-motor-shield/`.
- Update this README and the relevant detailed docs when changing public behavior, binary protocols, default pins, SD file formats, model names, or license posture.
