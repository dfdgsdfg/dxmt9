---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: split
order: 04
title: Global Split-Large Indexed Draw Probe
date: undated
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L6725-L6791
---

# Global Split-Large Indexed Draw Probe

**Question / hypothesis.** Is the `~1.6GiB` top-frame Xcode VS Buffer Device
Memory Bytes Written bucket caused primarily by very large indexed
triangle-list submissions? If so, splitting large indexed draws frame-wide into
smaller Metal draws should reduce per-draw backend/tiler pressure or VS-write
density. This is the unscoped (global) form, before the row/class selectors were
added.

**Method.** `DXMT9_SPLIT_LARGE_INDEXED_DRAWS=4096` as an opt-in GPU-side
pressure probe (suffix `split-large-indexed-4096-gputrace-r3`), with the
standard Xcode replay/export and `frame60-xcode-dxmt-comparison.md` finalizer.

**Result.** Split executed in the hot frame across the top-3 encoders:
`34 / 98 / 64` source/Metal/extra draws, `330,240` split primitives
(`seq=60 enc=2`: 20→+40, `206,348` prims; `enc=1`: 9→+14, `72,305`; `enc=0`:
5→+10, `51,587`). Top GPU `34.737 → 34.206ms` (`-1.53%`); top VS buffer write
`1627.395 → 1630.471MiB` (`+0.19%`, worse). Draw/state shape otherwise
unchanged (`385` draws, `2,146,185` vertices, `715,395` triangles). Run-level:
draws `+0.24%`, tile preservation `+0.67%`, same-key preservation `+2.87%`.

**Verdict.** Rejected as a bottleneck-removal candidate. Splitting large indexed
draws is useful only as a diagnostic; the dominant VS buffer-write traffic is
not explained by one draw exceeding a primitive threshold, and the split makes
the bucket slightly worse while adding draw-count/preservation cost. The next
GPU candidate must reduce vertex-stage/backend write pressure without increasing
draw count or render-pass preservation traffic.

**Related.** [[primitive-reorder-diagnostics]] · prior:
[[primitive-reorder-diagnostics-split.03]] (scoped successors of this probe) ·
[[hidden-backend-storage]] (the ~1.6GiB hidden bucket this fails to move) ·
[[index-cache-locality]] (semantic-safe successor) ·
[[state-churn-encode]] (draw-count/preservation side effects).
