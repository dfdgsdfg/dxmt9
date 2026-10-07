---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "Default-path tail CPU: external thread budgets before function attribution"
type: evidence
status: thread-budgets-measured-removable-cost-open
updated: 2026-10-07
source: traces/stkcop-cpu-owner-h449/{run.py,join.py,analyze.py,calibration.json,receipt.json,cpu.jsonl,identity-join-1.json,owners.json,scene.png,middle.sample.txt,late1.sample.txt,late2.sample.txt}; experiments/output/app-d3d9-stkcop-bench-cpu-owner-h449/{result.json,daybenchmark.result,stkcop-requested.ltx,stkcop-after-run.ltx,stkcop-controls.txt}
related: present-pacing-l64-repaired-wild-abba.448.md; present-pacing-drawrun-preparation.426.md; present-pacing-stalker-cpu-recheck.392.md
---

# Default-path thread budgets, not a removable-cost verdict

At `bafada9e`, one Day/high 1280x720 run uses the unchanged H448 repaired
images, Sikarugir CX24.0.7, two engine threads, sixteen sound targets and
command limit 0. No candidate query/range policy is enabled. All five staged
artifact receipts and requested/final/control-file hashes match H448 A1.
The runner exits normally and passes. The manually inspected loading and
150-second scene images show the game; they do not prove continuous foreground
or full-route pixel equality. No runtime code or default changes.

## Observation and identity

Product perf counters, frame sampling, supply/phase observers and PE sampler
are OFF. A separate process reads every native thread's cumulative user and
system CPU every 0.5 seconds through libproc. Each snapshot checks process
start identity before/after; the largest all-thread query takes 0.421 ms.
The reader is calibrated against the existing native-TID CPU helper in a
single-threaded process, requiring its counters to lie inside the external
counter brackets. This checks the ctypes layout and counter route, not zero
observer perturbation.

`PROC_PIDLISTTHREADS` yields thread handles, not native TIDs. A same-process
bracket joins stack TIDs through `PROC_PIDTHREADID64INFO` to continuously
observed handles by name and both monotone CPU counters. All 28 initial stack
TIDs have exactly one match; the replay role is established by
`ReplayOffloadWorker::run` ancestry, not an unnamed-thread ordinal.

Three `/usr/bin/sample` requests use 3 seconds / 5 ms at process-relative
110, 175 and 195 seconds. Each command takes 8–10 seconds including analysis.
The CPU windows below exclude the **whole command lifetime**, not merely the
requested sampling duration, and exclude screenshot operations. Their endpoints
are actual counter snapshots; all selected threads have continuous presence and
monotone counters. One terminal ESRCH after the game exits is preserved outside
the windows, not converted into a zero CPU sample.

## Measured native CPU occupancy

One core means one CPU-second per elapsed second. These overlapping threads
must not be added to derive a frame time. Window positions are relative to
engine discovery, not exact engine-frame or matched-camera identities.

| Actual process-relative interval | App Primary | Replay | Encode | Yielding game helper |
|---|---:|---:|---:|---:|
| 125.07–144.78 s | 0.886 cores | 0.558 | 0.591 | 0.929 |
| 155.39–169.54 s | 0.853 | 0.539 | 0.555 | 0.935 |
| 185.23–192.82 s | 0.816 | 0.487 | 0.513 | 0.914 |
| 205.47–214.56 s | 0.894 | 0.524 | 0.547 | 0.930 |

Encode is slightly larger than Replay among the two identified dxmt9 workers.
This is **not** a ranking against PE/synchronous Unix on Primary: its CPU
includes game and Wine too, and exclusive attribution there remains absent.
Nor does about half-core average exclude short service-critical bursts.

The game helper is a distinct native TID with repeated `NtYieldExecution` /
`sched_yield` ancestry. It is not the replay worker. Its cost is not chargeable
to dxmt9 or newly established as removable by a renderer change.

## Stack leads and limits

The disjoint stack reducer checks child-count conservation for every thread.
These are wall-state populations, not CPU-weighted percentages.

| Sample window | Encode ready-input wait | Replay input wait |
|---|---:|---:|
| Middle | 75 / 150 | 80 / 150 |
| Late 1 | 153 / 194 | 162 / 194 |
| Late 2 | 77 / 113 | 70 / 113 |

Among non-wait/unresolved Encode stacks, `encodeChunk` appears in 34/39 and
30/35 late samples; nested `encodeDraw` appears in 21 and 12 respectively.
Replay's concrete recurring chain is `replaySparseRecord` →
`submitCompatibilityReplayDrawBatched` → `cachedBaseDrawStateForSubmissionBatch`,
plus `flushCompatibilityReplayDrawBatch` / `submitDrawRunBatch`.
These small populations locate candidates, not their removable milliseconds.
Rosetta leaves remain opaque; nearest named ancestors are not self-PC costs.

## Decision

This replaces old-image thread-budget assumptions with current default-path,
product-counter-OFF evidence. H426's finer DrawRun partition used query ranges
ON and product diagnostics ON, so it is not a current default-path CPU budget.
H443's controlled acknowledgment consumer is likewise not production Encode.

Do not promote grouping, parallel encoding or another snapshot rewrite from
these measurements. The largest dxmt9 **function-level** CPU owner and its
semantics-preserving removable amount are still unproven. The next bounded
measurement is Running-only CPU stack attribution with same-process image
mapping and submission timing, followed by an identical-input cost/removal
test for the selected owner. No FPS, README or whole-pipeline speedup claim is
made from this externally observed run.
