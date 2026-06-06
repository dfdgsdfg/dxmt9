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
import re
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
        text = str(value).strip()
        if not text:
            return 0
        try:
            return int(text, 0)
        except ValueError:
            return int(float(text))
    except (TypeError, ValueError):
        return 0


def as_bool(value: Any) -> bool:
    return as_int(value) != 0


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


def parse_class_filters(values: list[str]) -> list[str]:
    filters: list[str] = []
    aliases = {
        "opaque": "opaque-depth-write",
        "opaque-depth": "opaque-depth-write",
        "non-opaque": "nonopaque",
        "blend": "alpha-blend",
        "blend-off": "no-alpha-blend",
        "no-blend": "no-alpha-blend",
        "texture": "textured",
        "large-4096": "large4096",
    }
    known = {
        "opaque-depth-write",
        "nonopaque",
        "depth-read",
        "alpha-blend",
        "no-alpha-blend",
        "screen-blend",
        "standard-alpha",
        "additive-alpha",
        "scissor",
        "no-scissor",
        "textured",
        "large4096",
    }
    for value in values:
        for item in re.split(r"[,;\s+&]+", value):
            item = item.strip().lower().replace("_", "-")
            if not item or item == "any":
                continue
            item = aliases.get(item, item)
            if item not in known:
                raise SystemExit(f"unsupported class filter: {item}")
            filters.append(item)
    return filters


def blend_equation_matches(row: dict[str, str], src: int, dst: int, op: int) -> bool:
    return (
        as_bool(row.get("alpha_blend"))
        and as_int(row.get("src_blend")) == src
        and as_int(row.get("dst_blend")) == dst
        and as_int(row.get("blend_op")) == op
        and not as_bool(row.get("separate_alpha"))
    )


def opaque_depth_write(row: dict[str, str]) -> bool:
    return (
        as_bool(row.get("depth_enabled"))
        and as_bool(row.get("depth_write"))
        and as_int(row.get("depth_func")) in (2, 4)
        and not as_bool(row.get("alpha_blend"))
        and not as_bool(row.get("alpha_test"))
        and not as_bool(row.get("stencil"))
        and not as_bool(row.get("clip_plane"))
    )


def row_matches_class_filter(row: dict[str, str], class_filter: str) -> bool:
    depth_enabled = as_bool(row.get("depth_enabled"))
    depth_write = depth_enabled and as_bool(row.get("depth_write"))
    alpha_blend = as_bool(row.get("alpha_blend"))
    if class_filter == "opaque-depth-write":
        return opaque_depth_write(row)
    if class_filter == "nonopaque":
        return not opaque_depth_write(row)
    if class_filter == "depth-read":
        return depth_enabled and not depth_write
    if class_filter == "alpha-blend":
        return alpha_blend
    if class_filter == "no-alpha-blend":
        return not alpha_blend
    if class_filter == "screen-blend":
        return blend_equation_matches(row, 10, 2, 1)
    if class_filter == "standard-alpha":
        return blend_equation_matches(row, 5, 6, 1)
    if class_filter == "additive-alpha":
        return blend_equation_matches(row, 5, 2, 1)
    if class_filter == "scissor":
        return as_bool(row.get("scissor"))
    if class_filter == "no-scissor":
        return not as_bool(row.get("scissor"))
    if class_filter == "textured":
        return as_int(row.get("texture_mask")) != 0
    if class_filter == "large4096":
        return as_int(row.get("primitive_count")) >= 4096
    return True


def row_matches_class_filters(row: dict[str, str], class_filters: list[str]) -> bool:
    return all(row_matches_class_filter(row, item) for item in class_filters)


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


def candidate_miss32_available(row: dict[str, str]) -> bool:
    if "candidate_index_available" in row:
        return as_bool(row.get("candidate_index_available"))
    return bool(str(row.get("candidate_cache_miss32", "")).strip())


def candidate_miss32(row: dict[str, str]) -> int:
    if candidate_miss32_available(row):
        return as_int(row.get("candidate_cache_miss32"))
    return as_int(row.get("original_cache_miss32"))


def candidate_miss32_delta(rows: list[dict[str, str]]) -> int:
    return sum(
        as_int(row.get("original_cache_miss32")) - candidate_miss32(row)
        for row in rows
    )


def miss32_summary(rows: list[dict[str, str]]) -> dict[str, int]:
    original = sum(as_int(row.get("original_cache_miss32")) for row in rows)
    candidate = sum(candidate_miss32(row) for row in rows)
    return {
        "candidate_available_rows": sum(1 for row in rows if candidate_miss32_available(row)),
        "original_miss32": original,
        "candidate_miss32": candidate,
        "candidate_miss32_delta": original - candidate,
    }


def score_rows(rows: list[dict[str, str]], rank_by: str) -> tuple[int, int, int]:
    draws = len(rows)
    primitives = sum(as_int(row.get("primitive_count")) for row in rows)
    cache64 = sum(as_int(row.get("original_cache_miss64")) for row in rows)
    if rank_by == "draws":
        return draws, primitives, cache64
    if rank_by == "cache64":
        return cache64, primitives, draws
    if rank_by == "candidate-miss32-delta":
        return candidate_miss32_delta(rows), primitives, draws
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


def rank_group_items(
    args: argparse.Namespace,
    groups: dict[tuple[str, ...], list[dict[str, str]]],
) -> list[tuple[tuple[str, ...], list[dict[str, str]]]]:
    def ranking_score(item: tuple[tuple[str, ...], list[dict[str, str]]]) -> tuple[int, int, int]:
        _, rows = item
        if args.rank_scope == "window":
            window_rows = best_window(rows, args.max_draws, args.rank_by)
            if not window_rows:
                return (-1, -1, -1)
            return score_rows(window_rows, args.rank_by)
        return score_rows(rows, args.rank_by)

    return sorted(groups.items(), key=ranking_score, reverse=True)


def matching_rows(args: argparse.Namespace, class_filters: list[str]) -> list[dict[str, str]]:
    rows = [
        row for row in load_csv(args.probe_draws)
        if row_key(row) == args.row and as_int(row.get("primitive_count")) > 0
        and row_matches_class_filters(row, class_filters)
        and (not args.applied_only or as_bool(row.get("applied")))
    ]
    if not rows:
        class_suffix = (
            f" and class filters {','.join(class_filters)}"
            if class_filters else ""
        )
        applied_suffix = " and applied=1" if args.applied_only else ""
        raise SystemExit(f"no probe rows matched row {args.row}{class_suffix}{applied_suffix}")
    return rows


def build_selection_from_rank(
    args: argparse.Namespace,
    class_filters: list[str],
    ranked_groups: list[tuple[tuple[str, ...], list[dict[str, str]]]],
    rank: int,
) -> dict[str, Any]:
    if rank < 1 or rank > len(ranked_groups):
        raise SystemExit(f"group rank {rank} is out of range; found {len(ranked_groups)} groups")

    key, selected_rows = ranked_groups[rank - 1]
    window_rows = best_window(selected_rows, args.max_draws, args.rank_by)
    if not window_rows:
        raise SystemExit("selected group has no contiguous encoder-draw window")

    min_draw = as_int(window_rows[0].get("encoder_draw_index"))
    max_draw = as_int(window_rows[-1].get("encoder_draw_index"))
    group_score = score_rows(selected_rows, args.rank_by)
    window_score = score_rows(window_rows, args.rank_by)
    summary = key_to_summary(key, args.group_by)
    group_miss32 = miss32_summary(selected_rows)
    window_miss32 = miss32_summary(window_rows)

    return {
        "schema": "dxmt9.3dmark05.payload_window.v1",
        "sources": {"probe_draws": str(args.probe_draws)},
        "selection": {
            "row": args.row,
            "group_by": args.group_by,
            "rank_by": args.rank_by,
            "rank_scope": args.rank_scope,
            "rank": rank,
            "available_group_count": len(ranked_groups),
            "class_filters": class_filters,
            "applied_only": bool(args.applied_only),
            "group": {
                **summary,
                "draws": len(selected_rows),
                "primitive_count": sum(as_int(row.get("primitive_count")) for row in selected_rows),
                "cache_miss64": sum(as_int(row.get("original_cache_miss64")) for row in selected_rows),
                **group_miss32,
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
                **window_miss32,
                "draw_ordinals": [as_int(row.get("draw_ordinal")) for row in window_rows],
            },
            "capture_flags": [
                "--dump-indexed-geometry",
                "--dump-indexed-geometry-max-draws",
                str(len(window_rows)),
                "--probe-reverse-indexed-triangles-row",
                args.row,
                *(
                    [
                        "--probe-reverse-indexed-triangles-classes",
                        ",".join(class_filters),
                    ]
                    if class_filters else []
                ),
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


def build_selection(args: argparse.Namespace) -> dict[str, Any]:
    class_filters = parse_class_filters(args.class_filter)
    rows = matching_rows(args, class_filters)
    groups = grouped_rows(rows, args.group_by)
    ranked_groups = rank_group_items(args, groups)
    return build_selection_from_rank(args, class_filters, ranked_groups, args.rank)


def build_selection_list(args: argparse.Namespace) -> dict[str, Any]:
    class_filters = parse_class_filters(args.class_filter)
    rows = matching_rows(args, class_filters)
    groups = grouped_rows(rows, args.group_by)
    ranked_groups = rank_group_items(args, groups)
    limit = min(args.list_ranks, len(ranked_groups))
    return {
        "schema": "dxmt9.3dmark05.payload_window_list.v1",
        "sources": {"probe_draws": str(args.probe_draws)},
        "selection_count": limit,
        "available_group_count": len(ranked_groups),
        "selections": [
            build_selection_from_rank(args, class_filters, ranked_groups, rank)["selection"]
            for rank in range(1, limit + 1)
        ],
    }


def print_human(selection: dict[str, Any]) -> None:
    if "selections" in selection:
        for item in selection["selections"]:
            print_human({"selection": item})
        return
    data = selection["selection"]
    group = data["group"]
    window = data["window"]
    print(
        f"{data['row']} rank {data['rank']} {data['group_by']} group: "
        f"{group['draws']} draws, {group['primitive_count']} tris, "
        f"cache64 {group['cache_miss64']}, "
        f"candidate32_delta {group.get('candidate_miss32_delta', 0)}, "
        f"vs {short_hash(group.get('vs', ''))}, ps {short_hash(group.get('ps', ''))}"
    )
    print(
        f"window: encoder_draw_index {window['encoder_draw_min']}.."
        f"{window['encoder_draw_max']} ({window['draws']} draws, "
        f"{window['primitive_count']} tris, cache64 {window['cache_miss64']}, "
        f"candidate32_delta {window.get('candidate_miss32_delta', 0)})"
    )
    print("flags: " + " ".join(data["capture_flags"]))
    if data.get("class_filters"):
        print("class_filters: " + ",".join(data["class_filters"]))
    if data.get("applied_only"):
        print("applied_only: true")
    if data.get("shader_capture_flags"):
        print("shader_flags: " + " ".join(data["shader_capture_flags"]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe-draws", type=Path, required=True)
    parser.add_argument("--row", required=True, help="Target seq/encoder row, e.g. 60/2")
    parser.add_argument(
        "--class-filter",
        "--class",
        action="append",
        default=[],
        help=(
            "AND-list indexed triangle class filter. Accepts comma/space/+/& "
            "separated values such as depth-read,no-alpha-blend,no-scissor,textured"
        ),
    )
    parser.add_argument(
        "--applied-only",
        action="store_true",
        help="Only select rows where the mutating probe actually applied",
    )
    parser.add_argument("--rank", type=int, default=1, help="Ranked group to select, 1-based")
    parser.add_argument(
        "--list-ranks",
        type=int,
        default=0,
        help="Emit the top N ranked payload windows in one JSON object",
    )
    parser.add_argument("--max-draws", type=int, default=3, help="Max contiguous draws in capture window")
    parser.add_argument(
        "--group-by",
        choices=["shader-state", "shader", "state"],
        default="shader-state",
        help="How to group draw rows before ranking",
    )
    parser.add_argument(
        "--rank-by",
        choices=["primitives", "cache64", "draws", "candidate-miss32-delta"],
        default="primitives",
        help="Primary ranking metric for groups and candidate windows",
    )
    parser.add_argument(
        "--rank-scope",
        choices=["group", "window"],
        default="group",
        help="Rank groups by whole-group score or by their best capture window score",
    )
    parser.add_argument("--output", type=Path, help="Optional JSON output path")
    args = parser.parse_args()

    selection = build_selection_list(args) if args.list_ranks > 0 else build_selection(args)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(selection, indent=2, sort_keys=True), encoding="utf-8")
    print_human(selection)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
