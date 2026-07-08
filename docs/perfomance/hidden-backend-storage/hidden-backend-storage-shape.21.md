---
domain: hidden-backend-storage
workload: 3DMark05 GT1
subcategory: shape
order: 21
title: Backend Escape Surface Audit
date: 2026-06-06
type: validation
status: accepted-gate
source: scripts/tools/audit_backend_escape_surface.py; tests/scripts/test_audit_backend_escape_surface.py; scripts/tools/README.md; traces/app-d3d9-3dmark05-post-streamib-frame60-gate-r1/analysis/frame60-backend-escape-surface-audit.md; traces/app-d3d9-3dmark05-post-streamib-frame60-gate-r1/analysis/frame60-backend-escape-surface-audit.csv; traces/app-d3d9-3dmark05-tile-ffp-coverage-r1/analysis/frame60-tile-ffp-coverage.csv; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.14.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.15.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.20.md
---

# Backend Escape Surface Audit

**Question / hypothesis.** After current sample-visible locality is blocked by
the final-writer replay gate ([hidden-backend-storage-shape.20](hidden-backend-storage-shape.20.md)), is any
remaining primitive-order-preserving backend escape ready for a GT1 Xcode
capture?

**Method.**

1. Add `audit_backend_escape_surface.py`.
2. Audit winemetal bridge symbols separately from dxmt9 route/shader-emitter
   symbols.
3. Attach the existing frame60 Tile-FFP coverage CSV.
4. Emit a CSV/Markdown gate before scheduling another backend-denominator
   `.gputrace`.

```mermaid
flowchart TD
  Candidate["backend denominator candidate"]
  Candidate --> Mesh{"mesh/object"}
  Candidate --> Position{"position/binning"}
  Candidate --> Tile{"Tile-FFP"}

  Mesh --> MeshBridge["winemetal bridge present"]
  MeshBridge --> MeshRoute{"dxmt9 route + shader emitter?"}
  MeshRoute -- "No" --> MeshBlock["bridge-only-reduced-ab-required"]
  MeshRoute -- "Yes" --> MeshAB["reduced A/B before GT1"]

  Position --> PosProbe["visible position-only VSOut probe"]
  PosProbe --> PosRoute{"real binning route?"}
  PosRoute -- "No" --> PosBlock["visible-vsout-probe-only"]
  PosRoute -- "Yes" --> PosAB["Xcode bytes/inv gate"]

  Tile --> TileCoverage["Tile-FFP coverage CSV"]
  TileCoverage --> TileBlock["rejected-current-coverage"]
```

**Result.**

| Candidate | Bridge surface | dxmt9 route | Shader emitter | Evidence | Verdict |
|---|---|---|---|---|---|
| mesh/object | present | missing | missing | none | `bridge-only-reduced-ab-required` |
| position/binning | ordinary render | missing | visible `VSOut` probe | visible-width Xcode rejected | `visible-vsout-probe-only` |
| Tile-FFP | present | present | present | `9` frame60 rows with no coverage | `rejected-current-coverage` |

```mermaid
stateDiagram-v2
  [*] --> NonReorderBackend
  NonReorderBackend --> MeshObject
  MeshObject --> BridgeOnly: winemetal supports descriptors/replay
  BridgeOnly --> ReducedAB: no dxmt9 route/emitter
  NonReorderBackend --> PositionBinning
  PositionBinning --> VisibleOnly: source-visible VSOut probe only
  VisibleOnly --> RealRouteNeeded
  NonReorderBackend --> TileFfp
  TileFfp --> CoverageRejected: current GT1 hot rows no coverage
  ReducedAB --> [*]
  RealRouteNeeded --> [*]
  CoverageRejected --> [*]
```

**Verdict.** Accepted as a no-gputrace backend-escape gate. The mesh/object
surface is not a current GT1 hot-path implementation; it is winemetal bridge
support without a dxmt9 shader emitter or draw-route producer. The
position/binning candidate is still only the already rejected visible
position-only `VSOut` family. Tile-FFP is the only full dxmt9 route, but the
current frame60 coverage gate rejects it for GT1 hot rows. The next
primitive-order-preserving backend work must therefore define a reduced
synthetic/replay A/B for mesh/object or implement a real position/binning route
before any Xcode capture.

**Related.** [hidden-backend-storage](../hidden-backend-storage.md) ·
[hidden-backend-storage-shape.14](hidden-backend-storage-shape.14.md) · [hidden-backend-storage-shape.15](hidden-backend-storage-shape.15.md) ·
[hidden-backend-storage-shape.20](hidden-backend-storage-shape.20.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md).
