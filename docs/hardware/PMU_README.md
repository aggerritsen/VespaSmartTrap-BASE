# ⚡ AXP2101 PMIC Configuration for LILYGO_ESP32S3_CAM_SIM7080G (Updated)

This document details the critical register settings, voltage configurations, and sequencing logic for the AXP2101 Power Management IC (PMIC) used on the LilyGO T-Camera S3/SIM7080G platform.

The primary goal is to provide the correct power rails for the ESP32-S3, the OV2640 camera module, and the SIM7080G cellular/GPS modem.

## 1. ⚙️ Hardware & I2C Interface

| Parameter | Value | Notes |
| :--- | :--- | :--- |
| **PMIC Chip** | AXP2101 (X-Powers) | Used for multi-rail power management. |
| **I2C Address** | `0x34` (7-bit) | Standard slave address for AXP2101. |
| **I2C Port** | `I2C_NUM_0` | ESP32-S3 I2C peripheral port. |
| **SDA Pin** | `GPIO_15` | I2C Data line. |
| **SCL Pin** | `GPIO_7` | I2C Clock line. |
| **Clock Freq** | `400 kHz` | Standard Fast-mode I2C frequency. |
| **Timing** | `vTaskDelay(pdMS_TO_TICKS(5));` | Mandatory 5ms delay after I2C write for PMIC stability. |
| **Stabilization** | `vTaskDelay(pdMS_TO_TICKS(100));` | 100ms delay after all rails enabled for system stability. |

## 2. ⚡ Power Rail Requirements & Settings

This table maps the required voltages to their corresponding AXP2101 register addresses and the final hexadecimal write value (`VSET`). **These settings are validated by the diagnostic log.**

| Rail Name | Register (Addr) | Target Voltage | Type | Description / Load | **VSET Value (Hex)** | VSET (Decimal) |
| :--- | :---: | :---: | :---: | :--- | :---: | :---: |
| **DCDC1** | `0x82` | **3.30V** | DCDC | ESP32-S3 Core/Main Power | `0x12` | 18 |
| **DCDC3** | `0x84` | **3.00V** | DCDC | **SIM7080G Modem/GPS VCC** (Verified working) | `0x66` | 102 |
| **ALDO1** | `0x92` | **1.80V** | LDO | Camera (OV2640) DVDD Core | `0x0D` | 13 |
| **ALDO2** | `0x93` | **2.80V** | LDO | Camera (OV2640) I/O | `0x17` | 23 |
| **ALDO3** | `0x94` | **3.30V** | LDO | SD Card Power | `0x1C` | 28 |
| **ALDO4** | `0x95` | **3.00V** | LDO | Camera (OV2640) AVDD Analog | `0x19` | 25 |
| **BLDO1** | `0x96` | **1.80V** | LDO | Level Shifter/External IO | `0x0D` | 13 |
| **BLDO2** | `0x97` | **3.30V** | LDO | **Modem/GPS Peripherals** (Verified working) | `0x1C` | 28 |
| **DLDO1** | `0x99` | **3.30V** | LDO | General Purpose LDO 1 | `0x1C` | 28 |
| **DLDO2** | `0x9A` | **2.30V** | LDO | General Purpose LDO 2 | `0x12` | 18 |

### 3. 📐 Voltage Step Configuration

The register values are calculated based on the following step configurations:

| Rail Type | Voltage Range | Step Size (mV) | Base Voltage | Calculation Formula |
| :--- | :--- | :---: | :---: | :--- |
| **DCDC1** | 1.5V to 3.4V | 100 mV | 1.5V | $(V_{target} - 1.5\text{V}) / 0.1\text{V}$ |
| **DCDC3** | 1.6V to 3.4V | 100 mV | 1.6V | $(V_{target} - 1.6\text{V}) / 0.1\text{V} + \mathbf{0\text{x}58}$ |
| **LDOs** | 0.5V to 3.5V | 100 mV | 0.5V | $(V_{target} - 0.5\text{V}) / 0.1\text{V}$ |

### 4. 🛑 Master Enablement Registers

These registers control the on/off state of the power rails. Proper sequencing is crucial.

| Register (Addr) | Control | Required Value (Hex) | Enabled Outputs | Notes |
| :--- | :--- | :---: | :--- | :--- |
| `0x80` | **DCDC Enable/Status** | `0x05` | DCDC1, DCDC3 | **CRITICAL FIX:** Correct control register for DCDC Enable. **DCDC3 is Modem Power.** |
| `0x90` | **LDO (ALDO/BLDO) Enable** | `0xFF` | ALDO1-4, BLDO1-2 | Enables all ALDO/BLDO outputs. |
| `0x98` | **DLDO Enable** | `0x03` | DLDO1, DLDO2 | Only enables the two DLDOs used. |
| `0x01`, `0x48`| **Fault Clear** | `0xFF` | N/A | Clears latched faults before attempting to enable DCDCs. |

### 5. 🧑‍💻 Operational Sequence (`axp2101_init_pmic`)

The initialization function executes in a specific order to ensure stable power delivery and clear any potential latched fault conditions.

1.  **Fault Clearance:** Clear all general, power-off, and DCDC overcurrent faults (`0x01` and `0x48`).
2.  **Voltage Configuration:** Write the specific **VSET** values (from Section 2) to all 10 voltage registers (`0x82`, `0x84`, `0x92` through `0x9A`).
3.  **LDO Enable:** Enable all necessary LDO/ALDO/BLDO/DLDO outputs (`0x90` and `0x98`).
4.  **DCDC Enable (Critical Step):** Enable DCDC outputs by writing `0x05` to the master control register `0x80`. This activates DCDC1 (ESP) and DCDC3 (Modem).
5.  **Stabilization:** Wait 100ms for all rails to stabilize before the application proceeds.

### 6. ✅ Verification (`axp2101_verify_settings`)

The verification function reads back all configured registers to confirm the desired settings successfully stick.

| Check | Register (Addr) | Success Condition | Failure Implication |
| :--- | :---: | :--- | :--- |
| **Fault Status** | `0x48` (DCDC OC) | Read value must be `0x00`. | Indicates a hardware short or sustained over-current on a DCDC rail. |
| **DCDC Master** | `0x80` | Read value must be `0x05`. | Critical failure; DCDC3 (Modem) will be off, leading to "Modem failed to respond." |
| **LDO Master** | `0x90` | Read value must be `0xFF`. | Failure to enable ALDO/BLDO rails (e.g., Camera power). |
| **DLDO Master** | `0x98` | Read value must be `0x03`. | Failure to enable specific DLDO rails. |
| **Voltage Config** | `0x82`, `0x84`, `0x92-0x9A` | Read value must match the expected VSET (Hex) and calculated voltage (Volts). | Incorrect component voltage, leading to component malfunction. |

# Modem Initialization & Timestamp Retrieval  
**SIM7070 / SIM7080 / SIM7090 on ESP32-S3 with AXP2101 (Auto-Boot Design)**

This document serves as a **definitive reference** for how modem initialization and timestamp retrieval are implemented in this project, **why** it is done this way, and **what assumptions are intentionally made**.

It is written both for **future you** and for **future me**, so decisions, trade-offs, and SIMCom quirks are explicitly documented.

---

# Modem Initialization & Timestamp Retrieval  
**ESP32-S3 + SIM7070 / SIM7080 / SIM7090 (AXP2101 auto-boot boards)**

### Board Characteristics
- **ESP32-S3**
- **SIM7070 / SIM7080 / SIM7090 modem**
- **AXP2101 PMIC**
- Modem power rails:
  - `DCDC3` (3.0 V) → modem core
  - `BLDO2` (3.3 V) → modem I/O

### Critical Hardware Property
> **The modem auto-boots when power rails are enabled.**

This means:
- No manual `PWRKEY` pulse is required
- No GPIO-based modem power sequencing should be performed

### Flow

**PMIC powers modem → UART initialized → AT wake-up & autobaud → Enable network time sync → Modem ready → Timestamp retrieval**

| Step | AT Command      | Purpose | Expected Response | Notes |
|-----:|-----------------|---------|-------------------|-------|
| 1 | `AT` | Wake AT parser and lock autobaud | `AT` → `OK` | Echo is normal; first successful `OK` marks modem ready |
| 2 | `AT+CLTS=1` | Enable network-provided local time (NITZ) | `AT+CLTS=1` → `OK` | Persistent setting; required for `AT+CCLK?` to work |
| 3 | `AT+CTZR=1` | Enable timezone offset reporting | `AT+CTZR=1` → `OK` | Allows `+CCLK:` to include `±ZZ` timezone |
| 4 | `AT+CCLK?` | Query modem real-time clock | `+CCLK: "YY/MM/DD,HH:MM:SS±ZZ"` → `OK` | May initially return only `OK` until RTC is synced |
| 5 | *(retry)* | Allow time for network registration | `+CCLK:` appears | Retried several times with delay if time not yet available |
