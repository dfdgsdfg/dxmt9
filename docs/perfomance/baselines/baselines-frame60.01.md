---
domain: baselines
workload: 3DMark05 GT1
subcategory: frame60
order: 01
title: Current-Source Frame60 Validation
date: 2026-06-01
type: validation
status: accepted
source: specs/perfomance.plan.md#L5969-L6076
---

# Current-Source Frame60 Validation

**Question / hypothesis.** Rebuild the current source tree and capture frame60
with full finalizer gates to validate per-encoder dxmt attribution + unique
shader matches — establishing the frame60 A/B baseline for VSOut/backend/state
probes.

**Method.** Captured run `app-d3d9-3dmark05-current-source-frame60-r3` with
`frame60.gputrace`, embedded performance export, Xcode encoder counters, and
joined bottleneck + shader-dump reports under `analysis/`. The finalizer passed
`--require-xcode-counter-coverage`, `--require-dxmt-join-coverage`,
`--require-top-pso-attribution`, and `--require-shader-dump-matches`, so this
capture has current-source per-encoder dxmt attribution and unique VS/PS dump
matches.

**Result.** Xcode frame60: `34.02ms` GPU, 4 command buffers, 10 render
encoders, 396 draws, `2,146,296` vertices. Top 3 = `33.481ms` / `98.41%`.
- Top-3 buffer write `1628.086MiB`; **top-3 VS buffer write `1627.414MiB`**.
- Top-3 dxmt CPU writer bytes `0.444MiB`; unexplained Xcode buffer write `1627.642MiB`.
- VS buffer / expected VSOut **`7.9x`**; VS buffer / stream0 input `33.1x`.
- Weighted vertex-stage time `95.96%`; VS ALU limiter only `2.65%`;
  VS buffer-write limiter `21.75%`.
- dxmt stream handle changes `437`, IB handle changes `326`; argbuf cbuf `0.430MiB`;
  transient vertex/index `0.000MiB`.
- Per encoder: `seq=60 enc=2` `19.098ms` (`981.230MiB` write, 187 draws);
  `seq=60 enc=1` `8.994ms` (`421.398MiB`, 156 draws);
  `seq=60 enc=0` `5.389ms` (`225.458MiB`, 42 draws).
- Visible MSL `VSOut` width `184B`, but Xcode reports `1151`–`1603` VS bytes/inv.
- Run-level: `present_encoded=1379`, `draw_calls=1006708`, `render_pass_begin=16174`,
  `commit_chunk_draw_run_submits=79605`, `commit_chunk_draw_run_records=328886`,
  `commit_chunk_draw_run_binding_override_records=248205`,
  `commit_chunk_draw_batch_const_upload_passthrough=760213`,
  `encode_draw_cpu_ms=61273.148` (instrumented/capture run),
  `gpu_command_buffer_time_ms=3969.171`, `map/queue waits 0`.

**Verdict.** Accepted as **the frame60 A/B baseline**. Same shape as frame120
and frame50: top-3 buffer write ≈ all VS buffer write, dxmt CPU writers
(`0.444MiB`) explain ≈ none, visible `184B` VSOut width is `7.9x` too small to
explain the `1627.4MiB` bucket. Stream/IB override instrumentation closed the
attribution gap but did not move the bottleneck — primary owner is GPU-side
vertex-stage buffer write pressure; secondary is residual stream/IB churn +
const-upload run breaks.

**Related.** [[baselines]] · [[overview-3dmark05-gt1]] · [[baselines-frame50.01]]
(parallel frame50 baseline) · [[baselines-frame120.01]] ·
[[hidden-backend-storage]] (unexplained `1627.6MiB`) ·
[[vsout-layout]] (rejects `184B`/`7.9x`) · [[backend-shape-classifiers]] ·
[[state-churn-encode]] (stream/IB churn `437`/`326`, run breaks).
