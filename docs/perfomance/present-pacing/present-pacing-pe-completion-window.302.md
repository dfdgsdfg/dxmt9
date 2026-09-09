---
domain: present-pacing
workload: actual PE public COM
title: "Bounded completion windows for PE draw-parent calibration"
type: evidence
status: measured-runtime
updated: 2026-09-09
source: experiments/output/pe-completion-window-H302-20260909/analysis
related: present-pacing-pe-draw-parent-tail.300.md; present-pacing-pe-qualified-pin-output.301.md; present-pacing-pe-current-hotpath.291.md
---

# Bounded completion windows

H302 at parent `8b1f14de` changes only the public-COM fixture and its evidence
tools, not production query, queue, recorder or encoding policy. All five
product images remain byte-identical to H301. The new opt-in window makes
same-image measurements substantially more repeatable, but does not qualify
every small helper delta or establish any production CPU/FPS improvement.

## Contract and verification

`run_pe_draw_parent_economy.py --completion-window N` accepts 1–64, default 1.
Values above one require group/tail detail and schema 4. The fixture keeps
each group's setup, Clear/BeginScene/seed, 32 timed iterations and EndScene.
Only the fixture event Issue/GetData is deferred. Warm and measured phases
reset the window independently and always complete their final partial group
before the next phase or final getter/input/pixel checks. Failure cleanup
retains its final completion attempt before releasing row resources.

The fixture-used pure decision helper has independent native enumeration of
all windows through 64 and warm lengths through 128, plus representative
measured lengths through 8192, empty warm phases and partial boundaries.
The schema-4 parser independently derives boundaries, requires boundary
polls, rejects interior Issue/GetData time or calls, and preserves exact
sample order, coverage and tail conservation. Default schemas 1–3 are unchanged.
See R-BENCH-7.7 and `draw_parent_completion_policy.hpp::shouldDrain`.

Four canonical builds pass. Host focused tests pass 4/4, including all 26
parser/CLI cases; x86_64-native checks pass 2/2. Twelve actual PE trace
processes cover schemas 1–4, windows 1/8/32/64, zero warm groups, and final
partial windows. They qualify 2,322 timed group boundaries with no recorder
flush inside the setter/draw interval. Eighteen logging-OFF timing processes
use 512 measured + eight warm groups per cell, 32 iterations per group.
Each width/window has three same-image repeats; the frozen order rotates
windows and PE widths. No concurrent build, native test or second benchmark
is launched during measurement. Before/after process snapshots are retained;
they are not continuous proof of a quiet host.

All 30 processes pass 810 parsed state/input/HRESULT/pixel/release cells:
324 structural trace cells and 486 timing cells. Only the latter contribute
to the economics below. The strict
reparse conserves 254,880 group rows and 254,718 tail supplements. All 16
H300/H301 historical logs reparse to their original stored JSON. The driver
binds fixture source and policy header, executable, public COM slots, five
product images, three actual staged images, runtime, tools and build/source
receipts. `economics-main.json` retains per-run identities and native logs;
the raw logs and frozen plans live in its parent directory.
`final-audit-main.json` additionally checks H301 equivalence by product role,
unchanged production sources, full-build success and final code/doc identities.

## Same-image calibration

The table reports the median of three whole-matrix totals, including warm
groups. Process CPU includes all fixture threads, setup, drains and oracles;
parent QPC is wall time, not exclusive PE CPU.

| Width/window | Parent QPC (s) | Outer tail (s) | Row process CPU (s) | GetData calls per run |
|---|---:|---:|---:|---:|
| PE32 / 1 | 0.476730 | 20.282838 | 17.97 | 14,040 |
| PE32 / 8 | 0.268732 | 7.770743 | 6.66 | 1,755 |
| PE32 / 32 | 0.220731 | 6.343511 | 5.30 | 459 |
| PE64 / 1 | 0.284458 | 19.238197 | 16.50 | 14,040 |
| PE64 / 8 | 0.168663 | 8.081052 | 6.62 | 1,755 |
| PE64 / 32 | 0.134672 | 6.534123 | 5.22 | 459 |

Every boundary completes on its first GetData call; no retry poll is omitted.
Window 32 still spends median 6.092/6.346 seconds in GetData on PE32/PE64.
EndScene remains 0.244/0.184 seconds, Issue 0.00203/0.00136 seconds. Fewer
completion fences do not remove the rendering work or its final completion.
The remaining GetData internal wait/work split is not measured here.

For each of the 18 draw cells, repeat spread is `(max / min - 1) * 100` over
its three run means. The next table summarizes those 18 spreads; it is not
a confidence interval or a timing-sample percentile. No slow sample is removed.

| Width/window | Median draw-mean spread | Largest draw-mean spread |
|---|---:|---:|
| PE32 / 1 | 118.784% | 196.717% |
| PE32 / 8 | 6.814% | 36.482% |
| PE32 / 32 | 0.653% | 8.755% |
| PE64 / 1 | 125.378% | 202.959% |
| PE64 / 8 | 8.600% | 40.106% |
| PE64 / 32 | 0.561% | 6.970% |

Window-32 whole-matrix parent ranges are 1.102% on PE32 and 0.340% on PE64;
row process-CPU ranges are 0.377% and 0.768%. Aggregate stability does not
erase cell-level tails: PE32 `vs_f3_changed/indexed` means are
745.941/687.305/685.889 ns per iteration although all group medians are
684.375 ns. PE64 `vs_f4_changed/primitive` means are
411.133/439.789/411.377 ns. These samples remain in every sum. Per-cell group
median repeat spreads reach 3.232% on PE32 and 1.550% on PE64. Switching to
medians alone would conceal some remaining economic uncertainty.

## Disposition and next decision

Retain default window 1 for compatibility and window 32 as an explicitly
selected diagnostic recipe for future matched-shape comparisons. This is
useful calibration evidence, not universal sub-percent resolution. A candidate
must exceed its own same-image control spread, preserve native/state/pixel
checks and show the intended change in the loaded PE32/PE64 code. If not,
stop the small-delta claim; do not infer a gain from fewer instructions alone.

No new TLA transition or wild FPS run is claimed: production images are
unchanged and this is fixture pacing. README and H301's latest wild evidence
remain unchanged. FFP final pixels do not prove intermediate frames,
programmable shaders, UP or game-representative resource/state distributions.

The next substantive PE question is whether one scoped source borrow can
replace admission/writer re-querying without crossing mutation, retain,
rollback or settlement boundaries. H290's deliberately fresh writer checks
cannot simply be deleted. Rejected first-use publication and descriptor-copy
changes remain rejected (H289/H292/H293). A small unreachable full-snapshot
branch cleanup is separate and should stop if the compiler already folds it.
No follow-up production implementation is included in H302.
