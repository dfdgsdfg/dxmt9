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

Latest tracked row: `H38` - Same-generation draw-submission state-copy elision directly causes the latest transparent-weapon / black-vertex report (rejected for the sampled effects-heavy window; `DXMT9_DISABLE_DRAW_SUBMISSION_STATE_ELISION=1` forces `d3d9_snapshot_state_elided=0`, while the default path elides `411,532` states / `4.211GiB`, and both screenshots render coherent bloom, sparks, geometry, and lighting. Keep the knob as an exact-window diagnostic, but do not demote P4 work based on state elision alone).

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [snapshot-cache-snapshot.29 - Batch Miss Semantic Reuse Probe Rejects Small Recent-Key Cache](snapshot-cache-snapshot.29.md)
- [snapshot-cache-snapshot.28 - Batch Miss Refreshes Hot State In Place](snapshot-cache-snapshot.28.md)
- [snapshot-cache-snapshot.27 - Batch Miss Reuses Non-constant Uniform Payload Fields](snapshot-cache-snapshot.27.md)
- [snapshot-cache-snapshot.26 - Replay / Snapshot Derived Ranking Re-centers P2/P3 After Direct Cbuf](snapshot-cache-snapshot.26.md)
- [snapshot-cache-snapshot.25 - Batch-Miss Uniform Payload Reuse Gate](snapshot-cache-snapshot.25.md)
- [snapshot-cache-snapshot.24 - Batch-Miss Reason Bucket Instrumentation](snapshot-cache-snapshot.24.md)
- [snapshot-cache-snapshot.23 - Direct-Cbuf Residual Snapshot Owner Recheck](snapshot-cache-snapshot.23.md)
- [snapshot-cache-snapshot.22 - Redundant Shader Constant No-Op Invalidation](snapshot-cache-snapshot.22.md)
- [snapshot-cache-snapshot.21 - Binding-Only Miss Reason Recheck](snapshot-cache-snapshot.21.md)
- [snapshot-cache-snapshot.20 - Adjacent Uniform Generation Opportunity Probe](snapshot-cache-snapshot.20.md)
- [snapshot-cache-snapshot.19 - Batch-Miss Shader Layout Reuse](snapshot-cache-snapshot.19.md)
- [snapshot-cache-snapshot.18 - VS Indexed-Float Partial Hash Opportunity Probe](snapshot-cache-snapshot.18.md)
