---
domain: present-pacing
workload: Actual PE32/PE64 constant pixels; 3DMark05 GT2; 3DMark06 GT2
title: "Inline constants: StateBlock boundaries and continuous CPU envelopes"
type: evidence
status: measured-not-promoted
updated: 2026-09-08
source: experiments/output/pe-inline-boundary-economy-20260908/analysis
related: present-pacing-inline-constant-pixels.265.md; present-pacing-inline-constant-economy.266.md; present-pacing-pe-native-thread-cpu.268.md; present-pacing-pe-held-constant-value.269.md
---

# StateBlock boundaries and continuous CPU envelopes

## Scope

`DXMT9_PE_INLINE_CONST_DELTA` remains default-off. This increment strengthens
its actual PE correctness evidence and external CPU measurement; it does not
change a runtime binary, wire representation, renderer default or README FPS.
The five runtime images remain H269's restored H267 production images.

## Actual PE StateBlock and full-snapshot matrix

`tests/conformance/d3d9/d3d9_inline_constants.cpp` preserves the original
default fixture. Its test-only `DXMT9_TEST_INLINE_CONSTANT_CASE=stateblock`
mode adds a separate six-family VS/PS float/int/bool sequence on each of the
VB, indexed and standalone UP paths. It uses the same executable SM3 shaders
and independent sampled colors, not a new replay imitation.

For each family the four cells are A, B, A, A:

1. BeginStateBlock, record B, EndStateBlock; draw A. Recording must not
   change the live family.
2. Apply the block; draw B.
3. Explicitly set A; draw A.
4. Capture A into the block, set pending B without drawing, Apply A; draw A.

Exact public getter bytes pin recording/Apply/Capture checkpoints separately
from the final GPU readback. Every block is released before device teardown.
The fixture selector chooses the test sequence, never the inline mode's
expected result; an unknown selector fails before COM creation.

Sixteen fresh builtin Sikarugir processes pass 1,152 exact sampled pixels:

| Fixture | PE widths | Inline | Replay | Full snapshot | Processes |
|---|---|---|---|---|---:|
| StateBlock | 32/64 | OFF/ON | sync/worker | OFF | 8 |
| StateBlock | 32/64 | OFF/ON | worker | ON | 4 |
| Original A,A,B,A | 32/64 | OFF/ON | worker | ON | 4 |

Each process has 72 draws, one final readback, device Release zero, and eight
reported renderer-error counters at zero. Each StateBlock process additionally
has 18 zero block-release receipts. The independent qualifier pins the whole
intended recipe set, unique family/path/row signatures, source/fixture/image
identity and these exact append receipts:

| Fixture / inline | Standalone VS / PS constants | APPLY_STATE | All records |
|---|---:|---:|---:|
| StateBlock OFF | 37 / 37 | 68 | 216 |
| StateBlock ON | 19 / 19 | 68 | 180 |
| Original OFF | 19 / 19 | 6 | 118 |
| Original ON | 6 / 6 | 6 | 92 |

All have 24 ordinary, 24 indexed and 24 UP records, plus Clear and one other
record. Mode receipt counts are not reused from H265's original-only matrix.
Full snapshot preserves these counts/pixels but increases StateBlock wire
bytes from 25,704/25,128 to 354,112/353,536 for OFF/ON. That is runtime
wire-shape evidence beyond merely echoing the environment selector.

`run_pixels.py`, `run_pixel_matrix.py` and `pixels-qualified.json` under the
analysis root preserve the canonical conformance-runner commands and receipts.
The compact committed receipt is
[`inline-constant-boundaries-20260908.json`](../../../tests/conformance/d3d9/evidence/inline-constant-boundaries-20260908.json);
it links the earlier H265 evidence without claiming a new mutation run.
This closes the tested recorded-StateBlock and full-snapshot combinations,
not all StateBlock kinds, high registers, FFP, ProcessVertices, indexed UP,
Render Tape capture, late failure boundaries or CPU-ready scheduling.
H265's existing replay mutation remains separate evidence; no new production
mutation was performed in this matrix. No state/concurrency transition was
changed, so no new TLA proof is claimed.

## External CPU envelope

H268's adjacent-stable-sample reducer drops both intervals next to an unstable
log observation. Different dropped intervals can select different scene
populations; H269 correctly refused to compare those as a CPU reduction.

`scripts/tools/observe_darwin_thread_cpu.py::summarize_envelope` additionally
differences the first/last stable endpoints of one contiguous scene envelope.
Every included intermediate row must remain a successful query with the same
PID/start/native-TID/device selector and log file identity, valid observer
brackets, nondecreasing counters and ordered in-scene log frames. Unstable
interior frame observations are permitted; query errors, missing provenance,
mixed identity and regressing counters are not bridged. The original partial
reducer is unchanged and remains available for comparison.

The result includes the frame span, omitted prefix/suffix, CPU and wall totals
and query bounds. File identity does not authenticate causal log ownership;
the caller must supply the fresh run's own log. Log-frame endpoints remain
approximate, not exact Present timestamps. This is game/Wine/synchronous Unix
inclusive commit-thread CPU, not PE-exclusive CPU. The observer runs natively
on Darwin without adding an ARM64 or macOS dependency inside PE.

## GT2 CPU diagnostics and contaminated-run replacement

All runs use the same five production images at `73d523d7`, builtin
Sikarugir CX 24.0.7, PE32, 05 1024x768 / 06 1280x720, replay worker ON,
contiguous transport, CPU-ready tape OFF and index reorder OFF. The recipe
uses `perf` with an explicit **Info** override to expose the existing native
commit-thread selector, frame sampling and external two-second cumulative
process/thread CPU polling. There is no PE phase timer or stopped-PC sampler.
These are diagnostics, not Warn-profile or benchmark-owned `.3dr` FPS.

05 ran OFF/ON/ON/OFF. The user reported desktop activity during **06 OFF r1**;
that run is preserved but excluded regardless of its result. The replacement
is **06 OFF r3**, measured without overlapping builds/tests. The retained 06
sequence is ON/ON/OFF/OFF, not a balanced ABBA. The exclusion and replacement
are explicit in `excluded-runs.json`; `qualify_cpu_matrix.py` re-derives all
retained scene sums, CPU envelopes and source/staging identities into
`cpu-matrix-qualified.json`.

| Product / mode / repeat | Info scene FPS | Scene frames / seconds | CPU endpoint frames | Thread ms/frame | Process ms/frame |
|---|---:|---:|---|---:|---:|
| 05 OFF r1 | 29.168 | 1,886 / 64.660 | 2–1833 | 31.675 | 71.065 |
| 05 ON r1 | 30.321 | 1,959 / 64.609 | 3–1902 | 30.862 | 69.695 |
| 05 ON r2 | 29.937 | 1,935 / 64.635 | 91–1897 | 31.550 | 70.869 |
| 05 OFF r2 | 29.374 | 1,898 / 64.615 | 4–1774 | 31.690 | 71.243 |
| 06 ON r1 | 18.314 | 1,172 / 63.995 | 1022–2155 | 52.113 | 94.740 |
| 06 ON r2 | 18.372 | 1,176 / 64.011 | 1031–2176 | 51.797 | 94.009 |
| 06 OFF r2 | 18.315 | 1,173 / 64.045 | 1022–2150 | 52.554 | 95.275 |
| 06 OFF r3 replacement | 18.132 | 1,163 / 64.141 | 1027–2152 | 52.469 | 95.431 |

CPU denominators are the endpoint differences, not the whole-scene frame
counts: 93.3–97.4% of each scene's endpoint span. Process CPU is also a single
cumulative difference at the **same polling iterations** as the thread
endpoints, not the older driver's sum of individually accepted intervals.
The `ps` query precedes the thread query and lacks an exact query bracket;
endpoint frames and elapsed deltas are checked but are not simultaneous
per-Present observations. Python `monotonic()` and `CLOCK_MONOTONIC` have
different Darwin epochs; absolute offsets are recorded, not misread as query
latency. The native helper and its observer bracket use `CLOCK_MONOTONIC`.

Pooling CPU totals over their frame spans gives OFF → ON:

| Product | Inclusive commit-thread ms/frame | All-thread process ms/frame | Info scene FPS |
|---|---:|---:|---:|
| 05 GT2 | 31.682 → 31.197 (−1.53%) | 71.152 → 70.267 (−1.24%) | 29.271 → 30.129 (+2.93%) |
| 06 GT2 | 52.512 → 51.954 (−1.06%) | 95.353 → 94.372 (−1.03%) | 18.224 → 18.343 (+0.65%) |

This is a **small economy signal**, not a demonstrated PE-exclusive saving
or a noise-qualified promotion. There are only two runs per mode, no thermal
control or randomization, and continuous envelopes still have different
approximate scene endpoints across runs. In particular, 05 ON r1 vs r2
varies substantially more than the average CPU delta. Info FPS exceeding an
old README value does not establish its recovery under the README recipe.

All eight retained runners pass and the eight reported error counters are
zero. 05's final cumulative counters cover its logged frames; each 06 run's
last counter snapshot is Present 2160 while its frame log reaches 2182–2194.
The remaining 22–34 frames are **not** covered by those cumulative zero-error
receipts, and scene pixels are not exhaustively checked. Catalogue-permitted
result-GUI timeout is not a renderer failure. CB/frame stays near 4.001–4.002;
pass/frame is 15.788–15.816 (05) and 19.857–19.860 (06). These are locality
spot observations, not a strict no-increase gate.

## Rejected source-only candidate

A source audit suggested moving two six-entry constant pointer arrays inside
`source.hasConstantRanges` in `acceptSparseStatePlanWithSources`. Linked PE32
and PE64 inspection disproves the proposed cost: the compiler already replaces
both arrays with guarded, direct unrolled field accesses. There is no
unconditional array materialization to remove. `accept-sparse-*.asm` and
`linked-scope-audit.json` preserve the exact symbols and image identities.
The six ON-path range checks/clears are real; the source arrays are not a
separate runtime cost. No cosmetic source movement is presented as a saving.

## Verification and next gate

Canonical native, x86 Unix and both PE builds pass; only the conformance
executable required rebuilding. Final native verification is 7/7, including
the 23 Python observer tests, native calibration/peer, inline replay, full
snapshot, PE producer views and manifest check. The same three semantic
specs pass in the x86 Unix build. The 16 actual PE pixel processes and eight
retained GT2 diagnostics are separately qualified above.

Inline constants remain default-off. The next useful gate is remaining actual
consumer/layout/failure/capture coverage followed by matching Warn economy
measurements; neither more source-only array cleanup nor a large performance
claim follows from this increment.
