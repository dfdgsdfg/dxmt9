---
domain: present-pacing
workload: STALKER CoP H502/r5 late Normal input, x86 release
title: "Adjacent admission authority removes real replay work with validator timers disabled"
type: evidence
status: local-cpu-gate-passed-runtime-restored-wild-transfer-pending
updated: 2026-10-13
source: traces/stkcop-validation-authority-h508/{contract.md,phase2-contract.md,candidate.patch,candidate-final.patch,observer-off.patch,build_runner.py,build_minimal.py,run.py,run_minimal.py,verify.py,candidate-build.json,minimal-A-build.json,minimal-B-build.json,rows.jsonl,phase2-rows.jsonl,summary.json,phase2-summary.json,verification.json,tests-final.log,tla.log,restored-build.log,restored-tests.log,restored.json}
related: present-pacing-validation-callers.507.md; present-pacing-production-query-diagnostic.506.md; present-pacing-validation-reuse.492.md; present-pacing-root-game.477.md; ../../../specs/backend/encode-scheduling/gap.md
---

# A work-removal candidate clears the local CPU gate, including timer control

Reusing successful continuation admission inside its uninterrupted queue-lock
scope eliminates the adjacent reserve scan found in H507. The exact original
late input retains all source, query and pixel oracles in 60 executions. A
second fixed comparison disables per-validator clocks identically in both
arms: planned replay still saves a paired median **2.839 ms per captured
interval**, above its 1.165 ms A/A envelope in 7/8 pairs.

This is a correctness-tested local removal estimate, not the largest default
game-thread owner, a live per-frame saving, a confidence bound or FPS evidence.
The runtime candidate is archived and restored pending live default transfer.

## Scope of the implementation

The archived patch changes only:

- `src/dxmt9/dxmt9_source_payload.hpp`, `ValidatedDirectContinuation` and the
  authority-consuming `TransactionalChunkSlotAssembler` constructor;
- `src/dxmt9/dxmt9_source_payload.cpp`, private `reserveImpl`;
- `src/dxmt9/dxmt9_command_queue.cpp`, the accepted continuation arm of
  `beginDirectChunkSlotReplay`; and
- its native tests in `tests/native/backend/direct_slot_provisioning_spec.cpp`.

The two-pointer, move-only authority is privately minted by CommandQueue after
the existing `AppendInPlace` decision. It is consumed immediately by the
assembler for the same payload while the same `unique_lock` owns the queue
mutex. No callback, unlock, payload mutation, or ownership transfer intervenes.
Only reserve's repeated `commandPayloadsInRange` walk is skipped. Reservation,
capacity bookkeeping, checkpoint, failure handling, resource marking, and the
full post-append `prepare` validator remain. Fresh, reclaimed, rotated and
ordinary compatibility construction retain the original constructor/checks.

This is **not** a cross-transaction validity cache. The pointer/`owns_lock`
checks do not prove absence of an earlier unlock/relock or same-address ABA:
private minting and immediate consumption make those histories unreachable at
the only production site. No authority may escape that stack interval or be
stored on the Slot. Foreign, moved-from and already-consumed tokens reject
before mutation. Rejection clears the assembler's destination reference before
destruction so it cannot roll an untouched prefix back to an uninitialized
checkpoint.

## Correctness and build evidence

Native comparison exercises all 64 combinations of six independent faults:
bad state, parameter extent, payload range, uniform identity, sealed pipeline,
and missing capacity. The real admission predicate gates both domains. The
positive verifies one removed scan, equal append extents, retained prepare
checks and rollback of only the added suffix. Separate cases cover move,
foreign destination, unlock, double consumption and corruption after admission.
Type assertions prohibit public minting, copying and default construction.

This is contraction of a repeated pure predicate inside an existing exclusive
critical section, not a new scheduling/ownership transition. The narrow proof
premise is the unchanged payload between validation and consumption; the
native cases bind the actual implementation. The existing 237 TLA configurations
pass as regression coverage, not as a new model of pointer authority. Four
focused native/provider/install-name tests pass. One new test's brace warning
was corrected and the tests rebuilt; pre-existing warning sites were not
mass-edited. `candidate-final.patch` adds only that test formatting correction
to the patch used to build the frozen runtime candidate.

Both phases preserve, on every execution:

| Original-input oracle | Result |
|---|---:|
| Source cuts | 7/7 |
| Query Issues / polls | 9/9; 5/5 |
| Defined query results / readiness | 4/4; 5/5 |
| Wrappers created / released | 670/670 |
| Source and output BGRX images | Both byte-exact, 3,686,400 bytes each |

The original pixel hash is
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
Queries execute the H506 production-counter profile, not injected results.
The hardware-query oracle remains false and all-resource initial-content
authority remains unknown. H505's unrelated textured-UP golden failure is not
waived by these checks.

## Two finite comparisons

Each phase freezes its contract before execution: two observer-OFF image/oracle
checks, two warmups per arm, four baseline A/A pairs, and eight alternating
AB/BA pairs. Acceptance requires paired median saving above the maximum
absolute A/A difference and at least seven positive pairs. These are empirical
gates, not statistical confidence intervals. No outlier is removed or run added
after seeing the result. Fresh-device warmups do not imply warm PSO caches.

The same H506 runner source, compiler/link command and canonical release build
configuration are used. Image hashes are checked before each execution;
observer-OFF logs prove actual private loaded paths. Neither phase stages Wine.
The Metal skill's doctor/readback workflow confirms Xcode 27, Metal compiler
32023.921 and Apple M1 Metal 4. Builds, native tests and TLA finish before timing.

| Phase / CPU scope | Paired median saving | Maximum A/A difference | Positive pairs | Gate |
|---|---:|---:|---:|---|
| Per-validation observer ON: validator | 3.007 ms | 0.059 ms | 8/8 | Pass |
| Per-validation observer ON: planned replay | 3.2425 ms | 2.116 ms | 8/8 | Pass |
| Per-validation observer OFF in both arms: planned replay | 2.839 ms | 1.165 ms | 7/8 | Pass |

Phase 1 confirms scans fall **646 -> 449** and successful command visits
**235,150 -> 159,850**, exactly the 197 duplicate scans / 75,300 visits
identified by H507. Phase 2's private probe suppression leaves all provider/raw
boundary clocks and V4 correctness hooks intact, but produces zero
SlotValidation events on both sides. It tests whether the effect persists
without removing those per-validation timers; it does not subtract a guessed
observer tax. The two phase medians must not be subtracted to price that tax.
One phase-2 pair regresses by 0.049 ms and is retained in the result.

All CPU numbers refer to one captured interval and still include V4/provider
observation. They are not observer-free live-game CPU or percentages of the
critical path. No cross-thread or nested scopes are summed.

## Restoration and next decision

The candidate and temporary probe suppression are removed with scoped patches.
All five affected source/test files match HEAD. Rebuilt Unix images are
byte-identical to H506; the four focused tests pass after restoration. All five
shared Wine files match the pre-existing restoration receipt. Earlier H501–H507
working changes remain intact. No runtime switch, commit, push, default
promotion or README/FPS update is made.

Unlike H492's rejected small uniform-lookup cache, this candidate now has a
positive whole-replay effect with its own timer control. The next useful test
is live default transfer using the archived baseline/candidate and matched late
scene boundaries, with command/source/locality/pixel gates. More unqualified
microbenchmarks are not needed to establish this local effect.

The original objective remains open: H477's mixed Primary and unjoined native
thread CPU have not been assigned, and offline Replay improvement cannot rank
them below Encode. A successful live candidate comparison would qualify this
removal in the default workload; it would still not by itself settle the
global largest-owner question.
