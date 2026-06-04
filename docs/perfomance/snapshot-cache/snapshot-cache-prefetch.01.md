---
domain: snapshot-cache
workload: 3DMark05 GT1
subcategory: prefetch
order: 01
title: Layout-Stride PSO Prefetch Fix (frame50 replay)
date: 2026-06-04
type: validation
status: accepted
source: specs/perfomance.plan.md#L875-L985
---

# Layout-Stride PSO Prefetch Fix (frame50 replay)

**Question / hypothesis.** After binding-agnostic snapshot bypassed PSO prefetch
for any override draw (pipeline-lookup CPU spiked to ~6.7s), preserve the
extra-stream stride in the shader layout so the prefetched PSO handle matches the
override-applied layout. Goal: prefetch becomes usable again with correct textures.

**Method.** Full `.gputrace` + Xcode encoder-counter export on frame50:

```bash
scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix layoutstride-frame50-gputrace-r1 \
  --frame 50 --encoder-breakdown-seq 50 --timeout 420
scripts/tools/finalize_3dmark05_perf_probe.sh \
  --suffix layoutstride-frame50-gputrace-r1 --frame 50 --top 3 --hot-gpu-share 95.0
```

Xcode Summary: 4 command buffers, 10 render encoders, 396 draws, 2,146,296
vertices, `34.379ms` total GPU; frame visually normal.

**Result.**
- Prefetch counters confirm the fix holds under capture:
  `encode_draw_pso_prefetch_handle_available=340157`,
  `encode_draw_pso_prefetch_handle_used=340157`,
  `encode_draw_pso_prefetch_bypass_binding_override=0`,
  `encode_draw_pso_prefetch_binding_override_compatible=313033`,
  `encode_draw_pso_prefetch_binding_override_incompatible=0`.
- CPU still nontrivial: `d3d9_snapshot_draw_submission_cpu_ms=19251.620ms`,
  `encode_draw_cpu_ms=20799.848ms`, `submit_draw_cpu_ms=3373.259ms`,
  `encode_draw_stream_bind_cpu_ms=2894.697ms`.
- Not buffer-bound: `map_buffer_wait_ms=0`, `queue_sequence_wait_ms=0`.
- Pacing wait high: `completion_wait_ms=28413.664ms`;
  `gpu_command_buffer_time_ms=4134.078ms`.
- GPU owner unchanged vs current-normal frame50 (`35.024ms` → `34.379ms`,
  `-1.84%`); top-3 VS buffer write `1627.287MiB`, top-3 draws/vertices/triangles
  and stream/IB handle changes identical (0.00% delta).

**Verdict.** Accepted as a CPU-path verification: PSO prefetch is now functional
(available == used, bypass == 0) with correct textures. It is NOT a GPU reduction —
the GPU owner remains hidden vertex/backend traffic. Snapshot CPU and pacing
(`completion_wait`) waits remain open CPU tracks distinct from the GPU bottleneck.

**Related.** [[snapshot-cache]] · prev [[snapshot-cache-binding.01]] · GPU owner
[[hidden-backend-storage]] · churn axis [[state-churn-encode]] · [[overview]] ·
CPU counters [[perfomance-bottleneck]]
