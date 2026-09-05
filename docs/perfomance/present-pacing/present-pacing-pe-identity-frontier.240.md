---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE admission inactive-storage initialization"
type: evidence
status: partial
updated: 2026-09-05
source: src/d3d9/d3d9_pe_semantic_owner.hpp; tests/native/bridge/pe_semantic_owner_spec.cpp; experiments/output/app-d3d9-3dmark05-gt2-perf-pe-padding-baseline-20260905; experiments/output/app-d3d9-3dmark05-gt2-perf-pe-padding-observer-off-20260905; experiments/output/app-d3d9-3dmark05-gt2-perf-pe-identity-frontier-on-20260905; experiments/output/app-d3d9-3dmark05-gt2-perf-pe-identity-frontier-on-r2-20260905; experiments/output/app-d3d9-3dmark05-gt2-perf-pe-identity-frontier-off-20260905
related: docs/perfomance/present-pacing/present-pacing-copy-materialization-ledger.237.md; specs/d3d9/recorder/gap.md
---

# PE admission inactive-storage initialization

The bounded implementation removes a release-PE zero-fill from every semantic
admission. Native byte-equivalence tests and independent review passed. GT2
measurements move in a favorable direction, but the four successful cells each
contain one run: they do not establish a stable FPS gain or close the PE CPU
frontier.

## Mechanism and safety

At base `da9ea8ff`, `detail::PeSemanticIdentitySet` value-initializes three
80-entry arrays on every `planPeSemanticAdmissionWith` call. Its `add()` method
only reads entries below `count`, and writes each new identity, object pointer,
and wire-visibility flag before advancing that count. The inactive entries have
no semantic consumer.

The constructor now initializes only `count` and `wireCount`. Copy and move are
deleted so an implicit memberwise copy cannot read inactive pointer/bool
elements. Qualified identity validation, same-ID generation/pointer collision
rejection, direct-pin-to-wire promotion, capacity, retention, rollback, and
canonical output remain unchanged.

The release i386 recorder object confirms the removed work: the planner
previously called `_memset` with `0x698` (1,688) bytes. The candidate has two
counter stores and no planner `_memset`; 1,680 bytes of inactive-array clearing
are avoided per invocation. The planner stack reservation remains `0x720`.
Before/after disassembly, the exact implementation patch, the analysis script,
and `comparison.jsonl` are retained in the successful candidate-ON artifact's
`analysis/` directory.

The native `identitySetUsesAnActiveFrontier` fixture reconstructs the set over
stale, valid object representations. A stale entry deliberately matches the
first new kind/object ID at another generation, so reading beyond the frontier
would cause a false rejection. It also covers null references, repeated wire
promotion, pointer/generation collisions, cross-kind identities, retry,
capacity denial, and an existing duplicate at full capacity. Existing owner
tests retain ExactFixed/segmented byte equality and rollback coverage.

This is a call-local value transform with unchanged transition ordering,
resource lifetime, and publication. Per the rendering-correctness rules, the
distinguishing native cases apply; no new concurrency/TLA model is required.

## GT2 evidence

All successful runs use Sikarugir `sikarugir-cx-24.0.7`, 1024x768, the `perf`
profile, info logging, frame sampling, contiguous transport, and default replay
and encode policies. There is no GPU trace or copy/materialization ledger.
Observer ON sets `DXMT9_PE_STATS_DECIMATION=127` and
`DXMT9_PE_SEMANTIC_OWNER_PHASE_SPLIT=1`; OFF sets both to zero. FPS below is the
benchmark-owned `.3dr` value, not process elapsed or renderer frame-sampling FPS.

| Cell / artifact suffix | Presents | .3dr FPS | CB/P | Pass/P |
|---|---:|---:|---:|---:|
| Base ON: `gt2-perf-pe-padding-baseline-20260905` | 1,584 | 25.309 | 3.9994 | 15.7866 |
| Base OFF: `gt2-perf-pe-padding-observer-off-20260905` | 1,685 | 26.936 | 3.9994 | 15.7662 |
| Candidate ON: `gt2-perf-pe-identity-frontier-on-r2-20260905` | 1,619 | 25.865 | 3.9994 | 15.7696 |
| Candidate OFF: `gt2-perf-pe-identity-frontier-off-20260905` | 1,710 | 27.338 | 3.9994 | 15.7737 |

Each suffix has the `experiments/output/app-d3d9-3dmark05-` prefix. All four
have zero chunk rejects, GPU command-buffer errors, and no-pipeline draw skips;
the runner reports `pass`, and the saved candidate-ON scene was visually
inspected. This is a bounded smoke observation, not an exhaustive pixel oracle.
The Unix provider and both bridge DLL hashes match across the cells. The i386
`d3d9.dll` changes from `00e98fcab7899c6c` to `46e9fdead064ffab`, with the same
1,764,352-byte size; result artifacts retain the x64 hash as well.

The initial candidate-ON artifact without `-r2` is excluded: it exited after
23.17 seconds / 390 Presents, returned zero, and produced neither a `.3dr` nor
the scheduled capture (`missing_capture`). No chunk/GPU error was recorded;
the cause of the early exit remains unassigned. Both subsequent candidate runs
completed normally.

| Calibrated ON phase | Base ms/P | Candidate ms/P |
|---|---:|---:|
| Prepare admission | 1.375 | 1.230 |
| Fixed/direct pins | 0.309 | 0.277 |
| Sparse/variable copy | 1.230 | 1.117 |
| Canonical materialization/metrics | 1.006 | 0.891 |
| PendingDelta settlement | 0.366 | 0.385 |
| Settle/clear | 0.684 | 0.586 |
| ExactFixed inclusive | 0.093 | 0.089 |

The analysis uses the last emitted phase group: subtract
`null_calibration_ms / null_calibration_samples` from each sampled mean,
multiply by that phase's event count, and normalize by final `present_encoded`.
These are run-normalized estimates: a periodic snapshot can omit the final
partial reporting interval. Prepare's calibrated mean itself moves from
512.9 to 468.8 ns/event. Parent timings include children and must not be added
to them. Unchanged child phases also improve, so the observed prepare delta
cannot all be assigned to the removed zero-fill.

The OFF FPS direction is +1.49%; ON is +2.20%. Populations differ and the order
was not counterbalanced. Further matched repetitions are required before
claiming either a stable speedup or strict locality equivalence. The earlier
pre-padding witness run is historical context; its different frame-sampling
configuration cannot isolate the padding change.

## Remaining CPU work

The larger PE candidate remains typed sparse/variable staging followed by
canonical serialization, including repeated pin-to-identity/handle-index work.
Any fusion must preserve the immutable ownership, null-unbind, exact-wire,
retention, and rollback contracts. The dead `EmissionPlan::records` staging is
a smaller cleanup candidate; ExactFixed remains below 0.1 ms/P here.

Current candidate-OFF replay carries about 72.6 draws per compatibility batch;
the historical one-draw queue-acquisition regression is not a current blanket
description. Append remains about 3.06 ms/P and resource marking 0.71 ms/P.
Encode draw time is about 12.25 ms/P, GPU command-buffer time 2.00 ms/P, and
offload worker idle wait 18.84 ms/P. These overlapping stage and wait measures
do not establish a new mutex or GPU bottleneck.

## Verification

Main-thread verification passed the full native, x86_64 Unix, PE x64, and PE
x86 builds, plus six focused specs: semantic owner, semantic batch, semantic
projection, chunk record value, chunk validation, and PE diagnostics.
`git diff --check` passed. Orca run `run_f17807a7c3d8` contains the bounded source
audit, evidence audit, implementation, and independent review with no blocking
findings; all owned worker terminals were released after completion.
