#!/usr/bin/env python3
"""Summarize current 3DMark05 perf optimization gates.

The report combines Xcode/dxmt VS-buffer scaling evidence with mini-replay
semantic evidence. It is intentionally conservative: a candidate can spend more
Xcode budget only if it already clears a cheap backend-shape or semantic gate.
"""

from __future__ import annotations

import argparse
import csv
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


def candidate_kind(run: str) -> str:
    text = run.lower()
    if "forceexpand" in text or "force-expand" in text:
        return "negative-geometry"
    if any(token in text for token in (
        "half-vsout",
        "texturewhite",
        "texture-white",
        "backend-shape",
    )):
        return "non-reorder-backend-shape"
    if (
        ("screenblend" in text or "screen-blend" in text)
        and ("index-cache" in text or "index_cache" in text or "cacheopt" in text)
    ):
        return "explicit-tolerance-locality"
    if "opaque-depth" in text:
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
            f"{len(candidates)} candidates, best GPU {best['gpu_delta_pct']:.2f}%, "
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
            "next_action": "require final-color/final-writer proof before promoting non-opaque primitive reorder",
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
        "no-final-color-exact-pass": "requires a runtime no-final-color/occlusion predicate before promotion",
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
        "next_action": "do not spend Xcode on broad non-opaque reorder; require final-color/final-writer proof",
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
        "next_action": "runtime-visible full payload identity is draw-local; require final-color/occlusion proof or avoid reorder",
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
        "next_action": "runtime state/geometry/shader selectors cannot split this hazard; require final-color/occlusion proof or a non-reorder mechanism",
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
            "next_action": "keep screen-blend cache as profiling-only until exact/lsb1 proof is attached",
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


def final_gate(gates: list[dict[str, str]]) -> dict[str, str]:
    by_name = {row["gate"]: row for row in gates}
    if by_name.get("non-reorder-backend-shape", {}).get("verdict") == "pass":
        verdict = "backend-shape-xcode-candidate"
        action = "spend Xcode budget on the passing non-reorder backend-shape candidate"
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
        action = "keep accepted opaque-depth locality and design final-color/final-writer proof for row 50/2"
    else:
        verdict = "needs-wider-proof"
        action = "collect wider semantic evidence before another production gputrace"
    return {
        "gate": "overall",
        "verdict": verdict,
        "evidence": "; ".join(f"{row['gate']}={row['verdict']}" for row in gates),
        "next_action": action,
    }


def implementation_track_rows(gates: list[dict[str, str]]) -> list[dict[str, str]]:
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
    blocker = by_name.get("final-color-runtime-blocker", {}).get("verdict", "")
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
        rows.append({
            "track": "final-color-occlusion-predicate",
            "status": "blocked-runtime-indistinguishable",
            "evidence": "runtime-visible fields cannot split the known visible final-color hazard; full uniform payload identity is draw-local overfit",
            "next_action": "do not implement a payload-identity selector; introduce a real final-color/occlusion signal or avoid reorder",
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

    if proof == "production-opaque-reorder":
        if production == "keep":
            return (
                "covered-production-path",
                "opaque-depth locality is already accepted; do not spend another trace on threshold-only retests",
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
        return (
            "needs-screen-blend-proof",
            "run the screen-blend movement and semantic-image gates together",
        )
    if proof in {"semantic-proof-or-non-reorder", "non-reorder-backend-shape-or-semantic-proof"}:
        if broad == "reject" or primitive == "final-color-oracle-required":
            return (
                "blocked-final-color-oracle",
                "primitive reorder needs final-color/final-writer proof; otherwise use a non-reorder backend-shape smoke",
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
        return (
            "blocked-final-color-oracle",
            "order-dependent material needs final-color/final-writer proof",
        )
    return (
        "manual-inspection",
        "inspect state/proof family and add a narrow preflight gate",
    )


def load_proxy_queue(path: Path, gates: list[dict[str, str]], top: int) -> list[dict[str, str]]:
    rows = load_csv(path)
    rows.sort(
        key=lambda row: (
            as_float(row.get("xcode_proxy_hidden_backend_mib")),
            as_float(row.get("xcode_proxy_vs_write_mib")),
            abs(as_float(row.get("miss32_delta"))),
        ),
        reverse=True,
    )
    queue: list[dict[str, str]] = []
    for row in rows[:top]:
        verdict, action = proxy_queue_verdict(row, gates)
        queue.append({
            "group": row.get("group", ""),
            "proof_family": row.get("proof_family", ""),
            "semantic_risk": row.get("semantic_risk", ""),
            "hidden_mib": f"{as_float(row.get('xcode_proxy_hidden_backend_mib')):.3f}",
            "gpu_ms": f"{as_float(row.get('xcode_proxy_gpu_ms')):.3f}",
            "miss32_delta": str(as_int(row.get("miss32_delta"))),
            "gate_status": verdict,
            "next_action": action,
        })
    return queue


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
    gates = [
        backend_shape_gate(deltas),
        locality_gate(deltas),
        semantic_gate(semantic_rows),
    ]
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
    gates.append(final_gate(gates))
    implementation_tracks = implementation_track_rows(gates)

    proxy_queues = [
        (path, load_proxy_queue(path, gates, args.class_proxy_top))
        for path in args.class_proxy_csv
    ]
    for index, path in enumerate(args.class_proxy_csv, start=1):
        inputs[f"Class proxy {index}"] = path

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
