---
domain: present-pacing
workload: STALKER CoP Day
title: "H340: DrawRun owns the selected source's command-body cost"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-command-phases-r680-off-day/{result.json,actual.png,supply.csv,waits.csv,progress.json}; experiments/output/app-d3d9-stkcop-bench-command-phases-r681-on-day/{result.json,actual.png,supply.csv,waits.csv,progress.json}; experiments/output/command-phase-validation-h340/{dxmt9-command-phase-native-tests.log,dxmt9-command-phase-unix-tests.log,dxmt9-command-phase-tla.log}
related: present-pacing-stalker-chunk-phases.339.md
---

# H340: DrawRun, non-draw, split and dispatch attribution

Base `b3b2f4a4` plus the uncommitted H339 and command-split observation
overlays. `DXMT9_CPU_SUPPLY_COMMAND_SPLIT` is default OFF and requires the
bounded supply trace. It selects serial sources with at least 128 input
commands; suppressed-effect fixtures and requested parallel partitions are
excluded. A body event explicitly marks selection. No runtime rendering,
query, queue or lifetime policy changes.

Command begin/end pairs classify DrawRun versus non-draw execution. Separate
pairs bracket `applyPerRecordSplitPolicy`. The analyzer requires disjoint,
same-pool/sequence/TID pairs, one split per executed command, valid clocks and
nonnegative residual. Dispatch residual is body minus these scopes and
includes observer work between endpoints. A DrawRun is a backend command,
not one D3D draw. Its internal pass opening/closing remains inside DrawRun;
non-draw helper/pass work remains inside the non-draw category. This does
not independently separate every pass operation.

## Same-image OFF/ON qualification

R680 disables only command splitting; R681 enables it. Both retain the
source-level diagnostics. Day/high 1280x720, Sikarugir cx24.0.7, perf; Metal
occlusion, async fences, covered Pending and publication dedup ON. Tape,
scoped CB join, END coalescing, query/receipt/backend observers and PE sampler
OFF. Split/frame observers ON. Supply 2235–2275, wait 2240–2270.

Both runners PASS, no capture error, failure or timeout. Both inspected
captures show the game (different scene positions, not a pixel differential).
All five staged images are identical and match current build outputs; Unix
hash prefix is `69ddf83ce8b9a5eb`. There is no overlapping task-owned build or
test; unrelated host activity and scene/scheduling drift are not excluded.

| Evidence | R680 OFF | R681 ON |
|---|---:|---:|
| Process | 4636 | 9558 |
| Encoded Presents | 2640 | 2566 |
| Supply rows | 73,989 | 193,520 |
| Complete sources | 1144 | 2336 |
| Complete large sources | 60 | 81 |
| Command-qualified sources | 0 (disabled) | 81 |
| Large-source command-count median | 272 | 270 |
| Large-source body CPU median | 12.596ms | 16.602ms |

Both traces close with zero overflow/incomplete writers; wait traces have
zero unknown identities. These populations differ substantially. The increased
ON cost is a perturbation warning, not an isolated estimate of observer
overhead. One OFF/ON pair with unmatched source content cannot calibrate a
subtraction or establish a product FPS regression. Neither run is globally
observer-OFF.

## Selected source in R681

The longest matched query-54 receipt targets raw 383120 at frame 2257,
receipt 21.315959ms. It publishes 55 sources; the head is sequence 13055,
306 commands, whole encode CPU 21.770ms. Its body partition is:

| Body category | Calls | Native CPU ms |
|---|---:|---:|
| DrawRun, including its pass work | 306 | 18.545 |
| Non-draw commands/helper passes | 0 | 0.000 |
| Per-record split policy | 306 | 0.281 |
| Dispatch/observer residual | — | 0.934 |
| Body total | — | 19.760 |

DrawRun is 93.85% of this observed body. Zero non-draw means no such commands
in this source, not zero render-pass work. Split-policy duration includes
timestamp/recording overhead; it is not evidence that actual CB splits cost
0.281ms. CPU and wall readings around these short calls can differ by small
amounts within the existing per-pair clock tolerance; do not derive negative
waits from them.

The main cap wait is 25.958ms at inflight 31, with only sequence 13055 retiring
inside it. Three smaller waits are also recorded. Target coverage is 55/55.
R680's selected source is different: sequence 9964, raw 381172, frame 2270,
277 commands, body CPU 13.004ms and cap wait 18.358ms. These are not the same
source, so subtracting the two head times would not price instrumentation.

Across all 81 qualified ON sources: DrawRun CPU totals 1249.710ms over 21,503
calls; non-draw 8.298ms over 240 calls; split policy 16.817ms over 21,743 calls;
dispatch residual 54.244ms. These are selected encode-thread scope sums,
not per-frame budgets or whole-pipeline costs. Small sources remain excluded.

## Decision and next gate

The measured owner is DrawRun work, not non-draw command handling or split
dispatch. Next separate DrawRun preparation/pass management from actual
encodeDraw work, then binding/uniform/resource/driver owners as warranted.
Prefer lower-interference sampling or bounded source aggregates; high-frequency
native CPU clocks demonstrably need a control and cannot be assumed free.
Do not optimize away 0.934ms dispatch based on this intrusive residual.
Pass work within DrawRun and pure product CPU remain attribution gaps.

## Verification

All four canonical builds pass. Native checks 6/6, x86 Unix 3/3, TLA regression
214.63s pass; clock tests run after TLA/builds. Python 22/22 passes, including
full nested analyzer joining, command/split count coverage, overlap/unpaired
rejection, disabled/missing classification, CPU monotonicity and residual
conservation. Diff and explicit leaf source-evidence audits pass.
