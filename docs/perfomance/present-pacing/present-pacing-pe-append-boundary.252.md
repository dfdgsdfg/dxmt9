---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE family-specific append preparation and ledger routing"
type: evidence
status: native-and-linked-verified
updated: 2026-09-07
source: experiments/output/pe-append-boundary-20260907/analysis
related: specs/d3d9/recorder/requirements.md; present-pacing-pe-append-effects.251.md
---

# PE append boundary refinement

## Scope

Base: `dcc4b66d`, whose compiled runtime is H251's `b83d59e1`.
R-CORE-REC-7.52–7.55 remove inactive preparation and repeated local work.
Canonical records, chunk cadence, source identity, transport negotiation,
queue policy and provider defaults remain unchanged. No new cache, runtime
knob, owner field or cross-boundary ABI is introduced.

1. The boundary always checks its semantic owner and emission frontier.
   Fault-only capacity prediction queries limits only when recorder faults
   are enabled. The fault seam retains its original **payload-byte** threshold;
   it is not replaced with the ordinary append's cadence-byte calculation.
2. A single family dispatch constructs constant, compact or generic
   preparation. One templated transaction body retains pre/post capacity,
   admission, settlement, retry and observer ordering. Constants carry only
   their input pointer; they do not construct a generic input view. This is
   compile-time specialization, not three independent transaction policies.
3. The compact planner owns its full sparse-layout reset. Outer preparation
   invalidates reused state first; generic reset remains complete. Adjacent
   sparse-count and semantic-byte quota claims share one private source and
   destination check. They contain no callback and do not move the qualifying
   record/handle/payload frontiers. Later serializer and cache-publication
   requalification remain intact, including after callbacks.
4. `PeProductionSemanticBatchOwner` explicitly selects stateless production
   ledger access. Generic/native owners default to the scoped override policy.
   Both instantiate the same owner algorithm. Production bypasses test TLS,
   but retains the cached process gate and owner-qualified pointer lookup.
   Existing admission/final-wire retention pointers are accounting ownership,
   not new lookup caches.

## Evidence boundaries

All four canonical full builds pass. Native PE/source-audit tests pass 36/36;
matched-compiler x86_64 focused tests pass 14/14, including the install-name
audit. Ledger-OFF and ledger-ON each run in a fresh process. The unchanged TLA
suite passes: 51 safety configurations, two progress configurations and 109
expected counterexamples. No new queue transition or concurrency claim is
introduced. Whitespace and performance-document source checks pass.

The production-used fault helper is compared with an independent overflow-safe
truth table, including zero records and size limits; disabled calls never
invoke the limit thunk. Family dispatch selects exactly one preparation and
preserves callback exception propagation. Real owner comparisons cover
generic accepted prefixes, capacity rejection, independent kind mismatch,
settlement rollback and retry. All six constant families cover A/B/A, valid
payload with stale-ticket settlement failure, same-ordinal retry, retained
prefix bytes and the independent canonical builder. Compact coverage includes
real plan admission, A/B/A layout reuse, malformed retry, and the existing
quota/retain/source-freshness failure suite. Both ledger policies exercise
actual admission, failed settlement, repeated emission, reset and retention
conservation; scoped nesting and thread isolation remain tested.

The analysis directory freezes pre-build linked PE32/PE64 evidence in
`baseline-r1-linked.json`; hashes match H251. Final integrated evidence is
`candidate-r2-linked.json`, its assembly, `codegen-summary-r2.txt`,
`native-tests.log`, `x86-native-tests-r2.log` and `tla.log`. Initial failed
iterations are retained: missing LLVM PATH for capture/cross compilation,
the owner forward/friend template declarations, and native fixture namespace
qualification. None is presented as a passing receipt.

Main review rejected the initial persistent-variant preparation design:
zero allocation alone does not remove the largest alternative's stack space
or repeated discriminator work. It also separated malformed-input tests from
full-capacity rejection and valid-payload settlement failures. Local helper
and owner differential tests do not execute the private COM device's whole
HRESULT/cadence choreography. That boundary still requires code review,
linked inspection and separate integration evidence.

## Linked production code

| Individual frame reservation (bytes) | PE32 | PE64 |
|---|---:|---:|
| Previous combined append | 312 | 552 |
| New family dispatch | 56 | 88 |
| New constant append body | 128 | 248 |
| New compact append body | 168 | 312 |
| New generic append body | 144 | 264 |

These are separate function prologues, not complete call-chain stack peaks
or CPU timings. Specialization outlines three bodies from one source template;
it adds a dispatch call and increases code footprint. Both PE DLLs grow by
29,184 bytes: PE32 1,868,800 → 1,897,984, PE64 1,734,144 → 1,763,328.
Instruction/cache economics therefore remain a runtime question.

Exports and vtable/RTTI archive owners match baseline. The 12 manifest hot
entries have zero deltas in bytes, instructions and direct calls on both
architectures; this does not mean their changed callees are cost-identical.
The standalone compact layout callee remains 602/632 static instructions;
single-reset ownership removes the preceding caller reset, not that callee.
Production ledger routing removes the five directly owned admission/exact-wire
TLS access sites selected by the linked audit on each architecture. Scoped
native routing remains a separate instantiation of the same owner body;
other PE diagnostics and Windows TLS support are not removed.

- PE32 SHA-256: `752ae23a8f25913fc98765b8358fcada04b6a69032f1d8467647b127f9c9c99d`
- PE64 SHA-256: `488c3cacd72afa3cb5a14c47373a436794596920e491a7ff2c9c38facd1143ae`
- Unix SHA-256: `e4543457d6992f1a2a8a088a590e75170360d8938e4c4aeda582ab3b1da4a09a`

The Unix provider and both bridge DLLs are byte-identical to baseline after
matched-toolchain rebuilding. The initial Unix build used a Mingw-prefixed
PATH, resolving bare `clang++` to Clang 22.1.2 despite cached Apple Clang 21
Meson metadata. Its smaller binary and first x86_64 test receipt are rejected,
not optimization evidence. A clean generated-output rebuild restores Apple
Clang 21.0.0 and LLVM ar 21.1.8; an intermediate Apple BSD ar/`csrD` mismatch
is also retained in the logs. `rejected-candidate.md` records these exclusions.
No build configuration or staged Wine installation was changed.

No wild run or FPS measurement is part of this change. H251 remains the
latest runtime checkpoint. Static instruction counts or smaller individual
stack frames must not be converted into predicted FPS gains.
