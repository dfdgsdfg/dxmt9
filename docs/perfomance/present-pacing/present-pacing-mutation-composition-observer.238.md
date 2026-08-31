---
workload: 3DMark05 GT2
date: 2026-08-31
title: Mutation-composition observer matched runtime gate
status: measured-closed
source: experiments/output/app-d3d9-3dmark05-mutation-composition-observer-off-gt2-20260831; experiments/output/app-d3d9-3dmark05-mutation-composition-observer-on-gt2-20260831
---

# Mutation-composition observer matched runtime gate

The same current build ran the GT2 `perf` lane once with the observer disabled
and once enabled. Both launches used `DXMT9_MANAGED_MUTATION_OFFLOAD=1` and
`DXMT_LOG_LEVEL=info`; the enabled launch changed only
`DXMT9_MUTATION_COMPOSITION_OBSERVER=1`.

- off: `experiments/output/app-d3d9-3dmark05-mutation-composition-observer-off-gt2-20260831`
- on: `experiments/output/app-d3d9-3dmark05-mutation-composition-observer-on-gt2-20260831`

Both runs passed with zero command-chunk rejects and zero GPU command-buffer
errors. They encoded 1,719 and 1,601 Presents respectively; the single-run
process elapsed times (73.05 s and 72.13 s) are descriptive only and are not an
FPS claim.

The corrected enabled run emitted one finalized report per 60 Presents. The
strict parser accepted 26 complete windows covering 1,560 Presents and 1,221
mutations. Conservation passed with `candidate_calls=0`,
`candidate_bytes=0`, `candidate_cpu_time_saved_ns=0`, and no invalid/dropped
residual. The production observer now uses one typed, generation-qualified
ordering identity at ingress; native/TLA coverage pins both sync→deferred and
deferred→sync ordering instead of comparing private CPU ordinals with replay
sequence IDs.

The finalized observer lines pass
`scripts/check/audit_mutation_composition_report.py --json`.
Each 60-Present line must preserve mutation bytes, raw candidate CPU time,
first-use distance total/max, typed barriers, all provisional and final
rejection reasons, completion/failure/discarded/pending conservation,
overflow/invalid drops, and weighted window Presents. The parser computes
`sum(candidate_cpu_time_saved_ns) / sum(window_presents) / 1e6`; `<0.2
ms/Present` is **closed**, `>=0.5 ms/Present` is **open** for a candidate-only
follow-up, and `0.2–<0.5` is **inconclusive**. A single matched off/on pair is
valid for screening and parser/conservation checks, but not sufficient for a
performance decision: repeat valid matched pairs under the 3DMark experiment
rules, with the same runtime/build and no concurrent workload.

Therefore this GT2 screening sample closes R-BACK-44.10 at `0.0 ms/Present`:
there is no production candidate population worth implementing. Mutation
composition remains forbidden. This is a workload-local economic decision,
not a claim that the observer proves unbounded ordering or other workloads.
