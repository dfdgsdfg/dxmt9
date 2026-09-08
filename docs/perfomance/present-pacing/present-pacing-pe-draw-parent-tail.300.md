---
domain: present-pacing
workload: actual PE public COM; 3DMark05 GT2; 3DMark06 GT2
title: "Draw-parent tail attribution: GetData FLUSH dominates the fixture"
type: evidence
status: diagnostic-qualified-economics-unqualified
updated: 2026-09-09
source: experiments/output/pe-draw-parent-tail-H300-20260909
related: present-pacing-pe-draw-parent-groups.299.md; present-pacing-pe-draw-parent-harness.298.md; present-pacing-pe-identity-call-abi.297.md
---

# Draw-parent tail attribution

H300 starts at `b229ddda` and resolves H299's three-call attribution question.
All five production images remain identical to H297/H298/H299. Only the test
fixture, driver/parser and diagnostic contracts change; this round does not
change production query behavior, optimize a helper or refresh wild FPS.

## Diagnostic contract

R-BENCH-7.6 adds default-off `--tail-detail`, which implies group detail.
Schema 3 pairs every buffered group with a case/path/ordinal-qualified tail
supplement.
Disjoint QPC pairs surround EndScene, event Issue and every GetData call;
normal HRESULT accumulation is inside these spans. Their sum plus a checked
remainder equals the outer tail exactly. The remainder includes clock,
control and retry overhead; no retry or slow sample is discarded.

The fixture retains its original HRESULT, deadline, retry and first-S_OK
behavior. No extra clocks enter the setter/draw interval. All sample storage
is allocated before row timing and output waits until the matrix completes.
The fixture's sample structure grows, and clocks/stores outside the timed
pair can affect later scheduling: schema compatibility is not fixture-cost
parity. This is a separate diagnostic recipe.

The parser requires immediate matching group/tail records, exact coverage and
component conservation. It reports per-case/path warm/measured distributions
separately. Schemas 1 and 2 retain their original parsed output without
invented tail components. Native negative pins cover wrong identities,
missing/duplicate/out-of-order samples, mixed versions, integer overflow,
non-conserving totals and warm-phase contamination.

## Verification and provenance

Four canonical builds and four focused Meson checks pass, including 23
parser/CLI tests. A separate main-thread audit re-parses 20 historical
H298/H299 raw transcripts to exactly match their frozen parsed receipts.

Ten actual PE processes pass 270 public-COM state/input/HRESULT/pixel/release
cells. Six short traces cover aggregate, group-detail and tail-detail modes
in PE32 and PE64; each has 324 balanced markers, 195 chunks and no timed
recorder flush. Four logging-OFF tail-detail processes, two per width, each
retain 14,040 groups (27 rows × 520 warm/measured groups). Across all runs,
**56,484 group samples and 56,322 tail supplements** close exactly.

`tail-analysis.json` requalifies raw logs and all recipe, source, build,
fixture/tool, staged-image and public-vtable-binding receipts. Sikarugir
PE32 is WoW64/Rosetta; PE64 is not native ARM64 PE. The fixed H298 recipe
remains 32 iterations, 512 measured + eight warm groups, replay offload on
and recorder/perf diagnostics off outside the separate traces. No build,
native test or second benchmark overlaps a timing process. No new TLA or
wild run is claimed for this fixture-only change.

## Result: the first GetData call owns almost all tail time

All values below include warm and measured groups. They are raw disjoint
wall scopes, not subtracted or exclusive PE costs.

| Whole-matrix component, two runs per width | PE32 | PE64 |
|---|---:|---:|
| Setter/draw group QPC | 0.519–0.549 s | 0.321–0.419 s |
| EndScene | 0.363–0.395 s | 0.307–0.386 s |
| Event Issue | 0.076–0.084 s | 0.052–0.065 s |
| GetData | 22.330–23.096 s | 21.232–25.018 s |
| Clock/control/retry remainder | 0.032–0.034 s | 0.017–0.021 s |
| GetData share of outer tail | 97.83–97.94% | 98.15–98.26% |
| Summed whole-row process CPU | 20.66–21.67 s | 19.12–23.31 s |

There are exactly 14,040 GetData calls per timing process, one per group;
the fixture's Sleep(0) retry loop is never entered. This rules out repeated
GetData polling or the retry sleep as the source of this observed tail.
EndScene includes recorder flush and is material beside the short setter/draw
scope, but it does not explain the 21–25 seconds attributed to GetData.

H299's source audit explains why a single call can wait: public GetData
flushes the PE recorder, its bridge may drain deferred replay, and the
provider's `Device::getQueryData` FLUSH branch reaches queue
`flushAndWait` before the final nonblocking query predicate. This experiment
locates wall time in the public GetData call, **not** among those internal
waits, CPU work or GPU execution. Whole-row process CPU includes all process
threads, setup, warm-up, drain and oracle work; its scope cannot be relabeled
as PE-exclusive CPU. Neither table implies a GT2 GPU bottleneck.

## Consequence for PE optimization

The parent harness currently puts a full FLUSH observation after every small
32-iteration group. H299 already showed that central timing populations shift
and correlate with preceding tails. H300 identifies the dominating public
boundary; it does not establish a causal scheduling explanation or stable
small-delta economics. Trimming slow groups, changing Sleep(0), or subtracting
GetData from process CPU would not supply that missing proof.

The next calibration option is a separately qualified, bounded producer window
with fewer group-level completion drains, retaining mandatory final drain,
source order, exact call counts and pixel/getter oracles. That is a fixture
policy experiment, not permission to weaken production query completion.

The next concrete production candidate remains the private
`pinPreparedQualifiedBinding` result in
`src/d3d9/d3d9_pe_semantic_owner.hpp`: its sole caller discards the pin ordinal
and always provides a wire-index destination. Removing only that dead output
and making the destination a reference must retain the seven-family physical
pin, identity, pre-retain snapshot, failure and rollback checks of
R-CORE-REC-7.36. Linked PE32/PE64 code reduction, native exact-byte/refcount
coverage, actual public-PE evidence and wild gates remain required; H300 does
not pre-accept this candidate or claim a CPU/FPS saving. H297 remains the last
wild FPS evidence, with its documented 06 health-tail limitation.
