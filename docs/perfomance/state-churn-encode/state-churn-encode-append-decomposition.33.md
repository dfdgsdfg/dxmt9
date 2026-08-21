---
domain: state-churn-encode
workload: 3DMark05 GT1/GT2/GT3 + SFIV
subcategory: append-decomposition
order: 33
title: Series Closure — Multi-Workload Gate Green, appendHandle O(1), T2d Deferred
date: 2026-08-21
type: experiment-run
status: accepted-verdict
source: experiments/output/app-d3d9-3dmark05-mw-{gt1,gt3}; experiments/output/app-d3d9-sfiv-benchmark-mw-sfiv; experiments/output/app-d3d9-3dmark05-qmutex-gt1; experiments/output/app-d3d9-sfiv-benchmark-qmutex-sfiv
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.32.md
---

# Series Closure — Multi-Workload Gate Green, appendHandle O(1), T2d Deferred

**1. The GT2-only evidence debt is paid.** The whole T2-era default set
(getter cache, warm epochs, cadence, T2a'/b'/c', scan batches) was gated on
GT2 alone; the multi-workload run closes that: GT1 31.4/31.8 fps, GT3
67/70 fps, SFIV **in-benchmark average 47.0 vs the 43.0 recorded in the
getter-cache era (+9%)** — all three status-pass, zero GPU errors, and
screenshot review clean (GT1 mech scene, GT3 canyon airship, SFIV ink-shaded
Ryu — no black geometry, UV, or texture drift). The SFIV number is the first
cross-workload confirmation that the producer-wall series transferred beyond
GT2.

**2. appendHandle joins the O(1) set** (`80ca19d6`, salvaged from a stalled
worker). The design honors the subtlety that made a naive fix wrong: the
original record-local scan is identity-keyed and DETECTS an integrity fault
(one generation-qualified identity arriving through two different pointers
fails the record) that a pointer-keyed lookup would silently swallow — so
the new table keys by the (kind, generation, objectId) tuple, stamps slots
with never-reused record ordinals (record-locality without per-record
clears; rolled-back ordinals self-invalidate), and overflow permanently
falls back to the original scan. Specs pin same-record hits, cross-record
misses, rollback non-aliasing, fault preservation, and overflow.
Conformance 234/235 at the final head (known xyzhw only; the decl flake did
not fire this run).

**3. T2d deferred on cross-workload evidence** (`2b64faa9`). Queue mutex
profiles outside GT2: GT1 total acquire-wait **0.126 ms/present**, SFIV
**0.089** — the producer sites do not even reach the top rows. Having
removed the producer's acquires (T2a'/b'/c'), the slot-append hold
(GT2 3.4-3.8 / GT1 1.46 / SFIV 0.86 ms/present) has no waiting victim
anywhere, so reserve-copy-commit would buy fps on no measured workload.
It stays an architecture option with recorded reopen conditions.

**Hygiene shipped alongside** (`decb7faf`): TSan halt-on-error in the test
env (with the corrected premise — no project ASan block ever existed), and
the thread-ownership declaration drift audit (`dxmt9-thread-ownership-audit`)
with induced-failure negative controls.

**The producer track's shape after this series.** Across .17→.33 the
game-thread dxmt9 share went from ~27% of a 37 ms frame to: bridge crossings
harvested (−2.2 ms, +7.2%), mutex waits eliminated (2.03 → ~0.15 ms
producer-side), the measured linear scans O(1)'d, and the diagnostic
scaffolds fenced behind the observer boundary. What remains on the
game-thread wall is dominated by the app itself (~60-66%), touchConstShadow
(0.42), buildSparseState (~0.23), and the slack-absorbed lock-path work the
path rule deprioritized. The next fps-bearing frontier is no longer the
producer: it is per-workload GPU/encode shape (and the parked parallel lane
if a workload ever makes encode the pacer).
