---
type: "Spec"
title: "Shared Diagnostic Consumers"
description: "Common bounded output for the remaining acquired diagnostic families."
tags: [specs, tracing, observations, provenance]
---

# Shared Diagnostic Consumers

Implements R-HARN-TRACE-15.1–15.6 and extends
[shared snapshots](shared-snapshots.md). Acquisition belongs to the existing
observer. The trace consumer never repeats polling, sampling, memory queries,
percentile calculations or DAG construction.

| Family | Shared boundary | Meaning |
|---|---|---|
| Query timeline/split/core/drain/poll | Acquired diagnostic report | Report observation; original IDs and populations retained |
| Process perf counters | Existing table iteration and breakdown rows | Exact integer values before display conversion; percentile window retained |
| Process memory | Existing successful/failed OS sample | Sample result; no new map walk |
| CPU supply and wait | Existing endpoint/row acquisition | Original endpoint with bounded native clock bracket; full predicate/identity metadata |
| PE sampler/recorder | Existing report output | Report receipt, cumulative sampled distribution and explicit omissions |
| Queue and signposts | Existing snapshot and interval sites | Same sequence/signpost identity; no claim of external capture coverage |
| Renderer | Existing DAG and diagnostic serialization | Structural evidence and source artifact references; no second renderer traversal |

## Record ownership and representation

The SDK-independent output facade accepts owned bounded POD records. Typed
fields retain unsigned integers exactly; formatted diagnostic reports retain
their original bytes and format/schema identity instead of reparsing external
log files. Reports exceeding one record use bounded, ordered fragments admitted
as one generation-qualified batch. Overflow or truncation cannot become a
successful complete report. Compatibility output remains independent.

Native events expose source, source-local record ordinal, report/field schema,
original value and timestamp semantics. Cold normalization reconstructs report
fragments and materializes structured rows in observations.sqlite; unknown or
ambiguous report tokens remain text, never guessed numeric units. Large
structural metadata and opaque artifacts retain hashes and source links.
Report fields with no established aggregation retain source-defined semantics;
they are not silently treated as frame deltas or interval samples.

The PE bridge copies bounded pointer-free report records through the generated
dispatch schema. It preserves guest process/thread incarnation and captures
session generation before transfer. The unix receiver validates size/version,
lengths and generation before native admission. Report delivery does not invoke
the sampler and must preserve LastError. No new per-draw bridge operation is
introduced when diagnostic acquisition is disabled.

## Clocks, lifecycle and failure

CPU/wait endpoints capture native brackets around the existing source clock
read. Native events use qualified midpoint observations with retained source
clock value and bracket uncertainty. Missing or excessive brackets cannot be
silently placed at export time. Endpoints and result-time counters remain
separate observations. Wait identity joins preserve owner/queue/sequence/query
and frame-window fields. Native slices require paired qualified endpoints;
unpaired endpoints remain explicit evidence.

Consumers share the existing trace admission gate, byte budget, generation and
drain/seal protocol. No consumer owns production query, queue or resource
lifetime. Legacy report gates and expensive collector selection remain in
force; overview alone does not enable them. Closing trace admission never
suppresses compatibility output. Artifacts/SQLite inherit immutable attempt and
test provenance. External Instruments, Processor Trace and Xcode captures stay
separate acquisitions and require their own identity/clock qualification.

## Verification

Fixtures exercise each producer boundary, exact large integer/ID round trips,
fragment reassembly, malformed/stale PE records, bounded exhaustion, failed
clock brackets and preserved legacy output. SDK-off/on builds and pinned native
Trace Processor verify annotations and SQL. Bridge changes require x86/x64 PE
and unix build evidence. Existing lifecycle model checks remain applicable;
new output must not change rendering state or retirement authority. Native
fixtures do not establish live Wine overhead or external capture qualification.
