---
type: "Spec Log"
title: "D3D9 Renderer Log"
description: "Domain-level maintenance history for D3D9 Renderer specs."
tags: [specs, log, d3d9-renderer]
---

# D3D9 Renderer Log

Domain-level maintenance history for `d3d9-renderer/` specs. Keep current implementation and evidence status in [gap](gap.md), and use this log for structural edits, migrations, and older detail that should not stay in the current overview.

## 2026-07-25

- Promoted the alias-aware, order-proven `framegraph + progressive +
  passcoalesce` L1 subset to the runtime and experiment-runner default.
- Preserved explicit `traditional`, `strict`, and empty-feature rollback paths.
  Memoryless, DCE, generic reorder, mesh, bindless, object scheduling, and
  GPU-driven execution remain disabled.
- Closed the SFIV rendered-scene evidence debt with an env-clean default
  capture and a separate low-overhead stability run with zero GPU/pipeline
  failures.
- Kept device-backed pixel parity as evidence debt rather than silently
  treating the broader modern-renderer plan as complete.

## 2026-07-08

- Added this domain log as the maintenance history companion for the domain specs.
