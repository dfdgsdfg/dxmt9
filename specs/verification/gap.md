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
| TLA+ specs: CommandQueue, QueueLifecycleRefinement, PresentFrameLatency, ResourceLifetime, BufferBackingVersioning, EncoderLifecycle, QuerySeqId, ConcurrentProgressSignals, DrawableToken, WireHandleGeneration, WireObjectRegistryV2, PresentIdAba, EncodeSessionCompletion, DceChunkLookahead, CpuReadySessionProgress, SessionCapacityLease | ✅ | `CpuReadySessionProgress` now excludes pressure-created releases while preserving tentative rollback, suffix visibility, ordered semantic fences, completion/reclaim, and the forward terminal action. `SessionCapacityLease` covers the `R-BACK-2.65` fixed generation lease, complete ordinary-successor reserve, deterministic cap predecessor, completion-independent grouping guards, and no-pressure-release invariant. BufferBackingVersioning covers backing snapshot/use watermarks. Traceability is in `specs/verification/spec.md` §7. |
| Encode-scheduling model extensions (`R-VERIF-2.13`–`2.15`) | ⚠️ | The `R-BACK-2.65` lease/headroom refinement is implemented by `SessionCapacityLease.tla`; it abstracts the production byte/draw/payload-block/retention/ticket axes into source/page capacity and omits control payloads, device loss, and Metal effects. The generalized ready-prefix DCE proof and parallel/Metal 4 completion refinement remain open. |
| All seventeen specs model-checked by TLC — zero errors | ✅ | The full `dxmt9-verify-tla` run completed on 2026-08-05 after adding `SessionCapacityLease`; all modules completed without invariant, deadlock, or liveness error. |
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
| `NoUseAfterFree` asserted with `// TLA+:` label | ⚠️ | `Pool::reclaimCompleted()`. **The model is incomplete for this invariant, demonstrated by a real escape (2026-08-02).** `ResourceLifetime.tla` represents in-flight GPU work solely as seq-id-marked chunks, so the *only* lifetime mechanism it can see is the `lastUsedSeqId <= completedSeqId` watermark. A pending `Initializer` upload is a GPU command referencing a texture that is **not** a chunk and never bumps `lastUsedSeqId`; the model was green throughout while the production use-after-free existed (`StagingCopy::destTexture` held a bare handle — see `specs/d3d9/gap.md` 2026-08-02), and it is equally green after the fix, so re-running TLC proved nothing about that change. The fix satisfies R-VERIF-3.1 through **refcounting, a mechanism the model does not represent at all.** Closing this needs either an `Initializer`-held-reference actor in `ResourceLifetime.tla` or an explicit statement in the spec that refcount-held references are out of model scope and are covered by native specs instead. Until then the `gcArena` comment at `dxmt9_resource_pool.cpp:166-177`, which presents the watermark as the whole realisation of no-use-after-free, overstates its coverage. |
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
