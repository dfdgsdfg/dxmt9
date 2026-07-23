---
domain: hidden-backend-storage
workload: 3DMark05 GT2
subcategory: shape
order: 36
title: GT2 Full-Frame Native Replay Preserves the Runtime Rate Without Partial Renders
date: 2026-07-22
type: measurement
status: accepted-attribution
source: traces/app-d3d9-3dmark05-gt2-v2-indexcache-current-frame279-on-r1-20260720/frame279.gputrace; traces/app-d3d9-3dmark05-gt2-v2-indexcache-current-frame279-on-r1-20260720/analysis/frame279-counters-xcode.csv; traces/app-d3d9-3dmark05-gt2-v2-indexcache-current-frame279-on-r1-20260720/analysis/frame279-xcode-dxmt-bottleneck-report.md
related: docs/perfomance/overview-3dmark05-gt2.md; docs/perfomance/hidden-backend-storage/index.md; docs/perfomance/tvb-mechanism-proof/index.md
---

# Hidden Backend Storage 36 - GT2 full-frame native replay

## Question

Does GT2 remain near its observed `~8 FPS` ceiling when the captured Metal
frame is replayed without 3DMark05, Wine, Rosetta, the PE/unix bridge, command
recording, or dxmt encoding? Is the large vertex-side device traffic caused by
parameter-buffer overflow and partial-render spill?

## Method

The existing current-stack `frame279.gputrace` is a complete capture of a
representative Firefly Forest frame at `1024x768`. Xcode Performance replay
submits the captured Metal command buffers and resources directly; application
and translation CPU work is absent from the replayed GPU interval. The Summary
view and the already-exported encoder-counter CSV are two replay modes of the
same capture. The CSV was reprocessed with strict Xcode-counter, dxmt-join, and
top-PSO coverage gates after adding `Partial Render Count` to the persistent
summary/report fields.

This is a GPU-path discriminator, not a standalone implementation of the D3D9
application. Xcode replay overhead and replay variance prevent treating the
small difference between Summary and counter-profiling time as an application
FPS delta.

## Result

Xcode Summary reports `126.77ms` GPU time, `4` command buffers, `19` render
encoders, `1,759` Metal draw calls, and `5,442,926` vertices. The instrumented
encoder-counter replay totals `131.678ms`. Those two native replay modes imply
replay-equivalent rates of `7.89` and `7.59 FPS`, respectively, the same
performance band as the current `~8.15 FPS` Wine run. Xcode Summary labels the
replay performance state `Medium`; no cross-state or maximum-frequency
normalization was performed. The result is therefore a CPU-removal
discriminator at the captured replay state, not an absolute M1 hardware
ceiling.

| Full-frame counter | Value |
|---|---:|
| GPU time | `131.678ms` |
| render encoders | `19` |
| VS invocations | `2,529,660` |
| primitives | `1,817,934` |
| vertices | `5,442,926` |
| VS buffer device write | `6,952.646MiB` |
| all buffer device write | `6,955.188MiB` |
| all device write | `7,052.706MiB` |
| expected visible `184B` VSOut traffic | `443.895MiB` |
| VS write / expected visible VSOut | `15.66x` |
| named tiled vertex + primitive-block buffers | `47.750MiB` |
| VS write / named tiled buffers | `145.61x` |
| partial render count | **`0`** |

The top five encoders cover `125.181ms` (`95.07%`) and write `6,672.050MiB`
from `2,411,387` VS invocations. They also report zero partial renders. The
top three spend `97.348ms` with weighted vertex-stage attribution `97.96%` and
VS ALU limiter only `2.92%`.

## Verdict

**Accepted attribution:** the low GT2 rate survives native Metal full-frame
replay. The dominant limiter is therefore in the emitted Metal GPU workload,
not Wine/Rosetta execution, PE/unix publication, or dxmt CPU encoding.

**Overflow-spill hypothesis rejected for this frame:** Xcode reports zero
partial renders across every encoder. The `6.95GiB` VS buffer-write bucket
cannot be attributed to parameter-buffer overflow-triggered partial-render
store/reload. Named tiled buffers account for less than `0.7%` of the bucket.
The remaining owner is a per-invocation hidden VS/backend write mechanism;
compiler spill, firmware vertex storage, and other internal backend traffic
remain possible subcomponents and require a controlled shader/backend-shape
A/B to separate.

This also demotes GT2-specific draw merging. A draw-boundary-preserving
multi-draw path may reduce CPU encode calls, but it preserves the VS invocation
and backend-write numerator that owns this frame. The next general experiment
must reduce or explain bytes per VS invocation, with VS invocations held stable,
rather than merely reducing API call count.
