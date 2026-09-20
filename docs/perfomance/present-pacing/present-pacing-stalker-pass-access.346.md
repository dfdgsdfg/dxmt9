---
domain: present-pacing
workload: STALKER CoP Day
title: "H346: Separate immutable pass-read metadata from draw-entry summaries"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-pass-access-h346-r694-day/{result.json,actual.png}; experiments/output/stalker-pass-access-h346-r694/{supply.csv,waits.csv,progress.json,tla.log}
related: present-pacing-stalker-resource-closure.345.md
---

# H346: Pass access metadata over immutable Slot SoA

Base `3b5c47a0` plus the pass-access overlay. `ParallelPassReadSet` owns a bounded
array of canonical read identities separately from the unchanged 104-byte,
12-entry `core::ExactResourceSet` used for draw-entry continuation summaries.
Pass snapshots, independently resolved child coverage, and the certificate's
expected union use the same pass-specific type. Attachment writes remain small.
No draw/state/uniform SoA is copied or resized, no source borrow escapes, and no
new resource-retention or completion authority is introduced.

The enclosing snapshot/certificate binds the identities to source/storage
generation, sequence, replay range and pass-action epoch. A pass read union is
not a lambda capture or a lifetime token. The immutable certificate still owns
value metadata, and its resolver independently re-reads source facts: this is
not yet zero-copy proof metadata or a once-only resolver optimization.

The initial admission budget is 256 identities. Each set occupies 2056 bytes,
1952 bytes more than an entry summary. The 16-candidate TLS batch and 16-child
resolved-coverage stack array each grow by 31,232 bytes. There is no per-draw
allocation, and serial execution builds no access plan unless the diagnostic
observer is enabled. Memory/copy pricing and a shared identity-table/bitset
representation remain possible follow-ups, not claimed benefits.

## Correctness evidence

The production accumulator is checked against an exact mathematical set over
4096 six-step small-domain traces, including nulls, duplicates, every write
alias, exact capacity and sticky overflow. Native tests also cover:

- unchanged small-entry overflow rejection;
- a 14-identity pass accepted without widening its entries;
- pass-wide capacity overflow rejected once despite repeated failures;
- late canonical write alias after the old 12-identity boundary;
- 60 identities across a real multi-command producer/certificate fixture;
- stale storage generation, missing sealed identities and omitted late child
  reads rejected by authority or independent coverage re-resolution;
- unchanged ordered coverage and malformed child range counterexamples.

Focused native/analyzer tests pass 4/4; Python tests pass 27/27; TLA regression
passes in 230.37 seconds. The exhaustive set check covers the new accumulator;
existing TLA models cover prior execution/lifetime contracts, not GPU bytes.
Native/unix and both PE builds pass; unix install-name audit passes. PE rebuild
reports the existing unused `g_ntAllocScanHint` warning in unchanged
`device_c_marshal.cpp`. No wide-resource GPU exact oracle is claimed.

## R694 serial-only observation

Day/high 1280x720, Sikarugir cx24.0.7, perf; same explicit query/dedup settings
and diagnostic windows as R693. Metal occlusion, async fences, covered Pending
and publication dedup ON; Tape, CB join, END coalescing and command/draw split
OFF. IdentitySerial plus parallel observer ON. Supply 2450–2470, waits
2452–2468. No builds or heavy tests overlapped the runtime.

Runner PASS; no failures/capture error; 2732 encoded Presents; zero resolved
parallel work and zero reported GPU command-buffer errors. Screenshot shows
the game rather than a desktop fallback, not pixel equality. PID 27999; unix
SHA-256 prefix `704996eb3773dfdf` matches the built provider; all staged hashes
are retained in `result.json`. Both traces close with zero overflow/incomplete
writers, no unknown wait identities and 17 CV entries. Two partial edge sources
are excluded; all 1196 complete ordinary sources have qualified observations.

| Source group | Sources | Inclusive encode CPU ms | Structural passes | Draws |
|---|---:|---:|---:|---:|
| Query-scoped | 1134 | 195.967 | 0 | 0 |
| Unscoped with candidates | 20 | 314.848 | 54 | 35159 |
| Unscoped without candidates | 42 | 281.567 | 0 | 0 |

Accepted passes contain at most 25 read identities, and 658 in sum across the
54 passes (not a unique whole-source cardinality). Resource incompleteness
persists in 21 passes, all pass-union overflow beyond 256; raw-read/write
incompleteness remains zero. These 21 sources account for 277.425ms inclusive
encode CPU, not recoverable parallel work. Other rejection counts: query 1134,
small work 306. Observer CPU totals 18.194ms.

R693 observed 40 accepted passes and 61 union failures at the old bound; this
fresh run observes 54 and 21. Fixed Present windows are not semantic alignment,
so neither the differences nor CPU totals establish causal speedup, regression,
or how many identical old passes became executable. The deterministic fixtures
establish the representation change; this run demonstrates accepted >12-read
work and remaining >256-read rejection in production observation.

## Remaining gates

Do not blindly grow the bound again. Measure the required cardinality and
identity reuse of the 21 remaining large passes, then choose owned bounded
storage or an exact identity-index representation without enlarging draw SoA.
Accepted-only cardinality events cannot determine an overflowed pass's full size.
The larger proof metadata must be priced before promotion. Wide-resource GPU
exact evidence, executor binding/economics and child imbalance remain open.
ExplicitParallel remains opt-in; no default parallel execution or FPS claim.
