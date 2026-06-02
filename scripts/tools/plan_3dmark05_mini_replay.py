#!/usr/bin/env python3
"""Summarize row-local mini replay readiness for 3DMark05 GT1.

The report joins three reduced artifacts:

* Xcode+dxmt joined encoder summary, for hot rows and VS-write ownership.
* Shader dump summary, for VS/PS source availability and VSOut liveness.
* Indexed probe draw CSV, for draw identity and index locality.
* Optional indexed geometry payload dumps, for raw index/stream0 replay bytes.

It intentionally does not claim a replay is runnable. It separates row/state/
shader/index identity from raw geometry-byte availability so the next harness
step is concrete.
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


def load_geometry_payloads(path: Path | None) -> list[dict[str, str]]:
    if path is None:
        return []
    if not path.exists():
        raise SystemExit(f"missing geometry dir: {path}")
    rows: list[dict[str, str]] = []
    for meta_path in sorted(path.glob("*.meta")):
        row: dict[str, str] = {"meta_file": str(meta_path)}
        for line in meta_path.read_text(encoding="utf-8").splitlines():
            if "=" not in line:
                continue
            key, value = line.split("=", 1)
            row[key] = value
        stem = meta_path.with_suffix("")
        index_path = Path(str(stem) + ".index.bin")
        stream_path = Path(str(stem) + ".stream0.bin")
        row["index_file"] = str(index_path)
        row["stream0_file"] = str(stream_path)
        row["index_file_exists"] = "1" if index_path.exists() else "0"
        row["stream0_file_exists"] = "1" if stream_path.exists() else "0"
        row["index_file_bytes"] = str(index_path.stat().st_size if index_path.exists() else 0)
        row["stream0_file_bytes"] = str(stream_path.stat().st_size if stream_path.exists() else 0)
        row["payload_valid"] = "1" if geometry_payload_valid(row) else "0"
        rows.append(row)
    return rows


def geometry_payload_valid(row: dict[str, str]) -> bool:
    return (
        row.get("index_range_valid") == "1"
        and row.get("stream0_range_valid") == "1"
        and row.get("wrote_index") == "1"
        and row.get("wrote_stream0") == "1"
        and row.get("index_file_exists") == "1"
        and row.get("stream0_file_exists") == "1"
        and as_int(row.get("index_file_bytes")) == as_int(row.get("index_byte_count"))
        and as_int(row.get("stream0_file_bytes")) == as_int(row.get("stream0_byte_count"))
    )


def geometry_by_key(geometry_rows: list[dict[str, str]]) -> dict[str, list[dict[str, str]]]:
    out: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in geometry_rows:
        key = row_key(row)
        if key and geometry_payload_valid(row):
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
    geometry_path: Path | None,
    joined: list[dict[str, str]],
    shaders: list[dict[str, str]],
    probes: list[dict[str, str]],
    geometries: list[dict[str, str]],
    top_n: int,
    top_groups: int,
) -> None:
    shader_index = shader_by_key(shaders)
    probe_index = probe_by_key(probes)
    geometry_index = geometry_by_key(geometries)
    hot_rows = top_rows(joined, top_n)

    lines: list[str] = []
    lines.append("# 3DMark05 Mini Replay Readiness")
    lines.append("")
    lines.append(f"- Joined summary: `{joined_path}`")
    lines.append(f"- Shader summary: `{shader_path}`")
    lines.append(f"- Indexed probe draws: `{probe_path}`")
    if geometry_path is not None:
        lines.append(f"- Indexed geometry payloads: `{geometry_path}`")
    lines.append("")
    lines.append("## Readiness Summary")
    lines.append("")
    lines.append("| Item | Status | Evidence |")
    lines.append("|---|---|---|")
    lines.append(f"| Hot row attribution | ready | `{len(hot_rows)}` Xcode/dxmt rows selected |")
    shader_ready = sum(1 for row in hot_rows if row_key(row) in shader_index)
    probe_ready = sum(1 for row in hot_rows if row_key(row) in probe_index)
    payload_ready = sum(1 for row in hot_rows if row_key(row) in geometry_index)
    payload_count = sum(len(rows) for rows in geometry_index.values())
    lines.append(f"| Shader sources | partial | `{shader_ready}/{len(hot_rows)}` hot rows have shader dump rows |")
    lines.append(f"| Draw identity/index locality | partial | `{probe_ready}/{len(hot_rows)}` hot rows have indexed probe rows |")
    if payload_count == 0:
        payload_status = "missing"
        payload_evidence = "no valid replayable geometry payload triplets found"
    else:
        payload_status = "partial"
        payload_evidence = (
            f"`{payload_count}` valid payload triplets across "
            f"`{payload_ready}/{len(hot_rows)}` hot rows"
        )
    lines.append(f"| Raw vertex/index payload | {payload_status} | {payload_evidence} |")
    lines.append("")
    lines.append("## Hot Rows")
    lines.append("")
    lines.append(
        "| Row | GPU ms | VS write MiB | VS B/inv | Shader dump | Draw rows | "
        "Payloads | Top draw group | Missing for replay |"
    )
    lines.append("|---|---:|---:|---:|---|---:|---:|---|---|")

    for row in hot_rows:
        key = row_key(row)
        shader = shader_index.get(key, {})
        row_probes = probe_index.get(key, [])
        row_payloads = geometry_index.get(key, [])
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
        if not row_payloads:
            missing.append("geometry payload")
        elif len(row_payloads) < len(row_probes):
            missing.append("remaining geometry payloads")
        if not missing:
            missing.append("none")
        lines.append(
            f"| `{key or 'n/a'}` | `{fmt(row.get('gpu_ms'))}` | "
            f"`{fmt(row.get('vs_buffer_write_mib'))}` | "
            f"`{fmt(row.get('vs_buffer_bytes_per_vs_invocation'))}` | "
            f"`{'yes' if shader else 'no'}` | `{len(row_probes)}` | "
            f"`{len(row_payloads)}` | {group_text} | `{', '.join(missing)}` |"
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
    if geometries:
        lines.append("## Geometry Payloads")
        lines.append("")
        lines.append("| Row | Payloads | Index bytes | Stream0 bytes | Valid |")
        lines.append("|---|---:|---:|---:|---:|")
        for key in sorted({row_key(row) for row in geometries if row_key(row)}):
            rows = [row for row in geometries if row_key(row) == key]
            valid_rows = [row for row in rows if geometry_payload_valid(row)]
            lines.append(
                f"| `{key}` | `{len(rows)}` | "
                f"`{fmt(sum(as_int(row.get('index_file_bytes')) for row in rows))}` | "
                f"`{fmt(sum(as_int(row.get('stream0_file_bytes')) for row in rows))}` | "
                f"`{len(valid_rows)}` |"
            )
        lines.append("")
    lines.append("## Next Instrumentation")
    lines.append("")
    if payload_count == 0:
        lines.append(
            "A row-local replay can be assembled only after dxmt9 dumps replayable "
            "vertex/index payloads for selected indexed draws. Use "
            "`--dump-indexed-geometry` with the existing row/class/draw-window "
            "filters and write the payload next to shader dumps under "
            "`traces/<run-id>/analysis/geometry/`."
        )
    else:
        lines.append(
            "The reduced artifacts now include raw geometry bytes for selected "
            "draws. The next step is a mini replay harness that consumes the "
            "payload triplets, dumped shaders, and row state, then runs Xcode "
            "counters on that isolated draw set."
        )
    lines.append("")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--joined", type=Path, required=True)
    parser.add_argument("--shader-summary", type=Path, required=True)
    parser.add_argument("--probe-draws", type=Path, required=True)
    parser.add_argument("--geometry-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--top", type=int, default=5)
    parser.add_argument("--top-groups", type=int, default=3)
    args = parser.parse_args()

    write_report(
        args.output,
        args.joined,
        args.shader_summary,
        args.probe_draws,
        args.geometry_dir,
        load_csv(args.joined),
        load_csv(args.shader_summary),
        load_csv(args.probe_draws),
        load_geometry_payloads(args.geometry_dir),
        args.top,
        args.top_groups,
    )
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
