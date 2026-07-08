---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: identity
order: 02
title: Gputrace-Backed No-Mutate Scout
date: 2026-06-03
type: scout
status: tooling
source: specs/perfomance.plan.md#L13591-L13674
---

# Gputrace-Backed No-Mutate Scout

**Question / hypothesis.** Capture the same no-mutate indexed-draw identity with a
real frame capture + dumped shaders, joined to Xcode counters, so top Xcode rows map
to actual draw handles and shader hashes from one frame instance.

**Method.** `run_3dmark05_perf_probe.sh --suffix current-head-index-scout-gputrace-r1
--frame 60 --encoder-breakdown-seq 60 --measure-index-reuse --dump-shaders --top 3
--hot-gpu-share 95 --timeout 180`, then normal Xcode export/finalizer (passed Xcode
counter coverage, DXMT join coverage, top PSO attribution).

**Result.** NOT a clean perf baseline — same-run GPU `50.832ms` vs
`current-normal-gputrace-r1` `35.456ms`; top draws `385→535`; top buffer write `+24.77%`;
only `60/1` stayed a shared top row. But bottleneck evidence is stable and authoritative:
hot-set VS buffer write `2,236.981MiB`, `1,266.2B`/VS invocation, expected VSOut `184.0B`
(`6.9x`), named tiled counters `20.438MiB`, hidden backend estimate `2,215.926MiB`,
dxmt CPU writer `0.617MiB`. Hot rows: `60/4` `1091.008MiB` depth-read alpha/scissor/textured;
`60/3`/`60/1` `~470MiB` opaque depth-write; `60/0` `206.055MiB`.

**Verdict.** Tooling / scout (proves the hidden bottleneck, not a perf A/B). Confirms
`gpu_vs_buffer_write → hidden_vertex_tiler_parameter_storage`, dxmt CPU writers ≈ 0
relative to Xcode's bucket. Use only as mini-replay construction input.

**Related.** [index-cache-locality](index.md) · prev: [index-cache-locality-identity.01](index-cache-locality-identity.01.md)
· [hidden-backend-storage](../hidden-backend-storage/index.md) (the `2236.981MiB`, `1266B/VS inv` evidence) ·
[mini-replay-bisection](../mini-replay-bisection/index.md) · [index-reuse-measurement](../index-reuse-measurement/index.md) · [vsout-layout](../vsout-layout/index.md) (184B ruled out).
