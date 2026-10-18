---
domain: present-pacing
workload: STALKER CoP H502/r5 late Normal input, x86 release
title: "Repeated Slot validation belongs to Direct admission and assembly"
type: evidence
status: callsite-qualified-not-removal-qualified
updated: 2026-10-13
source: traces/stkcop-validation-callers-h507/{run.py,analyze.py,temporary-probe.patch,build.json,build.log,runner-build.log,run.log,caller.json,calls.json,duplicate-candidates.json,summary.json,restored-build.log,tests-restored.log}
related: present-pacing-production-query-diagnostic.506.md; present-pacing-validation-reuse.492.md; present-pacing-root-game.477.md
---

# All 646 checks originate in the product Direct Slot path

A single caller-count diagnostic on the unchanged H502/r5 input attributes
H506's repeated full-Slot checks to three product functions. There is no
additional full-Slot scan directly requested by V4 qualification in this run.
This establishes code ownership, not the same call frequency in live default
gameplay or a CPU removal budget.

| Caller | Accepted calls | Successful command visits |
|---|---:|---:|
| `directContinuationAdmission` | 248 | 84,158 |
| `TransactionalChunkSlotAssembler::reserve` | 199 | 75,300 |
| `TransactionalChunkSlotAssembler::prepare` | 199 | 75,692 |
| Total | 646 | 235,150 |

Stored DrawParam extent sums remain extents, not parameter visit counts.
The count matches both H506 traces. H507 does not assign its instrumented
elapsed/CPU time to product work or transfer callsite timings across runs.

## Attribution method and exact-input gates

A temporary eight-frame native backtrace was added only to
`src/d3d9/device_c_chunk_replay.cpp`, `observeProviderSlotValidation`.
It used the existing cold provider/TLS probe and did not change validation,
admission, scheduling or query results. Each invocation still executes the
original validator once. Logging, unwinding and symbolization make this a
**count-only diagnostic**, not a benchmark. This does not retry H467's failed
live Primary/Wine guest-stack unwind.

Every immediate caller resolved; parent frames connect reserve to
`CommandQueue::beginDirectChunkSlotReplay`, prepare to
`CommandQueue::commitDirectChunkSlotReplay`, and admission through
`directSlotStorageTransition` to the begin path. The strict existing reducer
accepts all 259 raws and every validation scope. Attempt IDs join the stack
log to trace extents, results, thread and enclosing raw-event boundary.
Slot addresses are only same-process diagnostic keys, not lifetime authority.

The frozen private images and original manifest/receipt are checked. All seven
source cuts, nine query Issues, five polls, four defined results, and both
3,686,400-byte original readbacks match. Creation/release is 670/670.
The hardware-query oracle remains false; all-resource content authority remains
unknown. The Metal skill's doctor/readback workflow confirms Xcode 27 and Apple
M1 Metal 4. No shared Wine files are staged or game configuration changed.

## Concrete duplicate work and narrow next candidate

197 admission calls are immediately followed by reserve on the same raw event,
Slot and command/parameter extents. Those reserve scans account for all 75,300
reserve command visits, **32.0% of the observed full-Slot command visits**.
The other two reserve calls visit empty command lists. This percentage is
work multiplicity, not CPU percentage or predicted FPS improvement.

Static inspection confirms the duplicate mechanism:

1. `src/dxmt9/dxmt9_direct_continuation.hpp`,
   `directContinuationAdmission`, validates the populated Slot before capacity
   admission.
2. `src/dxmt9/dxmt9_command_queue.cpp`, `beginDirectChunkSlotReplay`, selects
   `AppendInPlace` and constructs the assembler while holding the same queue
   mutex. The accepted continuation branch does not mutate command payloads
   between these two checks.
3. `src/dxmt9/dxmt9_source_payload.cpp`,
   `TransactionalChunkSlotAssembler::reserve`, scans that existing prefix again
   before its reservation. `prepare` later scans the post-append contents.

The smallest candidate is to pass **transaction-local validated-destination
authority from successful continuation admission into reserve**. It should not
cache a prefix across transactions. Fresh/reclaimed, rejected, rotated and
ordinary compatibility construction must keep their existing validation;
post-append prepare must remain a full check in the first experiment.
Do not add a public `skipValidation` Boolean or derive authority from matching
extents alone. Bind it to the actual destination and uninterrupted ownership,
consume it once, and reject foreign/stale/reused authority. Allocation failure
and rollback must retain their current behavior.

This separates a directly adjacent redundant scan from the wider and riskier
proposal to validate only an appended suffix. The latter additionally needs
proof that old DrawRun/uniform/payload rows cannot be mutated or invalidated by
append, rollback, clear, reclaim or replacement. H492's rejected last-handle
microcache is not reopened.

Before accepting a saving, bind the narrow authority contract to native
negative/refinement tests, retain the original source/query/pixel gates, then
run a finite A/A and order-reversed A/B on the same frozen input. Price the
whole planned-replay change as well as validator scopes; a reduced visit count
alone is insufficient. Live default-tail transfer and the mixed Primary/native
ownership gap remain independent requirements.

## Restored state and limits

The temporary probe is archived and removed. The replay TU again hashes to
`e3c3eb7904be3c100247dfd090d2cfbab7dc76902ac499790304119d4bc71591`.
The full canonical Unix build succeeds after restoration; source-payload,
direct-slot-provisioning and install-name tests pass. All five shared Wine
images match the prior restoration receipt. No product optimization, runtime
knob, default promotion, commit or FPS claim is introduced by H507. Existing
H501–H506 working-tree changes remain intact. H505's separate textured-UP
golden mismatch is neither waived nor resolved by these checks.
