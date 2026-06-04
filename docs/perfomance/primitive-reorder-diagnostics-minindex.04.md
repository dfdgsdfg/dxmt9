---
domain: primitive-reorder-diagnostics
subcategory: minindex
order: 04
title: Screen-Blend Min-Index Full-Frame Rerun
date: undated
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L14710-L14782
---

# Screen-Blend Min-Index Full-Frame Rerun

**Question / hypothesis.** Run min-index ordering on a *geometry-locked* 16-draw
depth-input replay that preserves draw count, submitted vertex count, primitive
count, FS work, per-draw scissor state, and raw D24X8 depth input. With geometry
held fixed, does `sort-min-index` produce a clean vertex-stage win, or is the
earlier full-frame min-index drop only a backend address/locality classifier?

**Method.** `run_3dmark05_mini_replay.py
<screen-blend-run-71-188-payload16-streams-r1 manifest>
--primitive-order sort-min-index --draw-order original --depth-input
<frame60-2-depth.bin raw D24X8> --run`, exported through the standard Xcode
counter path and compared against the original-order full16 depth-input replay.
Software index-cache (LRU) estimate ran first as a pre-gate.

**Result.** Software LRU pre-gate already warned: LRU16
`56,991 → 60,628` (`+6.38%`), LRU32 `54,697 → 58,625` (`+7.18%`), LRU64
`52,784 → 56,695` (`+7.41%`). Xcode confirmed: GPU `1.082 → 1.042ms`
(`-3.78%`); vertices/primitives/FS invocations all `0.00%`; VS invocations
`54,104 → 57,839` (`+6.90%`); VS buffer write `31.987 → 33.100MiB` (`+3.48%`);
VS bytes/inv `619.933 → 600.085B` (`-3.20%`); VS-buffer-write limiter
`10.77 → 12.96%` (`+20.33%`). VS-write delta is invocation-count driven
(`+2.173MiB` invocation effect, `-1.059MiB` bytes/inv effect, net `+1.113MiB`).

**Verdict.** Rejected. Under a geometry-locked slice, naive per-draw
`sort-min-index` increases LRU misses, VS invocations, and VS buffer write
together; the small bytes/invocation density drop is outweighed by invocation
growth. The earlier full-frame min-index r2 drop is therefore only a backend
address/locality classifier with drift caveats, not a production optimization.

**Related.** [[primitive-reorder-diagnostics]] · prior:
[[primitive-reorder-diagnostics-minindex.03]] · next:
[[primitive-reorder-diagnostics-minindex.05]] (the full-frame r2 it reclassifies)
· [[mini-replay-bisection]] (geometry-locked depth-input replay harness) ·
[[index-cache-locality]] (semantic-safe cache-aware successor) ·
[[index-reuse-measurement]] (LRU miss model) · [[hidden-backend-storage]].
