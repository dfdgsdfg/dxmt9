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
| H5 | Resetting dirty-range counters with the dirty bit cuts VS cbuf | accepted as CPU win (`-66.48%` argbuf, `4.6GB`→~`1.06GB`); GPU unmoved | [const-upload-dirtyrange.01](const-upload-dirtyrange.01.md) |
| H6 | The post-fix top-pass GPU cost is still cbuf upload | rejected (cbuf down to `163KiB`/encoder; cost is memory-write/store) | [const-upload-dirtyrange.02](const-upload-dirtyrange.02.md) |
| H7 | Splitting sparse const records cuts payload without inflating count | accepted as CPU mechanism (`-30.92%` bytes, `+0.13%` count) | [const-upload-sparse.01](const-upload-sparse.01.md) |
| H8 | Sparse-const split moves the Xcode GPU bottleneck | rejected (VS write `1627.4→1627.3MiB` unchanged) | [const-upload-sparse.02](const-upload-sparse.02.md) |
| H9 | Hash-based downstream cbuf slice reuse cuts the bucket | inconclusive (~`0.5%`; target is upstream record coalescing) | [const-upload-cache.01](const-upload-cache.01.md) |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

> 8 of the 8 leaves listed below are marked `outdated:` and open with a banner naming the ground. They are history, not re-checkable evidence.

- [const-upload-sparse.02 - Sparse Const Split Xcode Validation](const-upload-sparse.02.md)
- [const-upload-dirtyrange.02 - Dirty Range Reset Xcode Frame Capture](const-upload-dirtyrange.02.md)
- [const-upload-volatility.01 - Cbuf Field Volatility Run](const-upload-volatility.01.md)
- [const-upload-sparse.01 - Sparse Const Split Run-Level Probe](const-upload-sparse.01.md)
- [const-upload-slice.01 - FFP VS Stable Slice Reuse Run](const-upload-slice.01.md)
- [const-upload-range.01 - VS Float Range Run](const-upload-range.01.md)
- [const-upload-dirtyrange.01 - Dirty Range Reset Run](const-upload-dirtyrange.01.md)
- [const-upload-class.01 - Cbuf Class Breakdown Run](const-upload-class.01.md)
