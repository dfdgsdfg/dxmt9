---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: pe-recorder
order: 193
title: PE Const-Chain Overhead Cuts Land Clean But Move No FPS
date: 2026-07-07
type: no-gputrace
status: accepted-null-fps-result
source: experiments/output/app-d3d9-3dmark05-pe-constA-r10-20260707/result.json; experiments/output/app-d3d9-3dmark05-pe-constA-r11-20260707/result.json; experiments/output/app-d3d9-3dmark05-pe-cost-nostats-r8-20260707/result.json; docs/perfomance/present-pacing/present-pacing-pe-cost-verification.192.md
related: docs/perfomance/present-pacing/index.md
---

# Present-Pacing H193 - PE const-chain overhead cuts (Plan A)

## Question

H192 attributed the PE const chain (~5-7 ms/present of the ~10 ms PE budget)
largely to fixed per-record/per-append overhead: a no-op handle pipeline
dispatched for every handle-free const record, a recursive-mutex acquisition
per append (up to 7 per draw boundary), and a per-flush `std::vector`
construction. Does removing that fixed overhead move FPS under offload?

## Change (`cb372887`, kept — byte-identical wire, tests pinned)

1. `recordTypeIsHandleFree()` fast path in the recorder append funnel: the
   six `SET_*_CONST_*` types skip retain/collect/extra-handle entirely
   (equivalence verified against the general path's no-op tail).
2. One `recorderMutex_` acquisition across the const-flush + draw-append
   pair in all four draw appenders (recursive re-entry instead of repeated
   cold acquisitions).
3. Default flush path emits the merged dirty range directly with no
   `std::vector`; the `DXMT9_SPLIT_SPARSE_CONST_RECORDS` diagnostic path is
   unchanged.
4. Setter bookkeeping inspection: already stats-gated, no change needed.

Verified: 4 PE-record byte-pinning specs + backend suite 32/32 + 4 builds.

## Verdict

Clean landing, **no measurable FPS effect**: post-A offload scouts `2005` /
`2015` presents (median `2010`) vs the pre-A offload population
`1996/1999/2022` (median `~1999`) — `+0.55%`, inside the noise band.
`offload_commit_app_cpu_ms` unchanged (`1.13` vs `1.13`),
`gpu_command_buffer_errors=0`.

Reading: the removed dispatch/lock/vector overhead is real but small
(`~0.3-1 ms/present` at most, invisible at a `60 ms` frame), and H192's
overhead-corrected arithmetic (`±3 ms` error bars) over-attributed cost to
the per-append fixed machinery. The cuts stay (harmless hygiene; marginally
less work per append), but the result also weakens Plan B's premise
(inline const deltas in the draw packet to eliminate the 645 records):
if stripping most per-record overhead moved nothing, eliminating the
records saves only the residual header/arena/push costs — likely also
sub-noise, and not worth a wire-format change without new evidence.

## Next owner

Before any further PE-side optimization, the PE budget needs a
non-perturbing re-measurement (decimated every-Nth-event stats or an
external sampling method that can attribute Rosetta-translated PE frames);
H192's corrected total (`~10±3 ms`) is now suspect at the low end. The
alternative and currently better-supported move is consolidation: take the
two proven opt-ins (commit-replay offload `+11%` FPS, opaque-depth
index-cache at parity) through their formal promotion gates
(paired offload+opt-in `.gputrace` proof, longer confirm runs, default
decisions) rather than chasing a producer residual that is increasingly
likely to be the game's own Rosetta-translated CPU.
