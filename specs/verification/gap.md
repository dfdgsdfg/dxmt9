---
type: "Spec Gap"
title: "Verification Gap"
description: "Implementation and evidence gaps for formal and native verification."
tags: [specs, gap, verification]
---

# Verification Gap

Domain-owned implementation and evidence gap tracker. Use the [root gap index](../gap.md) for cross-domain rollup.

## Verification Layer

| Area | Status | Evidence |
|---|---|---|
| TLA+ specs: CommandQueue, QueueLifecycleRefinement, PresentFrameLatency, ResourceLifetime, EncoderLifecycle, QuerySeqId, ConcurrentProgressSignals, DrawableToken, WireHandleGeneration, PresentIdAba, EncodeSessionCompletion | ✅ | ConcurrentProgressSignals added 2026-05-09 (T2 closes G1: pacing-axis independence under R-ARCH-6.8/6.9). DrawableToken / WireHandleGeneration / PresentIdAba added 2026-05-18 (close drawable-token handoff race, PE→unix generation-stamp zombie, and (slot, generation) ABA-safety respectively). EncodeSessionCompletion adds R-BACK-2.49 coverage for one Metal session tail expanding into ordered per-source `seqId` completion. Traceability matrix in `specs/verification/spec.md` §7. |
| All eleven specs model-checked by TLC — zero errors | ✅ | EncoderLifecycle now uses exact `lastReadHandles` / `lastWriteHandles : SUBSET Handles` instead of Boolean `hazardFlag` (T1 closes G2 / R-VERIF-4.4); `AtMostOneEncoder` tautology removed (T1 closes G3 / R-VERIF-4.1); `BloomNeverForcesSplit` invariant proves Bloom signal cannot trigger an encoder split (T4 closes G4 / R-BACK-2.28). EncodeSessionCompletion adds 2,226 distinct states covering R-BACK-2.49. |
| Companion native spec for `DrawableToken.tla` stash/take/wait state machine | ❌ | `tests/native/backend/present_acquire_policy_spec.cpp` covers env-var → `AcquirePolicy` resolution only. The token interleaving (stash → wait → take → complete/fail) is exercised by the TLA+ model and at runtime but has no deterministic native spec. |
| Companion native spec for `ConcurrentProgressSignals.tla` cross-axis non-blocking | ❌ | Pacing independence is observable only at the queue, not as a pure-data transform. A queue-observer / fake-backend probe covering all three axes (`completedSeqId` / `presentCompletedSeqId` / `ringSlotOccupancy`) simultaneously would close this. Same evidence gap as the queue-observer row below. |
| Companion native spec for `PresentIdAba.tla` slot-reuse ABA-safety | ❌ | Stable-index generation reuse/reject behavior is exercised through `chunk_record_v2_registry_spec.cpp`. A focused `HandleArena` slot-reuse spec asserting `StaleResolvesNull` / `NoCrossSlotAlias` directly is still missing. |
| Pacing-axis independence (`R-ARCH-6.8` / `R-ARCH-6.9`) | ✅ | `ConcurrentProgressSignals.tla` proves `NoQueryWaitBlocksPresent`, `NoFrameLatencyBlocksQuery`, `NoRingPressureBlocksPresentCompletion` liveness with `PacingOrdering` invariant. |
| Hazard model: exact handle sets, not Bloom (`R-BACK-2.28`) | ✅ | `EncoderLifecycle.tla` `MergeRenderDraw` checks RAW/WAR/WAW set intersection; `BloomNeverForcesSplit` invariant proves split path ignores Bloom signal; `bloomFalsePositiveCount` advances only when exact disagrees (diagnostic-only role formal). |
| `QueueLifecycleRefinement` concrete queue lifecycle model checked and asserted | ✅ | `QueueLifecycleController` debug invariants cover `readySlots`, `pendingCompletion`, `completedSeqQueue`, inline completion, empty commit, `waitForSequence`, and shutdown paths |
| `PresentFrameLatency` present-token model checked and asserted | ✅ | `completedPresentSeqQueue_` advances `presentCompletedSeqId` only after `completedSeqId`; `presentBoundary()` asserts `MAX_FRAME_LATENCY` wait return safety |
| `SeqIdSafety` asserted with `// TLA+:` label | ✅ | `Device` submitted/completed sequence guards and queue completion watermarks |
| `QueryResolutionSafety` asserted | ✅ | `Query::getData()` |
| `BoundedInflight` asserted | ✅ | sim + metal `QueueLifecycleController::commitCurrentChunk()` |
| `NoUseAfterFree` asserted with `// TLA+:` label | ✅ | `Pool::reclaimCompleted()` |
| `RingSafety` asserted with `// TLA+:` label | ✅ | `RingArena::allocateBytes()` + slot ring |
| `EncodeSafety` asserted with `// TLA+:` label | ✅ | encode loop |
| `WineCommit` action mapping comments | ✅ | `QueueLifecycleController::commitCurrentChunk()` and `CommandQueue::submit*()` paths |
| DOD wire-schema acceptance | ⚠️ | Existing chunk tests cover many POD/layout and import validation cases; R-VERIF-7.1 now tracks full wire-schema acceptance for size/alignment, command IDs, version constants, offsets, and variable-tail rules |
| Queue observer / fake-backend verification | ❌ | R-VERIF-7.3 requires deterministic queue-facing evidence for chunk seq IDs, retained handles, replay categories, barrier/readback boundaries, and encoded command order without relying on Metal timing |
| DXMT concept mapping acceptance | ⚠️ | README mapping exists; R-VERIF-7.4 requires explicit implementation-owner and test evidence for each hot-path concept before calling DXMT merge readiness complete |

**The verification layer is partial for DXMT merge readiness.** Existing
R-VERIF-1.x through R-VERIF-6.x evidence remains complete, while new R-VERIF-7.x
acceptance tracks wire-schema, fake-backend/queue-observer, bridge-budget, and
DXMT concept-mapping evidence.

---
