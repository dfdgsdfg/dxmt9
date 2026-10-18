---
domain: present-pacing
workload: STALKER H502/r5 original V4 input
title: "Same-image nesting exposes an overbroad append scope"
type: evidence
status: same-image-nesting-qualified-scope-correction
updated: 2026-10-16
source: traces/stkcop-append-nesting-h590/{contract.md,run.py,report.json,verify.py,verification.json}; traces/stkcop-append-nesting-h590/{parent0,nested0,nested1,parent1}-batch.bin; traces/stkcop-append-nesting-h590/{parent0,nested0,nested1,parent1}/{census.json,source.bgrx,output.bgrx}
related: present-pacing-batch-residual.589.md; present-pacing-append-tail.591.md
---

# The unexplained time includes work after the mutation callback

H590 reuses H589's exact executable and dynamic images without rebuilding.
Four fresh processes retain private phase CPU observation, in frozen order
parent0(mode0), nested0(mode1), nested1(mode1), parent1(mode0). Mode1 enables
product perf and existing inner append clocks together; their separate
overheads cannot be inferred. No reruns, Wine staging or product changes occur.

All four pass original source/output readbacks, seven source boundaries,
query transcript, 670 balanced wrappers and normal-state restoration. Each
retains 84 batches, 2,828 draws, 2,343 entries and 924 private phase rows.
Independent verification authenticates hashes and recomputes raw phase bounds.

Values are ms per captured interval, not per live frame:

| Same-run measurement | nested0 | nested1 |
|---|---:|---:|
| H589 named Append phase, caller CPU | 10.410–11.199 | 9.782–10.558 |
| H589 named Append phase, wall envelope | 10.797–10.914 | 10.169–10.273 |
| Inner Append, elapsed | 4.004746 | 3.484737 |
| Product append-loop counter, elapsed | 4.428 | 3.895 |
| Outer wall minus inner elapsed | 6.792–6.909 | 6.684–6.788 |

Both nested runs count 84 inner reservations and 2,343 inner appends. The
parent-only CPU bounds for the broad phase are 9.526–10.301ms and
9.098–9.878ms. These are observed costs, not removable-work estimates.
The product append counter is whole-process elapsed, not armed-only CPU.

## Boundary correction

H589 starts its phase before the append-loop `PerfScope` inside the mutation
lambda, but ends **after** `mutateWritingPayload` returns. In
`src/dxmt9/dxmt9_queue.hpp`, that template performs capacity-lease settlement
when qualified and `observeTapeRuntimeNewCommands` when enabled, after the
lambda returns. Both are outside the product/inner append clocks but inside
H589's named phase. The source audit therefore contradicts an interpretation
of the approximately6ms complement as argument-copy or query-wrapper cost.

H591 separates that post-callback work without disabling any verifier.
No CPU saving, default-live transfer or policy promotion follows from H590.
