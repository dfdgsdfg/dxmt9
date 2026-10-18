---
domain: present-pacing
workload: Native ARM64/Rosetta fixtures and original STALKER captured input
title: "Cooperative owned-worker CPU observer passes native and original-input gates"
type: evidence
status: private-observer-qualified
updated: 2026-10-16
source: traces/stkcop-owned-workers-h566/{contract.md,observer.hpp,analyze.py,preflight.json,build.json,link-verification.json,capture-verification.json,capture-off-receipt.json,capture-on-receipt.json}
related: present-pacing-process-freshness.565.md; present-pacing-owner-live.564.md
---

# Observe remaining worker bodies directly

H565 rejected the own-process aggregate CPU endpoint assumption. H566 instead
adds calling-thread CPU checkpoints to eight explicitly owned worker roles:
Finish, Completion, pipeline/library compilation, archive load/save, and the
two presenter acquisition workers. Replay/Encode retain their existing sparse
self observers. Product rendering policy and canonical binaries are unchanged.

## Validation

| Gate | Result |
|---|---|
| ARM64 and Rosetta lifecycle fixture | Each has 16 instances, 16 native TIDs, 1,604 published rows; disabled fixture exits zero |
| Negative contract cases | 20 pass, including identity, lifecycle, loss and publication rejection |
| Baseline rebuild parity | All five rebuilt original object members are byte-identical to H559 A |
| Private archive | Exactly five instrumented code objects differ; 74 other code members are unchanged |
| Original captured input, observer OFF/ON | Both exit zero and reproduce exact source/output BGRX SHA-256 |
| ON worker trace | 10 instances, 188 rows, one attachment, roles Finish/Completion/pipeline/library; every observed body exits |

The initial build gate stopped because it counted the regenerated `__.SYMDEF`
archive symbol index as an unexpected code member. The failed `build.json`
remains frozen. `resume_link.py` independently compares every member, binds
each changed object to its compiled file, admits only those five objects plus
the index, then links and audits the Unix dependency install names. It does
not waive another code-object difference or rebuild the failed experiment.

Both original-input runs preserve seven sources, 670 balanced wrappers,
9 query issues, 5 polls and 4 results. Exact source/output hash:
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
This is captured-input correctness, not live/default timing evidence.

## Scope limits

The observer uses PID/birth/TID and monotonically allocated instance identity,
fixed storage, and cooperative entry/work/exit checkpoints. It creates no
timer, forced wake, artificial job or suspended thread. Finish/Completion
sample the first and every 64th iteration; other roles sample work boundaries.
The CPU API is calling-thread `proc_pidinfo`, not Rosetta's rejected
`CLOCK_THREAD_CPUTIME_ID` or the rejected process-residual shortcut.

Bounds cover the instrumented **worker body**, not closure setup/destruction
outside it or shared Metal/driver workers. Missing enclosing endpoints and
unclosed instances remain unbounded; absent evidence never becomes zero.
The live remaining-worker episode and observer/default transfer are separate
gates. No global-largest-owner, removable-CPU or FPS verdict follows here.
