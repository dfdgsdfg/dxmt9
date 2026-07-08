---
domain: mini-replay-bisection
workload: 3DMark05 GT1
subcategory: depth
order: 01
title: Depth-Content Sensitivity Probes
date: undated
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L14550-L14765
---

# Depth-Content Sensitivity Probes

**Question / hypothesis.** The original `60/2` encoder is depth-enabled,
depth-write-off, compare-func `4`, and inherits depth from earlier passes, while
the mini replay clears a fresh depth texture to `1.0`. Is the missing
vertex-stage amplification owned by depth attachment **content** (clear scalar or
real pre-existing depth)?

**Method.** Two probes on the scissor-aware 16-draw replay. (1)
`--depth-clear 0.0` instead of `1.0`. (2) A raw D24X8 `--depth-input` loaded from
a real GPU-side depth dump: `DXMT9_DUMP_DEPTH_ATTACHMENT_HANDLE/_PATH` (wrapper
`--dump-depth-attachment-handle 0x300000100000001 --dump-depth-attachment-seq 60
--dump-depth-attachment-enc 2`), which blits the live depth/stencil target to a
readback buffer with `.json` metadata; the runner uploads it and switches the
depth load action `Clear`→`Load`. (The old BMP `DXMT_DUMP_GPU_TEXTURE_*` hook
skips format 41/D24X8 and is upload-path only, so it could not capture this.)

**Result.** Across depth=1, depth=0, and raw D24X8 input, the vertex bucket is
fixed at ~31.98 MiB:

| Metric | Original | depth=1 | depth=0 | raw D24X8 |
|---|---:|---:|---:|---:|
| GPU time | 20.327ms | 1.498ms | 0.989ms | 1.082ms |
| VS buffer write | 981.171MiB | 31.978MiB | 31.975MiB | **31.987MiB** |
| VS invocations | 642,001 | 54,104 | 54,104 | 54,104 |
| VS B / VS inv | 1602.5B | 619.8B | 619.7B | 619.9B |
| FS invocations | 3,296,064 | 2,963,392 | 786,432 | 786,432 |

**Verdict.** REJECTED. Depth content drives fragment work (depth=0 / raw input
cut FS inv to 786,432) but leaves vertex-stage backend traffic fixed at ~31.99 MiB
and ~620B/VS inv — 31x smaller than the original 981.171 MiB. The missing owner is
not a depth clear/load scalar. The next replay must preserve a wider `60/2`
draw prefix so the same primitive/binning/backend state exists before the measured
draws.

**Related.** [mini-replay-bisection](../mini-replay-bisection.md) · [mini-replay-bisection-replay.02](mini-replay-bisection-replay.02.md) ·
[mini-replay-bisection-replay.03](mini-replay-bisection-replay.03.md) · [render-pass-store](../render-pass-store.md) (rules out depth
re-entry content) · [hidden-backend-storage](../hidden-backend-storage.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md)
