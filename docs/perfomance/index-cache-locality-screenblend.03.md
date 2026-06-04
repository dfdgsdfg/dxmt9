---
domain: index-cache-locality
subcategory: screenblend
order: 03
title: Layout-Stride Screen-Blend Cache Xcode Replay
date: 2026-06-04
type: validation
status: inconclusive
source: specs/perfomance.plan.md#L1377-L1469
---

# Layout-Stride Screen-Blend Cache Xcode Replay

**Question / hypothesis.** Does the screen-blend index-cache produce a real Xcode
mechanism reduction on row `50/2` — fewer VS invocations, lower hidden VS write —
even though it is not production-safe without an image/semantic oracle?

**Method.** `run_3dmark05_perf_probe.sh --suffix layoutstride-screenblend-index-cache-frame50-gputrace-r1
--frame 50 --optimize-screen-blend-index-cache --optimize-screen-blend-index-cache-min-gain-pct 10
--measure-index-cache-opt-candidate --baseline-joined <layoutstride frame50 joined>
--target-row-key 50/2 --require-stable-frame-proof --require-target-index-cache-opt-miss32-decrease
--require-target-reordered-index-cache-hits --require-target-vs-buffer-write-decrease
--require-target-vs-invocations-decrease --timeout 420`.

**Result.** All requested gates passed. Target `50/2`: GPU `19.669→18.757ms`
(`-4.64%`); VS write `981.177→874.782MiB` (`-10.84%`); VS invocations `642,001→572,933`
(`-10.76%`); draw/vertex/triangle shape stable; `103` lookups / `66` hits / `37` rejected.
VS-write attribution: `-106.395MiB` total, `-105.507MiB` from invocation count vs
`-0.888MiB` from bytes/inv — **primary mover = invocations**. Top VS write
`1,627.287→1,520.885MiB` (`-6.54%`); residual hidden backend still `~1,493.878MiB`
(`~1.49GiB`). Non-target GPU moved `+0.661ms` (`+4.61%`) — treat as replay variance.

**Verdict.** Inconclusive for production — strong mechanism proof, NOT production-safe.
Confirms hidden TVB storage is sensitive to post-transform locality, but screen-blend
is destination-dependent; promotion needs an explicit semantic image proof or accepted
tolerance policy for the affected rows.

**Related.** [[index-cache-locality]] · prev: [[index-cache-locality-screenblend.02]]
· [[tvb-mechanism-proof]] (same scaling law) · [[index-cache-locality-triage.01]]
(50/2 owner triage) · [[hidden-backend-storage]] (residual).
