---
domain: vsout-layout
workload: 3DMark05 GT1
subcategory: varying
order: 02
title: VSOut Liveness Trim Hypothesis Rejection
date: undated
type: experiment-run
status: rejected
outdated: retired-journal
source: specs/perfomance.plan.md#L15096-L15110
---

# VSOut Liveness Trim Hypothesis Rejection

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** Does an *actual-FS-read* liveness manifest (trimming
`VSOut` to only the fields the paired fragment shader genuinely reads) reduce the
Xcode VS-write bucket where a blanket varying trim did not?

**Method.** Captured the actual-read-set VSOut mini replay (Xcode counters) for the
hot indexed classes. The variant preserved live high texcoords and removed only
genuinely unread fields (`color`, `secondaryColor`, some pair-local texcoords,
`pointSize`), keeping identical vertices and VS invocations.

**Result.**

- `VS Buffer Device Memory Bytes Written`: `347.956 -> 347.924MiB` (`-0.01%`).
- GPU time: `5.658 -> 5.521ms` (treated as too small to prove a root cause).
- Named tiled counters stay tiny vs the VS bucket (`2.375` / `0.688` / `0.375MiB`
  versus `273.554` / `95.517` / `81.328MiB`).
- High texcoords are genuinely live, so they cannot be trimmed.

**Verdict.** Rejected as a production pair-liveness PSO variant. Even an exact
FS-read liveness trim moves the dominant bucket by only `-0.01%`. The measured
bucket is hidden Apple vertex/tiler/backend storage *below* the visible VSOut
layout and below named tiled-buffer counters. Next proof must shift to Metal
compiler/backend spill, hidden VS private scratch, and primitive/binning
parameter storage — not field liveness.

**Related.** [vsout-layout](index.md) · narrowed from [vsout-layout-varying.01](vsout-layout-varying.01.md) · companion semantic-safe replay [vsout-layout-varying.03](vsout-layout-varying.03.md) · [hidden-backend-storage](../hidden-backend-storage/index.md) · [shader-codegen](../shader-codegen/index.md) · [mini-replay-bisection](../mini-replay-bisection/index.md).
