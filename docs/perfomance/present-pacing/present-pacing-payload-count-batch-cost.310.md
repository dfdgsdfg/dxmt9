---
domain: present-pacing
workload: Native queue refinement; captured-buffer batch sensitivity
title: "Single-count payload settlement and bounded batch-cost investigation"
type: evidence
status: native-verified-candidate
updated: 2026-09-09
source: src/dxmt9/dxmt9_draw_payload_arithmetic.hpp; tests/native/backend/queue_lock_scope_refinement_spec.cpp; build/meson-logs/lock-scope-single-count-native.txt; build/meson-logs/lock-scope-single-count-tla.txt; build-x86_64-builtin/meson-logs/lock-scope-single-count-runtime.txt; experiments/output/lock-scope-H310-20260909/batch-cost
related: present-pacing-lock-scope-wild.309.md; present-pacing-lock-scope-refinement.308.md
---

# Single-count payload settlement and batch cost

Later wild attribution: [H311](present-pacing-lock-scope-attribution.311.md)
finds both this provider and the exact pre-fix H309 provider near 18 FPS in
06 GT2. The recovery is not an isolated FPS benefit of the fix below.

This is a follow-up implementation overlay on `1f3f0358`, not a new GT2 FPS
result. The combined lock-scope candidate remains default-off following
[H309](present-pacing-lock-scope-wild.309.md). No batch policy, ownership,
flush fence, lock scope or renderer default is changed here.

## Removed duplicate work

H308's candidate counted immutable input before acquiring the queue mutex,
then counted every draw again when live binding capture changed any payload
span. `submitDrawRunBatch` now settles the final count in constant time:

`final bytes = prepared bytes + newly appended snapshots * sizeof(DrawBindingSnapshot)`.

The before/after cursor is source-local within the retained scratch vector.
`snapshotDrawRunBindingPayloads` only fills empty snapshot fields and preserves
other roles, so an already-captured field is never counted twice. The pure
`drawRunPayloadBytesAfterBindingSnapshotCapture` helper checks subtraction,
multiplication and addition, preserving the original overflow sentinel. It
adds no hot-path diagnostic fields, counters, clocks or allocations.

The reference OFF path still counts its final payload once. ON still performs
the initial count and the necessary live-capture walk; the removed work is the
second full count, not snapshot construction or final Slot copying. Future
capture operations that replace/remove variable-size payload fields must not
reuse this additive contract without a new proof.

## Verification

- Native focused tests: 5/5 pass; x86_64 runtime focused tests and install-name
  audit: 6/6 pass. Native targets and the complete x86_64 runtime build pass.
- The shared arithmetic reducer passes 2,048 bounded repeated-add oracle
  cells plus unchanged, shrinking-cursor, pre-existing overflow and
  multiplication/addition limit cases.
- Actual queue OFF/ON tests cover multiple source entries, mixed existing/live
  snapshots, implicit missing payload entries and other vertex/index roles.
  Four snapshots plus eight other payload bytes have exact final arena/record
  extents, and copied role bytes and backing identities match.
- Full TLC passes in 213.84s. The model is unchanged: this patch refines pure
  byte arithmetic and does not change thread/ownership transitions. The bounded
  arithmetic oracle does not claim to enumerate every machine-size input.

No fresh wild FPS or post-change Wine pixel claim follows from these tests.
H309's prior runtime images/results remain historical evidence, not a measured
speedup for this follow-up.

## Batch-cost scope

The ignored `batch-cost/` harness uses the production `BufferUseStampBatch`
primitive with controlled unique, repeated, A/B/A and flush-boundary shapes.
This separates bookkeeping sensitivity from actual workload distribution.
It is not a new always-on observer and cannot identify GT2's mean batch size,
duplicate rate or flush-cause distribution. Those remain a separate runtime
attribution gate before assigning H309's FPS decline to batching.

### Bookkeeping sensitivity

`batch_cost.cpp` times the actual append/clear/consume primitive with stack
construction or clear/reuse, a compiler memory barrier per sequence, and an
untimed exact retained-entry oracle. There are no heap allocations, clocks or
atomics per append. Two 300,000-sequence runs per architecture follow 5,000
warmup sequences. Timed runs start after project builds/tests/TLC finish.

For x86_64/Rosetta, fresh all-unique 64 costs 1.01–1.04 microseconds per
sequence (2,016 element comparisons), versus 0.119–0.129 microseconds for 64 copies
of A (63 comparisons). Fresh singleton is about 26 ns versus 4.8–4.9 ns with
clear/reuse. These include a checksum consumer and benchmark control flow;
they are not exact production constructor costs or a GT2 histogram. The
presence/absence of a `memset` symbol alone cannot price initialization, since
the compiler may inline or eliminate stores.

### Actual Pool, including batch construction

`pool_cost.cpp` links the existing native/x86_64 runtime libraries and calls
the real scalar and batched Pool methods on null-device synthetic versioned
records. The scalar lane borrows prebuilt snapshots. The full batch lane
constructs a fresh stack batch and appends every stamp inside the timed loop;
the separate prebuilt apply-only lane is not presented as the full cost.
Each mode's logical and concrete-backing watermarks are checked individually,
outside timing. No Metal device, concurrent contender or GPU work is present.
Lock diagnostics and perf counters are explicitly disabled.

Two final runs use 100,000 sequences per row with 1,000 warmups. Scalar,
prebuilt-apply and build-plus-apply order is fixed, not randomized; repeat
variation and host scheduling remain limitations. Use only
`raw-pool-{arm64,x86_64}-final{,-r2}.csv` for the final harness, not preliminary
Pool CSVs. Compiler commands, environment and hashes are in `provenance.txt`.

| All-unique entries/sequence | x86_64 scalar ns/sequence (two runs) | x86_64 build + apply ns/sequence (two runs) |
|---:|---:|---:|
| 1 | 34.72–41.68 | 89.02–104.08 |
| 8 | 313.55–346.70 | 170.62–185.76 |
| 16 | 670.43–738.87 | 292.73–320.08 |
| 32 | 1501.24–1529.97 | 651.98–664.56 |
| 64 | 3444.71–3466.52 | 2005.06–2159.11 |

For the actual Wine execution architecture, 8–64 distinct entries amortize
the uncontended locks even after bookkeeping; singleton batching loses by
about 2.5x. Prebuilt 64-entry apply alone is 461–466 ns/sequence, materially
less than the full build-plus-apply path: omitting preparation would overstate
the gain. Arm64 has a different crossover: unique 64 is slower in the final
Pool spots, so arm64 alone must not select a policy for Rosetta Wine.

The evidence supports measuring singleton/short flush frequency and exact
batch population next, not removing every batch or changing its capacity
blindly. It neither proves that batching caused H309's FPS decline nor closes
the application's contended wait/hold-tail gate. No batch policy is changed
and no FPS improvement/default promotion is claimed.
