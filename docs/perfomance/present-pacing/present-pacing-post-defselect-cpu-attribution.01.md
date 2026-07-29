---
domain: present-pacing
workload: 3DMark05 GT2
subcategory: post-defselect-cpu-attribution
order: 01
title: After The GPU Ceiling Fell, One Saturated Application Thread Is The Limit
date: 2026-07-29
type: experiment-run
status: accepted-attribution
source: traces/app-d3d9-3dmark05-gt2-cpu-attrib-r1/analysis/xctrace-cpu-thread-summary.md; traces/app-d3d9-3dmark05-gt2-cpu-attrib-r1/analysis/xctrace-cpu-thread-summary.csv; traces/app-d3d9-3dmark05-gt2-cpu-attrib-r1/analysis/xctrace-cpu-thread-verdict.json
related: docs/perfomance/shader-codegen/shader-codegen-defselect.02.md; docs/perfomance/overview.md
---

# After The GPU Ceiling Fell, One Saturated Application Thread Is The Limit

**Question / hypothesis.** `d63f7a65` removed the DEF-overlay register-file copy
and took GT2 from `8.84` to `18.58` fps
([defselect.02](../shader-codegen/shader-codegen-defselect.02.md)). GPU work
fell to `18%` of frame time and VS device-write bandwidth from `80%` of M1's
`68.25 GB/s` to `6.4%`. The prior CPU attribution (H212) concluded the residual
wall was the game's own CPU, but it was taken while the GPU dominated. With the
balance changed by an order of magnitude, does that conclusion still hold?

**Method.** `run_3dmark05_system_trace_sidecar.sh --export-cpu-summary
--cpu-producer-from-pe-log` over a GT2 run: an all-processes Metal System Trace
recorded for `25 s` starting `40 s` into the scene, exported and summarized per
thread. `33,588` `time-profile` rows and `22` `thread-info` rows matched the
`3DMark05.exe` process.

**Result.** Sampled running time, `33,588 ms` across a `25,000 ms` wall window —
an average of `1.34` cores busy.

| Thread | ms | share | % of one core |
|---|---:|---:|---:|
| `3DMark05.exe (0x17c1fb2)` | `23,718` | `70.6%` | **`94.9%`** |
| `dxmt9-encode (0x17c2175)` | `5,453` | `16.2%` | `21.8%` |
| `3DMark05.exe (0x17c21f4)` | `3,557` | `10.6%` | `14.2%` |
| all others (19 threads) | `860` | `2.6%` | — |

Grouped: **application threads `83.7%`, dxmt9 threads `16.3%`** (`dxmt9-encode`
`5,453 ms`, `dxmt9-finish` `16 ms`, `dxmt9-completion` `9 ms`). The application's
`Main Thread` is only `22 ms`; the dominant thread is a worker, consistent with
3DMark05 running under Rosetta and Wine.

`dxmt9-encode`'s samples carry `presentDrawable=20`, `CAMetalLayer=15`,
`nextDrawable=8` keyword hits, so part of its `21.8%` is drawable acquisition
and present, not translation work.

**Verdict.** ACCEPTED, and H212's conclusion survives the change in balance. A
single application thread runs at `94.9%` of one core for the whole window —
saturated. dxmt9's entire contribution is `16.3%` of burned CPU on a separate
thread, and that thread is itself only a fifth of a core. Removing all of
dxmt9's CPU cost would not move a frame time set by a serial application thread
that is already pinned.

**What this does and does not establish.** `time-profile` samples *running*
stacks, so this attributes CPU that is burned, not time that is lost: at
`1.34` cores busy on a machine with more, some of the `53.82 ms` frame is
serialization or waiting that this method cannot see. The producer-thread
auto-selection also failed — the PE log's `thread_id` is a Win32 id and does not
match xctrace's native Mach namespace, so the P4 producer verdict is
`producer-thread-not-found` and no producer-side wait/hold claim is made here.
The thread weight table is direct evidence and does not depend on that selector.

**Implication for the ceiling question.** The GPU/bandwidth ceiling identified
earlier is gone. What replaces it is not a dxmt9 cost: on GT2, dxmt9 burns one
sixth of the CPU while the application saturates a core on its own. Further
dxmt9-side optimization on this workload has a bounded ceiling of roughly that
one sixth, and only if it were free — which the drawable/present keyword hits
suggest part of it is not.

**Related.** [present-pacing](index.md) ·
[shader-codegen-defselect.02](../shader-codegen/shader-codegen-defselect.02.md) ·
[overview](../overview.md) · [overview-3dmark05-gt2](../overview-3dmark05-gt2.md)
