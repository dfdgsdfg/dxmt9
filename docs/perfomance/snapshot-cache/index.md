---
domain: snapshot-cache
workload: 3DMark05 GT1
title: "Snapshot Cache — D3D9 frontend draw-state snapshot/rebuild CPU bottleneck"
type: domain-index
status: current
updated: 2026-07-08
source: docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/snapshot-cache/overview.md; docs/perfomance/snapshot-cache/log.md
---

# Snapshot Cache — D3D9 frontend draw-state snapshot/rebuild CPU bottleneck

Latest tracked row: `H39` - GT1 t=40s giant-triangle artifact fixed (`a123166d`): cross-lane invalidation reason-mask poisoning let the batch snapshot cache reuse a stale shader layout; layout reuse now keys off a dedicated generation, pinned by permanent hit-path asserts and a bite-proven regression test.

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [snapshot-cache-snapshot.29 - Batch Miss Semantic Reuse Probe Rejects Small Recent-Key Cache](snapshot-cache-snapshot.29.md)
