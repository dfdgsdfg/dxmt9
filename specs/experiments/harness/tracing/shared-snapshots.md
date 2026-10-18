---
type: "Spec"
title: "Shared Query and Counter Snapshots"
description: "Reuse acquired diagnostic ledgers and frame/append snapshots across consumers."
tags: [specs, tracing, observations, counters]
---

# Shared Query and Counter Snapshots

Implements R-HARN-TRACE-14.1–14.5, extending [shared acquisition](shared-acquisition.md).
This increment covers query backend/receipt ledgers, the complete existing
`perf::CounterSnapshot` and compatibility-append `Snapshot`. The frame subset
does not contain all process perf counters or histograms.

## Ownership and flow

Existing observers own acquisition, gates, sampling, synchronization and legacy
reporting. SDK-independent value projections feed bounded native records;
deferred export and normalization feed Perfetto and observations.sqlite.
The receipt observer belongs to the unix provider despite its `src/d3d9/`
location. No new PE transport or renderer/query policy is introduced.

| Source | Acquired input | Projection cadence | Semantics |
|---|---|---|---|
| query-backend | Locked totals | First call, existing periodic/final reports | Cumulative census and valid sampled elapsed sums/maxima |
| query-receipt | Locked totals | First call, existing periodic/final reports | Cumulative route/kind/result census and sampled result/phase totals |
| perf-frame | Existing previous/current CounterSnapshot | Opt-in frame report | Nonnegative deltas; latest GPU identity fields remain latest |
| compat-append | Existing Snapshot result | Explicit snapshot consumers and enabled frame reports | Cumulative phase calls/elapsed totals; mode 1/2 preserved |

Query periodic reports retain their existing cap. A trace starting after the
first call may contain no snapshot before the next periodic report. Final
reports after trace closure reach only the legacy consumer. Append acquisition
remains opt-in; frame reports pull an append snapshot only when enabled. No
per-poll or per-append trace events are introduced.

## Record contract

Stable numeric fields resolve source/name/unit/aggregation during cold export.
Sample and invalid-clock populations accompany elapsed totals; zero alone does
not prove measured zero. Relaxed multi-counter snapshots are not atomic
transactions. Regressing frame counters clamp to zero like the compatibility
report and retain a regression census. Latest resource/PSO IDs must preserve
exact unsigned values without signed-counter truncation.

Each projection captures one generation, native timestamp and source-local
ordinal reused by all fields. A bounded batch stages all fields and admits them
under one writer lease; closure, filtering or capacity failure cannot retain a
partial snapshot. Numeric overflow records generation-qualified loss and drops
the snapshot. Failure never changes input/legacy aggregates. Query uses wait;
frame/append use encode. Overview does not enable diagnostic acquisition.

Native and normalized metadata include acquisition_version, observation_source,
observation_record, observation_timestamp_kind=snapshot-observed, observer mode,
timing stride and field aggregation. Ordinals identify projections, not log rows.
Existing immutable SQL export retains source hashes, attempt and test provenance.

## Verification

Deterministic ledger/snapshot fixtures cover modes, invalid populations,
counter regression, exact identity and source parity. SDK-off/on tests exercise
production projection sites, stale generations, filtering and bounded exhaustion.
Pinned Trace Processor compares values and annotations against normalized input;
SQLite checks preserve metadata and test provenance. Live Wine overhead is a
separate evidence gate.
