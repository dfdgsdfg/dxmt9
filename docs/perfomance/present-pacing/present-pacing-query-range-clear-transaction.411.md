---
domain: present-pacing
workload: native query range and real ChunkSlot Clear fixture
title: "H411: Atomic range metadata and Clear payload append"
type: evidence
status: clear-native-integration-only
updated: 2026-09-29
source: src/dxmt9/render/query_range_clear_source.hpp; tests/native/backend/query_range_clear_source_spec.cpp; traces/query-range-clear-h411/{native-testlog.txt,tla.log}
related: present-pacing-query-range-owner.410.md; ../../../specs/backend/query-scope-ranges.md
---

# H411: Compose metadata admission with real payload preparation

H399–H410 work is committed as `30ef0769`. This follow-up implements a
single-writer append transaction in the range owner and connects it to the real
`core::ChunkSlot::appendClear(ClearDesc&&)` appender in a private owned source.
It is not connected to production queue admission or a Metal range consumer.

The appender stores command headers and typed records in separate vectors.
A wrapper cannot commit metadata first and assume a later vector allocation
succeeds. `appendWithPayload` therefore admits tentative metadata, prepares
physical storage without changing logical payload lengths, and then invokes
a compile-time noexcept commit. Preparation exceptions restore the old cursor,
last extended range and new scope refs; existing END records remain untouched.

`QueryRangeClearSource` owns both the actual payload and metadata and admits
only Clear. It stages the nested rect vector, reserves commandHeaders and
clearRecords, then calls the production rvalue appender. No payload view escapes
before seal. This makes the final Clear commit allocation-free under its private
payload invariants. The callback seam is not a general transaction for arbitrary
external effects, and a throwing commit is not supported.

## Native verification

Four focused Meson tests pass: query-range-clear-source, query-scope-range-owner,
query-scope-range and query-end-marker. The Clear fixture injects failure before
rect copy, header reserve and record reserve, each with an extended existing
scope and a newly admitted scope (six cases). It verifies unchanged logical
payload/range lengths, restored scope refs and END offsets, then seals and
reads the actual surviving Clear from ChunkSlot. Successful three-command
unqueried/queried/unqueried input retains order, depths, nested rect ownership
and exact range coverage. Metadata capacity denial never invokes preparation.

Publication binding in this fixture is explicit simulated post-publication
input. The test does not create a real queue submission; Pending/completed
frontiers are supplied values. It is not a GPU oracle or a performance result.

Preparation failure may retain increased vector capacity while preserving all
logical sizes. That distinction must be priced in actual queue credit accounting.
The fixture has no queue credit, so it cannot qualify production allocation
rollback. Existing owner lifecycle/value-refinement tests remain passing.

The existing full TLA verification script exits 0, including expected failing
counterexamples. No new payload/queue concurrent refinement was added; this
does not prove the future integrated candidate or GPU behavior.

## Next integration boundary

DrawRun is not admitted by this wrapper. It has multiple state/uniform/payload
vectors and interning lookup structures; it needs its own preflight/reservation
proof, not a generic Clear-style resize rollback. Other families, queue FLUSH/
publication wiring, resource stamps, range-aware Metal execution and composed
progress/refinement remain open. No runtime selector/default, PE ABI or staged
Wine binary changes. No FPS benefit is claimed.
