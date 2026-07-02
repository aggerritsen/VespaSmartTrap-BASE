#!/usr/bin/env python3
"""Flash the Grove Vision AI V2 firmware image and model (cross-platform port of flash_gv2.ps1)."""

from __future__ import annotations

import argparse
import ast
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent
GV2_DIR = REPO_ROOT / "external" / "gv2-firmware"
SENDER = GV2_DIR / "xmodem" / "xmodem_send.py"

# USB VID/PID pairs for the Grove Vision AI V2. The GV2 exposes a WCH CH343
# USB-to-serial bridge in front of the Himax WE2 (VID:PID 1A86:55D3). VID/PID
# are model-level, so every GV2 module resolves the same way across machines.
GV2_USB_IDS = [(0x1A86, 0x55D3)]

# Excluded so the T-SIM7080G-S3 native USB (Espressif ESP32-S3) is never picked
# up here. It has its own auto-detect through PlatformIO's board hwids.
ESP32_S3_USB_IDS = [(0x303A, 0x1001)]

DEFAULT_BAUDRATE = 921600
DEFAULT_PROTOCOL = "xmodem"
DEFAULT_IMAGE = "we2_image_gen_local/output_case1_sec_wlcsp/output.img"
DEFAULT_MODEL = (
    "model_zoo/tflm_yolo11_od/"
    "yolo11n_vespa_2026-02v1_allpxNULL_full_integer_quant_vela.tflite "
    "0xB7B000 0x00000"
)


def _isatty() -> bool:
    try:
        return sys.stdout.isatty()
    except Exception:
        return False


def _color(text: str, code: str) -> str:
    return f"\033[{code}m{text}\033[0m" if _isatty() else text


def info(msg: str) -> None:
    print(_color(msg, "36"), flush=True)


def die(msg: str, code: int = 1) -> "None":
    print(_color(f"error: {msg}", "31"), file=sys.stderr, flush=True)
    sys.exit(code)


def _clean_line(line: str) -> str:
    """Rewrite ``b'...'`` bytes-repr lines emitted by xmodem_send.py into plain text.

    The upstream sender does ``print(<bytes>)`` on WE2 bootloader output, so
    every WE2 line arrives wrapped as a Python bytes literal. Lines that are
    not a bytes-repr (progress bars, our own info lines, etc.) pass through
    unchanged.
    """
    stripped = line.rstrip("\r\n")
    trailing = line[len(stripped):]
    if len(stripped) >= 3 and (
        (stripped.startswith("b'") and stripped.endswith("'"))
        or (stripped.startswith('b"') and stripped.endswith('"'))
    ):
        try:
            value = ast.literal_eval(stripped)
        except (ValueError, SyntaxError):
            return line
        if isinstance(value, (bytes, bytearray)):
            text = value.decode("latin-1", errors="replace").rstrip("\r\n")
            return text + trailing
    return line


def autodetect_gv2_port() -> "str | None":
    """Return the serial device of a connected GV2, or None if not found.

    Matches by USB VID/PID so every physical GV2 module resolves the same way,
    regardless of the OS-assigned tty suffix.
    """
    try:
        from serial.tools import list_ports  # type: ignore
    except ImportError:
        die(
            "pyserial is required for GV2 auto-detect. "
            "Install it into your active environment: 'pip install -r requirements.txt'"
        )

    matches = []
    for p in list_ports.comports():
        vid, pid = p.vid, p.pid
        if vid is None or pid is None:
            continue
        if (vid, pid) in ESP32_S3_USB_IDS:
            continue
        if (vid, pid) in GV2_USB_IDS:
            matches.append(p)

    if not matches:
        return None
    if len(matches) > 1:
        devices = ", ".join(m.device for m in matches)
        die(
            f"multiple GV2 candidates found ({devices}); "
            "pass --port explicitly to select one"
        )
    p = matches[0]
    info(
        f"GV2: auto-detected {p.device} "
        f"(VID:PID={p.vid:04X}:{p.pid:04X}, serial={p.serial_number or 'n/a'})"
    )
    return p.device


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Flash the Grove Vision AI V2 firmware image and model."
    )
    parser.add_argument(
        "--port",
        default=None,
        help="serial port of the GV2 module (default: auto-detect by USB VID/PID)",
    )
    parser.add_argument("--baudrate", type=int, default=DEFAULT_BAUDRATE)
    parser.add_argument("--protocol", default=DEFAULT_PROTOCOL, choices=["xmodem", "xmodem1k"])
    parser.add_argument(
        "--image-file",
        default=DEFAULT_IMAGE,
        help="firmware image path, relative to external/gv2-firmware",
    )
    parser.add_argument(
        "--model",
        default=DEFAULT_MODEL,
        help="model spec as '<path> <flash_offset> <load_addr>' (relative to external/gv2-firmware)",
    )
    args = parser.parse_args()

    if shutil.which("python3") is None and shutil.which("python") is None:
        die("Python was not found in PATH.")

    if not SENDER.exists():
        die(f"Missing xmodem sender: {SENDER}")

    image_path = GV2_DIR / args.image_file
    if not image_path.exists():
        die(f"Missing firmware image: {image_path}")

    model_rel = args.model.split(None, 1)[0]
    model_path = GV2_DIR / model_rel
    if not model_path.exists():
        die(f"Missing model file: {model_path}")

    port = args.port or autodetect_gv2_port()
    if not port:
        die(
            "no GV2 module found on any USB port; "
            "plug it in or pass --port /dev/cu.usbmodemXXXX"
        )

    cmd = [
        sys.executable,
        "-u",
        "xmodem/xmodem_send.py",
        "--port", port,
        "--baudrate", str(args.baudrate),
        "--protocol", args.protocol,
        "--file", args.image_file,
        "--model", args.model,
    ]

    info("Flashing GV2:")
    print(f"Port: {port}, baudrate: {args.baudrate}, protocol: {args.protocol}")
    print(f"Image: {image_path}")
    print(f"Model: {args.model}")
    print(_color(f"$ {' '.join(shlex.quote(str(c)) for c in cmd)}  (cwd={GV2_DIR})", "90"))

    proc = subprocess.Popen(
        cmd,
        cwd=str(GV2_DIR),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    assert proc.stdout is not None
    # Read raw bytes and split on either \n or \r so the xmodem progress bar
    # (which redraws in place with \r) is preserved instead of being flattened
    # to one line per tick by universal-newlines translation.
    buf = bytearray()
    try:
        while True:
            chunk = proc.stdout.read(4096)
            if not chunk:
                break
            for b in chunk:
                buf.append(b)
                if b in (0x0A, 0x0D):  # \n or \r
                    line = buf.decode("utf-8", errors="replace")
                    buf.clear()
                    sys.stdout.write(_clean_line(line))
                    sys.stdout.flush()
        if buf:
            sys.stdout.write(_clean_line(buf.decode("utf-8", errors="replace")))
            sys.stdout.flush()
    finally:
        returncode = proc.wait()
    sys.exit(returncode)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        die("interrupted", code=130)
