---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 04
title: Row-Scoped Reverse Probe Tooling
date: 2026-06-02
type: tooling
status: tooling
outdated: retired-journal
source: specs/perfomance.plan.md#L9664-L9790
---

# Row-Scoped Reverse Probe Tooling

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** The broad reverse subsets all drift frame shape. Add
tooling to constrain a reverse probe to one `RenderPass[seq=...,enc=...]` row or
a row set, plus mandatory shape gates, so a gputrace A/B can ask whether one
specific hot row's primitive order moves the hidden VS-write bucket.

**Method.** Added env vars `DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROW=<seq>/<enc>`
and `DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROWS=<rows>` (comma/semicolon/space
list, treated as a union), plus launcher flags
`--probe-reverse-indexed-triangles-row SEQ/ENC` /
`--probe-reverse-indexed-triangles-rows ROWS`. Selector applies after the
full/opaque switch, before building the transient reordered IB; non-target rows
do not count as `indexed_order_probe_skipped`. `ActiveEncoderBreakdown::begin`
now stores `seqId`/`encoderIndex` even when breakdown emission is off.
Also added the **frame-shape gates** now mandatory for all order/visibility
probes: `--require-top-row-key-match --max-top-draw-call-delta-ratio 0.05
--max-top-vertex-count-delta-ratio 0.05 --max-top-triangle-delta-ratio 0.05`.

**Result.** Build/test pass (`dxmt9-draw-seq-filter-spec`, `meson compile`,
`git diff --check`, `bash -n`). No-gputrace selector validation hit exactly the
target row (e.g. `60/3` -> 169 probe draws / `1,525,050B`; `60/4` -> 277 / `2.28MiB`).
Re-running the prior broad reverse exports through the new gates **rejected all
three**: full reverse (top rows change, draws `-14.49%`), opaque (draws `711->753`,
`+5.91%`), nonopaque (draws `711->776`, `+9.14%`).

**Verdict.** Tooling + gate policy. Establishes that no-gputrace row probes are
not yet clean (encoder/draw counts still drift on a different time-based GT1
frame). The shape gates are what made every later single-row / class-scoped
reverse result interpretable.

**Related.** [primitive-reorder-diagnostics](index.md) · prev: [primitive-reorder-diagnostics-reverse.03](primitive-reorder-diagnostics-reverse.03.md)
· feeds: [primitive-reorder-diagnostics-reverse.05](primitive-reorder-diagnostics-reverse.05.md), [primitive-reorder-diagnostics-reverse.06](primitive-reorder-diagnostics-reverse.06.md),
[primitive-reorder-diagnostics-reverse.07](primitive-reorder-diagnostics-reverse.07.md), [primitive-reorder-diagnostics-reverse.08](primitive-reorder-diagnostics-reverse.08.md)
· [mini-replay-bisection](../mini-replay-bisection/index.md) (gate discipline) · sibling tooling: [primitive-reorder-diagnostics-reverse.09](primitive-reorder-diagnostics-reverse.09.md).
