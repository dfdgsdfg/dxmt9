---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: opaque
order: 08
title: Refreshed Gate Covers Opaque-Depth Proof Input
date: 2026-06-06
type: validation
status: covered-current-gate
source: traces/app-d3d9-3dmark05-post-streamib-frame60-gate-r1/analysis/frame60-current-perf-gates.csv; traces/app-d3d9-3dmark05-post-streamib-frame60-gate-r1/analysis/frame60-current-next-experiment-queue.csv; traces/app-d3d9-3dmark05-post-streamib-frame60-gate-r1/analysis/frame60-vs-scaling-with-opaque-proof-aggregate.csv; traces/app-d3d9-3dmark05-post-streamib-frame60-gate-r1/analysis/frame60-vs-scaling-with-opaque-proof-delta.csv; traces/app-d3d9-3dmark05-post-streamib-frame60-opaque-proof-r1/analysis/frame60-xcode-dxmt-comparison.md
---

# Refreshed Gate Covers Opaque-Depth Proof Input

**Question / hypothesis.** Does the latest frame60 class-proxy queue prove that
the opaque-depth production path should be promoted for the newly ranked rows,
or does it only show candidate ceiling until the production proof input is
attached?

**Method.** Re-read the post-stream/IB gate report and next-experiment queue,
then regenerate the VS scaling input with the refreshed opaque proof run
`post-streamib-frame60-opaque-proof-r1` included. The historical
index-cache-locality-opaque.07 proof remains accepted: the fast-measure
frame50 run passed `--require-opaque-depth-index-cache-proof`. The refreshed
frame60 proof is recorded in index-cache-locality-proofinput.01.

```mermaid
flowchart TD
  Historical["opaque.07\naccepted frame50 proof\nGPU -9.50%\ntarget VS inv -14.12%"]
  Current["current frame60 gate\npost-stream/IB inputs"]
  Proxy["class-proxy rows\nproduction-opaque-reorder"]
  Proof{"opaque-depth Xcode\nproof input attached?"}
  Missing["old gate\nmissing-production-gate-input"]
  OpaqueProof["opaque proofinput.01\n60/0+60/1 proof passed"]
  Keep["production-locality=keep\ncovered-production-path"]

  Historical --> Current
  Current --> Proxy
  Proxy --> Proof
  Proof -- "No" --> Missing
  Missing --> OpaqueProof
  OpaqueProof --> Proof
  Proof -- "Yes; gates pass" --> Keep

  classDef good fill:#e8f5e8,stroke:#4d8b4d,color:#102a10
  classDef warn fill:#fff3d6,stroke:#b98222,color:#2a1b00
  classDef bad fill:#ffe8e8,stroke:#b64242,color:#2b0d0d
  class Historical,Current,Proxy,Proof warn
  class OpaqueProof,Keep good
  class Missing bad
```

**Result.** The earlier `frame60-current-perf-gates.csv` emitted
`production-locality=missing-production-gate-input` because the VS scaling set
did not include an opaque-depth production proof run. After adding the refreshed
opaque proof input, the current gate now emits `production-locality=keep`:
`post-streamib-frame60-opaque-proof-r1` improves top GPU `-3.31%` and VS
invocations `-6.43%`. The next-experiment queue correspondingly marks the
opaque-depth rows as `covered-production-path`.

**Verdict.** The input scope gap is closed for opaque-depth locality. Keep the
production-shaped opaque opt-in as the accepted GPU win, and do not spend
another trace on threshold-only opaque retests. The remaining unresolved proof
input is screen-blend movement plus same-input exact/`lsb1` semantic image
evidence.

**Related.** [index-cache-locality](index.md) · prev:
index-cache-locality-opaque.07 · index-cache-locality-proofinput.01 ·
[index-cache-locality-screenblend.05](index-cache-locality-screenblend.05.md) ·
[overview-3dmark05-gt1](../overview-3dmark05-gt1.md).
