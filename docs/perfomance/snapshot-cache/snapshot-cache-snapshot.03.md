---
domain: snapshot-cache
workload: 3DMark05 GT1
subcategory: snapshot
order: 03
title: Snapshot Cache Miss-Reason Instrumentation
date: 2026-06-04
type: measurement
status: model
source: specs/perfomance.plan.md#L1732-L1817
---

# Snapshot Cache Miss-Reason Instrumentation

**Question / hypothesis.** After the split left ~636k misses, classify *which*
state delta causes each remaining cache miss via new
`d3d9_draw_state_cache_miss_after_*` counters.

**Method.**

```bash
scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix snapshot-cache-miss-reason-nogputrace-r2 \
  --frame 50 --encoder-breakdown-seq 50 \
  --no-gputrace --timeout 420
```

Run `status=pass`, `timed_out=false`, `returncode=0`.

**Result.**
- hits / misses = `125555` / `634677`; `d3d9_snapshot_draw_submission_cpu_ms` =
  `21142.122ms` (still ~21s/run).
- `miss_after_draw_packet` = `634635` — almost every miss follows imported
  draw-packet state delta.
- `miss_after_stream` = `630814` — stream-source churn present on **99.39%** of
  misses.
- `miss_after_index_buffer` = `620041` — IB churn present on **97.69%** of misses.
- Secondary: `miss_after_texture=259096`, `miss_after_shader=199479`,
  `miss_after_fvf_vdecl=161441`.
- Real handle churn confirms it is not packet-mask noise:
  `commit_chunk_draw_delta_stream_handle=1044022`,
  `commit_chunk_draw_delta_ib_handle=752345`.

**Verdict.** Model: stream/IB handle churn is the dominant owner of the snapshot
rebuild cost. Next design must preserve the stable hot cache across
stream/IB-only or stream/IB-dominant packets and carry stream/IB bindings as
per-draw `DrawBindingOverride` data (matching the draw-run binding-override
direction), while still rebuilding for shader/FVF/RT-DS/viewport/scissor/
render-state changes.

**Related.** [snapshot-cache](index.md) · prev [snapshot-cache-snapshot.02](snapshot-cache-snapshot.02.md) · next
[snapshot-cache-binding.01](snapshot-cache-binding.01.md) · churn axis [state-churn-encode](../state-churn-encode/index.md) · indexed path
[index-cache-locality](../index-cache-locality/index.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md)
