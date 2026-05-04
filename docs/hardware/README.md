## 📝 README: AXP2101 PMIC Troubleshooting & Final Required Settings (ESP-IDF)

This document summarizes the final, working configuration settings and critical code adjustments needed to reliably initialize the **AXP2101 Power Management IC (PMIC)** on the **LILYGO T-SIM7080G-S3** board, enabling both the SD card and the Camera.

The minimal working code successfully loads the model from the SD card, unmounts it, and then initializes the camera for capture.

---

## 📌 1. Required ESP32-S3 GPIO Pin Definitions

The following GPIOs are required for I2C communication with the AXP2101 PMIC and the standard 1-bit SDMMC interface.

| Peripheral | Function | GPIO Pin | Constant | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **PMIC (AXP2101)** | I2C Data (SDA) | **GPIO15** | `I2C_MASTER_SDA_IO` | Used for configuring AXP2101. |
| | I2C Clock (SCL) | **GPIO7** | `I2C_MASTER_SCL_IO` | Used for configuring AXP2101. |
| **SD Card** | Command (CMD) | **GPIO39** | `SDMMC_CMD_GPIO` | **Shared with Camera D5.** Requires unmount/reset before camera init. |
| | Clock (CLK) | **GPIO38** | `SDMMC_CLK_GPIO` | |
| | Data 0 (D0) | **GPIO40** | `SDMMC_D0_GPIO` | |

---

## ⚡ 2. AXP2101 PMIC Rail Settings (AXP2101 Register Map)

The following table details the necessary voltage and control register values programmed into the AXP2101 to support the ESP32-S3, SD card, and OV Camera.

| Rail | Purpose | Voltage Setting | AXP2101 Register | Value Written | Register Address (Hex) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **DCDC1** | **System/VBUS ($3.3\text{V}$)** | **$3.30\text{V}$** | `DCDC1_VSET` | `0x39` | `0x80` |
| **DCDC3** | **SD Card VCC** | **$3.40\text{V}$** | `DCDC3_VSET` | `0x2A` | `0x82` |
| **ALDO1** | **Camera Core** | **$1.80\text{V}$** | `ALDO1_VSET` | `0x0D` | `0x92` |
| **ALDO2** | **Camera I/O** | **$2.80\text{V}$** | `ALDO2_VSET` | `0x17` | `0x93` |
| **ALDO4** | **Camera Analog** | **$3.00\text{V}$** | `ALDO4_VSET` | `0x19` | `0x95` |
| **Enable All** | Global Control | N/A | `LDO_EN_CTRL` | **`0xFF`** | `0x90` |
| **DCDC Enable** | DCDC Control | N/A | `DCDC_EN_CTRL` | **`0x07`** (DCDC1,2,3 ON) | `0x21` |



### Key Voltage Differences (The Fixes)

* **SD Card (DCDC3):** Explicitly set to **$3.4\text{V}$** (`0x2A`) to ensure the SD card receives stable operating voltage, preventing the previous issue where it ran at the default, non-functional $0.9\text{V}$.
* **Camera Rails (ALDOs):** Explicitly set to $1.8\text{V}$, $2.8\text{V}$, and $3.0\text{V}$ to provide the exact voltage levels required by the OV camera sensor (e.g., OV2640).

---

## ⏱️ 3. Required Timing and Delay Settings

Proper timing is crucial for I2C stability and power rail stabilization.

| Action | Delay Value (ms) | Code Context | Rationale |
| :--- | :--- | :--- | :--- |
| **Post-Register Write** | **$5\text{ms}$** | `vTaskDelay(pdMS_TO_TICKS(5));` inside `axp2101_write_reg()` | Prevents I2C transaction errors and gives the PMIC internal logic time to execute the command. |
| **Post-Rail Setup** | **$100\text{ms}$** | `vTaskDelay(pdMS_TO_TICKS(100));` | Allows the DCDC3/SD card power rail to fully stabilize before SDMMC initialization begins. |
| **After SD Unmount** | **$100\text{ms}$** | `vTaskDelay(pdMS_TO_TICKS(100));` | Allows the system to release the SDMMC peripherals, including the shared GPIO39, before camera initialization. |

---

## 🧠 4. Summary of Lessons Learned

| Key Lesson | Description |
| :--- | :--- |
| **Register Map:** | Must use the **correct AXP2101 datasheet register addresses** (e.g., DCDC1 voltage is `0x80`), as they differ from derived or older PMIC libraries. |
| **I2C Stability:** | An aggressive **$5\text{ms}$ delay after every I2C write** is mandatory for stability on this hardware platform. |
| **Explicit Enable:** | Voltage registers must be written **first**, followed by explicit writes to the LDO Enable (`0x90`) and DCDC Enable (`0x21`) registers to bring the rails online. |
| **Hardware Conflict:** | The program successfully loads the model from the SD card and then uses the camera. If GPIO39 is shared (CMD/D5), the **SD card must be explicitly unmounted and its GPIO reset** before the camera is initialized. |