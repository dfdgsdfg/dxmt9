#!/usr/bin/env python3
"""Match Xcode+dxmt top encoder rows to dumped Metal shader sources.

The 3DMark05 perf probe can dump translated MSL into
`traces/<run-id>/analysis/shaders/msl`. This script reads the joined Xcode/dxmt
encoder CSV, matches shader hash plus optional source hash attribution against
`*-shader-<hash>-source-<source>.metal`, and emits a compact report for top GPU
encoders.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


SHADER_FILE_RE = re.compile(
    r"^(?P<label>.+)-shader-(?P<shader>\d+)-source-(?P<source>\d+)\.metal$"
)
ARRAY_RE = re.compile(r"\bfloat4\s+(?P<name>[rc])\s*\[\s*(?P<count>\d+)\s*\]")
OUT_TEXCOORD_ARRAY_RE = re.compile(
    r"\bfloat4\s+outTexcoord\s*\[\s*(?P<count>\d+)\s*\]"
)
VSOUT_RE = re.compile(r"\bstruct\s+VSOut\s*\{(?P<body>.*?)\};", re.S)
ASSIGN_RE_TEMPLATE = r"\b[A-Za-z_][A-Za-z0-9_]*\.{field}\s*="
TEXCOORD_SELECT_RE = re.compile(
    r"\bdxmt9_select_texcoord(?:_h)?\s*\(\s*in\s*,\s*(?P<index>\d+)u?\s*\)"
)

OUTPUT_FIELDS = (
    "rank",
    "seq",
    "enc",
    "gpu_ms",
    "gpu_share_pct",
    "vs_buffer_write_mib",
    "vs_buffer_bytes_per_vs_invocation",
    "dxmt_vs_buffer_to_expected_stage_out_ratio",
    "dxmt_vertex_shader_last",
    "dxmt_vertex_shader_source_last",
    "vs_candidate_count",
    "vs_source_hash",
    "vs_file",
    "vs_label",
    "vs_lines",
    "vs_bytes",
    "vs_temp_r_count",
    "vs_temp_literal_index_count",
    "vs_temp_literal_max_index",
    "vs_temp_literal_span",
    "vs_temp_dynamic_access_count",
    "vs_temp_relative_access_count",
    "vs_temp_zero_init_bytes",
    "vs_temp_overdeclared_literal_bytes",
    "vs_out_texcoord_count",
    "vs_out_texcoord_literal_index_count",
    "vs_out_texcoord_literal_max_index",
    "vs_out_texcoord_literal_span",
    "vs_out_texcoord_dynamic_access_count",
    "vs_out_texcoord_relative_access_count",
    "vs_out_texcoord_zero_init_bytes",
    "vs_out_texcoord_overdeclared_literal_bytes",
    "vs_const_c_count",
    "vsout_field_count",
    "vsout_estimated_bytes",
    "vs_buffer_to_msl_vsout_ratio",
    "vsout_fields",
    "vsout_field_types",
    "vsout_write_count",
    "dxmt_pixel_shader_last",
    "dxmt_pixel_shader_source_last",
    "ps_candidate_count",
    "ps_source_hash",
    "ps_file",
    "ps_label",
    "ps_lines",
    "ps_bytes",
    "ps_temp_r_count",
    "ps_sample_calls",
    "ps_discard_count",
    "ps_vsout_read_field_count",
    "ps_vsout_read_fields",
    "ps_texcoord_read_mask",
    "vsout_unread_field_count",
    "vsout_unread_fields",
    "vsout_unread_estimated_bytes",
    "vsout_unread_estimated_share",
    "branch_count",
    "vs_branch_count",
    "texture_mentions",
    "missing_reason",
)


@dataclass(frozen=True)
class ShaderDump:
    label: str
    shader_hash: int
    source_hash: int
    path: Path


def parse_number(value: Any) -> float:
    try:
        return float(str(value))
    except (TypeError, ValueError):
        return 0.0


def parse_hash(value: Any) -> int:
    text = str(value or "").strip()
    if not text:
        return 0
    try:
        if text.startswith(("0x", "0X")):
            return int(text, 16)
        return int(text)
    except ValueError:
        return 0


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def index_shader_dumps(shader_dir: Path) -> dict[int, list[ShaderDump]]:
    indexed: dict[int, list[ShaderDump]] = {}
    if not shader_dir.exists():
        return indexed
    for path in sorted(shader_dir.glob("*.metal")):
        match = SHADER_FILE_RE.match(path.name)
        if not match:
            continue
        shader = ShaderDump(
            label=match.group("label"),
            shader_hash=int(match.group("shader")),
            source_hash=int(match.group("source")),
            path=path,
        )
        indexed.setdefault(shader.shader_hash, []).append(shader)
    return indexed


def stage_candidates(indexed: dict[int, list[ShaderDump]], shader_hash: int,
                     source_hash: int, stage: str) -> list[ShaderDump]:
    candidates = indexed.get(shader_hash, [])
    if not candidates:
        return []
    if stage == "vs":
        preferred = [item for item in candidates if item.label.endswith("-vs")]
    else:
        preferred = [item for item in candidates if item.label.endswith("-fs")]
    staged = preferred or candidates
    if source_hash:
        exact = [item for item in staged if item.source_hash == source_hash]
        if exact:
            return exact
    return staged


def choose_dump(candidates: list[ShaderDump]) -> ShaderDump | None:
    return candidates[0] if candidates else None


def parse_vsout_fields(text: str) -> list[tuple[str, str]]:
    match = VSOUT_RE.search(text)
    if not match:
        return []
    fields: list[tuple[str, str]] = []
    for raw in match.group("body").splitlines():
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        line = line.split("//", 1)[0].strip().rstrip(";")
        if not line:
            continue
        if "[[" in line:
            line = line.split("[[", 1)[0].strip()
        parts = line.split()
        if len(parts) >= 2:
            fields.append((parts[-2].strip("*&"), parts[-1].strip("*&")))
    return fields


def estimate_msl_type_bytes(type_name: str) -> int:
    clean = type_name.replace("const", "").replace("thread", "").strip()
    array_count = 1
    array_match = re.search(r"\[(\d+)\]$", clean)
    if array_match:
        array_count = int(array_match.group(1))
        clean = clean[:array_match.start()].strip()

    match = re.match(r"(?P<base>half|float|int|uint|short|ushort|char|uchar|bool)(?P<count>[1-4])?$", clean)
    if not match:
        return 0
    base = match.group("base")
    count = int(match.group("count") or "1")
    base_bytes = {
        "half": 2,
        "short": 2,
        "ushort": 2,
        "char": 1,
        "uchar": 1,
        "bool": 1,
        "float": 4,
        "int": 4,
        "uint": 4,
    }[base]
    return base_bytes * count * array_count


def count_vsout_writes(text: str, fields: list[tuple[str, str]]) -> int:
    count = 0
    for _, name in fields:
        pattern = ASSIGN_RE_TEMPLATE.format(field=re.escape(name))
        count += len(re.findall(pattern, text))
    return count


def remove_function_bodies(text: str, names: tuple[str, ...]) -> str:
    out = text
    for name in names:
        while True:
            pattern = (
                r"\b(?:inline\s+)?(?:half4|float4)\s+"
                + re.escape(name) + r"\s*\("
            )
            match = re.search(pattern, out)
            if not match:
                break
            open_brace = out.find("{", match.end())
            if open_brace < 0:
                break
            depth = 0
            close_brace = -1
            for index in range(open_brace, len(out)):
                if out[index] == "{":
                    depth += 1
                elif out[index] == "}":
                    depth -= 1
                    if depth == 0:
                        close_brace = index + 1
                        break
            if close_brace < 0:
                break
            line_start = out.rfind("\n", 0, match.start()) + 1
            out = out[:line_start] + out[close_brace:]
    return out


def stage_in_read_fields(text: str, fields: list[tuple[str, str]]) -> tuple[list[str], int]:
    body = remove_function_bodies(
        text, ("dxmt9_select_texcoord", "dxmt9_select_texcoord_h")
    )
    field_names = {name for _, name in fields}
    reads: set[str] = set()
    texcoord_mask = 0
    for match in TEXCOORD_SELECT_RE.finditer(body):
        index = int(match.group("index"))
        name = f"texcoord{index}"
        reads.add(name)
        if index < 32:
            texcoord_mask |= 1 << index
    for name in field_names:
        if re.search(r"\bin\s*\.\s*" + re.escape(name) + r"\b", body):
            reads.add(name)
            if name.startswith("texcoord"):
                suffix = name[len("texcoord"):]
                if suffix.isdigit() and int(suffix) < 32:
                    texcoord_mask |= 1 << int(suffix)
    return sorted(reads), texcoord_mask


def estimate_field_bytes(fields: list[tuple[str, str]], names: set[str]) -> int:
    return sum(
        estimate_msl_type_bytes(type_name)
        for type_name, name in fields
        if name in names
    )


def temp_register_metrics(text: str) -> dict[str, int | str]:
    literal_indices: set[int] = set()
    dynamic_access_count = 0
    relative_access_count = 0
    declared_count = 0

    for match in ARRAY_RE.finditer(text):
        if match.group("name") == "r":
            declared_count = max(declared_count, int(match.group("count")))

    zero_init_bytes = 0
    zero_init = re.search(
        r"for\s*\(\s*uint\s+i\s*=\s*0\s*;\s*i\s*<\s*(?P<count>\d+)u\s*;"
        r"\s*\+\+i\s*\)\s*\{\s*r\s*\[\s*i\s*\]\s*=\s*float4\s*\(\s*0\.0f\s*\)\s*;\s*\}",
        text,
    )
    if zero_init:
        zero_init_bytes = int(zero_init.group("count")) * 16

    for line in text.splitlines():
        if re.search(r"\bfloat4\s+r\s*\[", line):
            continue
        if re.search(r"\br\s*\[\s*i\s*\]\s*=\s*float4\s*\(\s*0\.0f\s*\)", line):
            continue
        for match in re.finditer(r"\br\s*\[\s*(?P<index>[^\]]+)\]", line):
            index = match.group("index").strip()
            if index.isdigit():
                literal_indices.add(int(index))
            else:
                dynamic_access_count += 1
                if "clamp(" in index:
                    relative_access_count += 1

    max_literal = max(literal_indices) if literal_indices else -1
    literal_span = max_literal + 1 if max_literal >= 0 else 0
    return {
        "temp_literal_index_count": len(literal_indices),
        "temp_literal_max_index": max_literal if max_literal >= 0 else "",
        "temp_literal_span": literal_span,
        "temp_dynamic_access_count": dynamic_access_count,
        "temp_relative_access_count": relative_access_count,
        "temp_zero_init_bytes": zero_init_bytes or declared_count * 16,
        "temp_overdeclared_literal_bytes": max(0, declared_count - literal_span) * 16,
    }


def vertex_output_scratch_metrics(text: str) -> dict[str, int | str]:
    literal_indices: set[int] = set()
    dynamic_access_count = 0
    relative_access_count = 0
    declared_count = 0

    for match in OUT_TEXCOORD_ARRAY_RE.finditer(text):
        declared_count = max(declared_count, int(match.group("count")))

    zero_init_bytes = 0
    zero_init = re.search(
        r"for\s*\(\s*uint\s+i\s*=\s*0\s*;\s*i\s*<\s*(?P<count>\d+)u\s*;"
        r"\s*\+\+i\s*\)\s*\{\s*outTexcoord\s*\[\s*i\s*\]\s*=\s*float4\s*\("
        r"\s*0\.0f\s*,\s*0\.0f\s*,\s*0\.0f\s*,\s*1\.0f\s*\)\s*;\s*\}",
        text,
    )
    if zero_init:
        zero_init_bytes = int(zero_init.group("count")) * 16

    for line in text.splitlines():
        if OUT_TEXCOORD_ARRAY_RE.search(line):
            continue
        if re.search(
            r"\boutTexcoord\s*\[\s*i\s*\]\s*=\s*float4\s*\("
            r"\s*0\.0f\s*,\s*0\.0f\s*,\s*0\.0f\s*,\s*1\.0f\s*\)",
            line,
        ):
            continue
        for match in re.finditer(r"\boutTexcoord\s*\[\s*(?P<index>[^\]]+)\]", line):
            index = match.group("index").strip()
            if index.isdigit():
                literal_indices.add(int(index))
            else:
                dynamic_access_count += 1
                if "clamp(" in index:
                    relative_access_count += 1

    max_literal = max(literal_indices) if literal_indices else -1
    literal_span = max_literal + 1 if max_literal >= 0 else 0
    return {
        "out_texcoord_count": declared_count,
        "out_texcoord_literal_index_count": len(literal_indices),
        "out_texcoord_literal_max_index": max_literal if max_literal >= 0 else "",
        "out_texcoord_literal_span": literal_span,
        "out_texcoord_dynamic_access_count": dynamic_access_count,
        "out_texcoord_relative_access_count": relative_access_count,
        "out_texcoord_zero_init_bytes": zero_init_bytes or declared_count * 16,
        "out_texcoord_overdeclared_literal_bytes": max(0, declared_count - literal_span) * 16,
    }


def safe_ratio(numerator: float, denominator: float) -> float | str:
    return (numerator / denominator) if denominator else ""


def shader_metrics(dump: ShaderDump | None) -> dict[str, Any]:
    if dump is None:
        return {}
    text = dump.path.read_text(encoding="utf-8", errors="replace")
    arrays: dict[str, int] = {}
    for match in ARRAY_RE.finditer(text):
        arrays[match.group("name")] = max(
            arrays.get(match.group("name"), 0),
            int(match.group("count")),
        )
    vsout_fields = parse_vsout_fields(text)
    vsout_names = [name for _, name in vsout_fields]
    vsout_types = [type_name for type_name, _ in vsout_fields]
    temp_metrics = temp_register_metrics(text)
    output_scratch_metrics = vertex_output_scratch_metrics(text)
    return {
        "file": dump.path.name,
        "label": dump.label,
        "lines": len(text.splitlines()),
        "bytes": len(text.encode("utf-8")),
        "temp_r_count": arrays.get("r", 0),
        **temp_metrics,
        **output_scratch_metrics,
        "const_c_count": arrays.get("c", 0),
        "vsout_field_count": len(vsout_fields),
        "vsout_estimated_bytes": sum(
            estimate_msl_type_bytes(type_name) for type_name in vsout_types
        ),
        "vsout_fields": ",".join(vsout_names),
        "vsout_field_types": ",".join(vsout_types),
        "vsout_write_count": count_vsout_writes(text, vsout_fields),
        "sample_calls": text.count(".sample(") + text.count(".sample_compare("),
        "discard_count": text.count("discard_fragment"),
        "branch_count": text.count("if (") + text.count("for (") + text.count("while ("),
        "texture_mentions": text.count("texture2d") + text.count("texturecube"),
    }


def summarize_row(rank: int, row: dict[str, str],
                  indexed: dict[int, list[ShaderDump]]) -> dict[str, Any]:
    vs_hash = parse_hash(row.get("dxmt_vertex_shader_last"))
    ps_hash = parse_hash(row.get("dxmt_pixel_shader_last"))
    vs_source_hash = parse_hash(row.get("dxmt_vertex_shader_source_last"))
    ps_source_hash = parse_hash(row.get("dxmt_pixel_shader_source_last"))
    vs_candidates = stage_candidates(indexed, vs_hash, vs_source_hash, "vs")
    ps_candidates = stage_candidates(indexed, ps_hash, ps_source_hash, "ps")
    vs_dump = choose_dump(vs_candidates)
    ps_dump = choose_dump(ps_candidates)
    vs = shader_metrics(vs_dump)
    ps = shader_metrics(ps_dump)
    vs_fields = parse_vsout_fields(
        vs_dump.path.read_text(encoding="utf-8", errors="replace")
        if vs_dump else ""
    )
    ps_text = (
        ps_dump.path.read_text(encoding="utf-8", errors="replace")
        if ps_dump else ""
    )
    ps_vsout_reads, ps_texcoord_read_mask = stage_in_read_fields(ps_text, vs_fields)
    vs_field_names = {name for _, name in vs_fields}
    ps_read_names = set(ps_vsout_reads)
    unread_fields = sorted(vs_field_names - ps_read_names - {"position"})
    vsout_estimated_bytes = parse_number(vs.get("vsout_estimated_bytes", 0))
    vsout_unread_estimated_bytes = estimate_field_bytes(vs_fields, set(unread_fields))
    vs_buffer_bytes_per_invocation = parse_number(
        row.get("vs_buffer_bytes_per_vs_invocation")
    )

    missing: list[str] = []
    if vs_hash == 0:
        missing.append("zero_vs_hash")
    elif vs_dump is None:
        missing.append("missing_vs_dump")
    elif vs_source_hash and vs_dump.source_hash != vs_source_hash:
        missing.append("missing_vs_source_dump")
    elif len(vs_candidates) > 1:
        missing.append("ambiguous_vs_dump")
    if ps_hash == 0:
        missing.append("zero_ps_hash")
    elif ps_dump is None:
        missing.append("missing_ps_dump")
    elif ps_source_hash and ps_dump.source_hash != ps_source_hash:
        missing.append("missing_ps_source_dump")
    elif len(ps_candidates) > 1:
        missing.append("ambiguous_ps_dump")

    return {
        "rank": rank,
        "seq": row.get("seq", ""),
        "enc": row.get("enc", ""),
        "gpu_ms": row.get("gpu_ms", ""),
        "gpu_share_pct": row.get("gpu_share_pct", ""),
        "vs_buffer_write_mib": row.get("vs_buffer_write_mib", ""),
        "vs_buffer_bytes_per_vs_invocation": row.get(
            "vs_buffer_bytes_per_vs_invocation", ""
        ),
        "dxmt_vs_buffer_to_expected_stage_out_ratio": row.get(
            "dxmt_vs_buffer_to_expected_stage_out_ratio", ""
        ),
        "dxmt_vertex_shader_last": row.get("dxmt_vertex_shader_last", ""),
        "dxmt_vertex_shader_source_last": row.get(
            "dxmt_vertex_shader_source_last", ""
        ),
        "vs_candidate_count": len(vs_candidates),
        "vs_source_hash": str(vs_dump.source_hash) if vs_dump else "",
        "vs_file": vs.get("file", ""),
        "vs_label": vs.get("label", ""),
        "vs_lines": vs.get("lines", ""),
        "vs_bytes": vs.get("bytes", ""),
        "vs_temp_r_count": vs.get("temp_r_count", ""),
        "vs_temp_literal_index_count": vs.get("temp_literal_index_count", ""),
        "vs_temp_literal_max_index": vs.get("temp_literal_max_index", ""),
        "vs_temp_literal_span": vs.get("temp_literal_span", ""),
        "vs_temp_dynamic_access_count": vs.get("temp_dynamic_access_count", ""),
        "vs_temp_relative_access_count": vs.get("temp_relative_access_count", ""),
        "vs_temp_zero_init_bytes": vs.get("temp_zero_init_bytes", ""),
        "vs_temp_overdeclared_literal_bytes": vs.get(
            "temp_overdeclared_literal_bytes", ""
        ),
        "vs_out_texcoord_count": vs.get("out_texcoord_count", ""),
        "vs_out_texcoord_literal_index_count": vs.get(
            "out_texcoord_literal_index_count", ""
        ),
        "vs_out_texcoord_literal_max_index": vs.get(
            "out_texcoord_literal_max_index", ""
        ),
        "vs_out_texcoord_literal_span": vs.get("out_texcoord_literal_span", ""),
        "vs_out_texcoord_dynamic_access_count": vs.get(
            "out_texcoord_dynamic_access_count", ""
        ),
        "vs_out_texcoord_relative_access_count": vs.get(
            "out_texcoord_relative_access_count", ""
        ),
        "vs_out_texcoord_zero_init_bytes": vs.get(
            "out_texcoord_zero_init_bytes", ""
        ),
        "vs_out_texcoord_overdeclared_literal_bytes": vs.get(
            "out_texcoord_overdeclared_literal_bytes", ""
        ),
        "vs_const_c_count": vs.get("const_c_count", ""),
        "vsout_field_count": vs.get("vsout_field_count", ""),
        "vsout_estimated_bytes": vs.get("vsout_estimated_bytes", ""),
        "vs_buffer_to_msl_vsout_ratio": safe_ratio(
            vs_buffer_bytes_per_invocation, vsout_estimated_bytes),
        "vsout_fields": vs.get("vsout_fields", ""),
        "vsout_field_types": vs.get("vsout_field_types", ""),
        "vsout_write_count": vs.get("vsout_write_count", ""),
        "dxmt_pixel_shader_last": row.get("dxmt_pixel_shader_last", ""),
        "dxmt_pixel_shader_source_last": row.get(
            "dxmt_pixel_shader_source_last", ""
        ),
        "ps_candidate_count": len(ps_candidates),
        "ps_source_hash": str(ps_dump.source_hash) if ps_dump else "",
        "ps_file": ps.get("file", ""),
        "ps_label": ps.get("label", ""),
        "ps_lines": ps.get("lines", ""),
        "ps_bytes": ps.get("bytes", ""),
        "ps_temp_r_count": ps.get("temp_r_count", ""),
        "ps_sample_calls": ps.get("sample_calls", ""),
        "ps_discard_count": ps.get("discard_count", ""),
        "ps_vsout_read_field_count": len(ps_vsout_reads) if ps_dump else "",
        "ps_vsout_read_fields": ",".join(ps_vsout_reads),
        "ps_texcoord_read_mask": f"0x{ps_texcoord_read_mask:x}" if ps_dump else "",
        "vsout_unread_field_count": len(unread_fields) if ps_dump else "",
        "vsout_unread_fields": ",".join(unread_fields),
        "vsout_unread_estimated_bytes": (
            vsout_unread_estimated_bytes if ps_dump else ""),
        "vsout_unread_estimated_share": (
            safe_ratio(vsout_unread_estimated_bytes, vsout_estimated_bytes)
            if ps_dump else ""),
        "branch_count": parse_number(vs.get("branch_count", 0)) +
        parse_number(ps.get("branch_count", 0)),
        "vs_branch_count": vs.get("branch_count", ""),
        "texture_mentions": parse_number(vs.get("texture_mentions", 0)) +
        parse_number(ps.get("texture_mentions", 0)),
        "missing_reason": ",".join(missing),
    }


def fmt(value: Any) -> str:
    if value == "":
        return ""
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=OUTPUT_FIELDS, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def write_report(path: Path, joined_csv: Path, shader_dir: Path,
                 rows: list[dict[str, Any]], indexed_count: int) -> None:
    matched_vs = sum(1 for row in rows if row.get("vs_file"))
    matched_ps = sum(1 for row in rows if row.get("ps_file"))
    lines: list[str] = []
    lines.append("# Shader Dump Encoder Report")
    lines.append("")
    lines.append(f"- Joined CSV: `{joined_csv}`")
    lines.append(f"- Shader dir: `{shader_dir}`")
    lines.append(f"- Indexed shader dumps: `{indexed_count}`")
    lines.append(f"- Top rows: `{len(rows)}`")
    lines.append(f"- Matched VS / PS: `{matched_vs}` / `{matched_ps}`")
    lines.append("")
    lines.append("## Top Encoder Shader Rows")
    lines.append("")
    header = [
        "rank", "seq", "enc", "GPU ms", "VS write MiB", "VS B/inv",
        "DXMT VS/VSOut", "MSL VS/VSOut", "VS hash", "VS candidates",
        "VS source", "VS file", "VS lines", "VS r[]",
        "VS temp span", "VS temp dyn/rel", "VS temp zero B",
        "VS temp over B", "VS outT[]", "VS outT span", "VS outT dyn/rel",
        "VS outT zero B", "VS outT over B", "VSOut fields", "VSOut bytes",
        "VSOut writes", "PS hash", "PS candidates",
        "PS source", "PS file", "PS lines", "PS samples", "PS reads",
        "PS read fields", "unread fields", "unread bytes", "unread share",
        "branches", "missing",
    ]
    lines.append("| " + " | ".join(header) + " |")
    lines.append("|" + "|".join("---:" for _ in header) + "|")
    for row in rows:
        lines.append(
            "| "
            + " | ".join([
                fmt(row.get("rank", "")),
                fmt(row.get("seq", "")),
                fmt(row.get("enc", "")),
                fmt(row.get("gpu_ms", "")),
                fmt(row.get("vs_buffer_write_mib", "")),
                fmt(row.get("vs_buffer_bytes_per_vs_invocation", "")),
                fmt(row.get("dxmt_vs_buffer_to_expected_stage_out_ratio", "")),
                fmt(row.get("vs_buffer_to_msl_vsout_ratio", "")),
                fmt(row.get("dxmt_vertex_shader_last", "")),
                fmt(row.get("vs_candidate_count", "")),
                fmt(row.get("vs_source_hash", "")),
                fmt(row.get("vs_file", "")),
                fmt(row.get("vs_lines", "")),
                fmt(row.get("vs_temp_r_count", "")),
                fmt(row.get("vs_temp_literal_span", "")),
                (
                    f"{fmt(row.get('vs_temp_dynamic_access_count', ''))}/"
                    f"{fmt(row.get('vs_temp_relative_access_count', ''))}"
                ),
                fmt(row.get("vs_temp_zero_init_bytes", "")),
                fmt(row.get("vs_temp_overdeclared_literal_bytes", "")),
                fmt(row.get("vs_out_texcoord_count", "")),
                fmt(row.get("vs_out_texcoord_literal_span", "")),
                (
                    f"{fmt(row.get('vs_out_texcoord_dynamic_access_count', ''))}/"
                    f"{fmt(row.get('vs_out_texcoord_relative_access_count', ''))}"
                ),
                fmt(row.get("vs_out_texcoord_zero_init_bytes", "")),
                fmt(row.get("vs_out_texcoord_overdeclared_literal_bytes", "")),
                fmt(row.get("vsout_field_count", "")),
                fmt(row.get("vsout_estimated_bytes", "")),
                fmt(row.get("vsout_write_count", "")),
                fmt(row.get("dxmt_pixel_shader_last", "")),
                fmt(row.get("ps_candidate_count", "")),
                fmt(row.get("ps_source_hash", "")),
                fmt(row.get("ps_file", "")),
                fmt(row.get("ps_lines", "")),
                fmt(row.get("ps_sample_calls", "")),
                fmt(row.get("ps_vsout_read_field_count", "")),
                fmt(row.get("ps_vsout_read_fields", "")),
                fmt(row.get("vsout_unread_fields", "")),
                fmt(row.get("vsout_unread_estimated_bytes", "")),
                fmt(row.get("vsout_unread_estimated_share", "")),
                fmt(row.get("branch_count", "")),
                fmt(row.get("missing_reason", "")),
            ])
            + " |"
        )
    lines.append("")
    lines.append("## Notes")
    lines.append("")
    if indexed_count == 0:
        lines.append("- No MSL dumps were found. Re-run the probe with `--dump-shaders`.")
    if any("zero_vs_hash" in str(row.get("missing_reason")) for row in rows):
        lines.append("- Some rows have zero shader hashes; the dxmt log predates shader attribution.")
    actual_missing = [
        row for row in rows
        if any(
            token in str(row.get("missing_reason"))
            for token in (
                "zero_vs_hash",
                "zero_ps_hash",
                "missing_vs_dump",
                "missing_ps_dump",
                "missing_vs_source_dump",
                "missing_ps_source_dump",
            )
        )
    ]
    if actual_missing:
        lines.append(
            "- Missing rows usually mean the capture was not run with matching dumped shaders, "
            "or the top encoder used an old log without shader hashes."
        )
    ambiguous = [
        row for row in rows
        if "ambiguous_" in str(row.get("missing_reason"))
    ]
    if ambiguous:
        lines.append(
            "- Ambiguous rows have more than one dumped source for the same shader hash. "
            "The table shows the first sorted candidate, so inspect the candidate count "
            "and source hash before making per-source conclusions."
        )
    high_vsout_ratio = [
        row for row in rows
        if parse_number(row.get("vs_buffer_to_msl_vsout_ratio")) >= 4.0
    ]
    if high_vsout_ratio:
        lines.append(
            "- Rows with high `MSL VS/VSOut` ratios write far more VS-buffer "
            "traffic per invocation than the source-visible `VSOut` struct width. "
            "Treat those as spill/internal vertex-stage traffic candidates before "
            "blaming ordinary varying width."
        )
    high_out_texcoord_overdecl = [
        row for row in rows
        if parse_number(row.get("vs_out_texcoord_overdeclared_literal_bytes")) > 0
    ]
    if high_out_texcoord_overdecl:
        lines.append(
            "- Rows with nonzero `VS outT over B` still declare more local "
            "`outTexcoord[]` scratch than the literal source references require. "
            "Those rows are candidates for `DXMT9_TRIM_VS_OUTPUT_SCRATCH=1`."
        )
    high_unread_share = [
        row for row in rows
        if parse_number(row.get("vsout_unread_estimated_share")) >= 0.25
    ]
    if high_unread_share:
        lines.append(
            "- Rows with high unread `VSOut` share are viable varying-trim "
            "candidates, provided the paired PSO liveness gate keeps every "
            "fragment-read field."
        )
    if not rows:
        lines.append("- The joined CSV had no rows.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("joined_csv", type=Path, help="Xcode+dxmt joined CSV")
    parser.add_argument(
        "--shader-dir",
        type=Path,
        required=True,
        help="Directory containing dumped .metal files",
    )
    parser.add_argument("--output", type=Path, required=True, help="Markdown report path")
    parser.add_argument("--csv-output", type=Path, required=True, help="Summary CSV path")
    parser.add_argument("--top", type=int, default=10, help="Top GPU rows to inspect")
    parser.add_argument(
        "--require-matches",
        action="store_true",
        help="Fail if top render rows have zero shader hashes or do not match dumped MSL files",
    )
    args = parser.parse_args()

    if args.top <= 0:
        raise SystemExit("--top must be positive")
    rows = read_rows(args.joined_csv)
    rows.sort(key=lambda item: parse_number(item.get("gpu_ms")), reverse=True)
    indexed = index_shader_dumps(args.shader_dir)
    summary = [
        summarize_row(rank, row, indexed)
        for rank, row in enumerate(rows[:args.top], start=1)
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_csv(args.csv_output, summary)
    write_report(
        args.output,
        args.joined_csv,
        args.shader_dir,
        summary,
        sum(len(items) for items in indexed.values()),
    )
    print(args.output)
    print(args.csv_output)

    if args.require_matches:
        failures = [
            row for row in summary
            if str(row.get("seq", "")) and str(row.get("enc", "")) and (
                "zero_vs_hash" in str(row.get("missing_reason")) or
                "zero_ps_hash" in str(row.get("missing_reason")) or
                "missing_vs_dump" in str(row.get("missing_reason")) or
                "missing_ps_dump" in str(row.get("missing_reason")) or
                "missing_vs_source_dump" in str(row.get("missing_reason")) or
                "missing_ps_source_dump" in str(row.get("missing_reason")) or
                "ambiguous_vs_dump" in str(row.get("missing_reason")) or
                "ambiguous_ps_dump" in str(row.get("missing_reason"))
            )
        ]
        if failures:
            print(
                f"missing or ambiguous shader attribution/dump matches for "
                f"{len(failures)} top render rows",
                file=sys.stderr,
            )
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
