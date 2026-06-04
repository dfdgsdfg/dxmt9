---
domain: index-cache-locality
subcategory: opaque
order: 07
title: Fast-Measure Xcode Proof
date: 2026-06-05
type: validation
status: accepted
source: specs/perfomance.plan.md#L576-L674
---

# Fast-Measure Xcode Proof

**Question / hypothesis.** Does the current fast-measure opaque-depth opt-in still
preserve the accepted hardware-visible mechanism — fewer VS invocations → less TVB
write → lower GPU time — under a strong-gated frame50 Xcode replay? This is the
ACCEPTED production proof.

**Method.** `run_3dmark05_perf_probe.sh --suffix indexcache-fastmeasure-frame50-gputrace-r1
--frame 50 --optimize-opaque-depth-index-cache --optimize-opaque-depth-index-cache-min-gain-pct 10
--baseline-joined <current-normal frame50 joined> --target-row-key 50/0 --target-row-key 50/1
--require-opaque-depth-index-cache-proof --timeout 420`, then `finalize_*` with
`--require-top-gpu-decrease --require-top-vs-buffer-write-decrease
--require-top-unexplained-buffer-write-decrease --require-opaque-depth-index-cache-proof`.

**Result.** ACCEPTED — all gates passed (top GPU, top VS write, top unexplained
write, target reordered-cache hits, target cache-opt LRU32 / VS write / VS invocation
decrease). Top GPU `34.664→31.371ms` (`-9.50%`); top VS write `1,627.372→1,518.868MiB`
(`-6.67%`). Target rows `50/0+50/1`: GPU `14.471→11.810ms` (`-18.39%`);
VS write `646.165→537.688MiB` (`-16.79%`); VS invocations `536,583→460,839` (`-14.12%`);
draw/vertex/triangle counts unchanged. `102` reordered cache hits / `96` rejected.
Matched-row attribution: `-92.1MiB` from invocation count vs `-16.4MiB` from
bytes/invocation — **primary mover is invocation count**.

**Verdict.** Accepted production proof. The reordered opaque-depth IB cache reduces
real Xcode GPU work, not just dxmt estimates. After the win, top VS write is still
`1,518.9MiB` (`~7.8x` the `184B` visible VSOut), so hidden vertex/tiler/backend
storage remains the residual owner. Keep opt-in until CPU cost is lower.

**Related.** [[index-cache-locality]] · prev: [[index-cache-locality-opaque.06]]
· [[tvb-mechanism-proof]] (mechanism proven) · [[hidden-backend-storage]] (residual) ·
[[vsout-layout]] (visible width ruled out as first-order owner).
