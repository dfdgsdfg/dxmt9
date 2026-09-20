---
domain: present-pacing
workload: STALKER CoP Day
title: "H339: command execution dominates encodeChunk"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-chunk-phases-r679-day/{result.json,actual.png,supply.csv,waits.csv,progress.json}; experiments/output/chunk-phase-validation-h339/{dxmt9-chunk-phase-native-tests.log,dxmt9-chunk-phase-unix-tests.log,dxmt9-chunk-phase-tla.log}
related: present-pacing-stalker-framegraph-phases.338.md
---

# H339: source setup versus command execution versus finalization

Base `b3b2f4a4` plus an observation-only overlay. FrameGraphBackend binds its
resource pool to the source call in diagnostic storage. encodeChunkImpl
brackets `encodeSelectedCommands` with pool/sequence-qualified body events.
The analyzer joins backend and pool identities, source sequence, thread and
enclosing intervals. Three extra events per source; no per-draw clocks,
runtime identity fields, query policy or synchronization changes.

## R679 qualification

STALKER Day/high 1280x720, Sikarugir cx24.0.7, perf. Metal occlusion, async
fences, covered Pending and publication dedup ON. Tape, scoped CB join, END
coalescing, query/receipt/backend observers and PE sampler OFF. Split and
per-frame observers ON. Supply window 2220–2290 and wait window 2230–2280
are narrower than R678 to preserve the fixed sink budget.

Runner PASS, 2638 encoded Presents, no timeout, capture error or failures.
The inspected screenshot contains the game; this is not continuous pixel
equivalence. PID 65339; supply contains 159,255 events, zero overflow and
incomplete writers. Wait trace closes with zero overflow/incomplete/unknown
rows. All 2690 complete sources have chunk partitions, including all 55
target publications. All five staged artifact hashes match built outputs;
Unix hash prefix `5c4ca3f3669e4474` is in result.json.

Builds/tests finish before launch. Host interference and observer cost are not
excluded; this run cannot establish FPS gains or regression. The selected
source and window differ from R678; 16.957ms here versus 13.407ms there is
not a matched before/after comparison.

## Target and phase partition

Longest matched query-54 receipt: ledger 1, raw 380554, encoded frame 2255,
worker TID 1541803. Receipt elapsed 22.994834ms; raw service 29.881ms wall /
4.775ms CPU includes 23.215ms cap wait. These scopes overlap.

The 55 SemanticBoundary sources again comprise one prelude (sequence 9840,
288 commands) and 54 one-command scopes. The main cap wait is 23.112ms,
31→30, with only source 9840 retiring inside it. A second 0.103ms wait
observes retirement of 9855 and 9856, 31→29. Retirement-in-interval does not
identify the OS notification that woke the waiter.

| Head encodeChunk scope | Wall ms | Native CPU ms |
|---|---:|---:|
| Setup / partition preparation | 0.076 | 0.076 |
| Command execution body | 16.727 | 16.719 |
| Finalize / return | 0.162 | 0.162 |
| Total encodeChunk | 16.965 | 16.957 |

The body accounts for 98.60% of encodeChunk CPU. It includes command cursor
dispatch, DrawRun encoding, non-draw commands, pass transitions and in-loop
split policy. It is not exclusive draw or Metal driver CPU. Finalize/return
includes tail pass closure, submission creation, destruction and autorelease
cleanup. Native CPU here describes the calling encode thread, not independent
parallel-child workers.

For context, the head has 18.246ms outer encode CPU, including 0.900ms PSO
prefetch, 0.352ms framegraph planning/setup and 0.005ms backend cleanup.
Encode wall is 18.288ms; encode-end→submit 0.482ms;
submit→completion-dequeue 0.028ms; dequeue→completion-observed 6.001ms;
observed→retirement 0.063ms. These elapsed stages across sources overlap.
Completion observation is not a GPU execution timestamp.

## Decision

Do not prioritize source setup/partition, framegraph planning or tail
finalization over the command body for this blocking-head case. Next split
draw-run work, non-draw/pass work and dispatch/split residuals within the
same qualified source. Prefer bounded source-level accumulation or sampling;
price any higher-frequency timers before treating their values as product
cost. Resource binding, uniform uploads and driver calls remain unassigned.
Source batching still requires separate scope/END receipt refinement; no
policy promotion follows from this observer.

## Verification

All four builds pass. Native focused checks 6/6, x86 Unix 3/3 and TLA
regression (207.55s) pass; CPU-clock fixtures run after TLA/build completion.
Python 21/21 passes: nested joins and partition sums, wrong pool/sequence,
missing/duplicate bindings and endpoints, thread mismatch, invalid clocks.
Old/unsupported traces remain unqualified rather than zero-filled. Diff and
leaf source-evidence checks pass. Existing duplicate-library warnings remain.
