---
type: "Design"
title: "Independent Collectors and Shared Observation Records"
description: "Complete acquisition sharing without legacy output dependencies."
tags: [specs, experiments, tracing, observations]
---

# Independent Collectors and Shared Observation Records

R-HARN-TRACE-17 complements [bounded transport](asynchronous-drain.md).
R-HARN-TRACE-15 report fan-out is compatible behavior, not proof that acquisition
is independent of a legacy file or that individual samples are available.

## Ownership and selection

The existing collector owns acquisition, windows, sample cadence and populations.
Its consumer selection is independent: legacy-only, trace-only, or both. Explicit
detail selection remains required; the default overview never enables expensive
samplers, memory map walks or renderer traversals. Failure to open a legacy CSV
must not disable a selected trace consumer. SDK-off legacy behavior is preserved.
Trace-only selection without an available trace session must fail visibly rather
than create an apparently successful empty file.

CPU/wait producers copy their existing row and endpoint brackets into bounded
owned records. No trace-only formatting runs while holding a production mutex.
Cold consumers derive legacy-compatible fields and native endpoints from that
copy, preserving failed brackets and the difference between wall and CPU time.
The full perf report publishes an immutable all-or-nothing trace batch from the
same loads and percentile evaluations used by the compatibility report.

## Raw samples and semantic reports

The PE sampler publishes a versioned pointer-free sample record containing PC,
sample ordinal, target thread incarnation, source clock frequency and bracketing
times, outcome and population counters. Copy context while suspended, resume the
target before bridge/output work, and preserve LastError. Native receipt time
is a separate field; unqualified QPC samples remain on their declared source
clock and must not become native-time slices. Suspend/context/resume failures,
overflow and omitted samples remain queryable. The sampler performs no second
SuspendThread/GetThreadContext acquisition for an additional consumer.

The binary payload is encoding 4 with explicit byte length and little-endian
unsigned fields: `cpu-supply-row-v2`, `wait-row-v2`, `pe-pc-sample-v1` and
`pe-pc-population-v1`. Encoding 5 contains individual endpoint brackets.
Cold native export supplies named exact integer annotations; SQLite retains
both original bytes and typed fields. PE population summaries are attempted
after sampler join, without an extra sample. Resume failure prevents further
bridge/output work; legacy snapshots retain its failure/omission counters.
Population omissions and native transport losses are separate counts. A closed
session rejects raw samples rather than counting them as accepted.

Query and queue reports may reuse bytes already required by a legacy consumer;
structured facts must have source/schema/record identity and original observation
semantics independent of log-level filtering. Do not claim report timestamps are
original query execution times. Signposts and external Instruments captures keep
separate acquisition and qualified joins.

## Structural renderer evidence

DAG, framegraph, draw/encoder, PSO and resource diagnostics project bounded owned
structural records at their existing observation site. Native source keys link
to directly imported SQLite rows without requiring a successful intermediate
JSON/CSV file. Preserve field order, exact integers, record identity, completeness
and schema. Legacy exports are optional consumers of the same acquired data;
images, shader source, binary captures and control/receipt files remain artifacts.
No worker may retain borrowed renderer pointers or traverse mutable state later.

## Verification and provenance

Tests cover legacy-only/trace-only/dual consumers, unwritable legacy paths,
SDK-off, bounded exhaustion, stale generations, source mutation after publication,
raw PE failure/population fields, structural imports without files and exact
native/SQLite values. Experiment and conformance provenance freezes collector
selection and transport mode/budgets and retains losses and incomplete captures.
R-HARN-TRACE-16 ownership/model and matched overhead requirements apply. Record
implementation and measured workload qualification separately in the gap/evidence
matrix; fixture success alone does not establish live Wine overhead.
