---
domain: present-pacing
workload: 3DMark05 GT2 r405 original Render Tape
title: "H397: Capture-only ingress promotion and retired-selector validation"
type: evidence
status: promoted-residual-qualification-open
updated: 2026-09-27
source: traces/ingress-h397/{run.py,audit.py,summary.json,native-tests.txt,tla.log}; traces/ingress-h397/{compatibility,production-slot}-ingress-{unset,0}/result.json
related: present-pacing-ingress-gpu-exact.396.md; present-pacing-ingress-handoff-validation.395.md
---

# H397: One ingress policy, independently of Replay execution policy

H396 evidence was committed as `058a6e2f`. The user explicitly requested
default promotion and removal of the OFF path. Ingress now unconditionally
captures backing snapshots without provisional marking; the environment read
and policy reducer are deleted. The base Device capture fallback no longer
silently calls combined marking. Existing direct publication and compatibility
Replay marking, mutation fences and completion-based reclamation are unchanged.
The combined queue helper and test spies remain as reference-protocol coverage,
not a selectable ingress policy.

## Final-binary evidence

- Full x86_64 provider build succeeds; focused routing, byte identity,
  offload/mutation, rename/lifetime and install-name tests pass **13/13**.
- Full `verify_tla.sh` regression exits zero. These unchanged models remain
  bounded protocol evidence, not proof of the residual composition gates.
- Reuse H396's unchanged original input and source boundaries, with two
  cold-device iterations per route/setting. CPU-ready Tape and commit offload
  remain zero. Compare an unset retired selector against explicit `0`, not
  an OFF/ON policy experiment.
- Compatibility and ProductionSlot pass **8/8 iterations**, with **16/16**
  original-exact RGBA readbacks. Input, provider, validator, all loaded image
  hashes and normalized environments match across the four runs.
- Each readback is 3,145,728 bytes, SHA-256
  `d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
- Compatibility ticket rechecks are **372** for both settings; ProductionSlot
  rechecks are **8** for both. Restamp fires are zero. Path counters are equal
  within each route. This confirms the retired selector cannot restore the old
  ingress work; it does not justify deleting Replay stamp/repair obligations.

The Metal skill environment check passes on Xcode/xctrace 27, Metal compiler
32023.921 and Apple M1/Metal 4. The oracle is synchronous provider replay, not
an asynchronous application timing or FPS measurement.

## Residual gates

Promotion is a user-directed deployment decision before all residual
R-BACK-43.9 qualification gates are closed. All-family failure/interleaving and
Reset composition, STALKER wild correctness, and matched performance remain
open. H395's parked-worker and backing tests complement this GPU oracle but
do not establish arbitrary schedules or all workloads.

The scoped source-artifact documentation audit passes. The global audit still
reports missing historical logs in unrelated mini-replay-bisection and H316
leaves; those artifacts were not recreated or treated as new evidence.
