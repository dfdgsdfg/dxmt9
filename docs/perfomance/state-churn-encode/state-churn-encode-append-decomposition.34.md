---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 34
title: buildSparseState Phase Split + touchConstShadow Bulk Path — Flat Attribution, Call-Volume Verdict
date: 2026-08-21
type: experiment-run
status: accepted-mechanism
source: experiments/output/app-d3d9-3dmark05-sparse-phase-r1; experiments/output/app-d3d9-3dmark05--pe-decim-now; experiments/output/app-d3d9-3dmark05--reverify-cand; experiments/output/app-d3d9-3dmark05--cadence-cand-b
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.32.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.33.md
---

# buildSparseState Phase Split + touchConstShadow Bulk Path — Flat Attribution, Call-Volume Verdict

Two increments landed (`f2c42492` phase probe, `1bd88232`+`278e8185`
touchConstShadow), then one GT2 decimation run (`N=64`, info logging — **not a
valid fps sample**) attributed both. Baselines are the three surviving
pre-change decimated runs cited in `source:`.

**1. buildSparseState attribution: no owner.** The seven parent-gated phases
(calibrated, null 187 ns/sample) split the state build's ~300-400 ns/call as:

| phase | cal ns/call | share |
|---|---:|---:|
| clip+TSS/sampler/transform+lights | 113 | 28% |
| textures+streams | 104 | 26% |
| remainder (validate/payload/header) | 51 | 13% |
| shaders+vertex input+IB | 46 | 11% |
| render states | 37 | 9% |
| constants drain | 29 | 7% |
| attachments+scalars | 23 | 6% |

Cross-check: the calibrated phase sum (403 ns/call) is consistent with the
un-instrumented parent from the three baselines (`draw_packet` cal 298-374
ns/call, 0.51-0.64 ms/present at ~1705 calls) — the probe is healthy, and the
parent's absolute reading in the instrumented run itself (raw 3490 ns/call) is
expected to be unusable: seven nested clock pairs ride inside it. **Correction
to the ledger: the `~0.23 ms/present` figure this series carried for
buildSparseState was stale — the un-instrumented parent is 0.5-0.6
ms/present.** The distribution is flat because every delta draw walks every
section (fixed-slot mask loops and tables) regardless of what is pending. The
only remaining optimization shape is whole-section early-skips
(mask==0/empty-table fast-outs); ceiling if halved ≈ 0.3 ms/present on a
saturated game thread — one small mechanical candidate, not a frontier.

**2. touchConstShadow: mechanism landed, honest counter verdict is
"small-to-inconclusive", and the real finding is call volume.** The bulk-span
memcmp early-out plus capacity pre-reservation (`278e8185`) preserves exact
dirty semantics — including the zero-fill-aliasing boundary where a first-time
all-zero set into a freshly extended region stays clean, now pinned by
`testTouchConstShadowSemantics()` (`1bd88232`) — and `reserve()` rather than
`resize()` because `values.size()` is load-bearing: stateblock capture copies
the vector wholesale and replay derives its restore register count from
`size()` (a literal pre-size would have been an observable behavior change).
Measured: `const_setter` cal 20-24 → 17 ns/call (0.42-0.52 → 0.37
ms/present). The delta is 3-7 ns against a 187 ns instrument — direction
consistent, magnitude below the instrument floor, so no win is claimed. The
bucket split explains why: **88% of the 21,700 setter calls/present are 3-4
register sets** (`const_setter_n3_4`) — matrix uploads whose data changes, so
the redundant-set fast path rarely fires and the per-element loop was already
near-minimal (~18-24 ns real). touchConstShadow's ~0.4 ms/present is **call
volume, not per-call inefficiency**; the remaining lever there is the PE entry
layer around it (`entry_const` cal 52-89 ns/call, ~1.1-1.9 ms/present — the
wrapper costs ~3x the touch itself), not the touch.

An `entry_const` drop across the same runs (1.75-1.94 → 1.12 ms/present) is
noted but **not claimed**: it is a cross-day single comparison and nothing in
the change plausibly explains its size; treat it as an observation requiring a
same-day A/B before any use.

**Gates.** Host suite 727/727 at every step; `sparse-phase-r1`
`status=pass`, zero GPU errors; disabled-path cost of the probe is zero clock
reads (parent-gated), and the production change leaves the decimation
instrument block byte-identical. Cold-server conformance: 249/254 with the
first pass, where the 5 fails were the known `visual_process_vertices_xyzhw`
plus the four-test decl group — the decl group passed 5/5 on an immediate
scoped rerun (`--start 26 --end 31`), confirming the known SEH flake fired
again rather than a regression; the flake's root cause remains on the
correctness backlog.
