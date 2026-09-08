---
domain: present-pacing
workload: Actual PE32/PE64 constant pixels; segmented transport qualification
title: "Segmented empty-role failure and high-register inline boundaries"
type: evidence
status: measured-not-promoted
updated: 2026-09-08
source: experiments/output/pe-transport-inline-boundaries-20260909/analysis
related: present-pacing-inline-constant-boundaries.270.md; present-pacing-pe-native-thread-cpu.268.md
---

# Segmented empty-role failure and high-register inline boundaries

Measurements occurred on 2026-09-08 KST. The `20260909` artifact/run-id suffix
is a mistaken bookkeeping label, not the execution date; original paths are
preserved so raw launch/staging receipts remain resolvable.

## Decision and scope

The transport experiment found a production correctness failure before CPU
measurement. `DXMT9_PE_SEGMENTED_TRANSPORT` and inline constants remain
default-off. Do not interpret successful negotiation as a usable transport,
and do not compare FPS from a rejected source. README is unchanged.

## Reproduced empty-role failure

At `21029d09`, the new edge fixture passes on contiguous transport but fails
on segmented transport on both PE32 and PE64. Two segmented chunks / 151
records are admitted; a later call enters the contiguous bridge with zero
record bytes and is rejected as `chunk-bad-outer`. Final readback cannot be
locked and subsequent setters return device-lost. This is a transport/seal
failure, not an incorrect high-register shader result.

The production chain is:

1. `PeSemanticBatchOwner::makeTransport` encodes `span.data()` even when the
   role has zero bytes. Fixed-arena and caller-supplied empty spans can have a
   non-null backing address.
2. The semantic emission's validity check accepts it, but
   `SegmentedCommandChunk::valid` correctly rejects a nonzero token for an
   absent role.
3. `D3D9DeviceImpl::flushPendingCommandChunk` uses this validity as a transport
   selector. It falls through to the zero-initialized `semanticExact`, although
   no ExactFixed emission ran.
4. `dxmt9c_device_commit_chunk_impl` rejects `recordBytes == 0`; the PE entered
   bridge failure follows its existing effect-unknown poison contract.

The emitter must normalize absent role tokens, and a malformed selected
descriptor must fail before successful sealing/bridge entry. It must never
select an unmaterialized alternative. Existing `CommandChunkBuilder` tests
already check empty-role tokens, but did not exercise the production semantic
owner's aliased regions. No concurrency policy is changed by this correction;
native value refinement and the actual PE failure/readback are the relevant
new evidence. Existing settlement/lifetime models remain separate.

Pre-fix evidence under the analysis root:

- `archx86-inline0-offload1-layoutedge-caseplain-segmented1/`
- `edge-x64-segmented-repro/`

The initial PE32 driver's assertion stopped before its final qualification
write; its raw log, runner verdict and staged-build sidecar retain the failure.
It is not counted as a passing pixel run.

The added `segmentedAbsentRoleTokensMatchExactFixed` native test first fails
with `ExactFixed normalizes every absent role token`, then passes after the
emitter fix. It covers a real zero-handle Clear, alias/external roles,
non-null zero-length caller storage, exact role-byte parity, and the production
descriptor predicate with a malformed-token negative. This directly pins the
absent-handle dimension; the Clear has nonempty payload. The generic emitter
normalizes all roles, but no zero-payload semantic case is claimed.

After rebuilding all four canonical directories, twelve fresh PE32/PE64
processes pass 1,152 exact edge-layout samples: OFF/ON inline × both transports
with worker replay, plus OFF/ON inline segmented synchronous replay. Every
run has the same mode-specific append counts, zero reported renderer-error
counters and zero device release. These post-fix image identities are separate
from the pre-fix controls, not retrospectively applied to them. Bridge DLLs
and Unix provider are byte-identical; both PE device DLLs changed. Native 9/9,
x86_64 4/4, 26 Python observer tests and unchanged TLC (146.95s) pass. TLC does
not prove the descriptor bytes; the native counterexample and actual Wine
readback close that bounded defect.

The compact committed receipt is
[`inline-constant-transport-boundaries-20260909.json`](../../../tests/conformance/d3d9/evidence/inline-constant-transport-boundaries-20260909.json).
It keeps pre-fix and post-fix image/recipe groups separate and links H270's
earlier evidence. `compact_pixels.py` independently checks exact colors,
getters' test verdict, device/block teardown and all eight health counters
from the saved logs; the native failing receipt is explicitly a copied tool
transcript, not the overwritten original Meson log.

## High-register and indexed-UP fixture

Test-only `DXMT9_TEST_INLINE_CONSTANT_LAYOUT=edge` selects authored SM3
shaders using VS c255/i15/b15 and PS c223/i15/b15; executable c4/c5 scales
remain unchanged. It adds INDEX16 `DrawIndexedPrimitiveUP` to ordinary,
indexed and UP paths, for 24 rows / 96 draws. The plain A,A,B,A and recorded
StateBlock A,B,A,A sequences share independent expected colors, never
inline-dependent expectations. StateBlock getters separately pin the live
values. Unset layout retains the original 18-row / 72-draw test.

On the pre-fix images, 14 contiguous processes pass 1,296 exact sampled
pixels: two original-layout controls, eight edge plain cases across both PE
widths and OFF/ON × sync/worker, and four edge StateBlock OFF/ON worker cases.
Each process releases the device to zero. The two UP APIs intentionally share
the `append_drawup_calls` counter: edge layout records 24 ordinary, 24 indexed
and 48 combined UP appends, not an independently observed indexed-UP counter.

| Edge sequence / inline | Standalone VS / PS | APPLY_STATE | Total appends |
|---|---:|---:|---:|
| Plain OFF | 25 / 25 | 6 | 154 |
| Plain ON | 12 / 12 | 6 | 128 |
| StateBlock OFF | 49 / 49 | 98 | 294 |
| StateBlock ON | 31 / 31 | 98 | 258 |

All rows additionally include the 96 draws, one Clear and one other append.
These mode-specific counts are not the older fixture's counts. Neither
all-register shader layouts, all StateBlock types, ProcessVertices, FFP,
dirty-only query/readback/reset boundaries nor Render Tape is proven by these
sampled pixels.

## CPU measurement and transport economy

The external Darwin observer now accepts an explicit expected transport 1 or
2, defaults to 1, and requires the unique logged negotiation to match it.
Selectors carry the actual value; mixed/unsupported/boolean transports are
rejected. All 26 Python tests pass. The same common native commit marker
identifies both paths; it is a thread selector, not a per-call timing window.

Cumulative CPU includes PE recording, game, Wine and synchronous Unix work on
that caller thread. Segmented Unix preparation occurs before the marker in
the same call and still contributes to later cumulative differences. A CPU
delta cannot independently isolate the removed PE gather from Unix pooled
copy/validation/retention costs. Replay worker CPU is outside that thread but
inside the separate process total. The post-fix pixel/health gate above is
the prerequisite for CPU comparisons.

Four post-fix GT2 diagnostics use identical five-image fingerprints from
`runtime-build-qualified.json`, at HEAD `21029d09` plus runtime diff SHA-256
`2ab24eb968a0fdd65cf0ebe15f2a20e422b38deedf40f780b10617988b9ce4a1`.
The final staged paths and hashes are checked, not just the build copies.
Recipe: builtin Sikarugir CX 24.0.7, PE32, 05 1024x768 / 06 1280x720,
replay worker ON, inline constants OFF, Tape OFF, index reorder OFF, `perf`
with explicit Info logging and frame sampling. The external observer polls
at two seconds; no PE stopped-PC sampler, hot phase timers, build or test
overlaps the scenes. This is **not** Warn-profile or benchmark-owned FPS.

| Product / transport | Info scene FPS | Scene frames / seconds | CPU endpoint frames | Inclusive thread ms/frame | Process ms/frame |
|---|---:|---:|---|---:|---:|
| 05 contiguous | 29.708 | 1,919 / 64.596 | 4–1867 | 31.106 | 69.887 |
| 05 segmented | 29.503 | 1,906 / 64.603 | 4–1855 | 31.357 | 70.594 |
| 06 contiguous | 18.187 | 1,165 / 64.056 | 1023–2152 | 52.629 | 95.359 |
| 06 segmented | 18.029 | 1,155 / 64.065 | 1016–2168 | 52.849 | 95.998 |

The execution order is 05 OFF/ON, then 06 ON/OFF, one run per mode and
workload. Segmented relative to contiguous is **+0.81% / +0.42% thread CPU**,
**+1.01% / +0.67% process CPU**, and **−0.69% / −0.87% FPS** for 05/06.
This single pair does not establish a regression, but gives no economy
evidence for promotion. Removing the PE gather does not remove the segmented
Unix admission work; the remaining duplicate semantic validation below is
a candidate explanation, not measured phase attribution.

`qualify_cpu.py` independently reconstructs scene sums and continuous native
CPU envelopes, then differences cumulative process CPU at the same polling
iterations. Endpoints cover 97.0–99.8% of each scene's endpoint span. Process
queries precede native queries without their own exact bracket; observed
endpoint frame offsets are zero, but this is not exact per-Present timing.
Python monotonic and native CLOCK_MONOTONIC epochs differ; only elapsed
deltas and offset drift are compared. Cross-run scene endpoints/populations
differ, with no thermal control or randomization.

All runners pass, and the eight reported error counters are zero. Only 06
segmented has an uncovered cumulative-counter tail: Present 2160 versus last
frame 2170. Those ten frames are not covered by the zero-counter snapshot.
CB/frame stays 4.001–4.002; pass/frame is 15.785–15.787 (05) and
19.861–19.865 (06). These are spot locality observations, not a strict
no-increase gate or exhaustive wild pixel comparison. Catalogue-allowed
result-GUI timeout is not a renderer failure. Full recipes/raw logs and
`cpu-matrix-qualified.json` remain under the analysis/run roots. README and
both defaults remain unchanged.

## Rejected compact-path candidate

The duplicate accepted-plan check in `acceptPreparedSparseState` and
`acceptInlineConstantDelta` is real in the linked PE images, but belongs to
generic/full-snapshot/UP settlement. Ordinary compact Draw/DrawIndexed uses
`acceptSparseStatePlan`, whose linked PE32/PE64 bodies contain one accepted
plan check; the consumer checks are inlined/sunk away. Without a substantial
generic-traffic profile, splitting the generic helper is not a justified
optimization of the measured compact hot path.

## Next work-removal candidate

`device_c_replay_offload.cpp::prepareOffloadChunk` copies contiguous bytes
before one full owned validation. In contrast, `prepareSegmentedOffloadChunk`
calls `validateSegmentedCommandChunk` before and after copying. Both invoke
the full canonical record/handle/payload semantic walk, including scratch
referenced-handle work. This is a real extra synchronous Unix pass, not a
second full validation on the contiguous path.

The next candidate is a pure metadata/geometry gate before segmented
allocation/copy, followed by the existing full validator on owned regions
before any resolve/retain. It must retain role extents, identity, alignment,
overflow and the 32 MiB cap, and must prove malformed-body, truncated-role,
copy mutation, allocation/retain failure and lease/ledger rollback behavior.
Allocation/error precedence also needs an explicit review. Never eliminate
owned validation or assume a borrowed PE view stayed immutable. This candidate
is not implemented or credited with a CPU/FPS gain here.
