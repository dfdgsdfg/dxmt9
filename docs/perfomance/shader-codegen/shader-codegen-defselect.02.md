---
domain: shader-codegen
workload: 3DMark05 GT2
subcategory: defselect
order: 02
title: The Same Copy Removal Doubles GT2 Frame Rate
date: 2026-07-29
type: experiment-run
status: accepted-fps-win
source: traces/app-d3d9-3dmark05-defsel-gt2-base-tr/analysis/frame279-counters-xcode.csv; traces/app-d3d9-3dmark05-defsel-gt2-cand-tr/analysis/frame279-counters-xcode.csv; experiments/output/app-d3d9-3dmark05-defsel-gt2-base/3dmark05-direct.log; experiments/output/app-d3d9-3dmark05-defsel-gt2-cand/3dmark05-direct.log
related: docs/perfomance/shader-codegen/shader-codegen-defselect.01.md; docs/perfomance/hidden-backend-storage/overview.md; docs/perfomance/overview-3dmark05-gt2.md
---

# The Same Copy Removal Doubles GT2 Frame Rate

**Question / hypothesis.** [defselect.01](shader-codegen-defselect.01.md)
showed the DEF-overlay register-file copy owned GT1's hidden
`VS Buffer Device Memory Bytes Written` bucket, but GT1 turned out not to be
GPU-bound: `-87%` GPU time bought only `+8.8%` fps. GT2 has long been the
GPU-bound workload of the pair (`~8 fps`, `~6.95 GiB` of VS device writes at
frame279). Does the same removal convert there?

**Method.** Same A/B as defselect.01 — baseline emitter at `959c848c^`
(the `float4 cFloat[256]` copy), candidate `d63f7a65` (pointer alias + ternary
DEF select) — run against `-gt2`. Scene fps from per-frame `wall_ms` samples
with `DXMT9_PERF_FRAME_SAMPLING=1`, per
`agents/rules/metal_debugging.rules.md`, which forbids using GT2's
presents-at-kill. Plus one `frame279` `.gputrace` and Xcode encoder-counter
export per lane, 17 encoders each.

**Result — scene frame rate.** This is the sound GT2 metric: both lanes replay
the same fixed ~68 s timeline, so a median over the whole scene is comparable.

| Lane | samples | median frame | fps |
|---|---:|---:|---:|
| baseline | `520` | `113.16 ms` | `8.84` |
| candidate | `1,148` | `53.82 ms` | **`18.58`** (**`+110%`**) |

The baseline's `8.84 fps` reproduces the historical GT2 figure, which confirms
it is the pre-fix state and not a mis-staged build.

**Result — frame279 counters.**

| Counter | baseline | candidate | delta |
|---|---:|---:|---:|
| `GPU Time` | `135.552 ms` | `9.714 ms` | `-92.83%` |
| `VS Buffer Device Memory Bytes Written` | `7,371,414,784` | `0` | **`-100%`** |
| `VS Bytes Written To Device Memory` | `7,404,670,720` | `42,146,560` | `-99.43%` |
| `Partial Render Count` | `0` | `0` | — |
| `VS Invocations` | `2,551,916` | `3,042,794` | `+19.24%` |
| `Primitives` | `1,842,054` | `2,083,904` | `+13.13%` |
| `Tiled Vertex Buffer Bytes` | `26,411,008` | `33,816,576` | `+28.04%` |
| `FS Invocations` | `25,512,512` | `28,086,784` | `+10.09%` |

**The frame279 comparison is not workload-identical, and the `-92.83%` must not
be quoted as a clean delta.** GT2 is a time-based animation: at `18.58` fps the
candidate reaches frame 279 at a different scene moment than the baseline does
at `8.84` fps, so the two captures hold different geometry. GT1's pair had
bit-identical invocation and primitive counts; this one does not.

What survives that caveat is the direction and the mechanism, both of which are
stronger for the confound, not weaker: the candidate carries **more** vertex
work (`+19.24%` invocations, `+13.13%` primitives) and still spends `14x` less
GPU time, and its `VS Buffer Device Memory Bytes Written` is exactly `0` despite
that extra geometry. `Partial Render Count` is `0` on both sides, so this is not
parameter-buffer overflow.

**Verdict.** ACCEPTED as an fps win. GT2 was GPU-bound on precisely the traffic
this fix removes, so unlike GT1 the GPU saving converts: `+110%` scene fps,
`8.84 -> 18.58`. The `6.86 GiB` VS device-write bucket that
[hidden-backend-storage](../hidden-backend-storage/index.md) has attributed to
"hidden backend storage" since H46 is now identified and gone — it was
per-invocation stack spill from the DEF-overlay copy, the same cause as GT1's.

For an exact same-workload frame279 delta, a future capture would need both
lanes pinned to the same scene moment rather than the same frame ordinal.

**Related.** [shader-codegen-defselect.01](shader-codegen-defselect.01.md) ·
[shader-codegen](index.md) · [hidden-backend-storage](../hidden-backend-storage/index.md) ·
[overview-3dmark05-gt2](../overview-3dmark05-gt2.md)
