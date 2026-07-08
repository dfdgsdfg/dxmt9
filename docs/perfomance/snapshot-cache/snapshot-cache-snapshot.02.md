---
domain: snapshot-cache
workload: 3DMark05 GT1
subcategory: snapshot
order: 02
title: Snapshot Cache Split Implementation Status
date: 2026-06-04
type: experiment-run
status: inconclusive
source: specs/perfomance.plan.md#L1656-L1731
---

# Snapshot Cache Split Implementation Status

**Question / hypothesis.** Splitting invalidation into full hot-state vs
uniform-only should let const-only and draw-param-only packets reuse the hot
state, lifting cache hits off zero and dropping snapshot CPU.

**Method.** First optimization patch:
- full hot-state invalidation for shader, FVF/vdecl, stream, IB, texture, RT/DS,
  render-state, viewport, scissor, transform, material, light, clip changes;
- uniform-only invalidation for VS/PS constant C-ABI setters.
- `applyDrawPacketStateDirect()` no longer calls `mutableState()` for param-only
  draw packets; `setStreamSource()` / `setIndices()` now no-op when the binding is
  identical to current, so repeated same-IB/same-stream packets do not advance the
  hot-state generation.

```bash
scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix snapshot-cache-split-nogputrace-r2 \
  --frame 50 --encoder-breakdown-seq 50 \
  --no-gputrace --timeout 420
```

**Result.** (before split → after split r2)
- `status`/`timed_out`/`returncode`: `pass`/`true`/`-15` → `pass`/`false`/`0` — run
  now exits cleanly after the GT1 window (was timeout).
- `d3d9_draw_state_cache_hits`: `0` → `126240`.
- `d3d9_draw_state_cache_misses`: `760979` → `636130` (~125k rebuilds removed, most
  misses remain).
- `d3d9_draw_state_cache_uniform_refreshes`: `0` → `126224` — const uploads now
  refresh uniform payloads without hot-state rebuild.
- `d3d9_snapshot_draw_submission_cpu_ms`: `21617.725` → `20973.012` (~3% only).

**Verdict.** Inconclusive as a win: hits left zero and the run stabilized, but
snapshot CPU dropped only ~3%. The remaining bottleneck is NOT const-only churn —
motivates per-miss-reason instrumentation to find what still rebuilds.

**Related.** [snapshot-cache](index.md) · prev [snapshot-cache-snapshot.01](snapshot-cache-snapshot.01.md) · next
[snapshot-cache-snapshot.03](snapshot-cache-snapshot.03.md) · [const-upload](../const-upload/index.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md)
