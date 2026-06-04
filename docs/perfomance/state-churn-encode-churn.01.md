---
domain: state-churn-encode
subcategory: churn
order: 01
title: Stream/IB State Churn (hypothesis)
date: undated
type: conceptual-model
status: model
source: specs/perfomance.plan.md#L3987-L4071
---

# Stream/IB State Churn (hypothesis)

**Question / hypothesis.** The draw-run scanner fails to batch because each draw
carries stream/IB deltas. Hypothesis: handle churn (not offset/stride) dominates,
so the fix is to carry per-draw stream/IB bindings *inside* a draw-run via a
`DrawBindingOverride` payload rather than break the run on every change.

**Method.** Design + instrumentation. PE-side `buildDrawPrimitivePacket()` copies
`pendingStreamMask` into each draw packet, serializing stream buffer/offset/stride
per pending stream, plus an IB delta when `pendingIb` is set. The importer scanner
can take the first stateful draw as the run base and carry later stream/IB changes
as `DrawBindingOverride` payloads. New counters:
`commit_chunk_draw_run_binding_override_{records,bytes,stream_records,ib_records}`.

**Result.** Historical pre-redesign traces: `commit_chunk_draw_run_submits=580`,
`commit_chunk_draw_run_break_type_const_upload=659938`,
`commit_chunk_draw_run_break_state_delta=232121`, with stream/IB deltas leading
the state-delta counters (`81.5%` IB / `81.9%` stream handle churn from the
breakdown runs). The design makes per-draw stream/IB bindings representable.

**Verdict.** Model. Establishes the binding-override mechanism and the prediction
that converting stream/IB state-delta breaks into larger draw-runs is possible
without moving GPU cost — to be validated downstream.

**Related.** [[state-churn-encode]] · validated by [[state-churn-encode-binding.01]] ·
[[state-churn-encode-stream.01]] (handle-churn measurement) ·
[[state-churn-encode-drawrun.01]] (the failure shape it targets) ·
[[const-upload]] (const-upload remains the larger separate break class).
