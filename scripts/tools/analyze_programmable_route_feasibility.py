#!/usr/bin/env python3
"""Classify GT1 hot rows for programmable/textured backend-route feasibility."""

from __future__ import annotations

import argparse
import csv
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable


CSV_FIELDS = [
    "row",
    "verdict",
    "draws",
    "primitives",
    "vertices",
    "unique_vs",
    "unique_ps",
    "unique_pso",
    "unique_shader_variants",
    "color_write_zero_draws",
    "color_write_zero_primitives",
    "color_write_nonzero_draws",
    "color_write_nonzero_primitives",
    "depth_write_draws",
    "depth_write_primitives",
    "alpha_blend_draws",
    "alpha_blend_primitives",
    "alpha_test_draws",
    "alpha_test_primitives",
    "textured_draws",
    "textured_primitives",
    "depth_only_candidate_draws",
    "depth_only_candidate_primitives",
    "programmable_color_draws",
    "programmable_color_primitives",
    "programmable_textured_draws",
    "programmable_textured_primitives",
    "top_ps",
    "top_texture_masks",
    "reason",
    "next_action",
]


def as_int(value: Any) -> int:
    text = str(value or "").strip().replace(",", "")
    if not text:
        return 0
    try:
        return int(text, 0)
    except ValueError:
        try:
            return int(float(text))
        except ValueError:
            return 0


def fmt_int(value: int) -> str:
    return f"{value:,}"


def safe_pct(numer: int, denom: int) -> float:
    return float(numer) / float(denom) * 100.0 if denom else 0.0


def fmt_pct(numer: int, denom: int) -> str:
    return f"{safe_pct(numer, denom):.2f}%"


def row_key(row: dict[str, str]) -> str:
    return f"{row.get('seq', '')}/{row.get('encoder') or row.get('enc', '')}"


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def is_textured(row: dict[str, str]) -> bool:
    return as_int(row.get("texture_mask")) != 0


def is_depth_only_candidate(row: dict[str, str]) -> bool:
    return (
        as_int(row.get("depth_enabled")) != 0
        and as_int(row.get("depth_write")) != 0
        and as_int(row.get("color_write")) == 0
        and as_int(row.get("alpha_blend")) == 0
        and as_int(row.get("alpha_test")) == 0
    )


def draw_bucket(row: dict[str, str]) -> str:
    if is_depth_only_candidate(row):
        return "depth-only"
    if is_textured(row):
        return "programmable-textured"
    return "programmable-color"


def format_counts(counter: Counter[str], limit: int = 4) -> str:
    return ";".join(f"{key}x{count}" for key, count in counter.most_common(limit))


def summarize_group(key: str, rows: list[dict[str, str]]) -> dict[str, str]:
    primitives = sum(as_int(row.get("primitive_count")) for row in rows)
    vertices = sum(as_int(row.get("vertex_count")) for row in rows)
    color_zero_prims = 0
    color_nonzero_prims = 0
    depth_write_prims = 0
    alpha_blend_prims = 0
    alpha_test_prims = 0
    textured_prims = 0
    depth_only_prims = 0
    programmable_color_prims = 0
    programmable_textured_prims = 0
    color_zero_draws = color_nonzero_draws = 0
    depth_write_draws = alpha_blend_draws = alpha_test_draws = 0
    textured_draws = depth_only_draws = 0
    programmable_color_draws = programmable_textured_draws = 0
    vs: set[str] = set()
    ps: set[str] = set()
    pso: set[str] = set()
    variants: set[str] = set()
    top_ps: Counter[str] = Counter()
    top_textures: Counter[str] = Counter()

    for row in rows:
        prim = as_int(row.get("primitive_count"))
        if row.get("vs"):
            vs.add(row["vs"])
        if row.get("ps"):
            ps.add(row["ps"])
            top_ps[row["ps"]] += prim
        if row.get("pso"):
            pso.add(row["pso"])
        if row.get("shader_variant"):
            variants.add(row["shader_variant"])
        texture_mask = row.get("texture_mask", "")
        if texture_mask:
            top_textures[texture_mask] += prim
        if as_int(row.get("color_write")) == 0:
            color_zero_draws += 1
            color_zero_prims += prim
        else:
            color_nonzero_draws += 1
            color_nonzero_prims += prim
        if as_int(row.get("depth_write")) != 0:
            depth_write_draws += 1
            depth_write_prims += prim
        if as_int(row.get("alpha_blend")) != 0:
            alpha_blend_draws += 1
            alpha_blend_prims += prim
        if as_int(row.get("alpha_test")) != 0:
            alpha_test_draws += 1
            alpha_test_prims += prim
        if is_textured(row):
            textured_draws += 1
            textured_prims += prim
        bucket = draw_bucket(row)
        if bucket == "depth-only":
            depth_only_draws += 1
            depth_only_prims += prim
        elif bucket == "programmable-textured":
            programmable_textured_draws += 1
            programmable_textured_prims += prim
        else:
            programmable_color_draws += 1
            programmable_color_prims += prim

    verdict, reason, action = classify_group(
        primitives=primitives,
        depth_only_prims=depth_only_prims,
        programmable_textured_prims=programmable_textured_prims,
        programmable_color_prims=programmable_color_prims,
        alpha_blend_prims=alpha_blend_prims,
        alpha_test_prims=alpha_test_prims,
    )
    return {
        "row": key,
        "verdict": verdict,
        "draws": str(len(rows)),
        "primitives": str(primitives),
        "vertices": str(vertices),
        "unique_vs": str(len(vs)),
        "unique_ps": str(len(ps)),
        "unique_pso": str(len(pso)),
        "unique_shader_variants": str(len(variants)),
        "color_write_zero_draws": str(color_zero_draws),
        "color_write_zero_primitives": str(color_zero_prims),
        "color_write_nonzero_draws": str(color_nonzero_draws),
        "color_write_nonzero_primitives": str(color_nonzero_prims),
        "depth_write_draws": str(depth_write_draws),
        "depth_write_primitives": str(depth_write_prims),
        "alpha_blend_draws": str(alpha_blend_draws),
        "alpha_blend_primitives": str(alpha_blend_prims),
        "alpha_test_draws": str(alpha_test_draws),
        "alpha_test_primitives": str(alpha_test_prims),
        "textured_draws": str(textured_draws),
        "textured_primitives": str(textured_prims),
        "depth_only_candidate_draws": str(depth_only_draws),
        "depth_only_candidate_primitives": str(depth_only_prims),
        "programmable_color_draws": str(programmable_color_draws),
        "programmable_color_primitives": str(programmable_color_prims),
        "programmable_textured_draws": str(programmable_textured_draws),
        "programmable_textured_primitives": str(programmable_textured_prims),
        "top_ps": format_counts(top_ps),
        "top_texture_masks": format_counts(top_textures),
        "reason": reason,
        "next_action": action,
    }


def classify_group(
    *,
    primitives: int,
    depth_only_prims: int,
    programmable_textured_prims: int,
    programmable_color_prims: int,
    alpha_blend_prims: int,
    alpha_test_prims: int,
) -> tuple[str, str, str]:
    if primitives == 0:
        return "no-primitives", "row has no primitives", "ignore this row"
    depth_share = safe_pct(depth_only_prims, primitives)
    textured_share = safe_pct(programmable_textured_prims, primitives)
    color_share = safe_pct(programmable_color_prims, primitives)
    blend_share = safe_pct(alpha_blend_prims, primitives)
    alpha_test_share = safe_pct(alpha_test_prims, primitives)
    if depth_share >= 80.0:
        return (
            "candidate-depth-only-route",
            f"depth-only candidate covers {depth_share:.2f}% of primitives",
            "prototype a depth-only programmable/binning route and prove depth/color equality before counters",
        )
    if textured_share >= 50.0:
        return (
            "needs-programmable-textured-route",
            f"programmable textured draws cover {textured_share:.2f}% of primitives",
            "a tile/mesh route must support texture sampling or preserve the existing fragment path",
        )
    if color_share >= 50.0:
        return (
            "needs-programmable-color-route",
            f"programmable non-textured color draws cover {color_share:.2f}% of primitives",
            "prototype a reduced programmable color route before GT1 Xcode",
        )
    if blend_share >= 10.0 or alpha_test_share >= 10.0:
        return (
            "order-dependent-fragment-route",
            f"alpha blend {blend_share:.2f}% / alpha test {alpha_test_share:.2f}% of primitives",
            "require final-color/final-writer proof before changing this route",
        )
    return (
        "mixed-programmable-route",
        "no single programmable route class dominates",
        "split the row into narrower draw classes before implementing a backend route",
    )


def analyze_rows(
    rows: Iterable[dict[str, str]],
    *,
    selected_rows: set[str],
    selected_seq: set[str],
) -> list[dict[str, str]]:
    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        key = row_key(row)
        if selected_rows and key not in selected_rows:
            continue
        if selected_seq and row.get("seq", "") not in selected_seq:
            continue
        grouped[key].append(row)
    result = [summarize_group(key, group) for key, group in grouped.items()]
    result.sort(key=lambda row: as_int(row["primitives"]), reverse=True)
    return result


def overall_verdict(rows: list[dict[str, str]]) -> tuple[str, str]:
    if not rows:
        return "missing-input", "no rows matched the selected filters"
    counts = Counter(row["verdict"] for row in rows)
    if counts.get("needs-programmable-textured-route"):
        return (
            "needs-programmable-textured-route",
            f"{counts['needs-programmable-textured-route']} row(s) are dominated by programmable textured draws",
        )
    if counts.get("candidate-depth-only-route"):
        return (
            "candidate-depth-only-route",
            f"{counts['candidate-depth-only-route']} row(s) can try a depth-only route first",
        )
    if counts.get("needs-programmable-color-route"):
        return (
            "needs-programmable-color-route",
            f"{counts['needs-programmable-color-route']} row(s) need programmable color route coverage",
        )
    return "mixed-programmable-route", "no single route class dominates all selected rows"


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_markdown(path: Path, rows: list[dict[str, str]], source: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    verdict, reason = overall_verdict(rows)
    lines = [
        "# Programmable Route Feasibility",
        "",
        f"- Source: `{source}`",
        f"- Overall verdict: `{verdict}`",
        f"- Reason: {reason}",
        "",
        "| Row | Verdict | draws | primitives | depth-only prim | textured prim | color prim | unique PS | texture masks | next action |",
        "|---|---|---:|---:|---:|---:|---:|---:|---|---|",
    ]
    for row in rows:
        lines.append(
            "| {row} | `{verdict}` | `{draws}` | `{primitives}` | "
            "`{depth}` ({depth_pct}) | `{textured}` ({textured_pct}) | "
            "`{color}` ({color_pct}) | `{unique_ps}` | `{masks}` | {action} |".format(
                row=row["row"],
                verdict=row["verdict"],
                draws=fmt_int(as_int(row["draws"])),
                primitives=fmt_int(as_int(row["primitives"])),
                depth=fmt_int(as_int(row["depth_only_candidate_primitives"])),
                depth_pct=fmt_pct(as_int(row["depth_only_candidate_primitives"]), as_int(row["primitives"])),
                textured=fmt_int(as_int(row["programmable_textured_primitives"])),
                textured_pct=fmt_pct(as_int(row["programmable_textured_primitives"]), as_int(row["primitives"])),
                color=fmt_int(as_int(row["programmable_color_primitives"])),
                color_pct=fmt_pct(as_int(row["programmable_color_primitives"]), as_int(row["primitives"])),
                unique_ps=row["unique_ps"],
                masks=row["top_texture_masks"],
                action=row["next_action"],
            )
        )
    lines.extend([
        "",
        "```mermaid",
        "flowchart TD",
        "  Row[programmable hot row] --> DepthOnly{color write off\\ndepth write on\\nno alpha/blend?}",
        "  DepthOnly -- yes --> DepthRoute[depth-only programmable/binning route]",
        "  DepthOnly -- no --> Textured{texture mask nonzero?}",
        "  Textured -- yes --> TexturedRoute[programmable textured tile/mesh route]",
        "  Textured -- no --> ColorRoute[programmable color route]",
        "  DepthRoute --> Equality[depth/color equality gate]",
        "  TexturedRoute --> Equality",
        "  ColorRoute --> Equality",
        "  Equality --> Counters[reduced counter A/B]",
        "```",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe_draw_csv", type=Path)
    parser.add_argument("--row", action="append", default=[], help="Restrict to SEQ/ENC row")
    parser.add_argument("--seq", action="append", default=[], help="Restrict to frame/seq id")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--csv-output", type=Path, required=True)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    rows = analyze_rows(
        load_rows(args.probe_draw_csv),
        selected_rows=set(args.row),
        selected_seq=set(args.seq),
    )
    write_csv(args.csv_output, rows)
    write_markdown(args.output, rows, args.probe_draw_csv)
    print(args.output)
    print(args.csv_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
