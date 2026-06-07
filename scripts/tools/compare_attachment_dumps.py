#!/usr/bin/env python3
"""Compare dxmt9 raw attachment dumps with image-gate-compatible CSV output."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any


CSV_FIELDS = [
    "area",
    "width",
    "height",
    "compared_pixels",
    "changed_pixels",
    "changed_pct",
    "before_active_pixels",
    "before_active_pct",
    "after_active_pixels",
    "after_active_pct",
    "max_delta",
    "mean_abs_delta",
    "rms_delta",
    "ssim",
    "compared_bytes",
    "changed_bytes",
    "changed_bytes_pct",
    "metadata_status",
    "format",
    "format_name",
    "row_bytes",
    "byte_count",
]


def load_meta(path: Path) -> dict[str, Any]:
    meta_path = Path(f"{path}.json")
    if not meta_path.exists():
        raise SystemExit(f"missing metadata JSON: {meta_path}")
    return json.loads(meta_path.read_text(encoding="utf-8"))


def as_int(value: Any) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def fmt_float(value: float) -> str:
    return f"{value:.6f}"


def comparable_meta(before: dict[str, Any], after: dict[str, Any]) -> tuple[bool, str]:
    keys = ["format", "metalPixelFormat", "width", "height", "rowBytes", "byteCount"]
    mismatches = [
        f"{key}:{before.get(key)!r}!={after.get(key)!r}"
        for key in keys
        if before.get(key) != after.get(key)
    ]
    if mismatches:
        return False, ";".join(mismatches)
    return True, "compatible"


def compare_bytes(before_path: Path, after_path: Path, area: str) -> dict[str, str]:
    before_meta = load_meta(before_path)
    after_meta = load_meta(after_path)
    compatible, status = comparable_meta(before_meta, after_meta)
    before = before_path.read_bytes()
    after = after_path.read_bytes()
    if len(before) != len(after):
        compatible = False
        status = f"{status};byte-length:{len(before)}!={len(after)}"
    compared = min(len(before), len(after))
    changed = 0
    max_delta = 0
    total_abs = 0
    total_sq = 0
    for lhs, rhs in zip(before[:compared], after[:compared]):
        delta = abs(lhs - rhs)
        if delta:
            changed += 1
            if delta > max_delta:
                max_delta = delta
        total_abs += delta
        total_sq += delta * delta
    width = as_int(before_meta.get("width"))
    height = as_int(before_meta.get("height"))
    pixels = width * height
    changed_pct = (changed / compared * 100.0) if compared else 0.0
    mean_abs = (total_abs / compared) if compared else 0.0
    rms = ((total_sq / compared) ** 0.5) if compared else 0.0
    return {
        "area": area,
        "width": str(width),
        "height": str(height),
        "compared_pixels": str(pixels),
        "changed_pixels": str(changed),
        "changed_pct": fmt_float(changed_pct),
        "before_active_pixels": "",
        "before_active_pct": "",
        "after_active_pixels": "",
        "after_active_pct": "",
        "max_delta": str(max_delta),
        "mean_abs_delta": fmt_float(mean_abs),
        "rms_delta": fmt_float(rms),
        "ssim": "1.000000" if changed == 0 and compatible else "0.000000",
        "compared_bytes": str(compared),
        "changed_bytes": str(changed),
        "changed_bytes_pct": fmt_float(changed_pct),
        "metadata_status": status,
        "format": str(before_meta.get("format", "")),
        "format_name": str(before_meta.get("formatName", "")),
        "row_bytes": str(before_meta.get("rowBytes", "")),
        "byte_count": str(before_meta.get("byteCount", "")),
    }


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_report(
    path: Path,
    before_path: Path,
    after_path: Path,
    rows: list[dict[str, str]],
    require_exact: bool,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    failures = [
        row for row in rows
        if row["changed_bytes"] != "0" or row["metadata_status"] != "compatible"
    ]
    lines = [
        "# Attachment Dump Comparison",
        "",
        f"- Before: `{before_path}`",
        f"- After: `{after_path}`",
        f"- Policy: `{'exact' if require_exact else 'report-only'}`",
        "",
        "| Area | Format | Size | Changed bytes | Changed % | Max delta | Metadata |",
        "|---|---|---:|---:|---:|---:|---|",
    ]
    for row in rows:
        lines.append(
            f"| `{row['area']}` | `{row['format_name']}` | "
            f"`{row['width']}x{row['height']}` | "
            f"`{row['changed_bytes']}/{row['compared_bytes']}` | "
            f"`{row['changed_bytes_pct']}%` | `{row['max_delta']}` | "
            f"`{row['metadata_status']}` |"
        )
    lines.extend(["", "## Requirement Status", ""])
    if require_exact and failures:
        lines.append(f"- Failed: `{len(failures)}` attachment dump gate(s) did not pass.")
    elif require_exact:
        lines.append("- Passed: all attachment dumps are byte-exact and metadata-compatible.")
    else:
        lines.append("- Not requested: exact gate was not enforced.")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--after", required=True, type=Path)
    parser.add_argument("--area", default="attachment")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--summary-output", required=True, type=Path)
    parser.add_argument("--require-exact", action="store_true")
    args = parser.parse_args()

    if not args.before.exists():
        parser.error(f"missing before dump: {args.before}")
    if not args.after.exists():
        parser.error(f"missing after dump: {args.after}")

    row = compare_bytes(args.before, args.after, args.area)
    write_csv(args.summary_output, [row])
    write_report(args.output, args.before, args.after, [row], args.require_exact)
    if args.require_exact and (row["changed_bytes"] != "0" or row["metadata_status"] != "compatible"):
        print(args.output)
        print(args.summary_output)
        return 1
    print(args.output)
    print(args.summary_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
