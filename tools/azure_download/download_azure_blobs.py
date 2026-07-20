#!/usr/bin/env python3
"""Download T-SIM Azure Blob uploads with AzCopy.

The script reads the Azure blob host/container/SAS from the local firmware
config_secrets.h file and uses AzCopy's source-newer overwrite mode so already
downloaded files are not fetched again unless Azure has a newer timestamp.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path
from urllib.parse import quote


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SECRETS = REPO_ROOT / "t-sim" / "firmware" / "src" / "config_secrets.h"
DEFAULT_DEST = Path(__file__).resolve().parent / "downloads"
DEFAULT_PREFIXES = ("photos", "logs")


def read_secret_macros(path: Path) -> dict[str, str]:
    if not path.exists():
        raise FileNotFoundError(f"Azure secrets file not found: {path}")

    text = path.read_text(encoding="utf-8")
    values: dict[str, str] = {}
    for name in ("AZURE_BLOB_HOST", "AZURE_BLOB_CONTAINER", "AZURE_BLOB_SAS"):
        match = re.search(rf'^\s*#define\s+{name}\s+"([^"]*)"', text, re.MULTILINE)
        if not match or not match.group(1):
            raise ValueError(f"{name} is missing or empty in {path}")
        values[name] = match.group(1)
    return values


def normalize_prefix(prefix: str) -> str:
    return prefix.strip().strip("/")


def container_url(secrets: dict[str, str]) -> str:
    host = secrets["AZURE_BLOB_HOST"].strip().strip("/")
    container = secrets["AZURE_BLOB_CONTAINER"].strip().strip("/")
    sas = secrets["AZURE_BLOB_SAS"].strip()
    if sas.startswith("?"):
        sas = sas[1:]
    return f"https://{host}/{quote(container)}?{sas}"


def redacted_container_url(secrets: dict[str, str]) -> str:
    host = secrets["AZURE_BLOB_HOST"].strip().strip("/")
    container = secrets["AZURE_BLOB_CONTAINER"].strip().strip("/")
    return f"https://{host}/{container}?<sas-redacted>"


def build_command(args: argparse.Namespace, source_url: str, prefixes: list[str]) -> list[str]:
    command = [
        args.azcopy,
        "copy",
        source_url,
        str(args.destination),
        "--recursive=true",
        "--overwrite=ifSourceNewer",
    ]

    if prefixes:
        command.append(f"--include-path={';'.join(prefixes)}")
    if args.dry_run:
        command.append("--dry-run")
    if args.extra_azcopy_args:
        command.extend(args.extra_azcopy_args)

    return command


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Download T-SIM Azure Blob uploads using AzCopy and firmware secrets."
    )
    parser.add_argument(
        "--secrets",
        type=Path,
        default=DEFAULT_SECRETS,
        help=f"Path to config_secrets.h (default: {DEFAULT_SECRETS})",
    )
    parser.add_argument(
        "--destination",
        type=Path,
        default=DEFAULT_DEST,
        help=f"Local download root (default: {DEFAULT_DEST})",
    )
    parser.add_argument(
        "--prefix",
        action="append",
        dest="prefixes",
        help="Blob prefix to download. Repeat for multiple prefixes. Defaults to photos and logs.",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Download the full container instead of the default T-SIM photos/logs prefixes.",
    )
    parser.add_argument(
        "--azcopy",
        default="azcopy",
        help="AzCopy executable name or path (default: azcopy).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Ask AzCopy to show planned transfers without downloading.",
    )
    parser.add_argument(
        "extra_azcopy_args",
        nargs=argparse.REMAINDER,
        help="Extra arguments passed to AzCopy after a literal --.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.extra_azcopy_args and args.extra_azcopy_args[0] == "--":
        args.extra_azcopy_args = args.extra_azcopy_args[1:]

    if shutil.which(args.azcopy) is None and not Path(args.azcopy).exists():
        print(f"ERROR: AzCopy executable not found: {args.azcopy}", file=sys.stderr)
        return 2

    prefixes = [] if args.all else [normalize_prefix(p) for p in (args.prefixes or DEFAULT_PREFIXES)]
    prefixes = [p for p in prefixes if p]

    secrets = read_secret_macros(args.secrets)
    source = container_url(secrets)
    args.destination.mkdir(parents=True, exist_ok=True)

    print("T-SIM Azure download", flush=True)
    print(f"  source:      {redacted_container_url(secrets)}", flush=True)
    print(f"  prefixes:    {', '.join(prefixes) if prefixes else '<entire container>'}", flush=True)
    print(f"  destination: {args.destination}", flush=True)
    print("  duplicate policy: skip local files unless the Azure blob timestamp is newer", flush=True)

    command = build_command(args, source, prefixes)
    completed = subprocess.run(command, check=False)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
