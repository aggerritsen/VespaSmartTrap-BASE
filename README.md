# VST-BASE

T-SIM7080G-S3 base firmware, GV2 stick-on module integration, custom PCB references, model tooling, and validation utilities for the Vespa Smart Trap base unit.

## Architecture

```text
T-SIM7080G-S3 base unit
        |
        +-- receiver firmware
        +-- POST diagnostics
        +-- SD image/log storage
        +-- SIM7080 network time
        +-- SIM7080 GNSS
        +-- future uplink / actuator logic
        |
        +-- custom PCB interconnect
              |
              +-- Grove Vision AI V2 stick-on module
                    +-- camera
                    +-- YOLO11 model
                    +-- inference metadata / image output
```

The T-SIM7080G-S3 is the base component. Grove Vision AI V2 is a stick-on vision module attached through the custom PCB. There is no XIAO broker in this base.

## Layout

```text
firmware/                     PlatformIO firmware for the T-SIM7080G-S3 base
external/gv2-firmware/        Submodule: Grove Vision AI V2 stick-on module firmware fork
external/t-sim-motor-shield/  Submodule: custom PCB / motor shield reference
tools/image-viewer/           Desktop random image viewer and sample images
tools/receiver/               Receiver helper scripts
build_gv2_image.ps1           GV2 stick-on module image build helper
flash_gv2.ps1                 GV2 stick-on module flash helper
docs/                         Architecture, hardware, protocol and bring-up docs
```

## GV2 YOLO11 Model

The Grove Vision AI V2 model binaries live with the GV2 stick-on module firmware submodule:

```text
external/gv2-firmware/model_zoo/tflm_yolo11_od/
```

Current VST-BASE default:

```text
Target:        Grove Vision AI V2
Model:         yolo11n_vespa_2026-02v1_allpxNULL_full_integer_quant_vela.tflite
Flash offset:  0xB7B000
Classes:       Apis mellifera, Vespa crabro, Vespa velutina
```

Large model binaries should stay in the GV2 firmware submodule unless explicitly copied here for release packaging.

## Build Receiver

```powershell
cd firmware
.\run.ps1
```

## External Dependencies

Submodules are intentionally kept under `external/`:

- `external/gv2-firmware`: `https://github.com/marcory-hub/Seeed_Grove_Vision_AI_Module_V2.git`, branch `yolo11-vespa`
- `external/t-sim-motor-shield`: `https://github.com/aggerritsen/T-SIMMotorShield.git`, branch `master`

Clone with:

```powershell
git clone --recurse-submodules <repo-url>
```

or after cloning:

```powershell
git submodule update --init --recursive
```
