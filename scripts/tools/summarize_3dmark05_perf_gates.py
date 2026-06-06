#!/usr/bin/env python3
"""Summarize current 3DMark05 perf optimization gates.

The report combines Xcode/dxmt VS-buffer scaling evidence with mini-replay
semantic evidence. It is intentionally conservative: a candidate can spend more
Xcode budget only if it already clears a cheap backend-shape or semantic gate.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any


def as_float(value: Any) -> float:
    try:
        text = str(value).strip()
        if not text:
            return 0.0
        return float(text)
    except (TypeError, ValueError):
        return 0.0


def as_pct(value: Any) -> float:
    text = str(value or "").strip().rstrip("%")
    return as_float(text)


def as_int(value: Any) -> int:
    try:
        return int(float(str(value).strip()))
    except (TypeError, ValueError):
        return 0


def pct_delta(value: float, baseline: float) -> float:
    return ((value - baseline) / baseline * 100.0) if baseline else 0.0


def load_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise SystemExit(f"empty CSV: {path}")
    return rows


def load_json(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise SystemExit(f"missing JSON: {path}")
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise SystemExit(f"expected JSON object: {path}")
    return data


def candidate_kind(run: str) -> str:
    text = run.lower()
    if "forceexpand" in text or "force-expand" in text:
        return "negative-geometry"
    if any(token in text for token in (
        "half-vsout",
        "live-vsout",
        "trim-varyings",
        "trim_unused_varyings",
        "trim-unused-varyings",
        "texturewhite",
        "texture-white",
        "backend-shape",
    )):
        return "non-reorder-backend-shape"
    if (
        ("screenblend" in text or "screen-blend" in text)
        and (
            "index-cache" in text
            or "index_cache" in text
            or "cacheopt" in text
            or "screenblend-proof" in text
            or "screen-blend-proof" in text
        )
    ):
        return "explicit-tolerance-locality"
    if (
        "opaque-depth" in text
        or "opaque_depth" in text
        or "opaque-proof" in text
    ):
        return "production-locality"
    if "cacheopt" in text or "index-cache" in text or "index_cache" in text:
        return "diagnostic-locality"
    return "unknown"


def find_baseline(rows: list[dict[str, str]], baseline_name: str | None) -> dict[str, str]:
    if baseline_name:
        exact = [row for row in rows if row.get("run") == baseline_name]
        if exact:
            return exact[0]
        contains = [row for row in rows if baseline_name in row.get("run", "")]
        if len(contains) == 1:
            return contains[0]
        raise SystemExit(f"could not identify baseline run: {baseline_name}")
    return rows[0]


def stable_geometry(row: dict[str, str], baseline: dict[str, str]) -> bool:
    return (
        row.get("top_row_keys", "") == baseline.get("top_row_keys", "")
        and abs(pct_delta(as_float(row.get("draw_calls")), as_float(baseline.get("draw_calls")))) <= 1.0
        and abs(pct_delta(as_float(row.get("dxmt_vertex_count")), as_float(baseline.get("dxmt_vertex_count")))) <= 5.0
        and abs(pct_delta(as_float(row.get("primitives")), as_float(baseline.get("primitives")))) <= 5.0
    )


def run_delta(row: dict[str, str], baseline: dict[str, str]) -> dict[str, Any]:
    gpu_delta = pct_delta(as_float(row.get("gpu_ms")), as_float(baseline.get("gpu_ms")))
    vs_delta = pct_delta(as_float(row.get("vs_buffer_mib")), as_float(baseline.get("vs_buffer_mib")))
    inv_delta = pct_delta(as_float(row.get("vs_invocations")), as_float(baseline.get("vs_invocations")))
    bpi_delta = pct_delta(
        as_float(row.get("vs_b_per_vs_invocation")),
        as_float(baseline.get("vs_b_per_vs_invocation")),
    )
    return {
        "run": row.get("run", ""),
        "kind": candidate_kind(row.get("run", "")),
        "geometry_stable": stable_geometry(row, baseline),
        "gpu_delta_pct": gpu_delta,
        "vs_delta_pct": vs_delta,
        "vs_invocations_delta_pct": inv_delta,
        "vs_b_per_inv_delta_pct": bpi_delta,
    }


def backend_shape_gate(deltas: list[dict[str, Any]]) -> dict[str, str]:
    candidates = [row for row in deltas if row["kind"] == "non-reorder-backend-shape"]
    passing = [
        row for row in candidates
        if row["geometry_stable"]
        and row["gpu_delta_pct"] <= -2.0
        and row["vs_b_per_inv_delta_pct"] <= -5.0
        and abs(row["vs_invocations_delta_pct"]) <= 2.0
    ]
    if passing:
        best = min(passing, key=lambda row: row["gpu_delta_pct"])
        return {
            "gate": "non-reorder-backend-shape",
            "verdict": "pass",
            "evidence": (
                f"{best['run']} clears GPU {best['gpu_delta_pct']:.2f}%, "
                f"VS B/inv {best['vs_b_per_inv_delta_pct']:.2f}%"
            ),
            "next_action": "spend gputrace/Xcode budget on the passing backend-shape candidate",
        }
    if not candidates:
        return {
            "gate": "non-reorder-backend-shape",
            "verdict": "missing",
            "evidence": "no non-reorder backend-shape candidates in VS scaling CSV",
            "next_action": "add a cheap no-gputrace preflight before Xcode",
        }
    best = min(candidates, key=lambda row: row["gpu_delta_pct"])
    return {
        "gate": "non-reorder-backend-shape",
        "verdict": "reject",
        "evidence": (
            f"{len(candidates)} candidates, best {best['run']} GPU {best['gpu_delta_pct']:.2f}%, "
            f"best-run VS B/inv {best['vs_b_per_inv_delta_pct']:.2f}%"
        ),
        "next_action": "do not spend another gputrace on this backend-shape family",
    }


def vs_write_attribution_gate(rows: list[dict[str, str]]) -> dict[str, str]:
    locality = [
        row for row in rows
        if row.get("candidate_kind") == "locality-reorder"
        and row.get("primary_mover") == "invocations"
        and as_float(row.get("vs_write_delta_mib")) < 0.0
        and as_float(row.get("gpu_delta_pct")) <= -2.0
    ]
    backend = [
        row for row in rows
        if row.get("candidate_kind") == "non-reorder-backend-shape"
    ]
    backend_pass = [
        row for row in backend
        if row.get("backend_shape_gate") == "pass"
        and row.get("primary_mover") == "bytes_per_invocation"
    ]
    if backend_pass:
        best = min(backend_pass, key=lambda row: as_float(row.get("gpu_delta_pct")))
        return {
            "gate": "vs-write-attribution",
            "verdict": "backend-bytes-candidate",
            "evidence": (
                f"{best.get('run', '')} primary={best.get('primary_mover', '')}, "
                f"bytes/inv effect {as_float(best.get('bytes_per_invocation_effect_mib')):.3f}MiB, "
                f"GPU {as_float(best.get('gpu_delta_pct')):.2f}%"
            ),
            "next_action": "spend Xcode only after the backend bytes/inv candidate also passes semantic/shape guards",
        }
    if backend:
        best_backend = min(
            backend,
            key=lambda row: (
                row.get("backend_shape_gate") == "reject",
                as_float(row.get("gpu_delta_pct")),
            ),
        )
        backend_reason = (
            f"best backend {best_backend.get('run', '')} primary={best_backend.get('primary_mover', '')}, "
            f"bytes/inv effect {as_float(best_backend.get('bytes_per_invocation_effect_mib')):.3f}MiB, "
            f"gate={best_backend.get('backend_shape_gate', '')}"
        )
        if locality:
            best_locality = min(locality, key=lambda row: as_float(row.get("gpu_delta_pct")))
            return {
                "gate": "vs-write-attribution",
                "verdict": "locality-dominant-backend-rejected",
                "evidence": (
                    f"{len(locality)} locality win(s) are invocation-driven; "
                    f"best {best_locality.get('run', '')} invocation effect "
                    f"{as_float(best_locality.get('invocation_effect_mib')):.3f}MiB; "
                    f"{backend_reason}"
                ),
                "next_action": "do not spend Xcode on another backend-shape candidate until bytes/inv preflight clears the backend gate",
            }
        return {
            "gate": "vs-write-attribution",
            "verdict": "backend-rejected",
            "evidence": backend_reason,
            "next_action": "require a stronger bytes/inv preflight before another non-reorder backend gputrace",
        }
    if locality:
        best_locality = min(locality, key=lambda row: as_float(row.get("gpu_delta_pct")))
        return {
            "gate": "vs-write-attribution",
            "verdict": "locality-only",
            "evidence": (
                f"{len(locality)} locality win(s), best {best_locality.get('run', '')} "
                f"primary={best_locality.get('primary_mover', '')}"
            ),
            "next_action": "attribute current wins to locality; add backend candidates before spending backend-shape Xcode budget",
        }
    return {
        "gate": "vs-write-attribution",
        "verdict": "missing-attribution-candidate",
        "evidence": "no locality or non-reorder backend rows in VS delta CSV",
        "next_action": "regenerate analyze_vs_buffer_scaling.py --delta-output with candidate runs",
    }


def locality_gate(deltas: list[dict[str, Any]]) -> dict[str, str]:
    production = [
        row for row in deltas
        if row["kind"] == "production-locality"
        and row["geometry_stable"]
        and row["gpu_delta_pct"] <= -2.0
        and row["vs_invocations_delta_pct"] <= -2.0
    ]
    if production:
        best = min(production, key=lambda row: row["gpu_delta_pct"])
        return {
            "gate": "production-locality",
            "verdict": "keep",
            "evidence": (
                f"{best['run']} improves GPU {best['gpu_delta_pct']:.2f}% and "
                f"VS invocations {best['vs_invocations_delta_pct']:.2f}%"
            ),
            "next_action": "keep production opaque-depth locality; seek a safe selector for remaining non-opaque rows",
        }
    return {
        "gate": "production-locality",
        "verdict": "no-pass",
        "evidence": "no production-shaped locality candidate clears the GPU/invocation gate",
        "next_action": "do not promote locality without a semantic proof",
    }


def semantic_gate(rows: list[dict[str, str]]) -> dict[str, str]:
    visible_fail = sum(as_int(row.get("visible_fail_lru32_delta")) for row in rows)
    sparse_exact = sum(as_int(row.get("sparse_exact_lru32_delta")) for row in rows)
    no_color = sum(as_int(row.get("no_final_color_lru32_delta")) for row in rows)
    visible_exact = sum(as_int(row.get("visible_exact_lru32_delta")) for row in rows)
    has_visible_fail = any(row.get("verdict") == "visible-fail" for row in rows) or visible_fail != 0
    if has_visible_fail:
        return {
            "gate": "broad-depth-read-reorder",
            "verdict": "reject",
            "evidence": (
                f"visible-fail LRU32 {visible_fail}; exact visible {visible_exact}; "
                f"sparse/no-final-color {sparse_exact + no_color}"
            ),
            "next_action": "require final-color/final-writer proof before promoting non-opaque primitive reorder; current D3D9 occlusion query is primitive-count only",
        }
    if visible_exact:
        return {
            "gate": "broad-depth-read-reorder",
            "verdict": "needs-selector",
            "evidence": f"visible exact-pass LRU32 {visible_exact} without visible failures",
            "next_action": "prove a runtime selector before Xcode promotion",
        }
    return {
        "gate": "broad-depth-read-reorder",
        "verdict": "sparse-only",
        "evidence": f"sparse/no-final-color LRU32 {sparse_exact + no_color}",
        "next_action": "treat as positive control, not a production optimization",
    }


def final_color_proof_gap_gate(rows: list[dict[str, str]]) -> dict[str, str]:
    visible_fail = sum(as_int(row.get("visible_fail_lru32_delta")) for row in rows)
    visible_exact = sum(as_int(row.get("visible_exact_lru32_delta")) for row in rows)
    sparse_exact = sum(as_int(row.get("sparse_exact_lru32_delta")) for row in rows)
    no_color = sum(as_int(row.get("no_final_color_lru32_delta")) for row in rows)
    sparse_fail = sum(as_int(row.get("sparse_fail_lru32_delta")) for row in rows)
    exact_or_hidden = visible_exact + sparse_exact + no_color

    if visible_fail != 0 and exact_or_hidden != 0:
        return {
            "gate": "final-color-proof-gap",
            "verdict": "blocked-proof-gap",
            "evidence": (
                f"visible-fail LRU32 {visible_fail}; visible exact {visible_exact}; "
                f"sparse/no-final-color {sparse_exact + no_color}"
            ),
            "next_action": (
                "keep depth-read primitive reorder out of production until a "
                "runtime-visible selector, final-color/final-writer oracle, "
                "or primitive-order-preserving backend mechanism exists"
            ),
        }
    if visible_fail != 0:
        return {
            "gate": "final-color-proof-gap",
            "verdict": "visible-fail-only",
            "evidence": f"visible-fail LRU32 {visible_fail}; sparse-fail LRU32 {sparse_fail}",
            "next_action": "reject this reorder family unless a narrower semantic-safe class is found",
        }
    if visible_exact != 0:
        return {
            "gate": "final-color-proof-gap",
            "verdict": "selector-needed",
            "evidence": f"visible exact-pass LRU32 {visible_exact}",
            "next_action": "attach a runtime selector sweep before scheduling Xcode for this reorder class",
        }
    return {
        "gate": "final-color-proof-gap",
        "verdict": "positive-control-only",
        "evidence": f"sparse/no-final-color LRU32 {sparse_exact + no_color}; sparse-fail LRU32 {sparse_fail}",
        "next_action": "treat this as diagnostic evidence rather than production locality",
    }


def semantic_oracle_queue(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    totals = {
        "visible-exact-pass": sum(as_int(row.get("visible_exact_lru32_delta")) for row in rows),
        "visible-fail": sum(as_int(row.get("visible_fail_lru32_delta")) for row in rows),
        "sparse-exact-pass": sum(as_int(row.get("sparse_exact_lru32_delta")) for row in rows),
        "no-final-color-exact-pass": sum(as_int(row.get("no_final_color_lru32_delta")) for row in rows),
        "sparse-fail": sum(as_int(row.get("sparse_fail_lru32_delta")) for row in rows),
    }
    order = [
        "visible-exact-pass",
        "visible-fail",
        "sparse-exact-pass",
        "no-final-color-exact-pass",
        "sparse-fail",
    ]
    status = {
        "visible-exact-pass": "candidate-final-color-selector",
        "visible-fail": "blocks-broad-reorder",
        "sparse-exact-pass": "sparse-positive-control",
        "no-final-color-exact-pass": "no-final-color-positive-control",
        "sparse-fail": "exact-fail",
    }
    action = {
        "visible-exact-pass": "validate a runtime final-color/final-writer selector before promoting this gain",
        "visible-fail": "exclude this visible final-writer hazard or reject broad reorder",
        "sparse-exact-pass": "keep as exact positive control; coverage is too sparse for production promotion",
        "no-final-color-exact-pass": "requires a new runtime no-final-color/Metal visibility predicate before promotion; current D3D9 occlusion query is primitive-count only",
        "sparse-fail": "reject any sparse selector that still admits exact replay failures",
    }
    return [
        {
            "bucket": bucket,
            "oracle_status": status[bucket],
            "lru32_delta": str(totals[bucket]),
            "next_action": action[bucket],
        }
        for bucket in order
        if totals[bucket] != 0
    ]


def parse_mixed_all_fail(value: str) -> tuple[int, int]:
    left, sep, right = str(value or "").partition("/")
    if not sep:
        return as_int(left), 0
    return as_int(left), as_int(right)


def semantic_selector_gate(rows: list[dict[str, str]], min_gain_pct: float = 50.0) -> dict[str, str]:
    runtime_rows = [
        row for row in rows
        if row.get("queue") == "runtime-field-combination"
        and row.get("verdict") in {"runtime-scout", "geometry-scout"}
    ]
    if not runtime_rows:
        return {
            "gate": "runtime-selector-scout",
            "verdict": "missing",
            "evidence": "no runtime-shaped or geometry selector rows in semantic selector sweep",
            "next_action": "keep non-opaque reorder disabled; generate selector sweep before Xcode",
        }

    runtime_rows.sort(
        key=lambda row: (
            as_pct(row.get("gain_share")),
            abs(as_int(row.get("lru32_delta"))),
        ),
        reverse=True,
    )
    best = runtime_rows[0]
    gain = as_pct(best.get("gain_share"))
    mixed, all_fail = parse_mixed_all_fail(best.get("mixed_all_fail_groups", ""))
    evidence = (
        f"best {best.get('selector', '')} keeps {gain:.2f}% "
        f"LRU32 {as_int(best.get('lru32_delta'))}; mixed/all-fail {mixed}/{all_fail}"
    )
    if gain >= min_gain_pct and mixed == 0 and all_fail == 0:
        return {
            "gate": "runtime-selector-scout",
            "verdict": "candidate-runtime-selector",
            "evidence": evidence,
            "next_action": "validate this runtime-shaped selector on wider same-input replay before Xcode promotion",
        }
    if gain >= min_gain_pct:
        return {
            "gate": "runtime-selector-scout",
            "verdict": "partial-runtime-selector",
            "evidence": evidence,
            "next_action": "refine the selector until mixed/all-fail groups disappear before Xcode",
        }
    return {
        "gate": "runtime-selector-scout",
        "verdict": "insufficient-runtime-selector",
        "evidence": evidence,
        "next_action": "do not spend Xcode on broad non-opaque reorder; require final-color/final-writer proof or Metal-visibility-backed no-sample proof",
    }


def final_color_runtime_selector_gate(rows: list[dict[str, str]], min_gain_pct: float = 80.0) -> dict[str, str]:
    selector_rows = [
        row for row in rows
        if row.get("queue") == "final-color-runtime-selector"
        and row.get("verdict") in {"runtime-scout", "geometry-scout"}
    ]
    if not selector_rows:
        return {
            "gate": "final-color-runtime-selector",
            "verdict": "missing",
            "evidence": "no runtime-shaped final-color selector rows in semantic selector sweep",
            "next_action": "do not spend Xcode on broad depth-read reorder; keep searching for a final-writer predicate",
        }

    selector_rows.sort(
        key=lambda row: (
            as_pct(row.get("gain_share")),
            abs(as_int(row.get("lru32_delta"))),
        ),
        reverse=True,
    )
    best = selector_rows[0]
    gain = as_pct(best.get("gain_share"))
    blocked, all_fail = parse_mixed_all_fail(best.get("mixed_all_fail_groups", ""))
    evidence = (
        f"best {best.get('selector', '')} keeps visible gain {gain:.2f}% "
        f"LRU32 {as_int(best.get('lru32_delta'))}; blocked/all-fail groups {blocked}/{all_fail}"
    )
    if gain >= min_gain_pct and blocked == 0:
        return {
            "gate": "final-color-runtime-selector",
            "verdict": "candidate-final-color-selector",
            "evidence": evidence,
            "next_action": "validate this final-color selector on wider same-input replay before Xcode promotion",
        }
    if gain >= 50.0:
        return {
            "gate": "final-color-runtime-selector",
            "verdict": "partial-final-color-selector",
            "evidence": evidence,
            "next_action": "refine until the selector keeps most visible exact gain without blocked target groups",
        }
    return {
        "gate": "final-color-runtime-selector",
        "verdict": "insufficient-final-color-selector",
        "evidence": evidence,
        "next_action": "do not spend Xcode on broad depth-read reorder; current runtime fields do not isolate visible exact gain",
    }


def final_writer_runtime_selector_gate(rows: list[dict[str, str]], min_gain_pct: float = 80.0) -> dict[str, str]:
    all_rows = [
        row for row in rows
        if row.get("queue") == "final-writer-runtime-selector"
    ]
    selector_rows = [
        row for row in all_rows
        if row.get("verdict") in {"runtime-scout", "geometry-scout", "runtime-constant-scout"}
    ]
    overfit_rows = [
        row for row in all_rows
        if row.get("verdict") in {
            "trace-local-constant",
            "trace-local-payload",
            "runtime-payload-overfit",
            "overfit-singleton",
        }
    ]
    if not all_rows:
        return {
            "gate": "final-writer-runtime-selector",
            "verdict": "missing",
            "evidence": "no final-writer runtime selector rows in semantic selector sweep",
            "next_action": "do not spend Xcode on broad depth-read reorder; keep searching for a final-writer predicate",
        }

    if selector_rows:
        selector_rows.sort(
            key=lambda row: (
                as_pct(row.get("gain_share")),
                abs(as_int(row.get("lru32_delta"))),
            ),
            reverse=True,
        )
        best = selector_rows[0]
        gain = as_pct(best.get("gain_share"))
        blocked, all_hazard = parse_mixed_all_fail(best.get("mixed_all_fail_groups", ""))
        evidence = (
            f"best {best.get('selector', '')} keeps owner-stable gain {gain:.2f}% "
            f"LRU32 {as_int(best.get('lru32_delta'))}; blocked/all-hazard groups {blocked}/{all_hazard}"
        )
        if gain >= min_gain_pct and blocked == 0:
            return {
                "gate": "final-writer-runtime-selector",
                "verdict": "candidate-final-writer-selector",
                "evidence": evidence,
                "next_action": "validate this final-writer selector on wider same-input replay before Xcode promotion",
            }
        if gain >= 50.0:
            return {
                "gate": "final-writer-runtime-selector",
                "verdict": "partial-final-writer-selector",
                "evidence": evidence,
                "next_action": "refine until the selector keeps most owner-stable movement without blocked hazard groups",
            }
        return {
            "gate": "final-writer-runtime-selector",
            "verdict": "insufficient-final-writer-selector",
            "evidence": evidence,
            "next_action": "do not spend Xcode on broad depth-read reorder; current runtime fields do not isolate final-writer safety",
        }

    overfit_rows.sort(
        key=lambda row: (
            as_pct(row.get("gain_share")),
            abs(as_int(row.get("lru32_delta"))),
        ),
        reverse=True,
    )
    best = overfit_rows[0]
    selectors = ", ".join(
        f"{row.get('selector', '')}={row.get('verdict', '')}"
        for row in overfit_rows[:3]
    )
    evidence = (
        f"only overfit/debug selectors found; best {best.get('selector', '')} keeps "
        f"owner-stable gain {as_pct(best.get('gain_share')):.2f}% "
        f"LRU32 {as_int(best.get('lru32_delta'))}; {selectors}"
    )
    return {
        "gate": "final-writer-runtime-selector",
        "verdict": "overfit-only",
        "evidence": evidence,
        "next_action": "runtime-visible full payload identity is draw-local; require final-color/final-writer or Metal-visibility-backed no-sample proof, not the current D3D9 query path",
    }


def final_color_runtime_blocker_gate(rows: list[dict[str, str]]) -> dict[str, str]:
    blocker_rows = [
        row for row in rows
        if row.get("queue") == "final-color-runtime-blocker"
        and row.get("verdict") == "runtime-indistinguishable-target-fail"
    ]
    if not blocker_rows:
        return {
            "gate": "final-color-runtime-blocker",
            "verdict": "none",
            "evidence": "no visible exact-pass/fail group is indistinguishable by all runtime-visible fields",
            "next_action": "selector search can continue with wider runtime fields if new manifest data exists",
        }
    blocker_rows.sort(
        key=lambda row: abs(as_int(row.get("lru32_delta"))),
        reverse=True,
    )
    best = blocker_rows[0]
    evidence = (
        f"{len(blocker_rows)} blocker group(s); largest visible LRU32 "
        f"{as_int(best.get('lru32_delta'))}; {best.get('mixed_all_fail_groups', '')}; "
        f"{best.get('meaning', '')}"
    )
    return {
        "gate": "final-color-runtime-blocker",
        "verdict": "runtime-indistinguishable-blocker",
        "evidence": evidence,
        "next_action": "runtime state/geometry/shader selectors cannot split this hazard; require final-color/final-writer or Metal-visibility-backed no-sample proof, or switch to a non-reorder mechanism",
    }


def read_full_image_row(path: Path) -> dict[str, str]:
    rows = load_csv(path)
    for row in rows:
        if row.get("area") == "full":
            return row
    return rows[0]


def screen_blend_semantic_pass(row: dict[str, str], policy: str) -> tuple[bool, str]:
    changed_pixels = as_int(row.get("changed_pixels"))
    changed_pct = as_float(row.get("changed_pct"))
    max_delta = as_int(row.get("max_delta"))
    mean_abs = as_float(row.get("mean_abs_delta"))
    rms = as_float(row.get("rms_delta"))
    ssim = as_float(row.get("ssim"))
    before_active = as_float(row.get("before_active_pct"))
    after_active = as_float(row.get("after_active_pct"))
    active_ok = before_active >= 1.0 and after_active >= 1.0
    if policy == "exact":
        ok = changed_pixels == 0 and active_ok
        return ok, f"exact changed={changed_pixels}, active={before_active:.2f}/{after_active:.2f}%"
    if policy == "lsb1":
        ok = (
            changed_pct <= 0.1
            and max_delta <= 1
            and mean_abs <= 0.001
            and rms <= 0.02
            and ssim >= 0.999999
            and active_ok
        )
        return (
            ok,
            (
                f"lsb1 changed={changed_pixels} ({changed_pct:.6f}%), "
                f"max_delta={max_delta}, ssim={ssim:.6f}"
            ),
        )
    return False, f"unsupported semantic image policy: {policy}"


def screen_blend_gate(
    deltas: list[dict[str, Any]],
    semantic_csv: Path | None,
    policy: str,
) -> dict[str, str]:
    candidates = [
        row for row in deltas
        if row["kind"] == "explicit-tolerance-locality"
        and row["geometry_stable"]
        and row["gpu_delta_pct"] <= -2.0
        and row["vs_invocations_delta_pct"] <= -2.0
    ]
    if semantic_csv is None:
        if not candidates:
            return {
                "gate": "screen-blend-explicit-tolerance",
                "verdict": "missing-xcode-movement",
                "evidence": "screen-blend locality candidates exist, but none clear geometry/GPU/invocation gates",
                "next_action": "attach a stable Xcode movement proof before spending semantic-image effort",
            }
        return {
            "gate": "screen-blend-explicit-tolerance",
            "verdict": "missing-semantic-image",
            "evidence": f"{len(candidates)} geometry-stable screen-blend candidate(s) clear GPU/invocation gates, but no semantic image CSV was provided",
            "next_action": "keep screen-blend cache as mechanism-only until explicit exact/lsb1 proof is attached",
        }
    semantic_ok, semantic_reason = screen_blend_semantic_pass(
        read_full_image_row(semantic_csv),
        policy,
    )
    if candidates and semantic_ok:
        best = min(candidates, key=lambda row: row["gpu_delta_pct"])
        return {
            "gate": "screen-blend-explicit-tolerance",
            "verdict": "explicit-tolerance-pass",
            "evidence": (
                f"{best['run']} GPU {best['gpu_delta_pct']:.2f}%, "
                f"VS invocations {best['vs_invocations_delta_pct']:.2f}%; "
                f"{semantic_reason}"
            ),
            "next_action": "allow only as explicit-tolerance opt-in; do not generalize to broad depth-read reorder",
        }
    if not semantic_ok:
        return {
            "gate": "screen-blend-explicit-tolerance",
            "verdict": "semantic-fail",
            "evidence": semantic_reason,
            "next_action": "reject screen-blend cache promotion for this proof run",
        }
    return {
        "gate": "screen-blend-explicit-tolerance",
        "verdict": "missing-xcode-movement",
        "evidence": f"{len(candidates)} geometry-stable screen-blend candidates clear GPU/invocation gates",
        "next_action": "attach a stable Xcode proof before considering screen-blend opt-in",
    }


def has_screen_blend_candidate(deltas: list[dict[str, Any]]) -> bool:
    return any(row["kind"] == "explicit-tolerance-locality" for row in deltas)


def has_production_locality_candidate(deltas: list[dict[str, Any]]) -> bool:
    return any(row["kind"] == "production-locality" for row in deltas)


def production_proxy_candidate_count(
    proxy_rows: list[tuple[Path, list[dict[str, str]]]],
) -> int:
    return sum(
        1
        for _path, rows in proxy_rows
        for row in rows
        if row.get("proof_family") == "production-opaque-reorder"
    )


def production_missing_gate_input(
    proxy_rows: list[tuple[Path, list[dict[str, str]]]],
) -> dict[str, str]:
    count = production_proxy_candidate_count(proxy_rows)
    return {
        "gate": "production-locality",
        "verdict": "missing-production-gate-input",
        "evidence": (
            f"{count} class-proxy opaque-depth production row(s), but the current "
            "VS scaling inputs expose no opaque-depth production proof run"
        ),
        "next_action": (
            "rerun the gate with the opaque-depth Xcode proof input or run "
            "--require-opaque-depth-index-cache-proof before promotion"
        ),
    }


def screen_blend_proxy_candidate_count(
    proxy_rows: list[tuple[Path, list[dict[str, str]]]],
) -> int:
    return sum(
        1
        for _path, rows in proxy_rows
        for row in rows
        if row.get("proof_family") == "explicit-tolerance-reorder"
    )


def screen_blend_missing_gate_input(
    proxy_rows: list[tuple[Path, list[dict[str, str]]]],
) -> dict[str, str]:
    count = screen_blend_proxy_candidate_count(proxy_rows)
    return {
        "gate": "screen-blend-explicit-tolerance",
        "verdict": "missing-screenblend-gate-input",
        "evidence": (
            f"{count} class-proxy screen-blend row(s), but the current VS scaling "
            "inputs expose no screen-blend movement candidate and no semantic image CSV was provided"
        ),
        "next_action": (
            "rerun the gate with screen-blend Xcode movement input and "
            "--screen-blend-semantic-csv before promotion"
        ),
    }


def primitive_selector_gate(rows: list[dict[str, str]]) -> dict[str, str]:
    color_positive = [
        row for row in rows
        if row.get("metric") in {"color_changed_pixels", "max_color_delta"}
        and row.get("verdict") != "overlap"
    ]
    non_color_positive = [
        row for row in rows
        if row.get("metric") not in {"color_changed_pixels", "max_color_delta"}
        and row.get("verdict") != "overlap"
    ]
    if color_positive and not non_color_positive:
        return {
            "gate": "primitive-conflict-selector",
            "verdict": "final-color-oracle-required",
            "evidence": "only color/final-output metrics separate exact pass/fail rows",
            "next_action": "do not use owner-count/depth/UV thresholds as a production selector",
        }
    if non_color_positive:
        return {
            "gate": "primitive-conflict-selector",
            "verdict": "candidate-runtime-selector",
            "evidence": "non-color metrics separate current pass/fail rows",
            "next_action": "validate the non-color selector on a wider replay set",
        }
    return {
        "gate": "primitive-conflict-selector",
        "verdict": "no-simple-selector",
        "evidence": "all primitive conflict metric ranges overlap",
        "next_action": "keep primitive reorder disabled for the class",
    }


def shader_variant_preflight_gate(rows: list[dict[str, str]]) -> dict[str, str]:
    live_rows = [
        row for row in rows
        if row.get("variant") == "live-vsout" and as_int(row.get("compile_ok")) == 1
    ]
    if not live_rows:
        return {
            "gate": "shader-variant-preflight",
            "verdict": "missing",
            "evidence": "no compiling live-vsout rows in shader variant CSV",
            "next_action": "run analyze_metal_shader_variants.py before another backend-shape Xcode spend",
        }

    original_by_key = {
        (row.get("rank", ""), row.get("source_file", "")): row
        for row in rows
        if row.get("variant") == "original"
    }

    def baseline_scratch_bytes(row: dict[str, str]) -> int:
        baseline_scratch = as_int(row.get("source_ir_scratch_bytes_estimate"))
        if baseline_scratch == 0:
            baseline_scratch = as_int(row.get("baseline_ir_scratch_bytes_estimate"))
        if baseline_scratch != 0:
            return baseline_scratch
        original = original_by_key.get((row.get("rank", ""), row.get("source_file", "")))
        if original is None:
            return 0
        return as_int(original.get("ir_scratch_bytes_estimate"))

    scratch_movers: list[dict[str, str]] = []
    visible_only: list[dict[str, str]] = []
    for row in live_rows:
        source_vsout = as_int(row.get("source_vsout_bytes"))
        variant_vsout = as_int(row.get("variant_vsout_bytes"))
        baseline_scratch = baseline_scratch_bytes(row)
        variant_scratch = as_int(row.get("ir_scratch_bytes_estimate"))
        vsout_shrunk = source_vsout > 0 and variant_vsout > 0 and variant_vsout < source_vsout
        scratch_shrunk = baseline_scratch > 0 and variant_scratch < baseline_scratch
        if vsout_shrunk and scratch_shrunk:
            scratch_movers.append(row)
        elif vsout_shrunk:
            visible_only.append(row)

    def row_label(row: dict[str, str]) -> str:
        return f"{row.get('seq', '')}/{row.get('enc', '')} rank{row.get('rank', '')}"

    if scratch_movers:
        scratch_movers.sort(key=lambda row: as_float(row.get("vs_buffer_write_mib")), reverse=True)
        best = scratch_movers[0]
        visible_labels = ", ".join(row_label(row) for row in visible_only[:3]) or "none"
        return {
            "gate": "shader-variant-preflight",
            "verdict": "runtime-smoke-candidate",
            "evidence": (
                f"live-vsout scratch mover {row_label(best)}: "
                f"VSOut {as_int(best.get('source_vsout_bytes'))}->{as_int(best.get('variant_vsout_bytes'))}B, "
                f"scratch {baseline_scratch_bytes(best)}->{as_int(best.get('ir_scratch_bytes_estimate'))}B; "
                f"visible-only rows: {visible_labels}"
            ),
            "next_action": "run a primitive-order-preserving no-gputrace/runtime smoke for the scratch-moving row before Xcode",
        }
    if visible_only:
        labels = ", ".join(row_label(row) for row in visible_only[:3])
        return {
            "gate": "shader-variant-preflight",
            "verdict": "visible-width-only",
            "evidence": f"live-vsout shrinks VSOut but not visible scratch for {labels}",
            "next_action": "do not spend Xcode on visible-width-only rows; look below AIR or at another backend denominator",
        }
    return {
        "gate": "shader-variant-preflight",
        "verdict": "no-structural-mover",
        "evidence": "live-vsout rows compile but do not shrink VSOut or visible scratch",
        "next_action": "reject this shader-variant family as a backend-shape preflight",
    }


def final_gate(gates: list[dict[str, str]]) -> dict[str, str]:
    by_name = {row["gate"]: row for row in gates}
    positive_visibility = by_name.get("visibility-positive-oracle", {}).get("verdict", "")
    locality_ceiling = by_name.get("locality-semantic-ceiling", {}).get("verdict", "")
    final_writer_replay = by_name.get("final-writer-replay-oracle", {}).get("verdict", "")
    backend_escape = by_name.get("backend-escape-surface", {}).get("verdict", "")
    backend_escape_plan = by_name.get("backend-escape-reduced-ab-plan", {}).get("verdict", "")
    if by_name.get("non-reorder-backend-shape", {}).get("verdict") == "pass":
        verdict = "backend-shape-xcode-candidate"
        action = "spend Xcode budget on the passing non-reorder backend-shape candidate"
    elif backend_escape_plan == "ready-reduced-ab":
        verdict = "backend-escape-reduced-ab-candidate"
        action = "run the reduced backend escape equality/counter A/B before GT1 Xcode promotion"
    elif backend_escape == "candidate-backend-escape":
        verdict = "backend-escape-reduced-ab-candidate"
        action = "run the reduced backend escape A/B and equality gate before GT1 Xcode promotion"
    elif final_writer_replay == "candidate-final-writer-safe":
        verdict = "semantic-oracle-replay-candidate"
        action = "validate the owner-safe replay set on wider same-input data before Xcode promotion"
    elif by_name.get("final-color-runtime-selector", {}).get("verdict") == "candidate-final-color-selector":
        verdict = "semantic-selector-replay-candidate"
        action = "validate the final-color selector on a wider same-input replay before Xcode promotion"
    elif by_name.get("final-writer-runtime-selector", {}).get("verdict") == "candidate-final-writer-selector":
        verdict = "semantic-selector-replay-candidate"
        action = "validate the final-writer selector on a wider same-input replay before Xcode promotion"
    elif by_name.get("screen-blend-explicit-tolerance", {}).get("verdict") == "explicit-tolerance-pass":
        verdict = "accepted-opaque-plus-explicit-screenblend"
        action = "keep opaque-depth locality and screen-blend only under explicit exact/lsb1 policy"
    elif by_name.get("broad-depth-read-reorder", {}).get("verdict") == "reject":
        verdict = "semantic-safe-locality-only"
        visibility = by_name.get("visibility-no-sample-hotpath", {}).get("verdict", "")
        production = by_name.get("production-locality", {}).get("verdict", "")
        screen = by_name.get("screen-blend-explicit-tolerance", {}).get("verdict", "")
        input_actions: list[str] = []
        if production == "missing-production-gate-input":
            input_actions.append("attach opaque-depth proof input before promoting frame60 opaque proxy rows")
        elif production == "keep":
            input_actions.append("keep accepted opaque-depth locality")
        if screen == "missing-screenblend-gate-input":
            input_actions.append("reattach screen-blend movement/semantic proof before screen-blend promotion")
        elif screen == "explicit-tolerance-pass":
            input_actions.append("keep screen-blend only under explicit exact/lsb1 policy")
        if backend_escape_plan == "blocked-before-reduced-ab" and final_writer_replay in {"blocked-final-writer-hazard", "blocked-owner-masked"}:
            tail = "current same-input real-texture semantic replay does not prove final-writer safety, and current backend escapes are blocked before reduced A/B route/coverage"
        elif backend_escape == "reduced-ab-required" and final_writer_replay in {"blocked-final-writer-hazard", "blocked-owner-masked"}:
            tail = "current same-input real-texture semantic replay does not prove final-writer safety, and current backend escapes require a reduced A/B or new route before GT1 Xcode"
        elif final_writer_replay in {"blocked-final-writer-hazard", "blocked-owner-masked"}:
            tail = "current same-input real-texture semantic replay does not prove final-writer safety; use a different oracle/replay set or a non-reorder backend mechanism"
        elif backend_escape_plan == "blocked-before-reduced-ab":
            tail = "current backend escapes are blocked before reduced A/B route/coverage"
        elif backend_escape == "reduced-ab-required":
            tail = "current backend escapes require a reduced A/B or new route before GT1 Xcode"
        elif locality_ceiling == "oracle-required":
            tail = "current color-exact/zero-sample locality is too small for Xcode; only a final-color/final-writer oracle that keeps enough sample-visible rows or a non-reorder backend mechanism justifies another capture"
        elif positive_visibility == "reject-positive-oracle":
            tail = "positive Metal visibility is not final-color proof, so use final-color/final-writer proof or a non-reorder backend mechanism"
        elif visibility == "reject-hotpath":
            tail = "no-sample rows are not the current hotpath, so use final-color/final-writer proof or a non-reorder backend mechanism"
        else:
            tail = "design final-color/final-writer or Metal-visibility-backed no-sample proof for depth-read rows"
        action = "; ".join([*input_actions, tail])
    else:
        verdict = "needs-wider-proof"
        action = "collect wider semantic evidence before another production gputrace"
    return {
        "gate": "overall",
        "verdict": verdict,
        "evidence": "; ".join(f"{row['gate']}={row['verdict']}" for row in gates),
        "next_action": action,
    }


def visibility_no_sample_hotpath_gate(rows: list[dict[str, str]]) -> dict[str, str]:
    total_draws = sum(as_int(row.get("draws")) for row in rows)
    zero_draws = sum(as_int(row.get("zero_draws")) for row in rows)
    total_primitives = sum(as_int(row.get("source_primitives")) for row in rows)
    zero_primitives = sum(as_int(row.get("zero_source_primitives")) for row in rows)
    total_delta = sum(as_int(row.get("miss32_delta")) for row in rows)
    zero_delta = sum(as_int(row.get("zero_miss32_delta")) for row in rows)

    if zero_draws == 0:
        return {
            "gate": "visibility-no-sample-hotpath",
            "verdict": "no-zero-sample",
            "evidence": f"{total_draws} visibility draw(s), 0 no-sample draws",
            "next_action": "do not spend more time on no-sample locality for this row; use final-color/final-writer or backend-shape proof",
        }

    if zero_primitives == 0 and zero_delta == 0:
        return {
            "gate": "visibility-no-sample-hotpath",
            "verdict": "insufficient-zero-breakdown",
            "evidence": "visibility summary lacks zero-only primitive/LRU32 columns",
            "next_action": "rerun summarize_visibility_scout.py with current schema before scheduling no-sample Xcode work",
        }

    primitive_share = (zero_primitives / total_primitives * 100.0) if total_primitives else 0.0
    total_gain = abs(total_delta)
    zero_gain = abs(zero_delta)
    gain_share = (zero_gain / total_gain * 100.0) if total_gain else 0.0
    evidence = (
        f"zero draws {zero_draws}/{total_draws}; zero primitives "
        f"{zero_primitives}/{total_primitives} ({primitive_share:.2f}%); "
        f"zero LRU32 delta {zero_delta}/{total_delta} ({gain_share:.2f}% of abs gain)"
    )
    if primitive_share < 10.0 and gain_share < 10.0:
        return {
            "gate": "visibility-no-sample-hotpath",
            "verdict": "reject-hotpath",
            "evidence": evidence,
            "next_action": "do not schedule Xcode for no-sample locality on this row; no-sample rows are not the current hotpath and the hot gain is sample-visible",
        }
    return {
        "gate": "visibility-no-sample-hotpath",
        "verdict": "candidate-no-sample-hotpath",
        "evidence": evidence,
        "next_action": "validate no-sample rows with semantic policy and then consider a scoped Xcode/gputrace run",
    }


def has_visibility_join(rows: list[dict[str, str]]) -> bool:
    return any(
        row.get("visibility_join_status")
        or as_int(row.get("visibility_draws")) != 0
        or as_int(row.get("visibility_positive_draws")) != 0
        for row in rows
    )


def visibility_positive_oracle_gate(rows: list[dict[str, str]]) -> dict[str, str]:
    joined = [
        row for row in rows
        if row.get("visibility_join_status")
        or as_int(row.get("visibility_draws")) != 0
    ]
    if not joined:
        return {
            "gate": "visibility-positive-oracle",
            "verdict": "missing-joined-visibility",
            "evidence": "semantic candidates do not contain joined Metal visibility rows",
            "next_action": "rerun summarize_semantic_payload_candidates.py with --visibility-csv before using visibility as an oracle",
        }

    positive = [
        row for row in joined
        if as_int(row.get("visibility_positive_draws")) > 0
    ]
    positive_samples = sum(as_int(row.get("visibility_visible_samples_sum")) for row in positive)
    if not positive:
        return {
            "gate": "visibility-positive-oracle",
            "verdict": "no-positive-samples",
            "evidence": f"{len(joined)} joined semantic row(s), 0 sample-positive rows",
            "next_action": "judge this path with the no-sample hotpath gate and semantic replay, not positive visibility",
        }

    def positive_lru32(verdicts: set[str], statuses: set[str]) -> int:
        return sum(
            as_int(row.get("lru32_delta"))
            for row in positive
            if row.get("verdict") in verdicts
            or row.get("visibility_join_status") in statuses
        )

    no_color_lru32 = positive_lru32(
        {"no-final-color-exact-pass"},
        {"sample-visible-final-color-empty"},
    )
    visible_fail_lru32 = positive_lru32(
        {"visible-fail"},
        {"sample-visible-visible-fail"},
    )
    sparse_fail_lru32 = positive_lru32(
        {"sparse-fail"},
        {"sample-visible-sparse-fail"},
    )
    visible_exact_lru32 = positive_lru32(
        {"visible-exact-pass"},
        {"sample-visible-visible-exact"},
    )
    sparse_exact_lru32 = positive_lru32(
        {"sparse-exact-pass"},
        {"sample-visible-sparse-exact"},
    )
    exact_lru32 = visible_exact_lru32 + sparse_exact_lru32
    fail_lru32 = visible_fail_lru32 + sparse_fail_lru32
    evidence = (
        f"{len(positive)} sample-positive semantic row(s), samples {positive_samples}; "
        f"no-final-color LRU32 {no_color_lru32}; exact LRU32 {exact_lru32}; "
        f"fail LRU32 {fail_lru32}"
    )
    if no_color_lru32 != 0 or (exact_lru32 != 0 and fail_lru32 != 0):
        return {
            "gate": "visibility-positive-oracle",
            "verdict": "reject-positive-oracle",
            "evidence": evidence,
            "next_action": "do not use positive Metal visibility as a production selector; require final-color/final-writer proof or avoid primitive reorder",
        }
    if fail_lru32 != 0:
        return {
            "gate": "visibility-positive-oracle",
            "verdict": "sample-positive-fail-only",
            "evidence": evidence,
            "next_action": "reject this visibility-positive reorder family unless a stricter final-writer selector excludes the fail rows",
        }
    return {
        "gate": "visibility-positive-oracle",
        "verdict": "positive-control-only",
        "evidence": evidence,
        "next_action": "positive visibility is only a control; validate final-color/final-writer safety on a wider replay set before Xcode",
    }


def pso_backend_isolation_gate(rows: list[dict[str, str]]) -> dict[str, str]:
    with_probe = [row for row in rows if as_int(row.get("probe_draws")) > 0]
    if not with_probe:
        return {
            "gate": "pso-backend-isolation",
            "verdict": "missing-per-draw-probe",
            "evidence": "PSO churn CSV lacks per-draw probe join columns",
            "next_action": "rerun analyze_pso_backend_churn.py with same-run indexed probe draws before using PSO churn as a backend-spill candidate",
        }

    isolated = [
        row for row in with_probe
        if as_int(row.get("probe_pso_isolated_run_count")) > 0
    ]
    if isolated:
        isolated.sort(
            key=lambda row: (
                as_int(row.get("probe_pso_isolated_run_count")),
                as_int(row.get("probe_pso_changes")),
                as_int(row.get("triangles")),
            ),
            reverse=True,
        )
        best = isolated[0]
        return {
            "gate": "pso-backend-isolation",
            "verdict": "candidate-isolated-pso",
            "evidence": (
                f"{len(isolated)} row(s) have PSO changes inside stream/IB-stable runs; "
                f"best {best.get('row', '')}: isolated runs "
                f"{as_int(best.get('probe_pso_isolated_run_count'))}, "
                f"PSO changes {as_int(best.get('probe_pso_changes'))}, "
                f"handle tuple changes {as_int(best.get('probe_handle_tuple_changes'))}"
            ),
            "next_action": "build a PSO-stable/PSO-churn A/B with geometry, stream/IB bindings, pass shape, and VS invocations held stable before Xcode",
        }

    pso_rows = [row for row in with_probe if as_int(row.get("probe_pso_changes")) > 0]
    if not pso_rows:
        return {
            "gate": "pso-backend-isolation",
            "verdict": "no-pso-motion",
            "evidence": f"{len(with_probe)} probed row(s), 0 per-draw PSO changes",
            "next_action": "do not spend Xcode on PSO/backend-spill for these rows",
        }

    top = max(
        pso_rows,
        key=lambda row: (
            as_int(row.get("probe_pso_changes")),
            as_int(row.get("triangles")),
        ),
    )
    total_pso = sum(as_int(row.get("probe_pso_changes")) for row in pso_rows)
    total_tuple = sum(as_int(row.get("probe_handle_tuple_changes")) for row in pso_rows)
    max_run = max(as_int(row.get("probe_handle_tuple_max_run")) for row in pso_rows)
    return {
        "gate": "pso-backend-isolation",
        "verdict": "reject-current",
        "evidence": (
            f"{len(pso_rows)} PSO-moving row(s), PSO changes {total_pso}, "
            f"handle tuple changes {total_tuple}, max stable tuple run {max_run}, "
            f"PSO-isolated runs 0; top {top.get('row', '')}: "
            f"PSO {as_int(top.get('probe_pso_changes'))}, "
            f"tuple {as_int(top.get('probe_handle_tuple_changes'))}"
        ),
        "next_action": "do not spend Xcode on current PSO churn; require a controlled PSO-only A/B or a different backend denominator mechanism",
    }


def find_bucket(rows: list[dict[str, str]], bucket: str) -> dict[str, str] | None:
    for row in rows:
        if row.get("bucket") == bucket:
            return row
    return None


def locality_semantic_ceiling_gate(rows: list[dict[str, str]]) -> dict[str, str]:
    color_exact = find_bucket(rows, "rank2_to_rank4_color_exact_owner_masked")
    positive = find_bucket(rows, "visibility_positive_sample_rows")
    zero = find_bucket(rows, "visibility_zero_sample_rows")
    all_scoped = find_bucket(rows, "rank1_to_rank4_all_scoped")

    if color_exact is None and positive is None:
        return {
            "gate": "locality-semantic-ceiling",
            "verdict": "missing-ceiling-buckets",
            "evidence": "semantic ceiling CSV lacks color-exact and visibility-positive aggregate buckets",
            "next_action": "regenerate the locality semantic ceiling CSV before scheduling another locality Xcode spend",
        }

    color_pct = as_float(color_exact.get("pct_of_flip_needed_lru32")) if color_exact else 0.0
    color_gpu = as_float(color_exact.get("est_gpu_ms_delta")) if color_exact else 0.0
    color_lru = as_int(color_exact.get("lru32_delta")) if color_exact else 0
    positive_pct = as_float(positive.get("pct_of_flip_needed_lru32")) if positive else 0.0
    positive_gpu = as_float(positive.get("est_gpu_ms_delta")) if positive else 0.0
    positive_lru = as_int(positive.get("lru32_delta")) if positive else 0
    zero_lru = as_int(zero.get("lru32_delta")) if zero else 0
    zero_pct = as_float(zero.get("pct_of_flip_needed_lru32")) if zero else 0.0
    all_lru = as_int(all_scoped.get("lru32_delta")) if all_scoped else 0
    all_pct = as_float(all_scoped.get("pct_of_flip_needed_lru32")) if all_scoped else 0.0
    evidence = (
        f"color-exact owner-masked LRU32 {color_lru}, GPU {color_gpu:.3f}ms, "
        f"flip {color_pct:.2f}%; zero-sample LRU32 {zero_lru}, flip {zero_pct:.2f}%; "
        f"all scoped LRU32 {all_lru}, flip {all_pct:.2f}%; "
        f"sample-visible LRU32 {positive_lru}, GPU {positive_gpu:.3f}ms, "
        f"flip {positive_pct:.2f}%"
    )
    if color_pct >= 100.0:
        return {
            "gate": "locality-semantic-ceiling",
            "verdict": "candidate-color-exact-ceiling",
            "evidence": evidence,
            "next_action": "validate whether the color-exact bucket is final-writer safe before any Xcode locality spend",
        }
    if positive_pct >= 100.0:
        return {
            "gate": "locality-semantic-ceiling",
            "verdict": "oracle-required",
            "evidence": evidence,
            "next_action": "do not spend locality Xcode on current color-exact/zero-sample buckets; only a final-color/final-writer oracle that keeps enough sample-visible rows can justify the next locality capture",
        }
    return {
        "gate": "locality-semantic-ceiling",
        "verdict": "not-enough-locality",
        "evidence": evidence,
        "next_action": "do not spend another locality Xcode capture; current semantic-safe and visibility buckets are too small",
    }


def semantic_replay_lru32(summary: dict[str, Any]) -> int:
    return as_int(
        summary.get("candidate_replay", {})
        .get("index_cache_estimate", {})
        .get("replay_lru32_miss_delta")
    )


def semantic_replay_color_pixels(summary: dict[str, Any]) -> int:
    return as_int(summary.get("color_compare", {}).get("changed_pixels"))


def semantic_replay_owner_pixels(summary: dict[str, Any]) -> int:
    return as_int(summary.get("owner_compare", {}).get("canonical_owner_changed_pixels"))


def semantic_replay_color_owner_pixels(summary: dict[str, Any]) -> int:
    return as_int(
        summary.get("owner_compare", {}).get("canonical_color_and_owner_changed_pixels")
    )


def final_writer_replay_oracle_gate(summaries: list[dict[str, Any]]) -> dict[str, str]:
    if not summaries:
        return {
            "gate": "final-writer-replay-oracle",
            "verdict": "missing-replay-summaries",
            "evidence": "no semantic-gate-summary.json files were provided",
            "next_action": "attach same-input real-texture semantic replay summaries before using sample-visible locality as an oracle",
        }

    fail = [
        item for item in summaries
        if item.get("verdict") == "fail-final-writer-hazard"
        or semantic_replay_color_owner_pixels(item) > 0
    ]
    masked = [
        item for item in summaries
        if item.get("verdict") == "masked-final-writer-hazard"
        or (
            semantic_replay_color_pixels(item) == 0
            and semantic_replay_owner_pixels(item) > 0
        )
    ]
    safe = [
        item for item in summaries
        if semantic_replay_color_pixels(item) == 0
        and semantic_replay_owner_pixels(item) == 0
    ]

    fail_lru = sum(semantic_replay_lru32(item) for item in fail)
    masked_lru = sum(semantic_replay_lru32(item) for item in masked)
    safe_lru = sum(semantic_replay_lru32(item) for item in safe)
    fail_color = sum(semantic_replay_color_pixels(item) for item in fail)
    fail_owner = sum(semantic_replay_owner_pixels(item) for item in fail)
    fail_color_owner = sum(semantic_replay_color_owner_pixels(item) for item in fail)
    masked_owner = sum(semantic_replay_owner_pixels(item) for item in masked)
    evidence = (
        f"{len(summaries)} replay summary(s); fail LRU32 {fail_lru}, "
        f"color pixels {fail_color}, owner pixels {fail_owner}, "
        f"color+owner pixels {fail_color_owner}; masked LRU32 {masked_lru}, "
        f"owner pixels {masked_owner}; owner-safe LRU32 {safe_lru}"
    )
    if fail:
        return {
            "gate": "final-writer-replay-oracle",
            "verdict": "blocked-final-writer-hazard",
            "evidence": evidence,
            "next_action": "do not spend Xcode on sample-visible locality from this replay set; at least one same-input real-texture replay changes final color and canonical final writer",
        }
    if masked:
        return {
            "gate": "final-writer-replay-oracle",
            "verdict": "blocked-owner-masked",
            "evidence": evidence,
            "next_action": "do not treat color-exact replay as an oracle; canonical final-writer movement is masked in the final image and still needs a stricter proof before Xcode",
        }
    if safe_lru != 0:
        return {
            "gate": "final-writer-replay-oracle",
            "verdict": "candidate-final-writer-safe",
            "evidence": evidence,
            "next_action": "validate the owner-safe replay set on a wider same-input capture, then compare its LRU32 budget against the locality semantic ceiling before Xcode",
        }
    return {
        "gate": "final-writer-replay-oracle",
        "verdict": "no-locality-movement",
        "evidence": evidence,
        "next_action": "the replay set is safe but too small to justify a locality Xcode capture",
    }


def backend_escape_surface_gate(rows: list[dict[str, str]]) -> dict[str, str]:
    if not rows:
        return {
            "gate": "backend-escape-surface",
            "verdict": "missing-surface-audit",
            "evidence": "no backend escape surface rows were provided",
            "next_action": "run audit_backend_escape_surface.py before treating mesh/object, position/binning, or Tile-FFP as an Xcode candidate",
        }

    candidate_verdicts = {"candidate-route-present", "candidate-coverage"}
    candidate_rows = [row for row in rows if row.get("verdict") in candidate_verdicts]
    evidence = "; ".join(
        f"{row.get('candidate', '')}={row.get('verdict', '')}"
        for row in rows
    )
    if candidate_rows:
        best = candidate_rows[0]
        return {
            "gate": "backend-escape-surface",
            "verdict": "candidate-backend-escape",
            "evidence": evidence,
            "next_action": (
                f"{best.get('candidate', '')} has enough route/coverage surface for a reduced equality "
                "and counter A/B; validate reduced replay before GT1 Xcode promotion"
            ),
        }
    return {
        "gate": "backend-escape-surface",
        "verdict": "reduced-ab-required",
        "evidence": evidence,
        "next_action": "do not spend GT1 Xcode on current backend escapes; build a reduced mesh/object A/B, a real position/binning route, or expand Tile-FFP hot-row coverage first",
    }


def backend_escape_reduced_ab_plan_gate(rows: list[dict[str, str]]) -> dict[str, str]:
    if not rows:
        return {
            "gate": "backend-escape-reduced-ab-plan",
            "verdict": "missing-reduced-ab-plan",
            "evidence": "no backend escape reduced A/B plan rows were provided",
            "next_action": "run plan_backend_escape_reduced_ab.py after audit_backend_escape_surface.py",
        }

    evidence_parts: list[str] = []
    for row in rows:
        part = f"{row.get('candidate', '')}={row.get('reduced_ab_status', '')}"
        expansion = row.get("expansion_status", "")
        if expansion:
            part += f"/{expansion}"
        evidence_parts.append(part)
    evidence = "; ".join(evidence_parts)
    ready = [
        row for row in rows
        if row.get("reduced_ab_status") == "ready-reduced-ab"
    ]
    if ready:
        labels = ", ".join(row.get("candidate", "") for row in ready[:3])
        return {
            "gate": "backend-escape-reduced-ab-plan",
            "verdict": "ready-reduced-ab",
            "evidence": evidence,
            "next_action": f"run reduced equality and counter gates for {labels} before GT1 Xcode promotion",
        }
    tile_programmable = any(
        row.get("candidate") == "tile-ffp"
        and row.get("expansion_status") == "needs-programmable-tile-route"
        for row in rows
    )
    next_action = (
        "do not spend GT1 Xcode from the backend escape lane until route/coverage, "
        "equality, and reduced counter gates clear"
    )
    if tile_programmable:
        next_action = (
            "do not treat current Tile-FFP widening as the GT1 route; define a "
            "programmable/textured tile or mesh route, then pass equality and reduced counters"
        )
    return {
        "gate": "backend-escape-reduced-ab-plan",
        "verdict": "blocked-before-reduced-ab",
        "evidence": evidence,
        "next_action": next_action,
    }


def backend_smoke_target(
    proxy_queues: list[tuple[Path, list[dict[str, str]]]] | None,
) -> dict[str, str] | None:
    if not proxy_queues:
        return None
    candidates: list[dict[str, str]] = []
    for source, queue in proxy_queues:
        for item in queue:
            proof = item.get("proof_family", "")
            status = item.get("gate_status", "")
            if (
                proof in {
                    "semantic-proof-or-non-reorder",
                    "non-reorder-backend-shape-or-semantic-proof",
                }
                or "non-reorder" in proof
                or status == "blocked-final-color-oracle"
            ):
                row = dict(item)
                row["source"] = str(source)
                candidates.append(row)
    if not candidates:
        return None
    candidates.sort(
        key=lambda row: (
            as_float(row.get("hidden_mib")),
            abs(as_int(row.get("miss32_delta"))),
            as_float(row.get("gpu_ms")),
        ),
        reverse=True,
    )
    return candidates[0]


def implementation_track_rows(
    gates: list[dict[str, str]],
    proxy_queues: list[tuple[Path, list[dict[str, str]]]] | None = None,
) -> list[dict[str, str]]:
    by_name = {row["gate"]: row for row in gates}
    rows: list[dict[str, str]] = []

    production = by_name.get("production-locality", {}).get("verdict", "")
    if production == "keep":
        rows.append({
            "track": "accepted-production-locality",
            "status": "keep",
            "evidence": by_name["production-locality"]["evidence"],
            "next_action": "keep opaque-depth index-locality reorder in the production path",
        })
    elif production == "missing-production-gate-input":
        rows.append({
            "track": "accepted-production-locality",
            "status": "missing-production-gate-input",
            "evidence": by_name["production-locality"]["evidence"],
            "next_action": by_name["production-locality"]["next_action"],
        })
    else:
        rows.append({
            "track": "accepted-production-locality",
            "status": "no-production-pass",
            "evidence": by_name.get("production-locality", {}).get("evidence", "production locality gate missing"),
            "next_action": "do not promote locality without the production gate",
        })

    screen = by_name.get("screen-blend-explicit-tolerance", {}).get("verdict", "")
    if screen == "explicit-tolerance-pass":
        rows.append({
            "track": "explicit-screenblend-locality",
            "status": "explicit-tolerance-only",
            "evidence": by_name["screen-blend-explicit-tolerance"]["evidence"],
            "next_action": "keep as an explicit exact/lsb1 policy path; do not generalize to broad depth-read reorder",
        })
    elif screen:
        rows.append({
            "track": "explicit-screenblend-locality",
            "status": screen,
            "evidence": by_name["screen-blend-explicit-tolerance"]["evidence"],
            "next_action": by_name["screen-blend-explicit-tolerance"]["next_action"],
        })

    final_color = by_name.get("final-color-runtime-selector", {}).get("verdict", "")
    final_writer = by_name.get("final-writer-runtime-selector", {}).get("verdict", "")
    final_writer_replay = by_name.get("final-writer-replay-oracle", {}).get("verdict", "")
    blocker = by_name.get("final-color-runtime-blocker", {}).get("verdict", "")
    positive_visibility = by_name.get("visibility-positive-oracle", {}).get("verdict", "")
    locality_ceiling = by_name.get("locality-semantic-ceiling", {}).get("verdict", "")
    if final_writer_replay:
        replay_gate = by_name["final-writer-replay-oracle"]
        if final_writer_replay == "candidate-final-writer-safe":
            rows.append({
                "track": "final-writer-replay-oracle",
                "status": "candidate-owner-safe-replay",
                "evidence": replay_gate["evidence"],
                "next_action": replay_gate["next_action"],
            })
        elif final_writer_replay in {"blocked-final-writer-hazard", "blocked-owner-masked"}:
            rows.append({
                "track": "final-writer-replay-oracle",
                "status": final_writer_replay,
                "evidence": replay_gate["evidence"],
                "next_action": replay_gate["next_action"],
            })
        else:
            rows.append({
                "track": "final-writer-replay-oracle",
                "status": final_writer_replay,
                "evidence": replay_gate["evidence"],
                "next_action": replay_gate["next_action"],
            })
    if final_color == "candidate-final-color-selector" or final_writer == "candidate-final-writer-selector":
        rows.append({
            "track": "final-color-occlusion-predicate",
            "status": "candidate-semantic-selector",
            "evidence": "; ".join(
                item for item in (
                    by_name.get("final-color-runtime-selector", {}).get("evidence", ""),
                    by_name.get("final-writer-runtime-selector", {}).get("evidence", ""),
                )
                if item
            ),
            "next_action": "validate the selector on wider same-input replay before any Xcode promotion",
        })
    elif blocker == "runtime-indistinguishable-blocker" and final_writer == "overfit-only":
        visibility = by_name.get("visibility-no-sample-hotpath", {}).get("verdict", "")
        if positive_visibility == "reject-positive-oracle":
            next_action = "do not implement a payload-identity selector; positive Metal visibility is not final-color proof, so add final-color/final-writer proof or avoid reorder"
        elif visibility == "reject-hotpath":
            next_action = "do not implement a payload-identity selector; no-sample rows are not the current hotpath, so add final-color/final-writer proof or avoid reorder"
        else:
            next_action = "do not implement a payload-identity selector; current D3D9 occlusion query is primitive-count only, so add a new final-color/final-writer or Metal visibility signal, or avoid reorder"
        rows.append({
            "track": "final-color-occlusion-predicate",
            "status": "blocked-runtime-indistinguishable",
            "evidence": "runtime-visible fields cannot split the known visible final-color hazard; full uniform payload identity is draw-local overfit",
            "next_action": next_action,
        })
    elif final_color or final_writer or blocker:
        rows.append({
            "track": "final-color-occlusion-predicate",
            "status": "needs-wider-proof",
            "evidence": "; ".join(
                item for item in (
                    f"final-color={final_color}" if final_color else "",
                    f"final-writer={final_writer}" if final_writer else "",
                    f"blocker={blocker}" if blocker else "",
                )
                if item
            ),
            "next_action": "extend runtime-visible semantic proof before promoting broad non-opaque reorder",
        })
    elif by_name.get("final-color-proof-gap", {}).get("verdict") == "blocked-proof-gap":
        visibility = by_name.get("visibility-no-sample-hotpath", {}).get("verdict", "")
        if locality_ceiling == "oracle-required":
            next_action = (
                "do not schedule another primitive-reorder Xcode run from this queue; "
                "current color-exact/zero-sample locality is too small, and sample-visible "
                "locality needs final-color/final-writer proof before it is worth a capture"
            )
        elif positive_visibility == "reject-positive-oracle":
            next_action = (
                "do not schedule another primitive-reorder Xcode run from this queue; "
                "positive Metal visibility is not final-color proof, so add final-color/final-writer "
                "proof or use a non-reorder backend mechanism"
            )
        elif visibility == "reject-hotpath":
            next_action = (
                "do not schedule another primitive-reorder Xcode run from this queue; "
                "no-sample rows are not the hotpath, so add final-color/final-writer "
                "proof or use a non-reorder backend mechanism"
            )
        else:
            next_action = (
                "attach a selector sweep or final-color/final-writer proof before "
                "spending Xcode on depth-read primitive reorder"
            )
        rows.append({
            "track": "final-color-occlusion-predicate",
            "status": "blocked-semantic-proof-gap",
            "evidence": by_name["final-color-proof-gap"]["evidence"],
            "next_action": next_action,
        })
    if locality_ceiling:
        ceiling = by_name["locality-semantic-ceiling"]
        rows.append({
            "track": "locality-semantic-ceiling",
            "status": ceiling["verdict"],
            "evidence": ceiling["evidence"],
            "next_action": ceiling["next_action"],
        })

    backend_escape_gate = by_name.get("backend-escape-surface")
    if backend_escape_gate is not None:
        verdict = backend_escape_gate.get("verdict", "")
        status = "reduced-ab-required" if verdict == "reduced-ab-required" else verdict
        if verdict == "candidate-backend-escape":
            status = "queued-reduced-ab"
        rows.append({
            "track": "backend-escape-surface",
            "status": status,
            "evidence": backend_escape_gate["evidence"],
            "next_action": backend_escape_gate["next_action"],
        })

    backend_escape_plan_gate = by_name.get("backend-escape-reduced-ab-plan")
    if backend_escape_plan_gate is not None:
        verdict = backend_escape_plan_gate.get("verdict", "")
        status = "queued-reduced-ab" if verdict == "ready-reduced-ab" else verdict
        rows.append({
            "track": "backend-escape-reduced-ab-plan",
            "status": status,
            "evidence": backend_escape_plan_gate["evidence"],
            "next_action": backend_escape_plan_gate["next_action"],
        })

    backend = by_name.get("non-reorder-backend-shape", {}).get("verdict", "")
    attribution = by_name.get("vs-write-attribution")
    if backend == "pass":
        rows.append({
            "track": "non-reorder-backend-mechanism",
            "status": "xcode-candidate",
            "evidence": "; ".join(
                item for item in (
                    by_name["non-reorder-backend-shape"]["evidence"],
                    attribution.get("evidence", "") if attribution else "",
                )
                if item
            ),
            "next_action": "spend Xcode budget on the passing primitive-order-preserving backend-shape candidate",
        })
    elif backend == "reject":
        rows.append({
            "track": "non-reorder-backend-mechanism",
            "status": "needs-new-mechanism",
            "evidence": "; ".join(
                item for item in (
                    by_name["non-reorder-backend-shape"]["evidence"],
                    attribution.get("evidence", "") if attribution else "",
                )
                if item
            ),
            "next_action": (
                attribution["next_action"] if attribution
                else "preflight must keep primitive order and move bytes/invocation or hidden backend proxy materially before Xcode"
            ),
        })
    elif backend:
        rows.append({
            "track": "non-reorder-backend-mechanism",
            "status": backend,
            "evidence": by_name["non-reorder-backend-shape"]["evidence"],
            "next_action": by_name["non-reorder-backend-shape"]["next_action"],
        })
        if backend == "missing":
            smoke = backend_smoke_target(proxy_queues)
            if smoke is not None:
                rows.append({
                    "track": "non-reorder-backend-smoke-target",
                    "status": "queued-from-class-proxy",
                    "evidence": (
                        f"{smoke.get('group', '')}; hidden {smoke.get('hidden_mib', '0')}MiB; "
                        f"LRU32 {smoke.get('miss32_delta', '0')}; source {smoke.get('source', '')}"
                    ),
                    "next_action": (
                        "run a primitive-order-preserving no-gputrace backend-shape smoke for this class; "
                        "only promote to Xcode if row shape stays stable and the candidate has a credible "
                        "bytes/invocation mechanism"
                    ),
                })

    shader_preflight = by_name.get("shader-variant-preflight", {}).get("verdict", "")
    backend_evidence = by_name.get("non-reorder-backend-shape", {}).get("evidence", "")
    closes_shader_variant = (
        backend == "reject"
        and any(
            token in backend_evidence.lower()
            for token in ("live-vsout", "trim-varyings", "trim_unused_varyings")
        )
    )
    if shader_preflight == "runtime-smoke-candidate" and closes_shader_variant:
        rows.append({
            "track": "shader-variant-backend-smoke",
            "status": "closed-by-xcode-gate",
            "evidence": "; ".join(
                item for item in (
                    by_name["shader-variant-preflight"]["evidence"],
                    backend_evidence,
                )
                if item
            ),
            "next_action": "do not queue another shader-output runtime smoke for this family; look below visible VSOut or use a semantic-safe invocation reducer",
        })
    elif shader_preflight == "runtime-smoke-candidate":
        rows.append({
            "track": "shader-variant-backend-smoke",
            "status": "queued-runtime-smoke",
            "evidence": by_name["shader-variant-preflight"]["evidence"],
            "next_action": by_name["shader-variant-preflight"]["next_action"],
        })
    elif shader_preflight:
        rows.append({
            "track": "shader-variant-backend-smoke",
            "status": shader_preflight,
            "evidence": by_name["shader-variant-preflight"]["evidence"],
            "next_action": by_name["shader-variant-preflight"]["next_action"],
        })

    pso_gate = by_name.get("pso-backend-isolation")
    if pso_gate is not None:
        pso_verdict = pso_gate.get("verdict", "")
        if pso_verdict == "candidate-isolated-pso":
            rows.append({
                "track": "pso-backend-spill",
                "status": "queued-isolated-ab",
                "evidence": pso_gate["evidence"],
                "next_action": pso_gate["next_action"],
            })
        elif pso_verdict == "reject-current":
            rows.append({
                "track": "pso-backend-spill",
                "status": "blocked-current-telemetry",
                "evidence": pso_gate["evidence"],
                "next_action": pso_gate["next_action"],
            })
        else:
            rows.append({
                "track": "pso-backend-spill",
                "status": pso_verdict,
                "evidence": pso_gate["evidence"],
                "next_action": pso_gate["next_action"],
            })
    return rows


def proxy_queue_verdict(row: dict[str, str], gates: list[dict[str, str]]) -> tuple[str, str]:
    by_name = {gate["gate"]: gate for gate in gates}
    proof = row.get("proof_family", "")
    risk = row.get("semantic_risk", "")
    screen = by_name.get("screen-blend-explicit-tolerance", {}).get("verdict", "")
    production = by_name.get("production-locality", {}).get("verdict", "")
    broad = by_name.get("broad-depth-read-reorder", {}).get("verdict", "")
    primitive = by_name.get("primitive-conflict-selector", {}).get("verdict", "")
    backend = by_name.get("non-reorder-backend-shape", {}).get("verdict", "")
    visibility = by_name.get("visibility-no-sample-hotpath", {}).get("verdict", "")
    positive_visibility = by_name.get("visibility-positive-oracle", {}).get("verdict", "")
    pso = by_name.get("pso-backend-isolation", {}).get("verdict", "")
    locality_ceiling = by_name.get("locality-semantic-ceiling", {}).get("verdict", "")
    final_writer_replay = by_name.get("final-writer-replay-oracle", {}).get("verdict", "")
    backend_escape = by_name.get("backend-escape-surface", {}).get("verdict", "")
    backend_escape_plan = by_name.get("backend-escape-reduced-ab-plan", {}).get("verdict", "")

    backend_reject_tail = "current backend-shape family is rejected"
    if pso == "reject-current":
        backend_reject_tail += " and current PSO per-draw motion is not isolated"
    if backend_escape_plan == "blocked-before-reduced-ab":
        backend_reject_tail += " and backend escape reduced A/B is blocked before route/coverage"
    if backend_escape == "reduced-ab-required":
        backend_reject_tail += " and current backend escape audit requires a reduced A/B or new route"

    if proof == "production-opaque-reorder":
        if production == "keep":
            return (
                "covered-production-path",
                "opaque-depth locality is already accepted; do not spend another trace on threshold-only retests",
            )
        if production == "missing-production-gate-input":
            return (
                "needs-production-gate-input",
                "attach opaque-depth Xcode proof input or rerun --require-opaque-depth-index-cache-proof before promotion",
            )
        return (
            "production-proof-candidate",
            "run the opaque-depth production proof gates before promotion",
        )
    if proof == "explicit-tolerance-reorder":
        if screen == "explicit-tolerance-pass":
            return (
                "explicit-tolerance-only",
                "keep as an opt-in/profiling ceiling unless exact/lsb1 policy is carried by the run",
            )
        if screen == "missing-semantic-image":
            return (
                "needs-semantic-image",
                "attach same-input exact or explicit lsb1 semantic image proof before promotion",
            )
        if screen == "missing-xcode-movement":
            return (
                "needs-xcode-movement",
                "prove stable Xcode GPU/VS-invocation movement before semantic-image work",
            )
        if screen == "missing-screenblend-gate-input":
            return (
                "needs-screen-blend-gate-input",
                "attach screen-blend movement and same-input exact/lsb1 semantic image inputs before promotion",
            )
        return (
            "needs-screen-blend-proof",
            "run the screen-blend movement and semantic-image gates together",
        )
    if proof in {"semantic-proof-or-non-reorder", "non-reorder-backend-shape-or-semantic-proof"}:
        if broad == "reject" or primitive == "final-color-oracle-required":
            if backend == "reject":
                if final_writer_replay in {"blocked-final-writer-hazard", "blocked-owner-masked"}:
                    return (
                        "blocked-final-writer-replay",
                        f"primitive reorder needs final-color/final-writer proof; current same-input real-texture replay reports {final_writer_replay}; {backend_reject_tail}, so define a new oracle or non-reorder mechanism before Xcode",
                    )
                if locality_ceiling == "oracle-required":
                    return (
                        "blocked-final-color-oracle",
                        f"primitive reorder needs final-color/final-writer proof; current color-exact/zero-sample locality is too small for Xcode and sample-visible rows need an oracle; {backend_reject_tail}, so define a new non-reorder mechanism before Xcode",
                    )
                if positive_visibility == "reject-positive-oracle":
                    return (
                        "blocked-final-color-oracle",
                        f"primitive reorder needs final-color/final-writer proof; current D3D9 occlusion query is not enough and positive Metal visibility is not enough; {backend_reject_tail}, so define a new non-reorder mechanism before Xcode",
                    )
                if visibility == "reject-hotpath":
                    return (
                        "blocked-final-color-oracle",
                        f"primitive reorder needs final-color/final-writer proof; current D3D9 occlusion query is not enough and no-sample rows are not the hotpath; {backend_reject_tail}, so define a new non-reorder mechanism before Xcode",
                    )
                return (
                    "blocked-final-color-oracle",
                    f"primitive reorder needs final-color/final-writer proof; current D3D9 occlusion query is not enough and {backend_reject_tail}, so define a new non-reorder mechanism before Xcode",
                )
            if final_writer_replay in {"blocked-final-writer-hazard", "blocked-owner-masked"}:
                return (
                    "blocked-final-writer-replay",
                    f"primitive reorder needs final-color/final-writer proof; current same-input real-texture replay reports {final_writer_replay}, so use a new oracle or non-reorder backend-shape smoke",
                )
            if locality_ceiling == "oracle-required":
                return (
                    "blocked-final-color-oracle",
                    "primitive reorder needs final-color/final-writer proof; current color-exact/zero-sample locality is too small for Xcode and sample-visible rows need an oracle, so otherwise use a non-reorder backend-shape smoke",
                )
            if positive_visibility == "reject-positive-oracle":
                return (
                    "blocked-final-color-oracle",
                    "primitive reorder needs final-color/final-writer proof; current D3D9 occlusion query is not enough and positive Metal visibility is not enough, so otherwise use a non-reorder backend-shape smoke",
                )
            if visibility == "reject-hotpath":
                return (
                    "blocked-final-color-oracle",
                    "primitive reorder needs final-color/final-writer proof; current D3D9 occlusion query is not enough and no-sample rows are not the hotpath, so otherwise use a non-reorder backend-shape smoke",
                )
            return (
                "blocked-final-color-oracle",
                "primitive reorder needs final-color/final-writer or Metal-visibility-backed no-sample proof; current D3D9 occlusion query is not enough, so otherwise use a non-reorder backend-shape smoke",
            )
        return (
            "semantic-proof-candidate",
            "collect wider same-input semantic evidence before Xcode replay",
        )
    if "non-reorder" in proof:
        if backend == "reject":
            return (
                "needs-new-backend-mechanism",
                "prior non-reorder backend-shape axes failed; require a new mechanism before Xcode",
            )
        return (
            "backend-shape-candidate",
            "run stable-shape smoke, then require TVB mechanism proof",
        )
    if risk.startswith("high-") and (broad == "reject" or primitive == "final-color-oracle-required"):
        if backend == "reject":
            if final_writer_replay in {"blocked-final-writer-hazard", "blocked-owner-masked"}:
                return (
                    "blocked-final-writer-replay",
                    f"order-dependent material needs final-color/final-writer proof; current same-input real-texture replay reports {final_writer_replay}; {backend_reject_tail}, so define a new oracle or non-reorder mechanism before Xcode",
                )
            if locality_ceiling == "oracle-required":
                return (
                    "blocked-final-color-oracle",
                    f"order-dependent material needs final-color/final-writer proof; current color-exact/zero-sample locality is too small for Xcode and sample-visible rows need an oracle; {backend_reject_tail}, so define a new non-reorder mechanism before Xcode",
                )
            if positive_visibility == "reject-positive-oracle":
                return (
                    "blocked-final-color-oracle",
                    f"order-dependent material needs final-color/final-writer proof; current D3D9 occlusion query is not enough and positive Metal visibility is not enough; {backend_reject_tail}, so define a new non-reorder mechanism before Xcode",
                )
            if visibility == "reject-hotpath":
                return (
                    "blocked-final-color-oracle",
                    f"order-dependent material needs final-color/final-writer proof; current D3D9 occlusion query is not enough and no-sample rows are not the hotpath; {backend_reject_tail}, so define a new non-reorder mechanism before Xcode",
                )
            return (
                "blocked-final-color-oracle",
                f"order-dependent material needs final-color/final-writer proof; current D3D9 occlusion query is not enough and {backend_reject_tail}, so define a new non-reorder mechanism before Xcode",
            )
        if final_writer_replay in {"blocked-final-writer-hazard", "blocked-owner-masked"}:
            return (
                "blocked-final-writer-replay",
                f"order-dependent material needs final-color/final-writer proof; current same-input real-texture replay reports {final_writer_replay}",
            )
        if locality_ceiling == "oracle-required":
            return (
                "blocked-final-color-oracle",
                "order-dependent material needs final-color/final-writer proof; current color-exact/zero-sample locality is too small for Xcode and sample-visible rows need an oracle",
            )
        if positive_visibility == "reject-positive-oracle":
            return (
                "blocked-final-color-oracle",
                "order-dependent material needs final-color/final-writer proof; current D3D9 occlusion query is not enough and positive Metal visibility is not enough",
            )
        if visibility == "reject-hotpath":
            return (
                "blocked-final-color-oracle",
                "order-dependent material needs final-color/final-writer proof; current D3D9 occlusion query is not enough and no-sample rows are not the hotpath",
            )
        return (
            "blocked-final-color-oracle",
            "order-dependent material needs final-color/final-writer or Metal-visibility-backed no-sample proof; current D3D9 occlusion query is not enough",
        )
    return (
        "manual-inspection",
        "inspect state/proof family and add a narrow preflight gate",
    )


def proxy_lru32_delta(row: dict[str, str]) -> int:
    candidate_delta_text = str(row.get("candidate_miss32_delta", "")).strip()
    if candidate_delta_text:
        return as_int(candidate_delta_text)
    return as_int(row.get("miss32_delta"))


def build_proxy_queue(rows: list[dict[str, str]], gates: list[dict[str, str]], top: int) -> list[dict[str, str]]:
    rows.sort(
        key=lambda row: (
            as_float(row.get("xcode_proxy_hidden_backend_mib")),
            as_float(row.get("xcode_proxy_vs_write_mib")),
            abs(proxy_lru32_delta(row)),
        ),
        reverse=True,
    )
    queue: list[dict[str, str]] = []
    for row in rows[:top]:
        verdict, action = proxy_queue_verdict(row, gates)
        lru32_delta = proxy_lru32_delta(row)
        queue.append({
            "group": row.get("group", ""),
            "proof_family": row.get("proof_family", ""),
            "semantic_risk": row.get("semantic_risk", ""),
            "hidden_mib": f"{as_float(row.get('xcode_proxy_hidden_backend_mib')):.3f}",
            "gpu_ms": f"{as_float(row.get('xcode_proxy_gpu_ms')):.3f}",
            "miss32_delta": str(lru32_delta),
            "gate_status": verdict,
            "next_action": action,
        })
    return queue


def load_proxy_queue(path: Path, gates: list[dict[str, str]], top: int) -> list[dict[str, str]]:
    return build_proxy_queue(load_csv(path), gates, top)


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = ("gate", "verdict", "evidence", "next_action")
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def write_queue_csv(path: Path, proxy_queues: list[tuple[Path, list[dict[str, str]]]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = (
        "source",
        "rank",
        "group",
        "proof_family",
        "semantic_risk",
        "hidden_mib",
        "gpu_ms",
        "miss32_delta",
        "gate_status",
        "next_action",
    )
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for source, queue in proxy_queues:
            for rank, item in enumerate(queue, start=1):
                writer.writerow({
                    "source": str(source),
                    "rank": str(rank),
                    **item,
                })


def write_semantic_queue_csv(path: Path, queue: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = ("bucket", "oracle_status", "lru32_delta", "next_action")
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(queue)


def write_markdown(
    path: Path,
    rows: list[dict[str, str]],
    inputs: dict[str, Path],
    implementation_tracks: list[dict[str, str]] | None = None,
    semantic_queue: list[dict[str, str]] | None = None,
    proxy_queues: list[tuple[Path, list[dict[str, str]]]] | None = None,
) -> None:
    lines = [
        "# 3DMark05 Perf Gate Summary",
        "",
        "## Inputs",
        "",
    ]
    for name, input_path in inputs.items():
        lines.append(f"- {name}: `{input_path}`")
    lines.extend([
        "",
        "## Gates",
        "",
        "| Gate | Verdict | Evidence | Next action |",
        "|---|---|---|---|",
    ])
    for row in rows:
        lines.append(
            f"| `{row['gate']}` | `{row['verdict']}` | "
            f"{row['evidence']} | {row['next_action']} |"
        )
    if implementation_tracks:
        lines.extend([
            "",
            "## Implementation Track Queue",
            "",
            "This queue turns the conservative gates above into implementation "
            "tracks. It separates accepted production paths from blocked proof "
            "families so overfit debug selectors are not mistaken for shippable "
            "runtime predicates.",
            "",
            "| Track | Status | Evidence | Next action |",
            "|---|---|---|---|",
        ])
        for item in implementation_tracks:
            lines.append(
                f"| `{item['track']}` | `{item['status']}` | "
                f"{item['evidence']} | {item['next_action']} |"
            )
    if semantic_queue:
        lines.extend([
            "",
            "## Semantic Final-Color Queue",
            "",
            "This queue is derived from the semantic candidate buckets, not from "
            "Xcode proxy size. It exposes which LRU32 movement is selector value "
            "and which movement is a correctness blocker.",
            "",
            "| Bucket | Oracle status | LRU32 delta | Next action |",
            "|---|---|---:|---|",
        ])
        for item in semantic_queue:
            lines.append(
                "| "
                f"`{item['bucket']}` | "
                f"`{item['oracle_status']}` | "
                f"`{item['lru32_delta']}` | "
                f"{item['next_action']} |"
            )
    if proxy_queues:
        lines.extend([
            "",
            "## Next Experiment Queue",
            "",
            "Class proxy rows are sorted by Xcode hidden-backend proxy size. "
            "The gate status combines the proxy's proof family with the gates "
            "above, so high proxy size alone does not schedule another Xcode run.",
        ])
        for queue_path, queue in proxy_queues:
            lines.extend([
                "",
                f"### `{queue_path}`",
                "",
                "| Group | Proof family | Semantic risk | Hidden MiB | GPU ms | LRU32 delta | Gate status | Next action |",
                "|---|---|---|---:|---:|---:|---|---|",
            ])
            for item in queue:
                lines.append(
                    "| "
                    f"`{item['group']}` | "
                    f"`{item['proof_family']}` | "
                    f"`{item['semantic_risk']}` | "
                    f"`{item['hidden_mib']}` | "
                    f"`{item['gpu_ms']}` | "
                    f"`{item['miss32_delta']}` | "
                    f"`{item['gate_status']}` | "
                    f"{item['next_action']} |"
                )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vs-scaling-csv", required=True, type=Path)
    parser.add_argument("--vs-delta-csv", type=Path)
    parser.add_argument("--semantic-candidates-csv", required=True, type=Path)
    parser.add_argument("--primitive-selector-csv", type=Path)
    parser.add_argument("--semantic-selector-csv", type=Path)
    parser.add_argument("--shader-variant-csv", type=Path)
    parser.add_argument("--visibility-summary-csv", type=Path)
    parser.add_argument("--pso-backend-churn-csv", type=Path)
    parser.add_argument("--locality-semantic-ceiling-csv", type=Path)
    parser.add_argument("--backend-escape-surface-csv", type=Path)
    parser.add_argument("--backend-escape-reduced-ab-plan-csv", type=Path)
    parser.add_argument(
        "--semantic-replay-summary-json",
        action="append",
        default=[],
        type=Path,
        help="Optional semantic-gate-summary.json files from run_3dmark05_semantic_replay_gate.py",
    )
    parser.add_argument("--screen-blend-semantic-csv", type=Path)
    parser.add_argument("--screen-blend-semantic-policy", choices=("exact", "lsb1"), default="exact")
    parser.add_argument(
        "--class-proxy-csv",
        action="append",
        default=[],
        type=Path,
        help="Optional analyze_indexed_probe_classes.py CSV to append a gate-aware next experiment queue",
    )
    parser.add_argument("--class-proxy-top", type=int, default=8)
    parser.add_argument("--baseline-run")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--csv-output", type=Path)
    parser.add_argument("--semantic-queue-csv-output", type=Path)
    parser.add_argument("--queue-csv-output", type=Path)
    args = parser.parse_args()

    vs_rows = load_csv(args.vs_scaling_csv)
    semantic_rows = load_csv(args.semantic_candidates_csv)
    semantic_queue = semantic_oracle_queue(semantic_rows)
    baseline = find_baseline(vs_rows, args.baseline_run)
    deltas = [
        run_delta(row, baseline)
        for row in vs_rows
        if row is not baseline
    ]
    raw_proxy_rows = [
        (path, load_csv(path))
        for path in args.class_proxy_csv
    ]
    gates = [
        backend_shape_gate(deltas),
        locality_gate(deltas),
        semantic_gate(semantic_rows),
        final_color_proof_gap_gate(semantic_rows),
    ]
    if has_visibility_join(semantic_rows):
        gates.append(visibility_positive_oracle_gate(semantic_rows))
    if (
        not has_production_locality_candidate(deltas)
        and production_proxy_candidate_count(raw_proxy_rows)
    ):
        gates[1] = production_missing_gate_input(raw_proxy_rows)
    inputs = {
        "VS scaling": args.vs_scaling_csv,
        "Semantic candidates": args.semantic_candidates_csv,
    }
    if args.vs_delta_csv is not None:
        gates.append(vs_write_attribution_gate(load_csv(args.vs_delta_csv)))
        inputs["VS delta attribution"] = args.vs_delta_csv
    if args.primitive_selector_csv is not None:
        gates.append(primitive_selector_gate(load_csv(args.primitive_selector_csv)))
        inputs["Primitive selector"] = args.primitive_selector_csv
    if args.shader_variant_csv is not None:
        gates.append(shader_variant_preflight_gate(load_csv(args.shader_variant_csv)))
        inputs["Shader variant preflight"] = args.shader_variant_csv
    if args.visibility_summary_csv is not None:
        gates.append(visibility_no_sample_hotpath_gate(load_csv(args.visibility_summary_csv)))
        inputs["Visibility scout summary"] = args.visibility_summary_csv
    if args.pso_backend_churn_csv is not None:
        gates.append(pso_backend_isolation_gate(load_csv(args.pso_backend_churn_csv)))
        inputs["PSO backend churn"] = args.pso_backend_churn_csv
    if args.locality_semantic_ceiling_csv is not None:
        gates.append(locality_semantic_ceiling_gate(load_csv(args.locality_semantic_ceiling_csv)))
        inputs["Locality semantic ceiling"] = args.locality_semantic_ceiling_csv
    if args.backend_escape_surface_csv is not None:
        gates.append(backend_escape_surface_gate(load_csv(args.backend_escape_surface_csv)))
        inputs["Backend escape surface"] = args.backend_escape_surface_csv
    if args.backend_escape_reduced_ab_plan_csv is not None:
        gates.append(backend_escape_reduced_ab_plan_gate(load_csv(args.backend_escape_reduced_ab_plan_csv)))
        inputs["Backend escape reduced A/B plan"] = args.backend_escape_reduced_ab_plan_csv
    if args.semantic_replay_summary_json:
        gates.append(final_writer_replay_oracle_gate([
            load_json(path) for path in args.semantic_replay_summary_json
        ]))
        for index, path in enumerate(args.semantic_replay_summary_json, start=1):
            inputs[f"Semantic replay summary {index}"] = path
    if args.semantic_selector_csv is not None:
        semantic_selector_rows = load_csv(args.semantic_selector_csv)
        gates.append(semantic_selector_gate(semantic_selector_rows))
        gates.append(final_color_runtime_selector_gate(semantic_selector_rows))
        gates.append(final_writer_runtime_selector_gate(semantic_selector_rows))
        gates.append(final_color_runtime_blocker_gate(semantic_selector_rows))
        inputs["Semantic selector sweep"] = args.semantic_selector_csv
    if args.screen_blend_semantic_csv is not None or has_screen_blend_candidate(deltas):
        gates.append(screen_blend_gate(
            deltas,
            args.screen_blend_semantic_csv,
            args.screen_blend_semantic_policy,
        ))
        if args.screen_blend_semantic_csv is not None:
            inputs[f"Screen-blend semantic ({args.screen_blend_semantic_policy})"] = args.screen_blend_semantic_csv
    elif screen_blend_proxy_candidate_count(raw_proxy_rows):
        gates.append(screen_blend_missing_gate_input(raw_proxy_rows))
    proxy_queues = [
        (path, build_proxy_queue(rows, gates, args.class_proxy_top))
        for path, rows in raw_proxy_rows
    ]
    for index, path in enumerate(args.class_proxy_csv, start=1):
        inputs[f"Class proxy {index}"] = path
    gates.append(final_gate(gates))
    implementation_tracks = implementation_track_rows(gates, proxy_queues)

    write_markdown(args.output, gates, inputs, implementation_tracks, semantic_queue, proxy_queues)
    if args.csv_output is not None:
        write_csv(args.csv_output, gates)
    if args.semantic_queue_csv_output is not None:
        write_semantic_queue_csv(args.semantic_queue_csv_output, semantic_queue)
    if args.queue_csv_output is not None:
        write_queue_csv(args.queue_csv_output, proxy_queues)
    print(args.output)
    if args.csv_output is not None:
        print(args.csv_output)
    if args.semantic_queue_csv_output is not None:
        print(args.semantic_queue_csv_output)
    if args.queue_csv_output is not None:
        print(args.queue_csv_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
