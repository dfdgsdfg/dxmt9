#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import shutil
import sys
import time
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TEMP_PREFIX_ROOT = REPO_ROOT / "tmp" / "prefixes"

@dataclass(frozen=True)
class PrefixEntry:
    path: Path
    size_bytes: int
    age_minutes: float


def format_size(size_bytes: int) -> str:
    units = ["B", "KiB", "MiB", "GiB", "TiB"]
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
            file_path = Path(root) / name
            try:
                total += file_path.stat().st_size
            except OSError:
                pass
    return total


def find_prefixes(temp_root: Path, older_than_minutes: float) -> list[PrefixEntry]:
    now = time.time()
    results: list[PrefixEntry] = []
    if not temp_root.is_dir():
        return results

    for path in sorted(temp_root.glob("dxmt9-exp-*")):
        if not path.is_dir():
            continue
        try:
            stat = path.stat()
        except OSError:
            continue
        age_minutes = max(0.0, (now - stat.st_mtime) / 60.0)
        if age_minutes < older_than_minutes:
            continue
        results.append(
            PrefixEntry(
                path=path,
                size_bytes=directory_size(path),
                age_minutes=age_minutes,
            )
        )
    return results


def cleanup_prefixes(prefixes: list[PrefixEntry], dry_run: bool) -> tuple[int, int]:
    deleted = 0
    freed = 0
    for entry in prefixes:
        if dry_run:
            deleted += 1
            freed += entry.size_bytes
            continue
        shutil.rmtree(entry.path, ignore_errors=True)
        if not entry.path.exists():
            deleted += 1
            freed += entry.size_bytes
    return deleted, freed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Delete stale dxmt9 experiment Wine temp prefixes.",
    )
    parser.add_argument(
        "--temp-root",
        default=str(DEFAULT_TEMP_PREFIX_ROOT),
        help="Temp root to scan. Default: <repo>/tmp/prefixes.",
    )
    parser.add_argument(
        "--older-than-minutes",
        type=float,
        default=60.0,
        help="Only delete prefixes older than this age. Default: 60 minutes.",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Ignore age and delete every matching dxmt9-exp-* prefix under the temp root.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="List matches and totals without deleting anything.",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Only print the final summary.",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    temp_root = Path(args.temp_root).expanduser().resolve()
    older_than_minutes = 0.0 if args.all else max(0.0, args.older_than_minutes)

    prefixes = find_prefixes(temp_root, older_than_minutes)
    total_size = sum(entry.size_bytes for entry in prefixes)

    if not args.quiet:
        print(f"temp_root: {temp_root}")
        print(f"matches: {len(prefixes)}")
        print(f"total_size: {format_size(total_size)}")
        for entry in prefixes:
            print(
                f"{entry.path}\t{format_size(entry.size_bytes)}\t{entry.age_minutes:.1f}m",
            )

    deleted, freed = cleanup_prefixes(prefixes, dry_run=args.dry_run)
    action = "would_delete" if args.dry_run else "deleted"
    print(f"{action}: {deleted}")
    print(f"freed: {format_size(freed)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
