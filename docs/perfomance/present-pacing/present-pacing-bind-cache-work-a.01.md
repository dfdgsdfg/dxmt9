---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: bind-cache
order: 01
title: Work A First Landing — bind-skip cache extended to five new classes (no measurable wallclock win)
date: 2026-06-05
type: measurement
status: rejected
outdated: evidence-missing
source: experiments/output/app-d3d9-3dmark05-current-nondiag-baseline-r1, experiments/output/app-d3d9-3dmark05-bind-cache-work-a-r3
---

# Work A First Landing — bind-skip cache extended to five new classes (no measurable wallclock win)

> **Outdated — every artifact this leaf cites in `source:` is gone from disk.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** The
[present-pacing-encode-budget-fix-proposal.01](present-pacing-encode-budget-fix-proposal.01.md) synthesis identified
extending the `_skipped` bind-cache pattern from texture/sampler to
seven additional classes as the most direct path to recovering the
DSync=0 fps win under vsync, on the model that the per-CB encode CPU
(p50 = 20.45 ms) sits 22.7% over the 16.67 ms vsync slot and that the
unaccounted 73% of `encode_draw_cpu_ms` is per-draw Metal bind calls.

This note measures the first landing covering five of those classes
(vertex_buffer, pipeline, depth_state, viewport, scissor; rasterizer
and index_buffer deferred).

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
# B: bind-cache-work-a-r3 (post-Work-A, same wrapper, same prefix,
#                          immediate retest after r2 anomaly)
DXMT_3DMARK05_PREFIX=/Users/dididi/workspaces/dxmt9/experiments/prefixs/app-d3d9-3dmark05 \
  bash scripts/tools/run_3dmark05_perf_probe.sh --no-gputrace \
  --suffix bind-cache-work-a-r3
```

**Result (r3, the controlled repeat).**

| Metric | Baseline | Work A (r3) | Δ |
|---|---:|---:|---:|
| **`process_elapsed_sec`** | 251.07 | **251.07** | **0.00%** |
| Implied fps | 1.00× | 1.00× | unchanged |
| `completion_wait_ms` | 31,445.8 | 30,946.6 | −1.6% |
| `completion_present_wait_p50_ms` | 23.978 | 24.414 | +1.8% |
| `completion_waits` (CB count) | 1,439 | 1,439 | unchanged |
| `gpu_command_buffer_time_ms` | 4,317.5 | 4,320.8 | +0.1% |
| `encode_chunk_cpu_ms` | 20,686.0 | 23,304.6 | +12.7% |
| `encode_chunk_cpu_p50_ms` | 20.45 | 21.861 | +6.9% |
| `bind_vertex_buffer` | 1,229,683 | 1,215,307 | −1.2% |
| `bind_pipeline` | 232,771 | 232,733 | −0.0% |
| `bind_depth_state` | 17,693 | 17,695 | +0.0% |
| `bind_viewport` | 368,782 | 369,469 | +0.2% |
| `bind_scissor` | 351,913 | 352,597 | +0.2% |

**The r2 anomaly.** An earlier run
(`bind-cache-work-a-r2`) reported `process_elapsed_sec = 131.62 s`
(−47.6%) but its output directory was subsequently deleted and the
result cannot be reproduced — r3 with the same env, same wrapper,
same code, immediately after, produced the exact baseline figure.
r2 was a measurement glitch, not a real win. The directional fps
signal in the original draft of this note was therefore wrong.

**Mechanism trace (why bind cache didn't move wallclock).**

The `bind_*` counts barely move (≤ 1.2% across all five classes
this round wired). That means the shadow cache rarely hits — each
draw re-binds essentially the same number of vertex buffers,
pipelines, viewports, scissors as before. Three reasons for this on
GT1:

1. Each draw genuinely targets a different stream slice / pipeline
   variant. The texture/sampler caches see 53% / 92% hit rates
   because the same texture handle reappears across many draws; the
   stream binding changes per-draw because each draw uses a
   different vertex buffer offset.
2. Even when the same (buffer, offset) pair would be valid, the
   shadow can lose track across encoder boundaries (the shadow
   resets when a render-pass restart fires), and GT1 has 10
   render-pass encoders per frame × 1,439 CBs = many resets.
3. The viewport / scissor are set per-encoder-prologue *and*
   per-draw via `setRasterizerCullMode`'s caller — when the
   prologue path runs, the per-draw cache is cold.

`encode_chunk_cpu_ms` actually rose 12.7% — the added cache-check
overhead (extra branches per bind site, equality comparisons on 6
doubles for viewport) costs more than the few skip savings can
return.

**Verdict.** Rejected. The bind-skip cache extension to vertex_buffer
/ pipeline / depth_state / viewport / scissor compiles cleanly, the
tests pass, but on 3DMark05 GT1 it does not move wallclock and
slightly increases encode CPU. The mechanism the
[present-pacing-encode-budget-fix-proposal.01](present-pacing-encode-budget-fix-proposal.01.md) synthesis proposed —
"bind-call suppression via wider cache coverage" — is not the right
lever for this workload because the per-draw binding diversity is
high.

The infrastructure (counters, helpers, shadow types) is harmless
and useful for future measurement, so it is left in place. The
counters themselves do appear when present
(`bind_texture_skipped`, `bind_sampler_skipped`), but the new
`_skipped` keys may not surface in `result.json` until the schema
allowlist is updated; that's a separate small fix.

**What this rules out.**

- Bind-call frequency is *not* the dominant unattributed remainder
  of `encode_draw_cpu_ms` on GT1.
- Adding equality comparisons on the per-draw rebind path can
  *increase* encode CPU when hit rate is low.
- The headline measurement in [present-pacing-encode-budget.01](present-pacing-encode-budget.01.md)
  (73% of `encode_draw_cpu_ms` unattributed) needs a different
  attribution model — possibly draw-record decode, payload-arena
  copy, or D3D9-state shadow synthesis rather than the Metal bind
  calls themselves.

**Next.**

The proven and shippable fps win remains [present-pacing-display-sync.01](present-pacing-display-sync.01.md)
via the new `DXMT9_DISABLE_VSYNC` option (commit `901c145`). For
restoring fps under vsync, the next attribution work should look
beyond the bind path:

- Profile `encode_draw_cpu_ms` with a sampling profiler against the
  current build to localise the unattributed 73%;
- Investigate draw-run break taxonomy as the encode-cost driver
  (`commit_chunk_draw_run_break_state_delta_mixed_*` already
  exposes it — owned by [state-churn-encode](../state-churn-encode/index.md));
- Defer rasterizer / index_buffer wiring until a path with real
  expected savings is demonstrated.

The
[present-pacing-encode-budget-fix-proposal.01](present-pacing-encode-budget-fix-proposal.01.md) "Work A: extend bind
cache" recommendation is hereby downgraded — keep the infrastructure
for future use, but do not expect fps improvement from extending it
to additional classes on this kind of workload.
