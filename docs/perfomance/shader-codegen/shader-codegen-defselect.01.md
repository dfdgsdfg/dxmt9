---
domain: shader-codegen
workload: 3DMark05 GT1
subcategory: defselect
order: 01
title: Removing The DEF-Overlay Register-File Copy Cuts GT1 Frame GPU Time 87%
date: 2026-07-29
type: experiment-run
status: accepted-gpu-win
source: traces/app-d3d9-3dmark05-defsel-base-gt1/analysis/frame60-counters-xcode.csv; traces/app-d3d9-3dmark05-defsel-cand-gt1/analysis/frame60-counters-xcode.csv; experiments/output/app-d3d9-3dmark05-defsel-fps-base/3dmark05-direct.log; experiments/output/app-d3d9-3dmark05-defsel-fps-cand/3dmark05-direct.log
related: docs/perfomance/hidden-backend-storage/overview.md; docs/perfomance/mini-replay-bisection/mini-replay-bisection-vertexremap.01.md; docs/perfomance/shader-codegen/index.md
---

# Removing The DEF-Overlay Register-File Copy Cuts GT1 Frame GPU Time 87%

**Question / hypothesis.** GT1's dominant `VS Buffer Device Memory Bytes
Written` bucket has never been explained by visible shader shape. Eight of the
seventeen frame60 `enc1` vertex variants emitted

```metal
float4 cFloat[256];
for (uint i = 0; i < 256; ++i) { cFloat[i] = vsConsts.vsFloatConst[i]; }
cFloat[199] = float4(3.0f, 0.0f, 0.0f, 0.0f);   // the only non-loop write
```

— 4,096 B of per-invocation private stack, materialized so a *single* DEF
literal could overwrite one register. Is that copy the hidden write bucket?

**Method.** Same-session A/B differing only in the translator's constant-access
emission. Baseline is the emitter at `959c848c^` (the copy array); candidate is
`d63f7a65`, which keeps the read-only `constant float4*` alias, hoists the DEF
literal, and overlays it with a ternary select at each relative read. Both lanes:
GT1 `--frame 60` `.gputrace` via `--with-wine-capture-layer`, one Xcode encoder-
counter export each, plus a separate no-gputrace run per lane with
`DXMT9_PERF_FRAME_SAMPLING=1` for wall-clock.

**Result.** Nine render encoders at `seq=60` in both captures.

| Counter | baseline | candidate | delta |
|---|---:|---:|---:|
| `GPU Time` | `31.414 ms` | `4.044 ms` | **`-87.13%`** |
| `VS Buffer Device Memory Bytes Written` | `1,593,314,944` | `0` | **`-100%`** |
| `VS Bytes Written To Device Memory` | `1,615,321,024` | `21,724,416` | `-98.66%` |
| `Tiled Vertex Buffer Bytes` | `16,252,928` | `16,220,160` | `-0.20%` |
| `VS Invocations` | `1,102,915` | `1,102,915` | `+0.00%` |
| `Primitives` | `715,432` | `715,432` | `+0.00%` |
| `Partial Render Count` | `0` | `0` | — |
| `FS Invocations` | `19,844,640` | `19,365,248` | `-2.42%` |

Scene wall-clock, median over per-frame samples:

| Lane | samples | median frame | fps |
|---|---:|---:|---:|
| baseline | `2,198` | `45.26 ms` | `22.09` |
| candidate | `2,371` | `41.61 ms` | **`24.03`** (`+8.8%`) |

**Verdict.** ACCEPTED. The copy was the bucket. VS invocations and primitive
count are bit-identical across the pair, so the workload is unchanged and the
delta is attributable to emission alone. `Tiled Vertex Buffer Bytes` moves
`-0.20%` and `Partial Render Count` is `0` on both sides, which rules out
tiling and parameter-buffer overflow: the `1.48 GiB` was per-thread stack spill,
and it goes to exactly zero.

GPU time falls `7.8x` while scene FPS rises `8.8%`, so GT1 is not GPU-bound at
this point — the frame-time floor is elsewhere. The GPU headroom this frees is
real but is not, on its own, a proportional FPS win.

This is the vertex-stage instance of H226 (`7abaa20e`), whose commit comment
predicted it: *"the per-invocation copy loop plus 2-4KB of stack write/read
traffic owned SFIV's ~30ms fullscreen scene passes and tracks GT1's hidden
VS-buffer-write bucket."* H226's gate excluded these shaders only because they
combine relative addressing with a DEF.

**Correctness.** The first attempt (`959c848c`) emitted the select as an
immediately-invoked MSL lambda and made every skinned character in GT1 vanish —
six captured frames spanning `0:13-0:57` showed the environment intact with
weapons floating unheld. `d63f7a65` emits a plain nested ternary instead and
passes the visual gate: characters present and correctly skinned at matching
frame ordinals in both lanes.

**Related.** [shader-codegen](index.md) · [hidden-backend-storage](../hidden-backend-storage/index.md) ·
[mini-replay-bisection-vertexremap.01](../mini-replay-bisection/mini-replay-bisection-vertexremap.01.md) ·
[overview-3dmark05-gt1](../overview-3dmark05-gt1.md)
