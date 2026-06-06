#!/usr/bin/env python3
"""Run a standardized 3DMark05 mini-replay semantic gate.

The gate compares original vs reordered mini-replay output, then optionally
uses primitive-id replays to classify whether any color movement follows a
canonical original-triangle final-writer change.
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "scripts" / "tools"


def resolve_path(path: str | Path) -> Path:
    value = Path(path)
    return value if value.is_absolute() else REPO_ROOT / value


def as_int(value: Any) -> int:
    try:
        return int(float(str(value)))
    except (TypeError, ValueError):
        return 0


def as_float(value: Any) -> float:
    try:
        return float(str(value))
    except (TypeError, ValueError):
        return 0.0


def load_manifest(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise SystemExit(f"missing manifest: {path}")
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)
    if data.get("schema") != "dxmt9.3dmark05.mini_replay_manifest.v1":
        raise SystemExit(f"unsupported manifest schema: {data.get('schema')}")
    if not data.get("draws"):
        raise SystemExit("manifest has no draws")
    return data


def load_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def read_first_csv_row(path: Path) -> dict[str, str]:
    rows = load_csv(path)
    if not rows:
        raise SystemExit(f"empty CSV: {path}")
    for row in rows:
        if row.get("area") == "full":
            return row
    return rows[0]


def read_json(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise SystemExit(f"missing JSON: {path}")
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def run_command(cmd: list[str], *, allow_fail: bool = False, dry_run: bool = False) -> int:
    print("+ " + " ".join(cmd))
    if dry_run:
        return 0
    proc = subprocess.run(cmd, check=False)
    if proc.returncode and not allow_fail:
        raise SystemExit(proc.returncode)
    return proc.returncode


def mini_replay_command(
    manifest: Path,
    output_dir: Path,
    primitive_order: str,
    color_output: Path,
    *,
    depth_input: Path | None,
    texture_input_dir: Path | None,
    force_primitive_id: bool = False,
    repeat: int = 1,
) -> list[str]:
    cmd = [
        sys.executable,
        str(TOOLS_DIR / "run_3dmark05_mini_replay.py"),
        str(manifest),
        "--output-dir",
        str(output_dir),
        "--compile",
        "--run",
        "--repeat",
        str(repeat),
        "--primitive-order",
        primitive_order,
        "--color-output",
        str(color_output),
    ]
    if depth_input is not None:
        cmd.extend(["--depth-input", str(depth_input)])
    if texture_input_dir is not None and not force_primitive_id:
        cmd.extend(["--texture-input-dir", str(texture_input_dir)])
    if force_primitive_id:
        cmd.append("--force-fragment-primitive-id")
    return cmd


def compare_command(
    before: Path,
    after: Path,
    output: Path,
    summary: Path,
    *,
    policy: str,
    label_before: str,
    label_after: str,
) -> list[str]:
    return [
        sys.executable,
        str(TOOLS_DIR / "compare_experiment_images.py"),
        "--before",
        str(before),
        "--after",
        str(after),
        "--label-before",
        label_before,
        "--label-after",
        label_after,
        "--policy",
        policy,
        "--require-similar",
        "--output",
        str(output),
        "--summary-output",
        str(summary),
    ]


def primitive_analysis_command(
    index_file: Path,
    before_pid: Path,
    after_pid: Path,
    before_color: Path,
    after_color: Path,
    output: Path,
    pixel_csv: Path,
    summary_csv: Path,
    *,
    before_order: str,
    after_order: str,
) -> list[str]:
    return [
        sys.executable,
        str(TOOLS_DIR / "analyze_primitive_id_replay.py"),
        "--index-file",
        str(index_file),
        "--before-primitive-id",
        str(before_pid),
        "--after-primitive-id",
        str(after_pid),
        "--before-primitive-order",
        before_order,
        "--after-primitive-order",
        after_order,
        "--before-color",
        str(before_color),
        "--after-color",
        str(after_color),
        "--pixel-scope",
        "color-or-primitive-changed",
        "--output",
        str(output),
        "--pixel-csv-output",
        str(pixel_csv),
        "--summary-csv-output",
        str(summary_csv),
    ]


def conflict_analysis_command(
    manifest: Path,
    draw_index: int,
    pixel_csv: Path,
    output: Path,
    csv_output: Path,
    summary_csv: Path,
) -> list[str]:
    return [
        sys.executable,
        str(TOOLS_DIR / "analyze_mini_replay_primitive_conflicts.py"),
        "--manifest",
        str(manifest),
        "--draw-index",
        str(draw_index),
        "--primitive-pixel-csv",
        str(pixel_csv),
        "--all-owner-changes",
        "--output",
        str(output),
        "--csv-output",
        str(csv_output),
        "--summary-csv-output",
        str(summary_csv),
    ]


def draw_index_file(manifest: dict[str, Any], draw_index: int) -> Path:
    draws = manifest.get("draws", [])
    if draw_index < 0 or draw_index >= len(draws):
        raise SystemExit(f"draw index out of range: {draw_index}")
    path = str(draws[draw_index].get("geometry", {}).get("index_file", ""))
    if not path:
        raise SystemExit(f"draw {draw_index} has no index_file")
    return resolve_path(path)


def default_primitive_draw_indices(manifest: dict[str, Any], value: str) -> list[int]:
    draws = manifest.get("draws", [])
    if value == "all":
        return list(range(len(draws)))
    out: list[int] = []
    for item in value.replace(";", ",").replace(" ", ",").split(","):
        item = item.strip()
        if not item:
            continue
        out.append(int(item, 0))
    return out


def aggregate_owner_summaries(paths: list[Path]) -> dict[str, Any]:
    rows: list[dict[str, str]] = []
    for path in paths:
        if path.exists():
            rows.extend(load_csv(path))
    return {
        "canonical_owner_changed_pixels": sum(
            as_int(row.get("primitive_identity_changed_pixels")) for row in rows
        ),
        "canonical_color_and_owner_changed_pixels": sum(
            as_int(row.get("color_and_primitive_changed_pixels")) for row in rows
        ),
        "canonical_color_changed_pixels": max(
            [as_int(row.get("color_changed_pixels")) for row in rows] or [0]
        ),
        "canonical_max_color_delta": max(
            [as_int(row.get("max_color_delta")) for row in rows] or [0]
        ),
        "owner_summary_rows": len(rows),
    }


def aggregate_conflict_summaries(paths: list[Path]) -> dict[str, Any]:
    rows: list[dict[str, str]] = []
    for path in paths:
        if path.exists():
            rows.extend(load_csv(path))
    return {
        "conflict_summary_rows": len(rows),
        "conflict_pixels": sum(as_int(row.get("pixels")) for row in rows),
        "conflict_color_changed_pixels": sum(
            as_int(row.get("color_changed_pixels")) for row in rows
        ),
        "conflict_both_cover_pixels": sum(
            as_int(row.get("both_cover_pixels")) for row in rows
        ),
        "conflict_max_color_delta": max(
            [as_int(row.get("max_color_delta")) for row in rows] or [0]
        ),
        "conflict_max_abs_depth_delta": max(
            [as_float(row.get("max_abs_depth_delta")) for row in rows] or [0.0]
        ),
        "conflict_max_uv0_delta": max(
            [as_float(row.get("max_uv0_delta")) for row in rows] or [0.0]
        ),
        "conflict_max_projected_tex7_delta": max(
            [as_float(row.get("max_projected_tex7_delta")) for row in rows] or [0.0]
        ),
        "conflict_max_tex1_delta": max(
            [as_float(row.get("max_tex1_delta")) for row in rows] or [0.0]
        ),
        "conflict_max_tex6_delta": max(
            [as_float(row.get("max_tex6_delta")) for row in rows] or [0.0]
        ),
    }


def verdict(color: dict[str, str], owner: dict[str, Any]) -> str:
    changed = as_int(color.get("changed_pixels"))
    owner_changed = int(owner.get("canonical_owner_changed_pixels", 0))
    color_owner = int(owner.get("canonical_color_and_owner_changed_pixels", 0))
    if changed == 0 and owner_changed == 0:
        return "pass-exact"
    if changed == 0 and owner_changed > 0:
        return "masked-final-writer-hazard"
    if color_owner > 0:
        return "fail-final-writer-hazard"
    return "fail-color-delta-without-owner-proof"


def write_summary(path: Path, summary: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")


def write_markdown(path: Path, summary: dict[str, Any]) -> None:
    color = summary["color_compare"]
    owner = summary["owner_compare"]
    conflict = summary.get("conflict_compare", {})
    estimate = summary["candidate_replay"].get("index_cache_estimate", {})
    lines = [
        "# 3DMark05 Mini-Replay Semantic Gate",
        "",
        "## Verdict",
        "",
        f"- Verdict: `{summary['verdict']}`",
        f"- Primitive order: `{summary['primitive_order']}`",
        f"- Color changed pixels: `{color.get('changed_pixels', '0')}`",
        f"- Color max delta: `{color.get('max_delta', '0')}`",
        f"- Canonical owner changed pixels: `{owner.get('canonical_owner_changed_pixels', 0)}`",
        f"- Color + owner changed pixels: `{owner.get('canonical_color_and_owner_changed_pixels', 0)}`",
        f"- Conflict pixels analyzed: `{conflict.get('conflict_pixels', 0)}`",
        f"- Conflict both-cover pixels: `{conflict.get('conflict_both_cover_pixels', 0)}`",
        f"- LRU32 delta: `{estimate.get('replay_lru32_miss_delta', 0)}`",
        f"- LRU32 delta %: `{estimate.get('replay_lru32_miss_delta_pct', 0.0)}`",
        "",
        "## Artifacts",
        "",
    ]
    for key, value in summary["artifacts"].items():
        lines.append(f"- `{key}`: `{value}`")
    lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def run_gate(args: argparse.Namespace) -> dict[str, Any]:
    manifest_path = resolve_path(args.manifest)
    manifest = load_manifest(manifest_path)
    output_dir = resolve_path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    depth_input = resolve_path(args.depth_input) if args.depth_input else None
    texture_input_dir = resolve_path(args.texture_input_dir) if args.texture_input_dir else None

    original_dir = output_dir / "original"
    candidate_dir = output_dir / args.primitive_order
    original_color = original_dir / "original.ppm"
    candidate_color = candidate_dir / f"{args.primitive_order}.ppm"
    original_pid_dir = output_dir / "original-primitive-id"
    candidate_pid_dir = output_dir / f"{args.primitive_order}-primitive-id"
    original_pid = original_pid_dir / "original-primitive-id.ppm"
    candidate_pid = candidate_pid_dir / f"{args.primitive_order}-primitive-id.ppm"

    if not args.summarize_only:
        run_command(mini_replay_command(
            manifest_path,
            original_dir,
            "original",
            original_color,
            depth_input=depth_input,
            texture_input_dir=texture_input_dir,
            repeat=args.repeat,
        ), dry_run=args.dry_run)
        run_command(mini_replay_command(
            manifest_path,
            candidate_dir,
            args.primitive_order,
            candidate_color,
            depth_input=depth_input,
            texture_input_dir=texture_input_dir,
            repeat=args.repeat,
        ), dry_run=args.dry_run)
        run_command(mini_replay_command(
            manifest_path,
            original_pid_dir,
            "original",
            original_pid,
            depth_input=depth_input,
            texture_input_dir=None,
            force_primitive_id=True,
            repeat=args.repeat,
        ), dry_run=args.dry_run)
        run_command(mini_replay_command(
            manifest_path,
            candidate_pid_dir,
            args.primitive_order,
            candidate_pid,
            depth_input=depth_input,
            texture_input_dir=None,
            force_primitive_id=True,
            repeat=args.repeat,
        ), dry_run=args.dry_run)

    color_report = output_dir / "color-compare.md"
    color_summary = output_dir / "color-compare-summary.csv"
    primitive_report = output_dir / "primitive-id-raw-compare.md"
    primitive_summary = output_dir / "primitive-id-raw-compare-summary.csv"
    if not args.summarize_only:
        run_command(compare_command(
            original_color,
            candidate_color,
            color_report,
            color_summary,
            policy=args.policy,
            label_before="original",
            label_after=args.primitive_order,
        ), allow_fail=True, dry_run=args.dry_run)
        run_command(compare_command(
            original_pid,
            candidate_pid,
            primitive_report,
            primitive_summary,
            policy="exact",
            label_before="original-primitive-id",
            label_after=f"{args.primitive_order}-primitive-id",
        ), allow_fail=True, dry_run=args.dry_run)

    owner_summary_paths: list[Path] = []
    conflict_summary_paths: list[Path] = []
    draw_indices = default_primitive_draw_indices(manifest, args.primitive_draw_indices)
    for draw_index in draw_indices:
        owner_md = output_dir / f"primitive-id-canonical-draw{draw_index:03d}.md"
        owner_pixels = output_dir / f"primitive-id-canonical-draw{draw_index:03d}-pixels.csv"
        owner_summary = output_dir / f"primitive-id-canonical-draw{draw_index:03d}-summary.csv"
        owner_summary_paths.append(owner_summary)
        conflict_md = output_dir / f"primitive-conflicts-draw{draw_index:03d}.md"
        conflict_csv = output_dir / f"primitive-conflicts-draw{draw_index:03d}.csv"
        conflict_summary = output_dir / f"primitive-conflicts-draw{draw_index:03d}-summary.csv"
        conflict_summary_paths.append(conflict_summary)
        if not args.summarize_only:
            run_command(primitive_analysis_command(
                draw_index_file(manifest, draw_index),
                original_pid,
                candidate_pid,
                original_color,
                candidate_color,
                owner_md,
                owner_pixels,
                owner_summary,
                before_order="original",
                after_order=args.primitive_order,
            ), dry_run=args.dry_run)
            if args.conflict_analysis:
                run_command(conflict_analysis_command(
                    manifest_path,
                    draw_index,
                    owner_pixels,
                    conflict_md,
                    conflict_csv,
                    conflict_summary,
                ), dry_run=args.dry_run)

    if args.dry_run:
        return {"dry_run": True}

    color_row = read_first_csv_row(color_summary)
    owner = aggregate_owner_summaries(owner_summary_paths)
    conflict = aggregate_conflict_summaries(conflict_summary_paths)
    original_summary = read_json(original_dir / "mini-replay-summary.json")
    candidate_summary = read_json(candidate_dir / "mini-replay-summary.json")
    summary = {
        "schema": "dxmt9.3dmark05.semantic_replay_gate.v1",
        "manifest": str(manifest_path),
        "primitive_order": args.primitive_order,
        "policy": args.policy,
        "depth_input": str(depth_input) if depth_input else None,
        "texture_input_dir": str(texture_input_dir) if texture_input_dir else None,
        "verdict": verdict(color_row, owner),
        "color_compare": color_row,
        "owner_compare": owner,
        "conflict_compare": conflict,
        "original_replay": original_summary,
        "candidate_replay": candidate_summary,
        "artifacts": {
            "color_compare": str(color_report),
            "color_compare_summary": str(color_summary),
            "primitive_raw_compare": str(primitive_report),
            "primitive_raw_compare_summary": str(primitive_summary),
            "summary_json": str(output_dir / "semantic-gate-summary.json"),
            "summary_md": str(output_dir / "semantic-gate-summary.md"),
        },
    }
    write_summary(output_dir / "semantic-gate-summary.json", summary)
    write_markdown(output_dir / "semantic-gate-summary.md", summary)
    return summary


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--primitive-order", default="cache-opt-lru32")
    parser.add_argument("--depth-input", type=Path)
    parser.add_argument("--texture-input-dir", type=Path)
    parser.add_argument("--policy", choices=("exact", "lsb1"), default="exact")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument(
        "--primitive-draw-indices",
        default="all",
        help="comma-separated manifest draw indices for canonical owner analysis, or 'all'",
    )
    parser.add_argument(
        "--conflict-analysis",
        action="store_true",
        help="also run the GT1 primitive conflict geometry analyzer per selected draw",
    )
    parser.add_argument(
        "--summarize-only",
        action="store_true",
        help="skip replay/compare execution and summarize existing output-dir artifacts",
    )
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def main() -> int:
    summary = run_gate(parse_args())
    print(json.dumps({
        key: summary.get(key)
        for key in ("schema", "verdict", "primitive_order", "dry_run")
        if key in summary
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
