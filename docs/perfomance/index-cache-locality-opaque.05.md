---
domain: index-cache-locality
subcategory: opaque
order: 05
title: Current Non-Diagnostic Opaque-Depth Smoke
date: 2026-06-05
type: experiment-run
status: accepted
source: specs/perfomance.plan.md#L179-L250
---

# Current Non-Diagnostic Opaque-Depth Smoke

**Question / hypothesis.** Does the opt-in carry an obvious profile-level runtime
cost *outside* the diagnostic path? Re-run the baseline/opt-in pair without
frame50 encoder breakdown or per-draw probe logging.

**Method.** Two `run_experiment.py run app-d3d9-3dmark05` runs under
`DXMT_EXPERIMENT_PROFILE=perf DXMT_3DMARK05_DIRECT=1 DXMT_3DMARK05_RESULT_FILE=dxmt9_gt1.3dr`,
the opt-in adding `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1` /
`DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE_MIN_GAIN_PCT=10`, `--timeout 180`.
Both `status=pass`, `returncode=143`, `present_encoded=1440`.

**Result.** No run-level GPU regression: `gpu_command_buffer_time_ms 4,317.475→4,310.685`
(`-0.16%`); `render_pass_tile_preservation_bytes +0.09%` (within noise);
`completion_wait_ms 31,445.814→27,666.905` (`-12.02%`). CPU encode-side
stream-binding still rises materially: `encode_draw_stream_bind_cpu_ms
1,794.718→2,249.982` (`+25.37%`, cold cache cost); `encode_draw_cpu_ms +2.11%`.

**Verdict.** Accepted (no GPU/store regression at profile scale) but the
`+25.37%` CPU stream-bind cost means the production path must not become a shared
`perf` default until CPU accounting is amortized. `process_elapsed_sec` includes
the timeout tail — not an FPS sample. This motivated the CPU-cost split that follows.

**Related.** [[index-cache-locality]] · prev: [[index-cache-locality-opaque.04]]
· next: [[index-cache-locality-cpucost.01]] (the CPU split) ·
[[tvb-mechanism-proof]].
