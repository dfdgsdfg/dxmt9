---
domain: present-pacing
workload: bounded query range owner fixture
title: "H410: Bounded query range ownership and cancellation"
type: evidence
status: native-owner-complete-production-integration-open
updated: 2026-09-29
source: src/dxmt9/render/query_scope_range_owner.hpp; tests/native/backend/query_scope_range_owner_spec.cpp; traces/query-range-owner-h410/{native-testlog.txt,tla.log}
related: present-pacing-query-range-prototype.409.md; ../../../specs/backend/query-scope-ranges.md
---

# H410: Own membership and deferred END together

Implements a candidate move-only bounded metadata owner using H409's reducer.
Fixed arrays retain scope snapshots, command ranges and per-END offsets with
initializer prefixes. Caller mutations of the input scope do not change owned
membership. Equal sets intern independent of input order; distinct storage
claiming the same object/issue/epoch refuses before mutation. Reissue identities
remain separate. Scope/range/END capacities are independent from command count.

Admission failure retains no new references and leaves the draft unchanged.
The first END allocates one shared state; later ENDs reuse it. Exception/null
allocation injection preserves both existing metadata and output receipt.
Only sealed/published owners expose const borrowed views; they must outlive
the consumer and cannot move while borrowed. This is not a linear borrow proof.

Writing/Sealed owners cancel on abandonment. Cancellation is retained with the
EndMarker in a typed receipt, preventing indefinite unbound Pending after
destruction. Published owners cannot roll back. Actual queue failure still
uses the queue terminal frontier. The receipt only classifies fence readiness;
GetData must separately check result failure/outstanding contributions.

## Verification

Focused native Meson tests pass 3/3: query-scope-range-owner, query-scope-range,
query-end-marker. Owner tests cover 15,625 six-operation lifecycle schedules
against an independent phase model, plus move invalidation, scope alias mutation,
reference release, every metadata capacity, invalid/duplicate identity, END
allocation exception/null failure, shared END allocation, initializer waiting,
stale/double publication binding and cancellation. H409's 4,096 value-refinement
cases remain passing. No compiler warnings in the focused target build.

The existing full TLA inventory rerun exits 0 (65 safety models, five progress
configurations, 157 expected counterexamples). No new concurrent owner/queue
composition model was added. The exhaustive owner test is sequential lifecycle
refinement; it does not prove cross-thread integration or Metal behavior.

## Integration boundary

This owner is not attached to CommandQueue or Metal. `bindPublished` accepts a
sequence only at the documented post-publication seam; it cannot independently
prove an actual queue publication. Command bytes are not owned here: the queue
must compose physical payload admission atomically with metadata admission.
Typed cancellation receipts must be connected without dropping cancellation
authority when adapting the existing polling path.

No new runtime policy, ABI/default change, GPU oracle or performance run. Fixed
array footprint/initialization and membership scans are unpriced. CPU savings
cannot be inferred from fewer modeled boundaries. Next integrate a real owned
source consumer and queue progress adapter under a deterministic fixture before
changing production admission. Range-aware Metal, composed temporal refinement,
GPU exact, same-input CPU comparison and wild qualification remain open.
