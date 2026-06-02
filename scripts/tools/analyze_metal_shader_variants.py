#!/usr/bin/env python3
"""Compile structural MSL variants for hot 3DMark05 vertex shaders.

This is an offline classifier. It rewrites dumped MSL vertex shaders to keep a
smaller `VSOut` shape, compiles each variant with Apple's Metal toolchain, and
reports compiler-visible IR changes. Runtime Xcode counters are still required
before promoting any variant as a performance fix.
"""

from __future__ import annotations

import argparse
import csv
import re
import tempfile
from pathlib import Path
from typing import Any

from analyze_metal_shader_codegen import compile_shader, parse_int, parse_number, read_rows


OUTPUT_FIELDS = (
    "rank",
    "variant",
    "seq",
    "enc",
    "gpu_ms",
    "vs_buffer_write_mib",
    "vs_buffer_bytes_per_vs_invocation",
    "source_file",
    "variant_file",
    "kept_fields",
    "removed_fields",
    "source_vsout_bytes",
    "variant_vsout_bytes",
    "compile_ok",
    "warning_count",
    "air_bytes",
    "metallib_text_bytes",
    "ir_return_field_count",
    "ir_return_bytes",
    "ir_alloca_count",
    "ir_alloca_bytes",
    "ir_scratch_bytes_estimate",
    "ir_lifetime_start_bytes",
    "ir_lifetime_end_bytes",
    "ir_memcpy_count",
    "ir_memset_count",
    "ir_instruction_lines",
    "vs_buffer_to_ir_return_ratio",
    "vs_buffer_to_ir_alloca_ratio",
    "vs_buffer_to_ir_scratch_ratio",
    "missing_reason",
)

VSOUT_RE = re.compile(r"\bstruct\s+VSOut\s*\{(?P<body>.*?)\};", re.S)
SELECT_TEXCOORD_RE = re.compile(
    r"inline\s+float4\s+dxmt9_select_texcoord\s*\(\s*VSOut\s+in\s*,\s*uint\s+index\s*\)\s*\{.*?\n\}",
    re.S,
)
OUT_ASSIGN_RE = re.compile(r"^\s*out\.(?P<field>[A-Za-z_][A-Za-z0-9_]*)\s*=")
FIELD_NAME_RE = re.compile(r"\b(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*(?:\[\[|;)")

FIELD_BYTES = {
    "float": 4,
    "float2": 8,
    "float3": 12,
    "float4": 16,
    "half": 2,
    "half2": 4,
    "half3": 6,
    "half4": 8,
}


def csv_list(value: str) -> list[str]:
    return [part.strip() for part in value.split(",") if part.strip()]


def field_name(line: str) -> str:
    match = FIELD_NAME_RE.search(line)
    return match.group("name") if match else ""


def field_type(line: str) -> str:
    return line.strip().split(None, 1)[0] if line.strip() else ""


def field_size(line: str) -> int:
    return FIELD_BYTES.get(field_type(line), 0)


def parse_vsout(text: str) -> tuple[str, list[str]]:
    match = VSOUT_RE.search(text)
    if not match:
        return "", []
    lines = [line for line in match.group("body").splitlines() if field_name(line)]
    return match.group(0), lines


def estimate_vsout_bytes(lines: list[str], fields: set[str] | None = None) -> int:
    total = 0
    for line in lines:
        name = field_name(line)
        if fields is not None and name not in fields:
            continue
        total += field_size(line)
    return total


def select_texcoord_function(kept_fields: set[str]) -> str:
    cases = []
    for index in range(8):
        name = f"texcoord{index}"
        if name in kept_fields:
            cases.append(f"    case {index}u: return in.{name};")
    if not cases:
        return (
            "inline float4 dxmt9_select_texcoord(VSOut in, uint index) {\n"
            "  (void)in;\n"
            "  (void)index;\n"
            "  return float4(0.0f);\n"
            "}"
        )
    body = "\n".join(cases)
    return (
        "inline float4 dxmt9_select_texcoord(VSOut in, uint index) {\n"
        "  switch (index) {\n"
        f"{body}\n"
        "    default: return float4(0.0f);\n"
        "  }\n"
        "}"
    )


def rewrite_vsout(text: str, kept_fields: set[str]) -> tuple[str, list[str], list[str], int, int]:
    original_struct, lines = parse_vsout(text)
    if not original_struct:
        return text, [], [], 0, 0

    all_fields = [field_name(line) for line in lines]
    kept = [name for name in all_fields if name in kept_fields]
    removed = [name for name in all_fields if name not in kept_fields]
    kept_lines = [line for line in lines if field_name(line) in kept_fields]
    new_struct = "struct VSOut {\n" + "\n".join(kept_lines) + "\n};"
    rewritten = text.replace(original_struct, new_struct, 1)
    rewritten = SELECT_TEXCOORD_RE.sub(select_texcoord_function(kept_fields), rewritten, count=1)

    filtered_lines: list[str] = []
    removed_set = set(removed)
    for line in rewritten.splitlines():
        assign = OUT_ASSIGN_RE.match(line)
        if assign and assign.group("field") in removed_set:
            continue
        filtered_lines.append(line)

    return (
        "\n".join(filtered_lines) + "\n",
        kept,
        removed,
        estimate_vsout_bytes(lines),
        estimate_vsout_bytes(lines, set(kept)),
    )


def variant_fields(row: dict[str, str], variant: str) -> set[str]:
    all_fields = set(csv_list(row.get("vsout_fields", "")))
    if variant == "original":
        return all_fields
    if variant == "position-only":
        return {"position"}
    if variant == "live-vsout":
        live = set(csv_list(row.get("ps_vsout_read_fields", "")))
        live.add("position")
        return live
    raise ValueError(f"unknown variant: {variant}")


def build_variant_file(
    row: dict[str, str],
    shader_dir: Path,
    output_dir: Path,
    variant: str,
) -> tuple[Path, list[str], list[str], int, int, str]:
    source_file = shader_dir / row.get("vs_file", "")
    if variant == "original":
        text = source_file.read_text(encoding="utf-8")
        _, lines = parse_vsout(text)
        fields = [field_name(line) for line in lines]
        bytes_ = estimate_vsout_bytes(lines)
        return source_file, fields, [], bytes_, bytes_, ""

    text = source_file.read_text(encoding="utf-8")
    rewritten, kept, removed, source_bytes, variant_bytes = rewrite_vsout(
        text, variant_fields(row, variant)
    )
    if not kept:
        return source_file, kept, removed, source_bytes, variant_bytes, "vsout_rewrite_failed"

    out_path = output_dir / f"rank{parse_int(row.get('rank'))}-{variant}-{source_file.name}"
    out_path.write_text(rewritten, encoding="utf-8")
    return out_path, kept, removed, source_bytes, variant_bytes, ""


def make_output_row(
    source_row: dict[str, str],
    variant: str,
    compile_row: dict[str, Any],
    variant_path: Path,
    kept: list[str],
    removed: list[str],
    source_bytes: int,
    variant_bytes: int,
    missing_reason: str,
) -> dict[str, Any]:
    out = {field: "" for field in OUTPUT_FIELDS}
    out.update({
        "rank": source_row.get("rank", ""),
        "variant": variant,
        "seq": source_row.get("seq", ""),
        "enc": source_row.get("enc", ""),
        "gpu_ms": source_row.get("gpu_ms", ""),
        "vs_buffer_write_mib": source_row.get("vs_buffer_write_mib", ""),
        "vs_buffer_bytes_per_vs_invocation": source_row.get("vs_buffer_bytes_per_vs_invocation", ""),
        "source_file": source_row.get("vs_file", ""),
        "variant_file": variant_path.name,
        "kept_fields": ",".join(kept),
        "removed_fields": ",".join(removed),
        "source_vsout_bytes": source_bytes,
        "variant_vsout_bytes": variant_bytes,
        "missing_reason": missing_reason,
    })
    for field in OUTPUT_FIELDS:
        if field in compile_row:
            out[field] = compile_row[field]

    if missing_reason and not out.get("missing_reason"):
        out["missing_reason"] = missing_reason
    bytes_per_vs = parse_number(out.get("vs_buffer_bytes_per_vs_invocation"))
    ir_return_bytes = parse_number(out.get("ir_return_bytes"))
    ir_alloca_bytes = parse_number(out.get("ir_alloca_bytes"))
    ir_scratch_bytes = parse_number(out.get("ir_scratch_bytes_estimate"))
    out["vs_buffer_to_ir_return_ratio"] = (
        bytes_per_vs / ir_return_bytes if bytes_per_vs and ir_return_bytes else 0.0
    )
    out["vs_buffer_to_ir_alloca_ratio"] = (
        bytes_per_vs / ir_alloca_bytes if bytes_per_vs and ir_alloca_bytes else 0.0
    )
    out["vs_buffer_to_ir_scratch_ratio"] = (
        bytes_per_vs / ir_scratch_bytes if bytes_per_vs and ir_scratch_bytes else 0.0
    )
    return out


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=OUTPUT_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in OUTPUT_FIELDS})


def fmt(value: Any) -> str:
    text = str(value)
    if text == "":
        return ""
    try:
        return f"{float(text):.3f}"
    except ValueError:
        return text


def write_report(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Metal Shader Variant Codegen Report",
        "",
        "This is an offline classifier. A runtime Xcode gputrace is still required",
        "before treating a variant as a performance fix.",
        "",
        "| rank | seq/enc | variant | VSOut B | IR return B | IR scratch B | IR alloca B | Xcode/scratch | memcpy/memset | warnings | kept fields | removed fields | missing |",
        "|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|---|",
    ]
    for row in rows:
        lines.append(
            "| {rank} | {seq}/{enc} | `{variant}` | `{vsout}` | `{ret}` | `{scratch}` | `{alloca}` | "
            "`{scratch_ratio}` | `{mem_ops}` | `{warn}` | `{kept}` | `{removed}` | `{missing}` |".format(
                rank=row.get("rank", ""),
                seq=row.get("seq", ""),
                enc=row.get("enc", ""),
                variant=row.get("variant", ""),
                vsout=row.get("variant_vsout_bytes", ""),
                ret=row.get("ir_return_bytes", ""),
                scratch=row.get("ir_scratch_bytes_estimate", ""),
                alloca=row.get("ir_alloca_bytes", ""),
                scratch_ratio=fmt(row.get("vs_buffer_to_ir_scratch_ratio", "")),
                mem_ops=(
                    f"{parse_int(row.get('ir_memcpy_count'))}/"
                    f"{parse_int(row.get('ir_memset_count'))}"
                ),
                warn=row.get("warning_count", ""),
                kept=row.get("kept_fields", ""),
                removed=row.get("removed_fields", ""),
                missing=row.get("missing_reason", ""),
            )
        )

    lines.extend([
        "",
        "## Interpretation",
        "",
        "- `live-vsout` keeps only `position` plus the fields read by the paired fragment shader.",
        "- `position-only` is a lower-bound compiler/backend structural probe, not a correct runtime variant.",
        "- `IR scratch B` is the max of compiler-visible `alloca` and lifetime byte ranges after Metal compilation.",
        "- If IR return bytes shrink but Xcode VS buffer writes remain stable in runtime A/B, the owner is below source-visible VSOut shape.",
    ])
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate and compile structural MSL variants for hot 3DMark05 VS dumps."
    )
    parser.add_argument("shader_summary", type=Path, help="CSV from analyze_shader_dumps.py")
    parser.add_argument("--shader-dir", type=Path, required=True, help="Directory containing dumped .metal files")
    parser.add_argument("--top", type=int, default=3, help="Top rows to process")
    parser.add_argument(
        "--variants",
        default="original,live-vsout,position-only",
        help="Comma-separated variants: original, live-vsout, position-only",
    )
    parser.add_argument("--output", type=Path, required=True, help="Markdown report path")
    parser.add_argument("--csv-output", type=Path, required=True, help="CSV report path")
    parser.add_argument("--variant-dir", type=Path, help="Optional directory to keep generated variant MSL")
    parser.add_argument("--work-dir", type=Path, help="Optional temporary compile work directory")
    parser.add_argument("--keep-temps", action="store_true", help="Keep .air/.metallib outputs in --work-dir")
    args = parser.parse_args()

    input_rows = read_rows(args.shader_summary)
    top_rows = [row for row in input_rows if parse_number(row.get("vs_buffer_write_mib")) > 0.0]
    top_rows = top_rows[: args.top]
    variants = csv_list(args.variants)

    if args.variant_dir:
        variant_context = None
        variant_dir = args.variant_dir
        variant_dir.mkdir(parents=True, exist_ok=True)
    else:
        variant_context = tempfile.TemporaryDirectory(prefix="dxmt9-metal-variants-")
        variant_dir = Path(variant_context.name)

    if args.work_dir:
        work_context = None
        work_dir = args.work_dir
        work_dir.mkdir(parents=True, exist_ok=True)
    else:
        work_context = tempfile.TemporaryDirectory(prefix="dxmt9-metal-variant-codegen-")
        work_dir = Path(work_context.name)

    rows: list[dict[str, Any]] = []
    try:
        for source_row in top_rows:
            for variant in variants:
                variant_path, kept, removed, source_bytes, variant_bytes, missing = build_variant_file(
                    source_row, args.shader_dir, variant_dir, variant
                )
                compile_row: dict[str, Any] = {}
                if not missing:
                    compile_row = compile_shader(variant_path, work_dir, args.keep_temps)
                rows.append(make_output_row(
                    source_row,
                    variant,
                    compile_row,
                    variant_path,
                    kept,
                    removed,
                    source_bytes,
                    variant_bytes,
                    missing,
                ))
    finally:
        if variant_context is not None:
            variant_context.cleanup()
        if work_context is not None:
            work_context.cleanup()

    write_csv(args.csv_output, rows)
    write_report(args.output, rows)
    print(args.output)
    print(args.csv_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
