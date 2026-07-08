---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: screenblend
order: 05
title: Current Gate Requires Reattached Screen-Blend Proof
date: 2026-06-06
type: validation
status: blocked-current-gate
source: traces/app-d3d9-3dmark05-post-streamib-frame60-gate-r1/analysis/frame60-current-perf-gates.csv; traces/app-d3d9-3dmark05-post-streamib-frame60-gate-r1/analysis/frame60-current-next-experiment-queue.csv; docs/perfomance/index-cache-locality/index-cache-locality-screenblend.04.md
---

# Current Gate Requires Reattached Screen-Blend Proof

**Question / hypothesis.** Does the latest frame60 gate still carry enough
evidence to treat screen-blend index locality as an accepted explicit
`exact`/`lsb1` path?

**Method.** Re-read the current post-stream/IB gate report and next-experiment
queue. The current gate was built from frame60 backend/semantic artifacts and
does not include `--screen-blend-semantic-csv`. The historical
[index-cache-locality-screenblend.04](index-cache-locality-screenblend.04.md) result records the `739 / 786,432`
`lsb1` image tolerance, but the current trace tree does not expose that image
comparison CSV as a gate input.

```mermaid
flowchart TD
  Historical["screenblend.04\nhistorical combined run\nGPU -11.89%\nlsb1 image numbers recorded"]
  Current["current frame60 gate\npost-stream/IB inputs"]
  Csv{"screen-blend\nsemantic CSV attached?"}
  Gate{"screen-blend-explicit-tolerance\ngate emitted?"}
  Queue["class proxy rows\nexplicit-tolerance-reorder"]
  Blocked["missing-screenblend-gate-input /\nneeds-screen-blend-gate-input\nnot a current implementation candidate"]
  Accepted["explicit-tolerance-only\nallowed only with carried policy"]

  Historical --> Current
  Current --> Csv
  Csv -- "No" --> Queue
  Queue --> Blocked
  Csv -- "Yes" --> Gate
  Gate -- "exact/lsb1 pass" --> Accepted
  Gate -- "missing/fail" --> Blocked

  classDef good fill:#e8f5e8,stroke:#4d8b4d,color:#102a10
  classDef warn fill:#fff3d6,stroke:#b98222,color:#2a1b00
  classDef bad fill:#ffe8e8,stroke:#b64242,color:#2b0d0d
  class Historical,Current,Csv,Gate,Queue warn
  class Accepted good
  class Blocked bad
```

**Result.** The refreshed `frame60-current-perf-gates.csv` now emits
`screen-blend-explicit-tolerance=missing-screenblend-gate-input`: there are
`8` class-proxy screen-blend rows, but the current VS scaling inputs expose no
screen-blend movement candidate and no semantic image CSV was provided.
`frame60-current-next-experiment-queue.csv` correspondingly marks those rows as
`needs-screen-blend-gate-input`. That is the correct conservative status for
the current evidence set: proxy size and historical movement are not enough
without same-input Xcode movement and semantic image proof attached to the
automated gate.

**Verdict.** Demote screen-blend locality from "currently accepted" to
"historical explicit-tolerance proof, current gate missing movement/proof input." It
remains useful as a mechanism ceiling and as a proof artifact, but it must not
be treated as a production/default candidate or as a current Xcode-spend target
until the exact/`lsb1` image CSV is reattached or regenerated in the same gate
run.

**Related.** [index-cache-locality](../index-cache-locality.md) · prev:
[index-cache-locality-screenblend.04](index-cache-locality-screenblend.04.md) · next:
[index-cache-locality-screenblend.06](index-cache-locality-screenblend.06.md) · [index-cache-locality-proofinput.01](index-cache-locality-proofinput.01.md) ·
[overview-3dmark05-gt1](../overview-3dmark05-gt1.md) ·
[hidden-backend-storage](../hidden-backend-storage.md).
