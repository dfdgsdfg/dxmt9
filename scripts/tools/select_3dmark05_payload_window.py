#!/usr/bin/env python3
"""Select a same-run 3DMark05 indexed geometry payload capture window.

The indexed probe CSV records row-local draw identity, shader hashes, render
state, and index locality. This helper ranks shader/state groups inside one
`seq/encoder` row and emits a contiguous encoder-draw window suitable for
`run_3dmark05_perf_probe.sh --dump-indexed-geometry`.

It exists because row-local draw ordering can shift between 3DMark05 runs. A
window such as `234..236` is only reliable for the same probe CSV that selected
it.
"""

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path
from typing import Any


STATE_FIELDS = [
    "alpha_blend",
    "src_blend",
    "dst_blend",
    "blend_op",
    "separate_alpha",
    "src_blend_alpha",
    "dst_blend_alpha",
    "blend_op_alpha",
    "alpha_test",
    "depth_enabled",
    "depth_write",
    "depth_func",
    "stencil",
    "clip_plane",
    "scissor",
    "cull",
    "fill",
    "texture_mask",
    "color_write",
    "index_type",
    "stream0_stride",
]


def as_int(value: Any) -> int:
    try:
        text = str(value)
        if text.startswith(("0x", "0X")):
            return int(text, 16)
        return int(float(text))
    except (TypeError, ValueError):
        return 0


def load_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing probe CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def row_key(row: dict[str, str]) -> str:
    seq = row.get("seq", "")
    enc = row.get("enc", row.get("encoder", ""))
    if seq == "" or enc == "":
        return ""
    return f"{seq}/{enc}"


def short_hash(value: str) -> str:
    if not value:
        return ""
    return value if len(value) <= 18 else value[:18]


def group_key(row: dict[str, str], mode: str) -> tuple[str, ...]:
    shader = (row.get("vs", ""), row.get("ps", ""))
    state = tuple(row.get(field, "") for field in STATE_FIELDS)
    if mode == "shader":
        return shader
    if mode == "state":
        return state
    return shader + state


def key_to_summary(key: tuple[str, ...], mode: str) -> dict[str, Any]:
    if mode == "shader":
        return {"vs": key[0], "ps": key[1], "state": {}}
    if mode == "state":
        return {"vs": "", "ps": "", "state": dict(zip(STATE_FIELDS, key))}
    return {
        "vs": key[0],
        "ps": key[1],
        "state": dict(zip(STATE_FIELDS, key[2:])),
    }


def score_rows(rows: list[dict[str, str]], rank_by: str) -> tuple[int, int, int]:
    draws = len(rows)
    primitives = sum(as_int(row.get("primitive_count")) for row in rows)
    cache64 = sum(as_int(row.get("original_cache_miss64")) for row in rows)
    if rank_by == "draws":
        return draws, primitives, cache64
    if rank_by == "cache64":
        return cache64, primitives, draws
    return primitives, cache64, draws


def contiguous_runs(rows: list[dict[str, str]]) -> list[list[dict[str, str]]]:
    ordered = sorted(rows, key=lambda row: as_int(row.get("encoder_draw_index")))
    runs: list[list[dict[str, str]]] = []
    current: list[dict[str, str]] = []
    previous_index: int | None = None
    for row in ordered:
        index = as_int(row.get("encoder_draw_index"))
        if previous_index is None or index == previous_index + 1:
            current.append(row)
        else:
            if current:
                runs.append(current)
            current = [row]
        previous_index = index
    if current:
        runs.append(current)
    return runs


def best_window(
    rows: list[dict[str, str]],
    max_draws: int,
    rank_by: str,
) -> list[dict[str, str]]:
    best: list[dict[str, str]] = []
    best_score: tuple[int, int, int] = (-1, -1, -1)
    for run in contiguous_runs(rows):
        width = len(run) if max_draws <= 0 else min(max_draws, len(run))
        if width <= 0:
            continue
        for start in range(0, len(run) - width + 1):
            candidate = run[start:start + width]
            score = score_rows(candidate, rank_by)
            if score > best_score:
                best = candidate
                best_score = score
    return best


def grouped_rows(rows: list[dict[str, str]], mode: str) -> dict[tuple[str, ...], list[dict[str, str]]]:
    groups: dict[tuple[str, ...], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if as_int(row.get("encoder_draw_index")) < 0:
            continue
        groups[group_key(row, mode)].append(row)
    return groups


def build_selection(args: argparse.Namespace) -> dict[str, Any]:
    rows = [
        row for row in load_csv(args.probe_draws)
        if row_key(row) == args.row and as_int(row.get("primitive_count")) > 0
    ]
    if not rows:
        raise SystemExit(f"no probe rows matched row {args.row}")

    groups = grouped_rows(rows, args.group_by)
    ranked_groups = sorted(
        groups.items(),
        key=lambda item: score_rows(item[1], args.rank_by),
        reverse=True,
    )
    if args.rank < 1 or args.rank > len(ranked_groups):
        raise SystemExit(f"group rank {args.rank} is out of range; found {len(ranked_groups)} groups")

    key, selected_rows = ranked_groups[args.rank - 1]
    window_rows = best_window(selected_rows, args.max_draws, args.rank_by)
    if not window_rows:
        raise SystemExit("selected group has no contiguous encoder-draw window")

    min_draw = as_int(window_rows[0].get("encoder_draw_index"))
    max_draw = as_int(window_rows[-1].get("encoder_draw_index"))
    group_score = score_rows(selected_rows, args.rank_by)
    window_score = score_rows(window_rows, args.rank_by)
    summary = key_to_summary(key, args.group_by)

    return {
        "schema": "dxmt9.3dmark05.payload_window.v1",
        "sources": {"probe_draws": str(args.probe_draws)},
        "selection": {
            "row": args.row,
            "group_by": args.group_by,
            "rank_by": args.rank_by,
            "rank": args.rank,
            "group": {
                **summary,
                "draws": len(selected_rows),
                "primitive_count": sum(as_int(row.get("primitive_count")) for row in selected_rows),
                "cache_miss64": sum(as_int(row.get("original_cache_miss64")) for row in selected_rows),
                "contiguous_runs": len(contiguous_runs(selected_rows)),
                "longest_contiguous_run": max(len(run) for run in contiguous_runs(selected_rows)),
            },
            "window": {
                "encoder_draw_min": min_draw,
                "encoder_draw_max": max_draw,
                "draws": len(window_rows),
                "primitive_count": window_score[0] if args.rank_by == "primitives" else sum(
                    as_int(row.get("primitive_count")) for row in window_rows),
                "cache_miss64": sum(as_int(row.get("original_cache_miss64")) for row in window_rows),
                "draw_ordinals": [as_int(row.get("draw_ordinal")) for row in window_rows],
            },
            "capture_flags": [
                "--dump-indexed-geometry",
                "--dump-indexed-geometry-max-draws",
                str(len(window_rows)),
                "--probe-reverse-indexed-triangles-row",
                args.row,
                "--probe-indexed-triangle-encoder-draw-min",
                str(min_draw),
                "--probe-indexed-triangle-encoder-draw-max",
                str(max_draw),
            ],
            "shader_capture_flags": [
                flag for flag in [
                    "--dump-indexed-geometry",
                    "--dump-indexed-geometry-max-draws",
                    str(len(window_rows)),
                    "--dump-indexed-geometry-vs" if summary.get("vs") else "",
                    summary.get("vs", ""),
                    "--dump-indexed-geometry-ps" if summary.get("ps") else "",
                    summary.get("ps", ""),
                ] if flag
            ],
            "group_score": {
                "primary": group_score[0],
                "secondary": group_score[1],
                "tertiary": group_score[2],
            },
        },
    }


def print_human(selection: dict[str, Any]) -> None:
    data = selection["selection"]
    group = data["group"]
    window = data["window"]
    print(
        f"{data['row']} rank {data['rank']} {data['group_by']} group: "
        f"{group['draws']} draws, {group['primitive_count']} tris, "
        f"cache64 {group['cache_miss64']}, "
        f"vs {short_hash(group.get('vs', ''))}, ps {short_hash(group.get('ps', ''))}"
    )
    print(
        f"window: encoder_draw_index {window['encoder_draw_min']}.."
        f"{window['encoder_draw_max']} ({window['draws']} draws, "
        f"{window['primitive_count']} tris, cache64 {window['cache_miss64']})"
    )
    print("flags: " + " ".join(data["capture_flags"]))
    if data.get("shader_capture_flags"):
        print("shader_flags: " + " ".join(data["shader_capture_flags"]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe-draws", type=Path, required=True)
    parser.add_argument("--row", required=True, help="Target seq/encoder row, e.g. 60/2")
    parser.add_argument("--rank", type=int, default=1, help="Ranked group to select, 1-based")
    parser.add_argument("--max-draws", type=int, default=3, help="Max contiguous draws in capture window")
    parser.add_argument(
        "--group-by",
        choices=["shader-state", "shader", "state"],
        default="shader-state",
        help="How to group draw rows before ranking",
    )
    parser.add_argument(
        "--rank-by",
        choices=["primitives", "cache64", "draws"],
        default="primitives",
        help="Primary ranking metric for groups and candidate windows",
    )
    parser.add_argument("--output", type=Path, help="Optional JSON output path")
    args = parser.parse_args()

    selection = build_selection(args)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(selection, indent=2, sort_keys=True), encoding="utf-8")
    print_human(selection)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
