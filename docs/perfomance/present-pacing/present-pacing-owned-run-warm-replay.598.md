---
domain: present-pacing
workload: STALKER H502/r5 repeated prepared replay on one Device
title: "Same-device warm replay fails the reference pixel oracle before timing"
type: evidence
status: rejected-timing-population-reference-output-mismatch
updated: 2026-10-16
source: traces/stkcop-owned-run-cpu-h598/{contract.md,prepare.py,source_probe.cpp,build.json,run.py,runs.json,O0.log}; traces/stkcop-owned-run-cpu-h598/O0/{0,1}/census.json
related: present-pacing-owned-run-construction.597.md; present-pacing-replay-cpu-owner.596.md
---

# No warmed CPU contrast is accepted

H598 attempted two warmups followed by sixteen same-input replays per process,
with process order O0,N0,N1,O1. The renderer is the already-qualified H597 image;
only the capture executable adds a loop and caller-self CPU endpoints. Prepared
input and Device are reused within each process.

O0 reference iteration0 passes the original pixel/query/source/lifetime oracle.
Reference iteration1 returns **`status=output-mismatch`, event5025**, with both
source and output equality false. Query transcript, seven source mappings,
670 created/released wrappers and reported normal-state restoration still pass.
The executable exits1, and the runner stops before launching any candidate.
No measured iterations are admitted, no comparison is computed, and there are
no replacement or extension runs. Artifacts remain intact.

This counterexample rejects this probe's same-device repeated-replay timing
gate. It is **not evidence that the H597 candidate changes pixels**, nor proof
of a default-live renderer regression. The mechanism behind the repeated
reference mismatch is not yet diagnosed. Do not silently create a fresh Device
per iteration and describe that as warmed capacity reuse.

CPU endpoints were placed in the existing armed/closed observer callbacks.
Inspection of `src/d3d9/device_c_render_tape_provider.cpp`,
`PreparedObserverV4Session` and the replay cleanup callback, shows this interval
includes Tape verification, readback/query handling and resource retirement,
not exclusive chunk-replay CPU. Even a passing result would need that scope
label and would not establish default-live route frequency or per-frame savings.

The next gate moves to actual default-live STALKER with the unchanged H597
same-image candidate and a newly frozen OFF/ON population. H597's native
refinement and eight single-replay effect controls remain valid. H598 supplies
no CPU-saving estimate and does not complete the original optimization goal.
