#!/usr/bin/env python3
"""Compile dumped MSL shaders and summarize Metal compiler IR shape.

The 3DMark05 perf probe can pair hot Xcode encoder rows with dumped MSL via
`analyze_shader_dumps.py`. This script takes that shader-dump summary, compiles
the top shader files with Apple's Metal toolchain, links a temporary metallib,
and extracts a small set of repeatable codegen metrics from `metal-size` and
`metal-objdump`.
"""

from __future__ import annotations

import argparse
import csv
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any


OUTPUT_FIELDS = (
    "rank",
    "stage",
    "seq",
    "enc",
    "gpu_ms",
    "vs_buffer_write_mib",
    "vs_buffer_bytes_per_vs_invocation",
    "shader_file",
    "shader_label",
    "source_bytes",
    "source_lines",
    "compile_ok",
    "warning_count",
    "air_bytes",
    "metallib_bytes",
    "metallib_text_bytes",
    "metallib_data_bytes",
    "metallib_bss_bytes",
    "metallib_dec_bytes",
    "ir_lines",
    "ir_instruction_lines",
    "ir_type_def_count",
    "ir_max_type_def_bytes",
    "ir_return_field_count",
    "ir_return_bytes",
    "ir_alloca_count",
    "ir_alloca_array_count",
    "ir_alloca_struct_count",
    "ir_alloca_bytes",
    "ir_scratch_bytes_estimate",
    "ir_lifetime_start_bytes",
    "ir_lifetime_end_bytes",
    "ir_insertvalue_count",
    "ir_load_count",
    "ir_store_count",
    "ir_memcpy_count",
    "ir_memset_count",
    "ir_call_count",
    "ir_branch_count",
    "ir_select_count",
    "ir_getelementptr_count",
    "ir_phi_count",
    "ir_air_dot_calls",
    "ir_texture_sample_calls",
    "ir_addrspace1_refs",
    "ir_addrspace2_refs",
    "ir_addrspace3_refs",
    "ir_addrspace4_refs",
    "vs_buffer_to_ir_return_ratio",
    "vs_buffer_to_ir_alloca_ratio",
    "vs_buffer_to_ir_scratch_ratio",
    "missing_reason",
)

SIZE_RE = re.compile(
    r"^\s*(?P<text>\d+)\s+(?P<data>\d+)\s+(?P<bss>\d+)\s+"
    r"(?P<dec>\d+)\s+(?P<hex>[0-9a-fA-F]+)\s+(?P<file>.+)$"
)
DEFINE_RE = re.compile(r"^define\s+(?P<ret>.+?)\s+@(?P<name>[A-Za-z0-9_]+)\(")
TYPE_DEF_RE = re.compile(r"^(?P<name>%[A-Za-z0-9_.$-]+)\s*=\s*type\s+(?P<body>.+)$")
ALLOCA_RE = re.compile(r"\balloca\s+(?P<type>.+?)(?:,\s+align\b|$)")
LIFETIME_RE = re.compile(r"llvm\.lifetime\.start[^)]*\(\s*i64\s+(?P<bytes>\d+)\s*,")
LIFETIME_END_RE = re.compile(r"llvm\.lifetime\.end[^)]*\(\s*i64\s+(?P<bytes>\d+)\s*,")
VECTOR_TYPE_RE = re.compile(r"^<(?P<count>\d+)\s+x\s+(?P<base>[A-Za-z0-9_]+)>$")
ARRAY_TYPE_RE = re.compile(r"^\[(?P<count>\d+)\s+x\s+(?P<element>.+)\]$")


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str
    stderr: str


def parse_number(value: Any) -> float:
    try:
        return float(str(value))
    except (TypeError, ValueError):
        return 0.0


def parse_int(value: Any) -> int:
    try:
        text = str(value).strip()
        if text.startswith(("0x", "0X")):
            return int(text, 16)
        return int(float(text))
    except (TypeError, ValueError):
        return 0


def run_command(args: list[str]) -> CommandResult:
    result = subprocess.run(
        args,
        text=True,
        capture_output=True,
        check=False,
    )
    return CommandResult(result.returncode, result.stdout, result.stderr)


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def type_size(
    type_name: str,
    type_table: dict[str, str] | None = None,
    seen: set[str] | None = None,
) -> int:
    clean = type_name.strip()
    type_table = type_table or {}
    seen = seen or set()
    if clean.endswith("*"):
        return 8
    if clean == "ptr" or clean.startswith("ptr addrspace("):
        return 8
    if clean in type_table:
        if clean in seen:
            return 0
        return type_size(type_table[clean], type_table, seen | {clean})
    vector = VECTOR_TYPE_RE.match(clean)
    if vector:
        return int(vector.group("count")) * type_size(vector.group("base"), type_table, seen)
    array = ARRAY_TYPE_RE.match(clean)
    if array:
        return int(array.group("count")) * type_size(array.group("element"), type_table, seen)
    if clean.startswith("<{") and clean.endswith("}>"):
        return sum(type_size(part, type_table, seen) for part in split_top_level_csv(clean[2:-2].strip()))
    if clean.startswith("{") and clean.endswith("}"):
        return sum(type_size(part, type_table, seen) for part in split_top_level_csv(clean[1:-1].strip()))
    return {
        "half": 2,
        "float": 4,
        "i1": 1,
        "i8": 1,
        "i16": 2,
        "i32": 4,
        "i64": 8,
        "int": 4,
        "uint": 4,
    }.get(clean, 0)


def split_top_level_csv(text: str) -> list[str]:
    parts: list[str] = []
    start = 0
    angle = 0
    brace = 0
    bracket = 0
    for index, char in enumerate(text):
        if char == "<":
            angle += 1
        elif char == ">":
            angle = max(angle - 1, 0)
        elif char == "{":
            brace += 1
        elif char == "}":
            brace = max(brace - 1, 0)
        elif char == "[":
            bracket += 1
        elif char == "]":
            bracket = max(bracket - 1, 0)
        elif char == "," and angle == 0 and brace == 0 and bracket == 0:
            parts.append(text[start:index].strip())
            start = index + 1
    tail = text[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def type_field_count(type_name: str, type_table: dict[str, str] | None = None) -> int:
    clean = type_name.strip()
    type_table = type_table or {}
    if clean in type_table:
        clean = type_table[clean].strip()
    if clean.startswith("<{") and clean.endswith("}>"):
        return len(split_top_level_csv(clean[2:-2].strip()))
    if clean.startswith("{") and clean.endswith("}"):
        return len(split_top_level_csv(clean[1:-1].strip()))
    return 0 if clean == "void" else 1


def return_type_metrics(return_type: str, type_table: dict[str, str] | None = None) -> tuple[int, int]:
    text = return_type.strip()
    if text == "void":
        return 0, 0
    fields = type_field_count(text, type_table)
    return fields, type_size(text, type_table)


def parse_alloca_type(line: str) -> str:
    match = ALLOCA_RE.search(line)
    return match.group("type").strip() if match else ""


def alloca_bytes(line: str, type_table: dict[str, str] | None = None) -> int:
    alloca_type = parse_alloca_type(line)
    return type_size(alloca_type, type_table) if alloca_type else 0


def parse_type_table(ir: str) -> dict[str, str]:
    type_table: dict[str, str] = {}
    for raw in ir.splitlines():
        match = TYPE_DEF_RE.match(raw.strip())
        if match:
            type_table[match.group("name")] = match.group("body").strip()
    return type_table


def parse_metal_size(output: str) -> dict[str, int]:
    for line in reversed(output.splitlines()):
        match = SIZE_RE.match(line)
        if not match:
            continue
        return {
            "metallib_text_bytes": int(match.group("text")),
            "metallib_data_bytes": int(match.group("data")),
            "metallib_bss_bytes": int(match.group("bss")),
            "metallib_dec_bytes": int(match.group("dec")),
        }
    return {
        "metallib_text_bytes": 0,
        "metallib_data_bytes": 0,
        "metallib_bss_bytes": 0,
        "metallib_dec_bytes": 0,
    }


def parse_ir_metrics(ir: str) -> dict[str, int]:
    type_table = parse_type_table(ir)
    metrics = {
        "ir_lines": 0,
        "ir_instruction_lines": 0,
        "ir_type_def_count": len(type_table),
        "ir_max_type_def_bytes": max((type_size(value, type_table) for value in type_table.values()), default=0),
        "ir_return_field_count": 0,
        "ir_return_bytes": 0,
        "ir_alloca_count": 0,
        "ir_alloca_array_count": 0,
        "ir_alloca_struct_count": 0,
        "ir_alloca_bytes": 0,
        "ir_scratch_bytes_estimate": 0,
        "ir_lifetime_start_bytes": 0,
        "ir_lifetime_end_bytes": 0,
        "ir_insertvalue_count": 0,
        "ir_load_count": 0,
        "ir_store_count": 0,
        "ir_memcpy_count": 0,
        "ir_memset_count": 0,
        "ir_call_count": 0,
        "ir_branch_count": 0,
        "ir_select_count": 0,
        "ir_getelementptr_count": 0,
        "ir_phi_count": 0,
        "ir_air_dot_calls": 0,
        "ir_texture_sample_calls": 0,
        "ir_addrspace1_refs": 0,
        "ir_addrspace2_refs": 0,
        "ir_addrspace3_refs": 0,
        "ir_addrspace4_refs": 0,
    }
    for raw in ir.splitlines():
        line = raw.strip()
        if not line:
            continue
        metrics["ir_lines"] += 1
        define = DEFINE_RE.match(line)
        if define and metrics["ir_return_field_count"] == 0:
            fields, bytes_ = return_type_metrics(define.group("ret"), type_table)
            metrics["ir_return_field_count"] = fields
            metrics["ir_return_bytes"] = bytes_
        if line.startswith(";"):
            continue
        if " = " in line or line.startswith(("br ", "ret ", "store ", "call ")):
            metrics["ir_instruction_lines"] += 1
        if " alloca " in line:
            alloca_type = parse_alloca_type(line)
            metrics["ir_alloca_count"] += 1
            metrics["ir_alloca_bytes"] += alloca_bytes(line, type_table)
            if alloca_type.startswith("["):
                metrics["ir_alloca_array_count"] += 1
            if "%" in alloca_type:
                metrics["ir_alloca_struct_count"] += 1
        lifetime = LIFETIME_RE.search(line)
        if lifetime:
            metrics["ir_lifetime_start_bytes"] += int(lifetime.group("bytes"))
        lifetime_end = LIFETIME_END_RE.search(line)
        if lifetime_end:
            metrics["ir_lifetime_end_bytes"] += int(lifetime_end.group("bytes"))
        metrics["ir_insertvalue_count"] += line.count("insertvalue")
        if re.search(r"\bload\b", line):
            metrics["ir_load_count"] += 1
        if re.search(r"\bstore\b", line):
            metrics["ir_store_count"] += 1
        metrics["ir_memcpy_count"] += line.count("llvm.memcpy")
        metrics["ir_memset_count"] += line.count("llvm.memset")
        if re.search(r"\bcall\b", line):
            metrics["ir_call_count"] += 1
        if re.search(r"\bbr\b", line):
            metrics["ir_branch_count"] += 1
        if re.search(r"\bselect\b", line):
            metrics["ir_select_count"] += 1
        if re.search(r"\bgetelementptr\b", line):
            metrics["ir_getelementptr_count"] += 1
        if re.search(r"\bphi\b", line):
            metrics["ir_phi_count"] += 1
        metrics["ir_air_dot_calls"] += line.count("@air.dot")
        metrics["ir_texture_sample_calls"] += line.count(".sample")
        metrics["ir_addrspace1_refs"] += line.count("addrspace(1)")
        metrics["ir_addrspace2_refs"] += line.count("addrspace(2)")
        metrics["ir_addrspace3_refs"] += line.count("addrspace(3)")
        metrics["ir_addrspace4_refs"] += line.count("addrspace(4)")
    metrics["ir_scratch_bytes_estimate"] = max(
        metrics["ir_alloca_bytes"],
        metrics["ir_lifetime_start_bytes"],
        metrics["ir_lifetime_end_bytes"],
    )
    return metrics


def warning_count(stderr: str) -> int:
    return sum(1 for line in stderr.splitlines() if "warning:" in line)


def resolve_shader_path(shader_dir: Path, value: str) -> Path:
    path = Path(value)
    if path.is_absolute() or path.exists():
        return path
    return shader_dir / value


def compile_shader(shader_path: Path, work_dir: Path, keep_temps: bool) -> dict[str, Any]:
    stem = shader_path.stem
    air_path = work_dir / f"{stem}.air"
    metallib_path = work_dir / f"{stem}.metallib"

    row: dict[str, Any] = {
        "compile_ok": 0,
        "warning_count": 0,
        "air_bytes": 0,
        "metallib_bytes": 0,
        "metallib_text_bytes": 0,
        "metallib_data_bytes": 0,
        "metallib_bss_bytes": 0,
        "metallib_dec_bytes": 0,
        "missing_reason": "",
    }
    row.update(parse_ir_metrics(""))

    compile_result = run_command([
        "xcrun",
        "-sdk",
        "macosx",
        "metal",
        "-c",
        str(shader_path),
        "-o",
        str(air_path),
    ])
    row["warning_count"] = warning_count(compile_result.stderr)
    if compile_result.returncode != 0:
        row["missing_reason"] = "metal_compile_failed"
        return row

    link_result = run_command([
        "xcrun",
        "-sdk",
        "macosx",
        "metallib",
        str(air_path),
        "-o",
        str(metallib_path),
    ])
    if link_result.returncode != 0:
        row["missing_reason"] = "metallib_link_failed"
        return row

    size_result = run_command([
        "xcrun",
        "-sdk",
        "macosx",
        "metal-size",
        str(metallib_path),
    ])
    if size_result.returncode == 0:
        row.update(parse_metal_size(size_result.stdout))

    dump_result = run_command([
        "xcrun",
        "-sdk",
        "macosx",
        "metal-objdump",
        "-d",
        str(metallib_path),
    ])
    if dump_result.returncode == 0:
        row.update(parse_ir_metrics(dump_result.stdout))

    row["compile_ok"] = 1
    row["air_bytes"] = air_path.stat().st_size if air_path.exists() else 0
    row["metallib_bytes"] = metallib_path.stat().st_size if metallib_path.exists() else 0
    if not keep_temps:
        air_path.unlink(missing_ok=True)
        metallib_path.unlink(missing_ok=True)
    return row


def shader_jobs(rows: list[dict[str, str]], top: int) -> list[tuple[dict[str, str], str, str]]:
    jobs: list[tuple[dict[str, str], str, str]] = []
    for row in rows:
        if len(jobs) >= top * 2:
            break
        rank = parse_int(row.get("rank"))
        if rank <= 0 or rank > top:
            continue
        for stage, key in (("vs", "vs_file"), ("ps", "ps_file")):
            filename = row.get(key, "")
            if filename:
                jobs.append((row, stage, filename))
    return jobs


def analyze(args: argparse.Namespace) -> list[dict[str, Any]]:
    summary_path = Path(args.shader_summary)
    shader_dir = Path(args.shader_dir)
    rows = read_rows(summary_path)
    jobs = shader_jobs(rows, args.top)
    if not jobs:
        raise SystemExit("no shader files found in summary top rows")

    output_dir = Path(args.work_dir) if args.work_dir else None
    temp_context = None
    if output_dir is None:
        temp_context = tempfile.TemporaryDirectory(prefix="dxmt9-metal-codegen-")
        work_dir = Path(temp_context.name)
    else:
        work_dir = output_dir
        work_dir.mkdir(parents=True, exist_ok=True)

    try:
        output_rows: list[dict[str, Any]] = []
        for source_row, stage, filename in jobs:
            shader_path = resolve_shader_path(shader_dir, filename)
            out: dict[str, Any] = {field: "" for field in OUTPUT_FIELDS}
            out.update({
                "rank": parse_int(source_row.get("rank")),
                "stage": stage,
                "seq": source_row.get("seq", ""),
                "enc": source_row.get("enc", ""),
                "gpu_ms": source_row.get("gpu_ms", ""),
                "vs_buffer_write_mib": source_row.get("vs_buffer_write_mib", ""),
                "vs_buffer_bytes_per_vs_invocation": source_row.get("vs_buffer_bytes_per_vs_invocation", ""),
                "shader_file": filename,
                "shader_label": source_row.get(f"{stage}_label", ""),
            })
            if not shader_path.exists():
                out["missing_reason"] = "missing_shader_file"
                output_rows.append(out)
                continue
            text = shader_path.read_text(encoding="utf-8", errors="replace")
            out["source_bytes"] = len(text.encode("utf-8"))
            out["source_lines"] = len(text.splitlines())
            out.update(compile_shader(shader_path, work_dir, args.keep_temps))
            bytes_per_vs = parse_number(out.get("vs_buffer_bytes_per_vs_invocation"))
            ir_return_bytes = parse_number(out.get("ir_return_bytes"))
            ir_alloca_bytes = parse_number(out.get("ir_alloca_bytes"))
            out["vs_buffer_to_ir_return_ratio"] = (
                bytes_per_vs / ir_return_bytes if stage == "vs" and ir_return_bytes else 0.0
            )
            out["vs_buffer_to_ir_alloca_ratio"] = (
                bytes_per_vs / ir_alloca_bytes if stage == "vs" and ir_alloca_bytes else 0.0
            )
            ir_scratch_bytes = parse_number(out.get("ir_scratch_bytes_estimate"))
            out["vs_buffer_to_ir_scratch_ratio"] = (
                bytes_per_vs / ir_scratch_bytes if stage == "vs" and ir_scratch_bytes else 0.0
            )
            output_rows.append(out)
        return output_rows
    finally:
        if temp_context is not None:
            temp_context.cleanup()
        elif not args.keep_temps and output_dir is not None:
            for path in output_dir.glob("*.air"):
                path.unlink(missing_ok=True)
            for path in output_dir.glob("*.metallib"):
                path.unlink(missing_ok=True)


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=OUTPUT_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in OUTPUT_FIELDS})


def write_markdown(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    ok_rows = [row for row in rows if parse_int(row.get("compile_ok"))]
    top_vs = [row for row in ok_rows if row.get("stage") == "vs"]
    path.write_text(
        "\n".join([
            "# Metal Shader Codegen Summary",
            "",
            f"- Shader entries analyzed: `{len(rows)}`",
            f"- Compile/link successes: `{len(ok_rows)}`",
            f"- Vertex shader entries: `{len(top_vs)}`",
            "",
            "## Top Rows",
            "",
            "| Rank | Stage | Seq/enc | GPU ms | VS buffer MiB | text B | IR return B | IR scratch B | alloca B | lifetime B | memcpy/memset | warnings | Notes |",
            "|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|",
            *[
                "| {rank} | {stage} | {seq}/{enc} | {gpu_ms} | {vs_mib} | {text_b} | {ret_b} | {scratch_b} | {alloca_b} | {life_b} | {mem_ops} | {warn} | {note} |".format(
                    rank=row.get("rank", ""),
                    stage=row.get("stage", ""),
                    seq=row.get("seq", ""),
                    enc=row.get("enc", ""),
                    gpu_ms=f"{parse_number(row.get('gpu_ms')):.3f}",
                    vs_mib=f"{parse_number(row.get('vs_buffer_write_mib')):.3f}",
                    text_b=row.get("metallib_text_bytes", ""),
                    ret_b=row.get("ir_return_bytes", ""),
                    scratch_b=row.get("ir_scratch_bytes_estimate", ""),
                    alloca_b=row.get("ir_alloca_bytes", ""),
                    life_b=row.get("ir_lifetime_start_bytes", ""),
                    mem_ops=(
                        f"{parse_int(row.get('ir_memcpy_count'))}/"
                        f"{parse_int(row.get('ir_memset_count'))}"
                    ),
                    warn=row.get("warning_count", ""),
                    note=row.get("missing_reason", ""),
                )
                for row in rows
            ],
            "",
            "## Vertex Ratios",
            "",
            "| Rank | Seq/enc | Xcode VS B/invocation | IR return B | Xcode/IR return | IR scratch B | Xcode/IR scratch | IR alloca B | Xcode/IR alloca |",
            "|---:|---|---:|---:|---:|---:|---:|---:|---:|",
            *[
                "| {rank} | {seq}/{enc} | {vs_b} | {ret_b} | {ret_ratio} | {scratch_b} | {scratch_ratio} | {alloca_b} | {alloca_ratio} |".format(
                    rank=row.get("rank", ""),
                    seq=row.get("seq", ""),
                    enc=row.get("enc", ""),
                    vs_b=f"{parse_number(row.get('vs_buffer_bytes_per_vs_invocation')):.1f}",
                    ret_b=row.get("ir_return_bytes", ""),
                    ret_ratio=f"{parse_number(row.get('vs_buffer_to_ir_return_ratio')):.2f}x",
                    scratch_b=row.get("ir_scratch_bytes_estimate", ""),
                    scratch_ratio=f"{parse_number(row.get('vs_buffer_to_ir_scratch_ratio')):.2f}x",
                    alloca_b=row.get("ir_alloca_bytes", ""),
                    alloca_ratio=f"{parse_number(row.get('vs_buffer_to_ir_alloca_ratio')):.2f}x",
                )
                for row in rows if row.get("stage") == "vs"
            ],
            "",
            "## Interpretation",
            "",
            "- `IR return B` is the compiler-visible stage return aggregate size after MSL compilation.",
            "- `IR scratch B` is the max of compiler-visible `alloca` bytes and lifetime byte ranges that survived into metallib IR.",
            "- `memcpy/memset` and address-space reference columns are coarse signs of backend-lowered private or memory traffic, not a replacement for Xcode counters.",
            "- If MSL-local temp arrays are optimized away here while Xcode VS buffer writes stay high, the owner is below source-visible scratch.",
        ]),
        encoding="utf-8",
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shader_summary", help="CSV from analyze_shader_dumps.py")
    parser.add_argument("--shader-dir", required=True, help="Directory containing dumped .metal files")
    parser.add_argument("--top", type=int, default=3, help="Top encoder ranks to compile")
    parser.add_argument("--output", required=True, help="Markdown report path")
    parser.add_argument("--csv-output", required=True, help="CSV report path")
    parser.add_argument("--work-dir", help="Optional temporary work directory")
    parser.add_argument("--keep-temps", action="store_true", help="Keep .air/.metallib files in --work-dir")
    args = parser.parse_args(argv)

    if args.top <= 0:
        parser.error("--top must be positive")
    if args.keep_temps and not args.work_dir:
        parser.error("--keep-temps requires --work-dir")

    rows = analyze(args)
    write_csv(Path(args.csv_output), rows)
    write_markdown(Path(args.output), rows)
    failures = [row for row in rows if not parse_int(row.get("compile_ok"))]
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
