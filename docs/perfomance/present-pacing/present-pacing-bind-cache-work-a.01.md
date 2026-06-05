---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: bind-cache
order: 01
title: Work A First Landing — bind-skip cache extended to five new classes
date: 2026-06-05
type: measurement
status: provisional
source: experiments/output/app-d3d9-3dmark05-current-nondiag-baseline-r1, experiments/output/app-d3d9-3dmark05-bind-cache-work-a-r2
---

# Work A First Landing — bind-skip cache extended to five new classes

**Question / hypothesis.** The
[[present-pacing-encode-budget-fix-proposal.01]] synthesis identified
extending the `_skipped` bind-cache pattern from texture/sampler to
seven additional classes as the most direct path to recovering the
DSync=0 fps win under vsync. This note measures the first landing
covering five of those classes (vertex_buffer, pipeline, depth_state,
viewport, scissor; rasterizer and index_buffer deferred).

**Method.**

Implementation commits (this round):

- `e07cbfe feat(perf): add bind_*_skipped counters + wire vertex_buffer
  cache hit` — counter infrastructure plus vertex_buffer wiring (the
  existing `setVertexBufferCached` lambda already had the shadow
  check; the hit branch was silent).
- `5eef5d4 feat(perf): wire bind-skip cache for pipeline / depth_state
  / viewport / scissor` — pipeline and depth_state had existing
  `bindShadowMatches` cache checks that only counted misses; both
  branches now count. Viewport and scissor had no cache; new
  `ViewportBindShadowSlot` / `ScissorBindShadowSlot` added with
  field-equality checks (`WMTViewport` 6 doubles, `WMTScissorRect`
  4 uint64).

A/B configuration:

```
# A: baseline = current-nondiag-baseline-r1 (run pre-Work-A)
# B: bind-cache-work-a-r2 (run post-Work-A, same wrapper, same prefix)
bash scripts/tools/run_3dmark05_perf_probe.sh --no-gputrace \
  --suffix bind-cache-work-a-r2
```

**Result.**

| Metric | Baseline | Work A (r2) | Δ |
|---|---:|---:|---:|
| **`process_elapsed_sec`** | 251.07 | **131.62** | **−47.6%** |
| Implied fps (≈ inverse) | 1.00× | **1.91×** | **+91%** |
| `completion_wait_ms` | 31,445.8 | 31,345.0 | −0.3% |
| `completion_present_wait_p50_ms` | 23.978 | 24.074 | +0.4% |
| `completion_present_wait_p95_ms` | 29.465 | 31.886 | +8.2% |
| `completion_waits` (CB count) | 1,439 | 1,439 | unchanged |
| `gpu_command_buffer_time_ms` | 4,317.5 | 4,330.9 | +0.3% |
| `encode_draw_cpu_ms` | 16,476.7 | 18,113.7 | +9.9% |
| `encode_chunk_cpu_ms` | 20,686.0 | 23,087.0 | +11.6% |
| `encode_chunk_cpu_p50_ms` | 20.45 | 20.897 | +2.2% |
| `bind_vertex_buffer` | 1,229,683 | 1,215,518 | −1.2% |
| `bind_pipeline` | 232,771 | 232,432 | −0.1% |
| `bind_depth_state` | 17,693 | 17,699 | +0.0% |
| `bind_viewport` | 368,782 | 369,341 | +0.2% |
| `bind_scissor` | 351,913 | 352,463 | +0.2% |

**Interpretation.** The wallclock improvement is large and real
(scene completes in 131.6 s vs 251.1 s — same scene, same Wine, same
binaries except dxmt9 builds with Work A in). However, the bind
counters themselves are essentially unchanged, and the new
`_skipped` counters (`bind_vertex_buffer_skipped`,
`bind_pipeline_skipped`, `bind_depth_state_skipped`,
`bind_viewport_skipped`, `bind_scissor_skipped`) do not appear in
`result.json` for this run, even though `bind_texture_skipped`
(993k) and `bind_sampler_skipped` (1.74 M) do.

Two open questions:

1. **Counter snapshot path.** The new `_skipped` counters were added
   to the `kCounterTable` (`dxmt9_perf_counters.cpp:1124-1138`) but
   are not surfacing in `result.json`. The Python `run_experiment.py`
   may be running an allowlist or schema validation that excludes
   unknown keys. Needs follow-up investigation; the counter table
   itself looks correct on inspection.
2. **Source of the wallclock win.** Per-CB encode (p50 20.45 → 20.90
   ms) is essentially unchanged. `bind_*` counts are essentially
   unchanged. `gpu_command_buffer_time_ms` is essentially unchanged.
   Yet wallclock dropped 47.6%. The improvement does not match the
   sub-counter movements expected from the bind cache itself.
   Candidate explanations:
   - measurement noise / different desktop state between sessions;
   - other recent commits (`c448c3c docs+rules: reflect GT1 perf env
     vars & experiment methods`) may have flipped a default in the
     wrapper between baseline and Work A;
   - the new viewport/scissor cache might short-circuit some
     downstream encoder bookkeeping that wasn't directly attributed.

**Verdict.** Provisional. The directional fps signal (+91%) is large
and welcome, but the mechanism trace is incomplete. Until the
`_skipped` counters surface in `result.json` *and* the source of the
encode CPU change is attributed, treat this run as the first data
point in a longer series rather than the definitive Work A landing.

**Next.**

- Investigate why `bind_*_skipped` counters do not surface in
  `result.json` (counter-table audit, run_experiment.py allowlist).
- Re-run baseline + Work A consecutively in one session to control
  for environment drift, and ideally include a `(baseline,
  Work-A-disabled)` comparison to attribute the wallclock delta to
  the actual cache code rather than measurement-time noise.
- Wire the remaining two classes (rasterizer, index_buffer) per the
  fix-proposal taxonomy in
  [[present-pacing-encode-budget-fix-proposal.01]].

Out of scope for this note: a stable production-grade promotion
decision. Promotion requires the counter signal *and* a clean A/B
that isolates the code change.
