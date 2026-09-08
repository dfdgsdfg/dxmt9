---
domain: present-pacing
workload: actual PE public COM; 3DMark05 GT2; 3DMark06 GT2
title: "Draw-parent group diagnostics: drain-dominated fixture, shifting populations"
type: evidence
status: diagnostic-qualified-economics-unqualified
updated: 2026-09-09
source: experiments/output/pe-draw-parent-groups-H299-20260909
related: present-pacing-pe-draw-parent-harness.298.md; present-pacing-pe-identity-call-abi.297.md
---

# Draw-parent group diagnostics

H299 starts at `316c74d6`. It changes only the test harness and its contracts;
all five H297/H298 production images remain byte-identical. It investigates
H298's noisy public-COM measurements before pricing another private binding
helper. It does not measure a new product optimization or update README FPS.

## Measurement boundary

R-BENCH-7.5 adds default-off `--group-detail`. A bounded buffer is allocated
before row timing, then preserves every warm/measured group's original QPC
interval. A separate pair measures the tail from EndScene through event Issue
and the GetData/Sleep(0) drain. The existing tail HRESULT count gives the number
of GetData calls. No extra clock, allocation or sample output runs inside the
setter/draw interval; all samples print after the full matrix's CPU captures.

Detail schema 2 preserves case/path/ordinal, warm/measured classification,
unfiltered ticks and poll counts. The strict parser requires complete ordered
coverage, exact measured-sum conservation and phase-separated nearest-rank
distributions. Traced sample output must follow the complete trace loop and
precede all rows. Aggregate mode retains schema 1. Additional tail clocks and
buffer stores still perturb scheduling outside the timed pair: detail numbers
are diagnostics, not interchangeable H298 aggregate costs.

The unchanged recipe uses nine cases × setter/primitive/indexed paths, 32
iterations per group, 512 measured + eight warm groups, Sikarugir PE32
WoW64/Rosetta and PE64. Recorder logging is off in the six detail timing runs;
four separate short traces qualify both aggregate and detail modes. No build,
native test or second benchmark overlaps these runs.

## Evidence

Ten PE processes pass all 270 state/input/HRESULT/pixel/release cells. Four
traces each contain 324 ordered markers and 195 recorder chunks, with no flush
inside a timed pair. Two detail traces preserve 81 groups each; six timing
processes preserve 14,040 groups each: **84,402 samples**, without truncation
or outlier removal. `group-analysis.json` requalifies the raw logs against
their recipes, staged images, loaded public COM bindings and matched
source/build/tool identities.

All four canonical builds complete; four focused Meson checks pass, including
18 parser/CLI tests. Negative pins cover missing/moved/duplicate samples,
incorrect phases, sum and integer overflow, version mixing and trace-time
publication. A separate main-thread audit re-parses ten frozen H298 receipts
to exactly equal their original schema-1 parsed values; this is artifact
validation, not a committed historical native fixture. No production semantic
transition changed, so this round does not claim new TLA or wild evidence.

## What causes this fixture's uncertainty?

| Six same-image diagnostic runs, three per width | PE32 | PE64 |
|---|---:|---:|
| All setter/draw group QPC, including warm groups | 0.520–0.649 s | 0.378–0.381 s |
| EndScene + Issue + GetData tail QPC | 22.637–25.787 s | 23.175–24.153 s |
| Tail fraction of those two disjoint QPC scopes | 97.54–97.75% | 98.39–98.46% |
| Summed whole-row process CPU | 20.73–23.89 s | 20.63–21.87 s |
| GetData calls per process / groups per process | 14,040 / 14,040 | 14,040 / 14,040 |

Process CPU includes setup, warm-up, drain and oracle work across process
threads; it is not exclusive PE CPU or the same scope as either QPC column.
Tail percentages exclude setup and the canonical runner's staging/startup/
teardown. They do **not** locate the bottleneck in GT2 or prove GPU-boundness.

Every successful group completes with exactly one GetData call, so the
fixture's `Sleep(0)` retry loop is not exercised. Replacing that sleep cannot
fix this observed shape. The tail contains three public calls; its current
bracket cannot assign the time to any one of them.

The source path explains why a single call need not be cheap. PE
`D3D9DeviceImpl::EndScene` in `src/d3d9/d3d9_pe_device.cpp` flushes the recorder;
its bridge/core scene transition itself is not drain-fenced. Query Issue
normally appends an ordered record. Query GetData in
`src/d3d9/d3d9_pe_device_child_misc.cpp` flushes the child recorder, then its
bridge in `src/d3d9/device_c_bridge_swapchain_query_stateblock.cpp` can drain
the replay worker. `Device::getQueryData` in `src/d3d9/core_resources.cpp` then
calls the production provider's flush. `CommandQueue::submitFlush` reaches
`QueueLifecycleController::flushAndWait` / `waitForSequence` in
`src/dxmt9/dxmt9_queue.cpp`, waiting for the published completion watermark.
Only the final inner `Query::getData` predicate is nonblocking; the whole
public FLUSH call is not. These are source-verified synchronization candidates,
not measured attribution of the 22–26 seconds to individual waits.

The variation is not just a few isolated slow groups. For primitive/indexed
rows, the slowest six of 512 measured groups (1.17%) account for only
1.31–3.82% of each row's total time. Meanwhile, across repeats:

| Draw-row `(max - min) / median` | PE32 | PE64 |
|---|---:|---:|
| Unfiltered row mean per iteration | 4.40–174.47% | 8.57–146.14% |
| Per-run group p50 per iteration | 0.39–218.94% | 2.52–256.38% |

These are range ratios across three runs, not confidence intervals. Central
populations shift as well as means; switching to a favorable median would
hide uncertainty rather than resolve it. The analysis retains an exploratory
within-row correlation of the preceding tail with the next measured group.
That association is not a causal attribution to the scheduler, CPU frequency,
Wine, driver or any particular PE helper.

The next bounded measurement is to subdivide EndScene, Issue and GetData in
the diagnostic fixture before changing its pacing policy. The current receipt
supports neither an unused-pin-output economics decision nor a production
query-policy change. Programmable-shader/UP/app-local coverage and exclusive
PE CPU attribution remain separate open items. H297 is still the last wild
FPS evidence, including its documented 06 health-tail limitation.
