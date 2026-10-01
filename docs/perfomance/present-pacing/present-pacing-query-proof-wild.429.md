---
domain: present-pacing
workload: STALKER CoP Day
title: "H429: Query store-proof elision has no resolved FPS benefit"
type: evidence
status: no-resolved-performance-benefit
updated: 2026-10-01
source: traces/query-proof-h429/{run.py,compare.py,comparison.json,current-buildoptions.json,pe64.log,pe32.log}; traces/query-proof-h429/{before,after}/build-x86_64-builtin/src/winemetal/unix/winemetal_dxmt9.so; traces/query-proof-h429/{after-aa2,after-forward2,before-forward2,before-reverse2,after-reverse2}/{invocation.json,completion.json}; traces/query-proof-h429/after-aa/{qualification.json,restored-scene.png}; experiments/output/app-d3d9-stkcop-bench-query-proof-h429-{after-aa,after-aa2,after-forward2,before-forward2,before-reverse2,after-reverse2}/{result.json,daybenchmark.result,actual.png,dxmt9.log,xrEngine_dxmt9.log}
related: present-pacing-query-proof-elision.428.md; present-pacing-drawrun-preparation.426.md
---

# H429: Whole-run FPS does not resolve a store-proof benefit

H428 is committed as `c671f5a6`. Five qualified observer-OFF Day runs complete
an initial same-image A/A and reversed-order before/after pairs. Their signs
disagree and their magnitudes are below the observed A/A difference.

## Configuration and provenance

Sikarugir CX24.0.7; Day/high; 1280x720 windowed; `max-threads 2`; sixteen sound
targets. Metal occlusion, async query fences and CPU-ready tape ON. Query ranges,
covered-Pending, query CB joining and END coalescing OFF. Perf counters, frame
sampling and validation OFF. Same harness screenshot request at 60 seconds.
No build, model checker or profiler runs alongside the workloads.

This measures the default range-OFF route, not H426's range-ON diagnostic route.
Do not map its FPS difference onto H426's 22.466 ms observed pass-entry bucket.

The installed Wine provider still matched H426's historical receipt before
staging. It was preserved as the before image (`19ac4abc19bd90be`) and compared
with the H428 image (`4e5e27df1df3c5e4`), both 10,933,504 bytes. H427 only added
documentation after H426. Each provider is staged through the catalogue runner
from a separate immutable snapshot path, with its full SHA checked before launch.
Both PE builds complete before measurement, and all four PE receipts remain
identical across the five runs and to H426. The before artifact set exactly
matches the H426 artifact receipt. There is no archived historical build-option
snapshot; the current canonical build options are retained, not mislabelled as
independent proof of old build-option parity.

`compare.py` verifies successful non-timeout exits, loaded provider identity,
equal PE artifacts, equal explicit environments and requested configuration,
perf/validation OFF, no capture errors and no perf/split trace output. Requested
config SHA is `1f17283bd2ac940ed4c813e5a186f332241d1335b2a1edbd90b7e8a93747205e`.
Final installed provider matches H428; no benchmark process remains.

## Game result-file average FPS

Execution order: after-AA, after-forward, before-forward, before-reverse,
after-reverse. The second A/A run is also the candidate in the first pair.

| Comparison | Before | After | After change |
|---|---:|---:|---:|
| After -> before | 21.650623 | 22.011763 | +1.668% |
| Before -> after | 21.738346 | 21.402302 | -1.546% |

The initial same-image A/A is 22.572407 -> 22.011763 FPS (-2.484%). The two
paired means are 21.694485 before and 21.707032 after (+0.058%). These means
are descriptive, not confidence intervals or a statistically established win.
Neither a stable FPS improvement nor a stable regression is resolved here.

## Visual and failure qualification

The initial `after-aa` attempt is excluded: the harness could not find an
on-screen game window and fell back to capturing Orca. Computer-use restored
the game window mid-run. The catalogue child still completed normally; its
batch supervisor was stopped/terminated to prevent automatic follow-on runs.
Its result and images are preserved, not pooled into the comparison.

The restored scene shows the outdoor building and terrain without obvious
broad corruption. This is a sampled visual smoke only. The inspected first
and last qualified-run captures show loading screens, so they are not evidence
of full-scene pixel equivalence or absence of intermittent flicker. Continuous
foreground and unrelated-host-load monitoring was not performed. The five
qualified runs complete without intentional mid-run UI manipulation.

Every qualified backend log retains the known warning about an incomplete
framegraph replay plan and source-order canonical fallback. Do not describe
the logs as warning-free. Perf is OFF, so no zero-GPU-error counter claim is
derived from these runs. H428's deterministic GPU/query and prepared-output
tests remain the stronger correctness evidence.

## Decision

Retain the narrow semantics-preserving computation elision, with no claim of
FPS improvement. Do not promote query ranges, update README FPS, or treat this
as closing the STALKER throughput gap. The native equivalence evidence and the
absence of a resolved performance change are separate conclusions.

The next useful experiment is identical-input pass preparation: distinguish
lookahead/proof work, actual encoder creation/configuration and perf-only summary
work. H428 intentionally keeps proof evaluation with perf ON, so another broad
perf-ON wild trace cannot price the eliminated product-path work. ContinueActive
versus real pass transitions remains an unpriced split. Broader temporal visual
qualification remains open.
