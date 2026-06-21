#!/usr/bin/env python3
"""Build a power/uptime CSV dataset and matplotlib timeline from device logs."""

from __future__ import annotations

import argparse
import csv
import json
from bisect import bisect_left
from datetime import datetime
from pathlib import Path


DEFAULT_HEALTH_LOG = Path(r"D:\health.log")
DEFAULT_POWER_LOG = Path(r"D:\power.log")
DEFAULT_CSV = Path("data/power_uptime_dataset.csv")
DEFAULT_PLOT = Path("data/power_uptime_plot.png")


def parse_timestamp(value: str) -> datetime:
    return datetime.fromisoformat(value)


def iter_json_lines(path: Path):
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                yield line_number, json.loads(line)
            except json.JSONDecodeError:
                continue


def load_health(path: Path, since: datetime | None) -> list[dict]:
    records: list[dict] = []
    for line_number, record in iter_json_lines(path):
        if record.get("type") != "health" or "timestamp" not in record:
            continue
        timestamp = parse_timestamp(record["timestamp"])
        if since and timestamp < since:
            continue

        power = record.get("power") or {}
        records.append(
            {
                "timestamp": timestamp,
                "source_line_health": line_number,
                "uptime_ms": record.get("uptime_ms"),
                "state": record.get("state"),
                "errors": ";".join(record.get("errors") or []),
                "battery_percent": power.get("battery_percent"),
                "battery_mv": power.get("battery_mv"),
                "vbus_mv": power.get("vbus_mv"),
                "mains_power": power.get("mains_power"),
                "low_power": power.get("low_power"),
            }
        )
    return sorted(records, key=lambda item: item["timestamp"])


def load_power(path: Path, since: datetime | None) -> list[dict]:
    records: list[dict] = []
    for line_number, record in iter_json_lines(path):
        if "timestamp" not in record:
            continue
        timestamp = parse_timestamp(record["timestamp"])
        if since and timestamp < since:
            continue

        battery = record.get("battery") or {}
        input_power = record.get("input") or {}
        system = record.get("system") or {}
        charger = record.get("charger") or {}
        records.append(
            {
                "timestamp": timestamp,
                "source_line_power": line_number,
                "power_uptime_ms": record.get("uptime_ms"),
                "pmu_temp_c": system.get("pmu_temp_c"),
                "power_battery_percent": battery.get("percent"),
                "power_battery_mv": battery.get("mv"),
                "power_vbus_mv": input_power.get("vbus_mv"),
                "power_mains_power": input_power.get("mains_power"),
                "charger_direction": charger.get("direction"),
                "charger_status": charger.get("status"),
            }
        )
    return sorted(records, key=lambda item: item["timestamp"])


def nearest_power_record(power_records: list[dict], timestamp: datetime) -> dict | None:
    if not power_records:
        return None

    timestamps = [record["timestamp"] for record in power_records]
    index = bisect_left(timestamps, timestamp)
    candidates = []
    if index > 0:
        candidates.append(power_records[index - 1])
    if index < len(power_records):
        candidates.append(power_records[index])
    return min(candidates, key=lambda item: abs((item["timestamp"] - timestamp).total_seconds()))


def build_rows(health_records: list[dict], power_records: list[dict]) -> list[dict]:
    rows: list[dict] = []
    previous: dict | None = None
    previous_uptime_ms: int | None = None

    for record in health_records:
        timestamp = record["timestamp"]
        nearest_power = nearest_power_record(power_records, timestamp)
        uptime_ms = record.get("uptime_ms")
        uptime_seconds = None if uptime_ms is None else round(float(uptime_ms) / 1000.0, 3)

        gap_seconds = None
        inferred_sleep_seconds = 0.0
        uptime_reset = False
        if previous:
            gap_seconds = (timestamp - previous["timestamp"]).total_seconds()
            expected_cadence_seconds = 60.0
            inferred_sleep_seconds = max(0.0, gap_seconds - expected_cadence_seconds)
        if previous_uptime_ms is not None and uptime_ms is not None:
            uptime_reset = int(uptime_ms) < int(previous_uptime_ms)

        nearest_delta_seconds = None
        if nearest_power:
            nearest_delta_seconds = round(abs((nearest_power["timestamp"] - timestamp).total_seconds()), 3)

        row = {
            "timestamp": timestamp.isoformat(timespec="seconds"),
            "date": timestamp.date().isoformat(),
            "time": timestamp.time().isoformat(timespec="seconds"),
            "uptime_ms": uptime_ms,
            "uptime_seconds": uptime_seconds,
            "gap_since_previous_seconds": gap_seconds,
            "inferred_sleep_seconds": round(inferred_sleep_seconds, 3),
            "uptime_reset": uptime_reset,
            "health_state": record.get("state"),
            "health_errors": record.get("errors"),
            "low_power": record.get("low_power"),
            "battery_percent": record.get("battery_percent"),
            "battery_mv": record.get("battery_mv"),
            "vbus_mv": record.get("vbus_mv"),
            "mains_power": record.get("mains_power"),
            "pmu_temp_c": nearest_power.get("pmu_temp_c") if nearest_power else None,
            "nearest_power_timestamp": nearest_power["timestamp"].isoformat(timespec="seconds")
            if nearest_power
            else None,
            "nearest_power_delta_seconds": nearest_delta_seconds,
            "charger_direction": nearest_power.get("charger_direction") if nearest_power else None,
            "charger_status": nearest_power.get("charger_status") if nearest_power else None,
            "source_line_health": record.get("source_line_health"),
            "source_line_power": nearest_power.get("source_line_power") if nearest_power else None,
        }
        rows.append(row)

        previous = record
        if uptime_ms is not None:
            previous_uptime_ms = int(uptime_ms)

    return rows


def write_csv(rows: list[dict], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "timestamp",
        "date",
        "time",
        "uptime_ms",
        "uptime_seconds",
        "gap_since_previous_seconds",
        "inferred_sleep_seconds",
        "uptime_reset",
        "health_state",
        "health_errors",
        "low_power",
        "battery_percent",
        "battery_mv",
        "vbus_mv",
        "mains_power",
        "pmu_temp_c",
        "nearest_power_timestamp",
        "nearest_power_delta_seconds",
        "charger_direction",
        "charger_status",
        "source_line_health",
        "source_line_power",
    ]
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_plot(rows: list[dict], path: Path) -> None:
    import matplotlib.dates as mdates
    import matplotlib.pyplot as plt

    path.parent.mkdir(parents=True, exist_ok=True)

    timestamps = [parse_timestamp(row["timestamp"]) for row in rows]
    battery_percent = [as_float(row["battery_percent"]) for row in rows]
    battery_voltage = [as_float(row["battery_mv"]) / 1000.0 for row in rows]
    pmu_temp = [as_float(row["pmu_temp_c"]) for row in rows]
    state_times, state_values = build_operating_state_series(rows)

    fig, axes = plt.subplots(4, 1, figsize=(14, 9), sharex=True)

    axes[0].plot(timestamps, battery_percent, color="#1f77b4", linewidth=1.6)
    axes[0].set_ylabel("Battery %")
    axes[0].grid(True, alpha=0.3)

    axes[1].plot(timestamps, battery_voltage, color="#2ca02c", linewidth=1.6)
    axes[1].set_ylabel("Voltage (V)")
    axes[1].grid(True, alpha=0.3)

    axes[2].plot(timestamps, pmu_temp, color="#d62728", linewidth=1.6)
    axes[2].set_ylabel("PMU temp (C)")
    axes[2].grid(True, alpha=0.3)

    axes[3].step(state_times, state_values, where="post", color="#111111", linewidth=1.7)
    axes[3].set_ylabel("Ops")
    axes[3].set_yticks([0, 1])
    axes[3].set_yticklabels(["sleep", "awake"])
    axes[3].set_ylim(-0.15, 1.15)
    axes[3].grid(True, alpha=0.3)

    axes[-1].xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m-%d\n%H:%M"))
    fig.suptitle("Power, Operating State and Temperature Timeline")
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(path, dpi=160)
    plt.close(fig)


def build_operating_state_series(rows: list[dict], cadence_seconds: float = 60.0) -> tuple[list[datetime], list[int]]:
    """Return a step-series where 1 is awake/logging and 0 is inferred sleep/offline."""
    if not rows:
        return [], []

    timestamps = [parse_timestamp(row["timestamp"]) for row in rows]
    state_times: list[datetime] = [timestamps[0]]
    state_values: list[int] = [1]

    for previous, current in zip(timestamps, timestamps[1:]):
        gap_seconds = (current - previous).total_seconds()
        if gap_seconds > cadence_seconds * 1.5:
            sleep_start = previous
            sleep_end = current
            state_times.extend([sleep_start, sleep_start, sleep_end, sleep_end])
            state_values.extend([1, 0, 0, 1])
        else:
            state_times.append(current)
            state_values.append(1)

    return state_times, state_values


def as_float(value) -> float:
    if value in (None, ""):
        return float("nan")
    return float(value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--health-log", type=Path, default=DEFAULT_HEALTH_LOG)
    parser.add_argument("--power-log", type=Path, default=DEFAULT_POWER_LOG)
    parser.add_argument("--csv", type=Path, default=DEFAULT_CSV)
    parser.add_argument("--plot", type=Path, default=DEFAULT_PLOT)
    parser.add_argument("--since", default="2026-06-18T00:00:00")
    parser.add_argument("--no-plot", action="store_true")
    args = parser.parse_args()

    since = parse_timestamp(args.since) if args.since else None
    health_records = load_health(args.health_log, since)
    power_records = load_power(args.power_log, since)
    rows = build_rows(health_records, power_records)

    write_csv(rows, args.csv)
    if not args.no_plot:
        write_plot(rows, args.plot)

    total_sleep_seconds = sum(as_float(row["inferred_sleep_seconds"]) for row in rows)
    print(f"health_records={len(health_records)}")
    print(f"power_records={len(power_records)}")
    print(f"csv={args.csv}")
    if not args.no_plot:
        print(f"plot={args.plot}")
    print(f"inferred_sleep_hours={total_sleep_seconds / 3600.0:.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
