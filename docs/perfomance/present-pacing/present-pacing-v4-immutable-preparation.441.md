---
domain: present-pacing
workload: STALKER CoP capture29
title: "Immutable V4 preparation reused across fresh executions"
type: experiment
status: diagnostic-only
updated: 2026-10-04
source: experiments/output/stkcop-v4-immutable-preparation-24/verification.md; experiments/output/stkcop-v4-immutable-preparation-24/{paired-l0-r1,l0-r1,l3-r1,l64-r1}/result.json
related: docs/perfomance/present-pacing/present-pacing-v4-provider-call-timing.440.md; specs/experiments/harness/replay/spec.md
---

# Immutable preparation, fresh execution

The eager `prepareRenderTapeIdentityV4` factory owns authenticated source/setup
plans and immutable execution templates in stationary PIMPL storage. Named
replay initializes fresh query bindings, local record facts, cursor, Present
state and completion evidence; it borrows only immutable metadata. Runtime
command/root qualification and cancel/flush/retire ordering remain intact.
Legacy/V4 API cross-use rejects; failed preparation cannot publish partial
authority. Native move/isolation/allocation-failure tests cover these contracts.

Native 8/8, x86 9/9 and all 230 existing TLA configurations pass. Both-route
CPU/GPU paired qualification passes. All 54 fresh-device L0/L3/L64 repetitions
preserve original pixels and EVENT readiness with zero GPU errors. Every row
records one immutable preparation, reuse, and one consumed fresh admission.
Executable, loaded images, input and plan match across the three repetition
sets. Reproduce the receipt checks with the artifact directory's `analyze.py`.

## Cost ownership

| Limit | H440 full call ms | Current full call ms | Fresh init ms | Replay ms | Inclusive encode ms |
|---|---:|---:|---:|---:|---:|
| 0 | 569.419 | 290.860 | 0.015 | 290.839 | 9.654 |
| 3 | 543.624 | 265.514 | 0.015 | 265.493 | 22.206 |
| 64 | 537.538 | 258.014 | 0.015 | 257.995 | 11.062 |

Current medians use 16 measurements after two warm-ups. About 280 ms of
immutable work now occurs once, not on every invocation. Whole eager factory
cost is 4.55–4.57 seconds, including tape/blob ownership and General preflight;
the V4 component is 279–282 ms. Final prepared-owner destruction is outside
both factory and per-call clocks. The historical H440 image differs, policy
order is fixed and background/cache effects are uncontrolled: this is evidence
of amortized harness preparation, not a same-binary FPS improvement or policy
ranking. Timing schemas are versioned to distinguish changed scopes.

Resource setup remains 142–145 ms and chunk replay 32–37 ms; both include
synchronous drains. L3 still has more inclusive encode time despite less full
provider wall. Do not add these overlapping scopes or identify one as the
production bottleneck. Next attribution should separate resource bootstrap,
uploads and their drains before prioritizing encode parallelism.

Warm-device reset, complete scalar/uniform semantics, numeric occlusion,
future backing preservation and exclusive encoder/wait attribution remain
open. This EVENT-only capture does not discharge those gates. No runtime
policy or game-FPS result is promoted.
