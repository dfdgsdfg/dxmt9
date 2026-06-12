#!/usr/bin/env python3
"""Summarize dxmt9 Frame Graph DAG debug dumps.

The script is intentionally conservative: it does not claim that production
coalescing is legal. It extracts same-attachment re-entry pairs and the local
evidence needed for the render-pass-store H6 proof: direct A->B edges,
intervening same-attachment access, intervening edge involvement, draw counts,
and post-opt load/store shape.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path
from typing import Iterable, Sequence


SUMMARY_FIELDS = [
    "file",
    "frame_id",
    "chunk_seq_id",
    "stage",
    "passes",
    "render_passes",
    "present_passes",
    "resources",
    "edges",
    "same_attachment_pairs",
    "safe_relocatable_pairs",
]

CANDIDATE_FIELDS = [
    "file",
    "frame_id",
    "chunk_seq_id",
    "stage",
    "a_pass",
    "b_pass",
    "distance",
    "attachment_key",
    "color_key",
    "depth",
    "a_draw_first",
    "a_draw_count",
    "b_draw_first",
    "b_draw_count",
    "a_load_store",
    "b_load_store",
    "direct_edge_resources",
    "intervening_passes",
    "intervening_same_attachment_accesses",
    "intervening_edge_count",
    "safe_no_intervening_attachment_access",
    "intervening_edge_free",
    "safe_relocatable_candidate",
]


class DagFile:
    def __init__(self, path: Path, data: dict[str, object]) -> None:
        self.path = path
        self.data = data


def read_dag(path: Path) -> DagFile:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected a JSON object")
    for key in ("passes", "resources", "edges"):
        if not isinstance(data.get(key), list):
            raise ValueError(f"{path}: missing list field {key!r}")
    return DagFile(path=path, data=data)


def expand_inputs(paths: Sequence[Path]) -> list[Path]:
    result: list[Path] = []
    for path in paths:
        if path.is_dir():
            result.extend(sorted(path.glob("dag-frame*-chunk*-*.json")))
        else:
            result.append(path)
    return sorted(dict.fromkeys(result))


def pass_index(pass_row: dict[str, object]) -> int:
    return int(pass_row.get("index", 0))


def draw_value(pass_row: dict[str, object], key: str) -> int:
    draws = pass_row.get("draws")
    if not isinstance(draws, dict):
        return 0
    return int(draws.get(key, 0) or 0)


def color_key(pass_row: dict[str, object]) -> str:
    color = pass_row.get("color")
    if not isinstance(color, list):
        return ""
    return ",".join(str(item) for item in color)


def depth_key(pass_row: dict[str, object]) -> str:
    value = pass_row.get("depth")
    return "" if value is None else str(value)


def attachment_key(pass_row: dict[str, object]) -> str:
    return f"c=[{color_key(pass_row)}];d={depth_key(pass_row)}"


def attachment_handles(pass_row: dict[str, object]) -> set[str]:
    handles: set[str] = set()
    color = pass_row.get("color")
    if isinstance(color, list):
        handles.update(str(item) for item in color if item is not None)
    depth = pass_row.get("depth")
    if depth is not None:
        handles.add(str(depth))
    return handles


def load_store_text(pass_row: dict[str, object]) -> str:
    load_store = pass_row.get("load_store")
    if not isinstance(load_store, dict):
        return ""
    color = load_store.get("color")
    color_text = ",".join(str(item) for item in color) if isinstance(color, list) else ""
    depth = load_store.get("depth")
    return f"c={color_text};d={'' if depth is None else depth}"


def render_passes(dag: DagFile) -> list[dict[str, object]]:
    passes = dag.data["passes"]
    assert isinstance(passes, list)
    return [
        row
        for row in passes
        if isinstance(row, dict) and str(row.get("kind", "")) == "Render"
    ]


def present_pass_count(dag: DagFile) -> int:
    passes = dag.data["passes"]
    assert isinstance(passes, list)
    return sum(
        1
        for row in passes
        if isinstance(row, dict) and str(row.get("kind", "")) == "Present"
    )


def resource_accesses_by_handle(dag: DagFile) -> dict[str, list[int]]:
    resources = dag.data["resources"]
    assert isinstance(resources, list)
    result: dict[str, list[int]] = {}
    for resource in resources:
        if not isinstance(resource, dict):
            continue
        handle = resource.get("handle")
        accesses = resource.get("accesses")
        if handle is None or not isinstance(accesses, list):
            continue
        passes: list[int] = []
        for access in accesses:
            if isinstance(access, dict) and "pass" in access:
                passes.append(int(access["pass"]))
        result[str(handle)] = passes
    return result


def edge_rows(dag: DagFile) -> list[dict[str, object]]:
    edges = dag.data["edges"]
    assert isinstance(edges, list)
    return [edge for edge in edges if isinstance(edge, dict)]


def edge_involves_intervening(edge: dict[str, object], a: int, b: int) -> bool:
    src = int(edge.get("src_pass", -1))
    dst = int(edge.get("dst_pass", -1))
    return (a < src < b) or (a < dst < b)


def direct_edge_resources(edges: Iterable[dict[str, object]], a: int, b: int) -> list[str]:
    resources: list[str] = []
    for edge in edges:
        if int(edge.get("src_pass", -1)) == a and int(edge.get("dst_pass", -1)) == b:
            resources.append(str(edge.get("resource", "")))
    return sorted(resource for resource in resources if resource)


def intervening_same_attachment_accesses(
    access_by_handle: dict[str, list[int]], handles: set[str], a: int, b: int
) -> list[str]:
    rows: list[str] = []
    for handle in sorted(handles):
        for pass_id in sorted(set(access_by_handle.get(handle, []))):
            if a < pass_id < b:
                rows.append(f"{handle}@P{pass_id}")
    return rows


def summarize_dag(dag: DagFile) -> tuple[dict[str, object], list[dict[str, object]]]:
    passes = dag.data["passes"]
    resources = dag.data["resources"]
    edges = edge_rows(dag)
    assert isinstance(passes, list)
    assert isinstance(resources, list)
    rpasses = sorted(render_passes(dag), key=pass_index)
    access_by_handle = resource_accesses_by_handle(dag)

    candidate_rows: list[dict[str, object]] = []
    for i, a_pass in enumerate(rpasses):
        a_index = pass_index(a_pass)
        a_key = attachment_key(a_pass)
        if a_key == "c=[];d=":
            continue
        for b_pass in rpasses[i + 1 :]:
            b_index = pass_index(b_pass)
            if attachment_key(b_pass) != a_key:
                continue
            handles = attachment_handles(a_pass)
            direct_resources = direct_edge_resources(edges, a_index, b_index)
            same_attachment_accesses = intervening_same_attachment_accesses(
                access_by_handle, handles, a_index, b_index
            )
            intervening_edges = [
                edge for edge in edges if edge_involves_intervening(edge, a_index, b_index)
            ]
            no_intervening_attachment = not same_attachment_accesses
            edge_free = not intervening_edges
            candidate_rows.append(
                {
                    "file": str(dag.path),
                    "frame_id": dag.data.get("frame_id", ""),
                    "chunk_seq_id": dag.data.get("chunk_seq_id", ""),
                    "stage": dag.data.get("stage", ""),
                    "a_pass": a_index,
                    "b_pass": b_index,
                    "distance": max(0, b_index - a_index - 1),
                    "attachment_key": a_key,
                    "color_key": color_key(a_pass),
                    "depth": depth_key(a_pass),
                    "a_draw_first": draw_value(a_pass, "first"),
                    "a_draw_count": draw_value(a_pass, "count"),
                    "b_draw_first": draw_value(b_pass, "first"),
                    "b_draw_count": draw_value(b_pass, "count"),
                    "a_load_store": load_store_text(a_pass),
                    "b_load_store": load_store_text(b_pass),
                    "direct_edge_resources": ";".join(direct_resources),
                    "intervening_passes": ",".join(
                        str(pass_id) for pass_id in range(a_index + 1, b_index)
                    ),
                    "intervening_same_attachment_accesses": ";".join(
                        same_attachment_accesses
                    ),
                    "intervening_edge_count": len(intervening_edges),
                    "safe_no_intervening_attachment_access": int(
                        no_intervening_attachment
                    ),
                    "intervening_edge_free": int(edge_free),
                    "safe_relocatable_candidate": int(
                        no_intervening_attachment and edge_free
                    ),
                }
            )

    summary = {
        "file": str(dag.path),
        "frame_id": dag.data.get("frame_id", ""),
        "chunk_seq_id": dag.data.get("chunk_seq_id", ""),
        "stage": dag.data.get("stage", ""),
        "passes": len(passes),
        "render_passes": len(rpasses),
        "present_passes": present_pass_count(dag),
        "resources": len(resources),
        "edges": len(edges),
        "same_attachment_pairs": len(candidate_rows),
        "safe_relocatable_pairs": sum(
            int(row["safe_relocatable_candidate"]) for row in candidate_rows
        ),
    }
    return summary, candidate_rows


def write_csv(rows: Sequence[dict[str, object]], fields: Sequence[str], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def markdown_table(rows: Sequence[dict[str, object]], fields: Sequence[str], limit: int) -> list[str]:
    shown = rows[:limit] if limit > 0 else rows
    lines = [
        "| " + " | ".join(fields) + " |",
        "| " + " | ".join("---" for _ in fields) + " |",
    ]
    for row in shown:
        lines.append("| " + " | ".join(str(row.get(field, "")) for field in fields) + " |")
    return lines


def write_markdown(
    summary_rows: Sequence[dict[str, object]],
    candidate_rows: Sequence[dict[str, object]],
    path: Path,
    limit: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    safe_count = sum(int(row["safe_relocatable_candidate"]) for row in candidate_rows)
    lines = [
        "# Frame Graph DAG Summary",
        "",
        f"- Files: `{len(summary_rows)}`",
        f"- Same-attachment re-entry pairs: `{len(candidate_rows)}`",
        f"- Safe-relocatable candidate pairs: `{safe_count}`",
        "",
        "## Files",
        "",
    ]
    lines.extend(markdown_table(summary_rows, SUMMARY_FIELDS, limit))
    lines.extend(["", "## Same-Attachment Pairs", ""])
    candidate_fields = [
        "file",
        "frame_id",
        "stage",
        "a_pass",
        "b_pass",
        "distance",
        "attachment_key",
        "a_load_store",
        "b_load_store",
        "direct_edge_resources",
        "intervening_same_attachment_accesses",
        "intervening_edge_count",
        "safe_relocatable_candidate",
    ]
    lines.extend(markdown_table(candidate_rows, candidate_fields, limit))
    if limit > 0 and len(candidate_rows) > limit:
        lines.append("")
        lines.append(f"- Candidate table truncated to first `{limit}` rows.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path, help="DAG JSON files or directories")
    parser.add_argument("--stage", help="Only include snapshots with this stage")
    parser.add_argument("--csv", type=Path, help="Write candidate CSV")
    parser.add_argument("--summary-csv", type=Path, help="Write file summary CSV")
    parser.add_argument("--markdown", type=Path, help="Write Markdown summary")
    parser.add_argument("--limit", type=int, default=20, help="Markdown row limit; 0 means all")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    paths = expand_inputs(args.paths)
    if not paths:
        print("no DAG JSON files found", file=sys.stderr)
        return 1

    summary_rows: list[dict[str, object]] = []
    candidate_rows: list[dict[str, object]] = []
    for path in paths:
        dag = read_dag(path)
        if args.stage and str(dag.data.get("stage", "")) != args.stage:
            continue
        summary, candidates = summarize_dag(dag)
        summary_rows.append(summary)
        candidate_rows.extend(candidates)

    if args.summary_csv:
        write_csv(summary_rows, SUMMARY_FIELDS, args.summary_csv)
    if args.csv:
        write_csv(candidate_rows, CANDIDATE_FIELDS, args.csv)
    if args.markdown:
        write_markdown(summary_rows, candidate_rows, args.markdown, args.limit)

    if not args.summary_csv and not args.csv and not args.markdown:
        write_markdown(summary_rows, candidate_rows, Path("/dev/stdout"), args.limit)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
