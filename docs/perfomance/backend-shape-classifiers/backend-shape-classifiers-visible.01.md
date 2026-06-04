---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
subcategory: visible
order: 01
title: Force-Visible Render-State Probe
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L7528-L7594
---

# Force-Visible Render-State Probe

**Question / hypothesis.** Are the hidden vertex-stage writes coupled to fragment
visibility? Force visibility/blend/write-mask behavior and check whether the VS
bucket follows. Diagnostic only, not correctness-preserving.

**Method.** `DXMT_DEBUG_FORCE_VISIBLE=1`, rerun as
`force-visible-frame60-gputrace-r2` (first attempt discarded). Finalized vs the
`drop-vsout-point-size-gputrace-r1` baseline with `--require-top-pso-attribution
--require-xcode-counter-coverage --require-dxmt-join-coverage
--require-shader-dump-matches`.

**Result.**

| Metric | Drop point_size baseline | Force visible | Delta |
|---|---:|---:|---:|
| Total GPU | `34.624ms` | `36.398ms` | `+5.13%` |
| Top-3 GPU time | `34.085ms` | `35.844ms` | `+5.16%` |
| Top-3 VS buffer write | `1627.311MiB` | `1627.353MiB` | `+0.042MiB` |
| Top-3 unexplained write | `1627.607MiB` | `1627.648MiB` | `+0.041MiB` |
| Top-3 depth write | `3.650MiB` | `1.188MiB` | `-67.47%` |
| Top-3 draws / verts / tris | `385 / 2,146,185 / 715,395` | unchanged | — |

**Verdict.** Rejected. Forcing visibility does not reduce the hidden VS-write
bucket — the main effect is GPU-time regression plus a large depth-write drop
that is too small to matter against the ~1.63 GiB bucket. Hidden traffic is not
tied to fragment visibility. The surviving target is Apple vertex-stage/backend
storage that scales with submitted indexed primitive work, not visibility.

**Related.** [[backend-shape-classifiers]] · used the [[vsout-layout]] drop-point-size baseline · confirms [[hidden-backend-storage]] scales with primitives · directly motivated the [[shader-codegen]] offline-Metal-codegen inspection · related [[backend-shape-classifiers-depthwrite.01]].
