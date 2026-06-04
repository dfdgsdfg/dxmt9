---
domain: state-churn-encode
subcategory: drawrun
order: 02
title: Const-Upload Boundary Semantics
date: undated
type: conceptual-model
status: model
source: specs/perfomance.plan.md#L3821-L3857
---

# Const-Upload Boundary Semantics

**Question / hypothesis.** Why can't a draw-run simply cross a constant-upload
record between two draws? Establish the correctness boundary and the safe
fallback that preserves per-draw uniforms.

**Method.** Scanner + replay semantics. The importer scanner reports a constant
upload with an explicit `ImportedDrawRunScanStop::ConstantUpload` stop reason
(no longer folded into generic `DifferentRecordType`).

**Result.** A constant upload between two draws cannot be blindly crossed by the
current single-uniform `drawPrimitiveRun()` representation: it owns one
`DrawUniformPayload`, so Draw A (pre-upload) and Draw B (post-upload) would share
one uniform snapshot. The safe path is the pending `submitDrawRunBatch()`
fallback, which may pass through constant-upload records but snapshots uniforms
*per draw* — Draw A keeps pre-upload constants, Draw B uses post-upload constants.
Pinned by a native regression test plus the scanner-boundary test.

**Verdict.** Model. This is instrumentation/attribution + a correctness invariant,
not a perf win. The next batching step requires either per-draw uniform-payload
draw-runs or upstream const-coalescing proof.

**Related.** [[state-churn-encode]] · prev: [[state-churn-encode-drawrun.01]] ·
[[const-upload]] (const-upload coalescing is the dependency for crossing this
boundary) · [[state-churn-encode-statedelta.03]] (stream+IB payload solves a
different, separate break class).
