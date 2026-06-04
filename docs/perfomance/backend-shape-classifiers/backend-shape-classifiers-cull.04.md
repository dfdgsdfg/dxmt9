---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
subcategory: cull
order: 04
title: Row/Class-Scoped Cull Shape Probe
date: 2026-06-02
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L10531-L10642
---

# Row/Class-Scoped Cull Shape Probe

**Question / hypothesis.** Broad cull probes are noisy for the current hot-row
shape. With a row/class-scoped cull override, does changing the effective cull
mode of one targeted opaque row own its hidden VS-write share?

**Method.** `DXMT9_PROBE_FORCE_CULL_MODE=none`,
`DXMT9_PROBE_FORCE_CULL_MODE_ROW=60/1`,
`DXMT9_PROBE_FORCE_CULL_MODE_CLASS=opaque-depth-write` (wrapper
`--probe-force-cull-mode none --probe-force-cull-mode-row 60/1
--probe-force-cull-mode-class opaque-depth-write`). No-gputrace smoke confirmed
scope (`60/1` cull `0/156/0 -> 156/0/0`, image was a normal GT1 frame), then a
gputrace export with strict top-row key/coverage/PSO/drift gates finalized vs
`measure-index-cache-gputrace-r1`.

**Result.**

| Metric | Baseline | `60/1` cull-none | Delta |
|---|---:|---:|---:|
| Total GPU | `34.391ms` | `34.877ms` | `+1.41%` |
| Hot-set VS buffer write | `1472.747MiB` | `1472.784MiB` | `+0.00%` |
| `60/1` GPU | `8.252ms` | `8.599ms` | `+4.21%` |
| `60/1` VS buffer write | `437.404MiB` | `437.306MiB` | `-0.02%` |
| `60/1` named tiled buffer | `0.750MiB` | `1.000MiB` | `+33.33%` |
| `60/1` cull n/f/b | `0/156/0` | `156/0/0` | target changed |

**Verdict.** Rejected. The scoped probe definitively flips the target row's cull
mode (`front -> none`) while preserving hot-row membership and geometry, yet VS
write, VS B/invocation, and hidden write all stay flat. The small named tiled
movement again proves backend shape changed but is far below the ~437 MiB
target-row hidden estimate. Together with broad cull and force-cull-back, cull
state/orientation is rejected as a first-order fix at every scope.

**Related.** [[backend-shape-classifiers]] · last in the cull sequence after [[backend-shape-classifiers-cull.03]] · confirms [[hidden-backend-storage]] · motivates [[primitive-reorder-diagnostics]] locality/order probes.
