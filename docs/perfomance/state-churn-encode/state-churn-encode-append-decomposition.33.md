---
domain: state-churn-encode
workload: 3DMark05 GT1/GT3 + SFIV (evidence-availability correction)
subcategory: append-decomposition
order: 33
title: Series Closure — Evidence Availability Correction; Multi-Workload Gate Deferred
date: 2026-08-21
type: experiment-run
status: accepted-verdict
source: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.32.md; scripts/check/audit_perf_docs_sources.py
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.32.md
---

# Series Closure — Evidence Availability Correction; Multi-Workload Gate Deferred

This leaf is an evidence-retention correction, not a new multi-workload
acceptance claim. The previously cited `experiments/output/app-d3d9-3dmark05-
mw-gt1` and `...-mw-gt3` directories are absent. The GT1/GT3 multi-workload
frame-rate, visual, and “gate green” statements that depended on those paths
cannot be rechecked and are withdrawn here. They remain historical debt rather
than current evidence.

No raw run artifact is claimed as surviving by this leaf. The only accepted
verdict is that the multi-workload gate is deferred: the prior GT1/GT3/SFIV
run directories are not tracked provenance, and the GT1/GT3 multi-workload
frame-rate, visual, GPU-error, and “gate green” statements cannot be
rechecked. The earlier SFIV and GT1 qmutex values are therefore not promoted,
and no FPS comparison is asserted here.

The append-handle O(1) design discussion and the cross-workload T2d decision
from the earlier version are retained only as historical context. Their
implementation/profiling proof is not reasserted by this leaf. Reopen the gate
only after concrete GT1 and GT3 artifacts, including result summaries and
visual/error checks, are restored under `source:`.
