# ESP32-S3 GV2 UART Receiver

This firmware is the working reference implementation for receiving the GV2 UART output on a Seeed XIAO ESP32-S3 and forwarding status/JPEG information to the USB serial monitor.

## Hardware Interface

- Board: Seeed XIAO ESP32-S3
- Framework: Arduino via PlatformIO
- GV2 power: GV2 remains powered over USB-C
- ESP32-S3 USB: used for flashing, serial monitor, and optional JPEG byte capture
- UART peripheral: `Serial1`
- UART baud: `921600`
- UART format: `SERIAL_8N1`

Pin mapping:

| ESP32-S3 pin | UART role | Connects to |
| --- | --- | --- |
| GPIO44 | `Serial1` RX | GV2 TX |
| GPIO43 | `Serial1` TX | GV2 RX |

USB serial monitor settings:

- Baud: `921600`
- PlatformIO monitor filter used by helper script: `printable`

## PlatformIO Environment

The working environment is:

```ini
[env:esp32s3-gv2-uart]
platform = espressif32@6.3.0
board = seeed_xiao_esp32s3
framework = arduino

monitor_speed = 921600
upload_speed = 921600
```

## GV2 UART Protocol

The receiver scans the incoming UART byte stream for two framed message types. Both use a 4-byte ASCII magic prefix.

### State Frame

Magic:

```text
V S T S
```

Byte layout:

| Offset | Size | Field | Type | Description |
| --- | ---: | --- | --- | --- |
| 0 | 4 | magic | bytes | ASCII `VSTS` |
| 4 | 1 | state | `uint8_t` | GV2 state byte |

The current receiver consumes this frame and discards the state byte. It is informational only in this firmware because JPEG frames also contain a state byte.

### JPEG Frame

Magic:

```text
V S T J
```

Byte layout:

| Offset | Size | Field | Type | Description |
| --- | ---: | --- | --- | --- |
| 0 | 4 | magic | bytes | ASCII `VSTJ` |
| 4 | 1 | state | `uint8_t` | GV2 state byte |
| 5 | 1 | class_idx | `uint8_t` | Detection class index |
| 6 | 1 | conf | `uint8_t` | Confidence, interpreted as `conf / 255.0` |
| 7 | 4 | bbox_x | `uint32_t`, little-endian | Bounding box X |
| 11 | 4 | bbox_y | `uint32_t`, little-endian | Bounding box Y |
| 15 | 4 | bbox_w | `uint32_t`, little-endian | Bounding box width |
| 19 | 4 | bbox_h | `uint32_t`, little-endian | Bounding box height |
| 23 | 4 | len | `uint32_t`, little-endian | JPEG payload length in bytes |
| 27 | `len` | jpeg_payload | bytes | Raw JPEG payload |

JPEG payload length validation:

- `len == 0` is invalid
- `len > 524288` is invalid
- Invalid frames are reported on USB serial as `[jpeg] invalid len=<len>`

## Receiver Behavior

At startup, the ESP32-S3:

1. Starts USB serial at `921600`.
2. Prints initialization messages.
3. Starts `Serial1` at `921600` using RX `GPIO44` and TX `GPIO43`.
4. Enters a loop that processes USB commands and consumes GV2 UART bytes.

The UART parser continuously scans for magic sequences. It is stream-based and does not require frame alignment at the start of reads.

When a valid JPEG frame header is received, the receiver prints one metadata line over USB serial:

```text
recv #<n> len=<bytes> state=<state> class=<class_idx> conf_u8=<conf> conf=<conf_float> bbox=[<x>,<y>,<w>,<h>] jpeg_raw=<on|off>
```

Example field meanings:

- `recv #<n>` increments for each valid JPEG frame header
- `len` is the raw JPEG payload length
- `conf_u8` is the original `uint8_t` confidence
- `conf` is `conf_u8 / 255.0`, printed with 3 decimals
- `jpeg_raw` reports whether this frame's JPEG bytes will be forwarded over USB

JPEG payload bytes are always consumed from UART. They are forwarded to USB serial only when raw forwarding is enabled before that frame starts.

## USB Serial Commands

Commands are line-based. Send them over the USB serial monitor followed by newline.

| Command | Effect |
| --- | --- |
| `jpeg on` | Enable raw JPEG byte forwarding over USB |
| `raw on` | Same as `jpeg on` |
| `jpeg off` | Disable raw JPEG byte forwarding over USB |
| `raw off` | Same as `jpeg off` |

Responses:

```text
[cfg] jpeg_raw=on
[cfg] jpeg_raw=off
```

Raw forwarding is sampled at JPEG frame start. Changing the setting while a JPEG payload is already being consumed does not affect that in-progress frame.

## Build, Flash, Monitor

The helper script flashes and starts the serial monitor on a selected COM port:

```powershell
.\flash-monitor.ps1 -Port COM7
```

The script defaults to `COM3`:

```powershell
.\flash-monitor.ps1
```

Its functional command flow is:

```powershell
pio run -t upload --upload-port $Port
if ($LASTEXITCODE -eq 0) {
    pio device monitor -p $Port -b 921600 --filter printable
}
```

## Porting Notes

To port this UART interface into another development, preserve these functional requirements:

- Receive GV2 data at `921600` baud, `8N1`.
- Search the stream for `VSTS` and `VSTJ` magic prefixes.
- Decode all multi-byte integers as unsigned 32-bit little-endian values.
- Treat the JPEG header length after `VSTJ` as exactly 23 bytes.
- Treat the full JPEG frame length as `27 + len` bytes including magic.
- Reject JPEG payload lengths of `0` or greater than `512 KiB`.
- Consume every JPEG payload completely, even when not forwarding it.
- Only forward raw JPEG bytes after an explicit `jpeg on` or `raw on` command.
- Print one metadata line per valid JPEG frame header before consuming/forwarding its payload.
