#!/usr/bin/env python3
"""Regression tests for 3DMark05 perf gate summaries."""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_3dmark05_perf_gates.py"


VS_FIELDS = [
    "run",
    "top_row_keys",
    "gpu_ms",
    "vs_buffer_mib",
    "vs_invocations",
    "vs_b_per_vs_invocation",
    "draw_calls",
    "dxmt_vertex_count",
    "primitives",
]


SEMANTIC_FIELDS = [
    "rank",
    "verdict",
    "visible_exact_lru32_delta",
    "sparse_exact_lru32_delta",
    "no_final_color_lru32_delta",
    "visible_fail_lru32_delta",
]


PRIMITIVE_FIELDS = ["metric", "verdict"]
IMAGE_FIELDS = [
    "area",
    "changed_pixels",
    "changed_pct",
    "before_active_pct",
    "after_active_pct",
    "max_delta",
    "mean_abs_delta",
    "rms_delta",
    "ssim",
]
PROXY_FIELDS = [
    "group",
    "proof_family",
    "semantic_risk",
    "xcode_proxy_hidden_backend_mib",
    "xcode_proxy_vs_write_mib",
    "xcode_proxy_gpu_ms",
    "miss32_delta",
]
DELTA_FIELDS = [
    "run",
    "candidate_kind",
    "verdict",
    "backend_shape_gate",
    "backend_shape_reason",
    "geometry_stable",
    "row_keys_match",
    "gpu_delta_pct",
    "vs_delta_pct",
    "vs_invocations_delta_pct",
    "vs_b_per_inv_delta_pct",
    "vs_write_delta_mib",
    "invocation_effect_mib",
    "bytes_per_invocation_effect_mib",
    "residual_mib",
    "primary_mover",
]
SELECTOR_FIELDS = [
    "queue",
    "selector",
    "verdict",
    "kept_draws",
    "kept_fail",
    "lru32_delta",
    "gain_share",
    "mixed_all_fail_groups",
    "meaning",
]


def write_csv(path: Path, fields: list[str], rows: list[dict[str, object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


class Summarize3DMark05PerfGatesTests(unittest.TestCase):
    def write_gate_inputs(
        self,
        root: Path,
        *,
        screen_blend_gpu_ms: float = 29.0,
        screen_blend_vs_invocations: float = 850.0,
        extra_vs_delta_rows: list[dict[str, object]] | None = None,
    ) -> tuple[Path, Path, Path, Path, Path, Path]:
        vs_scaling = root / "vs-scaling.csv"
        vs_delta = root / "vs-delta.csv"
        semantic = root / "semantic.csv"
        primitive = root / "primitive.csv"
        selector = root / "selector.csv"
        screen_blend = root / "screen-blend-image.csv"
        write_csv(vs_scaling, VS_FIELDS, [
            {
                "run": "current-normal-frame50-gputrace-r1",
                "top_row_keys": "50/2,50/1,50/0",
                "gpu_ms": 34.0,
                "vs_buffer_mib": 1600.0,
                "vs_invocations": 1000.0,
                "vs_b_per_vs_invocation": 1600.0,
                "draw_calls": 100,
                "dxmt_vertex_count": 2000,
                "primitives": 700,
            },
            {
                "run": "half-vsout-gputrace-r1",
                "top_row_keys": "50/2,50/1,50/0",
                "gpu_ms": 35.0,
                "vs_buffer_mib": 1580.0,
                "vs_invocations": 998.0,
                "vs_b_per_vs_invocation": 1583.0,
                "draw_calls": 100,
                "dxmt_vertex_count": 2000,
                "primitives": 700,
            },
            {
                "run": "opaque-depth-index-cache-proof-r1",
                "top_row_keys": "50/2,50/1,50/0",
                "gpu_ms": 31.0,
                "vs_buffer_mib": 1480.0,
                "vs_invocations": 900.0,
                "vs_b_per_vs_invocation": 1644.0,
                "draw_calls": 100,
                "dxmt_vertex_count": 2000,
                "primitives": 700,
            },
            {
                "run": "combined-opaque-screenblend-index-cache-gputrace-r1",
                "top_row_keys": "50/2,50/1,50/0",
                "gpu_ms": screen_blend_gpu_ms,
                "vs_buffer_mib": 1400.0,
                "vs_invocations": screen_blend_vs_invocations,
                "vs_b_per_vs_invocation": 1647.0,
                "draw_calls": 100,
                "dxmt_vertex_count": 2000,
                "primitives": 700,
            },
        ])
        delta_rows: list[dict[str, object]] = [
            {
                "run": "combined-opaque-screenblend-index-cache-gputrace-r1",
                "candidate_kind": "locality-reorder",
                "verdict": "shape-stable VS-moved",
                "backend_shape_gate": "not-applicable",
                "backend_shape_reason": "locality-reorder candidate",
                "geometry_stable": "True",
                "row_keys_match": "True",
                "gpu_delta_pct": -14.7,
                "vs_delta_pct": -12.5,
                "vs_invocations_delta_pct": -15.0,
                "vs_b_per_inv_delta_pct": 3.0,
                "vs_write_delta_mib": -200.0,
                "invocation_effect_mib": -220.0,
                "bytes_per_invocation_effect_mib": 20.0,
                "residual_mib": 0.0,
                "primary_mover": "invocations",
            },
            {
                "run": "opaque-depth-index-cache-proof-r1",
                "candidate_kind": "locality-reorder",
                "verdict": "shape-stable VS-moved",
                "backend_shape_gate": "not-applicable",
                "backend_shape_reason": "locality-reorder candidate",
                "geometry_stable": "True",
                "row_keys_match": "True",
                "gpu_delta_pct": -8.8,
                "vs_delta_pct": -7.5,
                "vs_invocations_delta_pct": -10.0,
                "vs_b_per_inv_delta_pct": 2.75,
                "vs_write_delta_mib": -120.0,
                "invocation_effect_mib": -150.0,
                "bytes_per_invocation_effect_mib": 30.0,
                "residual_mib": 0.0,
                "primary_mover": "invocations",
            },
            {
                "run": "half-vsout-gputrace-r1",
                "candidate_kind": "non-reorder-backend-shape",
                "verdict": "shape-stable GPU-only",
                "backend_shape_gate": "reject",
                "backend_shape_reason": "bytes/inv reduction < 5%; GPU did not improve by >= 2%",
                "geometry_stable": "True",
                "row_keys_match": "True",
                "gpu_delta_pct": 2.94,
                "vs_delta_pct": -1.25,
                "vs_invocations_delta_pct": -0.2,
                "vs_b_per_inv_delta_pct": -1.06,
                "vs_write_delta_mib": -20.0,
                "invocation_effect_mib": -3.0,
                "bytes_per_invocation_effect_mib": -17.0,
                "residual_mib": 0.0,
                "primary_mover": "bytes_per_invocation",
            },
        ]
        if extra_vs_delta_rows:
            delta_rows.extend(extra_vs_delta_rows)
        write_csv(vs_delta, DELTA_FIELDS, delta_rows)
        write_csv(semantic, SEMANTIC_FIELDS, [
            {
                "rank": 1,
                "verdict": "visible-fail",
                "visible_exact_lru32_delta": -80,
                "sparse_exact_lru32_delta": -20,
                "no_final_color_lru32_delta": -10,
                "visible_fail_lru32_delta": -5,
            },
        ])
        write_csv(primitive, PRIMITIVE_FIELDS, [
            {"metric": "max_color_delta", "verdict": "exact-fail-only-positive"},
            {"metric": "max_abs_depth_delta", "verdict": "overlap"},
        ])
        write_csv(selector, SELECTOR_FIELDS, [
            {
                "queue": "runtime-field-combination",
                "selector": "state.index_count",
                "verdict": "runtime-scout",
                "kept_draws": "0,1,8,9,10,11,12,13,14,15,16,17,18",
                "kept_fail": 0,
                "lru32_delta": -3778,
                "gain_share": "30.92%",
                "mixed_all_fail_groups": "1 / 0",
                "meaning": "best runtime-shaped selector remains narrow",
            },
            {
                "queue": "runtime-field-combination",
                "selector": "constant.vsconsts_hash",
                "verdict": "trace-local-constant",
                "kept_draws": "0,1,2,3,5,6,7,8,9,10,11,12,13,14,15,16,17,18",
                "kept_fail": 0,
                "lru32_delta": -10813,
                "gain_share": "88.49%",
                "mixed_all_fail_groups": "0 / 1",
                "meaning": "trace-local only",
            },
            {
                "queue": "final-color-runtime-selector",
                "selector": "state.index_count",
                "verdict": "runtime-scout",
                "kept_draws": "1,3",
                "kept_fail": 0,
                "lru32_delta": -3200,
                "gain_share": "40.00%",
                "mixed_all_fail_groups": "1 / 0",
                "meaning": "runtime field does not keep enough visible exact gain",
            },
            {
                "queue": "final-color-runtime-selector",
                "selector": "constant.vsconsts_hash",
                "verdict": "trace-local-constant",
                "kept_draws": "1,3,5,7",
                "kept_fail": 0,
                "lru32_delta": -8000,
                "gain_share": "100.00%",
                "mixed_all_fail_groups": "0 / 1",
                "meaning": "trace-local only",
            },
            {
                "queue": "final-color-runtime-blocker",
                "selector": "all-runtime-visible-fields",
                "verdict": "runtime-indistinguishable-target-fail",
                "kept_draws": "3,5,6,7",
                "kept_fail": 1,
                "lru32_delta": -5628,
                "gain_share": "",
                "mixed_all_fail_groups": "fail_lru32=-1407",
                "meaning": "41 runtime/geometry/shader fields are identical; only trace-local fields can split this blocker",
            },
            {
                "queue": "final-writer-runtime-selector",
                "selector": "constant.vsconsts_hash",
                "verdict": "trace-local-constant",
                "kept_draws": "2,3,5,6,7",
                "kept_fail": 0,
                "lru32_delta": -7035,
                "gain_share": "100.00%",
                "mixed_all_fail_groups": "0 / 1",
                "meaning": "trace-local constant payload can isolate owner-stable draws in this replay only",
            },
            {
                "queue": "final-writer-runtime-selector",
                "selector": "runtime.uniform_payload_hash",
                "verdict": "runtime-payload-overfit",
                "kept_draws": "2,3,5,6,7",
                "kept_fail": 0,
                "lru32_delta": -7035,
                "gain_share": "100.00%",
                "mixed_all_fail_groups": "0 / 1",
                "meaning": "full runtime uniform-payload hash can isolate owner-stable draws but overfits identity",
            },
        ])
        write_csv(screen_blend, IMAGE_FIELDS, [
            {
                "area": "full",
                "changed_pixels": 739,
                "changed_pct": "0.093969",
                "before_active_pct": "22.6",
                "after_active_pct": "22.6",
                "max_delta": 1,
                "mean_abs_delta": "0.000342",
                "rms_delta": "0.018506",
                "ssim": "1.0",
            },
        ])
        return vs_scaling, vs_delta, semantic, primitive, selector, screen_blend

    def test_rejects_backend_shape_and_broad_reorder_but_keeps_production_locality(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vs_scaling, vs_delta, semantic, primitive, selector, screen_blend = self.write_gate_inputs(root)
            report = root / "gates.md"
            summary = root / "gates.csv"
            semantic_queue = root / "semantic-queue.csv"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--vs-scaling-csv",
                    str(vs_scaling),
                    "--vs-delta-csv",
                    str(vs_delta),
                    "--semantic-candidates-csv",
                    str(semantic),
                    "--primitive-selector-csv",
                    str(primitive),
                    "--semantic-selector-csv",
                    str(selector),
                    "--screen-blend-semantic-csv",
                    str(screen_blend),
                    "--screen-blend-semantic-policy",
                    "lsb1",
                    "--output",
                    str(report),
                    "--csv-output",
                    str(summary),
                    "--semantic-queue-csv-output",
                    str(semantic_queue),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("`non-reorder-backend-shape` | `reject`", text)
            self.assertIn("`vs-write-attribution` | `locality-dominant-backend-rejected`", text)
            self.assertIn("`production-locality` | `keep`", text)
            self.assertIn("`broad-depth-read-reorder` | `reject`", text)
            self.assertIn("`screen-blend-explicit-tolerance` | `explicit-tolerance-pass`", text)
            self.assertIn("`runtime-selector-scout` | `insufficient-runtime-selector`", text)
            self.assertIn("`final-color-runtime-selector` | `insufficient-final-color-selector`", text)
            self.assertIn("`final-writer-runtime-selector` | `overfit-only`", text)
            self.assertIn("`final-color-runtime-blocker` | `runtime-indistinguishable-blocker`", text)
            self.assertIn("`overall` | `accepted-opaque-plus-explicit-screenblend`", text)
            self.assertIn("## Implementation Track Queue", text)
            self.assertIn("`accepted-production-locality` | `keep`", text)
            self.assertIn("`explicit-screenblend-locality` | `explicit-tolerance-only`", text)
            self.assertIn("`final-color-occlusion-predicate` | `blocked-runtime-indistinguishable`", text)
            self.assertIn("`non-reorder-backend-mechanism` | `needs-new-mechanism`", text)
            self.assertIn("locality win(s) are invocation-driven", text)
            self.assertIn("## Semantic Final-Color Queue", text)
            self.assertIn("| `visible-exact-pass` | `candidate-final-color-selector` | `-80` |", text)
            self.assertIn("| `visible-fail` | `blocks-broad-reorder` | `-5` |", text)
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = {row["gate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["primitive-conflict-selector"]["verdict"], "final-color-oracle-required")
            self.assertEqual(rows["screen-blend-explicit-tolerance"]["verdict"], "explicit-tolerance-pass")
            self.assertEqual(rows["vs-write-attribution"]["verdict"], "locality-dominant-backend-rejected")
            self.assertEqual(rows["runtime-selector-scout"]["verdict"], "insufficient-runtime-selector")
            self.assertEqual(rows["final-color-runtime-selector"]["verdict"], "insufficient-final-color-selector")
            self.assertEqual(rows["final-writer-runtime-selector"]["verdict"], "overfit-only")
            self.assertIn("runtime.uniform_payload_hash=runtime-payload-overfit", rows["final-writer-runtime-selector"]["evidence"])
            self.assertEqual(rows["final-color-runtime-blocker"]["verdict"], "runtime-indistinguishable-blocker")
            self.assertEqual(rows["overall"]["verdict"], "accepted-opaque-plus-explicit-screenblend")
            with semantic_queue.open(newline="", encoding="utf-8") as handle:
                queue_rows = list(csv.DictReader(handle))
            self.assertEqual([row["bucket"] for row in queue_rows], [
                "visible-exact-pass",
                "visible-fail",
                "sparse-exact-pass",
                "no-final-color-exact-pass",
            ])
            self.assertEqual(queue_rows[0]["oracle_status"], "candidate-final-color-selector")
            self.assertEqual(queue_rows[1]["oracle_status"], "blocks-broad-reorder")

    def test_vs_write_attribution_prefers_non_rejected_backend_candidate_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vs_scaling, vs_delta, semantic, primitive, selector, screen_blend = self.write_gate_inputs(
                root,
                extra_vs_delta_rows=[
                    {
                        "run": "backend-shape-preflight-r1",
                        "candidate_kind": "non-reorder-backend-shape",
                        "verdict": "shape-stable preflight-only",
                        "backend_shape_gate": "preflight",
                        "backend_shape_reason": "cheap bytes/inv proxy moved; Xcode gate not run",
                        "geometry_stable": "True",
                        "row_keys_match": "True",
                        "gpu_delta_pct": 9.0,
                        "vs_delta_pct": -5.0,
                        "vs_invocations_delta_pct": 0.0,
                        "vs_b_per_inv_delta_pct": -5.0,
                        "vs_write_delta_mib": -80.0,
                        "invocation_effect_mib": 0.0,
                        "bytes_per_invocation_effect_mib": -80.0,
                        "residual_mib": 0.0,
                        "primary_mover": "bytes_per_invocation",
                    },
                ],
            )
            report = root / "gates.md"
            summary = root / "gates.csv"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--vs-scaling-csv",
                    str(vs_scaling),
                    "--vs-delta-csv",
                    str(vs_delta),
                    "--semantic-candidates-csv",
                    str(semantic),
                    "--primitive-selector-csv",
                    str(primitive),
                    "--semantic-selector-csv",
                    str(selector),
                    "--screen-blend-semantic-csv",
                    str(screen_blend),
                    "--screen-blend-semantic-policy",
                    "lsb1",
                    "--output",
                    str(report),
                    "--csv-output",
                    str(summary),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = {row["gate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["vs-write-attribution"]["verdict"], "locality-dominant-backend-rejected")
            self.assertIn("best backend backend-shape-preflight-r1", rows["vs-write-attribution"]["evidence"])
            self.assertIn("gate=preflight", rows["vs-write-attribution"]["evidence"])

    def test_screen_blend_candidate_reports_missing_semantic_image_when_not_attached(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vs_scaling, _vs_delta, semantic, primitive, _selector, _screen_blend = self.write_gate_inputs(root)
            report = root / "gates.md"
            summary = root / "gates.csv"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--vs-scaling-csv",
                    str(vs_scaling),
                    "--semantic-candidates-csv",
                    str(semantic),
                    "--primitive-selector-csv",
                    str(primitive),
                    "--output",
                    str(report),
                    "--csv-output",
                    str(summary),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("`screen-blend-explicit-tolerance` | `missing-semantic-image`", text)
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = {row["gate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["screen-blend-explicit-tolerance"]["verdict"], "missing-semantic-image")
            self.assertEqual(rows["overall"]["verdict"], "semantic-safe-locality-only")

    def test_screen_blend_candidate_without_xcode_movement_does_not_request_semantic_image_first(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vs_scaling, _vs_delta, semantic, primitive, _selector, _screen_blend = self.write_gate_inputs(
                root,
                screen_blend_gpu_ms=34.0,
                screen_blend_vs_invocations=1000.0,
            )
            report = root / "gates.md"
            summary = root / "gates.csv"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--vs-scaling-csv",
                    str(vs_scaling),
                    "--semantic-candidates-csv",
                    str(semantic),
                    "--primitive-selector-csv",
                    str(primitive),
                    "--output",
                    str(report),
                    "--csv-output",
                    str(summary),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("`screen-blend-explicit-tolerance` | `missing-xcode-movement`", text)
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = {row["gate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["screen-blend-explicit-tolerance"]["verdict"], "missing-xcode-movement")
            self.assertEqual(rows["overall"]["verdict"], "semantic-safe-locality-only")

    def test_class_proxy_queue_is_gate_aware(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vs_scaling, vs_delta, semantic, primitive, selector, screen_blend = self.write_gate_inputs(root)
            proxy = root / "class-proxy.csv"
            write_csv(proxy, PROXY_FIELDS, [
                {
                    "group": "50/2|depth=read|blend=screen|large4096=yes",
                    "proof_family": "explicit-tolerance-reorder",
                    "semantic_risk": "screen-blend-tolerance",
                    "xcode_proxy_hidden_backend_mib": 240.0,
                    "xcode_proxy_vs_write_mib": 246.0,
                    "xcode_proxy_gpu_ms": 5.1,
                    "miss32_delta": -23502,
                },
                {
                    "group": "50/1|depth=write|blend=off|large4096=no",
                    "proof_family": "production-opaque-reorder",
                    "semantic_risk": "low-opaque-depth-write",
                    "xcode_proxy_hidden_backend_mib": 219.0,
                    "xcode_proxy_vs_write_mib": 221.0,
                    "xcode_proxy_gpu_ms": 4.6,
                    "miss32_delta": -52211,
                },
                {
                    "group": "50/2|depth=read|blend=off|textured=yes",
                    "proof_family": "semantic-proof-or-non-reorder",
                    "semantic_risk": "medium-depth-read-order-sensitive",
                    "xcode_proxy_hidden_backend_mib": 180.0,
                    "xcode_proxy_vs_write_mib": 190.0,
                    "xcode_proxy_gpu_ms": 3.9,
                    "miss32_delta": -8446,
                },
            ])
            report = root / "gates.md"
            queue = root / "gates-queue.csv"

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--vs-scaling-csv",
                    str(vs_scaling),
                    "--vs-delta-csv",
                    str(vs_delta),
                    "--semantic-candidates-csv",
                    str(semantic),
                    "--primitive-selector-csv",
                    str(primitive),
                    "--semantic-selector-csv",
                    str(selector),
                    "--screen-blend-semantic-csv",
                    str(screen_blend),
                    "--screen-blend-semantic-policy",
                    "lsb1",
                    "--class-proxy-csv",
                    str(proxy),
                    "--output",
                    str(report),
                    "--queue-csv-output",
                    str(queue),
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("## Implementation Track Queue", text)
            self.assertIn("`blocked-runtime-indistinguishable`", text)
            self.assertIn("`needs-new-mechanism`", text)
            self.assertIn("## Semantic Final-Color Queue", text)
            self.assertIn("## Next Experiment Queue", text)
            self.assertIn("`explicit-tolerance-only`", text)
            self.assertIn("`covered-production-path`", text)
            self.assertIn("`blocked-final-color-oracle`", text)
            self.assertIn("final-writer-runtime-selector=overfit-only", text)
            with queue.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual([row["gate_status"] for row in rows], [
                "explicit-tolerance-only",
                "covered-production-path",
                "blocked-final-color-oracle",
            ])
            self.assertEqual(rows[0]["source"], str(proxy))
            self.assertEqual(rows[0]["rank"], "1")


if __name__ == "__main__":
    unittest.main()
