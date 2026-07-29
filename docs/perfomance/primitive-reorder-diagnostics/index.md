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
