---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads2 snd_targets16 late spatial interval
title: "Queue cost separates into append, immutable preparation and resource marking"
type: evidence
status: post-hoc-process-exit-correction-partition-measured
updated: 2026-10-16
source: traces/stkcop-queue-partition-h603/{contract.md,live-contract.md,partition.hpp,prepare.py,derivation.json,build.json,preflight.json,verification.json,partition-tests.json,inputs.json,original-rejections.json,process-exit-correction.md,process_exit_parse.py,process-exit-tests.json,analyze_process_exit.py,verify_process_exit.py,process-exit-verification.json}; traces/stkcop-queue-partition-h603/{O0,O1}/{receipt.json,self.bin,phase.bin,partition.bin,analysis-process-exit.json,post-tail.png,restore/restored.json}
related: present-pacing-generation-cpu.602.md; present-pacing-queue-cpu-owner.601.md; present-pacing-live-batch.592.md; present-pacing-append-nesting.590.md
---

# Queue capacity work is small; append and immutable preparation warrant attention

H603 partitions ordinary compatibility submission into disjoint phases. In the
better-bounded O1 episode, actual append is3.528ms/frame elapsed, immutable
preparation2.783ms/frame and resource marking1.835ms/frame. Capacity reservation
and segment planning are only0.090/0.099ms/frame. Their conservative CPU
ceilings remain small. Queue-wide CPU is7.600–9.250ms/frame, not all removable.

These are **exploratory results after an explicit lifecycle-model correction**.
Both episodes fail the frozen destructor-only parser. That rejection remains
recorded; a separate analyzer uses verified process exit to close remaining
diagnostic identities. No timing criterion or population was changed, no run
was replaced, and no H601 pointer-only result is reinterpreted.

## Lifecycle correction and its limits

H602's explicit generation binding works: domain1 is created and retired;
domain2 owns both worker epochs. The game exits0 normally without emitting
domain2's C++ destructor event. Requiring that event at process EOF was an
overstrict observer condition, not a necessary proof of the identity during
the earlier measured interval.

The correction was written after seeing O0's lifecycle rows and before any
admitted H603 timing analysis; O1 was already running. Original parser/analyzer
files remain immutable. `original-rejections.json` preserves both
`ValueError: missing retirement` failures. Separate
`analysis-process-exit.json` files explicitly state
`frozen_destructor_contract_passed=false`.

Process exit may close remaining diagnostic identities only with the external
witness: matching process birth identity, natural game exit0, no forced kill,
no timeout, all active spatial knots, and successful restoration after process
absence. Overlapping bindings, stale domains, cross-generation samples,
identity drift and fault/loss still reject. A missing terminal witness rejects
missing retirement. This does **not** prove GPU completion, destructor effects,
resource reclamation or absence of leaks. Positive process-exit closure and
nine missing-witness/identity negatives pass.

## Boundaries and evidence

The private image adds one changed runtime object to H602. It does not change
product fields, policies or effects. Root CPU still uses caller-self endpoints
only at ordinary batch boundaries. Children use wall clocks, not CPU syscalls:

| Phase | Exact responsibility |
|---|---|
| Prepare | Scratch preparation/reserves, payload-byte traversal and immutable neighboring-state/layout compatibility comparison before queue acquisition |
| Acquire | Initial queue mutex acquisition and its probe start |
| Binding | Per-entry binding snapshot capture |
| Slot | `ensureWritingSlotUnlocked` lexical scope, potentially including waits |
| Plan | Segment compatibility/capacity counting before mutation |
| Reserve | `reserveDrawRunBatchStorage` |
| Mark | Captured/override resource stamps, dedup and batch markers |
| Append | Actual append-loop lexical block, including uniform/state/SoA work |
| Commit | Capacity-blocked publication and post-segment commit scopes |
| Residual | Other root work, mutation settlement, scoped cleanup and unlock |

The coarse H602 root is slightly wider than H603's partition root; their
difference is recorded separately as an observer-envelope gap. Children plus
residual plus this gap exactly equal coarse batch wall. This avoids H589's
mistake of calling post-mutation verification work "append".

Six ARM64/Rosetta partition controls pass mapped/disabled/open-failure effects,
unwinding, duplicate finish, nesting and generation rejection. Twelve malformed
partition cases and24,336 bounded CPU-allocation cases pass. Four original and
ordinary OFF/ON effect controls pass; original pixel SHA remains
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`, with seven
sources, query transcript,670 wrapper lifetimes and restoration. Ordinary
draw-skipping controls are not pixel oracles.

Both live episodes finish all knots, exit normally and restore14 files each.
Post-tail images are visually coherent. Independent raw verification imports
neither parser nor estimator, reproduces identity/interval/conservation checks,
checks28 restoration hashes and confirms no remaining Wine process.

## Results

| Quantity | O0 | O1 |
|---|---:|---:|
| Completed frames | 123 | 119 |
| Replay CPU, ms/frame | 29.282–32.114 | 31.220–31.364 |
| Encode CPU, ms/frame | 27.699–30.786 | 29.770–30.095 |
| Queue CPU, ms/frame | 7.151–11.379 | 7.600–9.250 |
| Prepare wall, ms/frame | 2.654 | 2.783 |
| Initial acquire wall | 0.408 | 0.385 |
| Binding wall | 0.593 | 0.631 |
| Slot preparation wall | 0.060 | 0.065 |
| Segment planning wall | 0.099 | 0.099 |
| Reservation wall | 0.094 | 0.090 |
| Resource marking wall | 1.714 | 1.835 |
| Actual append wall | 3.300 | 3.528 |
| Commit wall | 0.045 | 0.048 |
| Residual wall | 0.310 | 0.335 |
| Observer-envelope gap wall | 0.042 | 0.045 |
| CPU endpoint-read wall | 0.589 | 0.647 |
| Selected late pacing, FPS | 18.045–18.053 | 17.143–17.150 |

O0's first controller request takes336ms and creates a339.688ms missing-start
allowance; O1's allowance is10.330ms. The new controller was not prewarmed by
this build script, unlike H601. Cold launch is a plausible contributor, not an
independently isolated cause. Retain O0's broad intervals; do not drop the run,
subtract the delay or claim equivalent observer perturbation.

For a child wall total W, batch wall B and measured batch CPU lower L,
`max(0,L-(B-W))` is a conservative admitted child CPU lower. Subtract post-end
tail wall for the full-window lower; add missing-start allowance to W for its
upper. This yields O1 CPU intervals:

| Child | CPU bound, ms/frame |
|---|---:|
| Actual append | 1.284–3.615 |
| Immutable preparation | 0.539–2.870 |
| Resource marking | 0–1.921 |
| Binding capture | 0–0.718 |
| Initial acquisition | 0–0.472 |
| Segment planning | 0–0.186 |
| Reservation | 0–0.177 |
| Commit | 0–0.135 |

The top three bounds overlap: elapsed ranking is not a proven CPU ranking.
Repeated start allowances mean these upper bounds must not be summed. Whole
Replay and Encode also overlap in time and are not additive frame latency.
At least21.969ms/frame of O1 observed Replay CPU remains outside this queue
envelope. Queue micro-optimization alone cannot explain that majority.

## Decision and next removal candidates

Stop treating capacity reservation, segment planning or initial lock waiting
as the likely large queue CPU owner for this interval. Focus local attribution
on the two largest measured bodies:

1. `src/dxmt9/dxmt9_command_queue.cpp`, `submitDrawRunBatch` preparation:
   split payload counting from `drawStatesCompatibleForDrawRunBatch` /
   `shaderLayoutsCompatibleForDrawRunBatch` and the per-entry `PerfScope`.
   The latter still takes timestamps even when its downstream counters are
   disabled. Do not assume all preparation time is comparison or memcpy.
2. `src/dxmt9/dxmt9_backend_types.hpp`, `appendDrawRun` and
   `appendCompatibleDrawRunEntryUnchecked`: split uniform interning from
   state/layout ownership and final payload/SoA writes. The former accepts
   `CanonicalDrawState` by value before moving fields into final arrays;
   actual copy-elision/codegen and net cost require evidence.

Use actual immutable input and effect-preserving local comparisons before
another broad FPS trial. These are candidates, not already-certified redundant
work. Preserve the wider Replay/cache and Encode ownership problem; neither
global largest CPU operation nor removable amount is yet established.
