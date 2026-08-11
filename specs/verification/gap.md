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
| TLA+ specs: CommandQueue, QueueLifecycleRefinement, PresentFrameLatency, ResourceLifetime, BufferBackingVersioning, EncoderLifecycle, QuerySeqId, ConcurrentProgressSignals, DrawableToken, WireObjectRegistry, PresentIdAba, EncodeSessionCompletion, DceChunkLookahead, CpuReadySessionProgress, SessionCapacityLease, PostEncodePayloadRetirement, EncodeSchedulingProgress, ReplayScopedDrain | ✅ | `WireObjectRegistry` alone owns canonical command-chunk stable identity, exact kind/object-ID/generation admission, generation advance, and wrap retirement. `PostEncodePayloadRetirement` models the synchronous Encode gate, receipt activation, locked detach, out-of-lock owner destruction, finish-time page/generation release, mixed legacy/receipt completion, exactly-once device-loss settlement, retained resources, and Present/completed-Present ordering. `SessionCapacityLease` separates physical residency from encoded work and preserves a deterministic work-cap boundary independent of reclaim or GPU completion. `EncodeSchedulingProgress` composes these interfaces with FIFO session continuation, lost-wakeup freedom, accepted-source release, and distinct Present decision/settlement obligations. Traceability is in `specs/verification/spec.md` §7. |
| Encode-scheduling model extensions (`R-VERIF-2.13`–`2.15`) | ⚠️ | `SessionCapacityLease.tla` separates physical residency from encoded-unsubmitted work, retains the submitted-residency denial/reclaim startup, and adds an explicit full-residency Writing successor startup whose lease/admit/retire/publish liveness fails if the successor is double-counted. `PostEncodePayloadRetirement.tla` covers the behavior-bearing receipt, retirement, device-loss, resource-retention, and Present-ordering refinement. The production representation-aware Tape-byte/draw/payload-block/retention/ticket axes remain abstracted into source/page capacity; native admission/Tape tests distinguish exact claims, ineligible states, overflow, and generation behavior. The generalized ready-prefix DCE proof and parallel/Metal 4 completion refinement remain open. |
| Post-encode payload retirement (`R-BACK-2.44`, `2.45`, `2.49`, `2.59`, `2.65`) | behavior implemented behind default-off Tape gate / promotion pending | A bounded generation-stamped receipt ledger replaces eligible Legacy or Arena payload locators after synchronous encode. Retirement uses locked detach, out-of-lock destruction, and locked generation/page finish; completion accepts mixed receipt and legacy identities, rejects stale/duplicate/ABA receipts before effects, and preserves resource/GPU waterlines. Native specs cover mixed completion, more than 30 resident-retired sources in one open session, the deterministic 128-source work cap, and receipt failure cases; the behavior-bearing TLA model covers device-loss and Present ordering. Wild visual/locality/performance promotion evidence remains intentionally open. |
| All eighteen specs model-checked by TLC — zero errors | ✅ | The full `dxmt9-verify-tla` run completed on 2026-08-11; all modules completed without invariant, deadlock, or liveness error, including the composed `EncodeSchedulingProgress` proof with queue-only weak fairness and an explicit GPU settlement environment assumption. |
| Companion native spec for `DrawableToken.tla` stash/take/wait state machine | ❌ | `tests/native/backend/present_acquire_policy_spec.cpp` covers env-var → `AcquirePolicy` resolution only. The token interleaving (stash → wait → take → complete/fail) is exercised by the TLA+ model and at runtime but has no deterministic native spec. |
| Companion native spec for `ConcurrentProgressSignals.tla` cross-axis non-blocking | ❌ | Pacing independence is observable only at the queue, not as a pure-data transform. A queue-observer / fake-backend probe covering all three axes (`completedSeqId` / `presentCompletedSeqId` / `ringSlotOccupancy`) simultaneously would close this. Same evidence gap as the queue-observer row below. |
| Companion native spec for `PresentIdAba.tla` slot-reuse ABA-safety | ❌ | Stable-index generation reuse/reject behavior is exercised through `chunk_record_registry_spec.cpp`. A focused `HandleArena` slot-reuse spec asserting `StaleResolvesNull` / `NoCrossSlotAlias` directly is still missing. |
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
