---
domain: present-pacing
workload: STALKER CoP Day late windows
title: "H367: Late supply pressure; audio ownership remains unresolved"
type: evidence
status: partial-attribution
updated: 2026-09-25
source: traces/app-d3d9-stkcop-tail-h367/{supply.csv,supply-analysis.json,wait.csv,audio-overloads.log}; traces/app-d3d9-stkcop-tail-h367b/{supply.csv,supply-analysis.json,wait.csv,samples.json,states.json,toc.xml,supervisor.jsonl,audio-overloads.log}; experiments/output/app-d3d9-stkcop-bench-tail-h367/result.json; experiments/output/app-d3d9-stkcop-bench-tail-h367b/result.json
related: present-pacing-coalesce-query-reuse.366.md; present-pacing-encode-residual.364.md
---

# H367: Investigate upstream execution before changing queue policy

At HEAD `4c139c38`, two perf-enabled catalogue runs pass with zero reported
GPU errors. They are diagnostic windows, not observer-OFF FPS baselines or a
paired optimization verdict. Neither run establishes the timing of the user's
audible interruption. No runtime optimization is introduced by this investigation.

## Supply and blocking evidence

Both bounded supply traces close without overflow or incomplete events. The
analyzer trims two Presents at each edge. Thread envelopes have slightly
different endpoints; concurrent and nested scopes must not be added.

| Metric | H367 | H367b |
|---|---:|---:|
| Trimmed Present boundaries | 2702–2728 | 2662–2678 |
| Frame intervals | 26 | 16 |
| Window wall time | 2966.395ms | 1350.388ms |
| Diagnostic window FPS | 8.765 | 11.848 |
| App CPU / Present | 83.085ms | 72.774ms |
| App core utilization | 0.739 | 0.880 |
| Replay scope CPU / Present | 45.956ms | 42.576ms |
| Encode scope CPU / Present | 41.136ms | 39.356ms |
| Replay input wait / Present | 62.696ms | 36.329ms |
| Replay queue empty fraction | 59.28% | 47.98% |
| Encode unpublished-input gap / Present | 69.722ms | 41.265ms |
| Encode ready-dispatch/tail gap / Present | 0.694ms | 0.695ms |

The selected windows have no writer-wait or commit-wait events. Their bounded
wait traces report `closed=1`, `overflow=0`, `incomplete=0`, `cv_entries=0`
and zero rows. This does not prove that every lock or driver wait in the
process is absent; it qualifies only the instrumented sites/windows.

H367's planning optimizer maximum is 0.338ms on real input, supporting removal
of the earlier 26ms failed-search spike. The remaining evidence favors app-side
execution/input supply over another receipt or queue-capacity wait change.
App CPU includes game, Wine, PE and synchronous Unix work, not PE alone.
Fixed-arrival sensitivity does not bound actual FPS benefit when contention
or producer arrival times change.

## System Trace and audio qualification

The first 35-second System Trace cannot be opened (`Document Missing Template
Error`) after finalization timed out. It is retained as failure evidence, not
used for attribution. The retry's five-second requested capture finalizes and
exports its TOC, thread states and raw time samples. Its time-profile export
fails with SIGTRAP and is truncated; no percentages are taken from it.

Raw samples contain common all-process gaps of approximately 704, 653, 587,
334 and 227ms. Thread-state intervals spanning those holes cannot establish
precise scheduler latency or long audio blocking. The exact coverage failure
mechanism remains unresolved. `states.json` aggregates are exploratory and
have not excluded these holes; they are not qualified CPU or starvation totals.

Seven unnamed game-process threads (`12283196` through `12283202`) repeatedly
sample at `0x7ff89da9d9f4`: roughly 84–86% of each thread's recorded Timer Fired
samples, with four-frame stacks. They also dominate recorded state transitions.
The address is not symbolicated. This is an ownership lead, not proof of an
X-Ray worker loop, Wine spin, or audio starvation. Another hot unnamed thread,
`12283414`, has translated addresses with no defensible module attribution.

The supply trace identifies app `12282986`, replay `12283437`, and encode
`12283424`. System Trace names audio threads `wine_dsound_mixer` (`12283413`),
`audio_client_timer` (`12283412`) and the client IO thread (`12283410`). Thread
creation proximity does not identify other threads as audio workers.

CoreAudio logs contain `ClientHALIODurationExceededBudget` /
`SafetyViolationOccurred`, but only near exit: approximately 23:14:12–13 before
the first app exits at 23:14:14, and 23:22:06 before the retry exits at 23:22:08.
These do not demonstrate the reported in-game audio interruption. No user
timestamp for an audible interruption was obtained.

## Clock and identity limits

The diagnostic supervisor uses Python `time.monotonic_ns()` (`mach_absolute_time`
on this host); supply uses POSIX `CLOCK_MONOTONIC`. A direct check found an
approximately 21.5-second epoch difference. Do not directly subtract these
timestamps. Supervisor realtime observations provide approximate brackets,
not an exact source-to-System-Trace join.

Supply Present IDs are one-based, while `encodeChunk`'s frame-log counter is
zero-based and independent. The analyzer's numeric frame-log selection is a
scene sanity check, not an exact Present identity join. Original artifacts
are preserved rather than silently relabeled.

## Selected next gate

1. Obtain a short, process-scoped sample and image map for the primary thread
   and seven repeated-PC threads. Resolve game/Wine/PE/synchronous Unix owners
   before proposing a spin, affinity, or encode-parallelism change.
2. Use a same-clock anchor and explicit frame/Present mapping, reject trace
   intervals with missing coverage, and record an audible-event timestamp.
   Correlate actual audio underrun evidence rather than teardown-only logs.
3. Only after ownership is established, compare a bounded change under
   observer-OFF conditions with unchanged rendering/lifetime contracts.

The supply investigation and next-axis selection are complete; exact audio
correlation and symbol-qualified app/worker attribution remain open.
