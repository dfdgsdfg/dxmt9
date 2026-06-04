---
domain: index-cache-locality
subcategory: opaque
order: 02
title: Layout-Stride Opaque Opt-In No-Gputrace Check
date: 2026-06-04
type: experiment-run
status: accepted
source: specs/perfomance.plan.md#L1107-L1182
---

# Layout-Stride Opaque Opt-In No-Gputrace Check

**Question / hypothesis.** Does the production-shaped opaque-depth cache opt-in
still apply *only* to the accepted safe rows on the layout-stride code, before
spending Xcode replay time? This is the safety-boundary preflight.

**Method.** `run_3dmark05_perf_probe.sh --suffix layoutstride-opaque-index-cache-nogputrace-r1
--frame 50 --encoder-breakdown-seq 50 --no-gputrace --optimize-opaque-depth-index-cache
--optimize-opaque-depth-index-cache-min-gain-pct 10 --timeout 240`
(`DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1`, `_MIN_GAIN_PCT=10`).
Timeout-finalized pass, `present_encoded=1440`.

**Result.** Safety boundary held: only opaque depth-writing rows received
reordered cached IBs, `50/2` untouched. `50/0` `33` applied (LRU32 `164,428→120,890`,
`-43,538` / `-26.48%`); `50/1` `69` applied (`295,591→213,046`, `-82,545` / `-27.93%`);
`50/2` `0` applied. Opaque target total `198` draws / `978,057` vertices,
`102` applied, LRU32 `460,019→333,936` (`-126,083` / `-27.41%`).
The no-mutate scout estimated `137` candidates on `50/1` but the min-gain path
applies only `69`; the extra candidates were too small to move VS write.

**Verdict.** Accepted (mechanism active and correctly scoped). Run-level GPU
counters (`gpu_command_buffer_time_ms` `4575 vs 4308`) are timeout-finalized
whole-run samples and cannot be used as the GPU proof; the boundary check is the
value here. Tracks the candidate ceiling from [[index-cache-locality-opaque.01]].

**Related.** [[index-cache-locality]] · prev: [[index-cache-locality-opaque.01]]
· next: [[index-cache-locality-opaque.03]] · [[tvb-mechanism-proof]] (mechanism that makes it real).
