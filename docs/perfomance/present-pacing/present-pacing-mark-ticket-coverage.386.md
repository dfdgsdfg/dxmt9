---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "H386: Audit uncovered mark-ticket acquisition time"
type: evidence
status: compatibility-holder-attributed-residual-open
updated: 2026-09-27
source: traces/app-d3d9-stkcop-mark-wait-h386/{calibrate.cpp,calibrate,calibration.csv,calibration-timing.csv,distribution.py,distribution.json,run.py,receipt.jsonl,supply.csv,summary.json}; traces/app-d3d9-stkcop-mark-wait-h386b/{run.py,receipt.jsonl,supply.csv}; experiments/output/app-d3d9-stkcop-bench-{mark-wait-h386,mark-wait-h386b}/{result.json,dxmt9.log}; traces/app-d3d9-stkcop-submit-summary-h385/{supply.csv,waits.json}
related: present-pacing-streaming-summary.385.md
---

# H386: Audit uncovered mark-ticket acquisition time

## Existing evidence, before changing observation coverage

The H385 interior 2702–2718 contains 1,409 mark-ticket attempts: 19.214ms
wall, 2.514ms thread CPU, and 1.108ms same-mutex/different-thread measured
holder overlap. Of these, 1,402 attempts have no observed holder: 17.588ms
wall, 2.436ms CPU, median 1us, p95 94us, maximum 359us. The other seven
attempts take 1.626ms wall. Absence of a holder event is not an unlocked-mutex
proof.

The 16.700ms wall-minus-CPU difference is not exclusive scheduler time:
`src/dxmt9/dxmt9_cpu_supply_trace.cpp`, `record`, samples wall before calling
`nativeCpuNs` (`proc_pidinfo`), so the two endpoints are not simultaneous.
`markChunkResourcesAndCaptureBufferBindings` includes the beginning event's
remaining work, `queueMutexProbeBegin`, the real lock and the ending event's
pre-timestamp work. It is not a mutex-only stopwatch.

## Observer calibration

The standalone x86_64/Rosetta `calibrate.cpp` compiles the current production
`dxmt9_cpu_supply_trace.cpp` at `-O2`. It runs 10,000 iterations per mode:
clock/lock, record/lock, and record/no-lock. The mutex has no other owner.
Timestamps around each record call and around the body are retained in fixed
arrays and reported after the trace closes. This is a diagnostic calibration,
not the full Wine app environment or a subtraction correction.

- Uncontended lock plus clock boundary: median/p95 42ns.
- Begin/end recording: median 541ns, p95 584ns.
- Recorded-lock body: median/p95 42ns.
- No-lock recording has similar cost; the largest record in this run is 4.5us.

Thus ordinary record cost in this isolated run does not reproduce the
100–350us application tail. It also cannot rule out observer-induced stalls
under real contention or system load.

## Missing holder coverage found in code

`src/dxmt9/dxmt9_command_queue.cpp`, `commitDirectChunkSlotReplay`, takes the
same queue mutex but previously emitted no held-body events. Under that lock
it marks the raw resource closure, walks span commands to mark their resources,
and walks the closure again for `ArenaResourceRetainDigest`. The latter two
both call `visitSourceCommandResources` through independently reconstructed
command views. This is an unobserved holder candidate, not yet causal proof.

Add only three uninterrupted diagnostic segments, each ending before any
fail-stop unlock or publication/CV path:

- `direct_commit/raw_resource_mark`
- `direct_commit/span_resource_mark`
- `direct_commit/retain_digest`

There is no change to lock ownership, marking order, retention, submission,
or default runtime policy. Other admission/compatibility and lifecycle gaps
remain uninstrumented; coverage is deliberately partial.

## Two controlled recaptures

Both catalogue runs use Day/high 1280x720, `-max-threads 2`, `snd_targets 16`,
perf profile and Present window 2700–2720, trimmed to 2702–2718. Builds and
tests finish before each run. Both report PASS, no failures and zero GPU
command-buffer errors. Staged binary hashes are retained. Both traces close
without overflow, incomplete writers, attempts or holds. Neither is an
observer-OFF FPS comparison or full visual certification.

H386 adds Direct commit coverage only. Its 1,412 attempts total 19.653ms;
observed holder union is 3.395ms. Newly observed Direct resource work overlaps
1.665ms, but 16.258ms remains uncovered. Of the no-holder attempts, 12.353ms
is wholly inside raw Replay spans (a correlation, not a lock-owner proof).

Code inspection then finds that `CommandQueue::submitDrawRun` deliberately
uses `skipHold=true` because some helpers may unlock/wait. Its uninterrupted
binding snapshot and reserve/mark/append bodies were not bracketed internally.
H386b adds scoped observations only around those bodies. The reserve/mark/
append scope is inside the `mutateWritingPayload` callback; it does not wrap
`ensureWritingSlotUnlocked` or `commitCurrentChunk`. Nested mark and append
scopes describe its children and must not be added to the parent total.

### H386b attribution

1,132 mark-ticket attempts total **31.133ms wall, 2.398ms thread CPU**.
Same-mutex/different-thread holder coverage, unioned, is **24.026ms (77.2%)**.

| Observed holder | Overlap with mark-ticket attempts |
|---|---:|
| Compatibility binding snapshots | 3.077ms |
| Compatibility reserve/mark/append parent | 17.525ms |
| ↳ Resource mark child (included above) | 4.059ms |
| ↳ Append child (included above) | **12.865ms** |
| Direct raw/span mark + digest | 1.149ms |
| Encode submit | 1.598ms |
| Other measured holders | 0.677ms |

Compatibility scopes account for **20.602ms**, without double counting their
children. The dominant newly identified holder is Replay's compatible Slot
construction under the queue mutex, particularly the append body—not merely
the Encode submission summary or Direct resource marking.

The 224 attempts with an observed holder account for 30.277ms wall. The 908
attempts with no holder overlap total only **0.856ms**, median/p95 1us,
maximum 14us. The previous long no-holder tail is no longer present in this
run. The remaining **7.107ms** uncovered union difference consists of 0.856ms
no-holder attempts and 6.251ms uncovered portions of holder-overlapping
attempts. The latter can include unbracketed work, observer endpoints and
handoff/scheduling; it is not exclusively proved wake latency.

This identifies a real missing coverage family, but does not retroactively
assign H385's exact 18.106ms to it. Source composition differs, and H386b adds
high-frequency record calls inside the lock (especially per-entry snapshots).
The 31.133ms versus 19.214ms difference is not a product regression measurement.
Do not subtract isolated calibration cost to manufacture observer-OFF numbers.

## Verification and next gate

The Unix provider builds with install-name fixup; queue completion, observer,
analysis and install-name tests pass. TLA regression checks pass (196.83s);
only diagnostic scopes changed, not the modeled queue/lifetime transitions.
Analyzer tests cover all new site names and retain the existing nested-union,
same-thread exclusion and foreign-mutex exclusion checks. `git diff --check`
passes.

Next inspect compatibility append ownership: reserve/allocation, state/uniform
materialization and final vector writes currently occur in the mutex-held
`mutateWritingPayload` body. Measure these on identical captured input with
low-perturbation/aggregated observation. Then decide whether preparation can
move off-lock under a generation-qualified destination lease; preserve
publication atomicity, resource stamps, rollback and completion/reclaim.
Do not remove the mark-ticket lock or reduce its lifetime checks merely
because this run identifies its blocker. No optimization is implemented here.
