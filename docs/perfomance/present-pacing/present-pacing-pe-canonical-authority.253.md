---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE grouped borrow authority and single pin index"
type: evidence
status: native-and-linked-verified
updated: 2026-09-07
source: experiments/output/pe-canonical-authority-20260907/analysis
related: specs/d3d9/recorder/requirements.md; present-pacing-pe-append-boundary.252.md
---

# PE canonical authority refinement

## Scope

Base: `9cd8f989` (H252). R-CORE-REC-7.56–7.59 remove repeated local
qualification and redundant indexing without changing canonical wire bytes,
chunk cadence, resource lifetimes, transport negotiation or queue policy.
There is no new runtime knob, cross-boundary ABI, provider policy or cache.

1. Ordinary compact draws skip the cadence-only `SparseStatePlan::source`
   lookup and six constant-range reads when inline constants are OFF. ON
   retains exact accounting. Plan construction and subsequent admission,
   destination rebase and settlement qualification remain mandatory.
2. `SparseStatePlan` holds one private `RecorderBorrowDomain` and three typed
   pointers instead of three independent borrow witnesses. Fresh access checks
   owner, epoch address/value, lane, all pointers and PendingDelta. The build
   guard revokes the aggregate on failure; successful initialization does not
   clear and immediately overwrite inactive source fields.
3. Live same-object texture, VS, PS and index-buffer setters bypass validated
   product reconstruction. Strong slot retention and immutable wrapper
   identity are the prerequisite, not arbitrary pointer equality. Recording,
   poison, argument checks, null, replacement and foreign inputs retain the
   full path. Borrowed vertex declarations are excluded. Wire shape checks do
   not independently prove nonzero identity freshness; the retained-slot
   invariant supplies that qualification.
4. Six kind-local object-ID indexes replace twelve paired indexes. The typed
   pin prefix is authoritative; a table hit only locates its candidate row.
   Admission and `referencesBuffer` share `checkedPinHit`, which verifies full
   identity, pin bounds and physical pointer. Reset, rollback, warm retention
   and stamp wrap retain their existing lifetime contracts. The small
   record-local linear dedup strategy is unchanged.

## Verification

Four canonical full builds pass with matched compilers. Native PE/access/
allocation/source-audit tests pass **39/39**; x86_64 focused tests pass
**14/14**, including install-name validation. Owner ledger OFF and ON execute
in separate processes. The unchanged TLA suite passes 51 safety and two
progress configurations, plus 109 expected counterexamples. These local
representation changes introduce no new queue or concurrency transition.

Production-used helper checks cover OFF/ON query counts and retained Live
reuse decisions against a small always-validate reference, including A/B/A,
Recording, null, write denial and multiplicity/dirty effects. This is a value
model, not actual COM AddRef/Release or a private-device HRESULT harness.
The real producer tests qualify exact source references, wrong owners,
changed epoch values/addresses, lane mismatch, stale PendingDelta, invalidation,
post-bind failure and retry. Thread-affinity denial is debug-only, matching
`ThreadOwnershipToken`; release retains the existing caller serialization
contract. Callback return-type pins do not prove arbitrary C++ capture safety.

The real owner trace compares each accepted prefix with a fresh independent
canonical builder and checks dependency queries, alias rejection, partial
rollback, retry, warm retention and wrap. A private friend seam corrupts an
existing hit's kind, generation, pin bounds or locator-to-row relation;
admission and queries both reject it. This bounded coverage does not claim
detection of arbitrary memory corruption or missing-index entries.

Final receipts: `native-tests-r4.log`, `x86-native-tests-r3.log`, `tla.log`,
`baseline-linked.json`, `candidate-linked.json`, `codegen-summary.txt`, and
the assembly/footprint witnesses in the analysis directory. Earlier failed
receipts remain preserved: a missing PE helper include, appending to a sealed
comparison builder, insufficient sparse fixture capacity, and counting the
comparison builder's retained ref as the owner's warm retain. Main review also
corrected the cadence query assertion before execution and removed redundant
reset/borrow checks. Fixture fixes retain the independent assertions;
they do not relax production rejection or ownership rules.

## Linked shape and limits

Production storage loses `6 * 1025 * 24 = 147,600` bytes on both PE targets.
Its compiled size is now 7,819,584 bytes; this is the owner backing allocation,
not the COM device object size or a process RSS measurement. The compiled
compact plan sizes are 288 bytes (PE32) and 368 bytes (PE64).

| Linked DLL | Baseline bytes | Candidate bytes | Change |
|---|---:|---:|---:|
| PE32 | 1,897,984 | 1,891,840 | -6,144 |
| PE64 | 1,763,328 | 1,760,768 | -2,560 |

Exports, vtable/RTTI archive ownership, device layout pins and build options
match. The 12 manifest hot-entry metrics remain identical; several changed
callees are outside that manifest. The standalone plan validator is now
inlined. Both compact draw append bodies have 23 → 22 static call sites,
but larger complete bodies: PE32 DrawPrimitive 233 → 294 instructions and
96 → 104 bytes of individual frame reservation; PE64 196 → 248 instructions
with its 120-byte frame unchanged. The OFF branch jumps over the cadence-only
qualification and range sum; ON still executes them.

PE64 SetTexture is 298 → 254 static instructions, while VS/PS setters grow
277 → 307 and SetIndices 308 → 337. Their individual frame reservations grow
by 8/32/32 bytes respectively. Same-binding branches skip validated-product
initialization and membership calls on both architectures, but replacement
paths pay the extra guard. The supplemental `candidate-x86-setters.asm`
inspection handles PE32 stdcall names omitted by the original demangled
symbol filter; no before/after PE32 setter count is claimed.

- PE32 SHA-256: `07a695e8a41f6795758ef317e810f9ee06ff75f525be3dc10ca5064c6ff97803`
- PE64 SHA-256: `cce6c0bb9ae14125e87d63cfdc341e6280bfc3c84e3b20ef65077599295ff32b`
- Unix SHA-256: `e4543457d6992f1a2a8a088a590e75170360d8938e4c4aeda582ab3b1da4a09a`

The final Unix provider and both bridge DLLs are byte-identical to baseline.
Static counts include cold paths and padding; they are not executed work,
call-chain stack peaks or CPU time. No wild run, FPS result or promotion is
part of H253. H251 remains the latest runtime checkpoint. Current PE CPU
attribution, same-binding hit distribution and whole-scene Wine correctness
remain separate integration evidence.
