---
domain: hidden-backend-storage
workload: 3DMark05 GT1
subcategory: shape
order: 01
title: Hidden Vertex Backend Shape (probe agenda)
date: undated
type: conceptual-model
status: model
source: specs/perfomance.plan.md#L8851-L8950
---

# Hidden Vertex Backend Shape (probe agenda)

**Question / hypothesis.** Once broad state toggles and visible-VSOut width are
all rejected as the sole owner of the hidden `~1.6GiB` top-three VS-buffer-write
bucket, what classes of probe are left to attack it directly? This is the agenda
that motivated the classifier / reorder / cache-locality work.

**Method.** Conceptual decision after the completed reject set. Rejected as sole
owners: broad alpha-blend, depth-write, depth-compare-only
(`--probe-depth-func-always`, top-three VS write unchanged), alpha-test,
scissor, cull, fog, texture sampling, source-visible VSOut liveness/width
(`DXMT9_TRIM_UNUSED_VARYINGS=1`), point-size-only output, direct texcoord
access, split-large indexed draws, and generic submission-batch structure.
Texture sampling, fog, and depth compare are secondary GPU costs; submission
batching is useful CPU work. Visible `VSOut` shrank to `36B`/`52B`/`16B` while
Xcode still reported `1150-1603B/VS invocation`.

**Result.** The remaining useful probe tracks, preserving normal geometry when
possible:

1. **Primitive/backend state-shape A/B** — small correctness-scoped
   experiments around the `60/2` shape (back-cull, scissor, alpha blend,
   depth-write-off, texture-source use, large indexed primitive pressure). The
   question is whether a *legal combination* changes VS invocations or
   bytes-per-invocation without destroying the frame.
2. **Compiler/backend stage-output inspection** — compare top MSL/AIR rows,
   generated pipeline descriptors, and Xcode vertex-stage counters.
3. **CPU/backend batching track** — kept separate from GPU FPS root cause;
   backend groups average only `1.879` records/group. A CPU encode/submit
   target, not evidence for the hidden GPU VS-write owner.

**Verdict.** model (agenda). After visible shape was rejected, the only strong
VS-write movers were destructive expansion or shape-drifting primitive/order
locality. The next useful fix must preserve row keys and geometry while
reproducing primitive/backend-locality movement, or isolate it in a row-local
replay. This agenda directly seeds the classifier, reorder-diagnostic, mini-
replay, and the accepted opaque-depth index-cache-locality work.

**Related.** [[hidden-backend-storage-scaling.02]] · [[hidden-backend-storage]] ·
[[backend-shape-classifiers]] · [[primitive-reorder-diagnostics]] ·
[[index-cache-locality]] · [[mini-replay-bisection]] · [[tvb-mechanism-proof]] ·
[[vsout-layout]] · [[overview-3dmark05-gt1]]
