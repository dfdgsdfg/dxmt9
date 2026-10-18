---
domain: present-pacing
workload: Native/Rosetta CPU-counter preflight and archived STALKER H509/H510
title: "External CPU counters do not provide instantaneous endpoint enclosures"
type: evidence
status: endpoint-freshness-falsified-owner-and-removal-unresolved
updated: 2026-10-14
source: traces/stkcop-cpu-counter-preflight-h512/{probe.c,run.py,receipt.json,arm64.csv,x86_64.csv,arm64-build.txt,x86_64-build.txt,audit_archives.py,archive-audit.json}; traces/stkcop-validation-live-h509/B1/{receipt.json,progress.jsonl}
related: present-pacing-current-thread-owner.510.md; present-pacing-validation-live.509.md; present-pacing-guest-attribution-feasibility.511.md; present-pacing-root-game.477.md
---

# Correct the endpoint premise before another ownership measurement

The previous CPU reports treated a read of `proc_threadinfo` user/system totals
as a counter value sampled during that API call, allowing only 2 us for rounding.
**That freshness premise is false on the current host.** The stored counter
arithmetic remains reproducible, but its very narrow ranges are not certified
enclosures of CPU actually executed between the selected camera endpoints.

This matters before constructing a deterministic non-renderer lower bound from
inactive-root intervals: deferred accounting can arrive during a later interval
and must not be charged to whichever root state is observed then.

## Bounded reproduction, without Wine or renderer changes

One two-second ARM64 process and one two-second x86_64/Rosetta process run a
continuous CPU worker. A separate thread polls that worker by its numeric
`pthread_threadid_np` identity using `PROC_PIDTHREADID64INFO`. Every call has
native monotonic before/after timestamps; raw user/system values, returned
structure size and run state are retained. Collection is preallocated; output
is emitted after the worker finishes. No game, GPU, profiler, thread suspension,
renderer rebuild or shared Wine staging is involved.

For consecutive non-overlapping reads A and B, an instantaneous cumulative CPU
counter must satisfy:

`CPU(B) - CPU(A) <= B.after - A.before + rounding allowance`.

A single thread cannot execute more CPU than the enclosing wall interval.
Both architectures repeatedly violate this necessary condition:

| Probe | Reads | Violating adjacent pairs | Example CPU increase | Enclosing wall interval |
|---|---:|---:|---:|---:|
| ARM64 | 5,252 | 517 | 10.071 ms | 0.387416 ms |
| x86_64/Rosetta | 5,239 | 486 | 10.000 ms | 0.341042 ms |

The host is macOS 27.0.1 build 26A434, Darwin 27.0.0 / XNU 13432.1.9.
Both raw series remain monotone; each reports approximately 1.996 CPU seconds
over approximately 2.000 wall seconds. Monotonicity and plausible long-window
totals therefore do not prove instantaneous freshness. Independent `awk`
reduction reproduces both violation counts and maximum excesses. Neither the
largest observed jump nor its excess is a proven worst-case staleness bound.
Do not replace the old 2 us allowance with an invented 10 ms allowance.

## The issue is also present in an archived real game run

`audit_archives.py` checks all non-overlapping neighboring native snapshots in
H509 A1/B1/B2/A2 and H510 A1, including endpoint before/after snapshots. It keeps
the process birth identity, excludes zero/error handles, and uses the entire
census time envelope rather than an artificially narrow per-thread interval.

H509 B1 has three necessary-condition violations. The named X-Ray Secondary
thread reports **0.444 ms CPU over at most 0.289667 ms wall**, exceeding even
the extra 2 us allowance by 0.152333 ms. Two further violations are on an
unnamed handle. Original raw artifacts and their hashes are preserved.

No violation is found in the other four sparse archives. That does not qualify
their endpoint freshness: sparse reads may simply hide delayed updates. The
standalone numeric-TID selector and the archive's opaque-handle selector both
return the same `proc_threadinfo` time fields; the live archive independently
demonstrates the problem without assuming those two lookups are identical.

## Consequences for the active conclusion

- H510's approximately 52.4/31.6/31.4 ms per frame remain **reported-counter
  differences** for Primary/Encode/Replay. Their displayed intervals are not
  certified true-CPU bounds. The 0.206--0.223 ms Encode-over-Replay ordering and
  >20.774947 ms Primary exclusion threshold are not established bounds.
- H509's stored 0.505 ms mean difference and 1.090 ms A/A difference remain
  reproducible arithmetic. Its narrow pair bounds and regression guard lack
  endpoint-freshness qualification. The outcome stays **no promotion / live
  economy unproved**; this finding does not rescue the candidate or authorize
  repeating its closed campaign.
- Same-process guest/TEB identity evidence, camera selection, restored hashes,
  original game results and H508's separate offline comparison are not erased.
  This counter test does not revalidate other clock or timing mechanisms.

The next measurement prerequisite is an accounting boundary with a verified
freshness contract, such as coarse target-thread self-reporting or qualified
scheduler accounting. It must be tested against running/idle transitions and
cross-thread reads before pricing subinterval CPU. External marker observations
alone cannot repair a stale CPU counter, and another per-call clock campaign
is not justified. The largest default dxmt9 owner and default-qualified
removal amount remain open. No runtime optimization or commit is made here.
