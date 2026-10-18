---
domain: present-pacing
workload: STALKER H502/r5 captured compatibility batches
title: "H638 — qualified snapshot owners preserve every captured run in shadow"
type: evidence
status: private-storage-refinement-and-shadow-qualified-consumer-and-economics-open
updated: 2026-10-16
source: traces/stkcop-snapshot-owner-h638/{contract.md,owner.hpp,native.cpp,shadow.hpp,foreign.cpp,prepare_qualified.py,build.json,runs.json,verify.py,verification.json}; traces/stkcop-snapshot-owner-h638/{N0,N1}.csv; traces/stkcop-snapshot-owner-h638/qualified/{contract.md,owner.hpp,native.cpp,shadow.hpp,derivation.json,build.json,runs.json,N0.csv,N1.csv}; traces/stkcop-snapshot-owner-h638/qualified/{O0,N0,N1,O1}.log
related: present-pacing-run-sharing.637.md; present-pacing-minimal-owner.636.md
---

# H638 — 726 owners reproduce 2,343 original run descriptors

A private bounded storage implementation now owns shared base snapshots and
separate per-run debug/binding/ranges. In both corrected shadow executions,
726 base owners reproduce all 2,343 production runs across 84 batches and
2,828 draws. Every hot/layout/uniform/debug/binding/range comparison succeeds
before the original queue submission. Each batch's counts also match H637's
independently collected predecessor-key census, not only its aggregate totals.

This is an implemented **shadow storage primitive**, not a replacement of the
production consumer. The original renderer still supplies all GPU commands.
No CPU reduction, FPS gain, largest-default-thread verdict or live promotion
is established. The full objective remains open.

## Storage and refinement boundary

The authoritative corrected primitive is
`traces/stkcop-snapshot-owner-h638/qualified/owner.hpp`, `Batch`:

- `begin` reserves both owner and run vectors before activating the batch.
  Reservation failure has no logical append effects; partially gained capacity
  may be retained for retry. A zero capacity or exhausted identity fails closed.
- `append` is allocation-free after reservation for the tested qualified value
  types. Full run identity controls run creation; nine generations and all raw
  stream strides control adjacent base sharing. Base constructors own their
  data; no mutable device-cache reference is retained.
- Each run keeps its own debug, binding and draw range. Its snapshot witness
  carries provider-local owner identity, batch epoch and owner index.
- `consume` exposes synchronous const borrows in run order, then retires the
  owned storage. Callback failure or exception retires it into `Poisoned`;
  retry, restart and reentrant append/cancel cannot replay a visible prefix.
- Normal consume/cancel permits a new epoch. Cross-batch and foreign-owner
  witnesses reject. Copying and moving the storage owner are prohibited.

The native reference owns a separate base for every run. Both arm64 and x86_64
enumerate all 78,125 length-seven words over append, binding-change/append,
semantic ABA mutation, consume and cancel: 546,875 checked transitions per
architecture. Same-key binding/debug changes preserve the first-draw debug
rule. Source mutation after append does not change prior owners. Reference
counts verify retention and release rather than comparing raw pointer bytes.

Directed checks additionally cover:

- All 25 generation/stride dimensions. Each omission mutant actually uses the
  same storage algorithm with a defective key predicate and produces the wrong
  second semantic value; the real predicate preserves it.
- Both reserve allocation failures, a full-capacity rejection and no allocator
  calls in the reserved append sequence.
- Failure and exception at each of three consumer positions, with no duplicate
  retry; invalid index and epoch; foreign owner; destruction/reconstruction at
  the same address; noncopyable/nonmovable owner type.

This bounded refinement uses the actual C++ key/storage implementation rather
than an unrelated policy replica. It does not prove production generation
writers, arbitrary concurrent use, queue handoff, GPU completion or Metal
behavior. The storage is single-writer and its consumer must not retain borrows.

## Preserved counterexample and correction

The initial version checked only epoch/index and was implicitly copyable.
Two independently constructed batches both assigned epoch1/index0; one
resolved the other's token to its own unrelated value. `foreign.cpp` reproduces
this counterexample. The original four shadow controls happened to pass
because they used only one owner; they are not qualified multi-owner evidence.

The original files, binaries and runs remain unchanged. `prepare_qualified.py`
derives a new revision with a monotone provider-local owner ID and deleted
copy/move operations. ID exhaustion denies construction use instead of wrapping.
Its fresh four-run population and extended native controls pass. This identity
does not cross process or PE/Unix boundaries, and no claim of cross-provider-DSO
token interchange is made.

## Actual-input qualification

Both revisions use fixed O0,N0,N1,O1 executions, without replacing failures
or unfavorable observations. O disables shadow; N builds and checks the new
storage in addition to the existing path. Only one private core translation
unit changes. Removing its include and two calls restores the exact current
`src/d3d9/core_draw.cpp`; shared class definitions and canonical binaries are
unchanged. No game patch, Wine staging, UI or live workload is involved.

Xcode27.0, Metal32023.921 and Apple M1 pass environment checks. The Metal skill's
environment/readback workflow is applied through the existing deterministic
capture harness, not an extra profiler or gputrace recording. All eight
executions preserve original source/output SHA-256
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`,
seven mapped sources, 670 balanced wrappers, normal restoration and query
transcript9/5/4/5. The query oracle uses the production-counter policy, not
actual Metal occlusion.

GPU equality demonstrates that the observer did not alter these executions;
it does **not** demonstrate that a candidate queue consumer renders correctly,
because that consumer is not connected. Shadow equality is stronger than
count equality but still leaves the downstream integration gate open.

The independent verifier checks 79 input/image hashes, source reversal, every
batch against H637 raw rows, run effects and nine corrupt trace rejections.
No Wine or capture process remains. No performance clocks are collected; the
extra copies/comparisons make shadow timing unsuitable for CPU savings claims.

## Remaining consumer boundary

`DrawRunBatchEntry` currently borrows a whole `CanonicalDrawState`, including
per-run debug. Production integration needs a split borrowed view that supplies
the shared hot/layout and run-specific debug without reconstructing the large
canonical object. Queue preflight, compatibility checks, resource marking,
ordinary append, query-range and Arena paths must agree on this view and its
lifetime. Every affected translation unit must use the same type definition.

After that integration, compare actual candidate GPU/query/lifetime effects
and fault settlement, then measure low-observer CPU on the identical input.
Only a demonstrated reduction should proceed to default late-live transfer.
H636 still does not separate original Replay and Encode CPU maxima; this
storage proof is not a substitute for that remaining global attribution.
