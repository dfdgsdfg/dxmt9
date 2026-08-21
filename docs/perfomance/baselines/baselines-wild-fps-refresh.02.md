---
domain: baselines
workload: 3DMark05 GT1/GT2/GT3 + SFIV
subcategory: wild-fps-refresh
order: 2
title: README FPS Refresh — Single-Run Sweep at 8ddfe5fa; GT1 30.1, GT2 28.2, GT3 64.4, SFIV 43.6
date: 2026-08-21
type: experiment-run
status: accepted-baseline
source: experiments/output/app-d3d9-3dmark05-final-gt1b; experiments/output/app-d3d9-3dmark05-final-gt2; experiments/output/app-d3d9-3dmark05-final-gt3; experiments/output/app-d3d9-sfiv-benchmark-final-sfiv; experiments/output/app-d3d9-3dmark05-final-gt1
related: docs/perfomance/baselines/baselines-wild-fps-refresh.01.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.25.md
---

# README FPS Refresh — Single-Run Sweep at `8ddfe5fa`

**Question.** Where does each wild workload stand at HEAD `8ddfe5fa`, for the
README performance table? This is a refresh, not an A/B: the intervening
accepted increments since the 2026-08-04 table (chunk-seal cadence promotion,
getter-cache + warm-epoch bridge harvest, T2a' mark-lock removal) each carry
their own paired evidence in `state-churn-encode`; this leaf only records the
current absolute numbers.

**Method.** One completed run per workload, back-to-back in one session
(user-directed single-run scope — deliberately weaker than the two-run
methodology of
[serial-partition A/B](baselines-serial-partition-ab.02.md); same-day ambient
variation on this host is about ±3%, so sub-3% deltas against this table are
not meaningful). `perf` profile, `DXMT9_PERF_FRAME_SAMPLING=1`, no gputrace,
encoder breakdown off, `--keep-frontmost`, 120 s supervised timeout (SFIV
180 s via `run_experiment.py`). Primary FPS is the positive-frame average
(`frames × 1000 / Σ wall_ms`) over the full scene; SFIV uses a 90-second
matched window after a 30-frame warm-up skip. Steady cross-check is
`1000 / median(wall_ms)` on the body (frame ≥ 30, `wall_ms` ≤ 200). Frame
lines extracted with the anchored `[dxmt9-perf-frame ` prefix. Wine root
`sikarugir-cx-24.0.7`. Uncommitted tree changes at measurement time touch
only docs and debug-assert layers (verified: zero non-comment/non-assert
lines in the `src/` diff), so release binaries are those of `8ddfe5fa`.

## Result

| workload | run | sampled avg | steady median | status / GPU errors |
|---|---|---:|---:|---|
| GT1 | `final-gt1b` | **30.10** | 31.08 | pass / 0 |
| GT2 | `final-gt2` | **28.17** | 30.70 | pass / 0 |
| GT3 | `final-gt3` | **64.35** | 69.38 | pass / 0 |
| SFIV | `final-sfiv` | **43.64** (90 s window) | 59.45 | pass / 0 |

Against the 2026-08-04 README table: GT1 `27.2 → 30.1` (+10.7%), GT2
`23.8 → 28.2` (+18.5%), GT3 `54.7 → 64.4` (+17.7%), SFIV `44.2 → 43.6`
(−1.4%, inside SFIV's known thermal spread — treated as flat).

**Discarded run.** `final-gt1` (kept under `source:` as provenance) produced a
complete frame stream at avg `30.47` but the Wine process exited `rc=1` after
the scene, so `status=fail` and the final counter flush was lost. The
immediate rerun `final-gt1b` passed at `30.10` — the two agree within 1.2%,
so the failure is an exit-path artifact, not a measurement problem; the
passing run is the one quoted.

**Limitations.** Single runs: no run-range column in the README this cycle,
and the ±3% ambient figure comes from this session's repeated same-build
GT2/GT3 window observations (GT2 27.2 daytime vs 28.7-29.0 evening; GT3 63 vs
67 — about 6% peak-to-peak), not from a variance run here. It was first
written as ±2%, which understated those very observations; corrected
2026-08-22 together with the README line quoting it. SFIV's steady median (59.45 vs the
long-standing ~59.7-59.9) confirms the renderer's steady body is unchanged;
its window average remains hitch-tail sensitive.
