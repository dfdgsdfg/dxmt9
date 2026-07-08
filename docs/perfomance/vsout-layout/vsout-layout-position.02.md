---
domain: vsout-layout
workload: 3DMark05 GT1
subcategory: position
order: 02
title: Fragment-Only Constant-Color Probe
date: undated
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L7878-L7989
---

# Fragment-Only Constant-Color Probe

**Question / hypothesis.** Control for the position-only probe, which changed two
things at once (16B VSOut **and** a constant fragment color). Does forcing only the
constant fragment color — while keeping the full `0xfff` / `184B` VSOut — reproduce
the `-79MiB` enc=2 VS-write delta? If yes, the mover is fragment/raster, not VSOut
width.

**Method.** `DXMT_DEBUG_FORCE_FRAGMENT_COLOR=1` (no
`DXMT9_PROBE_POSITION_ONLY_VSOUT`) via `run_3dmark05_perf_probe.sh
--force-fragment-color --frame 60 --encoder-breakdown-seq 60 --dump-shaders`.
Finalized against `current-normal-gputrace-r1` with the join/PSO/shader-dump gates.
(Note: an earlier export accidentally reused the position-only CSV; the corrected
distinct export hash is recorded in the source.)

**Result.**

| Metric | Normal | Force-fragment | Delta |
|---|---:|---:|---:|
| Total GPU | `35.456ms` | `35.377ms` | `-0.22%` |
| Top 3 GPU | `34.837ms` | `35.094ms` | `+0.74%` |
| Top 3 VS buffer write | `1627.240MiB` | `1548.284MiB` | `-4.85%` |
| VSOut key | `0xfff` | `0xfff` | unchanged |
| Expected VSOut B/vertex | `184B` | `184B` | unchanged |

The enc=2 VS-write delta (`-79.069MiB`: `-45.361MiB` invocations + `-33.707MiB`
B/inv) is essentially identical to position-only's `-79.022MiB` — but here the
VSOut layout never changed. Versus position-only directly: Top-3 VS write differs
only `+0.00%`, while Top-3 GPU is `+4.23%` (so position-only's GPU win was not
reproduced and is non-actionable noise).

**Verdict.** Rejected — and decisive as a *control*: the `-79MiB` VS-write
movement is produced by the constant-fragment/raster/backend interaction with
`184B` VSOut unchanged, proving visible VSOut width is **not** the mover. Confirms
the hidden vertex-stage / TVB owner; constant-fragment still leaves `1548.284MiB`
of VS writes, so fragment work is not the final owner either.

**Related.** [vsout-layout](index.md) · control for [vsout-layout-position.01](vsout-layout-position.01.md) · confirms [hidden-backend-storage](../hidden-backend-storage/index.md) · [backend-shape-classifiers](../backend-shape-classifiers/index.md) · [tvb-mechanism-proof](../tvb-mechanism-proof/index.md).
