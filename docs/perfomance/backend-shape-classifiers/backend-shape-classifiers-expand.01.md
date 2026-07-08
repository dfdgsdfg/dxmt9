---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
subcategory: expand
order: 01
title: Force-Expand-Indexed Probe
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L9116-L9228
---

# Force-Expand-Indexed Probe

**Question / hypothesis.** Primitive-pressure classifier: does the hidden
VS-buffer-write bucket react to vertex/primitive *submission* pressure? Expand
indexed draws into flat transient vertex data, removing index reuse from the
Metal submission path. Intentionally not an optimization (very expensive).

**Method.** `DXMT_FORCE_EXPAND_INDEXED=1` (wrapper `--force-expand-indexed`),
`--frame 60 --measure-index-reuse --top 4 --hot-gpu-share 95 --timeout 240`,
finalized vs `measure-index-cache-gputrace-r1` with coverage/PSO gates.

**Result.**

| Metric | Baseline | Force expand indexed | Delta |
|---|---:|---:|---:|
| Total GPU time | `34.391ms` | `64.565ms` | `+87.74%` |
| Hot VS buffer write | `1472.747MiB` | `2917.457MiB` | `+98.10%` |
| Hot VS B / invocation | `856.265B` | `1425.381B` | `+66.46%` |
| Hot VS / expected VSOut | `4.654x` | `7.747x` | `+66.46%` |
| Hot stream handle changes | `830` | `437` | `-47.35%` |
| Hot IB handle changes | `614` | `327` | `-46.74%` |
| Hot transient expanded vertex | `0.000MiB` | `85.892MiB` | `+85.9MiB` |

VS invocations / dxmt vertex `= 1.000x`. Transient expanded vertex bytes explain
only ~3% of Xcode buffer writes; hidden estimate rose to `2785.497MiB`
(`0.955x` of VS write). Top-row set drifted (shared `60/0`, `60/1` only), so it
is not a clean correctness-preserving A/B.

**Verdict.** Rejected as an optimization — but **decisive and informative**.
Removing indexed submission/cache behavior makes the hidden VS-write bucket
*much worse* (`+98.10%`) even though stream/IB churn *decreases*. The owner is
therefore GPU-side vertex/primitive backend behavior, not CPU state/bind churn.
**Indexed submission and vertex reuse are mandatory; never expand as an
optimization.** This confirms primitive/indexed-submission pressure is an active
first-order classifier.

**Related.** [backend-shape-classifiers](index.md) · directly followed [backend-shape-classifiers-cull.03](backend-shape-classifiers-cull.03.md) · the strongest confirmation of [hidden-backend-storage](../hidden-backend-storage/index.md) scaling · motivates the accepted [index-cache-locality](../index-cache-locality/index.md) win (reduce VS invocations within the indexed path) · related [index-reuse-measurement](../index-reuse-measurement/index.md).
