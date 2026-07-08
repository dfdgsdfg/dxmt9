---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: opaque
order: 03
title: Layout-Stride Opaque Opt-In Xcode Replay
date: 2026-06-04
type: validation
status: accepted
source: specs/perfomance.plan.md#L1183-L1292
---

# Layout-Stride Opaque Opt-In Xcode Replay

**Question / hypothesis.** Does the opaque-depth cache opt-in produce a real
hardware-visible reduction on its target rows (50/0, 50/1) in an Xcode replay,
even though it cannot fix the whole frame?

**Method.** `run_3dmark05_perf_probe.sh --suffix layoutstride-opaque-index-cache-gputrace-r1
--frame 50 --optimize-opaque-depth-index-cache --optimize-opaque-depth-index-cache-min-gain-pct 10
--baseline-joined <layoutstride frame50 joined> --target-row-key 50/0 --target-row-key 50/1
--require-opaque-depth-index-cache-proof --timeout 420`. The proof preset expands
to stable-frame gates, target cache-opt/effective LRU32 decrease, positive target
reordered-cache hits, and target VS write/invocation decrease.

**Result.** Target proof PASS, but the strict global `--require-top-gpu-decrease`
gate intentionally FAILED (`top_gpu_ms 34.018→34.304`, `+0.84%`).
Target `50/0+50/1`: GPU `14.081→12.908ms` (`-8.33%`); VS write `646.110→537.779MiB`
(`-16.77%`); VS invocations `536,583→460,839` (`-14.12%`). Per row: `50/0`
`5.700→4.813ms` (`-15.56%`), `50/1` `8.381→8.095ms` (`-3.41%`).
Top VS buffer write `1,627.287→1,518.957MiB` (`-6.66%`). Untouched `50/2`
now dominates at `21.111ms` / `60.90%`, still `981.178MiB` VS write
(`956.701MiB` hidden backend).

**Verdict.** Accepted as a real per-row win; rejected as a frame-level fix. Lower
post-transform misses reduce Xcode `VS Invocations`, VS write, and GPU time on
the intended opaque rows — confirming the [tvb-mechanism-proof](../tvb-mechanism-proof/index.md) scaling law.
The remaining owner is `50/2` depth-read/blended geometry.

**Related.** [index-cache-locality](index.md) · prev: [index-cache-locality-opaque.02](index-cache-locality-opaque.02.md)
· next: [index-cache-locality-screenblend.02](index-cache-locality-screenblend.02.md) (50/2 attack) ·
[tvb-mechanism-proof](../tvb-mechanism-proof/index.md) · [hidden-backend-storage](../hidden-backend-storage/index.md) (50/2 residual).
