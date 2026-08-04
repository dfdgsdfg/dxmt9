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
| CPU-ready publication boundary (`R-BACK-2.35`–`2.41`) | partial | Commit-replay offload creates producer/replay overlap and H229 can attach consecutive ready sources. Source/`seqId` boundaries are already treated as semantic non-boundaries in the session path. | Publication is still coupled to scarce slot residency. No bounded source-tape owner, independent admission watermark, occupancy counters, or promotion evidence exists. |
| CPU-ready source residency/admission (`R-BACK-2.60`) | not started | Queue slots and retained source views provide the existing ownership primitives. | Implement bounded storage independent of GPU-reclaimed slots; define admission/back-pressure counters and queue-observer evidence. |
| Scoped FIFO replay drain (`R-BACK-2.51(d)`, `R-BACK-2.61`) | not started | Direct device calls currently perform the safe full deferred-replay drain; raw entries are immutable and FIFO. | Add complete canonical access summaries, conflict-prefix selection, distinct replay/publication watermarks, conservative full-drain fallback, and deterministic tests. |
| EncodeSession semantic lifecycle (`R-BACK-2.42`–`2.49`) | partial / opt-in | H229 `DXMT9_OPEN_CB_CARRIER` carries `EncodeChunkSessionState` and one command buffer across compatible FIFO sources, preserves ordered completion, splits the final Present tail, and has native carrier, completion-source, and end-to-end lifecycle specs. `EncodeSessionCompletion.tla` covers ordered source completion. | The lane remains default-off and lacks a source-residency design that avoids back-pressure displacement plus the required visual/locality/performance promotion evidence. The earlier H86–H189 carrier family was removed after locality and P4 gates failed; see `docs/perfomance/present-pacing/index.md` and `log.md`. |
| Promotion gates (`R-BACK-2.50`) | partial | Existing perf counters expose completion-wait, queue-writer, drain, sequence, command-buffer, pass, load/store, and tile shape. Prior experiments identify wait displacement and locality regressions. | Add source-admission/session occupancy counters and enforce the expanded wait-conservation gate in comparative tooling and wild evidence. |
| Published source immutability (`R-BACK-2.46`, `R-BACK-2.59`) | partial | Published slots are consumed by const reference; only lifecycle, encode-worker prefetch memo, and reclaim clearing are intended mutations. | Add a type-level seal or audit guard and remove the read-only `const_cast` in `makeEncodePartitionReplayStream`. |
| Serial partition consumer (`R-BACK-2.57`, `R-BACK-2.58`) | implemented for serial identity and explicit native plans | All backend streams use the range interface. Complete preflight, identity fail-open, command-once behavior, and multi-subrange end-to-end lifecycle are covered by partition and session native specs. | Explicit multi-subrange Metal execution and wild callers remain unexercised; current production callers use identity ranges. |
| Production partition planner (`R-BACK-2.62`) | not started | Immutable entry/range snapshots, validation, identity cursor, and serial consumption are ready. | Implement deterministic subdivision, cost model, explicit-plan counters and rejection reasons, then prove wild explicit-plan use without changing Metal shape. |
| Parallel render-pass executor (`R-BACK-2.63`) | not started | Partition locators and pass/session ownership contracts provide prerequisites. | Add first-draw entry snapshots, partition-local native shadows, child ordering/join/failure policy, fake-child tests, model evidence, and measured large-pass eligibility before adopting `MTLParallelRenderCommandEncoder`. |
| Metal 4 segmented lane (`R-BACK-2.64`) | not started | Research and the logical-pass/session contracts identify the capability boundary. | Add capability selection and legacy fallback, bounded group gather, suspend/resume option validation, joint submission/completion, action semantics, native/integration evidence, and supported-hardware visual/locality A/B. |
| Logical-pass actions across children/segments (`R-BACK-2.48`, `R-BACK-15.17`) | not started | Current action policy is implemented for ordinary serial render passes. | Extend policy and tests so load/clear occurs once at logical start, store/resolve and sidecars once at logical end, and counters do not double-count physical segments. |
| Scheduling formal verification (`R-VERIF-2.13`–`2.15`) | not started | Existing `DceChunkLookahead.tla` models one already-ready successor; `EncodeSessionCompletion.tla` models one tail expanding ordered source completion. | Generalize bounded ready-prefix proof, add CPU-ready/session admission progress, and prove ordered parallel/joint completion. No new TLA module exists yet. |

## Historical Verdict

The removed carrier experiments demonstrated that overlap alone is insufficient:
waiting can move into writer/drain pressure, and source-grain publication can
increase command buffers, logical pass reopens, and tile cost. The current
roadmap therefore treats source storage, logical-pass ownership, and Metal
execution lanes as separate contracts and applies `R-BACK-2.50` before any
default promotion.
