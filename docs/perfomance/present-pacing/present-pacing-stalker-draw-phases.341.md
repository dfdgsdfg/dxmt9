---
domain: present-pacing
workload: STALKER CoP Day
title: "H341: Source-local DrawRun preparation versus encodeDraw"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-draw-phases-r682-off-day/{result.json,actual.png,supply.csv,waits.csv,progress.json}; experiments/output/app-d3d9-stkcop-bench-draw-phases-r683-on-day/{result.json,actual.png,supply.csv,waits.csv,progress.json}; experiments/output/draw-phase-validation-h341/{dxmt9-draw-phase-native-tests.log,dxmt9-draw-phase-unix-tests.log,dxmt9-draw-phase-tla.log}
related: present-pacing-stalker-command-phases.340.md
---

# H341: DrawRun preparation versus encodeDraw

Base `7274c32c` plus the nested draw observation overlay. No rendering,
query, scheduling or lifetime policy changes. Default-OFF
`DXMT9_CPU_SUPPLY_DRAW_SPLIT` requires command splitting and adds paired
events around serial encodeDraw calls in selected large sources. Body mask
0x2 explicitly records draw selection; mask 0x1 records command selection.
The analyzer checks pool/source/thread, parent DrawRun nesting, matching
command endpoints, clock monotonicity and nonnegative residual.

## Qualification and selection

R682 disables draw splitting; R683 enables it. Both enable command splitting.
Day/high 1280x720, Sikarugir cx24.0.7, perf; Metal occlusion, async query
fences, covered Pending and publication dedup ON. Tape, scoped CB join,
END coalescing, query/receipt/backend observers and PE sampler OFF.
Split/frame observers ON. Supply window 2247–2261, waits 2250–2256;
capture delay 160s, timeout 480s.

Both runners PASS without timeout or capture error. Inspected screenshots
show game scenes, not desktop fallback; this is not continuous pixel proof.
Five staged binaries match across runs and current outputs; Unix SHA-256
prefix `ec3516aea75327c1`. Builds/tests do not overlap these runs.
Both trace headers close with zero overflow/incomplete writers; wait traces
have zero unknown identities and **zero blocking CV entries**.

Consequently these runs do not reproduce the query-54 blocking-head case.
Explicit `--source-only` analysis selects the maximum encode CPU among
complete ordinary sources, with `target=null`. Normal receipt mode still
rejects missing matched receipts. Fixed Present windows are not stable scene
boundaries, and absence of waits here does not prove any optimization removed
the previous hitch.

| Evidence | R682 draw OFF | R683 draw ON |
|---|---:|---:|
| Process | 38587 | 45501 |
| Encoded Presents | 2786 | 2760 |
| Supply rows | 55,629 | 159,843 |
| Complete ordinary sources | 28 | 35 |
| Command-qualified sources | 14 | 15 |
| Draw-qualified sources | 0 (disabled) | 15 |
| Selected source sequence | 9622 | 9546 |
| Selected command count | 565 | 667 |
| Selected outer encode CPU | 33.038ms | 43.339ms |

Source identity is artifact/process plus queue owner and sequence; PID alone
is not a join key. Selected queue owners are 140271892008928 (OFF) and
140332956782560 (ON). Different source populations and command counts prevent
OFF/ON subtraction as an observer-overhead estimate. There is no repeated,
order-reversed comparison or observer-OFF FPS verdict.

## R683 source-local partition

| Scope | Calls | Native CPU ms | Wall ms |
|---|---:|---:|---:|
| DrawRun | 655 | 37.800 | 38.950 |
| Nested encodeDraw | 3583 | 27.178 | 28.243 |
| Preparation/pass/post-draw residual | — | 10.622 | 10.707 |
| Non-draw command | 12 | 0.439 | 0.437 |
| Per-record split policy | 667 | 0.472 | 0.469 |
| Dispatch/observer residual | — | 1.219 | 1.213 |

The nested rows partition DrawRun; they must not be added to their parent.
encodeDraw is 71.90% of observed DrawRun CPU; its scope includes argument
evaluation and internal binding/uniform/draw issue work. The remaining 28.10%
includes preparation, pass management, post-draw bookkeeping and observer
overhead, not pure pass cost. Call counts are not raw D3D draw counts.
Body CPU is 39.930ms; encodeChunk 40.112ms, framegraph planning 0.776ms,
backend 40.891ms, prefetch 2.418ms, outer encode 43.339ms (44.477ms wall).

Across 15 qualified sources, encodeDraw totals 347.893ms CPU over 46,969
calls; residual totals 133.910ms. These are selected source scope sums, not
per-Present costs or whole-pipeline budgets. R682's selected source records
27.698ms DrawRun over 559 calls; disabled nested observation is unqualified,
not zero encodeDraw cost.

## Decision and next gate

The observation separates actual encodeDraw calls from surrounding DrawRun
work but cannot attribute the previous late blocking head. First align scene
or semantic boundaries when repeating that investigation. Then distinguish
binding/uniform/driver ownership within encodeDraw and actual pass/setup work
using lower-interference sampling or bounded aggregates. Do not blindly add
more per-draw clocks or optimize the residual as though it were pure overhead.

## Verification

All four canonical builds pass. Native focused checks 6/6 and x86 Unix 3/3
pass; TLA regression passes in 218.34s. Clock calibration runs after builds
and TLA. Python analyzer tests pass 23/23, including nesting/identity/clock
rejection and explicit source-only selection without silent receipt fallback.
Source-only analyzer changes follow runtime collection and do not change the
measured binary. Validation logs are archived under the source paths above.
