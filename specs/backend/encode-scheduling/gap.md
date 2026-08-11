---
type: "Spec Gap"
title: "Encode Scheduling Gap"
description: "Implementation and evidence gaps for CPU-ready, EncodeSession, partition, and Metal execution scheduling."
tags: [specs, backend, encode-scheduling, gap]
---

# Encode Scheduling Gap

This is the detailed owner for `R-BACK-2.35`–`R-BACK-2.50` and
`R-BACK-2.57`–`R-BACK-2.67`. The parent [backend gap](../gap.md) keeps only a
routing summary. Historical experiments remain in `docs/perfomance/`.

| Area | Status | Current evidence | Missing implementation or evidence |
|---|---|---|---|
| CPU-ready publication boundary (`R-BACK-2.35`–`2.41`) | Direct/StateOnly routing and serial arena consumption implemented behind default-off promotion gate | Commit admission fixes the cutover decision once per raw entry. Gate-off preserves the historical synchronous combined resource-mark/backing-capture operation, worker Legacy replay without planning, and the compatibility publication cap of `kMaxQueuedChunks` GPU-inflight sources. With `DXMT9_CPU_READY_TAPE=1`, admission instead persists the validated raw blob, resolved objects, deduplicated resource identities, captured buffer backings, wrapper retention, and raw residency; the replay worker then plans each complete raw chunk once. `StateOnly` replays without a ticket or mark, eligible Direct chunks acquire a sized strict Tape lease, construct typed arena payload in place, and apply exact `seqId` marks before Ready visibility; planned Legacy/Inline mark before replay, and post-semantic Direct failure is fail-stop with no semantic fallback. Only the replay worker may wait for Direct admission pressure. `SourcePayloadView` carries both legacy and arena payloads through the common backend and serial partition cursor; DCE holds an arena successor without exposing it to the legacy proof window, and with the gate on the encode thread runs the Tape session lane (`runCpuReadySessionEncodeLoop`), which admits Arena sources into shared-session submissions with exact Tape completion identity instead of forcing one-source standalone submissions. Native evidence covers Direct planner-to-publish-to-consume-to-completion/reclaim, StateOnly/no-ticket, pressure wakeup, post-semantic fail-stop, compatibility inflight backpressure, and process-separated gate-off synchronous combined admission. | Keep the runtime gate default off until `R-BACK-2.50` formal/equivalence, wild visual/locality, and no-gputrace overlap/performance gates pass. Quantify whether direct construction reduces producer/replay cost without increasing command-buffer or render-pass shape. Sources exceeding the streaming source/page/segment hard caps and synchronous Inline paths intentionally retain compatibility payload construction; TriangleFan worst-case sizing is covered by Direct planning. Production multi-segment-plus-Present FrameGraph/session/Presenter evidence, explicit production partition subdivision, parallel render encoding, and Metal 4 segmentation remain separate work; cross-source session admission for both source kinds is tracked in the EncodeSession row below. |
| CPU-ready source residency/admission (`R-BACK-2.60`, `R-BACK-2.65`, `R-VERIF-2.14`) | split physical-residency and encoded-work accounting plus ordered-tail Writing no-double-count implemented behind the default-off Direct gate | `SessionCapacityLeaseState` still reserves the full physical source/page/byte/block/Ready/retention/ticket headroom before representation. A typed Tape snapshot separates older unavailable states from one unique current-generation ordered-tail Writing publication; only an exact claim within successor headroom is credited at first acquisition, while invalid identity/arithmetic fail-stops before the generation waiter. After synchronous encode, eligible Legacy and Arena payloads return residency without closing the session; encoded source/draw/command-buffer work remains charged until submission. The deterministic production work cap is 128 sources. Native production-loop evidence constructs the failed 31 Ready / 511-page + one Writing state, blocks the compatibility writer in `commitCurrentChunk`, then proves first encode, writer publication, and all 32 sources progress on one deferred session before an explicit ordered submit. Existing evidence keeps more than 30 eligible sources open and proves source 129 closes the exact 128-source predecessor with `SessionCap` attribution. `SessionCapacityLease.tla` retains older submitted denial/reclaim and adds explicit Writing-startup progress. | The same-build R21 GT2 tape-off/on pair clears the prior frame-12 progress blocker and restores command-buffer locality; it is one-scene evidence, not default-promotion evidence. Keep `DXMT9_CPU_READY_TAPE` default off until the remaining wild visual/locality/performance matrix passes. |
| Unified Arena source representation (`R-BACK-2.40`, `R-BACK-2.45`–`2.46`, `R-BACK-2.49`, `R-BACK-2.60`, `R-BACK-2.62`) | production multi-segment construction and Present implemented behind the default-off Direct gate / admission and end-to-end evidence fail promotion | The Direct planner emits an exact raw-record range table and packed `ArenaSourcePayloadLayout`, including dedicated jumbo segments. One strict Tape reservation owns all segment builders, publishes or aborts them atomically, exposes them as one logical `SourcePayloadView`, and reclaims them under one source/ticket/`seqId`/completion identity. Production replay flushes pending DrawRun batches at exact segment edges and visits every raw record once. A single final Present may be built in the Arena source while Query routes Legacy, Readback routes synchronous Inline, `UpdateTexture` routes Legacy, and non-final or multiple Present records route Legacy. Native specs cover packing, multi-segment ownership, Present publication, token abort, and source-qualified FrameGraph input. Clean H233 shows that removing the oversize bypass without reserving session headroom converts full-Arena sources into pressure-driven session fragmentation; the fixed headroom/cap policy is now implemented in the residency owner above. | Add a production multi-segment-plus-Present fixture through FrameGraph, shared EncodeSession, and Presenter; add Arena-source PSO-prefetch parity and source-qualified diagnostic evidence; prove that segment edges do not alter DAG, pass, submission, or completion shape. Keep `DXMT9_CPU_READY_TAPE` default off and legacy rollback intact until `R-BACK-2.50` passes. |
| Scoped FIFO replay drain (`R-BACK-2.51(d)`, `R-BACK-2.61`) | buffer lock/unlock scope implemented / promotion pending | A device-owned ledger canonicalizes shared-wrapper aliases by core-buffer identity, admission captures backing generations and raw residency, and buffer lock/unlock wait only for the relevant replay target with conservative terminal handling. Deterministic native and process-separated OFF/ON byte-identity evidence is present. | Complete wild/conformance promotion and paired performance attribution; broaden canonical access summaries beyond the current buffer lock/unlock allowlist only when another synchronous consumer needs it. |
| EncodeSession semantic lifecycle (`R-BACK-2.42`–`2.49`, `R-BACK-2.65`) | bounded multi-source serial planning, fresh-frontier Ready-head lookahead, and exact deferred terminal-suffix join implemented behind the default-off Direct/Tape gate / promotion evidence missing | The existing coordinator still bounds ordinary Ready planning to eight sources, preserves natural FIFO completion, and carries one active pass across the proven 8+1 window edge. The added source-local lane recognizes only exact `DrawRun(A), Clear(B), DrawRun(B) | DrawRun(A)`: it encodes the current `A`, retains a bounded pointer-free `DeferredTerminalSuffixState`, waits unlocked for one exact ordered-tail Writing successor within headroom, then either replays successor `A` before the older `Clear(B), B` suffix or drains naturally before successor effect. At prefix time it value-snapshots the replay frontier, complete active dependency and render-pass instance token, lease/release values, and the full controller plus pending-carrier capture boundary; it re-resolves payloads and revalidates those values after planning/charge and immediately before commit/replay. Semantic drains precede Ready handling; once absent, exact Ready wins simultaneous admission or writer pressure. Native evidence covers the exact join and default-off natural baseline, allocation-free proof/rejection, real lease/headroom charging, stale-successor restore, ordered `ExplicitFlush` release, stop, pending-carrier capture-start drain through the full capture predicate, Ready-over-admission-pressure, one observer, render-pass `3 -> 2`, one removed mid-chunk split, no held-edge action/sidecar/completion publication, exactly-once terminal action/Store handling, command-once, current-before-successor receipts, FIFO completion, and zero final residency. `CpuReadySessionProgress`, `SessionCapacityLease`, and `PostEncodePayloadRetirement` now cover bounded held ownership, writer publication, Join/StaleFailOpen/Drain, exact-successor-over-pressure, charge-once, no-retire-while-deferred, receipt/completion/reclaim ordering, and temporal progress; `dxmt9-verify-tla` is green. | For this terminal-suffix implementation there is no wild, visual, locality, performance, no-gputrace, or promotion evidence, and no GT2 improvement is claimed. `DXMT9_CPU_READY_TAPE` remains default off. The direct ordered-`ExplicitFlush` fixture exists, but API-originated Flush and Query/readback/`UpdateTexture`/Present/initializer/producer-wait control boundaries still lack production terminal-suffix fixtures; their pure drain policy is covered. The capture-controller-enabled and pending capture-object arms of the full capture boundary also lack separate production injection. Exact Ready with admission pressure is covered in production, but admission-pressure-only and writer-pressure natural-drain paths still lack production injection. Production multi-segment-plus-Present FrameGraph/session/Presenter evidence remains missing. Promotion requires a fresh same-build GT2 tape-off/on visual/locality/performance/no-gputrace comparison first, then paired GT1, GT3, and SFIV coverage plus repeated runs and conservation gates. Continuity after encoder end or command-buffer commit remains Metal 4 work under `R-BACK-2.64`. |
| Promotion gates (`R-BACK-2.50`) | partial | Permanent counters now split retirement attempts/success/ineligibility, receipt failures/depth, physical residency released, encoded-work cap closes, and GPU-outstanding current/peak from existing wait, command-buffer, pass, load/store, and tile shape. The Tape gate remains default off and the legacy payload-owning path remains the rollback baseline. | Enforce the expanded conservation gate in comparative tooling and collect fresh wild visual/locality/no-gputrace evidence. Do not claim a GT2 improvement or promote the gate from native/formal evidence alone. |
| Stable render scheduling provider modes (`R-BACK-2.66`) | canonical partition selector and distinct `ExplicitParallel` mode implemented / unified three-axis config partial | Device creation resolves `DXMT9_RENDER_PARTITION_MODE=identity|serial|parallel` once into queue-owned typed state and forwards it through every production encode path. Unset/`identity` select identity, `serial` selects the production explicit planner, and `parallel` resolves distinctly and runs the same planner before the current typed serial fallback. Unknown values fail closed to identity. Perf-disabled execution does not take clocks or update scheduling counters; enabled reports requested/resolved partition modes. Pure native coverage pins the spelling and fallback matrix. `DXMT9_CPU_READY_TAPE` remains the source-delivery migration alias. | Unify source, partition, and segment axes in one complete `RenderSchedulingProviderConfig`; add canonical source/segment parsing, legacy Tape alias precedence, requested/resolved source and segment reporting, and the full mode matrix. Real parallel and Metal 4 execution remain owned by their rows below. |
| Published source immutability (`R-BACK-2.46`, `R-BACK-2.59`) | behavior implemented for retirement eligibility / broader type-seal audit open | Published payload access remains const and call-local. Eligible storage mutates only after receipt activation and exclusive `Reclaiming`; locator-backed or borrowed payloads, pending clear, Present, and ordered-control sources remain resident. | A broader type-level seal and removal of the read-only `const_cast` in `makeEncodePartitionReplayStream` remain independent cleanup. |
| Post-encode payload retirement (`R-BACK-2.44`, `R-BACK-2.45`, `R-BACK-2.49`, `R-BACK-2.59`, `R-BACK-2.65`) | behavior implemented behind the default-off Direct gate / promotion pending | A bounded flat generation-stamped `PostEncodeCompletionLedger` installs source-kind-neutral receipts for eligible synchronously encoded Legacy and Arena sources. Session, submission, and pending-completion lists may contain mixed receipt and legacy locator identities. Retirement marks and detaches under the scheduling lock, destroys re-entrant owners outside it, then relocks to finish page/generation/control release; failures after receipt activation or Metal effects are fail-stop. `RenderEncoderGpuSample` uses locator-free `EncodedCommandId`. Present, pending clear, query/readback/update, ordered controls, and remaining borrows are ineligible. Completion validates stale/duplicate/ABA receipts before callbacks and preserves per-source, resource-waterline, and completed-Present ordering. Native specs cover ABA/stale/duplicate handling, mixed completion, two-phase Tape reuse, resource retention/callback ordering, more than 30 eligible sources in one session, the deterministic 128-source work cap, and both joined and natural-drain suffix paths: current receipt/detach stays blocked through pending suffix/effects/borrow, then current and successor activate, complete, and reclaim once in FIFO order with conserved residency and encoded work. `PostEncodePayloadRetirement.tla` now models deferred prefix/suffix/effect/borrow state and checks no-retire-while-deferred plus receipt/completion/reclaim safety and liveness; `dxmt9-verify-tla` is green. | Runtime gate promotion still requires fresh visual/locality/performance/no-gputrace evidence under `R-BACK-2.50`; no GT2 improvement is claimed. |
| Serial partition consumer (`R-BACK-2.57`, `R-BACK-2.58`) | production identity and explicit-serial callers implemented / GT2 production coverage observed | All backend streams use the range interface. Complete preflight, identity fail-open, command-once behavior, multi-subrange lifecycle, and production option forwarding are covered by partition, snapshot, and session native specs. The production comparison proves explicit subdivision retains one DrawRun setup, pass begin/end, split-policy decision, upload batch, and complete draw count. A GT2 scout exercised explicit serial consumption with zero GPU command-buffer errors and zero capacity fallback. | Matched repeated Metal-backed locality and visual evidence remains missing; the first unmatched scout is not promotion evidence. |
| Production partition planner (`R-BACK-2.62`) | explicit-serial production coverage observed / locality promotion pending | The queue-immutable canonical partition selector enables a fixed 256-range call-local planner after final replay selection. Large DrawRuns (threshold 64, target 32, minimum side 16) subdivide only at boundaries that preserve the existing compatible indexed-draw merge chain. Complete unsplit commands coalesce into command segments, so only actual subdivisions consume locator-bearing ranges. Full-plan validation precedes effects; typed invalid replay, capacity, snapshot, merge-preservation, and validation failures atomically select identity. Perf counters split identity/explicit selections, ranges/draws, subdivided and merge-preserved runs, fallback reasons, and planner CPU cost with clock reads gated behind perf enablement. Native specs cover threshold edges, determinism, mixed/reordered/DCE-empty streams, segmented Arena input, bounded overflow/malformed fail-open, wild-shaped range compression, selector resolution, and production serial consumption/shape. A same-build GT2 scout after compression observed 267 explicit selections, 680 explicit draw ranges, 21,760 explicit draws, 340 subdivided DrawRuns, zero capacity fallback, and zero GPU command-buffer errors across 1,281 presents. | Do not promote from the first GT2 scout: its identity comparison covered 1,018 rather than 1,281 presents and changed draws/present by 1.42%; the strict locality gate also reported CB/present +0.01%, pass/present +0.51%, and tile-preservation MiB/present +0.24%. Collect matched repeated GT2 locality and visual evidence, then exercise GT1, GT3, and SFIV coverage. The policy is serial only and does not implement `R-BACK-2.63`. |
| Parallel render-pass executor (`R-BACK-2.63`) | first Metal-free contract increment implemented / real execution default-off | Parallel requests run the validated production planner, then the current source-fragment path records typed `PassNotSealed` fallback and consumes the explicit plan serially. A pure fixed-capacity eligibility/selection/child-plan seam rejects commands, query, clear, observation, initializer, Present, hazards, or missing first-draw snapshots. Deterministic fake-child coverage proves ordered creation, arbitrary completion join, distinct child-local shadows, forced full first-draw binding, command-once and draw-once replay, coordinator-only actions/sidecars/completion, join-before-parent-end, pre-effect fallback, and post-effect fail-stop. Enabled counters expose considered/eligible/selected and grouped typed fallback reasons. | Implement sealed-pass snapshot production, the real WMT `MTLParallelRenderCommandEncoder` parent/child adapter, and a bounded worker pool; add formal/refinement evidence and measured large-pass eligibility before allowing a selected production child lane. The provider remains default-off and currently selects no Metal parallel work. |
| Metal 4 segmented lane (`R-BACK-2.64`) | not started | Research and the logical-pass/session contracts identify the capability boundary. | Add capability selection and legacy fallback, bounded group gather, suspend/resume option validation, joint submission/completion, action semantics, native/integration evidence, and supported-hardware visual/locality A/B. |
| Logical-pass actions across children/segments (`R-BACK-2.48`, `R-BACK-15.17`) | not started | Current action policy is implemented for ordinary serial render passes. Serial EncodeSession late Store resolution (`R-BACK-15.18`) is implemented separately: a fixed copied ledger survives source boundaries, resolves before the single `endEncoding`, and emits Store counters once. | Extend the same policy to parallel children/Metal 4 segments so load/clear occurs once at logical start, store/resolve and sidecars once at logical end, and counters do not double-count physical segments. |
| Composed end-to-end progress (`R-BACK-2.67`) | implemented / wild diagnostic exercise pending | `EncodeSchedulingProgress.tla` composes admission, lease/generation wake, publication, FIFO session continuation, deferred retirement, completion expansion/release, and Present pacing. TLC proves ownership/conservation, FIFO, sticky obligations, both lost-wakeup invariants, accepted-source release, explicit Present publish-or-skip, and settlement with GPU progress isolated as an environment fairness assumption. Production admission, first-lease, retained/deferred-session, initializer transition, and terminal fanout decisions share pure APIs with exhaustive truth tables and deterministic real condition-variable tests. Initializer empty-to-nonempty now wakes encode, and stop/device-loss fanout covers writer, encoder, finish, both Present wait axes, session release, and pending completion. The perf-gated CommandQueue watchdog has 256 generation-stamped slots, phase/age diagnostics, explicit skip/terminal/capture/suspend/overflow attribution, and perf-off zero-clock/zero-atomic evidence. | Exercise the opt-in watchdog in a no-gputrace wild run and archive one phase-attribution example; this is diagnostic evidence only and does not block the implemented formal/runtime contract. |
| Scheduling formal verification (`R-VERIF-2.13`–`2.15`) | split capacity and post-encode retirement refinements implemented / generalized DCE and parallel completion open | `SessionCapacityLease.tla` separates resident/session/submitted/completed sets and proves residency retirement cannot erase encoded work or choose a boundary. `PostEncodePayloadRetirement.tla` explicitly orders Publish, Encode, DetachRetiredPayload, DestroyOwner, FinishPayloadRetirement, submit, GPU/device-loss settlement, and completion; it proves bounded generation receipts, two-phase page finish, retained resources, exactly-once settlement, and `completedPresentSeqId <= completedSeqId`. | Production byte/block/retention/ticket dimensions remain native-test evidence. Generalized `R-VERIF-2.13` and parallel/Metal 4 `R-VERIF-2.15` remain open. |

## Post-encode owner and escape audit

This audit records the P3 ownership boundary that authorizes early retirement
only for sources passing the implemented eligibility gate.

| Value / field | Current owner and lifetime | Escape after synchronous encode | Classification for retirement |
|---|---|---|---|
| `SourcePayloadView` and its returned spans | Borrowed pointers into a represented `ChunkSlot` or sealed Arena chain; valid only under the Ready-selection or Represented encode pin | No intended escape. `ResolvedPublishedSource`, FrameGraph builder state, replay streams, store-proof lookahead, and `EncodeChunkOptions` hold it only for their enclosing synchronous call | Must remain call-local. Add a static/source audit that rejects it from session, submission, callback, and pending-completion storage before promotion |
| Legacy/Arena payload owners and destructors | Tape owns `ChunkSlot` or the in-place `ArenaSourcePayloadBlock` chain. Arena blocks own constructed `DrawShaderLayoutContext` values; Legacy slots expose `detachResourceOwners()` | Eligible owners detach after encode; ineligible owners remain resident through completion. Their destructors may release shared Buffer/VertexDecl owners and re-enter the resource pool | Retirement preserves the two-phase protocol: detach under the queue lock, destroy resource owners or `DetachedArenaOwner` outside the lock, then reacquire the lock and advance generations with `finishReclaim`. Metal retained references do not make in-lock destruction safe |
| `CpuReadyTape::SourceRef` | Generation-bearing queue locator; Tape owns the source/page storage | It remains only for ineligible legacy completion identities and synchronous planner/encoder resolution. Eligible post-encode session/submission entries are replaced by receipts | Required until the final synchronous borrow ends and receipt activation succeeds; stale locator resolution then fails after two-phase finish advances generations |
| `PublishedCommandRef` | Source/storage locator plus diagnostic slot, seqId, and command index | `PassState::pendingClearCommand` remains pre-encode/session state. Post-encode GPU samples carry `EncodedCommandId` through submission and completion | Pending clear makes its source ineligible; locator-free GPU attribution does not pin payload storage |
| `RetainedEncodeSourceLocator` / partition snapshots | Immutable locator values owned by bounded planner/cursor scratch | The value can live in an explicit range vector, but the vector span is synchronous and every resolution revalidates the represented source | Must not enter submission/completion storage. The planned locator-free conversion occurs only after the command has been consumed |
| `EncodeChunkSessionStorage` Metal encoders, command-buffer tail, attachment/hazard/binding shadows | EncodeSession owns these until finalization/reset | Active encoders and shadows do not move into `PendingCompletion`; finalization moves the tail command buffer and selected diagnostics/callbacks | Not payload-reclaim authority. An open session still requires source storage for future synchronous planning and pending-clear attribution |
| session `postCommitCallbacks`, `completionCallbacks`, capture state, sample buffer/samples | Session, then `QueueSubmissionRecord`; completion callbacks/samples and retained owners move to `PendingCompletion` | Yes | Each callback capture and diagnostic record requires a separate ownership audit. Metal retained references do not retain arbitrary C++ captures or Tape pages |
| `fixedCompletionSources` / `PendingCompletion::fixedCompletionSources` | Session registers natural FIFO order; submission and completion watcher own bounded copies containing either a locator or a queue receipt, plus the queue-sealed completion projection | Yes, intentionally | Receipt identities are authoritative for retired payloads; locator identities retain legacy completion/reclaim. Mixed lists validate and complete once in strict `seqId` and Present order |
| `retainedPayloads` | Submission, then pending completion | Yes, intentionally | Generic GPU-lifetime owner. The native probe confirms an empty EncodeSession retained here is destroyed when the record releases it; the production coordinator currently drops a completely empty session before calling the retention helper |

The use-stamp ordering is: Direct publication assigns `seqId`, marks resource
uses, seals and publishes the source; Represented encoding consumes all borrowed
payload bytes synchronously; the ordinary Metal command-buffer factory retains
referenced Metal objects; eligible payload retirement replaces the completion
locator with a receipt and returns Tape storage while keeping callbacks,
resource owners, and GPU work; tail GPU or device-loss settlement expands mixed
`QueueCompletionSource` entries exactly once. The Metal factory contract is recorded
in [the command-buffer retention research note](../../../docs/research/metal-command-buffer-retention.md).
It proves Metal-object retention only, not CPU payload retirement.

`StateOnly` creates no ticket, and ordered Legacy/control work can interpose in
the raw timeline, but the published-source allocator remains dense. Therefore
`EncodedCompletionSpan::sourceCount` is explicit and is checked against
`lastSeqId - firstSeqId + 1`; gaps and duplicates reject. The queue-owned span
validates the completion projection, while each bounded completion entry carries
either a generation-checked receipt or a legacy Tape locator. Receipt lookup is
source-kind-neutral and cannot resolve retired pages.

Source/page `SessionCap` observability now records only accepted cap events. A
typed admission result reports predecessor usage, candidate payload and actual
wrap-padding demand, required candidate pages, required session totals, and
independent source/page exceeded bits. Runtime counters split source-only,
page-only, and combined events and retain peak values for each demand field.
Native admission cases pin all three attributions and the zero/nonzero wrap
boundary. This changes neither capacity policy nor release selection; fresh
wild evidence is still required before changing the cap.

The first post-wake GT2 validation was runtime-clean with zero pressure-created
releases, but failed locality: tape-on command buffers rose
`+36.728%/+36.745%`, with `3,698/3,641` isolated sources and `252/252` byte-cap
releases. Inspection proved that Legacy `SourceSemanticSummary::byteCount` was
its flattened logical replay extent but was charged against the Arena-derived
session byte cap. Admission now preserves that logical extent for
validation/telemetry and uses a distinct session Tape-byte charge: exact
constructed bytes for Arena and reserved Tape pages for Legacy, while source/
page/block/retention/ticket/Ready/command-buffer bounds remain active. Legacy
`ChunkSlot` vector heap bytes are not directly represented by this byte credit;
compatibility source and slot counts bound them only indirectly. Native
admission evidence covers the production charge mapping,
large-logical/small-Tape Legacy admission, cumulative byte-cap behavior, and
deterministic Tape-byte over-cap isolation.

The representation-aware byte correction restored the subsequent reproducible
GT2 tape-on command-buffer rate to the tape-off `4.000` envelope, but render
passes remained elevated. A fresh instrumented run encoded 1,591 Presents with
6,421 command buffers (`4.036/Present`) and 28,029 render passes
(`17.617/Present`). The active-render snapshot was valid and applied 8,012
times, but 3,028 applications remained unmerged, no moved replay head was
proved or activated, and only 10 cycle plus 2 second-non-draw rejections were
observed. The 2,864 distance-one same-key re-entries account for 94.6% of the
unmerged applications. This disproved dependency rejection inside one source
as the dominant cause: the per-source planning window did not contain the
returning attachment command in the prevalent cross-source `A | B | A` shape,
even though the queue had already retained a bounded Ready prefix.

The bounded multi-source implementation then made that prefix available to one
combined planner without weakening dependencies. Its final no-gputrace GT2 run
encoded 1,580 Presents with 6,380 command buffers (`4.038/Present`), 27,802
render passes (`17.596/Present`), 2,823 distance-one same-key re-entries, zero
GPU command-buffer errors, and zero chunk rejects. Of 1,527 attempted windows,
531 stopped at an explicit eligibility boundary and all 996 valid windows
merged the active-pass seed. Every merged window nevertheless linearized to
natural order; no safe cross-source head movement was proved or replayed, and
neither the second-non-draw nor blocked-cycle diagnostic fired. Compared with
the earlier tape-off locality envelope of about `4.000` command buffers and
`15.77` render passes per Present, command-buffer locality is retained but the
roughly `+11.6%` pass excess remains. This is a failed `R-BACK-2.50` locality
gate, not evidence for promotion.

The result closes this bounded serial planner as an immediate measured GT2
remedy, while its qualified fragment replay and FIFO completion transaction
remain useful correctness foundations. It does not yet prove why every merge
was natural: the permanent counters omit the already-collected first matching
pass distance, so an adjacent target and an intervening target kept after a
producer are aggregated. Correlating `NaturalAfterMerge + SeedMerged` with that
distance is the remaining diagnostic step. If intervening windows dominate,
the combined graph establishes that serial reordering must preserve `B` before
the returning `A`; one logical pass across that order then requires segmented
physical-pass load/store and side-effect continuity, potentially the guarded
Metal 4 lane. `MTLParallelRenderCommandEncoder` would not remove such a
dependency. The current TLA abstraction covers source/page capacity rather than
this call-local FrameGraph planning proof, qualified fragment transaction,
representation-specific byte axis, or Metal effects.

The subsequent R5 diagnostic run attempted 1,654 bounded windows. It rejected
740 before planning: 226 at Present and 514 as
`cpu_ready_multi_source_eligibility_nonconsecutive_identity`; the remaining 914
merged the active seed but retained natural order. Inspection of the production
identities attributes those 514 rejections to exact `rawOrdinal + 1` checking,
not a semantic or completion boundary: Legacy sources carry no raw coordinate,
and StateOnly or Legacy raw entries can create forward gaps without creating a
Ready source. Bounded preflight now keeps exact `sourceOrdinal` and `seqId`
adjacency, treats zero raw identity as absent, and requires only strict monotonic
advance between observed nonzero raw coordinates. All capture, initializer,
ordered-release, semantic, admission, replay, and dependency fences remain
unchanged. This is a cause-specific bookkeeping correction with native evidence,
not a promotion result; no post-fix wild locality rerun has been recorded.

The subsequent R6 same-build A/B retained a measurable locality gap: tape-on
reported `17.654` render passes per Present versus tape-off `15.776`. The
tape-on side recorded 2,832 distance-one render-target-plus-depth re-entries;
2,630 were within the same `seqId`, and the corrected source-identity fallback
count was zero. This narrows the next observation point to source-local
passcoalesce and motivates the R7 conservation counters above, but it is
diagnostic evidence only: it does not satisfy `R-BACK-2.50`, promote the Tape
gate, or extend the current TLA models to FrameGraph or Metal behavior.

The R7 diagnostic run completed 1,500 Presents with 6,060 command buffers
(`4.040/Present`), 26,434 render passes (`17.623/Present`), and 2,733
distance-one re-entries. Its original source-local split reported 4,156
candidate evaluations as 2,720 merged, 1,431 blocked-cycle, and 5 returning-
non-draw terminals. That split is not final attribution: unchanged rejected
windows could be counted again after unrelated fixpoint merges, and a virtual
seed that absorbed a source pass could hide later source-owned returns. It also
did not say whether an evaluated merge survived the head-stable frontier and
final replay-plan validation. R7.1 corrects those diagnostic contracts and adds
mutually exclusive final replay outcomes while leaving valid-graph planning
unchanged. Its counters need a new runtime collection; the R7 totals must not be
used as the corrected population. The recorded provider hash begins
`35286e`, and the artifact is
`experiments/output/app-d3d9-3dmark05-source-local-return-on-gt2-r7-20260806/result.json`.

The corrected R7.1 run recorded 1,560 Presents, 6,299 command buffers, 27,542
render passes, and 2,888 distance-one same-key re-entries. Its stable candidate
population was 4,449 evaluations: 2,929 merged, 1,513 dependency-wedged, and 7
returning-non-draw terminals. Final attribution conserved that population as
4,202 candidates / 2,715 merges across 1,376 frontier-rollback sources, 3
candidates across 3 final-natural sources, and 244 candidates / 214 merges
across 214 final-reordered-activated sources; final-invalid was zero. Thus
frontier rollback contains 94.5% of candidates and 92.7% of successful merges.
The active-render-seed moved-head fallback count of 10 is not a decomposition
of those 1,376 sources because it observes every seed-applied frontier decision,
while the source-local outcome observes only candidate-bearing sources and also
includes seed-absent paths. This motivated R8's mutually exclusive reason split
under the broad rollback bucket. The provider hash begins `1584d772`, and the
artifact is
`experiments/output/app-d3d9-3dmark05-source-local-return-on-gt2-r7-1-20260806/result.json`.

R8 then recorded 1,440 Presents, 5,815 command buffers (`4.038/Present`),
25,367 render passes (`17.616/Present`), and 2,605 distance-one same-key
re-entries. Its 4,037 candidates contained 2,675 successful merges. Final
attribution assigned 3,747 candidates / 2,418 merges across 1,243 frontier-
rollback sources, and every one was `MovedHeadUnproved`; invalid-plan, live-set-
mismatch, and duplicate-command were all zero. Final-natural contributed one
source/candidate and no merge, while final-reordered-activated contributed 257
sources, 289 candidates, and 257 merges. GPU command-buffer errors and chunk
rejects were zero. This excludes planner validity and live-set corruption as the
observed GT2 locality blocker and justifies the implemented clean closed-encoder
frontier proof. It does not promote the gate: accepted clean-closed replay is
currently inferred from final-reordered activation plus the native state/path
fixtures rather than a dedicated accepted-state counter, and a fresh post-change
wild visual/locality/performance run is required. The staged provider hash is
`99df1cafdf0ed855`, and the artifact is
`experiments/output/app-d3d9-3dmark05-source-local-frontier-reason-on-gt2-r8-20260806/result.json`.

R11 supplied a same-build fragment-off/on GT2 comparison with staged unix
provider hash `780333fb9d959db5`. Fragment-off completed 1,582 Presents with
6,327 command buffers (`3.9994/Present`) and 24,967 render passes
(`15.7819/Present`). Fragment-on reached 720 Presents with 2,926 command buffers
(`4.0639/Present`) and 11,256 render passes (`15.6333/Present`), but then stopped
advancing and the wrapper terminated it: `timed_out` is true and the return code
is 143. GPU command-buffer errors, chunk rejects, and the two post-effect
fragment fatal counters were zero. The artifact's top-level `status: "pass"`
means that its collected checks found no listed failure; it does not override
the timeout and must not be cited as a completed or promotable run. The paired
artifacts are
`experiments/output/app-d3d9-3dmark05-multisource-fragment-off-gt2-r11-20260806/result.json`
and
`experiments/output/app-d3d9-3dmark05-multisource-fragment-on-gt2-r11-20260806/result.json`.

The leading code-level cause candidate was lock scope rather than Metal or
replay correctness: combined FrameGraph construction/resource-alias planning
and transaction observation could execute while holding the queue scheduling
mutex, excluding Ready publication, reclaim, and other scheduling progress.
The coordinator now reserves the exact prefix as `TentativeRepresented`, drops
scheduling for planner/resource work, reacquires and revalidates every source
identity plus release, lease, capture, initializer, and frontier state, then
commits or restores the exact Ready prefix without effects or observation. A
qualified transaction invokes its observer exactly once in natural FIFO order
only after all fragment effects and carrier folds succeed, with scheduling
released. This is an implemented lock-scope correction, not a verified runtime
fix: R11 predates post-change evidence, so a completed same-build GT2 rerun is
still required.

R14 temporarily executed validated `NaturalAfterMerge` results through the
exact fragment transaction. The run timed out after 281.43 seconds with return
code 143 at 900 Presents. Its partial interval recorded 713 exact-natural
windows / 1,882 sources / 1,882 runs, 1,414 planned windows, and 111 carrier
fallbacks. GPU errors, chunk rejects, completion-FIFO failures, and fragment
fatals were zero, but progress still stopped. At termination one valid current
lease used 6 sources / 121 pages, Tape residency was 8 sources / 123 pages, and
the source-plus-page cap counters totaled 34, so neither a full lease nor a
reported correctness failure explains the stop. The artifact is
`experiments/output/app-d3d9-3dmark05-exact-natural-on-gt2-r14-20260806/result.json`;
the staged unix provider hash is `2148c3280913301d`. The execution change was
removed. `NaturalAfterMerge` remains on source-local fallback until a distinct
progress-safe contract and completed runtime evidence exist.

R15 added source/page cap-demand observation without changing policy. Tape-on
GT2 completed 1,493 Presents with return code zero and no timeout, but still
failed strict locality against the R13 tape-off reference: command buffers were
`4.0422/Present` versus `3.9994` (`+1.07%`) and render passes were
`15.9210/Present` versus `15.7752` (`+0.92%`). Accepted cap releases separated
into 14 source-only, 24 page-only, and one combined event. The peak predecessor
was 30 sources / 384 pages; the peak candidate was 62 payload pages, 36 wrap
padding pages, and 96 required pages; peak predecessor-plus-candidate demand
was 31 sources / 437 pages. The current 512-page policy leaves 385 session
pages after reserving 127 successor pages. A 640-page bounded experiment would
leave 513 session pages and should remove the sampled page-axis releases, but
cannot remove source-axis releases because it does not change the 31-source
publication ceiling. The artifact is
`experiments/output/app-d3d9-3dmark05-cap-observe-on-gt2-r15-20260806/result.json`;
the provider hash is `15ae6d5af2055d13`.

R16 increased only the streaming page high/low water from 512/256 to 640/320;
source, Ready, and ordinary 64-page source limits were unchanged. GT2 completed
1,529 Presents with return code zero and no timeout. Page-only cap releases
fell from 24 to 11, while source-only releases were 16 and one event exceeded
both axes. Command buffers improved from `4.0422/Present` to
`4.0275/Present`, but render passes were `15.9477/Present`; relative to R13
tape-off, strict locality still failed by `+0.70%` command buffers and `+1.09%`
render passes. The larger session reached 509 predecessor pages and exposed a
new 546-page required-total peak, demonstrating that finite page growth defers
rather than removes the release boundary. The artifact is
`experiments/output/app-d3d9-3dmark05-page640-on-gt2-r16-20260806/result.json`;
the provider hash is `3f91dc477331ccfb`. The 640-page policy is a bounded
command-buffer mitigation only; it is not pass-streaming promotion evidence.

R17 canonicalized an exact redundant carried completion list before reordered
fragment effects. Native tests require complete equality across source/storage
generation, slot, sequence, Present, command range, length, and order, and
require final FIFO republish after fold/finalization. GT2 completed 1,542
Presents with return code zero, no timeout, zero carrier fallback (down from
27), and zero GPU/FIFO/post-effect fatal errors. Locality did not improve:
command buffers were `4.0305/Present` and render passes were
`15.9754/Present`, or `+0.78%` and `+1.27%` against R13 tape-off. Source/page
cap releases were 18/11, and distance-1-through-4 same-key re-entry totaled
286. The artifact is
`experiments/output/app-d3d9-3dmark05-carrier-canonical-on-gt2-r17-20260806/result.json`;
the provider hash is `7c0b5a1aa784a17f`. Exact completion duplication was a
real safety/opportunity seam but is not the sampled locality blocker.

R18 added attribution-only replay-window provenance without changing replay,
pass, cap, or release policy. Its GT2 run completed 1,559 Presents with zero
GPU errors, chunk rejects, completion-FIFO failures, or fallback-conservation
loss. Natural source-local fallback conserved 1,464 started/completed windows
and 3,734 sources; rejected-permutation fallback conserved 77 windows and 348
sources. Of 218 distance-1-through-4 same-key re-entries, zero were classified
as Natural same-window and 173 as Natural cross-window. The artifact is
`experiments/output/app-d3d9-3dmark05-natural-attribution-on-gt2-r18-20260806/result.json`;
the staged unix provider hash is `66f082a6fde6fd54`.

R19 implements the missing attribution seam.
`PassState` now owns an exact `(seqId, encoderIndex)` token; passcoalesce records
the exact successful active-seed merge target in a pre-sized bounded sink; the
planner maps and sorts the complete source-local witness set; and only a
revalidated `NaturalAfterMerge + SeedMerged` fallback receives synchronous
tickets. Exact-target joins require the prior same-key physical token and every
one-through-four intervening pass to prove the current Natural window. Native
tests pin active `A | B,A`, wrong target/token/window, multi-merge ordering, and
fail-closed witness accounting. Immediate active `A | A,B` is counted as a
continued ticket rather than an unconsumed target. Issued tickets conserve as
matched + continued + mismatch + unconsumed, while witness overflow and
mapping mismatch remain separate.
The token is exposed separately from semantic active-render completeness and
equality. A token-only revalidation mismatch preserves the accepted plan and
represented prefix, issues no ticket, and increments `seed_instance_stale`;
absence increments `seed_instance_unavailable`. Ticket issuance is owned by
the post-admission encode guard, so early rejection remains unissued and all
issued calls conserve terminal outcomes. Perf-off/empty-target calls skip the
resolver and pass-start classifier.
This is attribution only: no replay, pass lifetime, cap, release, or completion
policy changed. The GT2 wild run completed 1,565 Presents with 6,306 command
buffers (`4.02939/Present`) and 24,990 render passes (`15.96805/Present`). All
2,121 issued active-seed tickets were `continued`; reopened matched, mismatch,
unconsumed, stale, unavailable, witness overflow, witness mismatch, and
d1/d2/d3-4 seed bridge counts were all zero. The 199 short cross-window
re-entries therefore do not contain a hidden carried-seed reopen population:
every observed active-seed merge target continued the already-open physical
pass. Admission waited 22 times for 198.472 ms total; GPU errors and completion-
FIFO failures were zero, and sampled average FPS was 23.540. The artifact is
`experiments/output/app-d3d9-3dmark05-active-seed-bridge-on-gt2-r19-20260806/result.json`.

R19 resolves the R18 cross-window ambiguity and rejects the active-seed merge
itself as the sampled render-pass-close cause. Do not proceed with an R20
checkpointed-Natural execution experiment: all eligible targets already
continued the seed pass, so checkpointing that result has no demonstrated pass
boundary to remove. `NaturalAfterMerge` remains non-executable. The next
experiment must attribute the actual physical pass-end causes before selecting
another execution policy.

R20 implements that next observation point without enabling checkpointed
Natural replay or changing scheduling. Queue submission paths propagate a
typed finalize cause (`SessionCap`, independent, initializer, producer wait,
drain, or fail/other) into the session finalizer. Before `endRender` clears the
active identity, a fixed encode-thread ledger records the exact physical token,
attachment key, encoder split reason, and finalize cause. Exact-token pass starts attribute immediate same-key reopen by
finalize cause, while Natural short-cross re-entries attribute the prior
same-key instance's split reason with matched/missing conservation. Present
deterministically terminalizes remaining entries. Native pure tests pin exact-
token hit/miss, later short-cross lookup, and frame reset. A production 30+1
source-cap fixture pins active A finalization followed by same-key A reopening
on a new carrier, with 31 replay calls, two pass begin/end pairs, FIFO
completion, and perf-off structural equivalence with zero ledger work.

The R20 GT2 run completed 1,560 Presents with 6,285 command buffers
(`4.028846/Present`), 1,595 primary command buffers (`1.022436/Present`),
24,906 render passes (`15.965385/Present`), and sampled average FPS 23.296.
The full ledger conserved exactly as `24,906 = 30 adjacent + 23,317
nonadjacent + 1,559 not-reopened-before-Present`, with zero missing. Its
Final-only subset also conserved exactly as `31 = 30 adjacent + 1 nonadjacent
+ 0 not-reopened`, with zero missing. Final causes were 29 SessionCap and two
initializer waits; adjacent same-key causes were 28 SessionCap and two
initializer waits, exactly matching the 30 permanent same-key-adjacent
observations.

Natural short-cross attribution also conserved: all 198 observations matched,
zero were missing, 195 followed `ClearBarrier`, and three followed render-target
change. Final and every other split-reason bucket were zero. Active-seed tickets
conserved as 2,032 issued and 2,032 continued, with all other outcomes zero.
The 29 accepted capacity events split into 18 source and 11 page caps. Admission
waited 20 times for 320.393 ms total, with a 128.752 ms maximum, but created no
pressure release. GPU command-buffer errors and completion-FIFO failures were
zero. The artifact is
`experiments/output/app-d3d9-3dmark05-pass-close-ledger-on-gt2-r20-20260806/result.json`;
the provider hash begins `9df3b12`.

R20 therefore identifies deterministic capacity finalization as the measured
session-boundary locality blocker: 28 cap-finalized passes reopened the same key
immediately, while the Natural short-cross population is fully explained by
ClearBarrier and render-target-change closes and contains no Final close.
Increasing the cap would only defer this deterministic boundary, and executing
Natural plans has no matching close to remove. The next structural target is
to separate payload/source residency lifetime from the live Metal session and
completion identity, so reclaim/back-pressure can advance without forcing the
active pass to finalize. No cap enlargement or Natural execution experiment is
indicated.

R21 implements that lifetime split and closes two startup/reclaim races found by
the production gate. First, first-lease accounting no longer counts the unique
ordered-tail `Writing` publication both as unavailable residency and as reserved
successor headroom. The lease receives only an acquisition-time credit for an
exact, generation-valid claim within the fixed successor vector; the resulting
lease continues to own the complete vector. The native production fixture pins
the former 31 Ready / 511-page plus one Writing / 512-page deadlock and proves
all 32 sources encode and retire in FIFO order without a pressure-created
submission. `SessionCapacityLease.tla` independently requires the same startup
to acquire, admit, retire, and publish. Second, post-encode two-phase payload
destruction now exposes a control-side `Retiring` state paired only with Tape
`Reclaiming`, so a writer-side invariant cannot resolve payload ownership while
destruction runs outside the queue mutex. Deterministic native coverage and a
20-run join stress pass pin that transition.

The same-build GT2 R21 pair used staged unix provider hash
`a2f6e0101bd072ea`. Tape-off completed 1,553 Presents with 6,211 command buffers
(`3.99936/Present`) and 24,496 render passes (`15.77334/Present`). Tape-on
completed 1,464 Presents with 5,861 command buffers (`4.00342/Present`) and
23,330 render passes (`15.93579/Present`), differences of `+0.10%` and `+1.03%`
respectively. It returned code zero without timeout, GPU command-buffer error,
chunk reject, receipt failure, or post-encode retirement failure; all 7,505
retirement attempts succeeded. Peak Tape residency reached 33 sources / 638
pages and admission closed/reopened nine times, so the run exercised the
near-cap path rather than avoiding it. The paired artifacts are
`experiments/output/app-d3d9-3dmark05-post-writing-credit-off-gt2-r4-20260806/result.json`
and
`experiments/output/app-d3d9-3dmark05-post-writing-credit-on-gt2-r3-20260806/result.json`.
This clears the known GT2 liveness and command-buffer-locality blockers. It does
not by itself satisfy the multi-scene visual/performance promotion matrix, so
the runtime gate remains default off.

R14 and R15 evaluated active-session retained-head lookahead as an
observation-only experiment. R15 observed 409 held windows and 394 optimizer
candidates; every candidate retained natural `0|1` source-run order. Cross-
source return, `0|1|0`, `0|1|0|1`, Clear-rejected return, and both valid planned
single-cut shapes were all zero, while the strict locality comparison failed.
The experiment therefore did not expose the missing suffix and could not pass
`R-BACK-2.50`. Active-session retained-head parking, its physical-pass token,
seedless shadow planner call, candidate-shape/run diagnostics, and dedicated
performance counters were removed rather than left on the production hot path.
The ordinary replay permutation validator and bounded planner remain
authoritative. Fresh-frontier retention remains because it solves the distinct
startup window without delaying work already admitted to an active session;
native coverage requires the active case to consume its sole Ready head
immediately.

The final fresh-only plus replay-suffix composition GT2 run is
`experiments/output/app-d3d9-3dmark05-post-fresh-only-composite-lookahead-on-gt2-r16-20260807/result.json`.
It completed 1,497 Presents without timeout, command-buffer error, or chunk
reject. Fresh retention engaged only three times and waited 0.281 ms total.
Replay-active Store proof no longer discarded represented successor ranges:
`render_pass_no_lookahead_invalid` fell from 322 in the R13 diagnostic run to
zero, while 9,518 observations remained correctly classified as bounded-suffix
exhaustion. This is correctness/observability progress, not a locality
promotion: command buffers were `4.00334/Present`, render passes were
`15.90047/Present`, and normalized tile preservation was
`103.476 MiB/Present`, versus the tape-off reference `3.99936`, `15.77334`, and
`100.262 MiB/Present`. The command-buffer and pass gates therefore still fail;
the remaining target is cross-window logical-pass continuity rather than more
retained-head waiting or elimination of defensive suffix-exhaustion evidence.

The follow-up exact-joint GT2 run is
`experiments/output/app-d3d9-3dmark05-post-clear-open-joint-on-gt2-r17-20260807/result.json`.
It completed 1,529 Presents with zero timeout, GPU command-buffer error, or
chunk reject. The strict tape-off comparison still failed at
`4.00327` command buffers, `15.95160` render passes, and
`105.066 MiB` of tile preservation per Present. The exact
`prior A / Clear-open B in the same source / returning A in a newer source`
joint identified 226 of 276 distance-one re-entries and attributed
`1,356 MiB` of prior-A Store plus `1,296 MiB` of returning-A Load,
or `1.734 MiB/Present`. That is about 36.1% of the observed
`4.805 MiB/Present` tile-preservation excess and about 82% of the distance-one
re-entry count. Only 54 targets were `NaturalCrossWindow`, accounting for
`0.422 MiB/Present` (about 8.8% of the tile excess). A corrective planner must
therefore prove the exact source-qualified Clear-open shape across ordinary as
well as natural-fallback provenance; a Natural-only policy cannot recover the
dominant measured opportunity.

## Historical Verdict

The removed carrier experiments demonstrated that overlap alone is insufficient:
waiting can move into writer/drain pressure, and source-grain publication can
increase command buffers, logical pass reopens, and tile cost. The current
roadmap therefore treats source storage, logical-pass ownership, and Metal
execution lanes as separate contracts and applies `R-BACK-2.50` before any
default promotion.
