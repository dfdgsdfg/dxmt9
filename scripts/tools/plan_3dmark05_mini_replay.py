#!/usr/bin/env python3
"""Summarize row-local mini replay readiness for 3DMark05 GT1.

The report joins three reduced artifacts:

* Xcode+dxmt joined encoder summary, for hot rows and VS-write ownership.
* Shader dump summary, for VS/PS source availability and VSOut liveness.
* Indexed probe draw CSV, for draw identity and index locality.

It intentionally does not claim a replay is runnable. Today the reduced
artifacts still lack raw vertex/index payload dumps; the report makes that gap
explicit so the next instrumentation step is concrete.
"""

from __future__ import annotations

import argparse
import csv
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any


def as_float(value: Any) -> float:
    try:
        return float(str(value))
    except (TypeError, ValueError):
        return 0.0


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
        raise SystemExit(f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def row_key(row: dict[str, str]) -> str:
    seq = row.get("seq", "")
    enc = row.get("enc", row.get("encoder", ""))
    if seq == "" or enc == "":
        return ""
    return f"{seq}/{enc}"


def fmt(value: Any, digits: int = 3) -> str:
    number = as_float(value)
    if abs(number - round(number)) < 0.0005:
        return f"{int(round(number)):,}"
    return f"{number:,.{digits}f}"


def short_hash(value: str) -> str:
    if not value:
        return ""
    return value if len(value) <= 18 else value[:18]


def state_signature(row: dict[str, str]) -> str:
    return (
        f"ab={row.get('alpha_blend', '')} "
        f"depth={row.get('depth_enabled', '')}/{row.get('depth_write', '')} "
        f"scissor={row.get('scissor', '')} "
        f"tex={row.get('texture_mask', '')} "
        f"cull={row.get('cull', '')}"
    )


def top_rows(joined: list[dict[str, str]], top_n: int) -> list[dict[str, str]]:
    return sorted(joined, key=lambda row: as_float(row.get("gpu_ms")), reverse=True)[:top_n]


def shader_by_key(shader_rows: list[dict[str, str]]) -> dict[str, dict[str, str]]:
    return {row_key(row): row for row in shader_rows if row_key(row)}


def probe_by_key(probe_rows: list[dict[str, str]]) -> dict[str, list[dict[str, str]]]:
    out: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in probe_rows:
        key = row_key(row)
        if key:
            out[key].append(row)
    return out


def draw_groups(rows: list[dict[str, str]], limit: int) -> list[tuple[tuple[str, str, str], int, int, int]]:
    totals: dict[tuple[str, str, str], list[int]] = defaultdict(lambda: [0, 0, 0])
    for row in rows:
        key = (
            row.get("vs", ""),
            row.get("ps", ""),
            state_signature(row),
        )
        totals[key][0] += 1
        totals[key][1] += as_int(row.get("primitive_count"))
        totals[key][2] += as_int(row.get("original_cache_miss64"))
    ranked = sorted(totals.items(), key=lambda item: (item[1][1], item[1][2]), reverse=True)
    return [(key, values[0], values[1], values[2]) for key, values in ranked[:limit]]


def write_report(
    output: Path,
    joined_path: Path,
    shader_path: Path,
    probe_path: Path,
    joined: list[dict[str, str]],
    shaders: list[dict[str, str]],
    probes: list[dict[str, str]],
    top_n: int,
    top_groups: int,
) -> None:
    shader_index = shader_by_key(shaders)
    probe_index = probe_by_key(probes)
    hot_rows = top_rows(joined, top_n)

    lines: list[str] = []
    lines.append("# 3DMark05 Mini Replay Readiness")
    lines.append("")
    lines.append(f"- Joined summary: `{joined_path}`")
    lines.append(f"- Shader summary: `{shader_path}`")
    lines.append(f"- Indexed probe draws: `{probe_path}`")
    lines.append("")
    lines.append("## Readiness Summary")
    lines.append("")
    lines.append("| Item | Status | Evidence |")
    lines.append("|---|---|---|")
    lines.append(f"| Hot row attribution | ready | `{len(hot_rows)}` Xcode/dxmt rows selected |")
    shader_ready = sum(1 for row in hot_rows if row_key(row) in shader_index)
    probe_ready = sum(1 for row in hot_rows if row_key(row) in probe_index)
    lines.append(f"| Shader sources | partial | `{shader_ready}/{len(hot_rows)}` hot rows have shader dump rows |")
    lines.append(f"| Draw identity/index locality | partial | `{probe_ready}/{len(hot_rows)}` hot rows have indexed probe rows |")
    lines.append("| Raw vertex/index payload | missing | no reduced artifact currently contains replayable geometry bytes |")
    lines.append("")
    lines.append("## Hot Rows")
    lines.append("")
    lines.append(
        "| Row | GPU ms | VS write MiB | VS B/inv | Shader dump | Draw rows | "
        "Top draw group | Missing for replay |"
    )
    lines.append("|---|---:|---:|---:|---|---:|---|---|")

    for row in hot_rows:
        key = row_key(row)
        shader = shader_index.get(key, {})
        row_probes = probe_index.get(key, [])
        group_text = "none"
        groups = draw_groups(row_probes, 1)
        if groups:
            (vs, ps, state), draws, primitives, cache64 = groups[0]
            group_text = (
                f"{draws} draws / {fmt(primitives)} tris / "
                f"cache64 {fmt(cache64)} / vs {short_hash(vs)} ps {short_hash(ps)} / {state}"
            )
        missing: list[str] = []
        if not shader:
            missing.append("shader")
        if not row_probes:
            missing.append("draw identity")
        missing.append("geometry payload")
        lines.append(
            f"| `{key or 'n/a'}` | `{fmt(row.get('gpu_ms'))}` | "
            f"`{fmt(row.get('vs_buffer_write_mib'))}` | "
            f"`{fmt(row.get('vs_buffer_bytes_per_vs_invocation'))}` | "
            f"`{'yes' if shader else 'no'}` | `{len(row_probes)}` | "
            f"{group_text} | `{', '.join(missing)}` |"
        )

    lines.append("")
    lines.append("## Replay Target Groups")
    lines.append("")
    lines.append("| Row | Rank | Draws | Tris | Cache64 | VS | PS | State |")
    lines.append("|---|---:|---:|---:|---:|---|---|---|")
    for row in hot_rows:
        key = row_key(row)
        for rank, (group_key, draws, primitives, cache64) in enumerate(
            draw_groups(probe_index.get(key, []), top_groups), start=1
        ):
            vs, ps, state = group_key
            lines.append(
                f"| `{key}` | `{rank}` | `{draws}` | `{fmt(primitives)}` | "
                f"`{fmt(cache64)}` | `{short_hash(vs)}` | `{short_hash(ps)}` | `{state}` |"
            )
    lines.append("")
    lines.append("## Next Instrumentation")
    lines.append("")
    lines.append(
        "A row-local replay can be assembled only after dxmt9 can dump replayable "
        "vertex/index payloads for selected indexed draws. The natural selector "
        "should reuse the existing row/class/draw-window filters used by "
        "`DXMT9_MEASURE_INDEX_REUSE` and the indexed-triangle probes, and write "
        "the payload next to shader dumps under `traces/<run-id>/analysis/geometry/`."
    )
    lines.append("")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--joined", type=Path, required=True)
    parser.add_argument("--shader-summary", type=Path, required=True)
    parser.add_argument("--probe-draws", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--top", type=int, default=5)
    parser.add_argument("--top-groups", type=int, default=3)
    args = parser.parse_args()

    write_report(
        args.output,
        args.joined,
        args.shader_summary,
        args.probe_draws,
        load_csv(args.joined),
        load_csv(args.shader_summary),
        load_csv(args.probe_draws),
        args.top,
        args.top_groups,
    )
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
