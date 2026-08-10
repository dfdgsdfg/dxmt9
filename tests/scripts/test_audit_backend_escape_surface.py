#!/usr/bin/env python3

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "audit_backend_escape_surface.py"


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


class AuditBackendEscapeSurfaceTests(unittest.TestCase):
    def write_fixture_repo(self, root: Path, *, mesh_route: bool = False) -> Path:
        write(
            root / "src" / "winemetal" / "winemetal.h",
            "struct WMTMeshRenderPipelineInfo { obj_handle_t object_function; "
            "obj_handle_t mesh_function; }; "
            "struct WMTTileRenderPipelineInfo {}; "
            "struct wmtcmd_render_draw_meshthreadgroups {};",
        )
        write(
            root / "src" / "winemetal" / "Metal.hpp",
            "inline void InitializeMeshRenderPipelineInfo(WMTMeshRenderPipelineInfo& info) "
            "{ info.object_function = 0; info.mesh_function = 0; }",
        )
        write(
            root / "src" / "winemetal" / "unix" / "winemetal_private_api.mm",
            "[enc drawMeshThreadgroups:MTLSizeMake(1,1,1) threadsPerObjectThreadgroup:MTLSizeMake(1,1,1) "
            "threadsPerMeshThreadgroup:MTLSizeMake(1,1,1)]; "
            "MTLRenderCommandEncoder_dispatchThreadsPerTile();",
        )
        dxmt9 = (
            "fragmentlessDepthOnly positionOnlyVSOutLayout "
            "selectTileFfpForPass makeFfpTilePixelSource"
        )
        if mesh_route:
            dxmt9 += " WMTMeshRenderPipelineInfo [[mesh]] drawMeshThreadgroups"
        write(root / "src" / "dxmt9" / "surface.cpp", dxmt9)
        coverage = root / "coverage.csv"
        write_csv(coverage, [{
            "row": "60/2",
            "verdict": "no-tile-ffp-coverage",
        }])
        return coverage

    def test_reports_bridge_only_mesh_visible_position_and_rejected_tile_coverage(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            coverage = self.write_fixture_repo(root)
            out = root / "audit.csv"
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--repo-root",
                    str(root),
                    "--tile-ffp-coverage-csv",
                    str(coverage),
                    "--csv-output",
                    str(out),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with out.open(newline="", encoding="utf-8") as handle:
                rows = {row["candidate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["mesh-object"]["verdict"], "bridge-only-reduced-ab-required")
            self.assertEqual(rows["position-binning"]["verdict"], "visible-vsout-probe-only")
            self.assertEqual(rows["tile-ffp"]["verdict"], "rejected-current-coverage")

    def test_reports_mesh_candidate_when_route_and_emitter_exist(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            coverage = self.write_fixture_repo(root, mesh_route=True)
            out = root / "audit.csv"
            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--repo-root",
                    str(root),
                    "--tile-ffp-coverage-csv",
                    str(coverage),
                    "--csv-output",
                    str(out),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=True,
            )

            with out.open(newline="", encoding="utf-8") as handle:
                rows = {row["candidate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["mesh-object"]["verdict"], "candidate-route-present")


if __name__ == "__main__":
    unittest.main()
