---
domain: const-upload
workload: 3DMark05 GT1
title: "Const-Upload — CPU-side constant-buffer (cbuf/argbuf) upload traffic - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/const-upload/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/const-upload/index.md; docs/perfomance/const-upload/log.md
---

# Const-Upload — CPU-side constant-buffer (cbuf/argbuf) upload traffic - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `const-upload.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the multi-GB CPU-side constant-buffer write traffic dxmt9
emitted per GT1 frame — the argbuf/transient cbuf (VS / FFP-VS / PS / FFP-PS)
uploads. It asks: where do those bytes go, which fields are actually volatile,
and can the traffic be cut? The answer is a sequence of attributions and
reductions that cut cbuf/transient CPU bytes massively (FFP-VS slice reuse,
dirty-range reset, sparse-record split) but did **not** move the GPU frame
bottleneck — proving cbuf upload is a **CPU amplifier**, not the GPU limiter.

## Latest Conclusions

> **Every row below cites a leaf now marked `outdated: retired-journal`.** The
> byte and percentage figures in this table are last measurements, not
> re-checkable ones; they are kept because they record which cbuf/argbuf lanes
> were already tried.

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H5 | Resetting dirty-range counters with the dirty bit cuts VS cbuf | accepted as CPU win (`-66.48%` argbuf, `4.6GB`→~`1.06GB`); GPU unmoved | const-upload-dirtyrange.01 |
| H6 | The post-fix top-pass GPU cost is still cbuf upload | rejected (cbuf down to `163KiB`/encoder; cost is memory-write/store) | const-upload-dirtyrange.02 |
| H7 | Splitting sparse const records cuts payload without inflating count | accepted as CPU mechanism (`-30.92%` bytes, `+0.13%` count) | const-upload-sparse.01 |
| H8 | Sparse-const split moves the Xcode GPU bottleneck | rejected (VS write `1627.4→1627.3MiB` unchanged) | const-upload-sparse.02 |
| H9 | Hash-based downstream cbuf slice reuse cuts the bucket | inconclusive (~`0.5%`; target is upstream record coalescing) | const-upload-cache.01 |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

> 8 of the 8 leaves listed below are marked `outdated:` and open with a banner naming the ground. They are history, not re-checkable evidence.
