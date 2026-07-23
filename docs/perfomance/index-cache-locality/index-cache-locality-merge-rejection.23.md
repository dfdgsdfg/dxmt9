---
domain: index-cache-locality
workload: 3DMark05 GT2
subcategory: merge-selector
order: 23
title: Strict Merge Rejections Require Multiple Preserved Draw Properties
date: 2026-07-21
type: no-gputrace
status: measured-design-gate
source: experiments/output/app-d3d9-3dmark05-merge-reject-gt2-r2-20260721/result.json
related: docs/perfomance/index-cache-locality/index.md; docs/perfomance/index-cache-locality/index-cache-locality-scope-merge-gt2.22.md; docs/perfomance/overview-3dmark05-gt2.md
---

# Index-Cache Locality 23 - Strict merge rejection distribution

## Question

The strict compatible indexed-draw merge eliminates no GT2 draws. Is one
conservative predicate responsible for enough rejected pairs to justify a
narrow semantic expansion?

## Instrumentation

When compatible merge and perf counters are both enabled, the encoder scans
every adjacent `DrawParam` boundary within each draw run once. The pure
classifier is also the predicate used by the actual merge helper, preventing
telemetry/behavior drift. It records:

- overlapping raw causes for draw shape, index type, base/start vertex,
  uniform, serialized binding override/snapshot, index-range continuity, and
  primitive-count overflow;
- raw single-cause and multiple-cause populations;
- an exact logical partition over `binding payload`, `uniform`, and
  `non-contiguous index range`. Binding override and binding snapshot are
  grouped here because both serialize one draw's binding state;
- strictly selected pair count.

The scan is allocation-free and absent from the default path. It runs only
behind `DXMT9_OPTIMIZE_COMPATIBLE_INDEXED_DRAW_MERGE=1` with perf counters
enabled.

## Run

The authoritative r2 run used Sikarugir Wine, the `perf` profile, no Metal
capture, frame sampling, frontmost supervision, and the corrected GT2 command:

```sh
DXMT_3DMARK05_ARGS='-gt2 -nosplash -nosysteminfo -noscreens' \
DXMT_3DMARK05_RESULT_FILE=dxmt9_gt2.3dr \
DXMT9_OPTIMIZE_COMPATIBLE_INDEXED_DRAW_MERGE=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix merge-reject-gt2-r2-20260721 --frame 60 \
  --no-gputrace --no-encoder-breakdown --frame-sampling \
  --keep-frontmost --timeout 120 --top 5
```

The launcher log records exactly
`-gt2 -nosplash -nosysteminfo -noscreens dxmt9_gt2.3dr`. The run completed
normally with `status=pass`, return code zero, `525` encoded presents, and zero
GPU command-buffer, pipeline-build, skipped-pipeline, or V2-reject errors. The
frame-461 capture at `56.98s` shows the expected coherent glowing-tree scene.
This is a selector-volume run; its additional diagnostic scan makes it
unsuitable for an FPS comparison.

## Result

All `575,523` adjacent boundaries have the required indexed triangle-list,
single-instance, no-UP shape. Index type, base vertex, start vertex, and
primitive-count overflow reject zero pairs. The overlapping active causes are:

| Raw cause | Pairs | Share of attempts |
|---|---:|---:|
| non-contiguous source-IB range | `564,346` | `98.06%` |
| serialized binding override | `500,937` | `87.04%` |
| serialized binding snapshot | `500,937` | `87.04%` |
| uniform handle | `203,203` | `35.31%` |

No pair is strictly compatible, selected, or rejected by only one raw cause.
After grouping override+snapshot as one logical binding-payload condition, the
exact rejection partition is:

| Exact logical relaxation set | Pairs | Share |
|---|---:|---:|
| binding payload + non-contiguous IB | `361,143` | `62.75%` |
| binding payload + uniform + non-contiguous IB | `128,617` | `22.35%` |
| uniform + non-contiguous IB | `74,586` | `12.96%` |
| binding payload only | `11,177` | `1.94%` |
| every other set | `0` | `0.00%` |

The partition is exact:

```text
575,523 = 361,143 + 128,617 + 74,586 + 11,177
```

The unchanged strict accounting also holds:

```text
draw_calls = submit_draw_run_batch_append_params + draw_geometry_up
882,896 = 879,228 + 3,668
```

## Verdict

**There is ample adjacent-pair volume, but no single-predicate merge
frontier.** A joined index buffer alone recovers zero pairs. The dominant
class (`361,143`, `62.75%`) simultaneously needs non-contiguous index handling
and preservation of per-draw binding payload. Another `74,586` pairs need both
joined-index handling and per-draw uniforms, while `128,617` need all three.

An ordinary combined Metal indexed draw cannot change resource bindings or
uniforms inside that draw. Therefore relaxing byte equality or continuity in
the current helper would change D3D9 semantics. Keep the strict merge default
OFF; do not build a joined-index-buffer cache as a standalone next step.

The evidence instead points to a draw-boundary-preserving multi-draw design if
this direction continues. Before choosing that mechanism, split the large
binding-payload class into index binding/snapshot, vertex-stream binding, and
other binding/alpha-override changes. That discriminator determines whether a
joined-index carrier can absorb most of the `62.75%` class or whether true
per-subdraw resource state is required.
