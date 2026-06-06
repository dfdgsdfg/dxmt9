#!/usr/bin/env python3
"""Preflight whether PSO/state churn is a credible 3DMark05 backend-storage target."""

from __future__ import annotations

import argparse
from collections import Counter
import csv
from pathlib import Path
from typing import Any, Iterable


CSV_FIELDS = [
    "row",
    "verdict",
    "draws",
    "triangles",
    "vertices",
    "pso_changes",
    "pso_unique",
    "pso_changes_per_draw",
    "shader_variant_changes",
    "shader_changes_per_draw",
    "stream_handle_changes",
    "stream_handle_changes_per_draw",
    "ib_handle_changes",
    "ib_handle_changes_per_draw",
    "geometry_unique",
    "geometry_unique_ratio",
    "lru32_delta",
    "probe_draws",
    "probe_pso_changes",
    "probe_pso_unique",
    "probe_shader_variant_changes",
    "probe_shader_variant_unique",
    "probe_stream0_handle_changes",
    "probe_index_buffer_changes",
    "probe_extra_binding_changes",
    "probe_handle_tuple_changes",
    "probe_handle_tuple_unique",
    "probe_handle_tuple_max_run",
    "probe_handle_tuple_avg_run",
    "probe_pso_isolated_run_count",
    "probe_pso_isolated_run_top",
    "reason",
    "next_action",
]


def as_int(value: Any) -> int:
    text = str(value or "").strip().replace(",", "")
    if not text:
        return 0
    try:
        return int(float(text))
    except ValueError:
        return 0


def as_float(value: Any) -> float:
    text = str(value or "").strip().replace(",", "")
    if not text:
        return 0.0
    try:
        return float(text)
    except ValueError:
        return 0.0


def fmt_int(value: int) -> str:
    return f"{value:,}"


def fmt_float(value: float) -> str:
    return f"{value:.3f}"


def safe_ratio(numer: float, denom: float) -> float:
    return numer / denom if denom else 0.0


def row_key(row: dict[str, str]) -> str:
    return f"{row.get('seq', '')}/{row.get('encoder') or row.get('enc', '')}"


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def load_probe_draw_rows(path: Path | None) -> dict[str, list[dict[str, str]]]:
    if not path or not path.exists():
        return {}
    grouped: dict[str, list[dict[str, str]]] = {}
    for row in load_rows(path):
        grouped.setdefault(row_key(row), []).append(row)
    for rows in grouped.values():
        rows.sort(key=lambda item: as_int(item.get("encoder_draw_index")))
    return grouped


def count_changes(values: Iterable[str]) -> int:
    changes = 0
    previous = ""
    have_previous = False
    for value in values:
        if value == "":
            continue
        if have_previous and value != previous:
            changes += 1
        previous = value
        have_previous = True
    return changes


def unique_nonempty(values: Iterable[str]) -> int:
    return len({value for value in values if value})


def format_counts(counter: Counter[str], limit: int = 3) -> str:
    return ";".join(f"{key}x{count}" for key, count in counter.most_common(limit))


def handle_tuple_key(row: dict[str, str]) -> tuple[str, str, str]:
    return (
        row.get("stream0_handle", ""),
        row.get("index_buffer", ""),
        row.get("stream_extra_bindings", ""),
    )


def geometry_key(row: dict[str, str]) -> tuple[str, ...]:
    return (
        row.get("primitive_type", ""),
        row.get("primitive_count", ""),
        row.get("vertex_count", ""),
        row.get("base_vertex", ""),
        row.get("start_index", ""),
        row.get("index_type", ""),
        row.get("effective_index_offset", ""),
        row.get("effective_index_bytes", ""),
    )


def stable_runs(
    rows: list[dict[str, str]],
) -> list[tuple[int, list[dict[str, str]], tuple[str, str, str]]]:
    runs: list[tuple[int, list[dict[str, str]], tuple[str, str, str]]] = []
    current_key: tuple[str, str, str] | None = None
    current_rows: list[dict[str, str]] = []
    current_start = 0
    for index, row in enumerate(rows):
        key = handle_tuple_key(row)
        if current_key is not None and key != current_key:
            runs.append((current_start, current_rows, current_key))
            current_rows = []
            current_start = index
        current_key = key
        current_rows.append(row)
    if current_key is not None:
        runs.append((current_start, current_rows, current_key))
    return runs


def probe_run_summary(
    start: int,
    rows: list[dict[str, str]],
) -> dict[str, Any]:
    pso_values = [row.get("pso", "") for row in rows]
    shader_values = [row.get("shader_variant", "") for row in rows]
    return {
        "start": start,
        "draws": len(rows),
        "pso_changes": count_changes(pso_values),
        "pso_unique": unique_nonempty(pso_values),
        "shader_variant_changes": count_changes(shader_values),
        "shader_variant_unique": unique_nonempty(shader_values),
        "geometry_unique": len({geometry_key(row) for row in rows}) if rows else 0,
    }


def format_probe_runs(runs: list[dict[str, Any]], limit: int = 3) -> str:
    parts = []
    for run in runs[:limit]:
        parts.append(
            "d{start}+{draws}:pso{pso_changes}/{pso_unique},"
            "shader{shader_variant_changes}/{shader_variant_unique},geom{geometry_unique}".format(
                **run
            )
        )
    return ";".join(parts)


def probe_draw_metrics(
    rows: list[dict[str, str]],
    *,
    min_probe_run_draws: int,
    min_probe_run_pso_changes: int,
) -> dict[str, Any]:
    if not rows:
        return {
            "probe_draws": 0,
            "probe_pso_changes": 0,
            "probe_pso_unique": 0,
            "probe_shader_variant_changes": 0,
            "probe_shader_variant_unique": 0,
            "probe_stream0_handle_changes": 0,
            "probe_index_buffer_changes": 0,
            "probe_extra_binding_changes": 0,
            "probe_handle_tuple_changes": 0,
            "probe_handle_tuple_unique": 0,
            "probe_handle_tuple_max_run": 0,
            "probe_handle_tuple_avg_run": 0.0,
            "probe_pso_isolated_run_count": 0,
            "probe_pso_isolated_run_top": "",
        }

    pso_values = [row.get("pso", "") for row in rows]
    shader_values = [row.get("shader_variant", "") for row in rows]
    stream0_values = [row.get("stream0_handle", "") for row in rows]
    index_buffer_values = [row.get("index_buffer", "") for row in rows]
    extra_values = [row.get("stream_extra_bindings", "") for row in rows]
    handle_tuple_values = ["|".join(handle_tuple_key(row)) for row in rows]
    runs = stable_runs(rows)
    run_lengths = [len(run_rows) for _, run_rows, _ in runs]
    isolated_runs = [
        probe_run_summary(start, run_rows)
        for start, run_rows, _ in runs
        if len(run_rows) >= min_probe_run_draws
        and count_changes([row.get("pso", "") for row in run_rows]) >= min_probe_run_pso_changes
    ]
    isolated_runs.sort(
        key=lambda item: (
            as_int(item["pso_changes"]),
            as_int(item["pso_unique"]),
            as_int(item["draws"]),
        ),
        reverse=True,
    )
    return {
        "probe_draws": len(rows),
        "probe_pso_changes": count_changes(pso_values),
        "probe_pso_unique": unique_nonempty(pso_values),
        "probe_shader_variant_changes": count_changes(shader_values),
        "probe_shader_variant_unique": unique_nonempty(shader_values),
        "probe_stream0_handle_changes": count_changes(stream0_values),
        "probe_index_buffer_changes": count_changes(index_buffer_values),
        "probe_extra_binding_changes": count_changes(extra_values),
        "probe_handle_tuple_changes": count_changes(handle_tuple_values),
        "probe_handle_tuple_unique": unique_nonempty(handle_tuple_values),
        "probe_handle_tuple_max_run": max(run_lengths) if run_lengths else 0,
        "probe_handle_tuple_avg_run": (
            sum(run_lengths) / len(run_lengths)
            if run_lengths
            else 0.0
        ),
        "probe_pso_isolated_run_count": len(isolated_runs),
        "probe_pso_isolated_run_top": format_probe_runs(isolated_runs),
    }


def row_metrics(
    row: dict[str, str],
    probe_draw_rows: dict[str, list[dict[str, str]]],
    *,
    min_probe_run_draws: int,
    min_probe_run_pso_changes: int,
) -> dict[str, Any]:
    draws = as_int(row.get("draw_calls"))
    pso_changes = as_int(row.get("pso_handle_changes"))
    shader_changes = as_int(row.get("shader_variant_changes"))
    stream_changes = as_int(row.get("stream_metal_bind_handle_changes"))
    ib_changes = as_int(row.get("ib_handle_changes"))
    geometry_unique = as_int(row.get("draw_geometry_signature_unique"))
    return {
        "row": row_key(row),
        "draws": draws,
        "triangles": as_int(row.get("triangle_estimate")),
        "vertices": as_int(row.get("vertex_count")),
        "pso_changes": pso_changes,
        "pso_unique": as_int(row.get("pso_unique_handles")),
        "pso_changes_per_draw": safe_ratio(pso_changes, draws),
        "shader_variant_changes": shader_changes,
        "shader_changes_per_draw": safe_ratio(shader_changes, draws),
        "stream_handle_changes": stream_changes,
        "stream_handle_changes_per_draw": safe_ratio(stream_changes, draws),
        "ib_handle_changes": ib_changes,
        "ib_handle_changes_per_draw": safe_ratio(ib_changes, draws),
        "geometry_unique": geometry_unique,
        "geometry_unique_ratio": safe_ratio(geometry_unique, draws),
        "lru32_delta": as_int(row.get("indexed_cache_opt_candidate_miss_delta_32")),
        **probe_draw_metrics(
            probe_draw_rows.get(row_key(row), []),
            min_probe_run_draws=min_probe_run_draws,
            min_probe_run_pso_changes=min_probe_run_pso_changes,
        ),
    }


def classify(
    metrics: dict[str, Any],
    *,
    min_pso_changes_per_draw: float,
    min_pso_unique: int,
    stream_ib_dominance_ratio: float,
) -> tuple[str, str, str]:
    draws = int(metrics["draws"])
    if draws == 0:
        return "no-draws", "row has no draws", "ignore this row"

    pso_changes = int(metrics["pso_changes"])
    pso_unique = int(metrics["pso_unique"])
    pso_per_draw = float(metrics["pso_changes_per_draw"])
    stream_or_ib = max(
        int(metrics["stream_handle_changes"]),
        int(metrics["ib_handle_changes"]),
    )

    if pso_changes == 0 and pso_unique <= 1:
        return (
            "pso-stable",
            "PSO does not change within the row",
            "do not spend Xcode on PSO churn for this row",
        )

    if pso_changes > 0 and stream_or_ib >= int(pso_changes * stream_ib_dominance_ratio):
        return (
            "stream-ib-dominant",
            "stream/IB handle churn dominates PSO changes",
            "treat PSO coupling as unproven; investigate only with an isolated PSO-stable A/B",
        )

    if pso_per_draw < min_pso_changes_per_draw and pso_unique < min_pso_unique:
        return (
            "pso-not-dominant",
            "PSO churn is below the hotpath threshold",
            "do not schedule a PSO-churn Xcode run from this row",
        )

    probe_draws = int(metrics["probe_draws"])
    isolated_runs = int(metrics["probe_pso_isolated_run_count"])
    if probe_draws and isolated_runs == 0:
        return (
            "pso-coupled-to-bindings",
            "per-draw probe found no stream/IB handle-stable run where PSO changes",
            "do not spend Xcode on PSO churn until a handle-stable PSO A/B is constructed",
        )

    return (
        "candidate-pso-coupling",
        "PSO churn is high enough to justify an isolated stable-shape preflight",
        "run a no-gputrace PSO-stable/PSO-churn A/B before Xcode",
    )


def analyze_rows(
    rows: Iterable[dict[str, str]],
    probe_draw_rows: dict[str, list[dict[str, str]]],
    *,
    selected_rows: set[str],
    min_pso_changes_per_draw: float,
    min_pso_unique: int,
    stream_ib_dominance_ratio: float,
    min_probe_run_draws: int,
    min_probe_run_pso_changes: int,
    top: int,
) -> list[dict[str, str]]:
    analyzed: list[dict[str, str]] = []
    for row in rows:
        metrics = row_metrics(
            row,
            probe_draw_rows,
            min_probe_run_draws=min_probe_run_draws,
            min_probe_run_pso_changes=min_probe_run_pso_changes,
        )
        if selected_rows and metrics["row"] not in selected_rows:
            continue
        verdict, reason, action = classify(
            metrics,
            min_pso_changes_per_draw=min_pso_changes_per_draw,
            min_pso_unique=min_pso_unique,
            stream_ib_dominance_ratio=stream_ib_dominance_ratio,
        )
        analyzed.append({
            "row": str(metrics["row"]),
            "verdict": verdict,
            "draws": str(metrics["draws"]),
            "triangles": str(metrics["triangles"]),
            "vertices": str(metrics["vertices"]),
            "pso_changes": str(metrics["pso_changes"]),
            "pso_unique": str(metrics["pso_unique"]),
            "pso_changes_per_draw": fmt_float(metrics["pso_changes_per_draw"]),
            "shader_variant_changes": str(metrics["shader_variant_changes"]),
            "shader_changes_per_draw": fmt_float(metrics["shader_changes_per_draw"]),
            "stream_handle_changes": str(metrics["stream_handle_changes"]),
            "stream_handle_changes_per_draw": fmt_float(metrics["stream_handle_changes_per_draw"]),
            "ib_handle_changes": str(metrics["ib_handle_changes"]),
            "ib_handle_changes_per_draw": fmt_float(metrics["ib_handle_changes_per_draw"]),
            "geometry_unique": str(metrics["geometry_unique"]),
            "geometry_unique_ratio": fmt_float(metrics["geometry_unique_ratio"]),
            "lru32_delta": str(metrics["lru32_delta"]),
            "probe_draws": str(metrics["probe_draws"]),
            "probe_pso_changes": str(metrics["probe_pso_changes"]),
            "probe_pso_unique": str(metrics["probe_pso_unique"]),
            "probe_shader_variant_changes": str(metrics["probe_shader_variant_changes"]),
            "probe_shader_variant_unique": str(metrics["probe_shader_variant_unique"]),
            "probe_stream0_handle_changes": str(metrics["probe_stream0_handle_changes"]),
            "probe_index_buffer_changes": str(metrics["probe_index_buffer_changes"]),
            "probe_extra_binding_changes": str(metrics["probe_extra_binding_changes"]),
            "probe_handle_tuple_changes": str(metrics["probe_handle_tuple_changes"]),
            "probe_handle_tuple_unique": str(metrics["probe_handle_tuple_unique"]),
            "probe_handle_tuple_max_run": str(metrics["probe_handle_tuple_max_run"]),
            "probe_handle_tuple_avg_run": fmt_float(metrics["probe_handle_tuple_avg_run"]),
            "probe_pso_isolated_run_count": str(metrics["probe_pso_isolated_run_count"]),
            "probe_pso_isolated_run_top": str(metrics["probe_pso_isolated_run_top"]),
            "reason": reason,
            "next_action": action,
        })
    analyzed.sort(
        key=lambda item: (
            item["verdict"] == "candidate-pso-coupling",
            as_int(item["triangles"]),
            as_int(item["pso_changes"]),
        ),
        reverse=True,
    )
    return analyzed[:top] if not selected_rows else analyzed


def overall_verdict(rows: list[dict[str, str]]) -> tuple[str, str]:
    candidate_count = sum(1 for row in rows if row["verdict"] == "candidate-pso-coupling")
    if candidate_count:
        return (
            "candidate-pso-coupling",
            f"{candidate_count} row(s) need an isolated PSO-stable/PSO-churn no-gputrace A/B before Xcode",
        )
    stream_rows = [row for row in rows if row["verdict"] == "stream-ib-dominant"]
    if stream_rows:
        return (
            "no-pso-xcode-candidate",
            "hot rows show stream/IB churn dominating PSO changes; PSO/backend coupling is not isolated",
        )
    coupled_rows = [row for row in rows if row["verdict"] == "pso-coupled-to-bindings"]
    if coupled_rows:
        return (
            "no-pso-xcode-candidate",
            "per-draw probe rows show PSO changes coupled to stream/IB handle-tuple motion",
        )
    return (
        "no-pso-xcode-candidate",
        "no row crosses the PSO churn threshold",
    )


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_markdown(
    path: Path,
    encoder_csv: Path,
    probe_draws_csv: Path | None,
    rows: list[dict[str, str]],
) -> None:
    verdict, reason = overall_verdict(rows)
    lines = [
        "# PSO Backend Churn Preflight",
        "",
        f"- Encoder source: `{encoder_csv}`",
        f"- Probe draws source: `{probe_draws_csv}`" if probe_draws_csv else "- Probe draws source: `(not provided)`",
        f"- Overall: `{verdict}`",
        f"- Reason: {reason}",
        "",
        "| Row | Verdict | Draws | Triangles | PSO changes | PSO unique | PSO/draw | Shader/draw | Stream handle/draw | IB handle/draw | Geom unique % | LRU32 delta | Next action |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for row in rows:
        geom_unique_pct = as_float(row["geometry_unique_ratio"]) * 100.0
        lines.append(
            "| "
            + " | ".join([
                f"`{row['row']}`",
                f"`{row['verdict']}`",
                fmt_int(as_int(row["draws"])),
                fmt_int(as_int(row["triangles"])),
                fmt_int(as_int(row["pso_changes"])),
                fmt_int(as_int(row["pso_unique"])),
                row["pso_changes_per_draw"],
                row["shader_changes_per_draw"],
                row["stream_handle_changes_per_draw"],
                row["ib_handle_changes_per_draw"],
                f"{geom_unique_pct:.2f}%",
                fmt_int(as_int(row["lru32_delta"])),
                row["next_action"],
            ])
            + " |"
        )
    if any(as_int(row["probe_draws"]) for row in rows):
        lines.extend([
            "",
            "## Per-draw PSO Isolation",
            "",
            "| Row | Probe draws | Probe PSO changes | Probe PSO unique | Probe shader changes | Stream0 changes | IB changes | Extra changes | Handle tuple changes | Unique tuples | Max tuple run | Avg tuple run | PSO-isolated runs | Top isolated runs |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|",
        ])
        for row in rows:
            lines.append(
                "| "
                + " | ".join([
                    f"`{row['row']}`",
                    fmt_int(as_int(row["probe_draws"])),
                    fmt_int(as_int(row["probe_pso_changes"])),
                    fmt_int(as_int(row["probe_pso_unique"])),
                    fmt_int(as_int(row["probe_shader_variant_changes"])),
                    fmt_int(as_int(row["probe_stream0_handle_changes"])),
                    fmt_int(as_int(row["probe_index_buffer_changes"])),
                    fmt_int(as_int(row["probe_extra_binding_changes"])),
                    fmt_int(as_int(row["probe_handle_tuple_changes"])),
                    fmt_int(as_int(row["probe_handle_tuple_unique"])),
                    fmt_int(as_int(row["probe_handle_tuple_max_run"])),
                    row["probe_handle_tuple_avg_run"],
                    fmt_int(as_int(row["probe_pso_isolated_run_count"])),
                    f"`{row['probe_pso_isolated_run_top']}`",
                ])
                + " |"
            )
    lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("encoder_csv", type=Path)
    parser.add_argument("--probe-draws", type=Path)
    parser.add_argument("--row", action="append", default=[], help="Optional SEQ/ENC filter")
    parser.add_argument("--top", type=int, default=12)
    parser.add_argument("--min-pso-changes-per-draw", type=float, default=0.5)
    parser.add_argument("--min-pso-unique", type=int, default=8)
    parser.add_argument("--stream-ib-dominance-ratio", type=float, default=2.0)
    parser.add_argument("--min-probe-run-draws", type=int, default=4)
    parser.add_argument("--min-probe-run-pso-changes", type=int, default=1)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--csv-output", type=Path)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    probe_draws_csv = args.probe_draws
    if probe_draws_csv is None:
        candidate = args.encoder_csv.with_name("3dmark05-perf-indexed-probe-draws.csv")
        probe_draws_csv = candidate if candidate.exists() else None
    rows = analyze_rows(
        load_rows(args.encoder_csv),
        load_probe_draw_rows(probe_draws_csv),
        selected_rows=set(args.row),
        min_pso_changes_per_draw=args.min_pso_changes_per_draw,
        min_pso_unique=args.min_pso_unique,
        stream_ib_dominance_ratio=args.stream_ib_dominance_ratio,
        min_probe_run_draws=args.min_probe_run_draws,
        min_probe_run_pso_changes=args.min_probe_run_pso_changes,
        top=args.top,
    )
    if args.output:
        write_markdown(args.output, args.encoder_csv, probe_draws_csv, rows)
    if args.csv_output:
        write_csv(args.csv_output, rows)
    if not args.output and not args.csv_output:
        for row in rows:
            print(
                f"{row['row']} {row['verdict']} draws={row['draws']} "
                f"tri={row['triangles']} pso/draw={row['pso_changes_per_draw']} "
                f"stream/draw={row['stream_handle_changes_per_draw']} "
                f"ib/draw={row['ib_handle_changes_per_draw']}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
