---
domain: tvb-mechanism-proof
subcategory: proof
order: 01
title: TVB Pressure Mechanism Proof
date: 2026-06-03
type: validation
status: accepted
source: specs/perfomance.plan.md#L18817-L19154
---

# TVB Pressure Mechanism Proof

**Question / hypothesis.** Does reducing post-transform VS invocation count (via
an index-cache LRU32 reorder) linearly reduce the firmware-owned TVB / Parameter
Buffer write traffic that dominates GT1 GPU cost? This is the load-bearing
mechanism that makes the opaque-depth index-cache win real rather than incidental.

**Method.** Row-local mini-replays with geometry, shader pair, render target,
depth target, and draw count locked between the original-order and
`cache-opt-lru32` variants. Counters read from
`mini-replay-full-r3-{original,cache-opt-lru32}-xcode-dxmt-joined-summary.csv`.
Gate `--require-tvb-mechanism-proof` in
`scripts/tools/compare_xcode_dxmt_bottlenecks.py` verifies the predicate. The
visible `VSOut` layout was held constant (`0xfff`, 184 B per vertex) across
variants, so any named-tiled counter movement is attributable to VS invocation
reduction. Full-frame production opt-in via `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1`
reproduced under the strict finalizer gates (capture `opaque-depth-index-opt-gputrace-r2`).

**Result.** Row-local replays — named tiled buffers, VS invocations, and GPU time
all drop together:

| Row | Named tiled (MiB) | VS invocations | GPU time | Δ tiled | Δ VS inv | Δ GPU |
|---|---:|---:|---:|---:|---:|---:|
| `50/1` | 1.938 → 1.625 | 1,223,148 → 1,096,962 | 1.977 → 1.804 ms | -16.13% | -10.32% | -8.76% |
| `50/3` | 17.625 → 15.688 | 1,197,258 → 1,075,671 | 5.502 → 5.324 ms | -10.99% | -10.16% | -3.25% |

Full-frame production opt-in (`opaque-depth-index-opt-gputrace-r2` vs r4 no-mutate
baseline): top-3 GPU `35.277 → 33.280 ms` (-5.66%); top-3 VS write
`1627.338 → 1518.904 MiB` (-6.66%); target rows `50/0,50/1` VS invocations
`536,583 → 460,839` (-14.12%), target LRU32 misses `594,296 → 0` (-100%);
non-target row `50/2` geometry- and counter-stable (GPU +0.13%). Of the
`108.434 MiB` target VS-write drop, `92.132 MiB` is fewer VS invocations and
`16.302 MiB` is lower bytes per invocation.

**Verdict.** ACCEPTED. The chain *VS invocations ↓ ⇒ TVB write bytes ↓ ⇒ GPU
time ↓* is confirmed end-to-end by two independent row-local replays and
reproduced full-frame. TVB write scales linearly with `VS invocations ×
per-vertex VSOut bytes` (Imagination/Asahi model, [[tvb-mechanism-proof-proof.02]]).
This is the mechanism that turns the opaque-depth index-cache reorder into the
one accepted production win — confirming [[hidden-backend-storage]] (the cost is
hidden vertex-stage/tiler/parameter storage, not CPU writers or visible VSOut
width) and [[index-cache-locality]] (the production path that exploits it).

**Related.** [[tvb-mechanism-proof]] · [[tvb-mechanism-proof-proof.02]] ·
[[hidden-backend-storage]] · [[index-cache-locality]] · [[mini-replay-bisection]] ·
[[vsout-layout]] (visible width held constant, so not the owner) · [[overview]]
