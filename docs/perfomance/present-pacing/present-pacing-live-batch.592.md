---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads2 snd_targets16 late spatial interval
title: "Default-live compatibility covers less than the Replay CPU majority"
type: evidence
status: live-envelope-qualified-replay-complement-open
updated: 2026-10-16
source: traces/stkcop-live-batch-h592/{contract.md,prepare.py,trace.hpp,native.cpp,native.json,parser-tests.json,build.json,preflight.json,inputs.json,run.py,analyze_live.py,parse_batch.py,verify.py,verification.json}; traces/stkcop-live-batch-h592/{O0,O1}/{receipt.json,self.bin,batch.bin,analysis.json,post-tail.png,restore/restored.json}
related: present-pacing-append-tail.591.md; present-pacing-reserve-recurrence.588.md; present-pacing-replay-census.585.md; present-pacing-sparse-cpu.584.md
---

# The remaining Replay majority is outside ordinary compatibility submission

H592 measures the real default-live path rather than charging V4 verification
to production. In O1, Replay consumes **29.410–29.539ms/frame CPU**. The observed
ordinary compatibility envelope has a conservative **9.620ms/frame CPU upper**;
actual append has a **3.610ms/frame upper**. At least **19.790ms/frame of observed
Replay CPU lies outside this compatibility envelope**. These are bounds in an
instrumented execution, not removable work, observer-free costs or FPS gains.

This makes further compatibility micro-optimization a secondary investigation.
The next attribution target is the complementary raw/state/Direct preparation
and work-item lifecycle work. H592 does not distinguish those owners or certify
that the complement is redundant.

## Exact boundaries

Private H584 A images retain Clear OFF and sparse self CPU sampling. Two runtime
objects add a durable aggregate observer; canonical PE, product source, ABI and
rendering policy are unchanged. The unix image SHA-256 is
`3f445444207de6ebf9e8566747370b14276b46e147bff9d94c8c5a57b22972b0`.

`src/dxmt9/dxmt9_command_queue.cpp`, `CommandQueue::submitDrawRunBatch`:

- Batch starts at the ordinary compatibility path after the Arena early-return
  path and draw-count preamble. It includes scratch preparation, initial queue
  acquisition, binding capture, segmentation, reserve, mark, append, settlement,
  commit, scoped teardown and mutex unlock.
- Initial acquire ends immediately after the first `unique_lock` acquisition.
- Slot preparation is the lexical `ensureWritingSlotUnlocked` scope. It can
  include unlock/relock and waiting; it is not a pure mutex-hold measurement.
- Append brackets only the lexical append-loop block, including its scoped
  instrumentation. It excludes subsequent capacity settlement and Tape runtime
  observation, correcting the H589 interpretation closed by H591.
- `src/dxmt9/dxmt9_queue.cpp`, `observeTapeRuntimeNewCommands`, is counted only
  after the enabled-observer gate, on the same admitted batch thread.

No per-draw clocks or per-batch `libproc` reads are added. All ordinary calls
admitted at request1 finish their observation even if request2 arrives. Mmap
aggregates are process-birth/queue/thread qualified; faults reject evidence
without aborting or altering the product call. Instrumentation still perturbs
execution. Admission/aggregation costs outside the root scope are not priced.

## Controls and two fixed live episodes

Native ARM64/Rosetta six-process tests pass: output absent/failure, unchanged
effects, exceptions, foreign queue/thread, invalid nesting and `_exit`
durability. Parser positive/ten negative fixtures pass. Four exact-image
preflights pass: original H502/r5 capture and ordinary fixture, OFF/ON. Original
source/output readbacks retain SHA-256
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`, seven source
boundaries, query transcript, 670-wrapper lifetime and normal-state restoration.
The ordinary fixture skips draws and verifies completion/API readbacks, not
whole-game pixels. Original observed input has 84 batches and 84 Tape calls;
the ordinary observed fixture has none of those Tape calls.

O0/O1 are frozen, unreplaced live episodes. Both pass all seven active camera
knots, natural engine exit0 and shared-file restoration. Catalogue return1 is
solely the predeclared `missing_capture` waiver; the screenshot is deliberately
after the selected interval. Both post-tail screenshots are visually coherent,
not an exact whole-game oracle. The computer-use skill limits UI work to startup
focus and post-interval inspection. Independent verification decodes raw bytes
without importing the parser/estimator, reproduces arithmetic, checks 28 restored
file hashes and confirms no remaining Wine processes.

| Selected interval quantity | O0 | O1 |
|---|---:|---:|
| Completed frames | 126 | 127 |
| Replay CPU, ms/frame | 27.515–30.095 | 29.410–29.539 |
| Encode CPU, ms/frame | 27.777–30.520 | 29.513–29.922 |
| Ordinary batches | 33,242 | 34,796 |
| Ordinary draws / input entries | 481,859 / 405,710 | 504,587 / 423,431 |
| Batch aggregate wall, ms/frame | 8.766 | 9.550 |
| Initial acquire wall, ms/frame | 0.346 | 0.370 |
| Slot preparation wall, ms/frame | 0.062 | 0.066 |
| Actual append wall, ms/frame | 3.271 | 3.540 |
| Tape observation calls | **0** | **0** |
| Missing-start allowance, ms for whole interval | 318.120 | 8.878 |
| Full-scene batch CPU upper, ms/frame | **11.291** | **9.620** |
| Full-scene actual append CPU upper, ms/frame | 5.796 | 3.610 |
| Replay outside batch CPU lower, ms/frame | **16.224** | **19.790** |

The root includes the children: never add these rows. Wall is a CPU upper, not
CPU service. The missing-start allowance extends from knot1 observation through
Replay's first self-counter acknowledgement; all recorded post-end tails are
included conservatively. O0's slow request is retained, not discarded. Do not
sum individual scope uppers: each repeats that allowance.

The complement lower is `Replay CPU lower - batch CPU upper`, floored at zero.
It includes unmeasured preamble/lifecycle/observer work and is not a named
operation. Replay and Encode bounds still overlap; no permanent unique Encode
bottleneck follows. Initial acquire and Slot preparation do not explain the
Replay majority in O1. Later commit waits are inside the root, not separately
priced by this experiment.

## Decision

Preserve Tape verification and existing storage policy. H588/H591/H592 reject
cold reserve, harness observation and ordinary append as explanations for the
whole late Replay CPU majority. Next partition actual Replay work outside
`submitDrawRunBatch`, before changing representation or expanding Direct policy.
Largest individual default operation and correctness-qualified removable CPU
remain open. No production promotion, commit or goal-completion claim.

The source audit supplies concrete next boundaries, not cost attribution:

1. `src/d3d9/device_c_replay_offload.cpp`, `ReplayOffloadWorker` worker body:
   distinguish raw execution, StateBlock application, buffer mutation and
   post-replay `publishReplayed`/`bufferSnapshots.clear`/`releaseRetainedWrappers`.
2. `src/d3d9/device_c_chunk_replay.cpp`, `replayPlannedChunk`: separate
   `importOwnedChunk`, `planReplayEmission`, `markLegacyResources` and the
   `replayEmissionSpans` route, including pre-effect capacity fallback.
3. The same file, `replayResolvedChunk`: separate semantic/state projection from
   nested final destination submission. Do not assign the outer replay scope
   wholly to planning or count `submitDrawRunBatch` again.

Use same-thread nested accounting and raw identity for this partition. Preserve
all source boundaries and avoid per-draw CPU syscalls; first test these scopes
on the retained capture, excluding explicitly armed verifier costs, before a
bounded live transfer. The worker complement can include mutations and cleanup,
so expanding Direct coverage alone is not justified by its size.
