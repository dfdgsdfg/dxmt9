---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Retained stream binding reuse and refreshed PE attribution"
type: evidence
status: bounded-setter-economy
updated: 2026-09-08
source: experiments/output/pe-owner-hotpath-20260908/analysis; experiments/output/app-d3d9-3dmark{05,06}-pe-owner-hotpath-9eaf8965-gt2-off-fullselfpc-r1-20260908; experiments/output/app-d3d9-3dmark{05,06}-pe-stream-binding-9eaf8965-gt2-off-perf-r1-20260908
related: present-pacing-pe-layout-dynamic-economy.284.md; present-pacing-pe-post-lifetime-profile.276.md; ../../../specs/d3d9/recorder/requirements.md
---

# Retained stream binding reuse

H285 extends R-CORE-REC-7.58's retained-binding qualification to
`D3D9DeviceImpl::SetStreamSource`. The same strongly retained VB no longer
reconstructs its wrapper membership on every call. Warm actual-PE32/PE64 COM
measurements show a bounded setter saving, including offset/stride changes.
This is not a demonstrated whole-PE CPU saving or README FPS recovery.

## Semantic scope

The slot's strong public VB pointer and validated wire identity are the
authority. Reuse requires a non-null exact pointer match, writable Live state,
non-Recording phase, expected owner and valid BUFFER wire shape. The existing
pure `peSemanticCanReuseRetainedBinding` moves unchanged into the narrow
`src/d3d9/d3d9_pe_retained_binding.hpp`; no new cache or lifetime token is added.

Matching the entire buffer/offset/stride tuple is a no-op. A same-VB layout
change still executes the original typed `bindStream` transition and hot-setter
dirty accounting; only offset/stride are written. The strong pointer, wire row
and reference multiplicity do not change. Null, replacement, foreign,
Recording and malformed-wire cases retain full validation. Stream-index and
outer thread/MT/poison checks remain in their original order.

All ordinary, StateBlock, temporary SWVP and reset writers must maintain the
strong-reference/wire correspondence. Shape validation cannot certify arbitrary
nonzero memory corruption. Borrowed vertex declarations remain excluded.
PE/unix ABI, emission, resource lifetime, replay/encode policy and provider
defaults are unchanged.

## Actual public COM economics

`analysis/stream_binding_economy.cpp` calls the public virtual setter through
the loaded D3D9 device. It does not time a copied helper. The canonical
conformance runner stages the frozen before/after DLLs, validates their PE
width/postprocessing and retains staging receipts. Every source and actual
Wine destination hash is checked after the run. Fixture/source/driver hashes
reconcile across all eight usable processes in `stream-economy-qualified.json`.

Each case has 10,000 warmup iterations and three million timed iterations;
the two-slot case issues twice as many COM calls. Initial binding, tuple/getter
and reference checks, and a Recording/Apply check are outside timing. All
processes select hardware vertex processing (`0x40`). QPC includes the fixture
function-pointer call, loop, HRESULT aggregation and host scheduling; it is
not thread-exclusive CPU time. No diagnostic timers or sampler are enabled.

Order is baseline/candidate/candidate/baseline per PE width: x86 baseline
`r2`, candidate `r1`, candidate `r3`, baseline `r3`; then x64 baseline `r1`,
candidate `r1`, candidate `r2`, baseline `r2`. The first x86 baseline `r1`
failed the staging-stamp precondition before launch and is excluded. Frozen
baseline copies were re-postprocessed with the canonical Wine tool, with
unchanged full hashes; that repair receipt is preserved.

Mean QPC nanoseconds per COM call, two processes per image/width:

| Case | PE32 before → after | PE64 before → after |
|---|---:|---:|
| Same VB, same tuple | 19.789 → 14.095 | 12.561 → 9.826 |
| Same VB, changing offset | 34.723 → 15.900 | 21.851 → 12.024 |
| Same VB, changing stride | 33.741 → 16.085 | 21.032 → 10.157 |
| Two slots, same VB per slot | 17.999 → 11.822 | 11.568 → 7.819 |
| Alternating A/B buffers | 33.539 → 33.210 | 21.352 → 21.326 |
| Null, nonzero tuple | 16.067 → 15.837 | 10.127 → 10.235 |
| Null, zero tuple no-op | 14.026 → 14.017 | 9.740 → 9.930 |

Both repeats improve the targeted paths. Companion paths have small mixed
movement, including a repeat-consistent PE64 null-zero increase of 0.189 ns
(1.94%); do not describe every path as faster. Whole-workload binding-hit
distribution and cache/instruction-cache effects were not measured, so these
means cannot be weighted into a frame-time saving or net all-workload economy.

## Code shape and evidence boundary

The complete linked `SetStreamSource` body grows:

| Metric | PE32 before → after | PE64 before → after |
|---|---:|---:|
| Bytes | 2039 → 2649 | 2418 → 3034 |
| Instructions, excluding linker fill | 499 → 648 | 476 → 605 |
| Static direct call sites | 35 → 35 | 35 → 35 |

PE32 prologue stack subtraction grows 208→224 bytes with the same pushes and
alignment. PE64 subtraction grows 352→360 bytes, but one saved GPR push is
removed, keeping the combined prologue allocation at 408 bytes. These are
linked prologue observations, not whole-call maximum stack bounds;
`stream-stack.json` and exact disassemblies retain the distinction.

These totals include ordinary, diagnostic and fallback branches, not only the
taken warm path. All eleven other audited hot-entry metric rows are unchanged;
exports and QueryInterface/vtable/RTTI archive ownership match. No claim of
complete binary instruction equivalence follows from equal summary metrics.
`candidate-codegen.json` preserves both images and the remaining fallback
validator; static call presence is not a runtime-hit count.

The bounded model uses the production reuse predicate against an independent
always-validate reference: 432 one-step combinations plus an ordered two-slot
A/B/A trace compare result disposition, tuples, dirty/Recording effects and
Live reference multiplicity. Per-step expected-reuse assertions prevent an
always-false guard from passing vacuously. Malformed wire, stream 16, null
deactivation, foreign objects and write denial are explicit cases.

Native arm64 6/6 and x86_64 4/4 focused suites pass. Both actual PE widths pass
all eight selected stream COM tests, including alignment, null/zero-stride
policy and exact reference accounting across tuple changes/getter/unbind.
The existing TLA suite passes as a regression check; no new concurrency
transition or end-to-end model proof is claimed. The value model does not
model the complete Recording reference owner, and public conformance does not
inspect internal pending masks or exercise every foreign wrapper.

Four further actual PE processes run the same delayed-readback pixel fixture:
frozen baseline and candidate, each PE32/PE64. One managed VB contains packed
and padded geometry. Six non-indexed draws cover initial/exact-repeat,
offset A→B→A and same-offset stride 16→64; a seventh draw is indexed. Additive
ONE/ONE blending makes A's third contribution observable, so an earlier A
draw cannot mask a lost return transition. Interior probes distinguish A, B,
both strides and the indexed triangle. One readback follows all seven draws.
All 20 exact pixel observations match the independent expected colors and
each other; VB, IB and device release receipts are zero. This is bounded FFP
geometry/binding evidence, not a programmable-shader or concurrent Reset proof.
`analysis/pixel-runs/` preserves the four recipes, logs, parsed probes and
source/fixture/driver/staged-image hash checks. The fixture is an ignored
experiment artifact; the native and COM pins, not this auxiliary executable,
are currently part of the checked-in test suite.

## Fresh PE attribution, before this setter change

Two full-self-PC 250 Hz diagnostic runs use the clean H284 image `9eaf8965`.
They have complete self-PC bucket reports, no omitted/overflow samples or
suspend/context/resume failures, and exact five-image provenance. 05 covers
the cumulative prefix through Present 1860. 06 differences Present 900→2040,
approximately bracketing scene frames 954–2086; it is not exact scene-time
isolation. The 64-byte PC buckets are not call stacks or CPU milliseconds.

| Lexical exclusive group | 05 prefix samples | 06 prefix samples |
|---|---:|---:|
| All selected game-thread stops | 13,562 | 13,300 |
| Stops inside PE `d3d9.dll` | 1,302 | 834 |
| Semantic owner admission/emission | 482 | 348 |
| Constant shadow/setter/flush | 234 | 123 |
| Retainer | 35 | 23 |
| Sparse plan/destination/settlement | 11 | 7 |
| Autogen | 3 | 1 |
| Other unambiguous PE | 171 | 121 |
| Cross-symbol ambiguous, unassigned | 366 | 211 |

The owner grouping includes lexical template scopes and must not be read as
inclusive attribution. Ambiguous buckets remain unassigned. `SetStreamSource`
has only 8/9 unambiguous samples, the VB validator 6/2, and `bindStream` 4/3:
this motivates a bounded reusable qualification, not a claim that streams
dominate the producer. Diagnostic suspension changes the workload; no FPS
comparison uses these runs. Reported health counters are zero, with 46
unreported tail frames in 06.

The next measured-work candidates remain complete compact admission,
source/destination settlement and constant handling. Warm-retainer expiry is
another bounded candidate, not the dominant sampled category: its no-expiry
shortcut already exists, while expiry compacts entries and rebuilds the index.
Any incremental index update must first price expiry/survivor/tombstone shapes
and retain release order, warm epochs, rollback, identity and capacity rules.
The layout-only opportunity from H284 remains about 15–17 microseconds/Present;
it does not explain the whole README gap.

## Sampler-OFF GT2 spot checks

All runs use Sikarugir `sikarugir-cx-24.0.7`, actual PE32, offloaded replay ON,
inline constants/segmented transport/CPU-ready tape/index reorder OFF. The
catalogue `perf` profile uses Warn logging and frame sampling 1, with no PE
sampler, phase observer, external CPU observer or gputrace. 05 is 1024×768;
06 is 1280×720 with focus-free named GT2 autorun.

| Image / product | Scene frames | Wall interval | Scene FPS | Versus README |
|---|---:|---:|---:|---:|
| Clean H284, 06 GT2 rerun | 1144 (372–1515) | 64.195 s | 17.821 | −1.54% vs 18.1 |
| H285 candidate, 05 GT2 | 1921 (1–1921) | 64.604 s | 29.735 | −1.21% vs 30.1 |
| H285 candidate, 06 GT2 | 1165 (988–2152) | 64.076 s | 18.181 | +0.45% vs 18.1 |

These are `frames / sum(wall_ms)` scene samples, not benchmark-owned `.3dr`
results or the runner's process-elapsed FPS. 05 selects all positive-wall
frames; 06 selects the contiguous positive-wall frames with `draw_calls > 100`
and retains its loading-tail mixture. The current-image rerun supersedes the
earlier user-interfered 06 measurement for this check. No inference here uses
that interrupted population or the separate diagnostic profiles.

All three have `status=pass` and zero reported chunk rejects, GPU errors,
pipeline-build failures and no-pipeline skips. Both candidate runs finish
without timeout and have complete final counter coverage (through Present
1922/2153). The clean H284 06 GUI run reaches the permitted timeout after its
useful scene; counters end at Present 1500, leaving 15 frames outside the
health snapshot. Five-image before/after/staged hashes reconcile throughout.

The roughly +2.02% adjacent 06 candidate-versus-rerun difference is descriptive
only. One run per image is not a calibrated A/B, CPU attribution or durable
README recovery. No fresh sampler-OFF 05 baseline was collected in this
increment. README remains unchanged.

## Provenance

Candidate runtime is `9eaf8965` plus the three-source-file patch identified by
SHA-256 `87566e56d7ccbafc266fd3eeb9f1dde11ae2b21625f55c434ac597babdeb9be1`
in `candidate-runtime.json`. PE32 changes from `28217ceb5e1c1f53` to
`dfa760cf99cd9c6d`; PE64 from `844ab083dd703576` to `c13b30ce6fac93ee1`.
Both bridges and the Unix provider are byte-identical. Recipes retain full
hashes, not just these display prefixes. Initial include/build and codegen-tool
setup failures remain alongside corrected successful logs.

`final-audit.json` binds the runtime patch, native/TLC and COM receipts,
eight micro processes, four pixel processes, qualified scene artifacts and
all five restored current Wine images. Later measurements must restage their
own intended image set; a frozen baseline copy is not an isolated Wine root.

Recompute the diagnostic joins with `qualify_profiles.py` and
`join_profiles_main.py`; `qualify_stream_economy.py` reconciles the eight
actual COM micro runs without launching Wine. Runtime and evidence scripts
are frozen in the ignored analysis directory; the checked-in native and COM
tests retain the relevant semantic pins.
