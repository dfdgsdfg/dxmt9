---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 17
title: Current-HEAD Producer-Wall Resize — PE Layer 10.3 ms, Drain Fence Harvested, Draw Records Average 4.9 KB
date: 2026-08-18
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05--pe-decim-now; experiments/output/app-d3d9-3dmark05--drain-now
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.08.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.11.md; docs/perfomance/present-pacing/present-pacing-drain-fence-attribution.206.md
---

# Current-HEAD Producer-Wall Resize — PE Layer 10.3 ms, Drain Fence Harvested, Draw Records Average 4.9 KB

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

**The concrete lead — draw records average `4.9 KB` each.**
`append_drawidx_bytes = 12.81 GB` over the run against
`append_drawidx_calls = 2.60 M`: **`4,920 bytes` per indexed-draw record,
`8.2 MB/present` of wire volume**, against the sparse-delta design's
documented "typical record ~100 B" (`environment_variables_bridge.rules.md`,
`DXMT9_PE_DRAW_FULL_SNAPSHOT` row). Two orders of magnitude above the design
intent, and byte copying is exactly where `append`'s `2.3 µs/call` goes. The
next question is the record's byte composition: what fills 4.9 KB per draw
(inline uniform payloads? redundant sections? full-snapshot-shaped state
riding the delta path?), and how much of it is removable or deduplicable.
While frames are producer-paced, every removed millisecond here converts 1:1
to fps.

**Verdict.** The producer wall's reducible share today is owned by PE
draw-record production (`~6-8 ms/present`), not by fences (`0.33`), constant
setters (`0.5`), or the encode stage (slack). The next track is a draw-record
byte-composition decomposition.
