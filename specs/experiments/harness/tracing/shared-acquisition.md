---
type: "Spec"
title: "Shared Observation Acquisition"
tags: [specs, tracing, gpu, observations]
---

# Shared Observation Acquisition

Implements R-HARN-TRACE-13.1–13.5 in [requirements](requirements.md).

## Ownership

Acquisition is SDK-independent. A result is immutable after publication; output
adapters do not remeasure it. CPU events remain bounded POD records. GPU results
are cached on the actual retained Metal object, avoiding address-keyed global
registries and stale handle reuse. A command buffer owns one endpoint result;
a counter buffer owns bounded single-use adjacent timestamp pairs. Neither
source stores a reference back to its Metal owner.

Existing encoder diagnostics own attachment allocation and production retention.
When both consumers request encoder timing, the trace reserves a generation-
qualified ticket that retains the existing buffer and indices instead of adding
a second attachment. Trace-only capture retains its own bounded buffer. Both
resolvers consume the same cached pair after their completion evidence. Closing
a trace cannot release the legacy owner; closing a legacy encode session cannot
release a trace ticket's retained buffer. Existing quarantine gates still apply.

Source storage is installed only for an admitted trace consumer, before GPU
submission. Legacy-only capture keeps its direct command-buffer read and bulk
counter resolve. A copied Objective-C block owns the C++ source storage without
registering duplicate Objective-C classes when native fixtures link the private
library more than once. Source publication serializes concurrent readers on the
diagnostic path. It must never wait for GPU completion or change command submission. Per-pair resolution
can cost more CPU than the old legacy bulk resolve; sharing is a correctness and
redundancy change, not an unmeasured performance improvement.

## CPU and wait consumers

Legacy gates and frame windows remain authoritative for detailed acquisition.
Adapters reuse the acquired CPU/wait record and retain its source row key. Native
CPU queries are not repeated. Wait endpoints retain acquisition and CV semantics;
the native projection is a counter at result observation, not a reconstructed
wait slice. It captures the trace generation at wait entry to reject late old
work after rearm. CPU counters are timestamped immediately before the existing
native CPU query. Original CLOCK_MONOTONIC/steady-clock endpoints stay in the
supplemental row; the projection does not declare a cross-clock mapping. Missing
CPU queries do not become measured zero.

Raw compatibility output is retained for strict independent analyzers. The
existing normalization/store pipeline imports it into observations.sqlite.
Source acquisition version and selected consumers accompany the evidence.
Trace admission, capacity and closure remain independent of legacy coverage.
The raw header records acquisition_version, trace_attempted and trace_accepted;
filtered categories are not accepted. Native counter annotations and normalized
SQL arguments carry observation_source and the one-based observation_record.
These row ordinals are join candidates only for complete, zero-incomplete source
exports from the proven same process/attempt and immutable source artifact.
Unqualified clocks or process identity are not repaired by matching row numbers.
Detailed phase and wait-state rows remain in SQL; native counters supplement the
existing engine timeline. No collector knob or parser is retired here.

## Acceptance

Exercise concurrent consumers against a counting source; require one physical
read and identical endpoints, including invalid results. Exercise actual Metal
shared attachments with legacy-only, trace-only and simultaneous consumers.
Close the trace before GPU completion and verify late resolve, retained ownership
and rejection of stale tickets. Run SDK-off/on focused tests and lifecycle TLC.
Native/SQLite round trips must retain source keys and acquisition metadata.
Matched Wine workload controls are required before claiming reduced overhead.
