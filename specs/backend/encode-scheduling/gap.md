---
type: "Spec Gap"
title: "Encode Scheduling Gap"
description: "Implementation and evidence gaps for CPU-ready, EncodeSession, partition, and Metal execution scheduling."
tags: [specs, backend, encode-scheduling, gap]
---

# Encode Scheduling Gap

This is the detailed owner for `R-BACK-2.35`–`R-BACK-2.50`,
`R-BACK-2.57`–`R-BACK-2.67`, and `R-BACK-2.76`–`R-BACK-2.81`. The parent
[backend gap](../gap.md) keeps only a routing summary. Historical experiments
remain in `docs/perfomance/`.

## Current status correction (2026-08-21)

The production proof-core adapter and its fail-closed enforcement are
implemented. The two previously reported proof blockers (per-pass epoch
identity and bounded coverage-row capacity) are closed by independent epoch
re-derivation and the exact O(1) coverage fold; the typed wild re-measurement
reports zero certificate-invalid candidates on the GT2 and SFIV probes. The
parallel provider is `ExplicitParallel`/`parallel` only by explicit request,
remains default-off, and has no promotion claim. Any older row text below that
calls the adapter or those proof blockers open is historical attribution, not
the current gap; the remaining work is workload-specific performance/promotion
evidence and the documented reopen triggers.

| Area | Status | Current evidence | Missing implementation or evidence |
|---|---|---|---|
| CPU-ready publication boundary (`R-BACK-2.35`–`2.41`) | Direct/StateOnly routing and serial arena consumption implemented behind default-off promotion gate | Commit admission fixes the cutover decision once per raw entry. Gate-off preserves the historical synchronous combined resource-mark/backing-capture operation, worker Legacy replay without planning, and the compatibility publication cap of `kMaxQueuedChunks` GPU-inflight sources. With `DXMT9_CPU_READY_TAPE=1`, admission instead persists the validated raw blob, resolved objects, deduplicated resource identities, captured buffer backings, wrapper retention, and raw residency; the replay worker then plans each complete raw chunk once. `StateOnly` replays without a ticket or mark, eligible Direct chunks acquire a sized strict Tape lease, construct typed arena payload in place, and apply exact `seqId` marks before Ready visibility; planned Legacy/Inline mark before replay, and post-semantic Direct failure is fail-stop with no semantic fallback. Only the replay worker may wait for Direct admission pressure. `SourcePayloadView` carries both legacy and arena payloads through the common backend and serial partition cursor; DCE holds an arena successor without exposing it to the legacy proof window, and with the gate on the encode thread runs the Tape session lane (`runCpuReadySessionEncodeLoop`), which admits Arena sources into shared-session submissions with exact Tape completion identity instead of forcing one-source standalone submissions. Native evidence covers Direct planner-to-publish-to-consume-to-completion/reclaim, StateOnly/no-ticket, pressure wakeup, post-semantic fail-stop, compatibility inflight backpressure, and process-separated gate-off synchronous combined admission. | Keep the runtime gate default off until `R-BACK-2.50` formal/equivalence, wild visual/locality, and no-gputrace overlap/performance gates pass. Quantify whether direct construction reduces producer/replay cost without increasing command-buffer or render-pass shape. Sources exceeding the streaming source/page/segment hard caps and synchronous Inline paths intentionally retain compatibility payload construction; TriangleFan worst-case sizing is covered by Direct planning. Production multi-segment-plus-Present FrameGraph/session/Presenter evidence, explicit production partition subdivision, parallel render encoding, and Metal 4 segmentation remain separate work; cross-source session admission for both source kinds is tracked in the EncodeSession row below. |
| CPU-ready source residency/admission (`R-BACK-2.60`, `R-BACK-2.65`, `R-VERIF-2.14`) | split physical-residency and encoded-work accounting, ordered-tail Writing no-double-count, and bounded denied-first-lease pressure progress implemented behind the default-off Direct gate | `SessionCapacityLeaseState` still reserves the full physical source/page/byte/block/Ready/retention/ticket headroom before representation. A typed Tape snapshot separates older unavailable states from one unique current-generation ordered-tail Writing publication; only an exact claim within successor headroom is credited at first acquisition, while invalid identity/arithmetic fail-stops before the generation waiter. `classifyFirstLeaseCapacityWait` gives capacity generation priority, then permits one exact already-resident non-Present Direct Arena `ordinaryDirect` Ready head to execute standalone when a real Arena admission waiter would otherwise close the completion/reclaim cycle; the next denial parks until generation progress, and no pressure-created release event exists. Native production-loop evidence covers the full-Tape older-unavailable/Ready/pressure trace without sleeps, exact singleton FIFO submission, untouched suffix identity, and ordinary completion/reclaim, alongside the existing 31 Ready / 511-page + one Writing one-session trace and exact 128-source cap. `SessionCapacityLease.tla` retains detailed lease/refcount ownership; seeded `EncodeSchedulingProgress.tla` composes the pressure cycle and escape. | The same-build R21 GT2 tape-off/on pair clears the prior frame-12 progress blocker and restores command-buffer locality; it is one-scene evidence, not default-promotion evidence. Keep `DXMT9_CPU_READY_TAPE` default off until the remaining wild visual/locality/performance matrix passes. |
| Unified Arena source representation (`R-BACK-2.40`, `R-BACK-2.45`–`2.46`, `R-BACK-2.49`, `R-BACK-2.60`, `R-BACK-2.62`) | production multi-segment construction and Present implemented behind the default-off Direct gate / admission and end-to-end evidence fail promotion | The Direct planner emits an exact raw-record range table and packed `ArenaSourcePayloadLayout`, including dedicated jumbo segments. One strict Tape reservation owns all segment builders, publishes or aborts them atomically, exposes them as one logical `SourcePayloadView`, and reclaims them under one source/ticket/`seqId`/completion identity. Production replay flushes pending DrawRun batches at exact segment edges and visits every raw record once. A single final Present may be built in the Arena source while Query routes Legacy, Readback routes synchronous Inline, `UpdateTexture` routes Legacy, and non-final or multiple Present records route Legacy. Native specs cover packing, multi-segment ownership, Present publication, token abort, and source-qualified FrameGraph input. Clean H233 shows that removing the oversize bypass without reserving session headroom converts full-Arena sources into pressure-driven session fragmentation; the fixed headroom/cap policy is now implemented in the residency owner above. | Add a production multi-segment-plus-Present fixture through FrameGraph, shared EncodeSession, and Presenter; add Arena-source PSO-prefetch parity and source-qualified diagnostic evidence; prove that segment edges do not alter DAG, pass, submission, or completion shape. Keep `DXMT9_CPU_READY_TAPE` default off and legacy rollback intact until `R-BACK-2.50` passes. |
| Scoped FIFO replay drain (`R-BACK-2.51(d)`, `R-BACK-2.61`) | buffer lock/unlock scope implemented / promotion pending | A device-owned ledger canonicalizes shared-wrapper aliases by core-buffer identity, admission captures backing generations and raw residency, and buffer lock/unlock wait only for the relevant replay target with conservative terminal handling. Deterministic native and process-separated OFF/ON byte-identity evidence is present. A 2026-08-18 GT2 site measurement on `226922a2` shows the fence is effectively harvested: total drain wait `0.33 ms/present` (~0.9% of frame), with the historically dominant `dxmt9c_buffer_lock` down to `0.063 ms/present` (bypass counters active: 15,236 DISCARD / 52,400 NOOVERWRITE) and the largest residual being one global `get_swap_chain` wait per present at `0.249 ms/present`. | Complete wild/conformance promotion and paired performance attribution (the 2026-08-18 measurement sizes the win as small); broaden canonical access summaries beyond the current buffer lock/unlock allowlist only when another synchronous consumer needs it. |
| EncodeSession semantic lifecycle (`R-BACK-2.42`–`2.49`, `R-BACK-2.65`) | bounded multi-source serial planning, fresh-frontier Ready-head lookahead, and exact deferred terminal-suffix join implemented behind the default-off Direct/Tape gate / promotion evidence missing | The existing coordinator still bounds ordinary Ready planning to eight sources, preserves natural FIFO completion, and carries one active pass across the proven 8+1 window edge. The added source-local lane recognizes only exact `DrawRun(A), Clear(B), DrawRun(B) | DrawRun(A)`: it encodes the current `A`, retains a bounded pointer-free `DeferredTerminalSuffixState`, waits unlocked for one exact ordered-tail Writing successor within headroom, then either replays successor `A` before the older `Clear(B), B` suffix or drains naturally before successor effect. At prefix time it value-snapshots the replay frontier, complete active dependency and render-pass instance token, lease/release values, and the full controller plus pending-carrier capture boundary; it re-resolves payloads and revalidates those values after planning/charge and immediately before commit/replay. Semantic drains precede Ready handling; once absent, exact Ready wins simultaneous admission or writer pressure. Native evidence covers the exact join and default-off natural baseline, allocation-free proof/rejection, real lease/headroom charging, stale-successor restore, ordered `ExplicitFlush` release, stop, pending-carrier capture-start drain through the full capture predicate, Ready-over-admission-pressure, one observer, render-pass `3 -> 2`, one removed mid-chunk split, no held-edge action/sidecar/completion publication, exactly-once terminal action/Store handling, command-once, current-before-successor receipts, FIFO completion, and zero final residency. `CpuReadySessionProgress`, `SessionCapacityLease`, and `PostEncodePayloadRetirement` now cover bounded held ownership, writer publication, Join/StaleFailOpen/Drain, exact-successor-over-pressure, charge-once, no-retire-while-deferred, receipt/completion/reclaim ordering, and temporal progress; `dxmt9-verify-tla` is green. | For this terminal-suffix implementation there is no wild, visual, locality, performance, no-gputrace, or promotion evidence, and no GT2 improvement is claimed. `DXMT9_CPU_READY_TAPE` remains default off. The direct ordered-`ExplicitFlush` fixture exists, but API-originated Flush and Query/readback/`UpdateTexture`/Present/initializer/producer-wait control boundaries still lack production terminal-suffix fixtures; their pure drain policy is covered. The capture-controller-enabled and pending capture-object arms of the full capture boundary also lack separate production injection. Exact Ready with admission pressure is covered in production, but admission-pressure-only and writer-pressure natural-drain paths still lack production injection. Production multi-segment-plus-Present FrameGraph/session/Presenter evidence remains missing. Promotion requires a fresh same-build GT2 tape-off/on visual/locality/performance/no-gputrace comparison first, then paired GT1, GT3, and SFIV coverage plus repeated runs and conservation gates. Continuity after encoder end or command-buffer commit remains Metal 4 work under `R-BACK-2.64`. |
| Promotion gates (`R-BACK-2.50`) | partial | Permanent counters now split retirement attempts/success/ineligibility, receipt failures/depth, physical residency released, encoded-work cap closes, and GPU-outstanding current/peak from existing wait, command-buffer, pass, load/store, and tile shape. The Tape gate remains default off and the legacy payload-owning path remains the rollback baseline. | Enforce the expanded conservation gate in comparative tooling and collect fresh wild visual/locality/no-gputrace evidence. Do not claim a GT2 improvement or promote the gate from native/formal evidence alone. |
| Stable render scheduling provider modes (`R-BACK-2.66`) | canonical partition selector and distinct `ExplicitParallel` provider implemented / unified three-axis config partial | Device creation resolves `DXMT9_RENDER_PARTITION_MODE=identity|serial|parallel` once into queue-owned typed state and forwards it through every production encode path. Unset/`identity` select identity, `serial` selects the production explicit planner, and `parallel` selects the source-local WMT parallel provider with typed pre-effect serial fallback for ineligible passes. Unknown values fail closed to identity. Perf-disabled execution skips counter clocks and atomics but preserves an explicit parallel request. Enabled counters report requested/resolved partition modes and parallel worker activity. Native coverage pins the spelling and fallback matrix. `DXMT9_CPU_READY_TAPE` remains the source-delivery migration alias. | Unify source, partition, and segment axes in one complete `RenderSchedulingProviderConfig`; add canonical source/segment parsing, legacy Tape alias precedence, requested/resolved source and segment reporting, and the full mode matrix. Metal 4 execution remains owned by its row below. |
| Published source immutability (`R-BACK-2.46`, `R-BACK-2.59`) | behavior implemented for retirement eligibility / broader type-seal audit open | Published payload access remains const and call-local. Eligible storage mutates only after receipt activation and exclusive `Reclaiming`; locator-backed or borrowed payloads, pending clear, Present, and ordered-control sources remain resident. | A broader type-level seal and removal of the read-only `const_cast` in `makeEncodePartitionReplayStream` remain independent cleanup. |
| Post-encode payload retirement (`R-BACK-2.44`, `R-BACK-2.45`, `R-BACK-2.49`, `R-BACK-2.59`, `R-BACK-2.65`) | behavior implemented behind the default-off Direct gate / promotion pending | A bounded flat generation-stamped `PostEncodeCompletionLedger` installs source-kind-neutral receipts for eligible synchronously encoded Legacy and Arena sources. Session, submission, and pending-completion lists may contain mixed receipt and legacy locator identities. Retirement marks and detaches under the scheduling lock, destroys re-entrant owners outside it, then relocks to finish page/generation/control release; failures after receipt activation or Metal effects are fail-stop. `RenderEncoderGpuSample` uses locator-free `EncodedCommandId`. Present, pending clear, query/readback/update, ordered controls, and remaining borrows are ineligible. Completion validates stale/duplicate/ABA receipts before callbacks and preserves per-source, resource-waterline, and completed-Present ordering. Native specs cover ABA/stale/duplicate handling, mixed completion, two-phase Tape reuse, resource retention/callback ordering, more than 30 eligible sources in one session, the deterministic 128-source work cap, and both joined and natural-drain suffix paths: current receipt/detach stays blocked through pending suffix/effects/borrow, then current and successor activate, complete, and reclaim once in FIFO order with conserved residency and encoded work. `PostEncodePayloadRetirement.tla` now models deferred prefix/suffix/effect/borrow state and checks no-retire-while-deferred plus receipt/completion/reclaim safety and liveness; `dxmt9-verify-tla` is green. | Runtime gate promotion still requires fresh visual/locality/performance/no-gputrace evidence under `R-BACK-2.50`; no GT2 improvement is claimed. |
| Serial partition consumer (`R-BACK-2.57`, `R-BACK-2.58`) | production identity and explicit-serial callers implemented / GT2 production coverage observed | All backend streams use the range interface. Complete preflight, identity fail-open, command-once behavior, multi-subrange lifecycle, and production option forwarding are covered by partition, snapshot, and session native specs. The production comparison proves explicit subdivision retains one DrawRun setup, pass begin/end, split-policy decision, upload batch, and complete draw count. A GT2 scout exercised explicit serial consumption with zero GPU command-buffer errors and zero capacity fallback. | Matched repeated Metal-backed locality and visual evidence remains missing; the first unmatched scout is not promotion evidence. |
| Production partition planner (`R-BACK-2.62`) | explicit-serial production coverage observed / locality promotion pending | The queue-immutable canonical partition selector enables a fixed 256-range call-local planner after final replay selection. Large DrawRuns (threshold 64, target 32, minimum side 16) subdivide only at boundaries that preserve the existing compatible indexed-draw merge chain. Complete unsplit commands coalesce into command segments, so only actual subdivisions consume locator-bearing ranges. Full-plan validation precedes effects; typed invalid replay, capacity, snapshot, merge-preservation, and validation failures atomically select identity. Perf counters split identity/explicit selections, ranges/draws, subdivided and merge-preserved runs, fallback reasons, and planner CPU cost with clock reads gated behind perf enablement. Native specs cover threshold edges, determinism, mixed/reordered/DCE-empty streams, segmented Arena input, bounded overflow/malformed fail-open, wild-shaped range compression, selector resolution, and production serial consumption/shape. A same-build GT2 scout after compression observed 267 explicit selections, 680 explicit draw ranges, 21,760 explicit draws, 340 subdivided DrawRuns, zero capacity fallback, and zero GPU command-buffer errors across 1,281 presents. | Do not promote the serial planner from the first GT2 scout: its identity comparison covered 1,018 rather than 1,281 presents and changed draws/present by 1.42%; the strict locality gate also reported CB/present +0.01%, pass/present +0.51%, and tile-preservation MiB/present +0.24%. Collect matched repeated GT2 locality and visual evidence, then exercise GT1, GT3, and SFIV coverage. The independently planned `R-BACK-2.63` parallel provider is tracked in the next row. |
| Parallel render-pass executor (`R-BACK-2.63`) | production source-local WMT Stage 1/Stage 2b execution implemented / repaired bounded whole-command planner and boundary economics enforced pre-effect / deterministic correctness ladder green / completed matrix rejects default promotion / explicit opt-in | The sealed-pass builder uses the existing 64-draw eligibility quantum independently of the serial planner: it evenly covers a single DrawRun and groups indivisible commands at the earliest qualifying prefix while retaining a complete 64-draw suffix, absorbing a thin final suffix and never exceeding 16 children. Exact no-two-child-work, planner-invariant, child-capacity, and pass-capacity observations replace the former capacity aggregate. Production re-resolves every locator, draw, ABI, PSO, uniform payload, route, and resource identity under the residency pin, then enforces the pure economics classifier before render-pass preparation or any parent/child Metal effect. The classifier rejects child imbalance over 64 and requires both PSO and uniform identity changes at every produced child boundary; internal churn does not pay for first-bind reset. Rejection returns to exact serial replay; accepted passes retain coordinator ownership of Clear/Present, actions, sidecars, completion, and command-buffer state. Exact production counters conserve `considered = accepted + serial_fallback`, with typed reasons summing to fallback and min/max/imbalance plus boundary-transition totals attributable. `ParallelDrawBinding.tla`, pure single-run and whole-command subdivision/classifier/accounting tests, fake serial-fallback replay, child executor tests, and the Stage 2b Metal A-B-A readback cover the deterministic ladder. The pre-gate matched GT2 pair measured 21.087975 versus 19.729740 fps (-6.44%): all 2,670 parallel passes were safe Stage 2b but used 33,244 children / 1,017,361 draws (30.60 draws/child). The first post-gate pair measured parity at 24.188999176 versus 24.190946579 fps only because the defective whole-command planner rejected all 23,888 sealed candidates before economics and workers; it was vacuous economics evidence, not recovery. The valid decisive `r2` pair was non-vacuous and mixed: official GT2 +1.2441%, harmonic +1.7771%, and median -1.3590%, with 632 selected Stage 2b passes, 3,746 children, and 252,381 draws. The completed promotion matrix then rejected promotion. Repeat GT2 selected 641 Stage 2b passes / 3,799 children / 256,011 draws but measured harmonic -3.7556% and median -2.4312%; its identity official result was overwritten before preservation, so no official repeat delta is claimed. GT1 selected 3,245 Stage 2b passes and measured official -1.2492%, harmonic -0.5347%, median -0.9295%; all 22 bounded close-up captures showed no black polygon. GT3 selected 409 Stage 2b passes and measured official +2.2912%, harmonic +2.2642%. SFIV selected zero production parallel work, so its harmonic -0.9742% is descriptive only; its parallel screenshot is excluded after a harness capture miss. All eight lanes reported zero GPU errors, and locality was closely conserved for the non-vacuous 3DMark lanes. A 2026-08-16 SFIV counter run attributed the zero eligibility exactly: `parallel_pass_shadow_attempts=3240` with `parallel_pass_shadow_reject_command=3239`, matching `submit_stretch=3240` / `stretch_copy=3239` one-to-one — SFIV submits one StretchRect per present, and the sealed-pass builder's stream-wide command precondition (`DrawRun`/`Clear`/`Present` only, `dxmt9_parallel_render_pass.cpp` batch gate) rejected the whole source before candidates, sealing, or economics ran. That precondition is now replaced by per-interval extraction: `classifyParallelPassCommandRole` gives every command kind one total role, the five non-child coordinator helpers seal an interval at their own serial ordinal exactly as `Clear`/`Present` do, and only an unclassifiable kind still rejects a whole source. A coordinator command that a same-attachment draw resumes across fails both fragments closed, and `parallel_pass_shadow_coordinator_boundaries` / `parallel_pass_shadow_coordinator_splits` separate helpers met from passes failed. The 2026-08-16 post-extraction SFIV run opens eligibility at the shadow stage: `parallel_pass_shadow_attempts=2760`, `candidates=63182`, `sealed=63182`, `candidates_max=27`, against `0`/`0` before, with `coordinator_boundaries=2759` matching `stretch_copy=2759` one-to-one and `coordinator_splits=2673` (so `reject_command=5347`, two failed intervals per split plus one). SFIV still reports `shadow_eligible=0` because its passes are too small for two 64-draw children: `reject_no_two_child_work=51511` and `reject_hazard=12001`. GT2 is the coordinator-free regression control and is byte-identical to a matched `871dc233` baseline: `coordinator_boundaries=0`, `coordinator_splits=0`, and `shadow_{attempts=1560,candidates=24601,sealed=24601,eligible=3114,children=15534,draws=1199964}` on both. Zero GPU command-buffer errors on both apps. | Track closed 2026-08-18 as a parked preserved option (see spec §9 'Parallel partition lane: measured position and reopen trigger'): the provider is safety-proven and economics-calibrated but performance-neutral on every measured workload because the encode stage is not the critical path (producer-paced frames; encode ready-depth never exceeds 1). Reopen only when the observable trigger fires — encode-stage pacing — or a heavier-draw-density workload arrives; do not schedule further performance matrices on producer-paced workloads. Historical attribution retained below: SFIV zero eligibility was first attributed to the stream-wide command precondition, not the cost model: non-vacuous SFIV eligibility requires pass-interval extraction that keeps StretchRect (and other coordinator commands) at their serial position while sealing the surrounding DrawRun passes — the R-BACK-2.68–2.75 adapter increment — rather than cost-model tuning. Refine the cost model with scene-scale candidate data before a new attributable matrix; more repetitions of the unchanged policy are not the remaining gate. Cross-source/carried-session sealing, UP/tile/table child ownership, and Metal 4 joint completion remain open. |
| Parallel policy algebra and proof-carrying selection (`R-BACK-2.68`–`2.75`) | bounded proof core, checked fixed-point selector, adversarial native coverage, small TLC refinement, offline structural exploration, and production proof adapter/enforcement implemented / proof blockers closed / ExplicitParallel remains default-off with no promotion claim | `validateParallelPassSemanticPlan` copies a value-owned certificate only after an owner-issued synchronous snapshot authority re-resolves the sealed interval and a per-child resolver returns exact draw/read/write/attachment/route/epoch facts; it binds those facts to source/independent generations, first-child locator, command begin/count, pass epoch, coordinator proof, and ordered exact coverage. `selectParallelPassCandidate` performs structural economics checks only, then ranks checked Q16.16 values with positive benefit, fewer-child and full source-qualified range tie-breaks; invalid, overflowed, or non-positive inputs select serial in the proof core. `tests/native/backend/parallel_render_pass_spec.cpp` covers coherent mutations, exact read/write overlap, shifted/truncated absolute DrawRun ranges, stale sealing/interval authority, one resolver call per child, capacity bounds, arithmetic extremes, permutation-independent argmax, ties, whole-command coverage, and zero-effect fallback. `ParallelPolicySelection.tla`/`.cfg` covers the all-input-valid batch predicate, benefit-zero candidate skipping, selected-proof-only effect, serial fallback, join, parent end, completion, and fairness-backed progress. The authenticated Render Tape `policy-explore` command deterministically enumerates whole-draw-record 2/4/8/16-child structural candidates, excludes coordinator ranges, and reports source/pass identity plus draw, primitive, pipeline-input-section, and uniform-section counts. Candidate-capacity exhaustion invalidates the exploration rather than publishing a truncated result. The output is explicitly `structural_only` with `proof_core_validated=false`: the identity sidecar does not contain owner-issued first-draw, attachment, resource, route, hazard, or epoch facts, so the tool never fabricates a semantic certificate. The production coordinator now owns exactly one adapter into this core. `runParallelPassProofCoreAdapter` runs in `tryEncodeParallelPass` after every locator, ABI, PSO, uniform, route, and resource identity has been re-resolved under the residency pin and before render-pass preparation or any parent/child effect. `resolveParallelPassSnapshotAuthority` re-reads the pass from the producer's own sealed-pass batch by exact source/sequence/interval identity and fails closed on a missing or ambiguous entry; `resolveParallelPassCoverage` re-reads every command a child owns from the live source, checks attachment/route identity per command, and canonicalizes reads and writes through the same proof owner the producer used; `buildParallelPassCandidateCost` derives serial work, critical path, per-child setup, and imbalance as checked Q16.16 values from certified integers only. The adapter is a gate and not a relaxation: a certificate-invalid candidate never reaches the selector, an unselected candidate never reaches child creation, and the pre-existing economics classifier still runs unchanged and still rejects independently. `parallel_pass_adapter_{considered,certificate_valid,certificate_invalid,selected,serial_fallback}` conserve, and native coverage pins selected-and-executed, three certificate-invalid mutations with zero backend calls plus exact serial replay, invalid-economics and non-positive-benefit selection failures after a valid certificate, and conservation across every bounded child count. **The first measured GT2 adapter run rejects every production candidate**: `parallel_pass_adapter_considered=3114`, `certificate_invalid=3114`, `selected=0`. This is not a regression — a matched same-machine baseline built from `871dc233` reports numerically identical GT2 counters including `parallel_pass_selected=0`, `parallel_pass_economics_considered=3114`, `accepted=675`, `serial_fallback=2439` (all `reject_pso_first_bind`), `parallel_pass_shadow_{attempts=1560,candidates=24601,sealed=24601,eligible=3114,children=15534,draws=1199964}` and `draw_calls=2653372`. GT2 on this configuration already selected zero passes before the adapter existed, so the 675 economics-accepted passes were already being rejected downstream (leading-clear match, render-pass preparation, or unresolved late Store) and the certificate is not currently the binding constraint. What the adapter added was exact attribution for why those candidates were also not certifiable, from instrumentation giving exactly two causes, both in the proof core rather than the adapter. **(1) `PassIdentity` — closed.** `validateParallelPassSemanticPlan` required `coordinatorProof.firstPassActionEpoch == passActionEpoch` while the producer stored one source-wide coordinator proof and advanced the pass-action epoch at every boundary, so only a source's first pass could ever satisfy it — observed GT2 second passes carried `epoch=3` against `proofEpoch=1`. The fix does not relax the property; it makes it mean something. The producer now issues the coordinator proof per sealed pass and stamps it with that pass's own epoch, and the certificate no longer treats the stamp as evidence of itself: `deriveParallelPassActionEpoch` independently re-derives the interval's epoch by folding the shared `ParallelPassActionEpochState` over the generation-pinned replay stream through the shared `classifyParallelPassCommandRole` classifier, seeded by the coordinator rather than by the snapshot under test, and rejects unless the interval begins at a pass-opening draw whose derived epoch equals the stamp. The producer and the certificate share one epoch state machine, one classifier, and one clear-attachment-key helper; the producer additionally fails a source closed if its candidate lifecycle and the epoch fold ever disagree about whether a pass is open. Native pins in `tests/native/backend/parallel_render_pass_spec.cpp` cover fold determinism over every ordinal, the exact GT2 two-pass shape (`epoch=1` then `epoch=3` from a seed of `1`) certifying both intervals, a self-consistent stamp copied from another pass failing `PassIdentity` even through an echoing snapshot owner, a foreign stamp failing closed, and a stale storage generation failing `SourceIdentity` with zero epoch-witness reads — proving generation checks still precede the fold. **(2) `FirstDrawProof` via exact coverage — closed.** `ParallelPassResolvedCoverage::commands` was fixed at `kParallelRenderPassChildCapacity` (16) rows while `validateParallelPassSemanticPlanCoverage` required one row per command for a whole-command child, yet the producer bounds children to 16 without bounding commands per child — observed GT2 first passes had children owning 26, 30, 31, 36 and 52 commands, and only children owning 4-11 commands resolved. The resolution is neither a larger fixed capacity nor a lossy summary: `ParallelPassCoverageFold` streams the resolved commands through O(1) exact accumulator state (first command, previous command index and DrawParam boundary, running command and draw totals, first-failure reason plus locator). There is no hash in the type, deliberately, because a colliding summary would admit a false accept. The per-row predicates are enforced at append time against the previous boundary, the fold's fields are private so a resolver cannot fabricate a state its appends did not produce, and the 16-child capacity for children per pass is untouched. One predicate is stronger than the array form: whole-command rows must carry strictly increasing command indices, which implies the duplicate-freedom the old O(n) scan checked and also rejects an out-of-order row set that scan accepted — a fail-closed direction that can only shrink the accepted set. The evidence is an adversarial pin in `tests/native/backend/parallel_render_pass_spec.cpp`: a test-local stored-row reference reproducing the pre-change predicate set runs beside the fold over a generated domain of ~320 row sets (gap, overlap, duplicate, out-of-order, empty, DrawParam overflow, subrange, and exactly-boundary mutations at every position of one- to four-row children, whole-command and draw-subrange), asserting that the fold never accepts what the reference rejects, that accepted totals agree exactly, that rejected reason classes and first-failure locators agree, and that the only divergence found is the enumerated ordering strengthening. Separately, `wideWholeCommandChildrenCertify` drives a 61-command source through the production validator, confirms its widest child owns more than 16 commands, certifies it, and re-checks that per-row identity/range mutations at that width still fail closed. A downstream-consumer audit confirmed coverage rows never escape validation: the certificate copies only the snapshot and child plans, and the executor consumes sealed child ranges. A 2026-08-16 sweep ran `policy-explore` over all 29 archived GT2 bundles: the 12 without `identity.bin` fail closed as designed, and the 17 authenticated bundles yield 181 deterministic candidates (67/52/40/22 at 2/4/8/16 children; rejections 576 non-draw-record, 174 coordinator-record, 113 too-few-draw-records). That corpus is warm-up-frame scale — candidate `draw_total` p50 14, max 36, imbalance ≤1 — versus the runtime provider's scene workload of ~30–67 draws per child, so it exercises the explorer but is too small to calibrate the Q16.16 benefit function. A same-day GT2 scene-frame capture (`gt2-scene-policy-r1-20260816`, skip-presents 400, oracle-confirmed scene frame 387 with 1,199 events / 64 chunks / 3,704 records) then showed the explorer's structural limit rather than fixing the scale gap: only 1 of ~79 logical pass intervals produced candidates (23 draws) while 64 rejected as `non-draw-record`, because production PE streams interleave constant/state records with draws inside a pass. The whole-record enumeration operates in the tape record domain, whereas the runtime shadow producer operates post-import in the command domain where constants are folded into `DrawRun`; the two domains legitimately disagree at scene scale. | Scene-scale candidate distributions for Q16.16 calibration must come from runtime `parallel_pass_*` counters (already recorded: decisive GT2 `r2` 632 passes / 3,746 children / 252,381 draws) or from a record-to-command projection increment in `policy-explore` (the open tape-to-draw-slice projection item in `specs/verification/gap.md`); re-running the record-domain explorer on more scene tapes will not close the calibration gap. The production adapter now exists and is enforced. The earlier fail-closed measurement is historical; both proof blockers are now closed by independent epoch re-derivation and the exact coverage fold. Both blockers are now resolved in the proof core: the pass-action epoch is issued per pass and independently re-derived, and coverage is a streaming exact fold with no row capacity. Neither resolution relaxes a check — the epoch equality became a real binding rather than a tautology, and the coverage fold is strictly stronger on command ordering. The wild GT2 measurement now exists (2026-08-17, merged `0136be51`): `parallel_pass_adapter_considered=3210`, `certificate_valid=3164` (98.6%), `certificate_invalid=46`, `adapter_selected=3164`, and for the first time through the adapter `parallel_pass_selected=598` executed as Stage 2b parallel passes (598 worker batches, 3,540 child tasks), with zero GPU command-buffer errors and a visually normal in-scene screenshot at frame 1135. Adapter conservation holds exactly (3210 = 3164 + 46). The same-build SFIV probe confirms the structural opening is retained (60,388 sealed) with `eligible=0` now owned purely by the two-child 64-draw economics floor (`reject_no_two_child_work=49,318`). This is mechanism evidence only — a single unmatched run, not a performance or promotion claim. The opt-in `parallel` mode stays default-off. The later typed remeasurement closes that attribution debt: the residual is pass identity and the final remeasurement reports zero certificate-invalid candidates; no promotion claim follows. The Q16.16 cost function is deliberately a first honest model (draws, widest child, per-child setup, imbalance) and is not calibrated. Keep Render Tape structural evidence, proof-core evidence, and Metal oracle evidence separate; no result changes the default-off or no-promotion status. `DXMT9_PARALLEL_PASS_DRAW_QUANTUM` (env-only, clamped `[4,1024]`, default `kProductionPartitionDrawThreshold`) now exists so the sealed-pass builder's two-child eligibility quantum and the economics classifier's thin-child/imbalance bound can be A/B'd against the fixed `64` floor without touching the serial planner, and the always-on `parallel_pass_shadow_sealed_draws_{under8,8_15,16_31,32_63,64_127,128_255,256plus}` histogram (conserving against `parallel_pass_shadow_sealed`) exposes the sealed-pass draw-size distribution a future calibration would tune the floor against. The first calibration sweep and matched pairs now exist (2026-08-17, single build `b0d50687`, env-only A/B). SFIV quantum sweep {64,32,16,8}: the sealed-pass histogram shows 81% of SFIV sealed passes carry under 8 draws and only 3.8% carry 64 or more, so floor tuning cannot open SFIV — at quantum 8 the 1,630 candidates that reach the adapter all fail both the imbalance bound and the certificate (the certificate failures are untyped, raising the typed-reason debt to 1,630 samples), and eligibility at 32/16 (4/66 passes) dies before the adapter. SFIV parallel eligibility needs pass-granularity change (cross-source or merged-pass sealing), not floor tuning. GT2 quantum sweep {64,32,128} exposed that the quantum conflates three roles — eligibility floor, per-child minimum, and imbalance bound: at 32 the tightened imbalance bound rejects 2,461 of 3,116 candidates (`unbalanced_child`) and zero passes execute, while at 128 the looser bound plus fewer child boundaries lets 347 additional passes clear `pso_first_bind` (943 accepted, 896 selected, 2,400 tasks versus 647/603/3,570 at 64). Decoupling the imbalance bound from the eligibility quantum is the first concrete cost-model refinement this data demands. The ABBA-interleaved GT2 matched pairs (identity, parallel-64, parallel-128, two runs each) initially appeared to show a severe parallel-only hitch tail (harmonic 26.4 identity versus 22.0/21.3 parallel with flat medians); a same-day re-analysis proved that finding was a measurement artifact, retracted below. The imbalance decoupling knob (`DXMT9_PARALLEL_PASS_IMBALANCE_BOUND`, merged `2be3448d`: economics UnbalancedChild bound only, unset couples to the draw quantum byte-identically, explicit values clamp [4,4096]) then produced the decoupled sweep (2026-08-17). SFIV quantum-8 with bound 64/256: the whole pipeline is now open — certificate 100% valid, economics accepts 1,389/1,630 — and the final gate is the Q16.16 selector's positive-benefit requirement, which returns every candidate to serial (`adapter_selected=0`, `serial_fallback` equals considered). Pricing tiny 8-draw-child passes out rests on the model's own per-child setup estimate; whether that estimate is calibrated remains open. GT2 quantum-32 with bound 64: the imbalance rejections vanish but all 3,116 candidates then fail `pso_first_bind` — smaller children mean more boundaries and the all-boundaries-change condition becomes the next total gate. GT2 quantum-16 with bound 128 is the first small-child execution shape: 6,130 eligible, 1,270-1,280 selected at exactly 2 children per pass, zero GPU errors. **Measurement-artifact retraction (2026-08-18): the hitch tail never existed.** The tail attribution pass re-extracted per-frame walls anchored to the `[dxmt9-perf-frame ...]` prefix and found all four configurations statistically identical: total frame time 122.6-122.7s, p50 36.5-37.1ms, p99 59-65ms, max 146-159ms, about ten >100ms frames everywhere including identity. The earlier harmonic collapse came from a loose `wall_ms=` regex in the ad-hoc analysis that also matched the cumulative `parallel_pass_worker_wall_ms` counter in the periodic `[dxmt9-perf]` emissions — 13 samples worth 11.5 fake seconds injected into ~61 real seconds per parallel q64 run — while identity runs were immune because their zero worker-wall values were filtered out, fabricating a parallel-only tail and a spurious shape dependence. Corrected truth from anchored extraction: GT2 parallel execution is performance-neutral across every measured shape — true average fps identity 26.31, parallel-64 25.91 (-1.5%), parallel-128 26.20 (-0.4%), quantum-16/bound-128 25.95 (-1.4%), medians within 1%, zero GPU errors. The executor does not regress GT2; it also does not yet win (consistent with the H212 attribution that the residual wall is the game's own CPU), so `identity` remains the default on absence-of-benefit rather than presence-of-harm. A follow-up counter attribution then explained the neutrality structurally: in every matched-pair run — identity and parallel alike — `encode_dequeue_ready_depth_max=1` with zero `gt1` samples (the encode thread never has a backlog) and `completion_no_enqueue_wait_to_encode_dequeue` p50 of 10.3-11.7ms per source (the encode thread waits for the producer), while the encode stage wall `completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit` p50 is 19.2ms (identity) versus 20.3-20.4ms (parallel) — parallel execution does not even reduce the stage wall at these shapes because the per-pass fan-out/join wall (`parallel_pass_worker_wall_ms`/pass of 1.6-1.8ms at quantum 64/128) offsets the parallelized savings, even though real concurrency exists inside batches (`worker_cpu/worker_wall` of 4.7x at quantum 64). The encode stage is therefore not on GT2's critical path — the frame is producer-paced, consistent with H212 — and no encode-stage parallelization, however calibrated, can move GT2 fps. Parallel encoding's win condition is a workload or future where the encode stage becomes the binding constraint (heavier draw density, further producer-cost reduction, or the Metal 4 segmented lane); until then the lane's value is the preserved provider option plus the proof/measurement infrastructure. The calibration instruments now exist: typed `parallel_pass_adapter_selection_*` counters decompose certificate-valid-but-unselected candidates by `ParallelPassCandidateSelectionFailure` kind (conserving against certificate-valid minus selected), and the heavy opt-in `DXMT9_PERF_PARALLEL_CHILD_SPLIT` records per-child setup/body/end CPU with sample counts for measuring the real per-child fixed cost. The measurements were then taken (2026-08-18, merged `50718194` instruments): SFIV quantum-8/bound-64 attributes all 1,390 certificate-valid rejections to `non_positive_benefit`, and the GT2 child-split probe measures per-child setup 14.4µs + end 0.5µs ≈ 1.6 draw-equivalents at the measured 9.06µs/draw serial rate (child body ≈1.30ms ≈ the child's ~144 draws, confirming the split isolates fixed cost) plus ≈0.3ms ≈ 33 draw-equivalents of per-pass dispatch/join overhead that the model lacked entirely. The cost model is now calibrated: `kParallelPassChildSetupDrawEquivalents = 2` (rounded up, fail-closed) and the new `kParallelPassPerPassOverheadDrawEquivalents = 32` per-pass term, with provenance comments, static_assert pins, and an old-versus-new sign-flip pin (the SFIV-like 16-draw two-child shape scores +6 before and −28 after — the measured NonPositiveBenefit class). Both changes only increase cost and can only shrink the selected set. SFIV's exclusion now rests on measured constants rather than a guess. The wild re-measurement against the calibrated constants (2026-08-18, `4623465d`) confirms both predictions: GT2 selects 649 passes (statistically unchanged from 646-647 pre-calibration — large-pass margins of ~700 draw-equivalents absorb the added ~34) with `non_positive_benefit=0` and zero GPU errors, and SFIV quantum-8/bound-64 keeps 1,509/1,509 certificate-valid candidates at `non_positive_benefit` — now excluded by measured constants rather than a guess. The calibration track is closed with the lane parked as a preserved option (spec §9); `pso_first_bind` boundary economics for small-child shapes remains the one recorded follow-up and is deferred until the reopen trigger (encode-stage pacing) fires. The first step is now done: `parallel_pass_adapter_certificate_invalid_{missing_snapshot,source_identity,pass_identity,coordinator_proof,attachment_proof,resource_proof,first_draw_proof,child_capacity,child_plan,coverage,arithmetic}` typed counters exist and conserve against the aggregate, with native coverage driving three distinct checkpoints through the real adapter path; the same-day typed re-measurement (2026-08-17, `23430351`) collapses both residuals to a single checkpoint: GT2 reports 47 of 47 and SFIV quantum-8 reports 1,387 of 1,387 certificate-invalid candidates as `pass_identity`, with every other typed reason zero and zero GPU errors. Every residual certificate failure on both workloads is therefore owned by the pass-identity/epoch checkpoint — for GT2 a 1.5% edge population, for SFIV the systematic case, consistent with SFIV passes being sealed by StretchRect coordinator boundaries whose derived-epoch or pass-opening-draw requirement the current fold does not satisfy. That checkpoint is now diagnosed and both of its conditions are closed, reproduced deterministically through the real producer and the real certificate in `boundaryAdjacentPassIdentityCertifies`. **(a) Zero coordinator seed epoch — the SFIV 100%.** `encodeChunk` published `parallelPassSeedActionEpoch = options.session == nullptr ? 1u : 0u`, so every source carrying an open encode session seeded the epoch fold with the domain's invalid sentinel. `ParallelPassActionEpochWitness::valid()` refuses a zero seed, so `deriveParallelPassActionEpoch` returned `{epoch=0, valid=false}` for every candidate of that source while the producer's own fold advanced from zero and stamped later passes `1`, `2`, ...; the repro shows stamps `1`/`2` against `derived=0, derivedValid=0, witnessValid=0` and `failure=PassIdentity` on both passes of a carried SFIV-shaped source. The carried fact was already carried correctly and separately by `sourceStartsPass`, which rejects the source's first candidate with `UnsealedStart`; overloading the epoch seed with it added no safety and poisoned every later pass. The coordinator side was wrong: the seed is now the shared per-source constant `kParallelPassSeedActionEpoch = 1`, the epoch domain is scoped by (source, seqId) and never compared across sources (the session-admission epoch domain is a separate constant `1` in `CpuReadyTape::makeSealedSemanticSummary`), and `produceSealedParallelPassSnapshots` now fails a zero-seeded source closed with the typed `PassActionEpoch` reason before observing any candidate so the two sides share one seed requirement. **(b) Attachment-change sealing locator — the GT2 edge shape.** When a DrawRun's attachment key differs from the open pass's, the producer seals the pass with that DrawRun's own locator; `validateParallelPassSemanticPlan` required `parallelPassSealingKindAccepted(sealingCommand.kind)`, which is false for `DrawRun`, so the repro shows `stamp=1, derived=1, derivedValid=1, sealKind=DrawRun, sealAccepted=0, failure=PassIdentity` — the epoch fold agreed and only the sealing-kind field rejected. The producer side is right here: `parallelPassCloseReason` already consumes exactly that spelling and maps it to `EncoderSplitReason::RenderTargetChange`, so the certificate was the side that had never accounted for the non-coordinator pass end. The certificate now recognises a three-way total end proof and admits the attachment-change form only after a second, independent `deriveParallelPassActionEpoch` at `replayOrdinalEnd` proves that ordinal opens a pass of its own with a different epoch — an added obligation, not a relaxation. It is sufficient because exact coverage already requires the children to span `[replayOrdinalBegin, replayOrdinalEnd)` with every command a DrawRun carrying the pass's attachment key, so no boundary command or foreign attachment — and therefore no third pass opening — can hide inside the interval. Native pins: the SFIV shape (`Clear -> DrawRun(A) -> StretchRect -> DrawRun(B) -> Present`) certifying both the StretchRect-sealed pass and the post-boundary pass on fresh **and** carried sources; the pre-boundary pass's stamp failing `PassIdentity` on the post-boundary pass through an echoing owner; the attachment-change-sealed pass certifying, and failing `PassIdentity` under a truncated witness that can still derive its own epoch but cannot reach the sealing ordinal; a coordinator-sealed pass folding the stream exactly once; the zero-seed source failing closed with zero candidates observed; and a `static_assert` that the published seed is never the invalid sentinel. Each of the three code changes was reverted individually and produced a distinct pin failure. **The wild re-measurement now exists (2026-08-17, merged `5a52e037`)**: GT2 reports `certificate_invalid=0` with `certificate_valid=3114` of 3,114 considered, and the previously unattributed post-economics loss is gone — `parallel_pass_selected=647` equals `economics_accepted=647` exactly (3,834 child tasks), so the earlier 641-to-598 downstream loss is retroactively attributed to the same pass-identity rejections. SFIV quantum-8 reports `certificate_invalid=0` with `certificate_valid=1750` of 1,750 considered; its `selected` stays 0 because the imbalance-bound economics rejection (the known quantum-conflation issue) still owns SFIV. Both runs show every typed reason at zero and zero GPU command-buffer errors, with a visually normal GT2 screenshot. The certificate pipeline is now loss-free on both measured workloads; the remaining calibration order is imbalance-bound decoupling, then hitch-tail attribution. `identity` remains the default and no promotion claim is made. `DXMT9_PARALLEL_PASS_IMBALANCE_BOUND` (env-only, clamped `[4,4096]`, defaults to `DXMT9_PARALLEL_PASS_DRAW_QUANTUM`'s resolved value) now exists so the classifier's `UnbalancedChild` bound can be A/B'd independently of the eligibility quantum; this is only the decoupling knob itself — no imbalance-bound sweep, wild measurement, or promotion claim is made here. |
| Metal 4 segmented lane (`R-BACK-2.64`) | not started | Research and the logical-pass/session contracts identify the capability boundary. | Add capability selection and legacy fallback, bounded group gather, suspend/resume option validation, joint submission/completion, action semantics, native/integration evidence, and supported-hardware visual/locality A/B. |
| Logical-pass actions across children/segments (`R-BACK-2.48`, `R-BACK-15.17`) | source-local parallel implemented / Metal 4 open | The source-local parallel provider prepares one coordinator-owned render-pass descriptor, applies load/clear once before child emission, requires every late Store action to be resolved before effects, and completes the action ledger once after the parent joins every child. Child encoders carry no action or sidecar ownership. Native lifecycle evidence and GT2 wild counters preserve one physical pass and the serial CB/pass/tile shape. Serial EncodeSession late Store resolution (`R-BACK-15.18`) remains a fixed copied ledger that survives source boundaries and resolves once before `endEncoding`. | Define first/intermediate/last action ownership for Metal 4 suspend/resume segments. Cross-source/carried parallel passes and child-compatible sidecars remain fail-closed rather than sharing logical-pass ownership. |
| Composed end-to-end progress (`R-BACK-2.67`) | implemented / wild diagnostic exercise pending | `EncodeSchedulingProgress.tla` composes admission, lease/generation wake, publication, FIFO session continuation, deferred retirement, completion expansion/release, and Present pacing. TLC proves ownership/conservation, FIFO, sticky obligations, both lost-wakeup invariants, accepted-source release, explicit Present publish-or-skip, settlement, and terminal waiter unblocking. Source arrival makes the obligations non-vacuous; GPU settlement remains an environment action. Stage-specific terminal drains no longer fabricate submission or GPU milestones. Production tests park on the actual queue-owned CVs and invoke Initializer empty-to-nonempty, CommandQueue terminal fanout, lifecycle poison, arena abort, and the controller-owned pending-stop latch. The watchdog has 256 per-slot serialized generation identities, deterministic stale-writer reuse stress, phase/age and skip/terminal/capture/overflow attribution, and perf-off zero-clock/zero-atomic evidence. | Exercise the opt-in watchdog in a no-gputrace wild run and archive one phase-attribution example. Suspend attribution remains a deliberate gap until the unimplemented Metal 4 lane exposes a real, tested suspension disposition; the watchdog must not infer it from ordinary submission. |
| Scheduling formal verification (`R-VERIF-2.13`–`2.15`) | capacity/retirement and source-local parallel binding refinements implemented / generalized DCE and Metal 4 open | `SessionCapacityLease.tla` proves bounded lease/residency behavior; `PostEncodePayloadRetirement.tla` proves receipt retirement and completion ordering; `ParallelDrawBinding.tla` proves source-local child binding generations, ABI compatibility, serial ownership, join, parent, and completion ordering. | Production byte/block/retention/ticket dimensions remain native evidence. Generalized `R-VERIF-2.13`, cross-source parallel grouping, and Metal 4 segment-level `R-VERIF-2.15` remain open. |

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

## Producer↔queue mutex concurrency (opened 2026-08-20)

GT2 attribution (`docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.28.md`)
measured the producer losing ~1.0 ms/present acquiring `CommandQueue::mutex_`
(`mark_and_capture` 0.60 + `map_buffer` 0.42), refuted acquire-frequency
contention as the cause, and left the long holders hidden at hold-handoff
sites. Two tracks are open:

| Track | Status | Evidence debt |
|---|---|---|
| Segment-hold attribution of the handoff sites + trimming the top holder | instrumentation in flight | one GT2 profile run after the segment split lands |
| Producer concurrency redesign (T2 lock-free marking / T3 decoupling per `docs/superpowers/specs/2026-08-20-producer-queue-concurrency-design.md`) | design brief audit-resolved (`5d01a9d1`); formal layer delivered (`cfcfdac1`); **T2a' implemented**: both marking paths moved off `CommandQueue::mutex_` onto the pool's arena-stamp exception — the producer's commit-time bulk mark (`markChunkResources`, `markChunkResourcesAndCaptureBufferBindings`) and the replay worker's per-batch draw mark (`submitDrawRunBatchImpl`, the measured `0.93 ms/present` hold in `append-decomposition.29`). Model extended with a second premise and a second Buggy dimension: `WorkerStampMark`/`SlotAdvance`/`WorkerRestamp` and `RestampDiscipline`, with `WorkerAppendCoveredByStamps` as the direct obligation. Production cfg green at 276,840 distinct states / depth 29; **both** counterexample cfgs produce the expected `NoUseAfterFree` violation (`PinDiscipline="Removed"` in 3 steps, `RestampDiscipline="Removed"` in 9). `nextSeqId_` is now `std::atomic<u64>` — acquire ticket read, writes unchanged and still under the mutex. `dxmt9-producer-mark-reclaim-spec` carries both counterexample traces plus the re-stamp truth cases | evidence debt: (a) **profile re-measurement** — a `DXMT9_PERF_QUEUE_MUTEX_SPLIT` GT2 run confirming the `submit_draw_run_batch_impl/mark` segment row is gone from the hold ledger and that `mark_chunk_resources_and_capture_buffer_bindings` acquire-wait dropped, without a new row appearing elsewhere; (b) **wild matched pair** (GT2 primary, plus GT1/GT3/SFIV) for the fps claim and visual/`gpu_command_buffer_errors` gates; (c) the deterministic interleaving harness for the C++ atomics ordering (R-VERIF-7.3 direction) remains open — the model explicitly does not cover release/acquire pairing or torn reads; (d) no counter observes how often the re-stamp actually fires in the wild, so the size of the ticket/slot-seq window is modelled but unmeasured. `CommandQueue::submitDrawRun` (the non-batch path) deliberately keeps its mark under the mutex — strictly conservative, and not one of the two measured holds |

The pin-ordering premise (retainer pins make `destroyPending` impossible
during marking) was, before T2a', enforced by the shared mutex rather than by
the pins; promoting any lock-free mark without the model plus the
counterexample-capable Buggy cfg is prohibited — `ResourceLifetime.tla`'s
watermark-only scope has already missed one refcount-class escape
(`specs/verification/gap.md`, 2026-08-02).

T2a' added a **second** premise that the mutex had also been supplying for
free, and which the original model could not see: the stamp's seq ticket and
the seq its records finally land under were the same read only because the
ticket was taken inside the hold that ended at the append. Outside it, a
concurrent force-publish can move the seq between the two, leaving stamps below
the chunk's final seq — a premature reclaim, not a stale-pointer dereference,
so `PinDiscipline` alone would never have caught it. The production protocol is
a frozen-ticket re-read under the mutex plus a conditional re-stamp
(`restampIfTicketAdvancedLocked`), which generalizes the pre-existing
`forceDrawResourceMarkingAfterSplit_` flag. Any future caller that takes the
arena-stamp exception owes both premises; the pool header states them and each
has its own `.counterexample.cfg` executed by `scripts/check/verify_tla.sh`.

## Historical Verdict

The removed carrier experiments demonstrated that overlap alone is insufficient:
waiting can move into writer/drain pressure, and source-grain publication can
increase command buffers, logical pass reopens, and tile cost. The current
roadmap therefore treats source storage, logical-pass ownership, and Metal
execution lanes as separate contracts and applies `R-BACK-2.50` before any
default promotion.

| Render Tape identity v1/v2 and SegmentSerial (`R-BACK-2.40`, `R-BACK-2.49`, `R-BACK-2.60`, `R-BACK-2.66`, `R-BACK-2.76`–`2.81`) | ⚠️ v1 retired/rejected; bounded SegmentSerial admission/completion refinement and native bindings present; production/GT2 promotion open | `identity.bin` v1 is explicitly rejected and never reinterpreted. The v2 EventSerial one-source-per-event path remains the compatibility/default identity lane and the complete-event fallback. `RenderTapeIdentitySegments.tla`/`.cfg` proves bounded exact segment partition, pass-piece continuity, atomic publication/two-phase abort, reverse-tail reclaim, flattened completion, exact same-event/tail settlement, shared greatest-dependent watermarks, and pre-effect fallback/post-effect fail-stop. Native tape and capture-ledger tests bind the production detach/finish and full-run settlement predicates. The GT2 r65/r66 capture-authority runs prove surviving v2 sidecars and exact v2 event/record/pass coverage; they do not prove unbounded provider grammar, Metal segment completion, or visual/locality promotion. | Keep binding production callsites and add the non-vacuous GT2 identity-v2 EventSerial-vs-SegmentSerial pair with zero GPU errors and equal output/record identity. Keep SegmentSerial and the Tape lane default-off until R-BACK-2.50 and cross-workload visual/locality/no-gputrace gates pass; do not revive v1. |
