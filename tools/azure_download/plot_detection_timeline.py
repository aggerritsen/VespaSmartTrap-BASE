#!/usr/bin/env python3
"""Plot T-SIM frame confidence and simulated detection events over time."""

from __future__ import annotations

import argparse
import json
from datetime import datetime
from pathlib import Path

import matplotlib.dates as mdates
import matplotlib.pyplot as plt


DEFAULT_ROOT = Path(__file__).resolve().parent / "downloads" / "data"
DEFAULT_OUTPUT = Path(__file__).resolve().parent / "reports" / "detection_confidence_timeline.png"


def parse_timestamp(value: str) -> datetime | None:
    try:
        return datetime.fromisoformat(value)
    except (TypeError, ValueError):
        return None


def load_frames(root: Path, since: datetime | None) -> list[dict]:
    logs = root / "logs"
    frames: list[dict] = []
    for path in sorted(logs.rglob("frames*.jsonl")):
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                obj = json.loads(line)
                timestamp = parse_timestamp(obj.get("timestamp", ""))
                if timestamp is None or (since and timestamp < since):
                    continue

                inf = obj.get("inference") or {}
                jpeg = obj.get("jpeg") or {}
                frames.append(
                    {
                        "timestamp": timestamp,
                        "device": obj.get("device_name", ""),
                        "class_idx": inf.get("class_idx"),
                        "confidence": inf.get("confidence"),
                        "valid": jpeg.get("valid") is True and jpeg.get("crc_ok") is True,
                    }
                )
    frames.sort(key=lambda item: item["timestamp"])
    return frames


def simulate_detections(frames: list[dict], threshold: float, occurrence: int, window_s: int, target_class: int) -> list[list[dict]]:
    streak: list[dict] = []
    detections: list[list[dict]] = []

    for frame in frames:
        if streak and (frame["timestamp"] - streak[0]["timestamp"]).total_seconds() > window_s:
            streak = []

        is_match = (
            frame["valid"]
            and frame["class_idx"] == target_class
            and isinstance(frame["confidence"], (int, float))
            and frame["confidence"] >= threshold
        )
        if not is_match:
            continue

        streak.append(frame)
        if len(streak) >= occurrence:
            detections.append(streak[:])
            streak = []

    return detections


def plot(frames: list[dict], detections: list[list[dict]], args: argparse.Namespace) -> None:
    valid_target = [
        frame for frame in frames
        if frame["valid"] and frame["class_idx"] == args.target_class and isinstance(frame["confidence"], (int, float))
    ]
    other = [
        frame for frame in frames
        if frame["valid"] and frame["class_idx"] != args.target_class and isinstance(frame["confidence"], (int, float))
    ]
    invalid = [
        frame for frame in frames
        if not frame["valid"] and isinstance(frame["confidence"], (int, float))
    ]

    fig, ax = plt.subplots(figsize=(15, 7), constrained_layout=True)
    fig.patch.set_facecolor("#f8fafc")
    ax.set_facecolor("#ffffff")

    if other:
        ax.scatter(
            [frame["timestamp"] for frame in other],
            [frame["confidence"] for frame in other],
            s=22,
            c="#cbd5e1",
            label="Other classes",
            alpha=0.8,
            edgecolors="none",
        )
    if invalid:
        ax.scatter(
            [frame["timestamp"] for frame in invalid],
            [frame["confidence"] for frame in invalid],
            s=45,
            c="#f97316",
            marker="x",
            label="Invalid JPEG/CRC",
        )
    if valid_target:
        below = [frame for frame in valid_target if frame["confidence"] < args.threshold]
        above = [frame for frame in valid_target if frame["confidence"] >= args.threshold]
        ax.scatter(
            [frame["timestamp"] for frame in below],
            [frame["confidence"] for frame in below],
            s=28,
            c="#ef4444",
            label=f"Class {args.target_class} below threshold",
            alpha=0.75,
            edgecolors="none",
        )
        ax.scatter(
            [frame["timestamp"] for frame in above],
            [frame["confidence"] for frame in above],
            s=44,
            c="#2563eb",
            label=f"Class {args.target_class} >= threshold",
            alpha=0.9,
            edgecolors="#1e3a8a",
            linewidths=0.35,
        )

    for index, cluster in enumerate(detections, 1):
        xs = [frame["timestamp"] for frame in cluster]
        ys = [frame["confidence"] for frame in cluster]
        ax.plot(xs, ys, color="#dc2626", linewidth=1.8, alpha=0.9)
        ax.scatter(xs[-1], ys[-1], s=120, marker="*", c="#dc2626", edgecolors="#7f1d1d", linewidths=0.5)
        ax.annotate(
            str(index),
            (xs[-1], ys[-1]),
            textcoords="offset points",
            xytext=(5, 7),
            fontsize=8,
            color="#7f1d1d",
            weight="bold",
        )

    ax.axhline(
        args.threshold,
        color="#16a34a",
        linestyle="--",
        linewidth=1.8,
        label=f"New confidence threshold {args.threshold:.3f}",
    )

    ax.set_title(
        f"T-SIM frame confidence timeline, threshold {args.threshold:.3f}, "
        f"occurrence {args.occurrence} within {args.window_seconds}s",
        fontsize=14,
        weight="bold",
    )
    ax.set_xlabel("Frame timestamp")
    ax.set_ylabel("Model confidence")
    ax.set_ylim(0, 1.0)
    ax.grid(True, color="#e2e8f0", linewidth=0.8)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax.xaxis.set_major_locator(mdates.HourLocator(interval=1))
    fig.autofmt_xdate(rotation=35)
    ax.legend(loc="upper left", frameon=True, framealpha=0.92)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, dpi=160)
    plt.close(fig)


def main() -> int:
    parser = argparse.ArgumentParser(description="Plot confidence timeline and simulated T-SIM detections.")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT, help=f"Downloaded data root (default: {DEFAULT_ROOT})")
    parser.add_argument("--since", default="2026-07-19T00:00:00", help="Only include frames at or after this ISO timestamp.")
    parser.add_argument("--threshold", type=float, default=0.745, help="Positive confidence threshold to plot.")
    parser.add_argument("--occurrence", type=int, default=2, help="Matching frames required for detection.")
    parser.add_argument("--window-seconds", type=int, default=5, help="Occurrence window in seconds.")
    parser.add_argument("--target-class", type=int, default=3, help="Target class index.")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help=f"Output PNG path (default: {DEFAULT_OUTPUT})")
    args = parser.parse_args()

    since = parse_timestamp(args.since)
    if since is None:
        raise SystemExit(f"Invalid --since timestamp: {args.since}")

    frames = load_frames(args.root, since)
    detections = simulate_detections(frames, args.threshold, args.occurrence, args.window_seconds, args.target_class)
    plot(frames, detections, args)
    print(f"Frames plotted: {len(frames)}")
    print(f"Simulated detections: {len(detections)}")
    print(f"Output: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
