#!/usr/bin/env python3
"""Regression tests for 3DMark05 perf gate summaries."""

from __future__ import annotations

import csv
import json
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
    "lru32_delta",
    "visible_exact_lru32_delta",
    "sparse_exact_lru32_delta",
    "no_final_color_lru32_delta",
    "visible_fail_lru32_delta",
    "visibility_draws",
    "visibility_positive_draws",
    "visibility_visible_samples_sum",
    "visibility_join_status",
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
    "candidate_miss32_delta",
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
SHADER_VARIANT_FIELDS = [
    "rank",
    "variant",
    "seq",
    "enc",
    "vs_buffer_write_mib",
    "source_file",
    "source_vsout_bytes",
    "variant_vsout_bytes",
    "compile_ok",
    "ir_scratch_bytes_estimate",
]
VISIBILITY_SUMMARY_FIELDS = [
    "class",
    "draws",
    "zero_draws",
    "positive_draws",
    "zero_pct",
    "metal_draw_min",
    "metal_draw_max",
    "source_primitives",
    "zero_source_primitives",
    "positive_source_primitives",
    "submitted_elements",
    "zero_submitted_elements",
    "positive_submitted_elements",
    "visible_samples_sum",
    "visible_samples_max",
    "probe_rows",
    "original_miss32",
    "candidate_miss32",
    "miss32_delta",
    "zero_miss32_delta",
    "positive_miss32_delta",
]
PSO_CHURN_FIELDS = [
    "row",
    "verdict",
    "draws",
    "triangles",
    "pso_changes",
    "pso_unique",
    "probe_draws",
    "probe_pso_changes",
    "probe_shader_variant_changes",
    "probe_stream0_handle_changes",
    "probe_index_buffer_changes",
    "probe_extra_binding_changes",
    "probe_handle_tuple_changes",
    "probe_handle_tuple_unique",
    "probe_handle_tuple_max_run",
    "probe_handle_tuple_avg_run",
    "probe_pso_isolated_run_count",
    "probe_pso_isolated_run_top",
]
LOCALITY_CEILING_FIELDS = [
    "bucket",
    "scope",
    "semantic_class",
    "original_lru32",
    "candidate_lru32",
    "lru32_delta",
    "changed_color_pixels",
    "owner_changed_pixels",
    "est_vs_inv_delta",
    "est_vs_write_mib_delta",
    "est_gpu_ms_delta",
    "pct_of_current_applied_lru32",
    "pct_of_flip_needed_lru32",
    "pct_of_non_target_drift_lru32",
    "source",
]
BACKEND_ESCAPE_FIELDS = [
    "candidate",
    "bridge_surface",
    "dxmt9_route",
    "shader_emitter",
    "current_gt1_evidence",
    "verdict",
    "reason",
    "next_action",
]
BACKEND_ESCAPE_REDUCED_AB_FIELDS = [
    "candidate",
    "audit_verdict",
    "surface_status",
    "reduced_ab_status",
    "control",
    "treatment",
    "route_gate",
    "equality_gate",
    "counter_gate",
    "gt1_promotion_gate",
    "reason",
    "next_action",
]


def write_csv(path: Path, fields: list[str], rows: list[dict[str, object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


class Summarize3DMark05PerfGatesTests(unittest.TestCase):
    def write_pso_backend_churn(self, root: Path) -> Path:
        pso = root / "pso-backend-churn.csv"
        write_csv(pso, PSO_CHURN_FIELDS, [
            {
                "row": "60/2",
                "verdict": "stream-ib-dominant",
                "draws": 187,
                "triangles": 389376,
                "pso_changes": 47,
                "pso_unique": 20,
                "probe_draws": 187,
                "probe_pso_changes": 47,
                "probe_shader_variant_changes": 78,
                "probe_stream0_handle_changes": 160,
                "probe_index_buffer_changes": 160,
                "probe_extra_binding_changes": 111,
                "probe_handle_tuple_changes": 160,
                "probe_handle_tuple_unique": 58,
                "probe_handle_tuple_max_run": 6,
                "probe_handle_tuple_avg_run": "1.161",
                "probe_pso_isolated_run_count": 0,
                "probe_pso_isolated_run_top": "",
            },
            {
                "row": "60/1",
                "verdict": "stream-ib-dominant",
                "draws": 156,
                "triangles": 228725,
                "pso_changes": 33,
                "pso_unique": 3,
                "probe_draws": 156,
                "probe_pso_changes": 33,
                "probe_shader_variant_changes": 39,
                "probe_stream0_handle_changes": 130,
                "probe_index_buffer_changes": 136,
                "probe_extra_binding_changes": 0,
                "probe_handle_tuple_changes": 136,
                "probe_handle_tuple_unique": 92,
                "probe_handle_tuple_max_run": 5,
                "probe_handle_tuple_avg_run": "1.139",
                "probe_pso_isolated_run_count": 0,
                "probe_pso_isolated_run_top": "",
            },
        ])
        return pso

    def write_locality_semantic_ceiling(self, root: Path) -> Path:
        ceiling = root / "locality-semantic-ceiling.csv"
        write_csv(ceiling, LOCALITY_CEILING_FIELDS, [
            {
                "bucket": "rank2_to_rank4_color_exact_owner_masked",
                "scope": "aggregate estimate",
                "semantic_class": "color-exact-but-owner-masked",
                "original_lru32": "",
                "candidate_lru32": "",
                "lru32_delta": -9113,
                "changed_color_pixels": 0,
                "owner_changed_pixels": 878,
                "est_vs_inv_delta": -7228,
                "est_vs_write_mib_delta": "-11.134",
                "est_gpu_ms_delta": "-0.071",
                "pct_of_current_applied_lru32": "10.47",
                "pct_of_flip_needed_lru32": "22.37",
                "pct_of_non_target_drift_lru32": "7.14",
                "source": "fixture",
            },
            {
                "bucket": "visibility_zero_sample_rows",
                "scope": "60/2 visibility scout bucket",
                "semantic_class": "no-sample-triage-only",
                "original_lru32": "",
                "candidate_lru32": "",
                "lru32_delta": -2016,
                "changed_color_pixels": "",
                "owner_changed_pixels": "",
                "est_vs_inv_delta": -1599,
                "est_vs_write_mib_delta": "-2.463",
                "est_gpu_ms_delta": "-0.016",
                "pct_of_current_applied_lru32": "2.32",
                "pct_of_flip_needed_lru32": "4.95",
                "pct_of_non_target_drift_lru32": "1.58",
                "source": "fixture",
            },
            {
                "bucket": "rank1_to_rank4_all_scoped",
                "scope": "aggregate estimate",
                "semantic_class": "includes-visible-fail-and-owner-masked",
                "original_lru32": "",
                "candidate_lru32": "",
                "lru32_delta": -23706,
                "changed_color_pixels": 2,
                "owner_changed_pixels": 885,
                "est_vs_inv_delta": -18803,
                "est_vs_write_mib_delta": "-28.964",
                "est_gpu_ms_delta": "-0.186",
                "pct_of_current_applied_lru32": "27.22",
                "pct_of_flip_needed_lru32": "58.20",
                "pct_of_non_target_drift_lru32": "18.57",
                "source": "fixture",
            },
            {
                "bucket": "visibility_positive_sample_rows",
                "scope": "60/2 visibility scout bucket",
                "semantic_class": "sample-visible-needs-final-color-proof",
                "original_lru32": "",
                "candidate_lru32": "",
                "lru32_delta": -180840,
                "changed_color_pixels": "",
                "owner_changed_pixels": "",
                "est_vs_inv_delta": -143441,
                "est_vs_write_mib_delta": "-220.954",
                "est_gpu_ms_delta": "-1.416",
                "pct_of_current_applied_lru32": "207.68",
                "pct_of_flip_needed_lru32": "444.01",
                "pct_of_non_target_drift_lru32": "141.64",
                "source": "fixture",
            },
        ])
        return ceiling

    def write_backend_escape_surface(self, root: Path) -> Path:
        audit = root / "backend-escape-surface.csv"
        write_csv(audit, BACKEND_ESCAPE_FIELDS, [
            {
                "candidate": "mesh-object",
                "bridge_surface": "present",
                "dxmt9_route": "missing",
                "shader_emitter": "missing",
                "current_gt1_evidence": "none",
                "verdict": "bridge-only-reduced-ab-required",
                "reason": "bridge only",
                "next_action": "build a reduced synthetic/replay A/B",
            },
            {
                "candidate": "position-binning",
                "bridge_surface": "ordinary-render",
                "dxmt9_route": "missing",
                "shader_emitter": "visible-vsout-probe",
                "current_gt1_evidence": "visible-width Xcode rejected",
                "verdict": "visible-vsout-probe-only",
                "reason": "visible-only",
                "next_action": "define a real binning route",
            },
            {
                "candidate": "tile-ffp",
                "bridge_surface": "present",
                "dxmt9_route": "present",
                "shader_emitter": "present",
                "current_gt1_evidence": "9 no coverage row(s)",
                "verdict": "rejected-current-coverage",
                "reason": "no coverage",
                "next_action": "keep Tile-FFP narrow",
            },
        ])
        return audit

    def write_backend_escape_reduced_ab_plan(self, root: Path) -> Path:
        plan = root / "backend-escape-reduced-ab-plan.csv"
        write_csv(plan, BACKEND_ESCAPE_REDUCED_AB_FIELDS, [
            {
                "candidate": "mesh-object",
                "audit_verdict": "bridge-only-reduced-ab-required",
                "surface_status": "bridge-only",
                "reduced_ab_status": "blocked-missing-dxmt9-route",
                "control": "ordinary render mini-replay",
                "treatment": "mesh/object route",
                "route_gate": "dxmt9 route and mesh/object shader emitter exist",
                "equality_gate": "same-input color equality plus primitive-owner stability",
                "counter_gate": "VS buffer bytes decreases at stable shape",
                "gt1_promotion_gate": "attach reduced proof before GT1",
                "reason": "bridge only",
                "next_action": "implement route/emitter",
            },
            {
                "candidate": "position-binning",
                "audit_verdict": "visible-vsout-probe-only",
                "surface_status": "visible-probe-only",
                "reduced_ab_status": "blocked-real-route-missing",
                "control": "ordinary render route",
                "treatment": "real position/binning route",
                "route_gate": "separate route token exists",
                "equality_gate": "depth/color semantics stay stable",
                "counter_gate": "hidden backend bytes decrease",
                "gt1_promotion_gate": "prove reduced movement before GT1",
                "reason": "visible-only",
                "next_action": "define real route",
            },
            {
                "candidate": "tile-ffp",
                "audit_verdict": "rejected-current-coverage",
                "surface_status": "route-present-coverage-rejected",
                "reduced_ab_status": "blocked-hot-row-coverage",
                "control": "portable FFP render path",
                "treatment": "tile FFP route",
                "route_gate": "eligible hot-row primitive coverage",
                "equality_gate": "portable-vs-tile image equality",
                "counter_gate": "tile route reduces relevant counters",
                "gt1_promotion_gate": "meaningful GT1 hot-row share",
                "reason": "no coverage",
                "next_action": "expand coverage",
            },
        ])
        return plan

    def write_semantic_replay_summary(
        self,
        root: Path,
        name: str,
        *,
        verdict: str,
        lru32_delta: int,
        color_pixels: int,
        owner_pixels: int,
        color_owner_pixels: int,
    ) -> Path:
        path = root / f"{name}-semantic-gate-summary.json"
        path.write_text(json.dumps({
            "schema": "dxmt9.3dmark05.semantic_replay_gate.v1",
            "verdict": verdict,
            "primitive_order": "cache-opt-lru32",
            "color_compare": {
                "changed_pixels": str(color_pixels),
                "max_delta": "1" if color_pixels else "0",
            },
            "owner_compare": {
                "canonical_owner_changed_pixels": owner_pixels,
                "canonical_color_and_owner_changed_pixels": color_owner_pixels,
            },
            "candidate_replay": {
                "index_cache_estimate": {
                    "replay_lru32_miss_delta": lru32_delta,
                },
            },
        }), encoding="utf-8")
        return path

    def write_gate_inputs(
        self,
        root: Path,
        *,
        screen_blend_gpu_ms: float = 29.0,
        screen_blend_vs_invocations: float = 850.0,
        opaque_run_name: str = "opaque-depth-index-cache-proof-r1",
        screen_blend_run_name: str = "combined-opaque-screenblend-index-cache-gputrace-r1",
        include_opaque_depth_run: bool = True,
        include_screen_blend_run: bool = True,
        extra_vs_delta_rows: list[dict[str, object]] | None = None,
    ) -> tuple[Path, Path, Path, Path, Path, Path]:
        vs_scaling = root / "vs-scaling.csv"
        vs_delta = root / "vs-delta.csv"
        semantic = root / "semantic.csv"
        primitive = root / "primitive.csv"
        selector = root / "selector.csv"
        screen_blend = root / "screen-blend-image.csv"
        vs_rows: list[dict[str, object]] = [
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
        ]
        if include_opaque_depth_run:
            vs_rows.append(
                {
                    "run": opaque_run_name,
                    "top_row_keys": "50/2,50/1,50/0",
                    "gpu_ms": 31.0,
                    "vs_buffer_mib": 1480.0,
                    "vs_invocations": 900.0,
                    "vs_b_per_vs_invocation": 1644.0,
                    "draw_calls": 100,
                    "dxmt_vertex_count": 2000,
                    "primitives": 700,
                },
            )
        if include_screen_blend_run:
            vs_rows.append(
                {
                    "run": screen_blend_run_name,
                    "top_row_keys": "50/2,50/1,50/0",
                    "gpu_ms": screen_blend_gpu_ms,
                    "vs_buffer_mib": 1400.0,
                    "vs_invocations": screen_blend_vs_invocations,
                    "vs_b_per_vs_invocation": 1647.0,
                    "draw_calls": 100,
                    "dxmt_vertex_count": 2000,
                    "primitives": 700,
                },
            )
        write_csv(vs_scaling, VS_FIELDS, vs_rows)
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
                "lru32_delta": -5,
                "visible_exact_lru32_delta": 0,
                "sparse_exact_lru32_delta": 0,
                "no_final_color_lru32_delta": 0,
                "visible_fail_lru32_delta": -5,
                "visibility_draws": 1,
                "visibility_positive_draws": 1,
                "visibility_visible_samples_sum": 256,
                "visibility_join_status": "sample-visible-visible-fail",
            },
            {
                "rank": 2,
                "verdict": "no-final-color-exact-pass",
                "lru32_delta": -10,
                "visible_exact_lru32_delta": 0,
                "sparse_exact_lru32_delta": 0,
                "no_final_color_lru32_delta": -10,
                "visible_fail_lru32_delta": 0,
                "visibility_draws": 1,
                "visibility_positive_draws": 1,
                "visibility_visible_samples_sum": 512,
                "visibility_join_status": "sample-visible-final-color-empty",
            },
            {
                "rank": 3,
                "verdict": "visible-exact-pass",
                "lru32_delta": -80,
                "visible_exact_lru32_delta": -80,
                "sparse_exact_lru32_delta": 0,
                "no_final_color_lru32_delta": 0,
                "visible_fail_lru32_delta": 0,
                "visibility_draws": 1,
                "visibility_positive_draws": 1,
                "visibility_visible_samples_sum": 128,
                "visibility_join_status": "sample-visible-visible-exact",
            },
            {
                "rank": 4,
                "verdict": "sparse-exact-pass",
                "lru32_delta": -20,
                "visible_exact_lru32_delta": 0,
                "sparse_exact_lru32_delta": -20,
                "no_final_color_lru32_delta": 0,
                "visible_fail_lru32_delta": 0,
                "visibility_draws": 1,
                "visibility_positive_draws": 1,
                "visibility_visible_samples_sum": 64,
                "visibility_join_status": "sample-visible-sparse-exact",
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
            visibility = root / "visibility-summary.csv"
            pso_churn = self.write_pso_backend_churn(root)
            locality_ceiling = self.write_locality_semantic_ceiling(root)
            backend_escape = self.write_backend_escape_surface(root)
            backend_escape_plan = self.write_backend_escape_reduced_ab_plan(root)
            semantic_replays = [
                self.write_semantic_replay_summary(
                    root,
                    "rank1",
                    verdict="fail-final-writer-hazard",
                    lru32_delta=-14593,
                    color_pixels=2,
                    owner_pixels=7,
                    color_owner_pixels=2,
                ),
                self.write_semantic_replay_summary(
                    root,
                    "rank2",
                    verdict="masked-final-writer-hazard",
                    lru32_delta=-5937,
                    color_pixels=0,
                    owner_pixels=809,
                    color_owner_pixels=0,
                ),
                self.write_semantic_replay_summary(
                    root,
                    "rank3",
                    verdict="masked-final-writer-hazard",
                    lru32_delta=-2452,
                    color_pixels=0,
                    owner_pixels=52,
                    color_owner_pixels=0,
                ),
            ]
            write_csv(visibility, VISIBILITY_SUMMARY_FIELDS, [
                {
                    "class": "60/2|depth=read|blend=on|scissor=off|textured=yes|large4096=no|color_write=0xf",
                    "draws": 100,
                    "zero_draws": 10,
                    "positive_draws": 90,
                    "zero_pct": "10.00%",
                    "metal_draw_min": 0,
                    "metal_draw_max": 99,
                    "source_primitives": 100000,
                    "zero_source_primitives": 1000,
                    "positive_source_primitives": 99000,
                    "submitted_elements": 300000,
                    "zero_submitted_elements": 3000,
                    "positive_submitted_elements": 297000,
                    "visible_samples_sum": 1000000,
                    "visible_samples_max": 100000,
                    "probe_rows": 100,
                    "original_miss32": 200000,
                    "candidate_miss32": 100000,
                    "miss32_delta": -100000,
                    "zero_miss32_delta": -1000,
                    "positive_miss32_delta": -99000,
                },
            ])

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
                    "--visibility-summary-csv",
                    str(visibility),
                    "--pso-backend-churn-csv",
                    str(pso_churn),
                    "--locality-semantic-ceiling-csv",
                    str(locality_ceiling),
                    "--backend-escape-surface-csv",
                    str(backend_escape),
                    "--backend-escape-reduced-ab-plan-csv",
                    str(backend_escape_plan),
                    "--semantic-replay-summary-json",
                    str(semantic_replays[0]),
                    "--semantic-replay-summary-json",
                    str(semantic_replays[1]),
                    "--semantic-replay-summary-json",
                    str(semantic_replays[2]),
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
            self.assertIn("`visibility-positive-oracle` | `reject-positive-oracle`", text)
            self.assertIn("`visibility-no-sample-hotpath` | `reject-hotpath`", text)
            self.assertIn("`pso-backend-isolation` | `reject-current`", text)
            self.assertIn("`locality-semantic-ceiling` | `oracle-required`", text)
            self.assertIn("`final-writer-replay-oracle` | `blocked-final-writer-hazard`", text)
            self.assertIn("`backend-escape-surface` | `reduced-ab-required`", text)
            self.assertIn("`backend-escape-reduced-ab-plan` | `blocked-before-reduced-ab`", text)
            self.assertIn("`final-color-runtime-selector` | `insufficient-final-color-selector`", text)
            self.assertIn("`final-writer-runtime-selector` | `overfit-only`", text)
            self.assertIn("`final-color-runtime-blocker` | `runtime-indistinguishable-blocker`", text)
            self.assertIn("`overall` | `accepted-opaque-plus-explicit-screenblend`", text)
            self.assertIn("current D3D9 occlusion query is primitive-count only", text)
            self.assertIn("## Implementation Track Queue", text)
            self.assertIn("`accepted-production-locality` | `keep`", text)
            self.assertIn("`explicit-screenblend-locality` | `explicit-tolerance-only`", text)
            self.assertIn("`final-writer-replay-oracle` | `blocked-final-writer-hazard`", text)
            self.assertIn("`final-color-occlusion-predicate` | `blocked-runtime-indistinguishable`", text)
            self.assertIn("`locality-semantic-ceiling` | `oracle-required`", text)
            self.assertIn("`backend-escape-surface` | `reduced-ab-required`", text)
            self.assertIn("`backend-escape-reduced-ab-plan` | `blocked-before-reduced-ab`", text)
            self.assertIn("`non-reorder-backend-mechanism` | `needs-new-mechanism`", text)
            self.assertIn("`pso-backend-spill` | `blocked-current-telemetry`", text)
            self.assertIn("no-sample rows are not the current hotpath", text)
            self.assertIn("only a final-color/final-writer oracle that keeps enough sample-visible rows", text)
            self.assertIn("do not spend Xcode on current PSO churn", text)
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
            self.assertEqual(rows["visibility-positive-oracle"]["verdict"], "reject-positive-oracle")
            self.assertIn("positive Metal visibility", rows["visibility-positive-oracle"]["next_action"])
            self.assertEqual(rows["visibility-no-sample-hotpath"]["verdict"], "reject-hotpath")
            self.assertEqual(rows["pso-backend-isolation"]["verdict"], "reject-current")
            self.assertIn("PSO-isolated runs 0", rows["pso-backend-isolation"]["evidence"])
            self.assertEqual(rows["locality-semantic-ceiling"]["verdict"], "oracle-required")
            self.assertIn("sample-visible LRU32 -180840", rows["locality-semantic-ceiling"]["evidence"])
            self.assertEqual(rows["final-writer-replay-oracle"]["verdict"], "blocked-final-writer-hazard")
            self.assertIn("fail LRU32 -14593", rows["final-writer-replay-oracle"]["evidence"])
            self.assertIn("masked LRU32 -8389", rows["final-writer-replay-oracle"]["evidence"])
            self.assertEqual(rows["backend-escape-surface"]["verdict"], "reduced-ab-required")
            self.assertIn("mesh-object=bridge-only-reduced-ab-required", rows["backend-escape-surface"]["evidence"])
            self.assertEqual(rows["backend-escape-reduced-ab-plan"]["verdict"], "blocked-before-reduced-ab")
            self.assertIn("tile-ffp=blocked-hot-row-coverage", rows["backend-escape-reduced-ab-plan"]["evidence"])
            self.assertEqual(rows["final-color-runtime-selector"]["verdict"], "insufficient-final-color-selector")
            self.assertEqual(rows["final-writer-runtime-selector"]["verdict"], "overfit-only")
            self.assertIn("runtime.uniform_payload_hash=runtime-payload-overfit", rows["final-writer-runtime-selector"]["evidence"])
            self.assertEqual(rows["final-color-runtime-blocker"]["verdict"], "runtime-indistinguishable-blocker")
            self.assertIn(
                "current D3D9 occlusion query is primitive-count only",
                rows["broad-depth-read-reorder"]["next_action"],
            )
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

    def test_current_proof_suffixes_are_classified_as_locality_candidates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vs_scaling, vs_delta, semantic, primitive, selector, screen_blend = self.write_gate_inputs(
                root,
                opaque_run_name="post-streamib-frame60-opaque-proof-r1",
                screen_blend_run_name="post-streamib-frame60-screenblend-proof-r1",
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
            text = report.read_text(encoding="utf-8")
            self.assertIn("post-streamib-frame60-opaque-proof-r1 improves GPU", text)
            self.assertIn("post-streamib-frame60-screenblend-proof-r1 GPU", text)
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = {row["gate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["production-locality"]["verdict"], "keep")
            self.assertEqual(rows["screen-blend-explicit-tolerance"]["verdict"], "explicit-tolerance-pass")

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

    def test_missing_backend_gate_queues_class_proxy_smoke_target(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vs_scaling = root / "vs-scaling.csv"
            semantic = root / "semantic.csv"
            primitive = root / "primitive.csv"
            proxy = root / "proxy.csv"
            report = root / "gates.md"
            summary = root / "gates.csv"
            queue = root / "queue.csv"
            write_csv(vs_scaling, VS_FIELDS, [
                {
                    "run": "current-normal-frame60-gputrace-r1",
                    "top_row_keys": "60/2,60/1,60/0",
                    "gpu_ms": 33.0,
                    "vs_buffer_mib": 1600.0,
                    "vs_invocations": 1000.0,
                    "vs_b_per_vs_invocation": 1600.0,
                    "draw_calls": 100,
                    "dxmt_vertex_count": 2000,
                    "primitives": 700,
                },
            ])
            write_csv(semantic, SEMANTIC_FIELDS, [
                {
                    "rank": 1,
                    "verdict": "visible-fail",
                    "visible_exact_lru32_delta": -2452,
                    "sparse_exact_lru32_delta": -724,
                    "no_final_color_lru32_delta": -5937,
                    "visible_fail_lru32_delta": -14593,
                },
            ])
            write_csv(primitive, PRIMITIVE_FIELDS, [
                {"metric": "max_color_delta", "verdict": "exact-fail-only-positive"},
                {"metric": "max_abs_depth_delta", "verdict": "overlap"},
            ])
            write_csv(proxy, PROXY_FIELDS, [
                {
                    "group": "60/2|depth=read|blend=off|textured=yes|large4096=yes",
                    "proof_family": "semantic-proof-or-non-reorder",
                    "semantic_risk": "medium-depth-read-order-sensitive",
                    "xcode_proxy_hidden_backend_mib": "128.371",
                    "xcode_proxy_vs_write_mib": "128.371",
                    "xcode_proxy_gpu_ms": "2.574",
                    "miss32_delta": "0",
                    "candidate_miss32_delta": "-23502",
                },
                {
                    "group": "60/1|depth=write|blend=off|textured=no",
                    "proof_family": "production-opaque-reorder",
                    "semantic_risk": "low-opaque-depth-write",
                    "xcode_proxy_hidden_backend_mib": "290.800",
                    "xcode_proxy_vs_write_mib": "290.800",
                    "xcode_proxy_gpu_ms": "5.767",
                    "miss32_delta": "0",
                    "candidate_miss32_delta": "-65855",
                },
            ])

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
                    "--class-proxy-csv",
                    str(proxy),
                    "--output",
                    str(report),
                    "--csv-output",
                    str(summary),
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
            self.assertIn("`non-reorder-backend-shape` | `missing`", text)
            self.assertIn("`final-color-proof-gap` | `blocked-proof-gap`", text)
            self.assertIn("`final-color-occlusion-predicate` | `blocked-semantic-proof-gap`", text)
            self.assertIn("`non-reorder-backend-smoke-target` | `queued-from-class-proxy`", text)
            self.assertIn("60/2|depth=read|blend=off|textured=yes|large4096=yes", text)
            self.assertIn("primitive-order-preserving no-gputrace backend-shape smoke", text)
            self.assertIn("attach a selector sweep or final-color/final-writer proof", text)
            with summary.open(newline="", encoding="utf-8") as handle:
                summary_rows = {row["gate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(summary_rows["final-color-proof-gap"]["verdict"], "blocked-proof-gap")
            self.assertIn("visible-fail LRU32 -14593", summary_rows["final-color-proof-gap"]["evidence"])
            with queue.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            depth_read = [row for row in rows if row["group"].startswith("60/2|depth=read")]
            self.assertEqual(depth_read[0]["gate_status"], "blocked-final-color-oracle")
            self.assertIn("current D3D9 occlusion query is not enough", depth_read[0]["next_action"])

    def test_shader_variant_preflight_queues_only_scratch_moving_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vs_scaling, vs_delta, semantic, primitive, selector, screen_blend = self.write_gate_inputs(root)
            shader_variants = root / "shader-variants.csv"
            write_csv(shader_variants, SHADER_VARIANT_FIELDS, [
                {
                    "rank": 1,
                    "variant": "original",
                    "seq": 60,
                    "enc": 2,
                    "vs_buffer_write_mib": 981.159,
                    "source_file": "rank1.metal",
                    "source_vsout_bytes": 184,
                    "variant_vsout_bytes": 184,
                    "compile_ok": 1,
                    "ir_scratch_bytes_estimate": 128,
                },
                {
                    "rank": 1,
                    "variant": "live-vsout",
                    "seq": 60,
                    "enc": 2,
                    "vs_buffer_write_mib": 981.159,
                    "source_file": "rank1.metal",
                    "source_vsout_bytes": 184,
                    "variant_vsout_bytes": 36,
                    "compile_ok": 1,
                    "ir_scratch_bytes_estimate": 128,
                },
                {
                    "rank": 3,
                    "variant": "original",
                    "seq": 60,
                    "enc": 0,
                    "vs_buffer_write_mib": 224.947,
                    "source_file": "rank3.metal",
                    "source_vsout_bytes": 184,
                    "variant_vsout_bytes": 184,
                    "compile_ok": 1,
                    "ir_scratch_bytes_estimate": 128,
                },
                {
                    "rank": 3,
                    "variant": "live-vsout",
                    "seq": 60,
                    "enc": 0,
                    "vs_buffer_write_mib": 224.947,
                    "source_file": "rank3.metal",
                    "source_vsout_bytes": 184,
                    "variant_vsout_bytes": 52,
                    "compile_ok": 1,
                    "ir_scratch_bytes_estimate": 0,
                },
            ])
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
                    "--shader-variant-csv",
                    str(shader_variants),
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
            self.assertIn("`shader-variant-preflight` | `runtime-smoke-candidate`", text)
            self.assertIn("live-vsout scratch mover 60/0 rank3", text)
            self.assertIn("visible-only rows: 60/2 rank1", text)
            self.assertIn("`shader-variant-backend-smoke` | `queued-runtime-smoke`", text)
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = {row["gate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["shader-variant-preflight"]["verdict"], "runtime-smoke-candidate")

    def test_shader_variant_preflight_closes_after_matching_xcode_reject(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vs_scaling = root / "vs-scaling.csv"
            semantic = root / "semantic.csv"
            primitive = root / "primitive.csv"
            shader_variants = root / "shader-variants.csv"
            report = root / "gates.md"
            summary = root / "gates.csv"

            write_csv(vs_scaling, VS_FIELDS, [
                {
                    "run": "post-visualfix-frame60-baseline-r1",
                    "top_row_keys": "60/2,60/1,60/0",
                    "gpu_ms": 33.0,
                    "vs_buffer_mib": 1600.0,
                    "vs_invocations": 1000.0,
                    "vs_b_per_vs_invocation": 1600.0,
                    "draw_calls": 100,
                    "dxmt_vertex_count": 2000,
                    "primitives": 700,
                },
                {
                    "run": "trim-varyings-live-vsout-scoped-r1",
                    "top_row_keys": "60/2,60/1,60/0",
                    "gpu_ms": 33.4,
                    "vs_buffer_mib": 1600.1,
                    "vs_invocations": 1000.0,
                    "vs_b_per_vs_invocation": 1600.1,
                    "draw_calls": 100,
                    "dxmt_vertex_count": 2000,
                    "primitives": 700,
                },
            ])
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
            write_csv(shader_variants, SHADER_VARIANT_FIELDS, [
                {
                    "rank": 3,
                    "variant": "original",
                    "seq": 60,
                    "enc": 0,
                    "vs_buffer_write_mib": 224.947,
                    "source_file": "rank3.metal",
                    "source_vsout_bytes": 184,
                    "variant_vsout_bytes": 184,
                    "compile_ok": 1,
                    "ir_scratch_bytes_estimate": 128,
                },
                {
                    "rank": 3,
                    "variant": "live-vsout",
                    "seq": 60,
                    "enc": 0,
                    "vs_buffer_write_mib": 224.947,
                    "source_file": "rank3.metal",
                    "source_vsout_bytes": 184,
                    "variant_vsout_bytes": 52,
                    "compile_ok": 1,
                    "ir_scratch_bytes_estimate": 0,
                },
            ])

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
                    "--shader-variant-csv",
                    str(shader_variants),
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
            self.assertIn("`non-reorder-backend-shape` | `reject`", text)
            self.assertIn("best trim-varyings-live-vsout-scoped-r1", text)
            self.assertIn("`shader-variant-backend-smoke` | `closed-by-xcode-gate`", text)
            self.assertIn("look below visible VSOut", text)

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
            self.assertIn("mechanism-only until explicit exact/lsb1 proof is attached", text)
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = {row["gate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["screen-blend-explicit-tolerance"]["verdict"], "missing-semantic-image")
            self.assertIn(
                "mechanism-only until explicit exact/lsb1 proof is attached",
                rows["screen-blend-explicit-tolerance"]["next_action"],
            )
            self.assertEqual(rows["overall"]["verdict"], "semantic-safe-locality-only")

    def test_screen_blend_proxy_without_gate_inputs_reports_missing_gate_input(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vs_scaling, _vs_delta, semantic, primitive, _selector, _screen_blend = self.write_gate_inputs(
                root,
                include_screen_blend_run=False,
            )
            proxy = root / "class-proxy.csv"
            write_csv(proxy, PROXY_FIELDS, [
                {
                    "group": "60/2|depth=read|blend=screen|large4096=yes",
                    "proof_family": "explicit-tolerance-reorder",
                    "semantic_risk": "screen-blend-tolerance",
                    "xcode_proxy_hidden_backend_mib": 128.0,
                    "xcode_proxy_vs_write_mib": 130.0,
                    "xcode_proxy_gpu_ms": 2.5,
                    "miss32_delta": 0,
                    "candidate_miss32_delta": -23502,
                },
            ])
            report = root / "gates.md"
            summary = root / "gates.csv"
            queue = root / "gates-queue.csv"

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
                    "--class-proxy-csv",
                    str(proxy),
                    "--output",
                    str(report),
                    "--csv-output",
                    str(summary),
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
            self.assertIn("`screen-blend-explicit-tolerance` | `missing-screenblend-gate-input`", text)
            self.assertIn("no screen-blend movement candidate", text)
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = {row["gate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(
                rows["screen-blend-explicit-tolerance"]["verdict"],
                "missing-screenblend-gate-input",
            )
            self.assertIn("reattach screen-blend movement/semantic proof", rows["overall"]["next_action"])
            with queue.open(newline="", encoding="utf-8") as handle:
                queue_rows = list(csv.DictReader(handle))
            self.assertEqual(queue_rows[0]["gate_status"], "needs-screen-blend-gate-input")

    def test_production_proxy_without_gate_inputs_reports_missing_gate_input(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vs_scaling, _vs_delta, semantic, primitive, _selector, _screen_blend = self.write_gate_inputs(
                root,
                include_opaque_depth_run=False,
            )
            proxy = root / "class-proxy.csv"
            write_csv(proxy, PROXY_FIELDS, [
                {
                    "group": "60/1|depth=write|blend=off|large4096=no",
                    "proof_family": "production-opaque-reorder",
                    "semantic_risk": "low-opaque-depth-write",
                    "xcode_proxy_hidden_backend_mib": 290.8,
                    "xcode_proxy_vs_write_mib": 293.2,
                    "xcode_proxy_gpu_ms": 5.7,
                    "miss32_delta": 0,
                    "candidate_miss32_delta": -65855,
                },
            ])
            report = root / "gates.md"
            summary = root / "gates.csv"
            queue = root / "gates-queue.csv"

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
                    "--class-proxy-csv",
                    str(proxy),
                    "--output",
                    str(report),
                    "--csv-output",
                    str(summary),
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
            self.assertIn("`production-locality` | `missing-production-gate-input`", text)
            self.assertIn("`accepted-production-locality` | `missing-production-gate-input`", text)
            self.assertIn("no opaque-depth production proof run", text)
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = {row["gate"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["production-locality"]["verdict"], "missing-production-gate-input")
            self.assertIn("attach opaque-depth proof input", rows["overall"]["next_action"])
            with queue.open(newline="", encoding="utf-8") as handle:
                queue_rows = list(csv.DictReader(handle))
            self.assertEqual(queue_rows[0]["gate_status"], "needs-production-gate-input")

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
            self.assertIn("current D3D9 occlusion query is not enough", text)
            self.assertIn("current backend-shape family is rejected", text)
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

    def test_class_proxy_queue_uses_candidate_delta_for_no_mutate_proxy(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            vs_scaling, vs_delta, semantic, primitive, selector, screen_blend = self.write_gate_inputs(root)
            proxy = root / "class-proxy.csv"
            write_csv(proxy, PROXY_FIELDS, [
                {
                    "group": "60/2|depth=read|blend=screen|large4096=yes",
                    "proof_family": "explicit-tolerance-reorder",
                    "semantic_risk": "screen-blend-tolerance",
                    "xcode_proxy_hidden_backend_mib": 128.0,
                    "xcode_proxy_vs_write_mib": 130.0,
                    "xcode_proxy_gpu_ms": 2.5,
                    "miss32_delta": 0,
                    "candidate_miss32_delta": -23502,
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
            self.assertIn("| `60/2|depth=read|blend=screen|large4096=yes` |", report.read_text(encoding="utf-8"))
            self.assertIn("| `-23502` |", report.read_text(encoding="utf-8"))
            with queue.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[0]["miss32_delta"], "-23502")


if __name__ == "__main__":
    unittest.main()
