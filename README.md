# VST-BASE

Baseline firmware, hardware references, model tooling, and validation utilities for the Vespa Smart Trap direct-connect architecture.

## Architecture

```text
Grove Vision AI V2 + YOLO11 model
        |
        | direct UART / custom PCB interconnect
        v
T-SIM7080G-S3 receiver
        |
        +-- POST diagnostics
        +-- SD image/log storage
        +-- SIM7080 network time
        +-- SIM7080 GNSS
        +-- future uplink / actuator logic
```

There is no XIAO broker in this base. Grove Vision AI V2 connects directly to the T-SIM7080G-S3 through the custom T-SIM Motor Shield PCB.

## Layout

```text
firmware/t-sim7080g-s3-base/  Current PlatformIO receiver firmware
external/gv2-firmware/        Submodule: Grove Vision AI V2 firmware fork
external/t-sim-motor-shield/  Submodule: custom PCB and motor shield reference
models/gv2-yolo11/            Model manifest and notes
tools/gv2/                    GV2 build/flash helper scripts
tools/image-viewer/           Desktop random image viewer
tools/receiver/               Receiver helper scripts
docs/                         Architecture, hardware, protocol and bring-up docs
```

## First Setup

After this folder is created, run:

```powershell
.\create_github_repo.ps1
```

To also create the GitHub repository with GitHub CLI:

```powershell
.\create_github_repo.ps1 -Owner <github-owner> -CreateRemote -Visibility private
```

## Build Receiver

```powershell
cd firmware\t-sim7080g-s3-base
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
