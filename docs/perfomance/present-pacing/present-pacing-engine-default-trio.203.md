---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 203
title: The Promoted Trio Becomes The Engine Default
date: 2026-07-10
type: no-gputrace
status: accepted-default-promotion
source: experiments/output/app-d3d9-3dmark05-engine-default-proof-r2-20260710/result.json; specs/backend/requirements.md; specs/backend/gap.md
related: docs/perfomance/present-pacing/index.md; docs/perfomance/present-pacing/present-pacing-readonly-cache-stacking.198.md
---

# Present-Pacing H216 - Engine-default promotion of the trio

## Question

After H211 the trio (offload + index-cache + readonly lock cache) was fully
promoted only inside the `perf` launcher profile; the engine default was
blocked by the non-PE present-boundary caveat and the raw-latency niche. Can
the trio become the true engine default?

## The three blockers and how they fell

1. **Global boundary suppression** — `submitPresent` skipped the inline
   present boundary for every present in the process while the offload env
   was on. Fixed per-present (`72132513`): `core::SwapDesc::pacedByPresentOrdinal`
   is set only by the chunk-replay present path, resolved through the pure
   `resolvePresentBoundaryAction` truth table; direct COM presents keep the
   inline boundary. Native-pinned + TLA (`31.9s`, all green).
2. **Raw-latency niche** — the ordinal wait ignored
   `DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS`; now shares `cappedFrameLatency`
   with the inline boundary (`72132513`).
3. **"Heap corruption" under forced offload** — root-caused (`cad446ce`) to a
   native-spec harness gap, not a production race: two specs passed
   stack-allocated wire wrappers and bypassed the `drainDeferredReplay()`
   fence every real bridge call gets, so the worker replayed against
   destroyed stack objects. Fixed test-side; regression-pinned by
   offload-forced spec variants.

## The flip (`d45af067`)

`DXMT9_OFFLOAD_COMMIT_REPLAY` unset/empty → ON (explicit `0` opts out);
`DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE` unset → follows the offload state
(coupled pair; explicit value forces). The parse is duplicated in
`dxmt9_debug_trace.cpp` with a pointer at the canonical resolver because the
dxmt9 layer must not link upward into d3d9. A subtle casualty surfaced
immediately: the two offload-aware specs kept local copies of the old
default-off parse and failed the moment the default flipped — both now
delegate to the production resolver (drift-proof).

## Pure engine-default runtime proof

A catalogue run with the pair env set nowhere (debug-profile empty strings,
which the new resolvers treat as unset): `status=pass`, **`2,220` presents**
(healthy-population center), `gpu_command_buffer_errors=0`, visuals in class.
All three mechanisms live: offload (commit `1.648ms` / replay `9.119ms` /
worker idle `39.3ms` per present), index-cache (`169.2` reordered hits per
present), lock cache (`54.7` locks per present). Boundary correctness:
`present_boundary_skipped` is exactly `1.000/present` — only the chunk-replay
present skips the inline boundary.

## State after this leaf

GT1 cumulative: `1,800 -> ~2,26x` presents (`+26%`) now applies to every
run shape by default — engine, perf profile, and catalogue debug runs alike.
Opt-outs: `DXMT9_OFFLOAD_COMMIT_REPLAY=0` (also releases the index-cache
default), explicit `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=0`. The probe
wrapper keeps pinning both explicitly (default off) as a deliberate
minimal-interference diagnostic shape. Remaining known follow-ups: SFIV
generalization check of the trio, and the H211-era soft items (long confirm,
readonly-cache kill-switch, PE hit counters).
