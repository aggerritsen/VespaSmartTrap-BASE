# Direct GV2 To T-SIM

This project no longer uses a XIAO broker. The T-SIM7080G-S3 is the base unit, and Grove Vision AI V2 attaches as a stick-on module through the custom PCB documented in:

```text
external/t-sim-motor-shield/
```

The firmware-side receive path belongs in:

```text
firmware/src/
```

## UART Framing

GV2 sends framed messages over UART1 to the T-SIM receiver:

```text
VSTS + state
VSTJ + inference metadata + jpeg length + crc32 + jpeg payload
VSTE + error_code + detail + counter_u32_le
```

Error frames currently use:

```text
code=3 detail=1  camera missing / sensor init failed
code=3 detail=2  camera datapath init failed
```

The T-SIM logs these as `GV2: error ...` on serial and appends a `gv2_error` JSON event to `/frames.log`.
