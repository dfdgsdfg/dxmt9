---
domain: present-pacing
workload: STALKER H502/r5 original captured input
title: "H637 — binding-run churn repeats unchanged owned snapshots"
type: evidence
status: captured-mechanism-qualified-removal-and-live-transfer-open
updated: 2026-10-16
source: traces/stkcop-run-sharing-h637/{contract.md,census.hpp,native.cpp,build.py,build.json,run.py,runs.json,analyze.py,analysis.json,verify.py,verification.json}; traces/stkcop-run-sharing-h637/{O0,N0,N1,O1}.log; traces/stkcop-run-sharing-h637/{N0,N1}.csv
related: present-pacing-minimal-owner.636.md; present-pacing-owned-run-local.600.md; present-pacing-replay-cache-live.618.md; present-pacing-override-projection.627.md
---

# H637 — separate snapshot ownership from binding-run identity

The captured production path creates 2,343 compatibility runs. Of these,
**1,617 (69.0%)** have the same nine generations and all raw stream strides as
the immediately preceding run in the same publication batch. Every such row
also compares equal under the production hot-state, shader-layout and uniform
value comparators. They nevertheless start a new run because the full binding
identity differs. Both fixed observed executions reproduce these counts.

This is new evidence of redundant snapshot materialization, not measured CPU
removal. No optimization or live-game change is installed. H636's overlapping
default Replay/Encode ranking and the original global-owner objective remain
open. Captured occurrence rates must not be multiplied by historical live
costs to invent a removable budget.

## Concrete production mechanism

`src/d3d9/core_draw.cpp`, `Device::submitCompatibilityReplayDrawBatched`, uses
`CompatibilityDrawBatchIdentity` to decide when to start a new run. The
identity in `include/dxmt9/core_compat_draw_batch.hpp` combines state generations
with stream/index/alpha override fields. A change to binding-only identity can
leave the binding-agnostic cache intact while requiring a distinct draw run.

`include/dxmt9/core_snapshots.hpp`, `CompatibilityBatchRun`, embeds the full
`CanonicalDrawState` and `DrawUniformPayload` alongside the binding override
and draw range. Starting a binding-distinct run copies these large values even
when the cache values have not changed. `flushCompatibilityReplayDrawBatch`
later constructs borrowed `DrawRunBatchEntry` views for all owned runs.

This differs from H597–H600: those remove intermediate construction copies but
still construct a full owned snapshot for every run. It also differs from
H627's later Encode binding projection. Their failed CPU screens remain closed.

## Observation and qualifications

One private `core_draw.cpp` object adds two diagnostic calls: after draw
admission/before construction, and on nonempty flush. All production effects
remain unchanged. No shared class layout, PE ABI, product source, canonical
binary or Wine staging changes. Mechanical removal of the include and two
calls restores the exact current production source; archive verification
allows only `core_draw.cpp.o` to change.

The observer records scalar rows in a fixed 65,536-entry buffer and writes the
file at process exit. It rejects owner/thread drift or overflow. It compares
values only for new runs with an existing predecessor in that batch and never
retains cache references. This is an intrusive counting diagnostic, not a
timing instrument. There is no claim that thread checks, equality comparisons,
storage, the disabled skeleton or exit logging are free.

The conservative key is all nine generations plus all raw stream strides,
including unbound streams. Binding-only changes are excluded from that key,
but no generation field is omitted. Debug snapshots are explicitly excluded
from sharing: they remain draw/run-specific. This census is not a universal
proof that the key permits reuse under every state transition.

Both arm64 and x86_64 native key-classification tests pass 43 cases each.
The parser rejects 12 corrupt streams. These tests qualify the observation;
they do not replace a bounded ownership/refinement model for a future reuse
implementation.

Xcode27.0, Metal32023.921 and Apple M1 are verified. Fixed original-input order
is O0,N0,N1,O1 with no replacements. O disables only this new census; N enables
it. All four executions use the same private images and original H502/r5 bundle.
They preserve:

- Source/output pixel SHA-256
  `1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
- Seven mapped sources, 670 created/released wrappers and normal restoration.
- Nine query issues, five polls, four results and five readiness checks.

This is the production-counter query oracle, not actual Metal occlusion.
There is no additional wild run or GUI operation. Independent verification
re-reads 5,824 raw rows without importing the analyzer, checks batch/run/draw
conservation and effects, and finds no remaining runtime processes.

## Identical census in N0 and N1

| Quantity | Count per capture |
|---|---:|
| Compatibility draws | 2,828 |
| Publication batches | 84 |
| New owned runs | 2,343 |
| New runs with a predecessor in the same batch | 2,259 |
| Same conservative key | 1,617 |
| Same key and equal hot/layout/uniform values | 1,617 |
| Same-key value counterexamples | 0 |
| Equal hot/layout/uniform values regardless of key | 1,619 |
| Equal hot state alone | 1,619 |
| Equal shader layout alone | 2,145 |
| Equal uniform payload alone | 1,830 |
| Capacity-triggered cuts | 0 |

The two value-equal but key-different runs are not candidates under this
conservative rule. Cross-batch reuse, global interning and pointer reuse are
also excluded. If all admitted same-key links could share their owned base,
this particular input would need 726 base snapshots instead of 2,343 while
retaining 2,343 run descriptors. That is a representation-count opportunity,
not a CPU or memory measurement of an implemented candidate.

## Next implementation gate

Keep source-local snapshot owners separate from per-run binding/debug/range
descriptors. Run descriptors refer to a stable owner index; queue consumption
must finish before those owners are retired. Preserve draw/run order, capacity
cuts, transaction failure, resource references, debug values and query effects.
Do not borrow the mutable device cache or silently reuse across a flush.

`DrawRunBatchEntry` currently accepts a whole `CanonicalDrawState` pointer,
which includes per-run debug values. Sharing that whole object without changing
the consumer view would be incorrect. The ownership/view boundary therefore
needs an explicit design and consistent rebuild, not a pointer substitution
inside one translation unit. Add generation/stride/ABA/failure counterexamples
and native model binding before testing a private implementation.

Only after same-input effect verification and repeated CPU comparison may the
candidate be tested in the default late-live path. Neither a positive removable
CPU amount nor unique default single-thread maximum is established here.
