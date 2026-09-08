---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Actual public COM draw-parent economics"
type: evidence
status: measured
updated: 2026-09-09
source: experiments/output/pe-production-parent-H294-20260909/analysis
related: present-pacing-pe-descriptor-inline.293.md; present-pacing-pe-current-hotpath.291.md
---

# Actual public COM draw-parent economics

H294 measures loaded production D3D9 entry points, closing a bounded part of
[H293](present-pacing-pe-descriptor-inline.293.md)'s parent-cost gap. Results
are mixed: the matched-header append penalty is not a universal prediction
of public COM cost, but the actual entry measurements do not justify restoring
the withdrawn fixed-granule candidate either. Production and README stay
unchanged. No new game FPS or exclusive CPU saving is claimed.

## Exact inputs and measurement boundary

HEAD is `b22658a2`; current product sources equal H293's frozen baseline.
`setup.json` binds both immutable H293 manifests, all five images per version,
equal build options, utility members and unchanged bridges/provider. Current
canonical baseline DLLs match the frozen baseline after normalizing only COFF
and CodeView timestamps. Candidate images are counterfactual inputs, not code
restored into the working tree.

The standalone fixture includes public Windows/D3D9 headers only. It calls
`Direct3DCreate9`, requires hardware vertex processing, and invokes
`IDirect3DDevice9` COM methods. It does not use provider C entry points, private
owner layout or a separately compiled recorder implementation. One executable
per PE width is used unchanged against both image versions and repeats.
`public-binding.json` resolves vtable slot 81 from each frozen image to
`D3D9DeviceImpl::DrawPrimitive`: RVA `0x101b0` in PE32 and `0x11040` in PE64.
Runtime module/machine/RVA checks and Wine-root staging receipts bind the
loaded path; the final audit independently checks each image's vtable pointer.

The workload is a 32x32 offscreen A8R8G8B8 target, a real vertex buffer with
two identical transformed green triangles, and FFP diffuse color. There is no
Present. Each group performs legal base setup, Clear, BeginScene and one seed
draw outside timing. QPC surrounds 32 iterations of an API mix, optionally
with DrawPrimitive. EndScene, event-query completion, getters and readback are
outside QPC. A five-second event deadline bounds failure; no unbounded wait
is accepted.

| API mix | Timed setters per iteration |
|---|---|
| steady | None |
| viewport | Alternate viewport width 31/32 |
| viewport_scissor | Alternate viewport and scissor widths; scissor test stays disabled |
| scalar | Texture factor, texture-coordinate index, address mode, world matrix |
| stream_offset | Alternate offsets 0/60 in the same vertex buffer |
| six_constants | VS/PS float, integer and boolean register-zero setters |

These names are API mixes, not observed descriptor cardinalities. Scalar,
stream and float/integer cases start with a same-value setter, then alternate.
Inline constants are disabled, so six constants are not a six-descriptor draw.
Setters-only controls have different pending-state settlement and cannot be
subtracted to obtain an additive draw/append cost.

Both versions use Sikarugir `cx-24.0.7`, offload=1, inline constants=0,
segmented transport=0, CPU-ready tape=0 and index-cache optimization=0.
Cadence is explicitly 256 records / 1,310,720 bytes. Four separate trace
processes, with eight warm and two measured groups per row, prove no recorder
chunk flush between `before_timed` and `after_timed`. Chunk logs are present
outside those windows. Markers and recorder logs share stderr: the conformance
runner otherwise concatenates captured stdout and stderr and loses their
cross-stream order. Trace times do not enter performance comparisons.

## Actual COM results

Eight logging-OFF processes use eight warm and 64 measured groups per row:
PE32/PE64, baseline/candidate, two repeats, with versions alternating within
each width. No builds or tests run concurrently with the timed fixture.
`economics-main.json` preserves all twelve rows per process and individual
repeat values. The table shows setters-plus-draw only, in **ns per iteration**;
an iteration includes all listed setters and one draw, not just DrawPrimitive.
Positive change means slower. Medians are over two process means, not a
confidence interval or workload-weighted estimate.

| API mix + draw | PE32 baseline | PE32 candidate | Change | PE64 baseline | PE64 candidate | Change |
|---|---:|---:|---:|---:|---:|---:|
| steady | 470.19 | 471.66 | +0.31% | 305.71 | 321.02 | +5.01% |
| viewport | 512.23 | 512.55 | +0.06% | 343.97 | 353.17 | +2.68% |
| viewport_scissor | 560.89 | 580.69 | +3.53% | 372.14 | 371.34 | -0.22% |
| scalar | 939.21 | 896.34 | -4.56% | 553.91 | 527.39 | -4.79% |
| stream_offset | 618.90 | 640.60 | +3.51% | 431.91 | 412.38 | -4.52% |
| six_constants | 1783.40 | 2011.94 | +12.81% | 1220.48 | 1150.81 | -5.71% |

PE32 viewport/scissor and six-constant draw rows have non-overlapping repeat
ranges; PE64 steady and viewport draw rows do too. All other draw rows have
overlapping ranges. Setter-only changes are also mixed and are retained as
controls, not a decomposition. Two processes are insufficient to isolate
layout, scheduling or thermal effects, or attribute six-constant cost to the
descriptor loop alone. The result withholds promotion, not a universal
regression verdict for every application.

Wine reports a 10 MHz QPC and nonzero `GetThreadTimes` deltas in these runs.
The independent empty QPC pair is recorded, never subtracted. Thread CPU is
one aggregate for the row sequence, including per-group setup, warmup, drains
and oracles. It excludes initial device/resource creation and final teardown;
it is neither a per-row nor exclusive PE CPU measurement. Unsupported, zero
or nonmonotonic clocks remain explicit parser states rather than CPU savings.

## Qualification, restoration and remaining scope

All twelve rows require exact COM call counts, zero HRESULT failures, final
public getter checks, green/black pixel samples and device Release zero.
These are bounded FFP/state checks: shader constants unused by this FFP draw
are checked through getters, not shader execution. Two pixels do not establish
complete image parity, exactly-once drawing or general resource lifetime.

Initial `main1`/`main2` trace attempts passed row oracles but ended with device
Release=2, and are preserved and excluded. Merely adding a query fence did
not fix teardown. Final `main3` follows the existing inline-constants fixture:
avoid acquiring an implicit backbuffer solely for cleanup, unbind/drain,
release caller-owned resources, Reset, then require Release=0. This removes
the fixture failure; it is not a new product lifetime proof or product fix.

`parser-pins.json` passes one valid trace and rejects thirteen corrupt cases,
including timed flush, missing chunk evidence, wrong calls/pixels/groups,
duplicate rows, missing markers, wrong machine, skip, cleanup failure and
trace-as-perf. No native product or TLA rerun is claimed: product and lifecycle
transitions are unchanged; H293's previous checks remain historical evidence.

`final-audit.json` binds fourteen qualified processes: four initial traces,
eight performance processes and two final baseline restoration traces. It
checks immutable inputs, fixture/parser identities, actual public vtable
targets, unchanged product sources and all five baseline images restored in
the shared Wine root. No worker subsequently changes those receipts.

H293's surrogate is useful for its own layer, but its percentages must not be
reported as production draw overhead. The next decision needs genuine
production descriptor/cardinality and retain/settlement context, followed by
the same public-entry gate; this FFP fixture supplies neither GT2 weighting nor
a safe count-threshold policy. Dense/indexed/UP production entry economics,
exclusive CPU reduction and repeatable README FPS recovery remain open.
Latest OFF game spots remain H290 05 GT2 29.809 FPS and H291 06 GT2 18.217 FPS;
the latter retains its documented 34-frame full-health coverage gap.
