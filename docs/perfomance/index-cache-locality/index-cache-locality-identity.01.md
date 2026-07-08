---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: identity
order: 01
title: No-Mutate Indexed Draw Identity Scout
date: 2026-06-03
type: scout
status: tooling
source: specs/perfomance.plan.md#L13515-L13590
---

# No-Mutate Indexed Draw Identity Scout

**Question / hypothesis.** Cache-aware reorder runs perturbed hot-row shape and
invalidated the locality signal. Can we record per-draw identity (handles, PSO,
shaders, state) *without* mutating index order, to choose row-local replay candidates?

**Method.** `DXMT9_MEASURE_INDEX_REUSE=1` now emits `dxmt9-perf-indexed-probe-draw`
rows even with no mutating probe active (all probe flags `0`), when encoder breakdown
is on. `run_3dmark05_perf_probe.sh --suffix current-head-index-scout-r2 --frame 60
--no-gputrace --encoder-breakdown-seq 60 --measure-index-reuse --top 3 --hot-gpu-share 95
--timeout 180`.

**Result.** Pass, `664` indexed draw identity rows. Each row carries index locality,
stream0 span, index/stream0 handles, stream0 offset/stride, PSO handle, shader variant,
VS/PS hashes, VSOut key, and depth/blend/scissor/cull state. Row shape drifted vs the
latest Xcode current-head frame60: scout `60/0=135 draws / 179,613 tris`,
`60/1=212 / 320,499`, `60/2=307 / 406,591` vs Xcode `42 / 97,294`, `156 / 228,725`,
`187 / 389,376`.

**Verdict.** Tooling (data gap closed). The instrumentation is the new evidence, not
row ownership — the no-gputrace shape is not yet a valid substitute for the Xcode
frame. Next pass must be a gputrace-backed no-mutate scout on the same frame as the
Xcode counters.

**Related.** [index-cache-locality](index.md) · next: [index-cache-locality-identity.02](index-cache-locality-identity.02.md)
· [index-reuse-measurement](../index-reuse-measurement/index.md) (the `DXMT9_MEASURE_INDEX_REUSE` family) ·
[mini-replay-bisection](../mini-replay-bisection/index.md) (consumes the identity rows) · [primitive-reorder-diagnostics](../primitive-reorder-diagnostics/index.md)
(the reorder that perturbed shape).
