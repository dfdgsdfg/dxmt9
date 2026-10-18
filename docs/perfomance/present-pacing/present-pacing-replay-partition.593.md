---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads2 snd_targets16 late spatial interval
title: "Compatibility record replay owns the largest exclusive elapsed phase"
type: evidence
status: live-partition-qualified-cpu-removal-open
updated: 2026-10-16
source: traces/stkcop-replay-partition-h593/{contract.md,qualification-notes.md,trace.hpp,native.cpp,native.json,parser-tests.json,build.json,build_final.py,relink_probe.py,preflight.json,inputs.json,run.py,analyze_live.py,parse_partition.py,verify.py,verification.json}; traces/stkcop-replay-partition-h593/{O0,O1}/{receipt.json,self.bin,partition.bin,analysis.json,post-tail.png,restore/restored.json}
related: present-pacing-live-batch.592.md; present-pacing-append-tail.591.md; present-pacing-replay-census.585.md; present-pacing-sparse-cpu.584.md
---

# Target compatibility state/draw preparation before planning or cleanup

H593 partitions H592's default-live Replay complement. The largest exclusive
elapsed phase is **compatibility `replayResolvedChunk`, excluding nested queue
batch submission: 15.901 / 17.290ms/frame**. Emission planning is 1.110/1.192ms,
deferred StateBlock application 1.109/1.208ms and post-replay lifetime cleanup
0.194/0.210ms. Buffer-mutation work items and Tape verification calls are absent
in both selected populations.

This establishes the next operation family, not its pure CPU or removable
amount. Resolved replay can include setter locks, coordinator/control calls and
waits. Its conservative CPU lower bound remains zero with the available
coverage. Do not label its entire wall duration redundant copies or CPU service.

## Measurement and controls

H584 A is retained with Clear OFF and sparse self CPU endpoints. Four private
objects change: worker, chunk replay, command queue and queue. Product source,
canonical builds, PE ABI and rendering policies are unchanged. The unix image
SHA-256 is `336d0cf526ba46921215dbdb7b7db0e981ed1a46eb10d1b766fa846c465cd894`.

Each admitted worker item has process-birth/thread/queue-address/raw-ordinal
identity. A 32-entry TLS stack charges elapsed time to the active phase; nested
exclusive sums equal root wall exactly. Observation is mmap durable and
non-fatal. No per-draw/record clocks or CPU reads are introduced. Root admission
is request1-only; admitted roots finish after request2. Sparse CPU endpoints and
the missing-start allowance retain the H584/H592 conventions.

Native ARM64/Rosetta six-process tests pass nesting, exception unwind, identity,
overflow, unavailable sink, unchanged effects and hard-exit durability. Parser
positive/ten negative fixtures pass. Final four exact-image controls pass:
original H502/r5 OFF/ON and ordinary queue OFF/ON. The original retains source
and output SHA-256 `1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`,
query transcript, seven GPU source boundaries, 670 wrappers and restored state.
The ordinary fixture skips draws and verifies completion/API readbacks; it
does not exercise active worker observation.

Pre-live failures are preserved, not accepted as measurements. Three build
generator attempts stopped on ambiguous anchors/missing recipe. Two original
OFF/ON sets passed pixels but observed zero roots because the reused capture
executable statically contained the old provider. The initial hypothesis that
the provider bypassed `replayPlannedChunk` was **not established**: final controls
have 260 planned calls after relinking. The rebuilt H585 runner then rejected
the old `off` spelling; its OFF argument is `0`. All four controls were rerun
on the final images before staging Wine. See `qualification-notes.md`; do not
carry the initial bypass hypothesis in the frozen contract as a conclusion.

O0/O1 are fixed, unreplaced episodes. Both pass all seven active spatial knots,
natural exit0, image hashes, and 14-file restoration. Catalogue return1 is only
the predeclared `missing_capture` waiver. The computer-use skill limits focus
and screenshots to startup/post-selection. Post-tail screenshots are visually
coherent, not whole-game pixel equality. Independent raw verification checks
CPU arithmetic, exact exclusive conservation, 28 restoration hashes and no
remaining Wine. No parser/estimator imports are used by that verifier.

## Actual late partition

All phase rows below are **exclusive elapsed ms/frame**, not additive CPU costs.
Only the two labelled thread rows are sparse self CPU bounds.

| Scope | O0, 129 frames | O1, 118 frames |
|---|---:|---:|
| Replay thread CPU | 28.066–30.683 | 30.195–30.324 |
| Encode thread CPU | 28.441–31.096 | 30.225–30.459 |
| Compatibility resolved replay, excluding batch | **15.901** | **17.290** |
| Ordinary queue batch | 8.964 | 9.514 |
| Emission plan | 1.110 | 1.192 |
| Deferred StateBlock work item | 1.109 | 1.208 |
| Span driver, excluding nested replay/mark | 0.873 | 0.929 |
| Direct resolved replay | 0.493 | 0.467 |
| Legacy resource marking | 0.376 | 0.393 |
| Planned-route remainder | 0.194 | 0.213 |
| Post-replay cleanup | 0.194 | 0.210 |
| Raw-entry remainder | 0.100 | 0.117 |
| Worker remainder | 0.081 | 0.101 |
| Wire-view import | 0.037 | 0.038 |
| Mutation / Tape observer | 0 / 0 | 0 / 0 |
| Sum = worker root wall | 29.432 | 31.673 |

O0 has a 326.537ms whole-interval missing-start allowance; O1 has 8.827ms.
Retain both rather than dropping the slower boundary. O1's exclusive CPU uppers
including this allowance are 17.365ms/frame for compatibility resolved replay,
1.267ms for planning, 1.282ms for deferred StateBlock, and 0.285ms for cleanup.
Do not sum individual uppers, which repeat the same allowance. No buffer
mutation calls means absence in this population, not globally free uploads.

Worker roots: 59,304/57,705; Raw calls: 30,211/29,377; StateBlock calls:
29,093/28,328. All roots are actual worker roots; standalone and Tape counts
are zero. Ordinals run contiguously from 675,763–735,066 and 656,509–714,213.
The root excludes `queue_.pop`, admission and outer observer aggregation.
Whole-thread CPU includes those, so root wall is not a complete CPU partition.

## A cold capture routes differently

| Invocation count, not draws | Original preflight | Live O0 | Live O1 |
|---|---:|---:|---:|
| Resolved Direct | 199 | 253 | 239 |
| Resolved compatibility | 61 | 29,958 | 29,138 |
| Emission span driver | 250 | 29,178 | 28,350 |
| Ordinary queue batches | 84 | 33,946 | 33,016 |

Direct resolved calls are less than 1% of live resolved calls, versus 199/260
in the cold original control. This is not a draw-frequency claim. It is direct
evidence that cold capture route proportions do not reproduce warmed live
destination admission. Fixed-input Direct optimizations therefore cannot be
priced using the capture's route mix and silently transferred to live.

`src/d3d9/device_c_chunk_replay.cpp`, `replayEmissionSpans`, attempts
`beginDirectChunkSlotReplay` with `allowRotation=false`; a pre-effect failure
marks resources and replays compatibility. H593 does not log the rejection
reason, capacity dimension or draw count. Do not call every compatibility call
a capacity failure or restore the previously unsafe populated-Slot rotation.

## Concrete next operation boundaries

`replayResolvedChunk` includes record classification, transaction projection,
sparse setters, draw preparation and coordinator work. Its batch child is
already subtracted; the 17.290ms is not the previously measured final append.
The next test should separate:

1. Sparse state/constant application, journal capture and dirty-state updates
   in `DeviceReplaySink`.
2. Draw binding attachment and `Device::submitCompatibilityReplayDrawBatched`
   in `src/d3d9/core_draw.cpp`, excluding its nested final queue submission.
3. Coordinator/control work and loop/transaction remainder.

Within item 2, distinguish `cachedBaseDrawStateForSubmissionBatch` rebuilding
state/uniforms from new-run ownership. `CompatibilityBatchRun` owns both
`CanonicalDrawState` and full `DrawUniformPayload` by value, as declared in
`include/dxmt9/core_snapshots.hpp`; new runs copy these from the cache before
later queue emission. That is a concrete intermediate-materialization candidate,
not measured saving. State generations, backing spans and run lifetime prevent
replacing it with an unqualified borrowed pointer.

Use coarse aggregate and sampled caller-self CPU with explicit sampling/error
bounds rather than syscall timing every record. Before selecting a removal,
reproduce actual compatibility routing and preserve the original pixel/query/
lifetime oracle. Planning and cleanup are now lower-priority budgets. Largest
single operation CPU and correctness-qualified removable amount remain open;
no production edit, promotion, commit or goal completion follows from H593.
