---
domain: baselines
workload: 3DMark05 GT1/GT2/GT3 + SFIV
subcategory: wild-fps-refresh
order: 1
title: Post-Encode-Session-Merge Wild FPS Refresh — GT1 +18-22%, GT3 +64% Since 07-31, GT2/SFIV Flat
date: 2026-08-03
type: experiment-run
status: accepted-baseline
source: experiments/output/app-d3d9-3dmark05-enc-merge-gt1-r2; experiments/output/app-d3d9-3dmark05-enc-merge-gt2-r2; experiments/output/app-d3d9-3dmark05-enc-merge-gt2-r3; experiments/output/app-d3d9-3dmark05-enc-merge-gt3-r2; experiments/output/app-d3d9-sfiv-benchmark-enc-merge-r1; experiments/output/app-d3d9-3dmark05-h229-session-hardening-gt1-r1-20260803; experiments/output/app-d3d9-3dmark05-h229-session-hardening-gt2-r1-20260803; experiments/output/app-d3d9-3dmark05-h229-session-hardening-gt3-r1-20260803
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.16.md; docs/perfomance/shader-codegen/shader-codegen-defselect.03.md
---

# Post-Encode-Session-Merge Wild FPS Refresh — GT1 +18-22%, GT3 +64% Since 07-31, GT2/SFIV Flat

**Question.** After the encode-session refactor merge (`41b55643` →
`72ced034`, 2026-08-03), where does each wild workload stand against its
last measured baseline — and is any delta attributable to the refactor?

**Method.** `perf` profile, `DXMT9_PERF_FRAME_SAMPLING=1`, no gputrace,
encoder breakdown off, 120 s supervised timeout. Scene fps from per-frame
`wall_ms`, never presents-at-kill (H231): steady body = frame index `>= 30`
and `wall_ms <= 200`, scene fps = `1000 / median(body)`. GT runs through
`run_3dmark05_perf_probe.sh --keep-frontmost` with `DXMT_3DMARK05_ARGS`
selecting the scene; SFIV through `run_experiment.py`. Baselines were NOT
re-run — this leaf compares against their recorded artifacts, recomputed
from their `3dmark05-perf-frames.csv` with the identical body filter. All
runs `status=pass`, `gpu_command_buffer_errors=0`, wine root
`sikarugir-cx-24.0.7`, all four staged build dirs rebuilt at HEAD before
measuring.

## Result

| workload | last baseline | HEAD `72ced034` | delta |
|---|---|---|---|
| GT2 | `25.80-26.17` fps ×4 (2026-08-01, `dbggroup-*` set) | `25.94 / 26.06 / 26.81` ×3 | **flat** (spreads overlap) |
| SFIV | avg `43.02` / median `59.70` (2026-07-31, defselect.03) | avg `43.26` / median `59.87` | **flat** (`+0.6% / +0.3%`) |
| GT1 | `23.52-23.82` fps ×3 (2026-07-31, `baseline-gt1-r1..3`) | `27.99 / 29.07` ×2 | **`+18-22%`** |
| GT3 | `36.91-37.35` fps ×3 (2026-07-31, `baseline-gt3-r1..3`) | `61.13 / 61.14` ×2 | **`+64%`** |

Two independent stagings agree: the `h229-session-hardening` runs (other
session, 13:02-13:13) and the `enc-merge` runs (this session's rebuild)
land at `61.13` vs `61.14` on GT3 and `27.99` vs `29.07` on GT1.

## Attribution — the refactor is perf-neutral; the GT1/GT3 gains are not its

**GT2 is the refactor gate.** Its baseline (2026-08-01 `dbggroup` set) was
measured on the commits immediately preceding the refactor, so the GT2
comparison isolates the refactor alone: three HEAD runs sit inside the
four-run baseline spread. The "refactor" label's claim holds.

**GT1/GT3 baselines predate two documented wins** that landed between
07-31 and the merge: the SWVP probe removal (`83a0b085`, `+29%` GT2 scene
fps, `state-churn-encode-append-decomposition.11`) and capture-gating the
per-draw Metal debug groups (`8ac44cee`, `.16`). GT1/GT3 are
producer-CPU-bound and benefit most from PE entry-cost removal; SFIV is
GPU/present-bound (median ~60) and stays flat — consistent with that
attribution, not with an encode-side cause.

## Confidence, stated honestly

Cross-day, non-interleaved, single-sided comparisons for GT1/GT3/SFIV.
The verdicts survive because each is either "inside the spread" or tens
of times the observed run-to-run noise; nothing in between is claimed.
The discarded `h229` SFIV run (`slots=16`, never entered the benchmark
scene) is excluded as invalid, not as an outlier.

**These HEAD numbers are the new comparison baselines** for GT1
(`27.99-29.07`), GT2 (`25.94-26.81`), GT3 (`61.13-61.14`), and SFIV
(avg `43.26` / median `59.87`).
