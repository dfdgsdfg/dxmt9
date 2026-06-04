#!/usr/bin/env python3
"""Estimate whether adjacent indexed draws can be merged without reordering.

The tool consumes a dxmt indexed-probe CSV plus matching dumped index buffers
from DXMT9_DUMP_INDEXED_GEOMETRY_DIR. It keeps the original primitive order and
only simulates cache reuse across adjacent draws that share a selected key.
This is meant to reject or prioritize non-reorder merge/coalescing candidates
before spending a gputrace run.
"""

from __future__ import annotations

import argparse
import csv
import struct
from dataclasses import dataclass, field
from itertools import chain
from pathlib import Path
from typing import Callable, Iterable


STATE_FIELDS = (
    "primitive_type",
    "texture_mask",
    "color_write",
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
    "scissor_l",
    "scissor_t",
    "scissor_r",
    "scissor_b",
    "cull",
    "fill",
)

PIPELINE_FIELDS = (
    "pso",
    "shader_variant",
    "vs",
    "ps",
    "vsout",
)

BINDING_FIELDS = (
    "index_type",
    "index_buffer",
    "stream0_handle",
    "stream0_offset",
    "stream0_stride",
    "base_vertex",
)

PIPELINE_RELAXED_FIELDS = STATE_FIELDS + PIPELINE_FIELDS + (
    "index_type",
    "stream0_stride",
    "base_vertex",
)

MATERIAL_FIELDS = (
    "texture_mask",
    "color_write",
    "alpha_blend",
    "src_blend",
    "dst_blend",
    "blend_op",
    "separate_alpha",
    "depth_enabled",
    "depth_write",
    "depth_func",
    "scissor",
    "cull",
    "fill",
    "stream0_stride",
)


def as_int(value: object, default: int = 0) -> int:
    text = str(value or "").strip()
    if not text:
        return default
    try:
        return int(text, 0)
    except ValueError:
        return default


def row_label(row: dict[str, str]) -> str:
    return f"{row.get('seq', '')}/{row.get('encoder', '')}"


def parse_row_key(value: str) -> tuple[str, str]:
    text = value.strip()
    if "/" not in text:
        raise argparse.ArgumentTypeError(
            f"invalid row key '{value}', expected SEQ/ENC"
        )
    seq, enc = (part.strip() for part in text.split("/", 1))
    if not seq or not enc:
        raise argparse.ArgumentTypeError(
            f"invalid row key '{value}', expected SEQ/ENC"
        )
    return seq, enc


def fmt_int(value: int) -> str:
    return f"{value:,}"


def pct_delta(after: int, before: int) -> float | None:
    if before == 0:
        return None
    return (after - before) / before * 100.0


def fmt_pct(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:+.4f}%"


def index_is_32_bit(index_type: str) -> bool:
    text = str(index_type or "").strip().lower()
    return text in {"1", "uint32", "d3dfmt_index32", "32"}


def lru_misses(indices: Iterable[int], cache_size: int) -> int:
    cache: list[int] = []
    misses = 0
    for index in indices:
        try:
            pos = cache.index(index)
        except ValueError:
            misses += 1
            cache.insert(0, index)
            if len(cache) > cache_size:
                cache.pop()
        else:
            cache.pop(pos)
            cache.insert(0, index)
    return misses


def key_from_fields(row: dict[str, str], fields: Iterable[str]) -> tuple[tuple[str, str], ...]:
    return tuple((field, row.get(field, "")) for field in fields)


def key_for_mode(row: dict[str, str], mode: str) -> tuple[tuple[str, str], ...]:
    if mode == "full_row":
        return (("row", row_label(row)),)
    if mode == "same_binding":
        return key_from_fields(row, STATE_FIELDS + PIPELINE_FIELDS + BINDING_FIELDS)
    if mode == "same_pipeline":
        return key_from_fields(row, PIPELINE_RELAXED_FIELDS)
    if mode == "same_material":
        return key_from_fields(row, MATERIAL_FIELDS)
    raise ValueError(f"unsupported mode: {mode}")


@dataclass
class Draw:
    row: dict[str, str]
    indices: list[int]
    _miss_cache: dict[int, int] = field(default_factory=dict)

    @property
    def slot(self) -> int:
        return as_int(self.row.get("encoder_draw_index"))

    @property
    def draw_ordinal(self) -> int:
        return as_int(self.row.get("draw_ordinal"))

    def misses(self, cache_size: int) -> int:
        if cache_size not in self._miss_cache:
            self._miss_cache[cache_size] = lru_misses(self.indices, cache_size)
        return self._miss_cache[cache_size]


@dataclass
class GroupResult:
    mode: str
    group_index: int
    draws: list[Draw]
    before_misses: int
    after_misses: int

    @property
    def draw_count(self) -> int:
        return len(self.draws)

    @property
    def draw_saved(self) -> int:
        return max(0, self.draw_count - 1)

    @property
    def delta(self) -> int:
        return self.after_misses - self.before_misses

    @property
    def delta_pct(self) -> float | None:
        return pct_delta(self.after_misses, self.before_misses)

    @property
    def first_slot(self) -> int:
        return self.draws[0].slot

    @property
    def last_slot(self) -> int:
        return self.draws[-1].slot


@dataclass
class ModeSummary:
    mode: str
    note: str
    row_count: int
    group_count: int
    multi_group_count: int
    draws_in_multi_groups: int
    draw_saved: int
    miss_before: int
    miss_after: int
    groups: list[GroupResult]

    @property
    def delta(self) -> int:
        return self.miss_after - self.miss_before

    @property
    def delta_pct(self) -> float | None:
        return pct_delta(self.miss_after, self.miss_before)


def load_probe_rows(path: Path, row_key: tuple[str, str]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            if (row.get("seq", ""), row.get("encoder", "")) == row_key:
                rows.append(row)
    rows.sort(key=lambda row: as_int(row.get("encoder_draw_index")))
    return rows


def find_index_file(geometry_dir: Path, row: dict[str, str]) -> Path:
    seq = row.get("seq", "")
    enc = row.get("encoder", "")
    slot = as_int(row.get("encoder_draw_index"))
    pattern = f"seq{seq}-enc{enc}-draw*-slot{slot}.index.bin"
    matches = sorted(geometry_dir.glob(pattern))
    if not matches:
        matches = sorted(geometry_dir.glob(f"*-slot{slot}.index.bin"))
    if len(matches) != 1:
        raise FileNotFoundError(
            f"expected one index dump for {seq}/{enc} slot {slot}, found {len(matches)}"
        )
    return matches[0]


def read_indices(path: Path, row: dict[str, str]) -> list[int]:
    data = path.read_bytes()
    base_vertex = as_int(row.get("base_vertex"))
    if index_is_32_bit(row.get("index_type", "")):
        if len(data) % 4:
            raise ValueError(f"{path} size is not uint32 aligned")
        values = struct.unpack("<" + "I" * (len(data) // 4), data)
    else:
        if len(data) % 2:
            raise ValueError(f"{path} size is not uint16 aligned")
        values = struct.unpack("<" + "H" * (len(data) // 2), data)
    if base_vertex == 0:
        return list(values)
    return [base_vertex + value for value in values]


def load_draws(probe_rows: list[dict[str, str]], geometry_dir: Path) -> list[Draw]:
    draws: list[Draw] = []
    for row in probe_rows:
        index_file = find_index_file(geometry_dir, row)
        draws.append(Draw(row=row, indices=read_indices(index_file, row)))
    return draws


def split_adjacent_groups(
    draws: list[Draw],
    key_fn: Callable[[dict[str, str]], tuple[tuple[str, str], ...]],
) -> list[list[Draw]]:
    groups: list[list[Draw]] = []
    current: list[Draw] = []
    current_key: tuple[tuple[str, str], ...] | None = None
    for draw in draws:
        key = key_fn(draw.row)
        if current and key != current_key:
            groups.append(current)
            current = []
        current.append(draw)
        current_key = key
    if current:
        groups.append(current)
    return groups


def analyze_group(
    mode: str,
    index: int,
    group: list[Draw],
    cache_size: int,
) -> GroupResult:
    before = sum(draw.misses(cache_size) for draw in group)
    after = lru_misses(chain.from_iterable(draw.indices for draw in group), cache_size)
    return GroupResult(
        mode=mode,
        group_index=index,
        draws=group,
        before_misses=before,
        after_misses=after,
    )


def analyze_mode(draws: list[Draw], mode: str, cache_size: int) -> ModeSummary:
    groups = split_adjacent_groups(draws, lambda row: key_for_mode(row, mode))
    results = [
        analyze_group(mode, index, group, cache_size)
        for index, group in enumerate(groups)
    ]
    if mode == "full_row":
        candidates = results
        note = "semantic-impossible upper bound"
    else:
        candidates = [result for result in results if result.draw_count > 1]
        note = "adjacent same-key candidates only"
    return ModeSummary(
        mode=mode,
        note=note,
        row_count=len(draws),
        group_count=len(groups),
        multi_group_count=sum(1 for result in results if result.draw_count > 1),
        draws_in_multi_groups=sum(
            result.draw_count for result in results if result.draw_count > 1
        ),
        draw_saved=sum(result.draw_saved for result in results if result.draw_count > 1),
        miss_before=sum(result.before_misses for result in candidates),
        miss_after=sum(result.after_misses for result in candidates),
        groups=candidates,
    )


def write_summary_csv(path: Path, summaries: list[ModeSummary]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "mode",
        "note",
        "rows",
        "groups",
        "multi_groups",
        "draws_in_multi_groups",
        "draw_saved",
        "miss_before",
        "miss_after",
        "delta",
        "delta_pct",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for summary in summaries:
            writer.writerow({
                "mode": summary.mode,
                "note": summary.note,
                "rows": summary.row_count,
                "groups": summary.group_count,
                "multi_groups": summary.multi_group_count,
                "draws_in_multi_groups": summary.draws_in_multi_groups,
                "draw_saved": summary.draw_saved,
                "miss_before": summary.miss_before,
                "miss_after": summary.miss_after,
                "delta": summary.delta,
                "delta_pct": "" if summary.delta_pct is None else repr(summary.delta_pct),
            })


def top_groups(summary: ModeSummary, limit: int) -> list[GroupResult]:
    return sorted(summary.groups, key=lambda group: group.delta)[:limit]


def write_report(
    path: Path,
    probe_draws: Path,
    geometry_dir: Path,
    row_key: tuple[str, str],
    cache_size: int,
    summaries: list[ModeSummary],
    top_limit: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    lines.append("# Indexed Draw Merge Scout")
    lines.append("")
    lines.append(f"- row: `{row_key[0]}/{row_key[1]}`")
    lines.append(f"- cache model: `LRU{cache_size}`")
    lines.append(f"- probe draws: `{probe_draws}`")
    lines.append(f"- geometry dir: `{geometry_dir}`")
    lines.append("")
    lines.append(
        "This report keeps original primitive order. `full_row` is an upper "
        "bound and is not a legal production merge when state or uniforms differ."
    )
    lines.append("")
    lines.append("| Mode | Note | Rows | Groups | Multi groups | Draws in multi | Draw saved | LRU before | LRU after | Delta | Delta % |")
    lines.append("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for summary in summaries:
        lines.append(
            f"| `{summary.mode}` | {summary.note} | `{summary.row_count}` | "
            f"`{summary.group_count}` | `{summary.multi_group_count}` | "
            f"`{summary.draws_in_multi_groups}` | `{summary.draw_saved}` | "
            f"`{fmt_int(summary.miss_before)}` | `{fmt_int(summary.miss_after)}` | "
            f"`{fmt_int(summary.delta)}` | `{fmt_pct(summary.delta_pct)}` |"
        )
    lines.append("")

    for summary in summaries:
        lines.append(f"## `{summary.mode}` Top Candidate Groups")
        lines.append("")
        groups = top_groups(summary, top_limit)
        if not groups:
            lines.append("No adjacent multi-draw candidates.")
            lines.append("")
            continue
        lines.append("| Group | Slots | Draw ordinals | Draws | LRU before | LRU after | Delta | Delta % |")
        lines.append("|---:|---|---|---:|---:|---:|---:|---:|")
        for group in groups:
            lines.append(
                f"| `{group.group_index}` | `{group.first_slot}..{group.last_slot}` | "
                f"`{group.draws[0].draw_ordinal}..{group.draws[-1].draw_ordinal}` | "
                f"`{group.draw_count}` | `{fmt_int(group.before_misses)}` | "
                f"`{fmt_int(group.after_misses)}` | `{fmt_int(group.delta)}` | "
                f"`{fmt_pct(group.delta_pct)}` |"
            )
        lines.append("")

    lines.append("```mermaid")
    lines.append("flowchart TD")
    lines.append("  Row[\"target row\\nindexed draws\"] --> Full[\"full_row concat\\nupper bound only\"]")
    lines.append("  Row --> Binding[\"same_binding\\nstate + PSO + stream/IB\"]")
    lines.append("  Row --> Pipeline[\"same_pipeline\\nstate + PSO + stride\"]")
    lines.append("  Row --> Material[\"same_material\\nrelaxed material/state\"]")
    lines.append("  Binding --> Decision{\"material LRU drop?\"}")
    lines.append("  Pipeline --> Decision")
    lines.append("  Material --> Decision")
    lines.append("  Decision -- \"tiny\" --> Reject[\"reject draw-boundary merge\\nas primary VS-invocation fix\"]")
    lines.append("  Decision -- \"large\" --> Trace[\"launch scoped gputrace\"]")
    lines.append("```")
    lines.append("")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe-draws", required=True, type=Path)
    parser.add_argument("--geometry-dir", required=True, type=Path)
    parser.add_argument("--row", required=True, type=parse_row_key)
    parser.add_argument("--cache-size", type=int, default=32)
    parser.add_argument(
        "--mode",
        action="append",
        choices=("full_row", "same_binding", "same_pipeline", "same_material"),
        help="mode to analyze; may be repeated; defaults to all modes",
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--summary-output", type=Path)
    parser.add_argument("--top-groups", type=int, default=8)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    if args.cache_size <= 0:
        raise SystemExit("--cache-size must be positive")
    modes = args.mode or ["full_row", "same_binding", "same_pipeline", "same_material"]
    rows = load_probe_rows(args.probe_draws, args.row)
    if not rows:
        raise SystemExit(f"no probe rows matched {args.row[0]}/{args.row[1]}")
    draws = load_draws(rows, args.geometry_dir)
    summaries = [analyze_mode(draws, mode, args.cache_size) for mode in modes]
    write_report(
        args.output,
        args.probe_draws,
        args.geometry_dir,
        args.row,
        args.cache_size,
        summaries,
        args.top_groups,
    )
    if args.summary_output:
        write_summary_csv(args.summary_output, summaries)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
