---
domain: present-pacing
workload: STALKER CoP Day
title: "H423: Closed-prefix reuse passes wild smoke but ranges remain slower"
type: evidence
status: performance-gate-failed-default-off
updated: 2026-09-30
source: traces/query-ranges-h423/{compare.py,comparison.json,on-outdoor.png}; traces/query-ranges-h412/wild.py; traces/query-ranges-h412/wild-{on-h423-visual1,off-h423-forward1,on-h423-forward1,on-h423-reverse1,off-h423-reverse1}/{invocation.json,completion.json}; experiments/output/app-d3d9-stkcop-bench-query-ranges-h412-{on-h423-visual1,off-h423-forward1,on-h423-forward1,on-h423-reverse1,off-h423-reverse1}/{result.json,daybenchmark.result,dxmt9.log,xrEngine_dxmt9.log}
related: present-pacing-close-reuse-integration.422.md; present-pacing-query-range-wild.416.md; ../../../specs/backend/query-scope-ranges.md
---

# H423: Reuse does not close the range-policy performance gate

Runtime commit `9988a70c` integrates H422. The Unix provider and both PE
staging builds completed before testing. No code, builds, test suites or
external profilers ran alongside the measured workloads.

## Configuration and receipts

Sikarugir CX24.0.7, Day/high, 1280×720 windowed, two game threads and sixteen
sound targets. Metal occlusion, async query fences and CPU-ready tape are ON;
covered-Pending, query CB joining and END coalescing are OFF. Only
`DXMT9_QUERY_SCOPE_RANGES` differs between performance policies. Counters,
frame sampling and validation are OFF. Each run keeps the harness screenshot
at 60 seconds; the separate visual run includes additional computer-use reads.

The comparison script checks successful non-timeout exits, identical five
staged binary receipts and requested configuration, equal environments except
the policy, and absence of performance/split diagnostics. Config SHA-256 is
`1f17283bd2ac940ed4c813e5a186f332241d1335b2a1edbd90b7e8a93747205e`.
The Unix provider receipt is `ccc114c2aba630b2` (10,933,504 bytes).
Full receipts are in `comparison.json` and each `result.json`.

## Observer-OFF result

Game result-file average FPS; the additional visual run is not in these pairs.

| Execution order | OFF | ON | ON change |
|---|---:|---:|---:|
| OFF → ON | 21.615522 | 19.527084 | −9.66% |
| ON → OFF | 21.522850 | 19.696379 | −8.49% |

All five runs pass and exit zero, with no timeout or admission warnings.
The OFF backend logs contain a source-order canonical fallback warning for an
incomplete framegraph plan. OpenAutomate plugin/OA-not-initialized messages
also occur; these are not clean-error-free logs. The launcher removes previous
`.result` files before launch and requires fresh results, and every run produced
its own Day result. Such messages also occurred in prior successful native
benchmark runs; they do not by themselves invalidate the result file.

The separate ON visual run shows outdoor buildings, foliage and ground without
obvious broad corruption in the saved scene. This is a sampled smoke check,
not full-scene pixel equivalence or a continuous flicker test. No focus changes
were intentionally made during the four performance runs; continuous foreground
and unrelated-host-load monitoring was not performed.

## Decision and next experiment

Keep ranges **default OFF**. Both orders still show a policy-level regression.
H422's exact fixture proves actual reuse with preserved samples/pixels, but
this observer-OFF run does not count reuse activation in STALKER. Nor is this
a reuse-only A/B: OFF also changes the range route. Do not infer that reuse
itself regresses FPS, or subtract H416 from these runs as its isolated benefit.

The next discriminating experiment is a separate, bounded production trace
that measures actual reuse versus dirty/stale fallbacks, followed by identical
input route-cost isolation. H416 observed zero Arena/session activity and more
compatibility batches with ranges ON. Distinguish that global route exclusion
from range scanning/append costs before adding further ack micro-optimizations.
Do not simply enable Arena under ranges: preserve query membership, ordered
boundaries, completion identity and the GPU oracle first.
