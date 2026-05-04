# Modem Power-Up, Initialization & Timestamp Collection

This document describes, step by step, how the cellular modem is powered, initialized, brought online, and used to obtain a reliable timestamp in this project.  
It is intended as a concise technical reference for maintainers and reviewers.

---

## 1. Modem Power-Up (PMIC stage)

The modem is powered **before any UART or AT communication** via the **AXP2101 PMIC**.

Key points:

- Power rails for the modem and GPS are enabled by the PMIC:
  - `DCDC3` → Modem / GNSS (3.0 V)
  - `BLDO2` → Modem I/O (3.3 V)
- All voltages are explicitly verified by reading back PMIC registers.
- Power-good is confirmed *before* continuing.

This ensures:
- The modem never sees UART traffic while unpowered
- Brown-out and partial boot states are avoided

---

## 2. UART Initialization (`modem_init_uart()`)

Once power is stable, the modem UART is initialized.

Configuration:
- UART: `UART_NUM_1`
- Baud rate: `115200`
- Data format: `8N1`
- Flow control: disabled
- RX/TX pins: fixed board wiring

The UART driver is installed with sufficiently large RX/TX buffers to handle bursty AT responses.

No AT commands are sent at this stage.

---

## 3. Modem Readiness Check (`wait_for_modem()`)

The system waits until the modem is responsive at the AT level.

Process:
1. Periodically send `AT`
2. Wait for a clean `OK` response
3. Retry until timeout (30 s)

Once the modem responds:
- Network time updates are enabled:
  - `AT+CLTS=1` (network time sync)
  - `AT+CTZR=1` (timezone reporting)

This confirms:
- The modem firmware is running
- UART communication is reliable

---

## 4. Network Registration Wait

After AT readiness, the modem is **not yet trusted for time**.

The system explicitly waits for cellular registration:

- `AT+CEREG?` (LTE registration)
- `AT+CREG?`  (CS fallback)

Accepted states:
- `,1` → Registered (home)
- `,5` → Registered (roaming)

This step is critical because:
- Network time (NITZ) is only provided after registration
- The modem RTC often contains invalid default dates at boot

---

## 5. Timestamp Collection (`modem_get_timestamp()`)

The modem clock is queried using:

```text
AT+CCLK?
