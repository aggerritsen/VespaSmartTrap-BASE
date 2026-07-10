#!/usr/bin/env python3
"""Build the Grove Vision AI V2 firmware image (cross-platform port of build_gv2_image.ps1)."""

from __future__ import annotations

import argparse
import platform
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent
GV2_ROOT = REPO_ROOT / "external" / "gv2-firmware"
APP_ROOT = GV2_ROOT / "EPII_CM55M_APP_S"
MAKEFILE = APP_ROOT / "makefile"
ELF = (
    APP_ROOT
    / "obj_epii_evb_icv30_bdv10"
    / "gnu_epii_evb_WLCSP65"
    / "EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf"
)
IMAGE_GEN_ROOT = GV2_ROOT / "we2_image_gen_local"
IMAGE_GEN_PROJECT = IMAGE_GEN_ROOT / "project_case1_blp_wlcsp.json"
IMAGE_INPUT_ELF = (
    IMAGE_GEN_ROOT / "input_case1_secboot" / "EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf"
)
OUTPUT_IMAGE = IMAGE_GEN_ROOT / "output_case1_sec_wlcsp" / "output.img"


def _isatty() -> bool:
    try:
        return sys.stdout.isatty()
    except Exception:
        return False


def _color(text: str, code: str) -> str:
    return f"\033[{code}m{text}\033[0m" if _isatty() else text


def info(msg: str) -> None:
    print(_color(msg, "36"), flush=True)  # cyan


def ok(msg: str) -> None:
    print(_color(msg, "32"), flush=True)  # green


def die(msg: str, code: int = 1) -> "None":
    print(_color(f"error: {msg}", "31"), file=sys.stderr, flush=True)
    sys.exit(code)


def select_image_gen_binary() -> Path:
    system = platform.system()
    machine = platform.machine().lower()

    if system == "Windows":
        candidate = IMAGE_GEN_ROOT / "we2_local_image_gen.exe"
    elif system == "Darwin":
        if machine in ("arm64", "aarch64"):
            candidate = IMAGE_GEN_ROOT / "we2_local_image_gen_macOS_arm64"
        else:
            die(
                f"No macOS image generator binary available for arch '{machine}'. "
                "Only arm64 (Apple Silicon) is shipped in the submodule."
            )
    elif system == "Linux":
        if machine not in ("x86_64", "amd64"):
            die(
                f"No Linux image generator binary available for arch '{machine}'. "
                "Only x86_64 is shipped in the submodule."
            )
        candidate = IMAGE_GEN_ROOT / "we2_local_image_gen"
    else:
        die(f"Unsupported OS: {system}")

    if not candidate.exists():
        die(f"Missing image generator: {candidate}")
    return candidate


def run(cmd: list[str], cwd: Path) -> None:
    printable = " ".join(shlex.quote(str(c)) for c in cmd)
    print(_color(f"$ {printable}  (cwd={cwd})", "90"), flush=True)
    result = subprocess.run(cmd, cwd=str(cwd))
    if result.returncode != 0:
        sys.exit(result.returncode)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build the Grove Vision AI V2 firmware image."
    )
    parser.add_argument(
        "--target", default="all", help="make target (default: all)"
    )
    parser.add_argument(
        "--extra-make-args",
        default="-j8",
        help="extra arguments passed to make (default: '-j8')",
    )
    parser.add_argument(
        "--keep-previous-image",
        action="store_true",
        help="rename any existing output.img before rebuilding",
    )
    parser.add_argument(
        "--previous-image-name",
        default="output.old",
        help="filename used when keeping the previous image (default: output.old)",
    )
    args = parser.parse_args()

    if not MAKEFILE.exists():
        die(f"Missing GV2 makefile: {MAKEFILE}")

    if shutil.which("make") is None:
        die("'make' was not found in PATH.")

    if not IMAGE_GEN_PROJECT.exists():
        die(f"Missing image generator project: {IMAGE_GEN_PROJECT}")

    image_gen_exe = select_image_gen_binary()

    info("Building GV2 ELF...")
    make_cmd = ["make", args.target, *shlex.split(args.extra_make_args)]
    run(make_cmd, cwd=APP_ROOT)

    if not ELF.exists():
        die(f"Build completed, but expected ELF was not found: {ELF}")

    if args.keep_previous_image and OUTPUT_IMAGE.exists():
        previous_image = OUTPUT_IMAGE.with_name(args.previous_image_name)
        if previous_image.exists():
            previous_image.unlink()
        OUTPUT_IMAGE.rename(previous_image)

    info("Staging ELF for image generator...")
    IMAGE_INPUT_ELF.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ELF, IMAGE_INPUT_ELF)

    info("Generating output.img...")
    run([str(image_gen_exe), str(IMAGE_GEN_PROJECT.name)], cwd=IMAGE_GEN_ROOT)

    if not OUTPUT_IMAGE.exists():
        die(f"Image generator completed, but output image was not found: {OUTPUT_IMAGE}")

    size = OUTPUT_IMAGE.stat().st_size
    ok("GV2 image ready:")
    print(f"{OUTPUT_IMAGE} ({size} bytes)")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        die("interrupted", code=130)
