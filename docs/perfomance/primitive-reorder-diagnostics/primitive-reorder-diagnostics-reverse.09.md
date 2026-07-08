---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 09
title: Reverse Material-Class Probe Tooling
date: 2026-06-02
type: tooling
status: tooling
source: specs/perfomance.plan.md#L10643-L11009
---

# Reverse Material-Class Probe Tooling

**Question / hypothesis.** Single-row reverse moves a row's whole material set.
Add a material/state-class filter so a reverse probe can keep both the encoder
row *and* the material bucket stable, isolating per-class hidden-write movement.

**Method.** Added `DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASS` (values
`any|opaque-depth-write|nonopaque|depth-read|alpha-blend|scissor|textured|large4096`,
same `IndexedTriangleClassFilter` parser as split-large draws) and the AND-list
`DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASSES` (comma/semicolon/space/`+`/`&`
separated, for intersections like `large4096 && alpha-blend`). Launcher flags
`--probe-reverse-indexed-triangles-class` / `--probe-reverse-indexed-triangles-classes`.
The class gate composes with the full/opaque/nonopaque switch and the row
selector. Default `any`.

**Result.** Build/test pass (`dxmt9-draw-seq-filter-spec`, runtime + winemetal
compile, `git diff --check`). No-gputrace smoke proved runtime-applied scope:
`60/4 alpha-blend` -> 276 probe draws (matches alpha bucket), 25 skipped;
`large4096 && alpha-blend` -> 16/237 draws (`534,258B`); `large4096 && scissor`
-> 4/249 draws (`127,656B`). The 4 `large4096 && alpha-blend && scissor` draws
were identified as **screen-blend** (`InvDestColor + One + Add`, depth-write off,
depth test `LessEqual`), which is per-channel commutative — making a
correctness-preserving reorder candidate plausible (precedent:
`shouldAutoExpandIndexedDraw()` treats the same blend family specially).

**Verdict.** Tooling. Enables the narrowed class experiments
([primitive-reorder-diagnostics-reverse.10](primitive-reorder-diagnostics-reverse.10.md) alpha, [primitive-reorder-diagnostics-reverse.11](primitive-reorder-diagnostics-reverse.11.md)
large4096, [primitive-reorder-diagnostics-reverse.12](primitive-reorder-diagnostics-reverse.12.md) large4096+alpha,
[primitive-reorder-diagnostics-reverse.13](primitive-reorder-diagnostics-reverse.13.md) large4096+alpha+scissor,
[primitive-reorder-diagnostics-reverse.14](primitive-reorder-diagnostics-reverse.14.md) opaque-large). The screen-blend
safety observation directly seeded the semantic-safe optimization path in
[index-cache-locality](../index-cache-locality/index.md).

**Related.** [primitive-reorder-diagnostics](index.md) · prev: [primitive-reorder-diagnostics-reverse.08](primitive-reorder-diagnostics-reverse.08.md)
· sibling tooling: [primitive-reorder-diagnostics-reverse.04](primitive-reorder-diagnostics-reverse.04.md) · [index-cache-locality](../index-cache-locality/index.md)
· [index-reuse-measurement](../index-reuse-measurement/index.md) (shared class buckets).
