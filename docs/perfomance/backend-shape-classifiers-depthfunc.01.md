---
domain: backend-shape-classifiers
subcategory: depthfunc
order: 01
title: Depth-Func-Always Probe
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L8916-L8975
---

# Depth-Func-Always Probe

**Question / hypothesis.** Separating depth-compare shape from depth-write:
keep depth enable/write state but force the Metal depth compare function to
`Always`. Does depth-compare/backend visibility own the hidden VS-write bucket?
Correctness-invalid for depth-dependent frames.

**Method.** `DXMT9_PROBE_DEPTH_FUNC_ALWAYS=1` (wrapper `--probe-depth-func-always`),
`--frame 60 --encoder-breakdown-seq 60 --dump-shaders`, finalized vs
`current-normal-gputrace-r1` with `--require-xcode-counter-coverage
--require-dxmt-join-coverage --require-top-pso-attribution
--require-shader-dump-matches`. All finalizer gates passed.

**Result.**

| Metric | Normal | Depth func always | Delta |
|---|---:|---:|---:|
| Total GPU time | `35.456ms` | `37.195ms` | `+4.90%` |
| Top-3 GPU time | `34.837ms` | `36.590ms` | `+5.03%` |
| Top-3 VS buffer write | `1627.240MiB` | `1627.281MiB` | `+0.041MiB` |
| Top-3 VS B / invocation | `1447.741B` | `1447.778B` | `+0.036B` |
| Top depth write | `3.815MiB` | `3.699MiB` | `-3.03%` |

VS invocations and named tiled buffers unchanged in top rows (`60/2`, `60/1`, `60/0`).

**Verdict.** Rejected. Depth compare/backend visibility is not the hidden
VS-buffer-write owner — VS write effectively unchanged, GPU time regresses, and
the small depth-write delta is far too small to explain the ~1.6 GiB bucket.
Keep in the rejected state-shape set.

**Related.** [[backend-shape-classifiers]] · companion to [[backend-shape-classifiers-depthwrite.01]] · confirms [[hidden-backend-storage]] · related [[mini-replay-bisection]] depth probes.
