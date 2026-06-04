---
domain: snapshot-cache
workload: 3DMark05 GT1
subcategory: snapshot
order: 01
title: Snapshot Cache No-Gputrace Probe
date: 2026-06-04
type: measurement
status: model
source: specs/perfomance.plan.md#L1537-L1655
---

# Snapshot Cache No-Gputrace Probe

**Question / hypothesis.** Separate D3D9 frontend draw-state snapshot/build cost
from Metal backend encode cost. New `d3d9_draw_state_cache_*` and
`d3d9_snapshot_draw_submission_cpu_*` counters were added to measure how often
the importer-side `CachedBaseDrawState` actually serves a hit.

**Method.** No-gputrace probe:

```bash
scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix snapshot-cache-nogputrace-r1 \
  --frame 50 --encoder-breakdown-seq 50 \
  --no-gputrace --timeout 420
```

Run shape `status=pass`, `timed_out=true`, `returncode=-15`,
`process_elapsed_sec=491.399` (timeout-finalized after the GT1 window).

**Result.**
- `d3d9_snapshot_draw_submission_cpu_ms` = `21617.725ms` — the single largest CPU
  cost, larger than queue submit `submit_draw_cpu_ms=3031.424ms` and backend
  `encode_draw_cpu_ms=15951.144ms`.
- Draw-state cache hits / misses = `0` / `760979` — cache effectively disabled by
  generation churn.
- Miss split with-index / no-index = `751326` / `9653` — almost all misses are the
  indexed draw path.
- `commit_chunk_draw_batch_const_upload_passthrough=766530` — const records do NOT
  break the pending batch path; `commit_chunk_draw_run_break_type_const_upload=655183`.
- Per-snapshot latency p50/p95/p99/max = `0.032`/`0.034`/`0.047`/`61.256ms` — small
  per draw but multiplied by ~761k lookups.
- `gpu_command_buffer_time_ms=4079.087ms`; `completion_wait_ms=30573.154ms`.

**Verdict.** Model/diagnosis: const upload + mixed stream/IB state churn invalidate
the entire base draw-state cache every draw, so every snapshot rebuilds shader
layout, hot state, and uniforms. The optimization target shifts from "const upload
breaks submit" (already safe) to "split stable PSO/binding state from volatile
uniform state."

**Related.** [[snapshot-cache]] · next [[snapshot-cache-snapshot.02]] · the CPU
counter design [[perfomance-bottleneck]] · churn axis [[state-churn-encode]] ·
indexed path [[index-cache-locality]] · const path [[const-upload]] · [[overview]]
