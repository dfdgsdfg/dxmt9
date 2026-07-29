---
domain: mini-replay-bisection
workload: 3DMark05 GT1
subcategory: semantic
order: 01
title: Final-Color Runtime Blocker
date: 2026-06-05
type: validation
status: rejected
source: scripts/tools/analyze_mini_replay_semantics.py; scripts/tools/summarize_3dmark05_perf_gates.py; docs/perfomance/index-cache-locality/index-cache-locality-screenblend.04.md
---

# Final-Color Runtime Blocker

**Question / hypothesis.** Can broad non-opaque depth-read index reorder be made
production-shaped by a runtime selector that keeps visible exact-pass locality
gain while excluding the known final-color failure?

**Method.** Joined the row `50/2` depth-read/no-blend mini-replay semantic
bisect results with runtime indexed-probe telemetry. The analyzer emitted a
bounded runtime selector sweep, final-color queue, final-writer oracle buckets,
and an all-runtime-visible-fields blocker check. Runtime fields included state,
geometry, shader, runtime VS/PS constant hashes, and full uniform payload hash.

```mermaid
flowchart TD
  Candidate["broad depth-read locality candidate"] --> Semantic["single-draw semantic bisect"]
  Semantic --> Buckets["final-color / final-writer buckets"]
  Buckets --> Pass["visible exact-pass gain\nLRU32 -8446"]
  Buckets --> Fail["visible fail hazard\nLRU32 -1407"]
  Pass --> Selector{"runtime selector\nsplits pass from fail?"}
  Fail --> Selector
  Selector -- "No" --> Blocker["runtime-indistinguishable blocker"]
  Selector -- "Only payload identity" --> Overfit["draw-local overfit\nnot production"]
  Selector -- "Yes" --> Future["future final-color oracle"]
  Blocker --> Reject["reject broad depth-read reorder"]
  Overfit --> Reject

  classDef good fill:#e8f5e8,stroke:#4d8b4d,color:#102a10
  classDef warn fill:#fff3d6,stroke:#b98222,color:#2a1b00
  classDef bad fill:#ffe8e8,stroke:#b64242,color:#2b0d0d
  class Pass,Future good
  class Candidate,Semantic,Buckets,Selector,Overfit warn
  class Fail,Blocker,Reject bad
```

**Result.** The useful movement and the correctness hazard coexist. The semantic
queue has visible exact-pass movement (`-8,446` LRU32) but also visible-fail
movement (`-1,407` LRU32). The final-writer oracle identifies draw `4` as a real
color hazard: primitive order changes final color (`15` primitive-owner pixels,
`3` color pixels). Owner changes alone are not enough to reject, because draws
`2,3,5,6,7` keep `-7,035` LRU32 of owner-change/color-stable movement.

The runtime selector sweep fails to produce a production predicate. Best
runtime-shaped selector `state.index_count` keeps only `33.36%` of visible
exact gain (`-2,818` LRU32) and still leaves a blocked group. More importantly,
visible exact-pass draws `3,5,6,7` and fail draw `4` are identical across all
`43` runtime-visible state/geometry/shader/constant fields currently available.
The only separators are trace-local `vsconsts_hash` and draw-local
`runtime.uniform_payload_hash`; both are proof/debug keys, not predictive
production selectors.

**Verdict.** Rejected for broad promotion. This does not reject post-transform
locality as a mechanism; it rejects broad depth-read reorder without a real
final-color/final-writer or occlusion oracle. Do not implement a
uniform-payload-identity selector, and do not spend another Xcode capture on
this broad reorder family until the selector proof changes.

**Related.** [mini-replay-bisection](index.md) · [index-cache-locality-screenblend.04](../index-cache-locality/index-cache-locality-screenblend.04.md)
· [primitive-reorder-diagnostics](../primitive-reorder-diagnostics/index.md) · [index-cache-locality](../index-cache-locality/index.md) ·
[tvb-mechanism-proof](../tvb-mechanism-proof/index.md).
