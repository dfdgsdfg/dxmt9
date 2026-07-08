---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
title: "Primitive-Reorder Diagnostics — does index/primitive ORDER own the hidden VS-write bucket? - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/primitive-reorder-diagnostics/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/primitive-reorder-diagnostics/index.md; docs/perfomance/primitive-reorder-diagnostics/log.md
---

# Primitive-Reorder Diagnostics — does index/primitive ORDER own the hidden VS-write bucket? - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `primitive-reorder-diagnostics.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the family of **diagnostic primitive/triangle reorder probes**
that test whether index/primitive *order* (not vertex expansion, not draw count)
is the first-order owner of the hidden "VS Buffer Device Memory Bytes Written"
bucket. It spans three subcategories: `reverse.*` (this file's leaves — full and
scoped reverse-triangle-order probes), `split.*` (order-preserving bounded
large-draw splits — see [primitive-reorder-diagnostics-split.04](primitive-reorder-diagnostics-split.04.md)), and
`minindex.*` (min-index / cache-aware reorder scouts — see
[primitive-reorder-diagnostics-minindex.04](primitive-reorder-diagnostics-minindex.04.md)). The central conclusion: order
*can* move the hidden bucket, but every apparent win was frame-shape-sensitive
and almost all were rejected. The lasting value was motivating the
semantic-safe, cached index-cache-locality path in [index-cache-locality](../index-cache-locality/index.md).

## Latest Conclusions

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H10 | Order-preserving large-draw split owns it (size, not order) | rejected | [primitive-reorder-diagnostics-split.04](primitive-reorder-diagnostics-split.04.md) |
| H11 | The historical 4-draw win is stable on current HEAD | rejected (anomaly) | [primitive-reorder-diagnostics-reverse.15](primitive-reorder-diagnostics-reverse.15.md) |
| H12 | Scissor rectangle/tile coverage owns the historical win | rejected | [primitive-reorder-diagnostics-reverse.16](primitive-reorder-diagnostics-reverse.16.md) |
| H13 | The full 16-draw / `60/1` opaque reverse reproduces on current HEAD | rejected | [primitive-reorder-diagnostics-reverse.17](primitive-reorder-diagnostics-reverse.17.md), [primitive-reorder-diagnostics-reverse.18](primitive-reorder-diagnostics-reverse.18.md) |
| H14 | Order is the *stable* owner of the hidden bucket | rejected — it is frame-shape-sensitive; semantic-safe lever lives in [index-cache-locality](../index-cache-locality/index.md) | whole domain |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [primitive-reorder-diagnostics-reverse.18 - Current Row 60/1 Opaque Reverse Rerun](primitive-reorder-diagnostics-reverse.18.md)
- [primitive-reorder-diagnostics-reverse.17 - Current Full Large4096 + Alpha Reorder Rerun](primitive-reorder-diagnostics-reverse.17.md)
- [primitive-reorder-diagnostics-reverse.16 - Scissor Rectangle/Tiling Probe](primitive-reorder-diagnostics-reverse.16.md)
- [primitive-reorder-diagnostics-reverse.15 - Current Diagnostic 60/4 4-Draw Rerun](primitive-reorder-diagnostics-reverse.15.md)
- [primitive-reorder-diagnostics-reverse.14 - Opaque Large4096 Reverse](primitive-reorder-diagnostics-reverse.14.md)
- [primitive-reorder-diagnostics-reverse.13 - Row/Class 60/4 Large4096 + Alpha + Scissor Reverse](primitive-reorder-diagnostics-reverse.13.md)
- [primitive-reorder-diagnostics-reverse.12 - Row/Class 60/4 Large4096 + Alpha Reverse](primitive-reorder-diagnostics-reverse.12.md)
- [primitive-reorder-diagnostics-reverse.11 - Row/Class 60/4 Large4096 Reverse](primitive-reorder-diagnostics-reverse.11.md)
