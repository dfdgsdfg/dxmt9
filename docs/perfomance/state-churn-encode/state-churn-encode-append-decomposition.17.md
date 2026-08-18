---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 17
title: Current-HEAD Producer-Wall Resize — PE Layer 10.3 ms, Drain Fence Harvested, Chunk-Seal Cadence Is The Lead
date: 2026-08-18
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05--pe-decim-now; experiments/output/app-d3d9-3dmark05--drain-now
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.08.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.11.md; docs/perfomance/present-pacing/present-pacing-drain-fence-attribution.206.md
---

# Current-HEAD Producer-Wall Resize — PE Layer 10.3 ms, Drain Fence Harvested, Chunk-Seal Cadence Is The Lead

**Question.** After the parallel partition track closed as performance-neutral
(the encode stage has slack; frames are producer-paced), what is the producer
wall's reducible share **today**, at merged `226922a2`? Two candidates were
sized in separate GT2 runs: the PE layer riding the game thread
(`DXMT9_PE_STATS_DECIMATION=64`, 1,560 presents) and the offload drain fence
(`DXMT9_PERF_DRAIN_FENCE_SITES=1`, ~1,620 presents). Both runs
`DXMT_LOG_LEVEL=info`, perf profile, 120 s supervised.

**Result 1 — the drain fence is a harvested lever.** Total
`offload_drain_fence_wait_ms` is `521.5 ms` over the run = **`0.33 ms/present`
(~0.9% of the ~37 ms frame)**. The scoped-drain landing (`R-BACK-2.51(d)`,
`R-BACK-2.61`) already collapsed the historically dominant site:
`dxmt9c_buffer_lock`, once `99.8%` of the global fence
([.206](../present-pacing/present-pacing-drain-fence-attribution.206.md)), now
waits only `0.063 ms/present` (943 blocked plain/MANAGED locks; the bypass
counters show the mechanism working: `bypass_discard=15,236`,
`bypass_nooverwrite=52,400`). The largest residual is
`dxmt9c_device_get_swap_chain` at `0.249 ms/present` (one global wait per
present, `249.5 µs` each); scoping it is possible but worth at most ~0.7% of
the frame. **No follow-up scheduled** — the remaining fence is not a
first-order lever.

**Result 2 — the PE layer is the remaining reducible vein:
`10.3 ms/present`, ~28% of the frame.** Null-scope calibration applied per the
decimation rules (clock pair measured at `180 ns/sample`; the uncalibrated
`const_setter` reading would be ~10x its true cost):

| scope (calibrated) | ns/call | ms/present |
|---|---|---|
| `entry_draw` (whole Draw call) | 4,959 | **8.40** |
| — `draw_record` (recording inside it) | 4,070 | 6.90 |
| — `append` (recorder core) | 2,328 | 6.33 |
| `entry_const` (whole SetConstant call) | 81 | 1.75 |
| `entry_state` | 92 | 0.18 |
| `const_setter` / `const_flush` / `draw_packet` | 20 / 6 / 311 | 0.42 / 0.06 / 0.53 |
| entry − inner residual | | **2.98** |

The entry−inner residual has shrunk from `15.4 ms` (pre-SWVP-removal,
[.08](state-churn-encode-append-decomposition.08.md)) to `2.98 ms`; the
constant setters are effectively free after calibration. What remains is
draw-record production itself. Caveat: `append`'s reading carries the
documented N=64/chunk-period aliasing (`±13%`, [.08]); the direction of the
conclusion does not depend on it.

**Retraction of the first-draft lead — the `4.9 KB/record` figure was a
counter artifact, not wire volume.** A source investigation
(`d3d9_pe_device.cpp:10234-10298`, `:10401-10454`, `:10201-10231`) shows
`append_drawidx_bytes` sums the caller's `sizeHint`, and for DrawIndexed that
is the frozen constant `kLegacyDrawIndexedPrimitiveSizeHint = 4920` —
deliberately preserved so chunk-seal cadence stays bit-identical to the
pre-migration fat-packet recorder. The measured average equals the constant
bit-for-bit (`12,811,704,600 / 2,604,005 = 4,920.00`), the sibling
`append_draw`/`append_applystate` averages equal their own frozen hints
(`4,888`), and the real sparse record (56 B wire header plus dirty-gated
sections of 8-28 B each) is on the order of a few hundred bytes as designed.
The full-snapshot predicate is confirmed off. There is no wire-bloat lever;
the true per-record bytes live in `CommandChunkBuilder::payloadBytes()` and
are not what this counter measures.

**The corrected lead — chunk-flush cadence, not record size.** The same
emission already decomposes `append`'s real `6.33 ms/present`:
`append_encode` (66,318 samples, `64.5 ms`) is ~`790 ns`/record calibrated =
**`~2.2 ms/present` of per-record section building**, while `append_flush`
(1,073 samples, `72.9 ms`) is **`68 µs` per chunk seal × ~42 seals/present =
`~2.9 ms/present`** — the once-per-64-records chunk flush (the PE-side seal
plus `commitChunk` crossing) is the larger half. `790 ns` to serialize a few
hundred bytes also suggests section-building overhead rather than copying.
Both are attackable: `DXMT9_PE_CHUNK_MAX_RECORDS` (default 64) is already an
env tunable, so the flush share can be A/B'd immediately (fewer, larger
chunks), with the caveat that chunk cadence is a behavioural contract — a
cadence change shifts publish granularity and downstream pipelining, so the
A/B must watch encode-stage pacing and visual gates, not just producer CPU.

**Verdict.** The producer wall's reducible share today is owned by PE
draw-record **production cost** (`~6-8 ms/present`: ~2.9 chunk-seal, ~2.2
per-record encode, ~3.0 entry residual), not by record size, fences
(`0.33`), constant setters (`0.5`), or the encode stage (slack). The next
probes are a chunk-size cadence A/B and a per-record encode decomposition.
