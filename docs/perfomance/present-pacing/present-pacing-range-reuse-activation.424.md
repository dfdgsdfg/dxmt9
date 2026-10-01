---
domain: present-pacing
workload: STALKER CoP Day
title: "H424: Wild reuse activates; longest releases overlap real Encode work"
type: evidence
status: diagnostic-owner-identified-route-cost-open
updated: 2026-09-30
source: traces/query-ranges-h424/{analyze.py,analysis.json}; traces/query-ranges-h417/on-h424-active1/{invocation.json,completion.json,supply.csv,release.json}; experiments/output/app-d3d9-stkcop-bench-query-ranges-h417-on-h424-active1/{result.json,dxmt9.log,xrEngine_dxmt9.log}; src/d3d9/device_c_query_range_route.hpp; src/dxmt9/dxmt9_command_queue.cpp; tests/native/backend/render_tape_cpu_general_grammar.hpp
related: present-pacing-close-reuse-wild.423.md; present-pacing-session-release-cost.417.md; ../../../specs/backend/query-scope-ranges.md
---

# H424: Stop treating every ClosePass wait as redundant transport

HEAD `07257b76`, unchanged runtime from H422/H423. One ON diagnostic uses
the same Day/high configuration and five staged artifact receipts as H423.
`analyze.py` asserts both matches. Counters/frame sampling and the bounded
CPU-supply trace are ON; this is not a new FPS measurement. The catalogue run
passes, exits zero, reports zero GPU command-buffer errors, and yields a closed
trace for Presents 2000–2004 without overflow, incomplete or clipped releases.

## Actual activation and wait population

All 539 releases are DirectObservation/ClosePass; 534 have retained writers.
The producer reuses 226 acknowledgements (42.3% of retained requests).
The other outcomes are 251 `NoActivePass` and 62 `NoSession`.
These helper outcomes are not rejection reasons: the trace cannot distinguish
dirty context, missing receipt, changed generation or stale coverage.

| Population | Count | Caller round-trip total |
|---|---:|---:|
| All releases | 539 | 116.453 ms |
| Consecutive same-prefix releases | 524 | 6.227 ms |
| Same-prefix, not reused | 298 | 5.385 ms |
| Reused acknowledgements | 226 | 0.842 ms |
| Other releases | 15 | 110.226 ms |

The reuse row is a subset of the same-prefix population, not an additive row.
The remaining same-prefix transport is only 4.6% of this sampled release
elapsed total. Its complete removal would at most remove that observed scope
under fixed arrivals; this is not a whole-frame/FPS improvement bound.
Do not subtract H417's totals: the workload and query populations differ.

## Long waits are behind executing prefixes

The five largest requests all match the **same sequence identity** and the
same Encode thread as overlapping `chunk_body_begin/end` spans. The analysis
checks sequence identity rather than joining by time alone.

| Sequence | Release round trip | Encode body overlap | Whole body wall / CPU |
|---|---:|---:|---:|
| 5023 | 25.010 ms | 22.602 ms | 22.602 / 22.584 ms |
| 5011 | 19.566 ms | 19.041 ms | 20.061 / 19.975 ms |
| 5014 | 16.665 ms | 16.283 ms | 18.589 / 18.573 ms |
| 5020 | 16.486 ms | 16.114 ms | 18.339 / 18.319 ms |
| 5017 | 16.330 ms | 15.928 ms | 18.232 / 18.185 ms |

Together, 89.968 of 94.057 ms overlaps actual Encode body execution. Whole-body
CPU is not prorated to partial overlap. Near-equal CPU/wall supports CPU-active
work rather than a sleeping Encode thread in these spans; it does not identify
draw, binding, driver or split sub-costs. All five eventual close outcomes are
`NoSession`: that describes state at action time, **not** an empty queue or a
safe acknowledgement when the producer first posted the request.

The next two waits are 8.773 and 5.776 ms with only 0.005 ms of paired body
overlap each. They remain separately unattributed; the five large spans do
not explain every wait. No completion-order or prefix requirement is relaxed.

## Route audit and same-input limitation

The range lane excludes optimized routes globally, not only inside an active
query. In `src/dxmt9/dxmt9_command_queue.cpp`,
`beginDirectChunkSlotReplay` returns before admission whenever ranges are ON;
`beginCpuReadyArenaSource` and `beginCpuReadyArenaBatch` reject either active
occlusion **or the global range flag**. `queryRangeCompatibilityShortcut` in
`src/d3d9/device_c_query_range_route.hpp` explicitly sends ordinary draw/state
raws to compatibility unless capture is active. Capture disabling this shortcut
does not remove the queue's global exclusion. A raw with no query record may
still execute inside a scope begun by an earlier raw; absence of local query
records is not a safe admission witness.

This diagnostic again has zero Arena-to-Encode and session-head counts.
That establishes a real route restriction, not its isolated FPS cost.

The existing CPU general-frame harness rejects nonempty `plan.controls` with
`OrderedControlUnsupported` (`render_tape_cpu_general_grammar.hpp`). The general
GPU provider's OrderedControl execution currently accepts GammaRampSet only
(`device_c_render_tape_provider.cpp`). Therefore a full captured query/polling
transcript cannot yet provide the proposed identical-input route comparison.
No query events were stripped and no CPU route-cost number is claimed here.
The serialized query burst is a separate supported exact fixture, not full
STALKER tape support.

## Decision

Do not continue ack micro-optimization as the primary recovery strategy.
Reuse is active; the dominant sampled release waits overlap new-prefix Encode
work. Keep range default OFF and H423's negative performance verdict.

The next bounded investigation should attribute the identified source bodies
to draw/binding, pass/split and range-consumer work, using the existing bounded
trace vocabulary. In parallel planning, a route-isolation experiment needs a
query-aware immutable replay contract (including END/poll results and ordered
submission), or explicitly narrower query-free evidence. Simply enabling Arena
or deleting controls would change semantics and cannot answer the question.
Do not expand the full tape grammar merely to keep this candidate alive before
pricing the now-identified Encode bodies.
