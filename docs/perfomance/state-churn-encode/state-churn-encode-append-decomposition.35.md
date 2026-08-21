---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 35
title: Entry Fast Path + Section Skips — Mechanisms Land, FPS Null-to-Marginal, Instrument Blindness Noted
date: 2026-08-21
type: experiment-run
status: accepted-mechanism
source: experiments/output/app-d3d9-3dmark05-sparse-phase-r2; experiments/output/app-d3d9-3dmark05-ef-b1; experiments/output/app-d3d9-3dmark05-ef-a1; experiments/output/app-d3d9-3dmark05-ef-a2; experiments/output/app-d3d9-3dmark05-ef-b2; experiments/output/app-d3d9-3dmark05-ef-a3; experiments/output/app-d3d9-3dmark05-ef-b3; experiments/output/app-d3d9-3dmark05-ef-b4; experiments/output/app-d3d9-3dmark05-ef-a4
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.34.md
---

# Entry Fast Path + Section Skips — Mechanisms Land, FPS Null-to-Marginal

Follow-through on [.34]'s two candidates. Both landed semantics-preserving
(`29660161` const-setter entry fast path, `c8a0e1e9` buildSparseState
delta-mode section skips). The retained sources are the nine experiment
output directories listed in the frontmatter; they support the run-specific
performance tables and diagnostic outputs below. No preserved artifact in
those sources records the previously reported host-suite `727/727` or
cold-server conformance results, so neither claim is used as current evidence
here. Conformance remains a separate correctness gate and must be cited from
a preserved runner report before being stated as a result of this experiment.

This leaf records PE shadow/producer fast-path mechanics shared with Render
Tape capture; it does not close the general Render Tape producer contract.
Sparse-delta bootstrap folding, `PresentEx`, reducible controls/destruction,
prior-output loads,
broader provider grammar, and the production capture-to-provider replay pin
remain open in the [Render Tape gap](../../../specs/experiments/gap.md).

**Design facts worth keeping.** (1) The entry fast path folds four per-call
diagnostic gates (call tracking == recorder stats, decimation N, VS
const-range probe, debug-log level) into one process-constant bool; every
member was verified read-once (`d3d9_pe_device.cpp:117-172`,
`d3d9_pe_decimated_scope.hpp:26-33`, `log.cpp:106-123`). With any diagnostic
enabled, the verbatim old body runs as a noinline slow path, so the
`dxmt9-core-device-com-spec` phase-offset pins hold unchanged. (2) The
pending render-state/TSS/transform tables were found already ctz-word-based
(`forEach` over 4-8 occupancy words, free when empty) — only the plain-mask
slot loops needed guards.

The temporary sparse-state phase probe used to obtain the `.34`/`.35`
measurement was retired after collection. It is not part of the production
path, and its counters are not current runtime evidence.

**Instrument blindness, stated explicitly:** with `DXMT9_PE_STATS_DECIMATION`
set, every setter routes through the slow path, so the decimated
`entry_const` scope structurally CANNOT observe the fast path it gates. Any
future entry-layer claim must come from the PE sampler or an fps bracket,
never from the decimated entry scope.

**Phase A/B (r1 pre-skip vs r2 post-skip, same instrument, same day):
net null at instrument resolution.** Parent `draw_packet` cal 3303 → 3346
ns/call (+1.3%, single-run drift). The internal deltas — skipped phases
−108 ns while the untouched render-states phase moved +136 ns — are not
causally credible at 20-140 ns scale; this is the Rosetta layout/OoO noise
band ([.27]'s lesson at phase granularity). No counter-level win is claimed
for the skips.

**FPS: 8-run same-day B-A-A-B + A-B-B-A bracket, parity-verified**
(intro-buildoptions equal on all three lanes, `winemetal.so` byte-identical,
per-run staged `d3d9.dll` sha distinguishes the two builds in every
`result.json`):

| lane | runs | median | mean | range |
|---|---|---:|---:|---|
| B = HEAD (both changes) | 4 | 26.59 | 26.75 | 26.55-27.27 |
| A = `42cfc86b` (base) | 4 | 26.48 | 26.44 | 26.10-26.69 |

Median **+0.41%**, mean +1.17% carried by one first-run outlier (27.27).
Verdict: **null-to-marginal** — compatible with the predicted ~0.5 ms/present
saving (~1.4%) at its lower edge, but not separable from noise at this
bracket's resolution. No fps win is claimed. Both changes are kept: they are
byte-identical on the wire, reduce disabled-path scaffolding that every
future diagnostic would otherwise ride on, and show no measurable regression
within this bracket (performance-neutral at the resolution of these runs).

**Ledger after this increment.** The producer track's cheap mechanical
candidates are now exhausted: entry scaffolding folded, section walks
guarded, scans O(1), mutex waits gone. What remains on the game-thread wall
is the app itself (~60-66%), touchConstShadow's irreducible call volume, and
the slack-absorbed lock path. The frontier stays where [.33] left it:
per-workload GPU/encode shape.
