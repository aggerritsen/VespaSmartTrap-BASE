## Camera + SD Card Initialization (ESP32-S3 / LILYGO T-SIM7080G-S3)

### Camera (OV2640) – Critical Settings
- **Peripheral**: I2S_CAM (independent from SDMMC)
- **Pixel format**: JPEG  
- **Frame size**: QVGA (320×240)  
- **JPEG quality**: 10  
- **Frame buffers**: 1 (prevents DMA/PSRAM pressure)
- **Frame buffer location**: PSRAM  
- **XCLK**: 20 MHz  
- **Grab mode**: `CAMERA_GRAB_WHEN_EMPTY`
- **I2C (SCCB)**: Port 1
- **Reset sequence**:
  - Hardware reset via RESET pin
  - Software reset / wakeup (OV2640 DSP PWDN toggle)
- **Power rails (AXP2101)**:
  - ALDO1: 1.8 V (core)
  - ALDO2: 2.8 V (I/O)
  - ALDO4: 3.0 V (analog)

### SD Card – Critical Settings
- **Interface**: SDMMC (native, not SPI)
- **Bus width**: 1-bit (maximum pin compatibility)
- **Pins**:
  - CMD → GPIO39
  - CLK → GPIO38
  - D0  → GPIO40
- **Slot flags**: `SDMMC_SLOT_FLAG_INTERNAL_PULLUP`
- **Host flags**: `SDMMC_HOST_FLAG_1BIT` (OR’ed with defaults)
- **Mount point**: `/sdcard`
- **Filesystem**: FATFS
- **Long File Names (LFN)**: Enabled (heap-based, max 255)
- **External RAM**: Preferred for FATFS buffers

### Concurrency Model
- **Camera capture**: Dedicated FreeRTOS task
- **SD writes**: Separate FreeRTOS task
- **Buffer handoff**: Queue of `camera_fb_t*`
- **DMA isolation**:
  - Camera → I2S_CAM → PSRAM
  - SD → SDMMC → separate DMA engine

### Key Design Constraints (Why This Works)
- No GPIO overlap between camera and SD
- ESP32-S3 allows GPIO39 as SD CMD (not input-only)
- SDMMC + Camera use independent peripherals
- PSRAM prevents internal RAM exhaustion
- 1-bit SD avoids pin and timing conflicts
