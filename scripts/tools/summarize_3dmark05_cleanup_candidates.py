#!/usr/bin/env python3
"""Summarize 3DMark05 trace/output cleanup candidates by run id."""

from __future__ import annotations

import argparse
import csv
import os
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TRACE_ROOT = REPO_ROOT / "traces"
DEFAULT_OUTPUT_ROOT = REPO_ROOT / "experiments" / "output"
DEFAULT_REFERENCE_FILES = (REPO_ROOT / "specs" / "perfomance.plan.md",)
RUN_PREFIX = "app-d3d9-3dmark05-"


@dataclass
class RunEntry:
    run_id: str
    trace_path: Path | None = None
    output_path: Path | None = None
    trace_size_bytes: int = 0
    output_size_bytes: int = 0
    reference_count: int = 0

    @property
    def total_size_bytes(self) -> int:
        return self.trace_size_bytes + self.output_size_bytes

    @property
    def status(self) -> str:
        return "referenced" if self.reference_count else "unreferenced"

    @property
    def cleanup_hint(self) -> str:
        if self.reference_count:
            return "keep or copy needed analysis artifacts before deleting"
        return "candidate for manual run-id cleanup"


def format_size(size_bytes: int) -> str:
    units = ("B", "KiB", "MiB", "GiB", "TiB")
    value = float(size_bytes)
    for unit in units:
        if value < 1024.0 or unit == units[-1]:
            return f"{value:.1f}{unit}"
        value /= 1024.0
    return f"{size_bytes}B"


def directory_size(path: Path) -> int:
    total = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            try:
                total += (Path(root) / name).stat().st_size
            except OSError:
                pass
    return total


def read_references(paths: list[Path]) -> str:
    chunks: list[str] = []
    for path in paths:
        if not path.exists():
            raise SystemExit(f"missing reference file: {path}")
        chunks.append(path.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(chunks)


def scan_run_dirs(trace_root: Path, output_root: Path, references: str) -> list[RunEntry]:
    entries: dict[str, RunEntry] = {}

    def entry_for(path: Path) -> RunEntry:
        run_id = path.name
        entry = entries.get(run_id)
        if entry is None:
            entry = RunEntry(run_id=run_id, reference_count=references.count(run_id))
            entries[run_id] = entry
        return entry

    if trace_root.is_dir():
        for path in sorted(trace_root.glob(f"{RUN_PREFIX}*")):
            if path.is_dir():
                entry = entry_for(path)
                entry.trace_path = path
                entry.trace_size_bytes = directory_size(path)

    if output_root.is_dir():
        for path in sorted(output_root.glob(f"{RUN_PREFIX}*")):
            if path.is_dir():
                entry = entry_for(path)
                entry.output_path = path
                entry.output_size_bytes = directory_size(path)

    rows = list(entries.values())
    rows.sort(key=lambda row: (row.status != "unreferenced", -row.total_size_bytes, row.run_id))
    return rows


def limited(rows: list[RunEntry], top: int) -> list[RunEntry]:
    if top <= 0:
        return rows
    return rows[:top]


def write_csv(path: Path, rows: list[RunEntry]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = (
        "run_id",
        "status",
        "reference_count",
        "total_size_bytes",
        "total_size",
        "trace_size",
        "output_size",
        "trace_path",
        "output_path",
        "cleanup_hint",
    )
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({
                "run_id": row.run_id,
                "status": row.status,
                "reference_count": row.reference_count,
                "total_size_bytes": row.total_size_bytes,
                "total_size": format_size(row.total_size_bytes),
                "trace_size": format_size(row.trace_size_bytes),
                "output_size": format_size(row.output_size_bytes),
                "trace_path": str(row.trace_path or ""),
                "output_path": str(row.output_path or ""),
                "cleanup_hint": row.cleanup_hint,
            })


def write_markdown(path: Path, rows: list[RunEntry], reference_files: list[Path]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    unreferenced_total = sum(row.total_size_bytes for row in rows if row.status == "unreferenced")
    referenced_total = sum(row.total_size_bytes for row in rows if row.status == "referenced")
    lines = [
        "# 3DMark05 Cleanup Candidates",
        "",
        "This report is non-destructive. Delete only obsolete run ids after",
        "confirming that required analysis artifacts have been copied or are",
        "preserved elsewhere.",
        "",
        "## Inputs",
        "",
    ]
    for ref in reference_files:
        lines.append(f"- Reference: `{ref}`")
    lines.extend([
        "",
        "## Summary",
        "",
        "| Metric | Value |",
        "|---|---:|",
        f"| Rows | `{len(rows)}` |",
        f"| Unreferenced bytes | `{format_size(unreferenced_total)}` |",
        f"| Referenced bytes | `{format_size(referenced_total)}` |",
        "",
        "## Candidates",
        "",
        "| Run id | Status | Refs | Total | Trace | Output | Hint |",
        "|---|---|---:|---:|---:|---:|---|",
    ])
    for row in rows:
        lines.append(
            f"| `{row.run_id}` | `{row.status}` | `{row.reference_count}` | "
            f"`{format_size(row.total_size_bytes)}` | "
            f"`{format_size(row.trace_size_bytes)}` | "
            f"`{format_size(row.output_size_bytes)}` | {row.cleanup_hint} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace-root", type=Path, default=DEFAULT_TRACE_ROOT)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument(
        "--reference-file",
        action="append",
        type=Path,
        default=[],
        help="Reference text to scan for run ids. Defaults to specs/perfomance.plan.md.",
    )
    parser.add_argument("--top", type=int, default=40, help="Rows to emit; 0 means all.")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--csv-output", type=Path)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    reference_files = args.reference_file or list(DEFAULT_REFERENCE_FILES)
    references = read_references(reference_files)
    rows = limited(scan_run_dirs(args.trace_root, args.output_root, references), args.top)
    write_markdown(args.output, rows, reference_files)
    if args.csv_output is not None:
        write_csv(args.csv_output, rows)
    print(args.output)
    if args.csv_output is not None:
        print(args.csv_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
