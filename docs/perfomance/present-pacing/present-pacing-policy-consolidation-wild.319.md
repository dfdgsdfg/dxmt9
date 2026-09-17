---
domain: present-pacing
workload: 3DMark05 GT2 / 3DMark06 GT2 / STALKER CoP Day
title: "Committed policy consolidation: three completed wild smoke runs"
type: evidence
status: measured
updated: 2026-09-17
source: experiments/output/policy-consolidation-r641-20260917.noindex/analysis.json; experiments/output/app-d3d9-3dmark05-policy-consolidation-r641-05-gt2/result.json; experiments/output/app-d3d9-3dmark06-policy-consolidation-r641-06-gt2/result.json; experiments/output/app-d3d9-stkcop-bench-policy-consolidation-r641-stkcop-day/result.json
related: present-pacing-policy-consolidation.318.md; overview.md
---

# Committed-image wild smoke; performance non-regression remains open

Commit `c9256634` consolidates the H318 paths. One sequential run each of
05 GT2, 06 GT2 and STALKER Day completes with exit 0, no timeout, no runner
failure and an actual scene capture. Five staged-image hashes match the
current build outputs in every run. No source/binary changes, builds, tests,
TLC or profiler overlap the benchmark intervals.

The sampled scenes show no obvious new black-screen, missing-geometry or
repeating-pattern defect. This is spot visual evidence, not whole-sequence
pixel equality or a proof against intermittent flicker. Desktop inspection
uses Computer Use screenshots; it is not an additional rendering oracle.

## Recipe and results

All runs use `sikarugir-cx-24.0.7`, the `perf` profile, validation OFF,
Warn logging, offloaded replay ON, opaque-depth index optimization OFF,
perf counters ON and periodic reports every 60 Presents. Frame sampling is
ON for 05/06 and OFF for STALKER. No experimental policy overrides are set:
precise uniform invalidation and lock refinement are unconditional; async
query fences default ON and END coalescing remains OFF. There is no gputrace.

| Workload | Configuration | FPS / source | Measured population |
|---|---|---:|---|
| 3DMark05 GT2 | 1024×768 | 26.385, renderer scene frames | Frames 1–1712, 64.886141 s |
| 3DMark05 GT2, same run | 1024×768 | 27.389532, benchmark `.3dr` | Captured and SHA-256-verified GT2 result |
| 3DMark06 GT2 | 1280×720 | 16.417, renderer scene frames | Frames 775–1828, 64.201673 s |
| STALKER Day | High, `renderer_r2.5`, 1280×720 windowed | 13.763994, benchmark `.result` | 2,031 benchmark frame rows; min 2.672165 / max 43.095333 |

The scene predicate is the established H312 rule: positive `wall_ms` and
`draw_calls > 100`, with contiguous frame IDs. No fast menu frames enter the
scene average. The unfiltered 06 summary is **26.897 FPS and must not be used**:
it mixes menu/loading frames with the actual scene. Neither process-elapsed
FPS nor the 05 `.3dr` value is substituted for the renderer scene average.
The 05 `.3dr` is benchmark-owned, but this run still has observers enabled;
it is not an observer-free measurement.

06 Advanced Edition emits no `.3dr`, as recorded by `not_emitted`. After
the scene returns to the score dialog, the task-owned UI is closed. The
runner records normal exit rather than a timeout. STALKER produces its
benchmark-owned `daybenchmark.result`; its window includes the title bar
(1280×748), while the configured render area is 1280×720.

## Health and artifact qualification

All eight required counters are present and zero in every run:
`chunk_reject`, `command_chunk_rejects`, `gpu_command_buffer_errors`,
`pipeline_build_fail_library`, `draw_skipped_no_pipeline`,
`pipeline_build_fail_draw`, `pipeline_build_fail_function`, and
`pipeline_build_fail_pso`. The 05/06 final cumulative health endpoints cover
the entire sampled scene; uncovered health tail is zero in both runs.
STALKER's final cumulative endpoint is 2,120 encoded Presents, distinct from
its 2,031 benchmark-owned frame rows. Do not join these populations as if they
were one frame census.

The `analyze.py` beside `analysis.json` rechecks status, profile, image hashes,
eight health counters, capture presence, scene continuity and `.3dr` digest.
Each run directory retains `result.json`, logs, `actual.png`, independently
inspected scene screenshots and host process snapshots. STALKER also retains
its loading screenshot separately; that image is not counted as scene proof.

## Interpretation and next gate

Against the README's renderer values, the 05 scene spot is about **12.3%**
lower (30.1 → 26.385) and 06 about **9.3%** lower (18.1 → 16.417).
STALKER's benchmark average is about **10.1%** above its historical 12.5.
These are different-date spot comparisons, not change attribution. In
particular, the 05 `.3dr` result must not be compared directly with the
README renderer average as though both used the same timing interval.

Host snapshots record substantial unrelated activity during the run sequence
(including suggestion/indexing services and the application UI). This is a
confounder, not proof that background load caused the lower GT2 results.
No parent-commit same-session control was run, so the data neither establish
non-regression nor prove that H318 caused a regression. README is unchanged.
The next performance gate is a matched-config parent/current comparison,
with alternating order and repeat controls, on a quiet host. Preserve these
spots even if later measurements recover.
