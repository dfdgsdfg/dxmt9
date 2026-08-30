---
workload: 3DMark05 GT2
date: 2026-08-31
title: Mutation-composition observer matched runtime gate
status: retracted-pending-remeasurement
source: experiments/output/app-d3d9-3dmark05-task6-observer-off-info-r1-20260831; experiments/output/app-d3d9-3dmark05-task6-observer-on-info-r1-20260831
---

# Mutation-composition observer matched runtime gate

The same current build ran the GT2 `perf` lane once with the observer disabled
and once enabled. Both launches used `DXMT_LOG_LEVEL=info`; the enabled launch
changed only `DXMT9_MUTATION_COMPOSITION_OBSERVER=1`.

- off: `experiments/output/app-d3d9-3dmark05-task6-observer-off-info-r1-20260831`
- on: `experiments/output/app-d3d9-3dmark05-task6-observer-on-info-r1-20260831`

Both runs passed and produced benchmark artifacts. They encoded 1,757 and
1,683 Presents respectively; the single-run process elapsed times (72.14 s and
71.57 s) are descriptive only and are not an FPS claim.

The enabled run emitted one finalized report per 60 Presents. Every window
reported `candidate_calls=0`, `candidate_cpu_ms_per_present=0`, `pending=0`,
and `invalid_or_dropped=0`; provisional and final completion rejection counts
also remained zero. That zero-candidate result is now marked unreliable: sync
mutations used a private CPU ordinal while deferred/replay observations used
`replaySeq`, so valid mixed-path adjacency could be rejected as `SourceOrder`.
The observer now assigns one typed, generation-qualified ordering identity at
ingress and native/TLA coverage exercises both sync→deferred and
deferred→sync. The busiest first window observed 664 mutations, of which 661
reached a first GPU use and three were zero-use generations, but the economic
claim must be remeasured after this correction.

Therefore the R-BACK-44.10 economic gate is **not decided** by this GT2 sample;
the prior `<0.2 ms/Present` closure is retracted pending a matched rerun.
Mutation composition remains forbidden, and the evidence does not justify
implementing fusion. The earlier staging failure was an
environment-path issue: the PE compilers live under
`/Users/dididi/opt/llvm-mingw/bin`, while the host archive tool must remain
available from the Nix profile.
