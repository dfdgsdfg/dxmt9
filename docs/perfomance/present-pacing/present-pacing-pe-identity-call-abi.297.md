---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Compact identity private-call ABI"
type: evidence
status: measured-runtime
updated: 2026-09-09
source: experiments/output/pe-identity-call-abi-H297-20260909/analysis
related: present-pacing-pe-warm-pin-identity.296.md; present-pacing-pe-compact-identity-insertion.295.md; present-pacing-pe-public-com-parent.294.md
---

# Compact identity private-call ABI

H297 starts at `06bd8063`. H295's always-wire specialization still passed two
unused runtime arguments to its outlined identity helper: visibility `true`
and a null promotion-result pointer. This change removes those arguments from
the compact private signature. Generic insertion supplies a compile-time-checked
`bool, bool*` pack; compact insertion supplies no optional arguments. Both
execute the same identity transition. Public methods, witness layout, COM,
wire ABI, resource retention and queue scheduling remain unchanged.

R-CORE-REC-7.67 now requires the generic visibility input to be captured by
value before output initialization, including aliases. Native cases pin
input/output aliases, shared retained/promotion outputs, omitted ordinals,
duplicates and malformed-null failure. Existing bounded transition,
capacity/rollback, seven-family byte/refcount and source-mutation checks remain.
This is a local representation refinement, not a new concurrency proof.

## Linked implementation and correctness

All four canonical builds complete. Host native checks pass 8/8 and the
x86_64-native subset passes 4/4, including install-name validation. The existing
`dxmt9-verify-tla` regression passes in 147.34 seconds. Frozen full-image hashes,
source snapshots and build-option/coredata identities bind both versions.
Across the snapshotted `src`, `include` and `cross` trees, the only changed
product file is `src/d3d9/d3d9_pe_semantic_owner.hpp`; both bridge DLLs and the
Unix provider are unchanged.

| Linked scope | PE32 baseline → candidate | PE64 baseline → candidate |
|---|---|---|
| Compact explicit arguments | 6 → 4; `retl $0x18` → `$0x10` | 6 → 4; two dummy setups removed |
| Compact helper span / raw instructions | 432 / 128 → 432 / 128 | 352 / 98 → 352 / 96 |
| Generic helper span / raw instructions | 448 / 147 → 448 / 147 | 368 / 109 → 368 / 112 |
| Compact caller lambda span / raw instructions | 256 / 94 → 256 / 96 | 304 / 93 → 288 / 91 |
| Compact caller stack subtraction | 24 → 24 bytes | 72 → 56 bytes |

The instruction counts include alignment, not cycles or executed paths.
Excluding `int3` and `nop*`, the generic helpers stay at 135/106 instructions
on PE32/PE64. The caller lambda decreases 83 → 81 and 86 → 83 respectively.
The generic pack adds no helper calls or separately materialized tuple frame;
PE64 retains six register pushes and three stack-argument loads. Argument
reordering still changes register allocation, so identical counts are not a
CPU-neutrality proof. The selected top-level admission body retains its span,
instruction count and three calls; all five H296 pin controls retain their
span/count metrics. No generic-parent call-context claim is made.

The selected linked receipts are `linked-identity-abi-baseline-r2.json` and
`linked-identity-abi-candidate.json`. The earlier baseline audit is retained,
not silently overwritten. Full PE32/PE64 candidate image SHA-256 values are
`d720dd8d31ba9c836109a8ff98a826fc178caa9101677bdb426b5172fa22b479` and
`9cf1af7eda0524cfe5973a3d09bb63cb78c4428470dc1ae3bba1a1bf7e07076e`.

## Actual PE measurement

Twenty actual PE processes pass: four boundary traces and sixteen logging-OFF
timing processes, four per version/width. H294's frozen public COM fixture
checks twelve getter/FFP-pixel rows per process, with no private-entry calls or
matched-header surrogate. Separate traces establish no timed recorder flush.
QPC covers setter-only or setter-plus-draw groups, not exclusive helper CPU;
setup, eight warmup groups, query drains and readback are outside QPC.

The fixed recipe is 64 groups of 32 iterations, 256 records/1,310,720 bytes,
replay offload on, and inline/segmented/tape/index-cache, PE diagnostics and
perf counters off. PE32 runs under Sikarugir WoW64/Rosetta. PE64 is a separate
width, not native ARM64 PE. The first two repeats run baseline then candidate;
the next two use candidate-r3, baseline-r3, baseline-r4, candidate-r4, first
PE64 then PE32. No build, native test or second benchmark overlaps a run.

| Setter + draw mix | PE32 baseline/candidate ns | Delta | PE64 baseline/candidate ns | Delta |
|---|---:|---:|---:|---:|
| Steady | 466.14 / 468.60 | +0.53% | 307.32 / 303.00 | -1.41% |
| Viewport | 512.08 / 509.01 | -0.60% | 347.61 / 339.23 | -2.41% |
| Viewport + scissor | 544.48 / 570.78 | +4.83% | 380.44 / 377.17 | -0.86% |
| Four scalar states | 909.52 / 945.21 | +3.92% | 561.94 / 550.61 | -2.02% |
| Stream offset | 650.95 / 667.55 | +2.55% | 432.91 / 413.92 | -4.39% |
| Six constant registers | 2,018.51 / 1,986.84 | -1.57% | 1,300.07 / 1,268.90 | -2.40% |

These are medians of four process-level QPC aggregate values. Initial PE32
scalar, stream and viewport/scissor penalties of 4–5% triggered the additional
repeats. All PE32 ranges now overlap, but positive medians remain warnings;
overlap is not a neutrality proof. PE64 steady and viewport draw ranges do not
overlap, while the other four do. Setter-only ranges all overlap, including
PE32 constant median +7.32%. This bounded fixture does not establish whole-game
weights, causal helper savings or a general performance-neutrality result.
No empty-loop subtraction or weighted aggregate speedup is claimed.

API CPU totals are 470–500 ms candidate versus 460–480 ms baseline on PE32,
and 440–480 versus 460–480 ms on PE64. Sikarugir supplies a coarse process-time
fallback, including setup/warmup/drains, not exclusive PE/thread CPU. Lower
CPU usage is not established.

Both-width actual pixel baseline/candidate runs pass the exact four-function
verdict. Each yields 24 signatures of four samples, with candidate values
exactly matching baseline. This is bounded pixel evidence, not a proof of all
game frames or concurrency schedules. Timing and pixel receipts preserve
fixture, source, driver/parser and staged-image identities. Historical Wine
destination hashes are captured per run, not inferred from the final shared
destination after another version is staged. Public COM vtable/RVA binding is
checked against each frozen image.

## GT2 spot checks and disposition

One candidate run per product uses the canonical Sikarugir catalogue runner,
perf/warn profile, validation off, frame sampling on and PE observers off.
Replay offload is on; inline, segmented, tape and index-cache are off. The
05 focus-keepalive helper is explicitly disabled. No manual window operation,
build, test or second benchmark overlaps either run.

| Product | Resolution | Consecutive scene frames | Scene wall | Sampled FPS | README delta |
|---|---|---|---:|---:|---:|
| 05 GT2 | 1024x768 | 1–1947 (1,947) | 64,615.138 ms | 30.132 | +0.11% vs 30.1 |
| 06 GT2 | 1280x720 | 960–2128 (1,169) | 64,082.718 ms | 18.242 | +0.78% vs 18.1 |

Positive-wall rows with more than 100 draws select the consecutive scene,
excluding loading without trimming slow scene frames. This is elapsed scene
FPS, not the process-elapsed estimate or an observer-free benchmark score.
05 exits normally and its `.3dr` is recovered. 06 Advanced Edition produces
no `.3dr`; the catalogue permits its post-scene GUI timeout/143. The saved
captures contain rendered scenes, not a black window; a screenshot is not
proof of full-scene temporal correctness.

All eight reported chunk/GPU/pipeline health counters are zero. 05 has a full
snapshot through frame 1948. 06 has one through frame 2100, leaving 28 final
scene frames without that full snapshot. Per-frame GPU-error samples cannot
close the missing health categories. These are fresh runs; earlier
user-interrupted measurements are not incorporated or averaged into them.

Both PE images, both bridges and the Unix provider match the frozen candidate
and shared Wine root after both runs. Recipes, captures, the recovered result,
logs and frame tables remain in the ignored artifact tree.
`final-audit-main.json` binds their hashes, the twenty-process economy matrix,
four pixel processes, native/TLA and linked receipts, source snapshots and
this documentation. README remains unchanged.

Independent review supports retaining the bounded local call-ABI refinement,
without performance promotion. PE32 positive draw-cost medians remain
warnings despite overlapping ranges. Near-README single spots do not prove
causal FPS gain, neutrality or reduced CPU usage. Remaining work is stronger
exclusive/whole-parent attribution and current workload-shape qualification,
not restoration of a second identity authority or arbitrary helper inlining.
