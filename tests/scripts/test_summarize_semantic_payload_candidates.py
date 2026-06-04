#!/usr/bin/env python3
"""Regression tests for semantic payload candidate summaries."""

from __future__ import annotations

import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_semantic_payload_candidates.py"


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    keys: list[str] = []
    for row in rows:
        for key in row:
            if key not in keys:
                keys.append(key)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)


def write_compare(path: Path, changed: int, active: int) -> None:
    write_csv(path, [{
        "area": "full",
        "width": 1024,
        "height": 768,
        "compared_pixels": 786432,
        "changed_pixels": changed,
        "changed_pct": "0.0",
        "before_active_pixels": active,
        "before_active_pct": "0.0",
        "after_active_pixels": active,
        "after_active_pct": "0.0",
        "max_delta": 0,
        "mean_abs_delta": "0.0",
        "rms_delta": "0.0",
        "ssim": "1.0",
    }])


def write_summary(path: Path, lru32_delta: int, lru32_pct: float) -> None:
    path.write_text(json.dumps({
        "draw_count": 2,
        "index_cache_estimate": {
            "original_index_count": 100,
            "original_unique_indices_per_draw": 50,
            "replay_lru32_miss_delta": lru32_delta,
            "replay_lru32_miss_delta_pct": lru32_pct,
            "replay_lru64_miss_delta": -5,
            "replay_lru64_miss_delta_pct": -5.0,
        },
    }), encoding="utf-8")


class SummarizeSemanticPayloadCandidatesTests(unittest.TestCase):
    def test_rejects_broad_reorder_when_visible_fail_and_sparse_passes_exist(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            candidates = root / "candidates.json"
            candidates.write_text(json.dumps({
                "schema": "dxmt9.3dmark05.payload_window_list.v1",
                "selections": [
                    {
                        "rank": rank,
                        "row": "50/2",
                        "window": {"encoder_draw_min": rank, "encoder_draw_max": rank + 1},
                        "group": {
                            "draws": 2,
                            "primitive_count": rank * 100,
                            "cache_miss64": rank * 200,
                            "vs": f"0xvs{rank}",
                            "ps": f"0xps{rank}",
                        },
                    }
                    for rank in [1, 2, 3, 4]
                ],
            }), encoding="utf-8")

            rank1 = root / "rank1.csv"
            write_csv(rank1, [
                {
                    "draw_index": 0,
                    "semantic_status": "pass",
                    "changed_pixels": 0,
                    "max_active_pixels": 0,
                    "lru32_delta": -10,
                },
                {
                    "draw_index": 1,
                    "semantic_status": "fail",
                    "changed_pixels": 3,
                    "max_active_pixels": 465,
                    "lru32_delta": -20,
                },
            ])
            compare2 = root / "rank2-compare.csv"
            summary2 = root / "rank2-summary.json"
            compare3 = root / "rank3-compare.csv"
            summary3 = root / "rank3-summary.json"
            compare4 = root / "rank4-compare.csv"
            summary4 = root / "rank4-summary.json"
            write_compare(compare2, changed=0, active=9)
            write_summary(summary2, lru32_delta=-100, lru32_pct=-25.0)
            write_compare(compare3, changed=0, active=0)
            write_summary(summary3, lru32_delta=-50, lru32_pct=-10.0)
            write_compare(compare4, changed=0, active=128)
            write_summary(summary4, lru32_delta=-200, lru32_pct=-40.0)

            output = root / "summary.md"
            csv_output = root / "summary.csv"
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--candidates",
                    str(candidates),
                    "--rank1-analysis",
                    str(rank1),
                    "--rank-outcome",
                    f"2={compare2},{summary2}",
                    "--rank-outcome",
                    f"3={compare3},{summary3}",
                    "--rank-outcome",
                    f"4={compare4},{summary4}",
                    "--output",
                    str(output),
                    "--csv-output",
                    str(csv_output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = output.read_text(encoding="utf-8")
            self.assertIn("Aggregate verdict: `reject-broad-reorder`", text)
            self.assertIn("`visible-fail`", text)
            self.assertIn("`sparse-exact-pass`", text)
            self.assertIn("`no-final-color-exact-pass`", text)
            self.assertIn("`visible-exact-pass`", text)
            self.assertIn("## Promotion Boundary", text)
            self.assertIn("| Visible fail | `-20` |", text)
            self.assertIn("| Visible exact-pass | `-200` |", text)
            self.assertIn("| No-final-color exact-pass | `-60` |", text)
            self.assertIn("## Final-Color Oracle Queue", text)
            self.assertIn("`candidate-final-color-selector`", text)
            self.assertIn("`blocks-broad-reorder`", text)
            self.assertIn("`sparse-positive-control`", text)
            self.assertIn("`no-final-color-positive-control`", text)
            self.assertIn("## Final-Color Oracle Bucket Queue", text)
            self.assertIn("| `visible-exact-pass` | `candidate-final-color-selector` | `-200` |", text)
            self.assertIn("| `visible-fail` | `blocks-broad-reorder` | `-20` |", text)
            with csv_output.open(newline="", encoding="utf-8") as handle:
                csv_rows = list(csv.DictReader(handle))
            self.assertEqual([row["verdict"] for row in csv_rows], [
                "visible-fail",
                "sparse-exact-pass",
                "no-final-color-exact-pass",
                "visible-exact-pass",
            ])
            self.assertEqual(csv_rows[0]["safe_lru32_delta"], "-10")
            self.assertEqual(csv_rows[0]["unsafe_lru32_delta"], "-20")
            self.assertEqual(csv_rows[0]["visible_fail_lru32_delta"], "-20")
            self.assertEqual(csv_rows[0]["oracle_status"], "blocks-broad-reorder")
            self.assertIn("reject broad reorder", csv_rows[0]["oracle_next_action"])
            self.assertEqual(csv_rows[1]["sparse_exact_lru32_delta"], "-100")
            self.assertEqual(csv_rows[1]["oracle_status"], "sparse-positive-control")
            self.assertEqual(csv_rows[2]["no_final_color_lru32_delta"], "-50")
            self.assertEqual(csv_rows[2]["oracle_status"], "no-final-color-positive-control")
            self.assertEqual(csv_rows[3]["visible_exact_lru32_delta"], "-200")
            self.assertEqual(csv_rows[3]["oracle_status"], "candidate-final-color-selector")


if __name__ == "__main__":
    unittest.main()
