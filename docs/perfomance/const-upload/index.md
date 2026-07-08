---
domain: const-upload
workload: 3DMark05 GT1
title: "Const-Upload — CPU-side constant-buffer (cbuf/argbuf) upload traffic"
type: domain-index
status: current
updated: 2026-07-08
source: docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/const-upload/overview.md; docs/perfomance/const-upload/log.md
---

# Const-Upload — CPU-side constant-buffer (cbuf/argbuf) upload traffic

Latest tracked row: `H9` - Hash-based downstream cbuf slice reuse cuts the bucket (inconclusive (~`0.5%`; target is upstream record coalescing)).

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [const-upload-sparse.02 - Sparse Const Split Xcode Validation](const-upload-sparse.02.md)
- [const-upload-dirtyrange.02 - Dirty Range Reset Xcode Frame Capture](const-upload-dirtyrange.02.md)
- [const-upload-volatility.01 - Cbuf Field Volatility Run](const-upload-volatility.01.md)
- [const-upload-sparse.01 - Sparse Const Split Run-Level Probe](const-upload-sparse.01.md)
- [const-upload-slice.01 - FFP VS Stable Slice Reuse Run](const-upload-slice.01.md)
- [const-upload-range.01 - VS Float Range Run](const-upload-range.01.md)
- [const-upload-dirtyrange.01 - Dirty Range Reset Run](const-upload-dirtyrange.01.md)
- [const-upload-class.01 - Cbuf Class Breakdown Run](const-upload-class.01.md)
- [const-upload-cache.01 - Cbuf Slice Cache Experiment](const-upload-cache.01.md)
