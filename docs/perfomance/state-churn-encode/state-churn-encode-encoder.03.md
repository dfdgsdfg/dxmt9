---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: encoder
order: 03
title: Label-Join Xcode Validation
date: 2026-06-01
type: validation
status: accepted
outdated: retired-journal
source: specs/perfomance.plan.md#L5284-L5360
---

# Label-Join Xcode Validation

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** Can the per-encoder dxmt attribution be joined to
Xcode GPU counters directly (no row-order guessing), and does that prove the
CPU-side writers do NOT explain the top Xcode buffer-write bucket?

**Method.** `frame60` gputrace with `DXMT9_PERF_ENCODER_BREAKDOWN=1` and
`DXMT_DISABLE_AUTO_EXPAND_INDEXED=1`. Render-encoder labels now carry
`RenderPass[seq=60,enc=N,rt=...,depth=...]`, joined to
`[dxmt9-perf-encoder seq=60 encoder=N]`. Output:
`traces/app-d3d9-3dmark05-20260601-label-join-frame60/analysis/frame60-xcode-dxmt-joined-summary.csv`.

**Result.** Xcode: `34.05ms` GPU, 4 CBs, 10 encoders, 396 draws, 2,146,296
vertices. Top three encoders = `33.505ms` / `98.41%` and ~`1.63GiB` buffer
writes:
- `seq=60 enc=2`: `19.724ms`, `981.2MiB` write; 187 draws, 271 stream-handle / 160 IB-handle changes, `163320` argbuf cbuf bytes, 0 transient.
- `seq=60 enc=1`: `8.516ms`, `421.4MiB`; 156 draws, 129/129 handle changes, `111480` cbuf bytes.
- `seq=60 enc=0`: `5.265ms`, `225.4MiB`; 42 draws, 36/36 handle changes, `175064` cbuf bytes.

Combined dxmt argbuf cbuf for the top three ≈ `450KiB`; transient vertex/index = 0.

**Verdict.** Accepted (as proof of disjointness). The per-encoder breakdown now
proves the distinction: measured CPU/upload bytes (~450KiB) cannot explain the
~1.63GiB top-three Xcode buffer-write traffic. The top GPU cost is GPU-side
render-pass/device-memory write pressure; stream/IB handle churn is the *coupled*
backend batching problem, not the GPU-write owner.

**Related.** [state-churn-encode](index.md) · prev: [state-churn-encode-encoder.02](state-churn-encode-encoder.02.md) ·
[hidden-backend-storage](../hidden-backend-storage/index.md) (the unexplained ~1.63GiB VS-write bucket) ·
[state-churn-encode-expand.02](state-churn-encode-expand.02.md) (same-frame no-auto-expand validation) ·
[state-churn-encode-batch.01](state-churn-encode-batch.01.md) (later recheck on the same frame).
