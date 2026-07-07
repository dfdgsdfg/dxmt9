---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 19
title: Offload Absorbs The Index-Cache CPU Tax At FPS Parity
date: 2026-07-07
type: no-gputrace
status: accepted-cpu-tax-absorbed
source: experiments/output/app-d3d9-3dmark05-synergy-idxcache-r9-20260707/result.json; experiments/output/app-d3d9-3dmark05-pe-cost-nostats-r8-20260707/result.json; docs/perfomance/present-pacing/present-pacing-offload-backpressure-attribution.191.md
related: docs/perfomance/index-cache-locality.md; docs/perfomance/index-cache-locality/index-cache-locality-cpucost.18.md; docs/perfomance/present-pacing/present-pacing-commit-replay-offload.190.md
---

# Index-Cache Locality 19 - Offload absorbs the CPU tax

## Question

The opaque-depth index-cache opt-in has an accepted frame60 GPU proof
(target rows GPU `-10.64%`, VS invocations `-14.12%`, VS write `-16.77%`)
but stayed off the shared `perf` default because its candidate-construction
and lookup CPU previously sat on the producing thread's critical path. With
`DXMT9_OFFLOAD_COMMIT_REPLAY=1` moving replay (and the index-setup path with
it) onto a worker that idles `~45 ms/present` (H191), is the CPU tax still
FPS-visible?

## Run

Back-to-back offload-on pair: `--optimize-opaque-depth-index-cache` (r9) vs
without (r8), 120 s no-gputrace.

## Verdict

Accepted: the CPU tax is fully absorbed; the runtime blocker for promotion
is gone.

- **FPS parity**: `1999 -> 1980` presents (`-0.95%`, inside the noise band);
  `gpu_command_buffer_errors=0` in both.
- **Mechanism at scale** (`result.json` counters; the
  `3dmark05-index-cache-runtime-summary.md` shows zeros only because
  `--no-encoder-breakdown` suppresses the encoder-CSV rows it scrapes):
  `reordered_index_cache_lookups=787,596`, **`hits=333,283`**
  (`~168` reordered draws applied per present), `created=67` buffers
  (`1.39 MB`), `rejected_hits=454,170` (the conservative opt-in leaving
  non-target rows in original order, as designed), `misses=143`.
- **Absorbed cost**: candidate build `164 ms` + select `131 ms`
  (`302,538` select calls, `2.06 M` scored slots) + lookup `133 ms` +
  apply `5 ms` ≈ `~470 ms/run` (`~0.24 ms/present`) now runs on
  worker/encode threads with idle headroom.

## Promotion state

Runtime side of the default-promotion case is complete: GPU win (existing
frame60 Xcode proof) + FPS-parity cost under offload. The remaining formal
gate is a fresh paired `.gputrace` run through
`--require-opaque-depth-index-cache-proof` with the offload flag set (Xcode
manual counter export), plus the offload flag's own promotion decision —
the two opt-ins are natural companions and should be judged as a pair.
