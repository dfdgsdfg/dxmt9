---
domain: mini-replay-bisection
workload: 3DMark05 GT2 fixed Render Tape
title: "Qualified capacity survives optional denial and compatibility growth"
type: experiment
status: accepted-verdict
updated: 2026-09-14
source: experiments/output/gpu-tape-qualification-20260910/public-policy-r610/result.json; experiments/output/gpu-tape-qualification-20260910/public-policy-r611/result.json; experiments/output/gpu-tape-qualification-20260910/public-policy-r612/result.json; experiments/output/gpu-tape-qualification-20260910/audit-ownership-r614.json; build/meson-logs/ownership-native-final-r610.txt; build-x86_64-builtin/meson-logs/ownership-x86-r610.txt; build/meson-logs/ownership-tla-r608.txt; experiments/output/gpu-tape-qualification-20260910/ownership-direct-images-r607/ordinary-growth-counterexample.txt
related: specs/backend/encode-scheduling/gap.md; specs/verification/spec.md; docs/perfomance/mini-replay-bisection/gpu-position-invariance.04.md
---

# Qualified capacity survives optional denial and compatibility growth

## Verdict

The unchanged r405 GT2 capture now passes the public CPU/GPU policy gate with
both positive-headroom policies and the zero-headroom control. Each policy pair
includes two CPU verification iterations, two cold-device GPU iterations per
route, one CPU warm-up and three measured iterations, followed by atomic outer
publication. All three reports have `accepted`, `identity_bound` and
`gpu_evidence` true. This closes the observed ownership transition failures,
not a throughput ranking, default promotion or arbitrary-workload guarantee.

| Run | Per-slot / aggregate policy | GPU candidate result | Exact reuse / replacement |
|---|---|---|---|
| r610 | 1MiB / 16MiB | Two exact original readbacks; zero GPU errors | 34 / 2 |
| r611 | 5MiB / 64MiB | Two exact original readbacks; zero GPU errors | 12 / 0 |
| r612 | 0 / 0 | Two exact original readbacks; zero GPU errors | Provisioning disabled |

Reuse/replacement counts are **process totals across both GPU iterations**,
not per-frame values. The r610 and r611 optional admission denial totals remain
48 and 56 respectively, with zero exact fallback rejections. Denial itself is
normal policy behavior; preservation of ownership after denial is the repair.
Required exact capacity is accounted beyond the optional budget: r610 retains
17,882,444 bytes against a 16,777,216-byte optional ceiling. The ceiling was not
raised and no extra headroom is added to required exact plans.

Each candidate GPU iteration executes 1,856 Direct draws through 182 leases,
10 compatibility draws plus seven separately counted unbatched draws, and
186 encoded chunks. Observed post-reserve SoA growth remains nonzero (r610: 5,
r611: 9); this result is not a zero-allocation claim.

## Distinguishing failures and repair

The retained r586 negative rejected a new unqualified admission on a payload
which still carried a qualified capacity receipt. Optional provisioning denial
had fallen through without preserving that receipt. `recoverDirectSlotExactCapacity`
now validates the old owner before reconciliation, reuses exact coverage, or
stages required exact replacement with old and new storage charged together.
Allocation failure preserves old storage and returns staged credit.

The smaller r605 policy exposed a second boundary after all Direct draws had
committed: the final compatibility batch grew previously qualified storage,
leaving its physical capacity different from its receipt at reclaim. A native
one-Clear-capacity fixture followed by two ordinary Clear appends reproduced
that failure without GPU execution. `CompatibilityCapacityWrite` now proves
ownership **before** mutation; the queue wrapper settles actual capacity before
the mutex-held append/batch ends, including partial allocation exceptions.
Publication and reclaim reject unaccounted growth without issuing late proof.
No detached-owner guard was weakened and no source/oracle was replaced.

## Verification scope

- Native integration: 13/13; additional lifecycle/plan/counter audits: 8/8
  (`build/meson-logs/ownership-extra-r616.txt`); x86 integration/install-name
  audit: 5/5. The three new exact-fallback counter keys are also pinned in
  `scripts/check/perf_counter_schema.golden`.
- TLA gate: 196 configurations (59 normal, four progress, 133 expected negatives).
- `DirectSlotDeniedOwnership` composes optional denial, exact recovery,
  ordinary growth/partial failure, publication, completion and reclaim; five
  negative configurations detect downgrade, direct/ordinary unpriced growth,
  premature reclaim and missing pre-mutation authority.
- Native binding covers all 30 optional/required staging allocation points,
  partial ordinary-reserve exceptions across all 31 physical dimensions,
  stale source/storage/sequence/receipt, moved and consumed capabilities, and
  publication without a prior growth witness.
- `audit_ownership_r614.py` independently reauthenticates requested policies,
  original/staged bundles, source plan, CPU/GPU receipts, loaded images, exact
  readbacks and final publication. r586/r605 remain rejected and unmeasured.

Original source and output each contain 3,145,728 bytes, SHA-256
`d5a01daf5f2fcf66c90e08bf6e9064a5902eda58aa717d321ce4d11ee6701284`.
The immutable source-plan digest remains
`cc6b8312ea6abfc0a571741faca56aec729d8dac57e5585a3589bce29aac34f6`.
The original Tape, failures, and pre-fix runtime images are preserved.

Ordinary mutation is serialized by the existing queue mutex. The model does
not prove GPU byte behavior or arbitrary worker timing; the strict retained
GT2 oracle supplies the bounded GPU evidence. Other workloads/devices,
concurrent scheduling, locality and FPS promotion remain separate gates.
