---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H414: Ordered query replay publishes before the range consumer"
type: evidence
status: production-integration-gap-default-off
updated: 2026-09-29
source: traces/query-ranges-h412/{comparison.json,compare.py,wild.py}; traces/query-ranges-h412/wild-{on-reverse1,off-reverse1,off-diag1,on-diag1}/{invocation.json,completion.json}; experiments/output/app-d3d9-stkcop-bench-query-ranges-h412-{on-reverse1,off-reverse1,off-diag1,on-diag1}/{result.json,daybenchmark.result,dxmt9.log,xrEngine_dxmt9.log}
related: present-pacing-query-range-routing.413.md; present-pacing-query-range-integration.412.md; ../../../specs/backend/query-scope-ranges.md
---

# H414: Native range success did not cover production ordered-query replay

No runtime changes after H413. Same five staged binary receipts and requested
config hash across all six H413/H414 runs; Unix receipt `1fd47ba21a58c1c9`.
Sikarugir, Day/high, 1280x720, two game threads, 16 sound targets, actual
occlusion and CPU-ready tape ON. Only query ranges differ within each pair.
All four new runs pass and exit zero without timeout or admission warnings.
No concurrent builds or external CPU/GPU sampling. No new exact-image claim.

## Observer-OFF reverse repeat

| Order | Ranges OFF FPS | Ranges ON FPS | ON difference |
|---|---:|---:|---:|
| OFF then ON (H413) | 24.118372 | 21.300428 | -11.68% |
| ON then OFF (H414) | 23.889671 | 21.421894 | -10.33% |

Both orders reproduce the direction; two samples per policy are not a broad
statistical confidence interval. This is a within-image policy result, not a
comparison against old D9VK or default-occlusion-OFF scores.

## Separate diagnostic pair

Perf/frame counters, query split observer, backend count observer and total-only
replay-emission-plan observer enabled identically. No per-record timing, trace
sampling or policy changes. Diagnostic FPS is not performance evidence.
The table uses final cumulative counters divided by encoded Presents (2,569
OFF, 2,232 ON). Loading and different scene progress populations remain;
these are not paired source identities or exclusive thread CPU times.

| Whole-run observation per Present | OFF | ON |
|---|---:|---:|
| Draw calls | 2654.43 | 2632.70 |
| Compatibility batch calls | 38.01 | 82.95 |
| Compatibility draws | 2545.51 | 2632.70 |
| Compatibility emitted DrawRuns | 354.37 | 425.42 |
| Compatibility append elapsed, ms | 2.464 | 2.634 |
| Compatibility resource mark elapsed, ms | 0.925 | 0.681 |
| Publication preparation elapsed, ms | 0.858 | 0.711 |
| Arena sources reaching encode | 45.13 | 0 |
| Legacy sources reaching encode | 21.62 | 14.65 |
| Session heads appended | 51.72 | 0.50 |
| CB creations | 21.01 | 18.00 |
| Render passes | 32.45 | 32.70 |
| Offload replay elapsed, ms | 39.42 | 33.25 |
| EncodeChunk elapsed, ms | 20.87 | 18.23 |
| Encode dequeue-to-CB-commit stage, ms | 23.89 | 28.95 |
| Offload worker idle elapsed, ms | 37.95 | 54.90 |
| PSO prefetch elapsed, ms | 1.160 | 1.678 |

Both runs report zero GPU CB errors, zero unbatched compatibility draws, zero
Direct Slot raws, and zero `planReplayEmission` calls. That observer does not
time `planCpuReadyChunk`; zero is not proof of free Arena planning. OFF uses
115,947 Arena sources, not the Direct Slot route initially suspected. Ordinary
publication counts alone omit this family. Tail-CB observation represents
171,487 sources / 36,072 tail CBs OFF (4.75 sources/CB), versus 32,704 / 30,503
ON (1.07). Mid-source CBs and initializers are outside that tail ledger.

Append grows modestly while marking, replay and EncodeChunk scopes fall.
These scopes overlap waits and other stages; do not sum them or subtract them
from stage wall to manufacture a missing CPU cost. Larger outer encode-stage
wall and worker idle are scheduling/supply symptoms, not GPU saturation proof.
The data does not support blaming all of the FPS loss on append/reservation.

## The missing integration contract

ON reports **zero deferred-new END, deferred-shared END, FLUSH publication and
marker binding**. This is not the useful deferred-range behavior demonstrated
by H412's direct core-call fixture.

The actual immutable wire route is:

1. `device_c_ordered_control.cpp::makeOrderedControlDisposition` classifies
   Query Issue as ordered `ClosePass`.
2. `device_c_chunk_replay.cpp::replayResolvedChunk` calls
   `releaseCpuReadySessionBeforeOrderedControl` before dispatching that record.
3. `dxmt9_command_queue.cpp::releaseCpuReadySessionBeforeOrderedControl`
   unconditionally commits the current writer as `SemanticBoundary`, posts an
   ordered release and waits for encode acknowledgement.
4. Only then do query membership replacement and END reach
   `setOcclusionScope` / `publishQueryEnd`. Their populated-writer range path
   has already lost the writer it was intended to retain.

This explains why the candidate can pay process-wide Arena exclusion and lose
session packing without using deferred ENDs in this workload. It is a concrete
integration defect, not a quantified decomposition of the entire FPS gap.
No evidence yet independently attributes bounded range lookup cost.

## Next bounded implementation

First reproduce `Draw -> BEGIN -> Draw -> END -> FLUSH` through the real
serialized/imported replay route, not direct core calls. Require positive
range/receipt activation evidence, exact query results and pixel readback.
Then refine Query Issue release authority: distinguish result-membership
boundaries from readback/update submission or completion obligations. Keep
origin-qualified contributions, END-prefix FLUSH, reset/terminal and exactly-once
acknowledgements. Do not simply delete the release call or weaken all controls.
Bind the refined transition to native and temporal tests before another wild
promotion run. Keep default OFF and preserve the reference route.
