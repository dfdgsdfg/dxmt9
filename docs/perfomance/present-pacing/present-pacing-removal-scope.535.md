---
domain: present-pacing
workload: STALKER CoP retained late-window diagnostics and value-only native fixtures
title: "Chunk replay, not StateBlock count, leads the next removal investigation"
type: evidence
status: removal-scope-narrowed-no-runtime-optimization
updated: 2026-10-14
source: traces/stkcop-removal-scope-h535/{analyze.py,analysis.json,prefix_counterexample.cpp,run_native.py,native-build.log,native-receipt.json,append_prefix.cpp,run_append.py,append-receipt.json,run_append_current.py,append-current-build.log,append-current-receipt.json}; traces/stkcop-source-self-live-h515/{analysis.json,run1/receipt.json,run1/self.bin}; traces/stkcop-validation-callers-h507/{calls.json,summary.json}
related: present-pacing-boundary-comparison.534.md; present-pacing-validation-callers.507.md; present-pacing-validation-live.509.md; present-pacing-source-self-live.515.md
---

# Separate high call count from high measured work

H534 does not establish a canonical removal budget. This follow-up performs no
new game run and adds no runtime observer: it narrows the next mechanism using
retained raw data and exercises the current C++ predicates/assembler directly.

H515's dense, unsampled worker trace contains the following **fully enclosed
measured scope sums** between its spatial knots 1 and 6, covering 116 observed
completed frames. Three boundary-crossing pairs are excluded rather than
interpolated. Raw schema/identity/pairing and the original analysis hashes are
rechecked before aggregation; all five components are retained separately.

| Scope | Enclosed pairs | Recorded CPU sum / observed frame |
|---|---:|---:|
| StateBlock replay | 27,382 | 1.421 ms |
| Ordinary chunk replay | 28,414 | 29.544 ms |
| Encode | 699 | 31.226 ms |

These are **H515 observer-inclusive sums**, not H534 values, fresh default
CPU bounds, exhaustive whole-thread totals, or removable work. In particular,
many very small StateBlock scopes contain a substantial observer fraction;
no synthetic timer tax is subtracted. Different threads/runs are not summed.
The evidence selects ordinary chunk replay over a StateBlock optimization
based solely on its high call count. It does not transfer an exclusive
product attribution through the common observers.

## The prepare scan repeats a large prefix

Joining H507's actual `reserve` and `prepare` call records by adjacent call,
same raw event and same Slot matches **all 199 prepare transactions**, with no
unmatched prepare. Their command visits separate as follows:

| Prepare visitation | Command visits |
|---|---:|
| Already present at reserve | 75,300 |
| Added after reserve | 392 |
| Full prepare scan | 75,692 |

Thus 99.48% of these *prepare command visits* revisit the old prefix. This is
not 99.48% of Replay CPU, draw visits, or FPS. DrawRun payload validation also
walks parameters; stored parameter extents are not substituted for actual
parameter visits. H508 removed the distinct adjacent admission-to-reserve
scan, and H509 failed its live economy gate. Neither result is relabeled or
rerun here. Prepare suffix validation is a different, currently unimplemented
candidate whose savings have not been measured.

## Why topology is not validation authority

The private ARM64 fixture calls the real inline
`ChunkSlot::commandPayloadsInRange` from the current header. Nine cases retain
every vector's size, capacity and allocation address but invalidate a uniform
generation/hash, command payload index, state index, parameter extent or
payload range. One clears/repopulates the same storage before the stale-handle
change. The actual validator rejects all nine. The fixture has one retained
initializer-brace warning; it is not described as warning-clean production
code or an implemented cache.

Consequently a pointer/count/capacity cache is not sufficient. A correct
generation token helps with reuse, but cannot by itself prove absence of
in-place writes within the same generation. Existing public vectors and
mutable methods still permit such writes.

A legal writer also matters: `src/dxmt9/dxmt9_backend_types.hpp`,
`appendCompatibleDrawRunEntryUnchecked`, extends the last existing DrawRun's
parameter count and payload size without adding a command header. A prefix
certificate cannot cross arbitrary compatibility appends based on a command
cursor alone. PSO/depth-stencil patch methods mutate other old-record fields;
those fields are not read by this particular range validator, so they should
not be confused with its validation dependencies.

## Actual transaction-local preservation evidence

The second fixture uses the actual `TransactionalChunkSlotAssembler` to build
and commit an initial draw prefix, then exercises 16 combinations of subsequent
draws, explicit run cuts and an intervening Clear. It compares all old fields
read by the command-range validator, including uniform identities, through
append, full `prepare`, and rollback. All 16 preserve the old projection and
restore its extents; the full validator still accepts it.

To remove cached-library/TU ambiguity, a second executable recompiles the
**current `dxmt9_source_payload.cpp`** alongside the fixture. `nm` confirms
`TransactionalChunkSlotAssembler::prepare` is defined in that executable.
It passes all 16 cases with no build warnings. Runtime sources, canonical
libraries and Wine files are not modified. These bounded value cases are not
a concurrent ownership proof, exhaustive allocation-failure coverage, GPU
oracle, suffix-validator implementation, or performance comparison.

The code explains why this limited scope is promising:

- Each assembler begins with `directRunOpen_ = false`. Its first draw creates
  a new run, so later extension targets this transaction's suffix, not the old
  prefix. Coordinator appends close its run.
- Uniform interning appends semantic records or reuses their handles. Lookup
  bucket/next maintenance is separate from the fields the range validator
  reads. Rollback restores extents and rebuilds lookup tables.
- Reserve validates the old Slot before building. However the production
  build does **not** hold the queue mutex continuously: it hands off an active,
  owner-thread-pinned context. H508's uninterrupted-lock argument cannot be
  reused to justify the longer reserve-to-prepare interval.

## Required implementation boundary

The next candidate is an ephemeral **validated-prefix append capability**,
not a persistent Slot cache and not a public `skipValidation` switch:

1. Mint only after real validation and successful production admission; bind
   destination, active build, source/storage generations and prefix extents.
2. Permit only the admitted assembler's suffix writers until prepare. Bind
   their single-writer/exclusion premise to the existing Direct lifecycle;
   test foreign owner, same-address reuse, terminal/reset, rollback and
   compatibility intervention. Existing lifecycle models do not already model
   preservation of these concrete prefix fields.
3. Consume at prepare; validate every new command and its complete referenced
   payload, retain global SoA shape/range-conservation checks, and invalidate
   the capability on every terminal path. Generic/raw-value callers must
   retain full validation unless separately qualified.
4. Compare against the full validator, preserve exact late-input source/query/
   pixel gates, and price whole Replay with equal diagnostic settings before
   attempting live transfer. Visit-count reduction alone is not acceptance.

This supplies a concrete removal mechanism and its missing proof surface.
The original goal remains open: canonical/global largest-owner qualification
and an actual semantics-preserving CPU removal amount are still unproved.
