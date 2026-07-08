---
domain: baselines
workload: 3DMark05 GT1
subcategory: runlevel
order: 01
title: Run-Level Counters
date: undated
type: measurement
status: accepted
source: specs/perfomance.plan.md#L2017-L2041
---

# Run-Level Counters

**Question / hypothesis.** What is the whole-run GT1 counter shape that
per-frame captures sit inside? Provides the present/draw/state-delta context for
the single-frame snapshots.

**Method.** Run-level `[dxmt9-perf]` counters from the GT1 capture run, read
alongside the frame120 single-frame Xcode export. No new wrapper invocation —
these are the aggregate counters emitted across the captured run window.

**Result.**
- Presents `present_encoded=1260`; draws `draw_calls=915070` (~`726 draw/present`).
- Command buffers `5039`, sub-command buffers `3777` (mid-chunk split active, cap `4`).
- Render passes `render_pass_begin=14684` (~`11.7 pass/present`).
- Split causes: `render_split_rt_change=9842`, `render_split_clear=3589`,
  `render_split_present=1253` (RT changes + clear boundaries dominate splitting).
- Tile preservation `render_pass_tile_preservation_bytes=167739686912` (~`167.74GB`).
- CPU: `encode_chunk_cpu_ms=20085.516`, `encode_draw_cpu_ms=17342.358`
  (~`19.0us/draw`), `submit_draw_cpu_ms=4328.237`.
- `argbuf_hybrid_bytes_per_encoder=1064316728` (multi-GB VS/FFPVS amplification largely removed).
- Transient upload: `2.77M` calls / `2.11GB` / `2881.105ms`.
- `d3d9_buffer_lock_ms=3696.747`.
- Waits: `map_buffer_wait_ms=0`, `queue_sequence_wait_ms=0`,
  `present_boundary_wait_ms=0`, `present_acquire_wait_ms=127.137`.
- Draw-run submits `582` (runs still scarce); break classes: const upload `661153`,
  state delta `232307`, first delta `0`.
- State deltas: stream `796529`, IB `753409`, texture `234811`, shader `182551`, FVF `146391`.
- `cold_compile_count_after_warm=523`.

**Verdict.** Accepted as run-level context. Confirms the CPU side is structurally
expensive (per-draw encode, scarce draw-run batching, stream/IB churn) and that
none of the queue/map/boundary waits are the blocker — but the GPU limiter lives
in the per-frame captures, not here. Frames it contextualizes:
[baselines-frame120.01](baselines-frame120.01.md).

**Related.** [baselines](index.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md) · [baselines-frame120.01](baselines-frame120.01.md) ·
[state-churn-encode](../state-churn-encode/index.md) (stream/IB deltas, draw-run scarcity) ·
[render-pass-store](../render-pass-store/index.md) (tile-preservation + split causes) · [const-upload](../const-upload/index.md) (break classes).
