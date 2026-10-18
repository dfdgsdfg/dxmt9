---
domain: present-pacing
workload: STALKER H502/r5 actual inputs, post-oracle owned preparation
title: "Deferred preparation attributes local cost to pair comparison and ungated clocks"
type: evidence
status: local-cpu-attribution-live-transfer-open
updated: 2026-10-16
source: traces/stkcop-prepare-deferred-h605/{contract.md,owned.hpp,deferred.hpp,probe.hpp,native.cpp,prepare.py,derivation.json,build.py,build.json,run.py,runs.json,analyze.py,analyze-initial.py,analyzer-correction.md,analysis.json,codegen.py,codegen-initial.py,codegen-correction.md,codegen.json,verify.py,verification.json}; traces/stkcop-prepare-deferred-h605/{O0,P0,P1,O1}.log; traces/stkcop-prepare-deferred-h605/{P0,P1}.csv
related: present-pacing-prepare-local.604.md; present-pacing-queue-partition.603.md; present-pacing-owned-run-local.600.md
---

# Pair comparison dominates local preparation; clocks have a smaller measured cost

H605 collects 84 actual batches, 2,343 entries and 2,828 draws into bounded owned
storage. It runs scratch work only **after** the original pixel/query oracle
passes. Unlike H604, no repeated CPU experiment runs during rendering. Unlike
H598, no renderer is replayed repeatedly on the same Device.

Ownership includes canonical state/layout, uniforms, draw arrays and all four
payload byte ranges. Views are rebased; caller storage is never borrowed for
deferred execution. Bounds are 128 batches, 4,096 entries and 32MiB payload.
Native ARM64 and x86_64 tests verify caller destruction/mutation, rebased views,
both measurement orders and work failure. Product preparation stays unchanged.

## Fixed comparison and local CPU results

Fixed process order is O0,P0,P1,O1, without replacements. All four pass original
source/output pixels, query transcript, seven source boundaries, 670 wrapper
lifetimes and normal restoration. Product counters remain explicitly OFF.
P1 reverses initial arm and mode order. Each batch has two warmups and sixteen
pairs per mode, with 64 whole-batch repetitions per arm.

Caller-self CPU reads have wall brackets; lower bounds subtract endpoint
duration and 4us, upper bounds add 4us, then divide by 64. Batch means are summed
over the original population and normalized by 2,343 entries. No rows are
discarded. Ranges below span both order groups; they are endpoint allowances,
not statistical confidence intervals.

| Local operation | P0, ns/entry | P1, ns/entry |
|---|---:|---:|
| Timed preparation | 607.37–613.36 | 610.51–616.63 |
| Identical work without per-entry clock/counter | 552.41–557.50 | 554.90–560.34 |
| Payload-only preparation | 6.70–11.04 | 6.68–11.08 |
| State/layout pair-only preparation | 549.75–554.92 | 551.97–557.01 |

Payload-only and pair-only retain loop, vector and metadata overhead. They are
not isolated arithmetic or comparator instructions and must not be added as
disjoint production costs.

Clock-removal contrasts are 50.15–60.10 and 50.74–60.70ns/entry in P0;
50.79–60.79 and 51.15–61.17 in P1. All four lower bounds exceed the aggregate
sham bound of 5.77ns/entry. This new experiment predeclared aggregate treatment
versus aggregate sham; it does not replace H600's failed worst-individual rule.

## Verification and limits

Independent verification reproduces 16,128 raw rows without importing the
analyzer, including population, order, timing, weighting, contrast arithmetic,
image hashes and four effect controls. Eleven malformed-trace tests pass.
Generated-code inspection confirms the 64-repeat wrapper calls the work lambda,
which retains clock/counter calls and the entry lambda's production comparator.

Two verifier corrections are preserved explicitly. The analyzer initially
confused execution order with row serialization order; rows are emitted as
arms 0,1 after both finish, while timestamps prove alternating execution.
The codegen verifier initially searched only the outer wrapper rather than
its called lambdas and import stubs. Neither correction changes measurements,
images, population or acceptance criteria.

This is warmed owned-scratch CPU, not default-live queue residency. The clock
contrast corresponds to roughly 0.117–0.143ms for this captured population,
**not per live frame**. H603's live preparation wall value cannot be multiplied
by these ratios to certify removal. Largest default-live owner, transferable
CPU reduction and FPS gain remain open; no product change or promotion occurs.

The next focused candidate is the exact render-state equality common case
inside `include/dxmt9/core_snapshots.hpp`,
`flatRenderStatesEqualExceptAlphaTest`. A provably equivalent active-prefix
shortcut may avoid its branchy merge walk. Check unequal/alpha-only, overflow,
clamped counts and malformed ordering before actual-input timing; neither
benefit nor whole-pair cost attribution to that helper is established here.
