---
type: "Spec Log"
title: "Specifications Log"
description: "Root-level maintenance history for durable spec documents."
tags: [specs, log]
---

# Specifications Log

Root-level maintenance history for durable spec documents. Implementation plans
remain local-only and are excluded from the spec metadata sweep.

## 2026-07-08

- Added lightweight YAML frontmatter to durable `specs/**/*.md` documents except
  `specs/README.md`.
- Added root [index](index.md) as the spec-tree entry point.
- Added this shared [log](log.md) for spec-tree maintenance history.
- Left local-only `*.plan.md` and `specs/**/plan.md` files without metadata.
- Split the former root gap tracker into domain-owned `gap.md` files under
  `specs/{archicture,d3d9,backend,d3d9-renderer,deploy,verification,tests,experiments,benchmarks,d3d8,d3d7}/`.
- Moved the D3D9 API coverage inventory to [d3d9/gap_d3d9](d3d9/gap_d3d9.md)
  and the Wine D3D9 test inventory to
  [tests/gap_d3d9_wine_test](tests/gap_d3d9_wine_test.md).
- Reduced [gap](gap.md) to a project-level overview with domain gap/log links.
- Added domain `log.md` files for every top-level spec domain, including
  `winemetal/`, so structural edits and older detail no longer need to live in
  the root gap overview.
- Renamed durable topic design documents from `design.md` to `spec.md` and
  updated spec-tree references plus documentation rules to use the new name.
