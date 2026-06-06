#!/usr/bin/env python3
"""Audit backend-escape implementation surfaces before spending Xcode budget."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Iterable


CSV_FIELDS = [
    "candidate",
    "bridge_surface",
    "dxmt9_route",
    "shader_emitter",
    "current_gt1_evidence",
    "verdict",
    "reason",
    "next_action",
]


def read_text(path: Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="ignore")


def read_tree(root: Path, suffixes: tuple[str, ...] = (".cpp", ".hpp", ".mm", ".h")) -> str:
    if not root.exists():
        return ""
    chunks: list[str] = []
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.suffix in suffixes:
            chunks.append(read_text(path))
    return "\n".join(chunks)


def has_all(text: str, needles: Iterable[str]) -> bool:
    return all(needle in text for needle in needles)


def has_any(text: str, needles: Iterable[str]) -> bool:
    return any(needle in text for needle in needles)


def load_csv(path: Path | None) -> list[dict[str, str]]:
    if path is None:
        return []
    if not path.exists():
        raise SystemExit(f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def tile_coverage_status(rows: list[dict[str, str]]) -> tuple[str, str]:
    if not rows:
        return "missing", "no Tile-FFP coverage CSV attached"
    candidate_rows = [row for row in rows if row.get("verdict") == "candidate-tile-ffp-coverage"]
    low_rows = [row for row in rows if row.get("verdict") == "low-tile-ffp-coverage"]
    no_rows = [row for row in rows if row.get("verdict") == "no-tile-ffp-coverage"]
    if candidate_rows:
        rows_text = ", ".join(row.get("row", "") for row in candidate_rows[:4])
        return "candidate", f"{len(candidate_rows)} candidate coverage row(s): {rows_text}"
    if low_rows:
        return "low", f"{len(low_rows)} low coverage row(s), {len(no_rows)} no coverage row(s)"
    return "none", f"{len(no_rows)} no coverage row(s)"


def audit(root: Path, tile_coverage_csv: Path | None) -> list[dict[str, str]]:
    winemetal_text = "\n".join([
        read_text(root / "src" / "winemetal" / "winemetal.h"),
        read_text(root / "src" / "winemetal" / "Metal.hpp"),
        read_text(root / "src" / "winemetal" / "unix" / "winemetal_private_api.mm"),
    ])
    dxmt9_text = read_tree(root / "src" / "dxmt9")
    tile_rows = load_csv(tile_coverage_csv)
    tile_status, tile_reason = tile_coverage_status(tile_rows)

    mesh_bridge = has_all(
        winemetal_text,
        ("WMTMeshRenderPipelineInfo", "object_function", "mesh_function"),
    ) and has_any(
        winemetal_text,
        ("drawMeshThreadgroups", "wmtcmd_render_draw_meshthreadgroups"),
    )
    mesh_route = has_any(
        dxmt9_text,
        (
            "WMTMeshRenderPipelineInfo",
            "InitializeMeshRenderPipelineInfo",
            "drawMeshThreadgroups",
            "wmtcmd_render_draw_meshthreadgroups",
        ),
    )
    mesh_shader = has_any(dxmt9_text, ("[[mesh]]", "mesh<", "object_function", "mesh_function"))
    if mesh_bridge and not mesh_route and not mesh_shader:
        mesh_verdict = "bridge-only-reduced-ab-required"
        mesh_reason = "winemetal exposes mesh/object descriptors and replay commands, but dxmt9 has no mesh shader emitter or draw-route producer"
        mesh_action = "build a reduced synthetic/replay A/B before treating mesh/object as a GT1 backend denominator candidate"
    elif mesh_bridge and mesh_route and mesh_shader:
        mesh_verdict = "candidate-route-present"
        mesh_reason = "mesh/object bridge, route, and shader emitter all appear present"
        mesh_action = "run a reduced equality and Xcode counter A/B before GT1 promotion"
    else:
        mesh_verdict = "missing-bridge-surface"
        mesh_reason = "mesh/object bridge surface is incomplete in the audited source tree"
        mesh_action = "do not schedule mesh/object Xcode work"

    position_probe = has_all(dxmt9_text, ("DXMT9_PROBE_POSITION_ONLY_VSOUT", "positionOnlyVSOutLayout"))
    position_real_route = has_any(
        dxmt9_text,
        ("DXMT9_PROBE_POSITION_ONLY_BINNING", "positionOnlyBinning", "position_only_binning"),
    )
    if position_probe and not position_real_route:
        position_verdict = "visible-vsout-probe-only"
        position_reason = "source-visible position-only VSOut probe exists, but no separate position/binning route is wired"
        position_action = "do not use visible position-only VSOut as closure; define a real binning/depth/mesh route before Xcode"
    elif position_real_route:
        position_verdict = "candidate-route-present"
        position_reason = "a position/binning route token exists in dxmt9"
        position_action = "validate row stability and Xcode bytes/invocation movement"
    else:
        position_verdict = "missing"
        position_reason = "no position-only probe or real binning route found"
        position_action = "ignore this backend escape until implementation exists"

    tile_bridge = has_all(dxmt9_text, ("selectTileFfpForPass", "makeFfpTilePixelSource")) and has_any(
        winemetal_text,
        ("dispatchThreadsPerTile", "WMTTileRenderPipelineInfo"),
    )
    if not tile_bridge:
        tile_verdict = "missing"
        tile_next = "do not schedule Tile-FFP work until selector, shader, and tile command surfaces exist"
    elif tile_status == "candidate":
        tile_verdict = "candidate-coverage"
        tile_next = "run portable-vs-tile equality and then a scoped Xcode counter A/B"
    elif tile_status in {"low", "none"}:
        tile_verdict = "rejected-current-coverage"
        tile_next = "keep Tile-FFP as a narrow correctness/architecture lever, not a GT1 hot-row Xcode target"
    else:
        tile_verdict = "implemented-needs-coverage"
        tile_next = "attach analyze_tile_ffp_coverage.py output before any Xcode spend"

    return [
        {
            "candidate": "mesh-object",
            "bridge_surface": "present" if mesh_bridge else "missing",
            "dxmt9_route": "present" if mesh_route else "missing",
            "shader_emitter": "present" if mesh_shader else "missing",
            "current_gt1_evidence": "none",
            "verdict": mesh_verdict,
            "reason": mesh_reason,
            "next_action": mesh_action,
        },
        {
            "candidate": "position-binning",
            "bridge_surface": "ordinary-render" if position_probe else "missing",
            "dxmt9_route": "present" if position_real_route else "missing",
            "shader_emitter": "visible-vsout-probe" if position_probe else "missing",
            "current_gt1_evidence": "visible-width Xcode rejected",
            "verdict": position_verdict,
            "reason": position_reason,
            "next_action": position_action,
        },
        {
            "candidate": "tile-ffp",
            "bridge_surface": "present" if tile_bridge else "missing",
            "dxmt9_route": "present" if tile_bridge else "missing",
            "shader_emitter": "present" if tile_bridge else "missing",
            "current_gt1_evidence": tile_reason,
            "verdict": tile_verdict,
            "reason": tile_reason,
            "next_action": tile_next,
        },
    ]


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def md_cell(value: str) -> str:
    return value.replace("|", "\\|")


def write_markdown(path: Path, rows: list[dict[str, str]], root: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Backend Escape Surface Audit",
        "",
        f"- Repo root: `{root}`",
        "",
        "| Candidate | Bridge surface | dxmt9 route | Shader emitter | Evidence | Verdict | Next action |",
        "|---|---|---|---|---|---|---|",
    ]
    for row in rows:
        lines.append(
            "| "
            + " | ".join(
                [
                    f"`{md_cell(row['candidate'])}`",
                    f"`{md_cell(row['bridge_surface'])}`",
                    f"`{md_cell(row['dxmt9_route'])}`",
                    f"`{md_cell(row['shader_emitter'])}`",
                    md_cell(row["current_gt1_evidence"]),
                    f"`{md_cell(row['verdict'])}`",
                    md_cell(row["next_action"]),
                ]
            )
            + " |"
        )
    lines.extend([
        "",
        "```mermaid",
        "flowchart TD",
        "  Start[backend denominator candidate] --> Mesh{mesh/object?}",
        "  Mesh -->|bridge only| MeshAB[reduced synthetic/replay A/B required]",
        "  Start --> Pos{position/binning?}",
        "  Pos -->|visible VSOut only| PosBlock[define real binning route before Xcode]",
        "  Start --> Tile{Tile-FFP?}",
        "  Tile -->|coverage gate| TileDecision[use coverage verdict before Xcode]",
        "```",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--tile-ffp-coverage-csv", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--csv-output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    rows = audit(args.repo_root, args.tile_ffp_coverage_csv)
    if args.csv_output:
        write_csv(args.csv_output, rows)
    if args.output:
        write_markdown(args.output, rows, args.repo_root)
    if not args.output and not args.csv_output:
        for row in rows:
            print(f"{row['candidate']}: {row['verdict']} - {row['reason']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
