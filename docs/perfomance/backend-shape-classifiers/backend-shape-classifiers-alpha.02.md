---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
subcategory: alpha
order: 02
title: Scoped Screen-Blend Alpha Disable
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L2197-L2222
---

# Scoped Screen-Blend Alpha Disable

**Question / hypothesis.** Narrowing the alpha-blend disable to the
`large4096,screen-blend` class (after blend-signature class filters landed):
does a scoped blend-off move the hidden VS-write bucket without the broad
yellow-frame failure? Still correctness-invalid (the selected screen blend
`InvDestColor,One,Add` is real D3D9 blending, not a no-op).

**Method.** `DXMT9_PROBE_DISABLE_ALPHA_BLEND` scoped to class
`large4096,screen-blend`. Capture/export
`app-d3d9-3dmark05-screen-blend-class-gputrace-r1`, finalize against the
index-scout baseline. Probe applied to only `6` draw calls (`36,411` primitives /
`109,233` vertices), all in `seq=60,enc=2`.

**Result.** Apparent large drop: total GPU `50.832ms -> 25.417ms`, top VS buffer
write `2236.981MiB -> 1054.495MiB`. But the hot-row *set* drifted: shared top
rows are only `60/0`, `60/1`, `60/3`; `60/2` and `60/8` appear only in the probe
capture. Surviving-shape ownership remained vertex-stage dominated: top-3 GPU
still `24.823ms`, VS write still `1054.495MiB`, hidden estimate `1037.143MiB`
(`0.984x` of VS write), dxmt CPU writers only `0.727MiB`, unexplained ratio
`0.999x`, `94.19%` weighted vertex-stage time.

**Verdict.** Rejected as proof. The drop is confounded by hot-row identity drift,
not a strict same-row local result. Evidence that backend shape is sensitive to
scoped blend/pass composition — the next valid blend experiment must preserve the
blend equation and isolate same-row backend shape (row-local replay or locality).

**Related.** [backend-shape-classifiers](index.md) · follows [backend-shape-classifiers-alpha.01](backend-shape-classifiers-alpha.01.md), precedes the precise class-only [backend-shape-classifiers-alpha.03](backend-shape-classifiers-alpha.03.md) · confirms [hidden-backend-storage](../hidden-backend-storage/index.md) · motivates [mini-replay-bisection](../mini-replay-bisection/index.md) row-local approach.
