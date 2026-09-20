---
domain: present-pacing
workload: STALKER CoP Day
title: "H338: encodeChunk dominates the blocking source, not framegraph planning"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-framegraph-phases-r678-day/{result.json,actual.png,supply.csv,waits.csv,progress.json}; experiments/output/framegraph-phase-validation-h338/{dxmt9-fg-phase-native-tests.log,dxmt9-fg-phase-unix-tests.log,dxmt9-fg-phase-tla.log}
related: present-pacing-stalker-encode-phases.337.md; present-pacing-stalker-publication-progress.336.md
---

# H338: backend planning versus encodeChunk

Base `77569b48` plus an observation-only overlay. The ordinary queue records
its backend interface identity at backend entry. FrameGraphBackend brackets
all three encodeChunk call paths with that identity, source sequence, TID and
disposition: 1 = planning bypass, 2 = activated replay plan, 3 = natural-order
final path. The analyzer requires a unique pair inside the enclosing ordinary
backend call. No Slot/wire fields, scheduling or query policy change.

Planning/setup includes wrapper, observer, graph construction, optimization
and plan preparation before encodeChunk. Cleanup/return includes destruction
after encodeChunk. These are disjoint observed CPU/wall scopes, not exclusive
optimizer instruction measurements. Missing or unsupported paths are
unqualified; duplicate, mismatched or nonmonotone endpoints are rejected.

## R678 qualification

Day/high 1280x720, Sikarugir cx24.0.7, perf profile. Metal occlusion, async
fences, covered Pending and publication dedup ON. Tape, scoped CB join, END
coalescing, query/receipt/backend observers and PE sampler OFF; split/frame
observers ON. Supply window 2190–2310, wait window 2200–2300.

Runner PASS, 2625 encoded Presents, no timeout, failures or capture error.
Screenshot inspected: game scene, not desktop fallback; not continuous pixel
proof. PID 38587; both traces close without overflow/incomplete writers and
the wait trace has no unknown identities. Supply has 253,664 events, close to
the 262,144-row bound: narrower windows are advisable for finer future probes.
All 4337 complete sources have qualified framegraph endpoints, including the
selected 55-source target. The five staged hashes match current build outputs;
Unix hash prefix `a7b54f344e520396` is in result.json.

No task-owned build/test overlaps the run. `dasd` was using approximately one
core before launch. Do not claim interference-free execution, observer cost
qualification or an FPS improvement from this diagnostic.

## Matched blocking source

Longest matched query-54 receipt: ledger owner 1, raw 382304, frame 2257,
worker TID 1469319. Receipt elapsed 22.165084ms; target raw service is
26.344ms wall / 5.573ms native CPU, containing an 18.617ms cap wait.
These scopes overlap and must not be summed. The cap starts at inflight 31
and returns at 30; sequence 10383 is the only retirement inside that wait.

The raw publishes 55 sources: sequence 10383 contains 282 commands and the
remaining 54 contain one each, all SemanticBoundary. This reproduces the
prelude-plus-scope-closures pattern, not the identical source from R677.

| Head source scope | Wall ms | Native CPU ms |
|---|---:|---:|
| Whole backend | 13.768 | 13.749 |
| Planning/setup | 0.337 | 0.337 |
| encodeChunk call | 13.426 | 13.407 |
| Cleanup/return | 0.005 | 0.005 |

The three child rows partition the whole backend. encodeChunk owns 97.51%
of backend CPU; planning/setup owns 2.45%. The head uses disposition 3;
the remaining 54 scopes all use disposition 1, as expected for scoped sources.
Natural-order disposition does not mean that graph construction/optimization
was skipped; it means the final encode call uses natural command order.

Whole source encode is 14.735ms wall / 14.694ms CPU, including 0.919ms
prefetch CPU. Publication-to-encode is 0.086ms, encode-end-to-submit 0.157ms,
submit-to-completion-dequeue 0.051ms, dequeue-to-completion-observation 6.015ms
and observation-to-retirement 0.087ms. CPU-observed completion is not a hardware
GPU timestamp. The 0.001ms prefetch CPU-over-wall rounding is within the
existing 100us calibrated-clock tolerance, not a negative wait estimate.

## Decision and next gate

For this blocking head, framegraph planning is not the dominant CPU owner.
Prioritize source-qualified encodeChunk internals: setup/partition planning,
command dispatch and draw encoding, pass management, and finalization. Only
after measuring those owners should binding/uniform/driver-level detail be
enabled. encodeChunk time must not yet be called per-draw or Metal driver CPU:
it includes all work performed by the wrapper and implementation.

Source batching and scope/END receipt refinement remain independent. The
large encode head and small publications still interact through inflight
credit. No framegraph disablement, capacity increase or source merging is
authorized by these timing results.

## Verification

All four canonical builds pass. Native focused checks pass 6/6, including
framegraph backend/optimizer; x86 Unix checks pass 3/3. TLA regression passes
in 197.50s before CPU-clock tests and the wild run. Python tests pass 20/20,
covering all dispositions, partition sums, real analyzer joining, old/missing
bindings, wrong owner/sequence, duplicates, TID mismatch and invalid clocks.
R677 reanalysis correctly yields zero qualified framegraph partitions rather
than fabricating missing data. Diff/source-evidence checks pass. Existing
unused-variable/duplicate-library warnings remain outside this change.
