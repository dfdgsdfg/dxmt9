---
domain: present-pacing
workload: STALKER Day/high 1280x720 late-window source-shape census
title: "Draw-plus-Clear sources own 73% of observed Encode source CPU"
type: evidence
status: source-shape-qualified-treatment-open
updated: 2026-10-16
source: traces/stkcop-source-shape-h581/{contract.md,observer.hpp,build.py,build.json,prepare.py,build-receipt.json,build-receipt-preflight-v1.json,capture-verification.json,capture-verification-v2.json,verify_preflight_v2.py,amend_preflight.py,live-verification.json,raw-verification.json,shape-summary.json,shape-verification.json,summarize.py,verify_shape.py}; traces/stkcop-source-shape-h581/A0/{receipt.json,analysis.json,self.bin,shape.bin,post-tail.png,restore/restored.json,recovered.json}
related: present-pacing-foreign-owner-live.580.md; present-pacing-current-clear-self.573.md; present-pacing-current-clear-live.574.md; present-pacing-clear-crossover-preflight.576.md
---

# Compare source classes, not arbitrary source halves

H580's Encode source scopes have approximately 0.17 ms median CPU but more
than 21 ms at the 90th percentile. Arbitrarily dividing sources into OFF/ON
groups mixes radically different amounts of work. Parity also correlates with
source roles, as H576 already demonstrated. Another unstratified comparison
would not address the observed uncertainty.

H581 adds one private queue object to H579: one binary row per source with
qualified queue/sequence/index/generation/storage and existing vector sizes.
It reads command/draw/DrawRun/Clear/state/uniform counts before prefetch/backend
work. It does not traverse commands, add clocks, retain resources, switch
policy, split sources or change queue synchronization. Source CPU still comes
from H514's complete Encode iteration. The source probe is H573's auto-arming
variant so the captured-input control exercises the metadata writer.

## Validation and execution

Original captured-input observer OFF/ON preserves exact source/output pixels,
seven source mappings, query transcript and 670/670 wrappers. Nine source
metadata rows appear when enabled; captured source5 has 3,125 draws, 526
DrawRuns and 65 Clears, agreeing with H542's independent command transcript.

The first negative-test script incorrectly set an already-zero initialization
command count to zero and expected rejection. It failed after both actual
capture processes succeeded. The failed report and original receipt remain.
V2 targets the nonempty source8 row instead; eight genuinely malformed traces
are rejected using the same captured bytes, **without rerunning captures**.
The live launch guard/receipt amendment was recorded before any live execution.

The fixed A0 live episode reaches all seven spatial knots and exits naturally
with code0. Fourteen shared files are restored and four services recovered.
Only the predeclared catalogue `missing_capture` waiver applies. The post-tail
image shows coherent building geometry, not an exact whole-game oracle.

## Actual pre-encoding source classes

Knots1–6 contain 121 observed completed frames. There are 738 Encode iterations
fully contained inside the conservative boundaries, all joined to structural
rows by complete source identity. Boundary-crossing iterations are excluded
from this partition, not prorated. All 1,699 recorded shape rows remain in the
artifact. The source partition is not the whole-thread CPU total.

| Immutable source class | Sources | Complete source CPU | CPU / observed frame | Draws per source |
|---|---:|---:|---:|---:|
| Draws and Clears | 120 | 2,519.677 ms | 20.824 ms | 3,028–4,295 |
| Draws without Clears | 498 | 908.456 ms | 7.508 ms | 1–987 |
| No draws | 120 | 18.418 ms | 0.152 ms | 0 |

Draw-plus-Clear sources own **73.107%** of these observed source costs. Each
has 58–71 Clears and 525–891 commands. Their median is 20.696 ms/source;
this does **not** mean the Clear commands alone cost 20.696 ms. It identifies
the relevant source cohort for repricing the H573 fusion candidate.

The same-episode whole-thread bounds are Encode **28.692–28.859** and Replay
**28.854–29.205 ms/frame**; they narrowly overlap. No full-window or component
winner separates in this episode. H580's Encode lead is therefore an episode
result, not a stable ordering transferable to every current run. Primary's
renderer root upper is 17.923 ms/frame; other observed worker-body uppers are
at most 0.764 ms/frame. Instrumentation differences prevent interpreting the
H580→H581 numerical movement as a product performance change.

## Baseline-only design calculation

With observed baseline costs held fixed under a sharp null, independent fair
source assignments give `2 * sqrt(sum(cost_i²)) / frames` = **4.025 ms/frame**
standard deviation for the unadjusted signed-total estimator. This is a
randomization-design calculation, not measured OS noise or a treatment CI.

Selecting eligibility solely from pre-encoding `draws > 0 && clears > 0` and
randomly swapping OFF/ON within consecutive eligible pairs gives 60 pairs.
The analogous null deviation is **0.194 ms/frame**, from
`2 * sqrt(sum((cost_a - cost_b)²)) / frames`. An independent binary decoder
reproduces the 738 joins, class totals and pairing calculation. This reduced
baseline variation is why the next experiment should target paired eligible
sources rather than even/odd source averages or repeated whole-game A/B runs.

The pairing is a proposed design, not evidence that treatment has no carryover.
It does not guarantee the same workload under treatment, account for changed
GPU backlog, or prove live removal. It must retain immutable source-qualified
policy, original GPU/query/lifetime controls, a no-op assignment control and
independently assigned blocks. A mixed-policy direct-effect estimate is not an
all-ON FPS estimate; default promotion still requires appropriate evidence.

## Decision

Target the Draw-plus-Clear cohort for the next bounded CPU-removal experiment.
Freeze the class, pair rule, seeds and estimator before its results. Keep
noneligible sources unchanged. Collect full Encode/Replay self CPU as secondary
outcomes so any downstream bottleneck shift remains visible. Do not claim the
current 73% source cost is removable or multiply H573's one-source saving into
live ms/frame without that experiment. Global default maximum and the live
removal amount are not yet certified.
