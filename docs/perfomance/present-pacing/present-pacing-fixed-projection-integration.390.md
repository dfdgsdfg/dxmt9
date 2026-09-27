---
domain: present-pacing
workload: 3DMark05 GT2 fixed Render Tape compatibility route
title: "H390: Projection-free fixed comparison in production lookup"
type: evidence
status: cpu-semantics-verified-performance-qualification-open
updated: 2026-09-27
source: traces/compat-uniform-h390/analyze.py; traces/compat-uniform-h390/{baseline,candidate}/result.json; traces/compat-uniform-h390/{baseline,candidate}/provider.stderr; traces/compat-uniform-h390/queue-disassembly.txt
related: present-pacing-fixed-projection-probe.389.md
---

# H390: Remove the fixed-comparison temporary

## Change and contract

`src/dxmt9/dxmt9_backend_types.hpp`, the full-payload overload of
`drawUniformFixedPayloadMatches`, now compares fixed members directly in the
same order as projected-value equality. The hash guard, component identity
checks, constant comparisons, lookup order and Slot-owned storage remain
unchanged. No runtime switch, witness cache, new borrow, ABI change or
allocation policy is introduced. This serves both callers of the shared
predicate, not only the compatibility lane measured here.

The reference remains `hash matches && fixed == project(full)`. This is a pure
value substitution under R-VERIF-1.5, not a temporal-state optimization; no new
TLA transition is required. Float comparison stays fieldwise, not `memcmp`.
The spec records the contract beside R-BACK-2.15.2 projection reuse.

## Verification

- Canonical x86_64 Unix build completes, including install-name fixup. Existing
  macOS 27 deprecation warnings occur in unrelated Metal fixtures.
- Focused x86 tests pass 3/3: `dxmt9-dod-replay-observer-spec`,
  `dxmt9-source-payload-spec`, `dxmt9-winemetal-install-name-audit`.
- New native fixture compares the production predicate against projection for
  eleven floating-point members, all 6×6 pairs of signed zero, finite values,
  infinity and quiet NaN, with matching/mismatching hashes: 792 cases. A real
  Slot fixed-hash collision distinguishes clip masks and exercises bucket and
  last-handle lookup. Existing full-payload collision tests also remain passing.
- Same GT2 r405 input, capture-source boundaries and compatibility route:
  baseline and candidate each complete **18 verify-mode iterations**.
  The shared `compare_verification` accepts all semantic/draw digests and
  conservation checks. Each iteration has 1,866 compatibility draws, zero
  Direct draws, and 185 submitted/completed/reclaimed sources. All uniform
  work counts match between images. `analyze.py` reproduces these checks.
- Actual compiled queue object emits the new predicate without the large
  stack reservation. The retained disassembly is from
  `dxmt9_command_queue.cpp.o`, not just H389's synthetic wrapper. This is a
  candidate-image inspection, not a preserved before/after instruction diff.

Baseline is built before the code edit; candidate is rebuilt from the same
build directory/options afterward. Receipts retain provider and loaded-image
identities. Builds/tests finish before each corresponding captured-input run.
The append observer is OFF, existing perf counters are ON.

## Cost observation, not performance acceptance

The existing lookup elapsed counter totals 17.624ms baseline and 16.409ms
candidate across 18 iterations: **0.9791 → 0.9116ms/input**, a 0.0675ms local
difference. Counts stay at 919 last hits, 73 bucket hits and 874 appends per
input. This is consistent with H389's small-candidate scale, but it includes
all iterations and verification overhead. It is not an observer-OFF,
order-reversed performance campaign. No replay CPU, FPS or STALKER improvement
is accepted from these values.

Production implementation and CPU semantic checks are complete. Fresh GPU
exact/wild integration and controlled performance qualification remain open.
The separate fixed-component linear-search cost is unchanged; that remains
the next distinct owner, rather than expanding this small substitution into
cross-draw caching without evidence.
