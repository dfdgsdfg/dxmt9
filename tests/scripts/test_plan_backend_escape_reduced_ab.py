#!/usr/bin/env python3

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "plan_backend_escape_reduced_ab.py"


FIELDS = [
    "candidate",
    "bridge_surface",
    "dxmt9_route",
    "shader_emitter",
    "current_gt1_evidence",
    "verdict",
    "reason",
    "next_action",
]

EXPANSION_FIELDS = [
    "row",
    "verdict",
    "dominant_blocker",
    "primitives",
]


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


class PlanBackendEscapeReducedAbTests(unittest.TestCase):
    def run_script(
        self,
        root: Path,
        rows: list[dict[str, str]],
        *,
        expansion_rows: list[dict[str, str]] | None = None,
    ) -> tuple[Path, Path]:
        audit = root / "backend-escape-surface.csv"
        report = root / "plan.md"
        out = root / "plan.csv"
        write_csv(audit, rows)
        expansion = root / "tile-expansion.csv"
        if expansion_rows is not None:
            write_csv(expansion, expansion_rows)
        command = [
            sys.executable,
            str(SCRIPT),
            "--backend-escape-surface-csv",
            str(audit),
            "--output",
            str(report),
            "--csv-output",
            str(out),
        ]
        if expansion_rows is not None:
            command.extend(["--tile-ffp-expansion-csv", str(expansion)])
        result = subprocess.run(
            command,
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return report, out

    def test_blocks_current_audit_before_reduced_ab(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            report, out = self.run_script(Path(td), [
                {
                    "candidate": "mesh-object",
                    "bridge_surface": "present",
                    "dxmt9_route": "missing",
                    "shader_emitter": "missing",
                    "current_gt1_evidence": "none",
                    "verdict": "bridge-only-reduced-ab-required",
                    "reason": "bridge only",
                    "next_action": "build reduced A/B",
                },
                {
                    "candidate": "position-binning",
                    "bridge_surface": "ordinary-render",
                    "dxmt9_route": "missing",
                    "shader_emitter": "visible-vsout-probe",
                    "current_gt1_evidence": "visible-width Xcode rejected",
                    "verdict": "visible-vsout-probe-only",
                    "reason": "visible only",
                    "next_action": "define route",
                },
                {
                    "candidate": "tile-ffp",
                    "bridge_surface": "present",
                    "dxmt9_route": "present",
                    "shader_emitter": "present",
                    "current_gt1_evidence": "no coverage",
                    "verdict": "rejected-current-coverage",
                    "reason": "no coverage",
                    "next_action": "expand coverage",
                },
            ])

            text = report.read_text(encoding="utf-8")
            self.assertIn("Overall verdict: `blocked-before-reduced-ab`", text)
            with out.open(newline="", encoding="utf-8") as handle:
                rows = {row["candidate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["mesh-object"]["reduced_ab_status"], "blocked-missing-dxmt9-route")
            self.assertEqual(rows["position-binning"]["reduced_ab_status"], "blocked-real-route-missing")
            self.assertEqual(rows["tile-ffp"]["reduced_ab_status"], "blocked-hot-row-coverage")

    def test_tile_expansion_updates_tile_next_action(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            report, out = self.run_script(
                Path(td),
                [
                    {
                        "candidate": "tile-ffp",
                        "bridge_surface": "present",
                        "dxmt9_route": "present",
                        "shader_emitter": "present",
                        "current_gt1_evidence": "no coverage",
                        "verdict": "rejected-current-coverage",
                        "reason": "no coverage",
                        "next_action": "expand coverage",
                    },
                ],
                expansion_rows=[
                    {
                        "row": "60/2",
                        "verdict": "needs-programmable-tile-route",
                        "dominant_blocker": "not-ffp",
                        "primitives": "389376",
                    },
                    {
                        "row": "60/1",
                        "verdict": "needs-programmable-tile-route",
                        "dominant_blocker": "not-ffp",
                        "primitives": "228725",
                    },
                ],
            )

            text = report.read_text(encoding="utf-8")
            self.assertIn("needs-programmable-tile-route", text)
            with out.open(newline="", encoding="utf-8") as handle:
                rows = {row["candidate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["tile-ffp"]["expansion_status"], "needs-programmable-tile-route")
            self.assertIn("programmable/textured tile", rows["tile-ffp"]["next_action"])

    def test_ready_candidate_becomes_reduced_ab_queue(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            report, out = self.run_script(Path(td), [
                {
                    "candidate": "tile-ffp",
                    "bridge_surface": "present",
                    "dxmt9_route": "present",
                    "shader_emitter": "present",
                    "current_gt1_evidence": "candidate coverage",
                    "verdict": "candidate-coverage",
                    "reason": "coverage",
                    "next_action": "run equality",
                },
                {
                    "candidate": "mesh-object",
                    "bridge_surface": "present",
                    "dxmt9_route": "present",
                    "shader_emitter": "present",
                    "current_gt1_evidence": "synthetic route",
                    "verdict": "candidate-route-present",
                    "reason": "route",
                    "next_action": "run reduced A/B",
                },
            ])

            text = report.read_text(encoding="utf-8")
            self.assertIn("Overall verdict: `ready-reduced-ab`", text)
            with out.open(newline="", encoding="utf-8") as handle:
                rows = {row["candidate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["tile-ffp"]["reduced_ab_status"], "ready-reduced-ab")
            self.assertIn("portable FFP render path", rows["tile-ffp"]["control"])
            self.assertEqual(rows["mesh-object"]["surface_status"], "route-or-coverage-present")
            self.assertIn("mesh/object route", rows["mesh-object"]["treatment"])


if __name__ == "__main__":
    unittest.main()
