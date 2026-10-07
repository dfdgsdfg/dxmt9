---
domain: present-pacing
workload: STALKER Day high, corrected-provider visibility OFF/ON, late camera-matched CPU
title: "Visibility reduces observed Encode demand, but the OFF repeat fails stability"
type: evidence
status: finite-four-run-comparison-closed-repeatability-rejected
updated: 2026-10-08
source: traces/stkcop-visibility-budget-h468/{contract.md,images.json,run.py,compare_cpu.py,summarize.py,summary.json,forward.json,reverse.json,off-repeat.json,on-repeat.json}; traces/stkcop-visibility-budget-h468/{dxmt9-r1,dxmt9-visibility-r1,dxmt9-visibility-r2,dxmt9-r2}/{receipt.json,executed-runner.py,progress.jsonl,analysis.json,tail.png}; experiments/output/app-d3d9-stkcop-bench-visibility-budget-h468-{dxmt9-r1,dxmt9-visibility-r1,dxmt9-visibility-r2,dxmt9-r2}/result.json
related: present-pacing-positiont-pixel-center.466.md; present-pacing-default-unwind-feasibility.467.md; present-pacing-completed-workload-visibility.455.md
---

# A concrete effect observation, not an accepted removable CPU budget

Exactly four catalogue runs complete normally in OFF, ON, ON, OFF order.
Both directional comparisons show lower native Encode CPU per completed SASH
frame with real visibility. However, the OFF-repeat control fails the
predeclared +/-5% stability gate. The finite experiment is closed with
`repeatable_effect=false`; do not add replacement runs or relax that gate.

## Scope and provenance

All runs use the same frozen five renderer images, game, R2 renderer, observer
executable, executed supervision script and workload configuration. Day/high,
1280x720, `-max-threads 2`, `snd_targets 16`, draw command limit zero.
`DXMT9_METAL_OCCLUSION` is the only selected environment difference. The
H466-corrected provider hash is
`cb96a69d5335c177b4ca9026d7599713d8bbdfab8b22ff9121402c8a91c34e86`.
The complete image hashes are retained in `images.json`.

Git HEAD changes from `71fce059` to `da0ac771` between the first two and last
two runs because the already-built fix and evidence are committed. No runtime
image changes or builds occur between the runs. This is a same-image
comparison, not a claim of identical Git metadata.

Product perf counters and frame sampling are OFF. The H455 external
completed-frame/camera/native-CPU observer remains active. These are diagnostic
CPU observations, **not observer-OFF FPS results**. Every run restores the five
shared Wine renderer images after normal exit.

The perf profile logs at `warn`; the Info-level ABI success banner is absent.
ABI acceptance is an operational inference from the pinned functioning dxmt9
route and unique `dxmt9-encode` worker: `src/winemetal/main.c`, `DllMain`, rejects
attachment unless `dxmt9_winemetal_check_abi_handshake` succeeds. No observed
success banner or numerical logged ABI hash is claimed. The generic copied
`dxmt9.log` contains historical Vulkan output and is not used as current-run
provider or error evidence.

## Primary endpoint: scene 102–110 seconds

All four tail comparisons pass the seven-knot camera envelope of 2 m / 3
degrees. CPU is computed at observed endpoints, not interpolated. Candidate
ranges include all four surrounding endpoint choices; they are not confidence
intervals. The reducer additionally checks baseline knot support within 1.1 s,
monotone completed-frame counts, native process identity, unique stable named
workers, monotone CPU counters and exclusion of UI-action overlap.

| Chronological pair | OFF Encode, ms/frame | ON Encode, ms/frame | OFF minus ON |
|---|---:|---:|---:|
| OFF-r1 then ON-r1 | 30.242 | 25.172–25.324 | 4.918–5.070 |
| ON-r2 then OFF-r2 | 32.200 | 24.358–24.534 | 7.666–7.842 |

Within-policy controls determine acceptance:

| Repeat | Encode candidate/baseline | Whole Primary candidate/baseline | +/-5% gate |
|---|---:|---:|---|
| OFF-r1 versus OFF-r2 | 1.0611–1.0653 | 1.1084–1.1237 | Fail |
| ON-r1 versus ON-r2 | 0.9723–0.9754 | 0.9728–0.9811 | Pass |

The minimum observed between-policy Encode reduction, 4.918 ms/frame, exceeds
the maximum observed within-policy Encode spread, 1.974 ms/frame. This is
directionally useful but does not override the failed independent repeatability
requirement. Whole Primary also falls between policies, but its game/Wine/PE/
synchronous-provider components remain unseparated; do not assign that entire
change to dxmt9 or add it to Encode.

Secondary windows do not repair the tail verdict. Reverse 40–55 s lacks a
camera match; comparisons involving OFF-r2 at 80–95 s reject a nonmonotone
unnamed native-thread counter. Those failures remain in the reports, rather
than relabeling that handle as Replay. Shutdown frame-offset discontinuities
start after 118 s, outside the primary interval.

## What this changes

Real visibility remains a concrete workload-reduction candidate after the
raster fix. Its exact removable budget is **not yet accepted**, and the largest
default dxmt9-attributable single-thread owner remains unresolved. H465's
Primary transfer and H467's ancestry limitations are not repaired by this test.

The saved 75/100 s screenshots show the building scene without an obvious
whole-building loss; they do not qualify the full tail sequence. A separate
correctness-only temporal review must not donate CPU values to this experiment.
No runtime default, README FPS, or goal-completion status changes.

## Remaining measurement capability, not another benchmark retry

Default Primary attribution requires calibrated native per-thread CPU across
all non-overlapping PE API roots, including synchronous Unix descendants, or a
proved covering superset. Existing CPU-supply `ImportBegin`/`ImportEnd` measure
commit implementation CPU only; they cannot upper-bound PE, generated bridge,
segmented pre-import and other synchronous API work. H452's rejected Wine
thread clocks cannot fill that gap.

A sparse native CPU-boundary observer would be a different capability from
wall-residency markers or native stack sampling. It first needs a native clock
service accessible through a diagnostic bridge, complete root/nesting coverage,
thread/frame joins, and short-interval resolution plus observer-error bounds.
Do not scale clock-dominated samples or subtract an average empty-hook cost to
manufacture a default-path bound. No such observer is implemented or qualified
by H468; it is a feasibility decision before any further attribution game run.
