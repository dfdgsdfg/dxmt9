---
domain: present-pacing
workload: 3DMark05/06 GT2; actual PE32/PE64 segmented transport
title: "Segmented admission: geometry before copy, semantics on owned bytes"
type: evidence
status: measured-not-promoted
updated: 2026-09-08
source: experiments/output/pe-segmented-preflight-20260908/analysis
related: present-pacing-pe-transport-inline-boundaries.271.md; present-pacing-pe-native-thread-cpu.268.md
---

# Segmented admission: geometry before copy, semantics on owned bytes

## Hypothesis and boundary

H271 found that removing PE's contiguous gather did not demonstrate lower
inclusive commit-thread CPU. Inspection identified a concrete asymmetry:
`prepareOffloadChunk` validates the owned contiguous blob once, while
`prepareSegmentedOffloadChunk` performed the complete canonical semantic walk
both before and after copying. Both occur synchronously on the committing
caller; moving work into unixlib does not remove it from the game thread.

The candidate removes only the borrowed semantic walk. It freezes the
descriptor, checks geometry/identity without reading role bodies, copies the
bounded regions, and retains the full owned-byte validation before registry
retention/publication. No wire ABI, recorder semantics, worker policy, queue
order, shader binding, transport default, or diagnostic knob changes.
`DXMT9_PE_SEGMENTED_TRANSPORT` and inline constants remain default-off.

## Acceptance and failure refinement

R-CORE-REC-7.10 now distinguishes geometry from semantic authority. The
metadata helper checks canonical counts/extents/offsets, versions, reserved
fields, source identity, live-span alignment, and the checked 32 MiB virtual
wire bound before allocation. Protected role pages pin the no-body-read
property. Saved descriptor tokens are not compared with relocated owned
addresses; the C ABI entry owns token/extent pairing.

The production preparation test enumerates all eight geometry/body/allocation
boolean combinations with a fresh pool/output, then separately covers
resolution failure and pool reuse. It pins
geometry/body rejection, allocation
failure, resolution failure, unchanged output, zero retained ledger credit,
and pool recycle/reuse. Its post-copy rejection assertion fails against the
old runtime, then passes with the new preflight. Existing successful adoption
and source-mutation checks preserve role bytes and show that subsequent PE
mutation cannot change the accepted owned source. No unsafe concurrent
mutation or synthetic mid-copy callback is used; arbitrary concurrent client
mutation is not proved.

Semantically invalid but geometrically valid sources can now incur bounded
speculative allocation/copy. This is an intentional failure-cost change, not
an application-visible accepted-stream change. Storage/ledger/retain ownership
must still conserve. Both old and new `prepare == false` outcomes reach
`segmented-chunk-admission` / `D3DERR_INVALIDCALL` at the C entry; these native
tests do not prove public OOM HRESULT behavior. The pre-existing discrepancy
against R-CORE-REC-3.6's general recoverable-allocation contract remains open.

This is a finite native acceptance/conservation refinement of an unchanged
publication protocol, not a new temporal model. Existing TLC remains the
separate ordering/lifetime layer; it does not prove the parser, allocator,
concurrent client mutation, or Metal pixels.

## Build and measurement provenance

Baseline is committed `8e9dc320`. Candidate is that HEAD plus the runtime diff
whose SHA-256 is recorded in `candidate-runtime-build.json`. All four
canonical directories are rebuilt. Per-variant receipts pin the five actual
runtime images; shared validator code is also linked into the PE device DLLs,
so PE device image equality is not assumed. Bridge DLL equality is checked.
The shared provider archive is linked into the PE DLLs, adding helper code and
shifting symbol RVAs even without `d3d9_pe_*` changes. The production PE commit
entries still bridge to Unix; linked validator presence is not proof of PE
execution. This is a whole staged-build comparison, not an isolated swap of
only one Unix instruction range.

The initial baseline 05 r1 attempt failed before Wine launch: installer-driven
rebuilding selected llvm-mingw's unqualified `clang++`, which lacked native C++
headers. It contributes no FPS or correctness result. The Wine root still
matched all five frozen baseline images, so valid baseline runs use the
canonical runner's `--skip-stage --stage-mingw-runtime`, checking those images
before and after. Changed canonical build outputs are not baseline inputs.
The retained `baseline-images/` copies preserve the exact prior image set.

Two build-environment retries are separate from native counterexamples:
prepending Apple's toolchain selected an `ar` incompatible with the existing
`csrD` command; the native full build also needed the explicit macOS SDK root
for Foundation headers. The successful recipe preserves the existing PATH
(Apple compiler through the configured wrapper and LLVM `ar`), appends
llvm-mingw only for prefixed PE tools, and supplies SDKROOT for the native
full build. No build configuration or production source workaround is made.

`native-before-runtime-r2.log` is the actual failing test, not either toolchain
failure. `native-after-runtime.log` passes 2/2; expanded native focus passes
9/9, x86_64 focus 4/4, and unchanged TLC 160.90 seconds. An initial expanded
test command named a nonexistent ledger target and ran no tests; only the
corrected target list is evidence. The final explicit eight-cell checker was
added after scene measurement with no runtime change and rechecked in
`native-final.log` / `unix-final.log`.

## Measurements

CPU diagnostics use Sikarugir `sikarugir-cx-24.0.7`, named GT2, 1024x768 for
05 and 1280x720 for 06, perf profile with explicit Info logging, frame sampling
1, segmented 1, inline 0, offload 1, CPU Tape 0, and index reorder 0. They use
the external calibrated native thread observer and process snapshots, not a
new PE hot probe or stopped-PC sampling. No builds/tests overlap scenes.

The independently recomputed receipt uses one cumulative native-thread
interval and process observations from the same collection iterations.
Native CPU includes game/Wine/synchronous Unix, not PE-exclusive time. Frame
endpoints are approximate; cross-run scene populations differ. One run per
variant/product is not a noise-qualified promotion decision or a direct
comparison with README's Warn-profile FPS. Catalogue-allowed 06 timeout after
scene completion is recorded; health counters cannot prove an unreported tail.

Twelve fresh candidate PE32/PE64 processes pass 1,152 exact pixel samples:
inline OFF/ON on contiguous/segmented worker replay plus segmented synchronous
replay. All six high-register families and ordinary/indexed/UP/INDEX16
indexed-UP paths have exact colors, expected mode-specific append counts,
transport/staging receipts, eight zero health counters, and zero device
release. The fixture is unchanged from H271; this is a new-image integration
check, not wider StateBlock or consumer coverage.

| GT2 | Variant | Scene frames / seconds | Info FPS | Inclusive thread ms/frame | Process ms/frame |
|---|---|---:|---:|---:|---:|
| 05 | baseline | 1,919 / 64.587 | 29.712 | 31.247 | 70.216 |
| 05 | candidate | 1,925 / 64.603 | 29.797 | 30.892 | 69.659 |
| 06 | baseline | 1,154 / 64.074 | 18.010 | 52.781 | 96.147 |
| 06 | candidate | 1,164 / 64.071 | 18.167 | 52.579 | 95.604 |

CPU envelopes cover 96.56%/97.45% of 05 baseline/candidate frame intervals and
96.79%/96.82% for 06. The directional thread deltas are −1.14%/−0.38%, process
deltas −0.79%/−0.56%, and FPS deltas +0.29%/+0.87%. This is not repeatable
economy or isolation of the removed validator's cost; linked layouts and
cross-run arrival/scheduling remain uncontrolled.

CB/frame remains 4.0010 for 05 and 4.0017 for 06. Pass/frame is
15.7848 → 15.7855 and 19.8614 → 19.8643; different scene populations must not
be treated as exact pass equivalence. GPU CB time is 1.988 → 2.003ms/frame
and 11.956 → 11.917ms/frame, not a GPU optimization claim.
All reported eight health counters are zero. The 06 final counter snapshots
cover Present 2,100/2,160 versus final logged frames 2,126/2,176, leaving
26/16 tail frames without full error-counter coverage. 05 final counters
cover the full logged scenes.

The mechanism is implemented and the bounded safety gates pass; the first CPU
pair points in the intended direction, but does not promote segmented
transport or recover README on a comparable profile. README is unchanged.
`qualify_cpu.py` regenerates the raw-data comparison; `compact_evidence.py`
rechecks raw pixel signatures and assembles the durable
[`segmented-preflight-20260908.json`](../../../tests/conformance/d3d9/evidence/segmented-preflight-20260908.json)
receipt. Wider transport/capture/fault coverage and repeated low-perturbation
CPU/FPS comparisons remain separate gates.
