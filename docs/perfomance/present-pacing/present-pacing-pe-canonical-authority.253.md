---
domain: present-pacing
workload: 3DMark05 GT2
title: "PE grouped borrow authority and single pin index"
type: evidence
status: native-linked-and-wild-spotcheck
updated: 2026-09-07
source: experiments/output/pe-canonical-authority-20260907/analysis; experiments/output/app-d3d9-3dmark05-pe-canonical-authority-f997e237-gt2-r1-20260907
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
call-chain stack peaks or CPU time. Current PE CPU attribution and same-binding
hit distribution remain separate integration evidence.

## Post-commit GT2 spot check

One clean-tree run at `f997e237` uses Sikarugir cx-24.0.7, GT2 at 1024x768,
the perf profile and frame sampling ON. PE decimation, recorder statistics,
owner phase splitting and the copy ledger are OFF. There is no PE sampler,
GPU trace or interleaved baseline. All five installed Wine-root artifact
hashes match the staging/linked receipts. The provider and bridge DLLs also
match the prior measured H251 build.

| Metric | H251 `b83d59e1` | H253 `f997e237` |
|---|---:|---:|
| Positive-wall samples | 1,876 | 1,874 |
| Sum sampled wall ms | 64,614.120 | 64,599.637 |
| Sampled FPS | 29.034 | 29.009 |
| Encoded Presents | 1,877 | 1,875 |
| Command buffers / Present | 3.99947 | 3.99947 |
| Render passes / Present | 15.77890 | 15.77493 |
| Draw calls / Present | 1,681.425 | 1,680.949 |
| GPU command-buffer ms / Present | 2.032 | 2.013 |

Sampled FPS is positive-wall sample count divided by summed sample time;
process elapsed time (73.073 seconds) is not its denominator. The unrounded
FPS change is -0.0842%. This comparison includes H252 and H253, because H252
has no separate runtime checkpoint; it cannot isolate H253's four changes.

The run passes with exit 0, no timeout, no harness failures, and zero chunk
rejects, GPU errors, no-pipeline skips or draw/function/library/PSO build
failures. The recovered `.3dr` is retained, but its score is not the sampled
FPS metric. `actual.png` shows a coherent GT2 forest frame at overlay 1:00.34;
one frame does not establish whole-scene correctness or exclude flicker.

No competing Wine/build process was observed at preflight. An in-run snapshot
at scene-process elapsed 29 seconds records `duetexpertd` at 95.2% CPU and
3DMark05 at 177.5%. This background load was absent from H251's one snapshot;
neither snapshot is a continuous CPU profile. No background service was stopped.

Verdict: bounded runtime-health evidence with close CB/pass ratios, not an
attributed FPS gain, regression or noise-qualified neutrality. Whole-scene
correctness, repeatability, hit distribution and current PE CPU attribution
remain open. Exact command, counters, binary hashes and artifact provenance
are in `analysis/wild-run-receipt.md`; logs and the process snapshot are kept
beside it. README performance values are unchanged.
