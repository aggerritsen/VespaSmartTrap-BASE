#!/usr/bin/env python3
"""Summarize downloaded T-SIM Azure logs and photos."""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path
from typing import Any


DEFAULT_ROOT = Path(__file__).resolve().parent / "downloads" / "data"


def iter_jsonl(path: Path):
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line_no, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                yield line_no, json.loads(line), None
            except json.JSONDecodeError as exc:
                yield line_no, None, exc


def day_for_log(log_root: Path, path: Path) -> tuple[str, str]:
    parts = path.relative_to(log_root).parts
    device = parts[0] if parts else "<unknown>"
    day = parts[1] if len(parts) > 2 and parts[1].isdigit() else "<root>"
    return device, day


def confidence(value: Any) -> float | None:
    return value if isinstance(value, (int, float)) else None


def expected_photo_name(filename: str) -> str:
    name = filename.rsplit("/", 1)[-1] if filename else ""
    return f"saved_{name}" if name else ""


def parse_timestamp(value: Any) -> datetime | None:
    if not isinstance(value, str) or not value:
        return None
    try:
        return datetime.fromisoformat(value)
    except ValueError:
        return None


def summarize(
    root: Path,
    since: datetime | None = None,
) -> tuple[dict[tuple[str, str], Counter], Counter, list[dict[str, Any]], list[str], list[dict[str, Any]]]:
    logs = root / "logs"
    summary: dict[tuple[str, str], Counter] = defaultdict(Counter)
    azure = Counter()
    notable: list[dict[str, Any]] = []
    frames: list[dict[str, Any]] = []
    errors: list[str] = []
    confidences: dict[tuple[str, str], list[float]] = defaultdict(list)

    for path in sorted(logs.rglob("*.jsonl")):
        key = day_for_log(logs, path)
        name = path.name
        for line_no, obj, err in iter_jsonl(path):
            if err:
                errors.append(f"{path}:{line_no}: {err}")
                continue
            timestamp = parse_timestamp(obj.get("timestamp"))
            if since and (timestamp is None or timestamp < since):
                continue

            summary[key]["records"] += 1

            if name.startswith("frames") or "inference" in obj:
                summary[key]["frames"] += 1
                inf = obj.get("inference") or {}
                jpeg = obj.get("jpeg") or {}
                act = obj.get("actuation") or {}
                az = obj.get("azure") or {}
                c = confidence(inf.get("confidence"))
                if c is not None:
                    confidences[key].append(c)

                for field in ("filter_match", "doubtful_match", "detection_match"):
                    if inf.get(field):
                        summary[key][field] += 1
                if jpeg.get("saved"):
                    summary[key]["saved"] += 1
                if jpeg.get("valid") is False:
                    summary[key]["jpeg_invalid"] += 1
                if jpeg.get("crc_ok") is False:
                    summary[key]["crc_bad"] += 1
                if act.get("activated"):
                    summary[key]["actuated"] += 1

                status = az.get("status")
                if status and status != "not_applicable":
                    azure[status] += 1
                    summary[key][f"azure_{status}"] += 1

                frame_row = {
                    "timestamp": obj.get("timestamp"),
                    "timestamp_dt": timestamp,
                    "device": obj.get("device_name"),
                    "class": inf.get("class_idx"),
                    "state": inf.get("state"),
                    "confidence": c,
                    "confidence_threshold": inf.get("confidence_threshold"),
                    "doubtful_confidence_threshold": inf.get("doubtful_confidence_threshold"),
                    "detected_class": inf.get("detected_class"),
                    "occurrence_count": inf.get("occurrence_count"),
                    "occurrence_required": inf.get("occurrence_required"),
                    "occurrence_window_seconds": inf.get("occurrence_window_seconds"),
                    "filter_match": bool(inf.get("filter_match")),
                    "doubtful_match": bool(inf.get("doubtful_match")),
                    "detection_match": bool(inf.get("detection_match")),
                    "upload_doubtful_to_azure": bool(inf.get("upload_doubtful_to_azure")),
                    "valid": jpeg.get("valid"),
                    "crc_ok": jpeg.get("crc_ok"),
                    "saved": bool(jpeg.get("saved")),
                    "actuated": bool(act.get("activated")),
                    "azure": status or "not_applicable",
                    "cooldown_s": az.get("cooldown_remaining_s", 0),
                    "file": jpeg.get("filename") or "",
                    "log": str(path.relative_to(logs)),
                }
                frames.append(frame_row)

                if jpeg.get("saved") or act.get("activated") or status not in (None, "not_applicable"):
                    local_photo = expected_photo_name(jpeg.get("filename") or "")
                    notable.append(
                        {
                            "timestamp": obj.get("timestamp"),
                            "device": obj.get("device_name"),
                            "confidence": c,
                            "saved": bool(jpeg.get("saved")),
                            "actuated": bool(act.get("activated")),
                            "azure": status or "not_applicable",
                            "cooldown_s": az.get("cooldown_remaining_s", 0),
                            "file": jpeg.get("filename") or "",
                            "photo": local_photo,
                            "photo_present": bool(local_photo and (root / "photos" / local_photo).exists()),
                            "log": str(path.relative_to(logs)),
                        }
                    )
            elif name.startswith("health"):
                summary[key]["health"] += 1
            elif name.startswith("power"):
                summary[key]["power"] += 1
            elif name.startswith("log_upload_post_test"):
                summary[key]["post_upload_probe"] += 1

    for key, values in confidences.items():
        if values:
            summary[key]["max_conf_x1000"] = round(max(values) * 1000)
            summary[key]["avg_conf_x1000"] = round(sum(values) / len(values) * 1000)

    return summary, azure, notable, errors, frames


def gate_outcome(frame: dict[str, Any]) -> str:
    if frame["valid"] is not True:
        return "invalid_jpeg_or_crc"
    if frame["class"] != frame["detected_class"]:
        return "wrong_class"
    confidence_value = frame["confidence"]
    threshold = frame["confidence_threshold"]
    if not isinstance(confidence_value, (int, float)) or not isinstance(threshold, (int, float)):
        return "missing_confidence_or_threshold"
    if confidence_value < threshold:
        return "below_positive_confidence"
    if not frame["detection_match"]:
        return "positive_frame_but_occurrence_not_reached"
    return "positive_detection"


def print_frame_gate_summary(frames: list[dict[str, Any]], top: int) -> None:
    if not frames:
        return

    print("\nFrame gate analysis:")
    print(f"  frames: {len(frames)}")
    print(f"  devices: {dict(Counter(frame['device'] for frame in frames))}")
    print(f"  classes: {dict(Counter(frame['class'] for frame in frames))}")
    print(f"  states: {dict(Counter(frame['state'] for frame in frames))}")
    print(f"  outcomes: {dict(Counter(gate_outcome(frame) for frame in frames))}")

    hourly: dict[str, Counter] = defaultdict(Counter)
    for frame in frames:
        timestamp = frame["timestamp_dt"]
        hour = timestamp.strftime("%Y-%m-%d %H:00") if timestamp else "<unknown>"
        hourly[hour]["frames"] += 1
        for key in ("filter_match", "doubtful_match", "detection_match", "saved", "actuated"):
            if frame[key]:
                hourly[hour][key] += 1
        if frame["valid"] is False:
            hourly[hour]["invalid"] += 1
        if isinstance(frame["confidence"], (int, float)):
            hourly[hour]["max_conf_x1000"] = max(hourly[hour]["max_conf_x1000"], round(frame["confidence"] * 1000))

    print("\nHourly frame gate summary:")
    for hour, counts in sorted(hourly.items()):
        max_conf = counts["max_conf_x1000"] / 1000 if counts["max_conf_x1000"] else 0
        print(
            f"  {hour}: frames={counts['frames']} max_conf={max_conf:.3f} "
            f"filter={counts['filter_match']} doubtful={counts['doubtful_match']} "
            f"detection={counts['detection_match']} saved={counts['saved']} invalid={counts['invalid']}"
        )

    print("\nPositive-threshold match intervals:")
    previous = None
    matches = [frame for frame in frames if frame["filter_match"]]
    if not matches:
        print("  none")
    for frame in matches:
        timestamp = frame["timestamp_dt"]
        delta = (timestamp - previous).total_seconds() if timestamp and previous else None
        previous = timestamp
        delta_text = "first" if delta is None else f"{delta:.0f}s after previous"
        print(
            f"  {frame['timestamp']} conf={frame['confidence']:.3f} "
            f"occurrence={frame['occurrence_count']}/{frame['occurrence_required']} "
            f"detection={frame['detection_match']} ({delta_text})"
        )

    print(f"\nTop {top} confidence frames:")
    ranked = sorted(
        frames,
        key=lambda frame: frame["confidence"] if isinstance(frame["confidence"], (int, float)) else -1,
        reverse=True,
    )
    for frame in ranked[:top]:
        print(
            f"  {frame['timestamp']} class={frame['class']} conf={frame['confidence']:.3f} "
            f"valid={frame['valid']} filter={frame['filter_match']} "
            f"occurrence={frame['occurrence_count']}/{frame['occurrence_required']} "
            f"detection={frame['detection_match']} saved={frame['saved']}"
        )


def print_summary(root: Path, since: datetime | None, top: int) -> None:
    summary, azure, notable, errors, frames = summarize(root, since)
    photos = sorted((root / "photos").glob("*")) if (root / "photos").exists() else []

    print(f"Download root: {root}")
    if since:
        print(f"Since: {since.isoformat()}")
    print(f"Photos: {len(photos)}")
    for photo in photos:
        print(f"  {photo.name} ({photo.stat().st_size} bytes)")

    print("\nDevice/day summary:")
    for (device, day), counts in sorted(summary.items()):
        confidence_bits = ""
        if counts["frames"]:
            confidence_bits = (
                f" max_conf={counts['max_conf_x1000'] / 1000:.3f}"
                f" avg_conf={counts['avg_conf_x1000'] / 1000:.3f}"
            )
        print(
            f"  {device} {day}: records={counts['records']} frames={counts['frames']}"
            f"{confidence_bits}"
        )
        details = []
        for key in (
            "filter_match",
            "doubtful_match",
            "detection_match",
            "saved",
            "actuated",
            "jpeg_invalid",
            "crc_bad",
            "health",
            "power",
            "post_upload_probe",
            "azure_uploaded",
            "azure_skipped_cooldown",
        ):
            if counts[key]:
                details.append(f"{key}={counts[key]}")
        if details:
            print("    " + ", ".join(details))

    print("\nAzure frame upload statuses:")
    if azure:
        for status, count in sorted(azure.items()):
            print(f"  {status}: {count}")
    else:
        print("  none")

    print("\nSaved / actuated / uploaded frame records:")
    for row in sorted(notable, key=lambda item: item.get("timestamp") or ""):
        print(
            f"  {row['timestamp']} {row['device']} conf={row['confidence']:.3f} "
            f"saved={row['saved']} actuated={row['actuated']} azure={row['azure']} "
            f"cooldown_s={row['cooldown_s']} photo_present={row['photo_present']} "
            f"file={row['file']}"
        )

    print(f"\nJSON parse errors: {len(errors)}")
    for error in errors[:20]:
        print(f"  {error}")

    print_frame_gate_summary(frames, top)


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze downloaded T-SIM Azure data.")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT, help=f"Downloaded data root (default: {DEFAULT_ROOT})")
    parser.add_argument("--since", help="Only include records at or after this ISO timestamp, for example 2026-07-19T00:00:00.")
    parser.add_argument("--top", type=int, default=15, help="Number of highest-confidence frames to print (default: 15).")
    args = parser.parse_args()
    since = parse_timestamp(args.since) if args.since else None
    if args.since and since is None:
        raise SystemExit(f"Invalid --since timestamp: {args.since}")
    print_summary(args.root, since, args.top)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
