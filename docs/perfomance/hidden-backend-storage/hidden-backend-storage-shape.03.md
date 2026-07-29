---
domain: hidden-backend-storage
workload: 3DMark05 GT1
subcategory: shape
order: 03
title: 50/2 Next Xcode Spend Gate
date: 2026-06-06
type: validation
status: accepted
source: docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.02.md; docs/perfomance/index-cache-locality/index-cache-locality-screenblend.04.md; docs/perfomance/mini-replay-bisection/mini-replay-bisection-semantic.01.md; scripts/tools/run_3dmark05_perf_probe.sh; scripts/tools/finalize_3dmark05_perf_probe.sh
---

# 50/2 Next Xcode Spend Gate

**Question / hypothesis.** After the accepted opaque-depth locality win and the
rejected CPU-only index-cache probes, which candidates still justify an expensive
frame50 `.gputrace` / Xcode encoder-counter export for the residual `50/2`
hidden-backend owner?

**Method.** Treat Xcode capture budget as a scarce GPU-mechanism proof. A
candidate must first identify which class it belongs to, then pass the cheapest
available preflight. CPU-only frontier changes are no-gputrace experiments until
they prove a net CPU win. Primitive-order changes in depth-read rows need a
semantic oracle. Non-reorder backend-shape changes need a clear bytes-per-
invocation rationale before replay.

```mermaid
flowchart TD
  Candidate["new 50/2 or index-locality candidate"] --> Classify{"candidate class?"}

  Classify --> CPU["CPU-only index builder/frontier"]
  CPU --> CpuPre["no-gputrace timeout smoke\nperf counters only"]
  CpuPre --> CpuWin{"net CPU win\nand quality stable?"}
  CpuWin -- "No" --> RejectCPU["reject; no Xcode spend"]
  CpuWin -- "Yes" --> KeepCPU["keep as CPU optimization\nstill no GPU mechanism claim"]

  Classify --> Reorder["primitive-order / index-locality change"]
  Reorder --> Semantic{"semantic proof?\nexact / lsb1 / final-writer"}
  Semantic -- "No" --> RejectReorder["reject broad depth-read\nno Xcode spend"]
  Semantic -- "Yes" --> Xcode["frame50 gputrace + Xcode counters"]

  Classify --> Backend["non-reorder backend-shape change"]
  Backend --> Stable{"row key, draws,\ntriangles stable?"}
  Stable -- "No" --> RejectBackend["reject as shape drift"]
  Stable -- "Yes" --> Bytes{"credible bytes/inv\nmechanism >= gate?"}
  Bytes -- "No" --> RejectBackend
  Bytes -- "Yes" --> Xcode

  Xcode --> Finalize["finalize_3dmark05_perf_probe.sh\njoined Xcode/dxmt report"]
  Finalize --> Gate{"GPU time and\nVS write gate pass?"}
  Gate -- "No" --> RejectXcode["reject mechanism"]
  Gate -- "Yes" --> Promote["document accepted lever"]

  classDef good fill:#e8f5e8,stroke:#4d8b4d,color:#102a10
  classDef warn fill:#fff3d6,stroke:#b98222,color:#2a1b00
  classDef bad fill:#ffe8e8,stroke:#b64242,color:#2b0d0d
  class KeepCPU,Promote good
  class Candidate,Classify,CPU,CpuPre,CpuWin,Reorder,Semantic,Backend,Stable,Bytes,Xcode,Finalize,Gate warn
  class RejectCPU,RejectReorder,RejectBackend,RejectXcode bad
```

**Current decisions.**

| Candidate family | Current status | Xcode policy |
|---|---|---|
| Opaque-depth index-cache locality | accepted GPU mechanism; still opt-in because CPU side-effect remains | Xcode only for regression/proof refresh |
| Screen-blend `50/2` locality | useful only with explicit exact/`lsb1` image policy | Xcode allowed only when the run carries that semantic policy |
| Broad depth-read reorder | blocked by runtime-indistinguishable final-color hazard | no Xcode until final-color/final-writer proof exists |
| Fixed frontier cap | rejected: slots fall, select CPU rises | no Xcode |
| Generic heap-backed lazy frontier | rejected: scored work falls, CPU and miss32 regress | no Xcode |
| Cached-vertex-count bucketed selector | rejected: scored work falls, bucket maintenance regresses CPU | no Xcode |
| Unique upper-bound candidate pre-gate | rejected: rejects 76 candidates but candidate CPU regresses | no Xcode |
| Half VSOut / visible width | rejected: bytes/inv moves weakly, GPU regresses | no Xcode repeat |
| Texture-white / fragment material | rejected as first-order owner for `50/2` | no Xcode repeat |
| New non-reorder backend-shape idea | open | must pass stable-row and bytes/inv preflight before Xcode |

**Verdict.** Accepted as the current spend gate. The next high-value GPU work is
not another generic candidate-selector experiment. It is either:

- a semantic proof that legally expands the `50/2` locality set, or
- a primitive-order-preserving backend-shape mechanism with a credible
  bytes-per-invocation preflight.

Everything else should stay in no-gputrace counter runs until it changes CPU
wall time, quality counters, or a row-local semantic oracle.

**Related.** [hidden-backend-storage](index.md) · prev:
[hidden-backend-storage-shape.02](hidden-backend-storage-shape.02.md) · [index-cache-locality](../index-cache-locality/index.md) ·
[index-cache-locality-screenblend.04](../index-cache-locality/index-cache-locality-screenblend.04.md) · [mini-replay-bisection-semantic.01](../mini-replay-bisection/mini-replay-bisection-semantic.01.md).
