---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: screenblend
order: 04
title: Explicit LSB1 Screen-Blend Gate
date: 2026-06-05
type: validation
status: accepted
source: scripts/tools/summarize_3dmark05_perf_gates.py; docs/perfomance/index-cache-locality/index-cache-locality-screenblend.03.md; docs/perfomance/mini-replay-bisection/mini-replay-bisection-semantic.01.md
---

# Explicit LSB1 Screen-Blend Gate

**Question / hypothesis.** Can the `50/2` screen-blend index-cache path move the
same hidden TVB / parameter-buffer limiter as opaque-depth locality, and can it
be promoted beyond "profiling-only" without generalizing unsafe depth-read
reorder?

**Method.** Compared the current frame50 baseline against the combined
opaque-depth + screen-blend index-cache run and fed the Xcode scaling,
semantic-image, primitive-conflict, runtime-selector, and class-proxy CSVs into
`summarize_3dmark05_perf_gates.py`. The screen-blend path must clear movement
gates first, then carry an explicit semantic-image policy (`exact` or `lsb1`).
Broad depth-read reorder is judged separately with final-color/final-writer
evidence.

```mermaid
flowchart TD
  Candidate["screen-blend index-cache candidate"] --> Movement{"Xcode movement?"}
  Movement -- "No" --> MissingMove["missing-xcode-movement\nno semantic spend"]
  Movement -- "Yes" --> Image{"semantic image CSV?"}
  Image -- "No" --> MissingImage["missing-semantic-image\nprofiling-only"]
  Image -- "exact / lsb1 policy" --> Explicit["explicit-tolerance-pass"]
  Explicit --> Allowed["allowed only under explicit\nexact/lsb1 run policy"]

  Candidate --> Broad["broad depth-read reorder?"]
  Broad --> FinalColor{"runtime final-color /\nfinal-writer selector?"}
  FinalColor -- "No" --> Reject["reject broad promotion\nruntime-indistinguishable blocker"]
  FinalColor -- "Yes" --> Future["new proof family"]

  classDef good fill:#e8f5e8,stroke:#4d8b4d,color:#102a10
  classDef warn fill:#fff3d6,stroke:#b98222,color:#2a1b00
  classDef bad fill:#ffe8e8,stroke:#b64242,color:#2b0d0d
  class Explicit,Allowed good
  class Candidate,Movement,Image,Broad,FinalColor,Future,MissingImage warn
  class MissingMove,Reject bad
```

**Result.** Combined opaque + screen-blend locality is the strongest measured
stable GPU path in the current corpus: top GPU `-11.89%`, VS write `-13.20%`,
and VS invocations `-12.29%`. Attribution stays invocation-dominant:
`-214.760MiB` VS-write delta decomposes to `-198.917MiB` from invocation count
and only `-15.843MiB` from bytes/invocation. The screen-blend semantic image
gate passes only as an explicit `lsb1` tolerance artifact: `739 / 786,432`
pixels differ, max delta `1`, SSIM `1.000000`.

The same gate report rejects broader depth-read promotion. Visible exact-pass
movement exists (`-8,446` LRU32), but there is also a visible final-color hazard
(`-1,407` LRU32). Runtime-visible state, geometry, shader, and VS/PS constant
hash fields cannot split the known pass/fail group; visible exact draws
`3,5,6,7` and fail draw `4` share all `43` runtime-visible fields. The only
separators are trace-local `vsconsts_hash` and draw-local
`runtime.uniform_payload_hash`, which are overfit debug keys, not production
selectors.

**Verdict.** Accepted only under explicit semantic policy. Opaque-depth locality
remains the production-safe path. Screen-blend locality may be used as an
explicit exact/`lsb1` opt-in or proof artifact, but it must not be generalized
to broad depth-read reorder. The next Xcode budget should target either a real
final-color/final-writer oracle, or a new non-reorder backend mechanism that
preflights meaningful bytes/invocation or hidden-backend proxy movement before
capture.

**Related.** [index-cache-locality](../index-cache-locality.md) · prev: [index-cache-locality-screenblend.03](index-cache-locality-screenblend.03.md)
· [tvb-mechanism-proof](../tvb-mechanism-proof.md) · [primitive-reorder-diagnostics](../primitive-reorder-diagnostics.md) ·
[mini-replay-bisection](../mini-replay-bisection.md) · [hidden-backend-storage](../hidden-backend-storage.md).
