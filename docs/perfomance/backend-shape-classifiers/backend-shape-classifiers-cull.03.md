---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
subcategory: cull
order: 03
title: Force-Cull-Back Probe
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L9007-L9098
---

# Force-Cull-Back Probe

**Question / hypothesis.** Beyond removing cull (`--disable-cull`), does cull
*orientation* own the hidden bucket? Force every hot draw to back-cull and check
whether VS write tracks orientation.

**Method.** `DXMT_DEBUG_FORCE_CULL_MODE=back` (wrapper `--force-cull-mode back`),
`--frame 60 --measure-index-reuse --top 4 --hot-gpu-share 95`, finalized vs
`measure-index-cache-gputrace-r1` with Xcode/dxmt coverage + top-PSO gates.
Hot set `60/3, 60/4, 60/1, 60/0` (98.29% GPU); all `716` hot draws became back-cull
(`cull none/front/back = 0/0/716`).

**Result.**

| Metric | Baseline | Force cull back | Delta |
|---|---:|---:|---:|
| Total GPU time | `34.391ms` | `34.844ms` | `+1.32%` |
| Hot-set GPU time | `33.741ms` | `34.247ms` | `+1.50%` |
| Hot-set VS buffer write | `1472.747MiB` | `1472.850MiB` | `+0.01%` |
| Hot-set VS B / invocation | `856.265B` | `856.193B` | `-0.01%` |
| Hot-set hidden backend estimate | — | `1455.326MiB` | `0.988x` of VS write |

Per-row VS write stayed within ±0.03% on `60/3/4/1/0`.

**Verdict.** Rejected. Forcing all hot draws to back-cull does not move the
hidden VS-write bucket. Together with [backend-shape-classifiers-cull.01](backend-shape-classifiers-cull.01.md) and
`.02`, broad cull state and cull orientation are rejected together — the next
GPU probe should be primitive-pressure / backend-storage oriented, not another
cull toggle.

**Related.** [backend-shape-classifiers](index.md) · follows [backend-shape-classifiers-cull.02](backend-shape-classifiers-cull.02.md), precedes the scoped [backend-shape-classifiers-cull.04](backend-shape-classifiers-cull.04.md) · directly motivated [backend-shape-classifiers-expand.01](backend-shape-classifiers-expand.01.md) · confirms [hidden-backend-storage](../hidden-backend-storage/index.md).
