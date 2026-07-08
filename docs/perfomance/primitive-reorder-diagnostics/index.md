---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
title: "Primitive-Reorder Diagnostics — does index/primitive ORDER own the hidden VS-write bucket?"
type: domain-index
status: current
updated: 2026-07-08
source: docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/primitive-reorder-diagnostics/overview.md; docs/perfomance/primitive-reorder-diagnostics/log.md
---

# Primitive-Reorder Diagnostics — does index/primitive ORDER own the hidden VS-write bucket?

Latest tracked row: `H14` - Order is the *stable* owner of the hidden bucket (rejected — it is frame-shape-sensitive; semantic-safe lever lives in [index-cache-locality](../index-cache-locality/index.md)).

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
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
- [primitive-reorder-diagnostics-reverse.10 - Row/Class 60/4 Alpha Reverse](primitive-reorder-diagnostics-reverse.10.md)
- [primitive-reorder-diagnostics-reverse.09 - Reverse Material-Class Probe Tooling](primitive-reorder-diagnostics-reverse.09.md)
- [primitive-reorder-diagnostics-reverse.08 - Row 60/4 Reverse](primitive-reorder-diagnostics-reverse.08.md)
- [primitive-reorder-diagnostics-reverse.07 - Row-Set Hotrows Reverse](primitive-reorder-diagnostics-reverse.07.md)
