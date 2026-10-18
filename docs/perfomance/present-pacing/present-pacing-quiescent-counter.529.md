---
domain: present-pacing
workload: owned CPU-only native and Wine fixtures; no STALKER run
title: "External Wine suspension passes bounded CPU-counter consistency preflight"
type: evidence
status: fixture-qualified-live-transfer-open
updated: 2026-10-14
source: traces/stkcop-quiescent-counter-h526/{contract.md,failure-analysis.json,arm64-run.stdout,x86_64-run.stdout}; traces/stkcop-quiescent-external-h527/{contract.md,receipt.json,arm64-run.stdout}; traces/stkcop-wine-quiescent-h528/{contract.md,fixture.cpp,read.c,run.py,receipt.json,analyze.py,analysis.json}; traces/stkcop-wine-quiescent-h528/failed-build-1/{fixture.cpp,original-receipt.json}; traces/stkcop-wine-external-h529/{contract.md,target.cpp,controller.cpp,run.py,receipt.json,analyze.py,analysis.json,verify.py,verification.json}
related: present-pacing-root-wall-live.525.md; present-pacing-cpu-counter-freshness.512.md; present-pacing-self-counter-boundary.513.md
---

# A possible canonical-binary endpoint, not default CPU attribution

H528 and H529 qualify bounded Wine thread-control fixtures without changing
the canonical renderer or requiring a Mach task port. This addresses a
prerequisite for comparing H525's diagnostics with canonical binaries that
have no all-root clocks. No STALKER, GPU, FPS or removal experiment runs here.

## Preserved negative evidence

H526 fails its original **exact** stopped-counter equality gate: one ARM64
round advances by 1,000 ns over 310,542 ns while its heartbeat is unchanged.
The Rosetta fixture has no such change. Neither has a chronological CPU/wall
violation with the separate 2 us finite-host allowance. This does not reclassify
H526 as an exact pass.

H527 attempts external Mach access. `task_for_pid` returns `KERN_FAILURE` (5)
on the owned ARM64 child, before suspension or sampling. The child exits
normally. No permissions, entitlements, signing or system settings change;
the x86_64 child lane is not run. This privileged route is not pursued.

H528 initially fails compilation because a 64-bit packet error field uses
`%u`. Its original source and failed receipt are archived before correcting
the format to `%llu`; that failed attempt stages no service and starts no Wine.

## Wine results

Both H528 and H529 freeze the 2 us held-variation and chronological CPU/wall
allowance **before execution**, not in response to results. Each performs
128 rounds per PE architecture, alternating 1/4/12 ms busy exposures.

| Experiment | Suspension owner | PE32 rounds | PE64 rounds | Held CPU change |
|---|---|---:|---:|---:|
| H528 | Another thread in the same process | 128 | 128 | 0 ns in all rounds |
| H529 | Separate Wine process | 128 | 128 | 0 ns in all rounds |

The target binds its native PID, process birth and numeric TID through the
unchanged H518 service. H529's external controller opens the guest worker
thread, verifies its owning guest PID, and retains that handle. Suspension
requires previous count 0 and successful `GetThreadContext`; resume requires
previous count 1. The target main thread reports equal worker heartbeats on
both sides of the external CPU reads. After resume, the worker issues its own
CPU query with native identity and wall brackets.

All identity, API, heartbeat, monotonicity and frozen CPU/wall checks pass.
H529's parser rejects 15 corrupted receipts, including wrong thread/process
identity, invalid suspension/resume counts, moving heartbeat, failed join,
held increase, counter reversal, invalid bracket and impossible CPU jump.
An independent raw checker verifies all 73,536 chronological sample pairs per
architecture per experiment, rather than only adjacent samples. No pair
violates the retained 2 us consistency allowance.

Both experiments finish naturally and recover their three temporary service
files by hash after the owned processes exit. Final process census is empty.
The canonical Replay/queue source and provider binary hashes match H525.
Existing worktree changes are preserved. No product default, commit, push or
README performance value changes.

## What this does not prove

Stable held reads and CPU-versus-wall consistency are **necessary checks**,
not a proof that the counter has incorporated every prior CPU charge. A
constant accounting offset or delayed charge can pass them. The external
reader is launched per sample pair, so these fixtures also do not measure a
minimal suspension overhead or establish a live perturbation budget.

Wine suspension can park a target holding application or renderer locks.
An external controller avoids self-deadlock in this owned fixture; it does not
guarantee coherent camera state or simultaneous coverage of game, Replay and
Encode threads. A live endpoint must bind actual same-run identities, retain
bounded release-on-failure handling and account for the induced pause. The
same-process fixture alone would not have established external control.

H525 therefore remains a conditional diagnostic Primary exclusion, not a
default largest-owner result. Replay/Encode whole-thread totals near 32 ms
per observed frame remain neither a removable budget nor an additive latency.

## Next gate

Use the externally controlled endpoint only in a predeclared matched
canonical/diagnostic comparison once its freshness uncertainty and scene
endpoint coherence are defensible. Measure pause duration and A/A disturbance;
do not adopt it merely because FPS looks similar. If these gates fail, retain
the fixture result without claiming transfer. Correctness-qualified removable
CPU still requires a distinct actual work-removal experiment.
