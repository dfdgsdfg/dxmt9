---
type: "Spec Gap"
title: "Encode Scheduling Gap"
description: "Implementation and evidence gaps for CPU-ready, EncodeSession, partition, and Metal execution scheduling."
tags: [specs, backend, encode-scheduling, gap]
---

# Encode Scheduling Gap

This is the detailed owner for `R-BACK-2.35`–`R-BACK-2.50` and
`R-BACK-2.57`–`R-BACK-2.64`. The parent [backend gap](../gap.md) keeps only a
routing summary. Historical experiments remain in `docs/perfomance/`.

| Area | Status | Current evidence | Missing implementation or evidence |
|---|---|---|---|
| CPU-ready publication boundary (`R-BACK-2.35`–`2.41`) | design complete / implementation partial | Commit-replay offload creates producer/replay overlap and H229 can attach consecutive ready sources. Source/`seqId` boundaries are already semantic non-boundaries. The concrete design now assigns order-isomorphic `sourceOrdinal`/`seqId` identities and atomically publishes a directly-built sealed payload block. | Current publication is still coupled to payload-owning `ChunkSlot` residency. Implement the control-shell/block split and prove publication/state counters and overlap promotion. |
| CPU-ready source residency/admission (`R-BACK-2.60`, `R-VERIF-2.14`) | designed / not started | The spec selects a fixed source ring plus preallocated circular page arena, non-wrapping direct-build `SourcePayloadBlock`, generation-checked IDs, all-or-nothing reserve/seal, per-dimension high/low hysteresis, replay-worker-only admission wait, finish-owned `Completed -> Reclaiming -> Reclaimed`, and ordered pressure-release events. | Implement the tape and legacy no-copy oversize bypass; add pure layout/ABA tests, fake-actor admission/reclaim/shutdown tests, counters, and the `CpuReadySessionProgress` model. |
| Scoped FIFO replay drain (`R-BACK-2.51(d)`, `R-BACK-2.61`) | not started | Direct device calls currently perform the safe full deferred-replay drain; raw entries are immutable and FIFO. | Add complete canonical access summaries, conflict-prefix selection, distinct replay/publication watermarks, conservative full-drain fallback, and deterministic tests. |
| EncodeSession semantic lifecycle (`R-BACK-2.42`–`2.49`) | designed / partial opt-in refinement | H229 `DXMT9_OPEN_CB_CARRIER` already supplies `EncodeChunkSessionState`, session-aware serial encode, `finalizeEncodeChunkSessionIntoSubmission`, ordered completion sources, and final Present-tail ownership. The target spec separates session admission from active-render continuation, moves only a maximal compatible ready prefix to `Represented`, and permits open-session parking until an ordered D3D-visible or queue-progress event requires release. | Reuse H229 lifecycle/finalizer/completion and parked-session seams, but replace payload-owning slot residency and completion-wait/worker-timing release policy with tape locators, compatibility summaries, deterministic caps, ordered release-event fences, and pre-effect newly represented batch rollback. Extend native lifecycle and completion/TLA evidence; then satisfy visual/locality/performance promotion gates. Historical H86–H189 evidence remains in `docs/perfomance/present-pacing/index.md` and `log.md`. |
| Promotion gates (`R-BACK-2.50`) | partial | Existing perf counters expose completion-wait, queue-writer, drain, sequence, command-buffer, pass, load/store, and tile shape. Prior experiments identify wait displacement and locality regressions. | Add source-admission/session occupancy counters and enforce the expanded wait-conservation gate in comparative tooling and wild evidence. |
| Published source immutability (`R-BACK-2.46`, `R-BACK-2.59`) | partial | Published slots are consumed by const reference; only lifecycle, encode-worker prefetch memo, and reclaim clearing are intended mutations. | Add a type-level seal or audit guard and remove the read-only `const_cast` in `makeEncodePartitionReplayStream`. |
| Serial partition consumer (`R-BACK-2.57`, `R-BACK-2.58`) | implemented for serial identity and explicit native plans | All backend streams use the range interface. Complete preflight, identity fail-open, command-once behavior, and multi-subrange end-to-end lifecycle are covered by partition and session native specs. | Explicit multi-subrange Metal execution and wild callers remain unexercised; current production callers use identity ranges. |
| Production partition planner (`R-BACK-2.62`) | not started | Immutable entry/range snapshots, validation, identity cursor, and serial consumption are ready. | Implement deterministic subdivision, cost model, explicit-plan counters and rejection reasons, then prove wild explicit-plan use without changing Metal shape. |
| Parallel render-pass executor (`R-BACK-2.63`) | not started | Partition locators and pass/session ownership contracts provide prerequisites. | Add first-draw entry snapshots, partition-local native shadows, child ordering/join/failure policy, fake-child tests, model evidence, and measured large-pass eligibility before adopting `MTLParallelRenderCommandEncoder`. |
| Metal 4 segmented lane (`R-BACK-2.64`) | not started | Research and the logical-pass/session contracts identify the capability boundary. | Add capability selection and legacy fallback, bounded group gather, suspend/resume option validation, joint submission/completion, action semantics, native/integration evidence, and supported-hardware visual/locality A/B. |
| Logical-pass actions across children/segments (`R-BACK-2.48`, `R-BACK-15.17`) | not started | Current action policy is implemented for ordinary serial render passes. | Extend policy and tests so load/clear occurs once at logical start, store/resolve and sidecars once at logical end, and counters do not double-count physical segments. |
| Scheduling formal verification (`R-VERIF-2.13`–`2.15`) | requirements complete / models not started | Existing `DceChunkLookahead.tla` models one already-ready successor; `EncodeSessionCompletion.tla` models one tail expanding ordered source completion. `R-VERIF-2.14` now names tape generations, page reservation/reuse, `Reclaiming`, watermark pressure, suffix-stays-Ready behavior, ordered release-event fences, non-present submission, and shutdown/device-loss progress obligations. | Generalize bounded ready-prefix proof, add `CpuReadySessionProgress`, extend completion refinement with tape pins/reclaim, and prove ordered parallel/joint completion. No new TLA module exists yet. |

## Historical Verdict

The removed carrier experiments demonstrated that overlap alone is insufficient:
waiting can move into writer/drain pressure, and source-grain publication can
increase command buffers, logical pass reopens, and tile cost. The current
roadmap therefore treats source storage, logical-pass ownership, and Metal
execution lanes as separate contracts and applies `R-BACK-2.50` before any
default promotion.
