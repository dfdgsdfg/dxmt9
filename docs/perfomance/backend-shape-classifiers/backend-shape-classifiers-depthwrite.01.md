---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
subcategory: depthwrite
order: 01
title: Probe Disable Depth Write
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L2224-L2231
---

# Probe Disable Depth Write

**Question / hypothesis.** Does depth-write render state own the hidden VS-write
bucket? Diagnostic only — suppressing depth writes makes the frame visibly wrong.

**Method.** `DXMT9_PROBE_DISABLE_DEPTH_WRITE=1`. Xcode counter A/B against the
normal-source baseline.

**Result.**

| Metric | Normal | Disable depth write | Delta |
|---|---:|---:|---:|
| Top depth write | `3.815MiB` | `1.188MiB` | `-68.87%` |
| Top-3 VS buffer write | `1627.240MiB` | `1627.331MiB` | `+0.01%` |
| Top GPU time | `34.837ms` | `37.741ms` | `+8.34%` |

**Verdict.** Rejected. Depth-write mode is not the first-order owner: depth
attachment traffic drops materially but the ~1.6 GiB VS-write bucket stays flat
and GPU time regresses. Depth attachment traffic is a secondary cost, not the
hidden Apple vertex/tiler/parameter storage owner. Row-scoped depth-write and
depth-func probes later joined the same stable ~1627 MiB bucket — stop spending
gputrace time on depth-only state as a primary owner.

**Related.** [backend-shape-classifiers](index.md) · paired with [backend-shape-classifiers-depthfunc.01](backend-shape-classifiers-depthfunc.01.md) · confirms [hidden-backend-storage](../hidden-backend-storage/index.md) survives · related [backend-shape-classifiers-visible.01](backend-shape-classifiers-visible.01.md) (also dropped depth writes without moving the bucket).
