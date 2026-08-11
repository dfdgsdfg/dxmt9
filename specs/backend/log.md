---
type: "Spec Log"
title: "Backend Log"
description: "Domain-level maintenance history for Backend specs."
tags: [specs, log, backend]
---

# Backend Log

Domain-level maintenance history for `backend/` specs. Keep current implementation and evidence status in [gap](gap.md), and use this log for structural edits, migrations, and older detail that should not stay in the current overview.

## 2026-08-11

- Reclassified tile-FFP from "correct; performance pending" to a
  correctness-blocked candidate. The 2026-05-25 two-stage base-colour plus tile
  dispatch fixed the black full-screen draw and passed its single-draw equality
  fixture, but a partial-rectangle readback later proved that the
  attachment-wide dispatch modifies uncovered clear pixels and cannot recover
  pre-draw colour after alpha rejection.
- Added `R-BACK-13.7` as the coverage/prior-colour contract. Non-diagnostic
  `DXMT9_TILE_FFP=auto` now resolves to portable; `force` is retained only for
  diagnostic fixtures. Current status stays in [gap](gap.md) and
  [render-provider gap](render-provider/gap.md).

## 2026-07-08

- Added this domain log as the maintenance history companion for the domain specs.
