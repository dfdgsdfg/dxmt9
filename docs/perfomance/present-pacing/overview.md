---
domain: present-pacing
workload: 3DMark05 GT1
title: "Present-Pacing — display sync, frame latency, and the wallclock cap - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/present-pacing/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/present-pacing/index.md; docs/perfomance/present-pacing/log.md
---

# Present-Pacing — display sync, frame latency, and the wallclock cap - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `present-pacing.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the **CPU/sync side** of GT1 wallclock: why
`completion_wait_ms` (the time the completion handler thread spends in
`MTLCommandBuffer.waitUntilCompleted()`) reached 28-31 s while
`gpu_command_buffer_time_ms` was only ~4 s, where that gap actually lives,
and which production-safe knobs can recover the slack without breaking
visual sync.

Every finding here moves *wallclock fps* directly, not GPU frame time. The
GPU frame-time story is owned by [hidden-backend-storage](../hidden-backend-storage/index.md) /
[index-cache-locality](../index-cache-locality/index.md); the per-CB encode story by [state-churn-encode](../state-churn-encode/index.md).

## Latest Conclusions

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H193 | PE const-chain overhead cuts land clean but move no FPS | accepted null-FPS result; cuts kept | [present-pacing-pe-const-overhead-cut.193](present-pacing-pe-const-overhead-cut.193.md) removes the fixed per-record machinery H192 blamed (handle-free append fast path for the six `SET_*_CONST_*` types, single `recorderMutex_` acquisition across const-flush+draw append, vector-free default flush; byte-identical wire pinned by the PE record specs). Post-change offload scouts: `2005/2015` presents (median `2010`) vs pre-change `1996/1999/2022` (median `~1999`) — `+0.55%`, noise. The removed overhead is real but `~0.3-1ms/present` at most; H192's `±3ms` correction over-attributed to per-append fixed cost, which also weakens the inline-const-delta (Plan B) premise — eliminating the 645 records would save only residual header/arena/push costs. Next: non-perturbing PE re-measurement (decimated stats) before further PE work, or consolidate the two proven opt-ins through their formal promotion gates. |
| H194 | Consolidation long confirm: offload+index-cache +10.0% over the full demo | accepted long confirm | [present-pacing-consolidation-long-confirm.194](present-pacing-consolidation-long-confirm.194.md) runs timeout-150 full-demo pairs: baseline 1800 vs candidate 1980 presents (+10.0%), zero GPU errors, 332,785 reordered-cache hits, and a time-aligned visual gate (equal-wall-time actual.png pairs at demo t=34s show identical scene/effect classes; frame counters match the fps ratio). Two invalid attempts recorded as method lessons: CPU-contention pollution from a co-scheduled build agent, and the recurring startup flake. Remaining before default-flip: the paired offload+opt-in gputrace proof (manual Xcode export; June frame60 baseline joined CSV carries a code-drift caveat) and the flip decision. |
| H195 | The offload+opt-in pair passes the full formal promotion proof | accepted promotion proof | [index-cache-locality-offload-promotion-proof.20](../index-cache-locality/index-cache-locality-offload-promotion-proof.20.md) runs the frame60 `.gputrace` + manual Xcode export through the complete `--require-opaque-depth-index-cache-proof` gate set against the June baseline joined CSV: finalizer verdict "all requested requirement gates were satisfied". Target rows `60/0+60/1`: GPU `-7.39%`, VS buffer write `-16.54%`, VS invocations `-14.12%` (exactly matching the historical accepted proof), candidate miss32 `582,658 -> 450,807`. Evidence for the pair is now complete (runtime FPS + correctness + GPU proof); an unconditional engine default-flip for `DXMT9_OFFLOAD_COMMIT_REPLAY` remains blocked by the non-PE COM present-boundary caveat, so the recommended vehicle is the shared `perf` experiment profile. |
| H196 | Non-perturbing producer sampling finds the buffer-lock bridge storm | accepted attribution; next lever named | [present-pacing-producer-sampling-attribution.196](present-pacing-producer-sampling-attribution.196.md) samples the promoted-pair producer thread with a parallel xctrace window (no encoder breakdown, no PE stats): `70.6%` Rosetta-translated guest code, `21.7%` wow64/win64-PE layer, `2.2%` winemetal unix leaf. Counters name the dxmt9-owned share: `1,478.7` buffer locks/present (`99.2%` READONLY managed-pool, `14.78MB/present` re-shadowed), each crossing the wow64 bridge — `5.0ms/present` unix wall plus `2.7ms/present` map-mutex contention against worker/encode plus a share of the `12.6ms/present` wow64-transition bucket. Estimated reachable `5-9ms/present` (`+9-18%` FPS ceiling) via a bridge-free readonly re-lock fast path. Method lessons recorded: encoder-breakdown windows saturate the encode thread and invalidate CPU shares (`2040 -> 660` presents); startup flake produced its first crash signature (`mutex lock failed: Invalid argument` after factory-only bridge calls). |
| H197 | Readonly managed buffer cache collapses the lock bridge storm | accepted mechanism confirm; not FPS promotion | [present-pacing-readonly-managed-buffer-cache.197](present-pacing-readonly-managed-buffer-cache.197.md) runs the PE readonly managed-buffer cache in the real GT1 no-gputrace probe. The run passes (`returncode=0`, no timeout, no non-perf fatal/assert/crash/ABI rows) and reduces bridge-visible locks from H196's `1,478.7` to `54.0/present`, readonly locks from `1,466.7` to `42.1/present`, shadow traffic from `14.78MB` to `2.03MB/present`, `d3d9_buffer_lock_ms` from `5.005` to `0.744ms/present`, and map mutex wait from `2.716` to `0.017ms/present`. PE-cache hits are intentionally invisible to unix `map_buffer_*` counters, and sampled FPS (`20.456` mean / `20.034` median) does not prove a throughput win, so this is not an FPS promotion. Next attribution should add PE hit counters or re-sample the producer to split the now-exposed Rosetta guest / PE / Wine remainder. |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [present-pacing-readonly-managed-buffer-cache.197 - Readonly Managed Buffer Cache Collapses The Lock Bridge Storm](present-pacing-readonly-managed-buffer-cache.197.md)
- [present-pacing-producer-sampling-attribution.196 - Non-Perturbing Producer Sampling Finds The Buffer-Lock Bridge Storm](present-pacing-producer-sampling-attribution.196.md)
- [present-pacing-consolidation-long-confirm.194 - Consolidation Long Confirm - Offload+IndexCache +10% Over The Full Demo](present-pacing-consolidation-long-confirm.194.md)
- [present-pacing-pe-const-overhead-cut.193 - PE Const-Chain Overhead Cuts Land Clean But Move No FPS](present-pacing-pe-const-overhead-cut.193.md)
- [present-pacing-pe-cost-verification.192 - PE Recording Cost Is Real (~10ms/present, Overhead-Corrected)](present-pacing-pe-cost-verification.192.md)
- [present-pacing-offload-backpressure-attribution.191 - Offload Backpressure Attribution Closes The Mechanism Gate](present-pacing-offload-backpressure-attribution.191.md)
- [present-pacing-commit-replay-offload.190 - Commit-Replay Offload First Runtime Proof](present-pacing-commit-replay-offload.190.md)
- [present-pacing-deferred-boundary-isolated.189 - Isolated Deferred Present Boundary On The Baseline Shape](present-pacing-deferred-boundary-isolated.189.md)
