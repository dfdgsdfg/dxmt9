#!/usr/bin/env python3
"""Classify 3DMark05 large alpha-blend backend-shape candidates."""

from __future__ import annotations

import argparse
import csv
import re
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


BLEND_FACTOR_NAMES = {
    1: "zero",
    2: "one",
    3: "src-color",
    4: "inv-src-color",
    5: "src-alpha",
    6: "inv-src-alpha",
    7: "dst-alpha",
    8: "inv-dst-alpha",
    9: "dst-color",
    10: "inv-dst-color",
    11: "src-alpha-sat",
}

BLEND_OP_NAMES = {
    1: "add",
    2: "subtract",
    3: "rev-subtract",
    4: "min",
    5: "max",
}


def as_int(value: object, default: int = 0) -> int:
    if value is None:
        return default
    text = str(value).strip()
    if not text:
        return default
    try:
        return int(text, 0)
    except ValueError:
        return default


def as_bool(value: object) -> bool:
    return as_int(value) != 0


def fmt_int(value: int) -> str:
    return f"{value:,}"


def blend_kind(row: dict[str, str]) -> str:
    if not as_bool(row.get("alpha_blend")):
        return "off"
    src = as_int(row.get("src_blend"))
    dst = as_int(row.get("dst_blend"))
    op = as_int(row.get("blend_op"))
    separate_alpha = as_bool(row.get("separate_alpha"))
    if not separate_alpha and src == 10 and dst == 2 and op == 1:
        return "screen"
    if not separate_alpha and src == 5 and dst == 6 and op == 1:
        return "standard-alpha"
    if not separate_alpha and src == 5 and dst == 2 and op == 1:
        return "additive-alpha"
    return "custom"


def blend_signature(row: dict[str, str]) -> str:
    src = as_int(row.get("src_blend"))
    dst = as_int(row.get("dst_blend"))
    op = as_int(row.get("blend_op"))
    src_name = BLEND_FACTOR_NAMES.get(src, str(src))
    dst_name = BLEND_FACTOR_NAMES.get(dst, str(dst))
    op_name = BLEND_OP_NAMES.get(op, str(op))
    if as_bool(row.get("separate_alpha")):
        src_a = as_int(row.get("src_blend_alpha"))
        dst_a = as_int(row.get("dst_blend_alpha"))
        op_a = as_int(row.get("blend_op_alpha"))
        return (
            f"{src_name}:{dst_name}:{op_name};"
            f"a={BLEND_FACTOR_NAMES.get(src_a, str(src_a))}:"
            f"{BLEND_FACTOR_NAMES.get(dst_a, str(dst_a))}:"
            f"{BLEND_OP_NAMES.get(op_a, str(op_a))}"
        )
    return f"{src_name}:{dst_name}:{op_name}"


def depth_kind(row: dict[str, str]) -> str:
    if not as_bool(row.get("depth_enabled")):
        return "off"
    if as_bool(row.get("depth_write")):
        return "write"
    return "read"


def shader_key_from_msl(path: Path) -> str | None:
    match = re.match(r".*-fs-shader-(\d+)-source-\d+\.metal$", path.name)
    if not match:
        return None
    return hex(int(match.group(1)))


@dataclass
class ShaderEvidence:
    path: str = ""
    alpha_write: str = "missing"
    sample_calls: int = 0
    dynamic_alpha_refs: int = 0


def classify_shader_alpha(text: str) -> str:
    if "outColor[0] = (r[0] * r[1] + r[2])" in text:
        return "dynamic-expression"
    if re.search(r"outColor\[0\]\s*=\s*dxmt9_merge\(outColor\[0\],\s*float4\(in\.", text):
        return "varying-alpha"
    if re.search(r"outColor\[0\]\s*=\s*dxmt9_merge\(outColor\[0\],\s*cFloat\[\d+\]", text):
        return "uniform-alpha"
    if re.search(r"return\s+float4\([^;]+,\s*1\.0f?\)", text):
        return "constant-one-alpha"
    return "unknown"


def load_shader_evidence(msl_dir: Path | None) -> dict[str, ShaderEvidence]:
    if msl_dir is None:
        return {}
    evidence: dict[str, ShaderEvidence] = {}
    for path in msl_dir.glob("*.metal"):
        key = shader_key_from_msl(path)
        if key is None:
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        evidence[key] = ShaderEvidence(
            path=str(path),
            alpha_write=classify_shader_alpha(text),
            sample_calls=text.count(".sample("),
            dynamic_alpha_refs=text.count(".w") + text.count(".a"),
        )
    return evidence


@dataclass
class Group:
    key: tuple[str, ...]
    draws: int = 0
    primitives: int = 0
    vertices: int = 0
    ps: Counter[str] = field(default_factory=Counter)
    vs: Counter[str] = field(default_factory=Counter)
    pso: Counter[str] = field(default_factory=Counter)
    payload: Counter[str] = field(default_factory=Counter)
    draw_indices: list[str] = field(default_factory=list)
    blend_signatures: Counter[str] = field(default_factory=Counter)
    alpha_writes: Counter[str] = field(default_factory=Counter)
    sample_calls: int = 0
    dynamic_alpha_refs: int = 0

    def add(self, row: dict[str, str], shader_evidence: dict[str, ShaderEvidence]) -> None:
        self.draws += 1
        self.primitives += as_int(row.get("primitive_count"))
        self.vertices += as_int(row.get("vertex_count"))
        self.ps[row.get("ps", "")] += 1
        self.vs[row.get("vs", "")] += 1
        self.pso[row.get("pso", "")] += 1
        self.payload[row.get("uniform_payload_hash", "")] += 1
        self.draw_indices.append(row.get("encoder_draw_index", ""))
        self.blend_signatures[blend_signature(row)] += 1
        ev = shader_evidence.get(row.get("ps", ""), ShaderEvidence())
        self.alpha_writes[ev.alpha_write] += 1
        self.sample_calls += ev.sample_calls
        self.dynamic_alpha_refs += ev.dynamic_alpha_refs


def group_key(row: dict[str, str], mode: str) -> tuple[str, ...]:
    base = (
        f"{row.get('seq', '')}/{row.get('encoder', '')}",
        f"depth={depth_kind(row)}",
        f"blend={blend_kind(row)}",
        f"scissor={'on' if as_bool(row.get('scissor')) else 'off'}",
        f"textured={'yes' if as_int(row.get('texture_mask')) else 'no'}",
        f"color_write={row.get('color_write', '')}",
    )
    if mode == "class":
        return base
    if mode == "shader":
        return base + (f"ps={row.get('ps', '')}", f"vs={row.get('vs', '')}")
    raise ValueError(f"unsupported group mode: {mode}")


def select_rows(
    rows: Iterable[dict[str, str]],
    seq: str | None,
    row: str | None,
    primitive_threshold: int,
    include_small: bool,
) -> list[dict[str, str]]:
    selected: list[dict[str, str]] = []
    for csv_row in rows:
        if seq is not None and csv_row.get("seq") != seq:
            continue
        if row is not None and f"{csv_row.get('seq', '')}/{csv_row.get('encoder', '')}" != row:
            continue
        if not as_bool(csv_row.get("alpha_blend")):
            continue
        if not include_small and as_int(csv_row.get("primitive_count")) < primitive_threshold:
            continue
        selected.append(csv_row)
    return selected


def group_rows(
    rows: Iterable[dict[str, str]],
    mode: str,
    shader_evidence: dict[str, ShaderEvidence],
) -> list[Group]:
    groups: dict[tuple[str, ...], Group] = {}
    for row in rows:
        key = group_key(row, mode)
        group = groups.setdefault(key, Group(key=key))
        group.add(row, shader_evidence)
    return sorted(groups.values(), key=lambda group: (group.primitives, group.draws), reverse=True)


def alpha_evidence(group: Group) -> str:
    if not group.alpha_writes:
        return "missing"
    return ",".join(f"{name}:{count}" for name, count in group.alpha_writes.most_common())


def static_blend_off_verdict(group: Group) -> str:
    signatures = set(group.blend_signatures)
    if signatures == {"one:zero:add"}:
        return "already-replace"
    key_text = "|".join(group.key)
    if "blend=screen" in key_text:
        return "reject-screen-non-noop"
    if "blend=standard-alpha" in key_text:
        if group.alpha_writes and set(group.alpha_writes) == {"constant-one-alpha"}:
            return "needs-dst-equality-proof"
        return "reject-alpha-not-static-one"
    if "blend=additive-alpha" in key_text:
        return "reject-additive-non-noop"
    return "reject-no-static-equivalence"


def next_gate(group: Group) -> str:
    verdict = static_blend_off_verdict(group)
    if verdict.startswith("reject"):
        return "non-reorder backend A/B only; do not disable blend as a fix"
    return "requires image/final-color proof before Xcode"


def write_csv(path: Path, groups: list[Group]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "group",
                "draws",
                "primitives",
                "vertices",
                "ps_count",
                "vs_count",
                "pso_count",
                "payload_count",
                "blend_signatures",
                "alpha_evidence",
                "sample_calls_sum",
                "dynamic_alpha_refs_sum",
                "static_blend_off_verdict",
                "next_gate",
                "draw_indices",
            ]
        )
        for group in groups:
            writer.writerow(
                [
                    "|".join(group.key),
                    group.draws,
                    group.primitives,
                    group.vertices,
                    len(group.ps),
                    len(group.vs),
                    len(group.pso),
                    len(group.payload),
                    ";".join(f"{name}:{count}" for name, count in group.blend_signatures.most_common()),
                    alpha_evidence(group),
                    group.sample_calls,
                    group.dynamic_alpha_refs,
                    static_blend_off_verdict(group),
                    next_gate(group),
                    ",".join(group.draw_indices),
                ]
            )


def write_markdown(path: Path, groups: list[Group], input_path: Path, msl_dir: Path | None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    total_draws = sum(group.draws for group in groups)
    total_primitives = sum(group.primitives for group in groups)
    total_vertices = sum(group.vertices for group in groups)
    with path.open("w", encoding="utf-8") as f:
        f.write("# Alpha Backend Candidate Breakdown\n\n")
        f.write(f"- Input: `{input_path}`\n")
        if msl_dir is not None:
            f.write(f"- Shader MSL dir: `{msl_dir}`\n")
        f.write(
            f"- Selected alpha groups: `{len(groups)}`; draws `{fmt_int(total_draws)}`, "
            f"primitives `{fmt_int(total_primitives)}`, vertices `{fmt_int(total_vertices)}`\n\n"
        )
        f.write(
            "This report is a pre-Xcode gate. It checks whether a large alpha-blend "
            "class has a static reason to treat blend-disable as color-equivalent. "
            "Rejected rows may still be useful backend-shape probes, but not fixes.\n\n"
        )
        f.write(
            "| Group | draws | primitives | vertices | PS/VS/PSO/payload | blend signatures | "
            "alpha evidence | verdict | next gate |\n"
        )
        f.write("|---|---:|---:|---:|---:|---|---|---|---|\n")
        for group in groups:
            f.write(
                "| "
                f"`{'|'.join(group.key)}` | "
                f"`{group.draws}` | "
                f"`{fmt_int(group.primitives)}` | "
                f"`{fmt_int(group.vertices)}` | "
                f"`{len(group.ps)}/{len(group.vs)}/{len(group.pso)}/{len(group.payload)}` | "
                f"`{'; '.join(f'{name}:{count}' for name, count in group.blend_signatures.most_common())}` | "
                f"`{alpha_evidence(group)}` | "
                f"`{static_blend_off_verdict(group)}` | "
                f"{next_gate(group)} |\n"
            )
        f.write(
            "\n```mermaid\n"
            "flowchart TD\n"
            "  A[large alpha indexed draw] --> B{blend-off statically equivalent?}\n"
            "  B -- no --> C[diagnostic only\\nnot a correctness fix]\n"
            "  B -- yes --> D[semantic/image gate]\n"
            "  C --> E[look for primitive-order-preserving\\nbackend-state A/B]\n"
            "  D --> F[Xcode counter gate]\n"
            "```\n"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe_csv", type=Path)
    parser.add_argument("--seq", default=None)
    parser.add_argument("--row", default=None, help="Optional row selector, e.g. 60/2")
    parser.add_argument("--primitive-threshold", type=int, default=4096)
    parser.add_argument("--include-small", action="store_true")
    parser.add_argument("--group", choices=("class", "shader"), default="class")
    parser.add_argument("--msl-dir", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--csv-output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with args.probe_csv.open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    shader_evidence = load_shader_evidence(args.msl_dir)
    selected = select_rows(
        rows,
        seq=args.seq,
        row=args.row,
        primitive_threshold=args.primitive_threshold,
        include_small=args.include_small,
    )
    groups = group_rows(selected, args.group, shader_evidence)
    if args.csv_output:
        write_csv(args.csv_output, groups)
    if args.output:
        write_markdown(args.output, groups, args.probe_csv, args.msl_dir)
    if not args.csv_output and not args.output:
        for group in groups:
            print(
                "|".join(group.key),
                group.draws,
                group.primitives,
                alpha_evidence(group),
                static_blend_off_verdict(group),
                sep=",",
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
