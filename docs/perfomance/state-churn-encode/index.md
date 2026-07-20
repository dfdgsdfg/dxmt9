---
domain: state-churn-encode
workload: 3DMark05 GT1
title: "State-Churn Encode — the CPU encode path and draw-run batching"
type: domain-index
status: current
updated: 2026-07-20
source: docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.202.md
related: docs/perfomance/state-churn-encode/overview.md; docs/perfomance/state-churn-encode/log.md
---

# State-Churn Encode — the CPU encode path and draw-run batching

Latest tracked row: `H211` - direct-cbuf is a general constants-only Stage 2 CPU cleanup, but remains default-off.

Current status: the commit-replay offload is engine-default ON (`d45af067`, H216 in [present-pacing](../present-pacing/index.md)), and the rejected replay-carrier lanes documented in this domain's history (chunk-end carry + `AndRun`/`WithResourceMarking` family, draw-run preflush merge/mixed-carrier, compact uniform submission carrier, canonical draw-run fast path, publish-time PSO prefetch) were removed from the tree in the H217-H220 cleanup waves — see the [overview](overview.md) current-status section.

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [state-churn-encode-encode-phase.202 - Direct-Cbuf Cross-Workload Generality Gate](state-churn-encode-encode-phase.202.md)
- [state-churn-encode-encode-phase.201 - Uniform Append Residual After Fixed Handle Carry](state-churn-encode-encode-phase.201.md)
- [state-churn-encode-encode-phase.200 - Uniform Fixed Payload Handle Carry](state-churn-encode-encode-phase.200.md)
- [state-churn-encode-encode-phase.199 - Stage-Level Uniform Append Split Counters](state-churn-encode-encode-phase.199.md)
- [state-churn-encode-encode-phase.198 - Append Uniform CPU Residual Reanalysis](state-churn-encode-encode-phase.198.md)
- [state-churn-encode-encode-phase.197 - Draw Batch Submit Residual Reanalysis](state-churn-encode-encode-phase.197.md)
- [state-churn-encode-encode-phase.196 - Queue Lock Attribution Runtime](state-churn-encode-encode-phase.196.md)
- [state-churn-encode-encode-phase.195 - Current Wall Review and Next Owner Split](state-churn-encode-encode-phase.195.md)
- [state-churn-encode-encode-phase.194 - Forced Resource-Marking Flush Attribution](state-churn-encode-encode-phase.194.md)
- [state-churn-encode-encode-phase.193 - Chunk-End Carry Runtime Gate](state-churn-encode-encode-phase.193.md)
- [state-churn-encode-encode-phase.192 - Owned Chunk-End Carry Skeleton](state-churn-encode-encode-phase.192.md)
- [state-churn-encode-encode-phase.191 - Forced Resource-Marking Submit Prerequisite](state-churn-encode-encode-phase.191.md)
- [state-churn-encode-encode-phase.190 - Chunk-End Carry Feasibility Audit](state-churn-encode-encode-phase.190.md)
