---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads 2 snd_targets 16
title: "Completed-frame samples: excess default submissions and a visibility-associated CPU reduction"
type: evidence
status: repeated-tail-encode-cpu-effect-primary-attribution-open
updated: 2026-10-07
source: traces/stkcop-workload-h455/{observe.c,run.py,analyze.py,compare.py,compare_cpu.py,test_witness.py,comparison.json,visibility-comparison.json,visibility-d9vk-comparison.json,visibility-cpu-comparison.json,repeated_summary.py,repeated-summary.json,reverse-cpu-comparison.json,reverse-workload-comparison.json,aa-off-cpu.json,aa-on-cpu.json}; traces/stkcop-workload-h455/{dxmt9-r1,d9vk-recovered-r1,dxmt9-visibility-r1,dxmt9-r2,dxmt9-visibility-r2}/{receipt.json,progress.jsonl,analysis.json,observe.c,observe.exe,run.py,scene.png,tail.png}; traces/stkcop-workload-h455/d9vk-r1/{receipt.json,runner.log,observe.c,observe.exe}; traces/stkcop-workload-h455/d9vk-recovered-r1/{actual-helper-cpu.jsonl,actual-helper-cpu-summary.json,removed-app-local-d3d9.dll}; traces/stkcop-workload-h455/dxmt9-r2/rejected-handle-regression.json; experiments/output/app-d3d9-stkcop-bench-workload-h455-{dxmt9-r1,dxmt9-visibility-r1,dxmt9-r2,dxmt9-visibility-r2}/{result.json,stkcop-requested.ltx,stkcop-after-run.ltx,stkcop-controls.txt}; experiments/output/app-d3d9-stkcop-bench-workload-h455-d9vk-r1/{dxmt9.log,stkcop-requested.ltx,stkcop-controls.txt}
related: present-pacing-backend-cpu-camera.454.md; present-pacing-stalker-visibility-gap.401.md; present-pacing-run-construction-removal.451.md
---

# Equal camera does not imply equal submitted work

H455 implements H454's external completed-frame witness. Three closely matched
late camera pairs have substantially more native counted submissions under
default dxmt9 than D9VK. A separate same-binary dxmt9 real-visibility run reduces
late submitted work and named Encode CPU in this diagnostic pair. This changes
the next investigation toward visibility-dependent workload, but does not
establish the largest overall dxmt9 CPU owner or its validated removable amount.

No product source, runtime default or README score changes. The visibility
candidate remains opt-in; no complete pixel oracle or promotion is claimed.

## Runs and qualification

All runs use Sikarugir CX24.0.7, Day/high, 1280x720, two game threads, sixteen
sound targets, draw command limit zero, and product perf/frame observers OFF.
The requested config and controls hashes match H454. Both dxmt9 runs pass the
catalogue runner with identical five staged artifacts and identical final
config. Their sole renderer-policy difference is `DXMT9_METAL_OCCLUSION=1`.
The x86 D3D9 hash begins `534ea6f17128354e`; Unix begins `7f45561ddf0fd22f`.

D9VK is the same archived release `22511d1fbb15cdbc`, with runtime banners
`v1.10.3-20250511-async (macOS)` and MoltenVK 1.4.1. Its initial supervisor fails
during loading because a newly added observer code signature is addressed one
byte late (`0x754c4` rather than `0x754c3`). The game remains alive. After fixing
the observer, recovery attaches to that **same** native process identity
`[38523, 1791369962, 174016]`, before benchmark start; it does not restart the game.
The observer sees benchmark stop and subsequent process disappearance, but the
original runner is gone and a normal game exit code is **unknown**. This is a
recovered submission diagnostic, not a passing benchmark/FPS comparison.
The reducer explicitly binds the original/recovered identities, environment,
observed stop and terminal order. Recovery status is retained in comparisons.

The corrected reader also records the actual mapped R2 path, image size and
reset-code signature. The first dxmt9 run predates that addition: it has the
expected on-disk R2 hash and live module base, but not a mapped-path/signature
receipt. Per-run source and executable copies preserve this observer difference.

## Completed-frame evidence contract

The helper has only read/query access. At randomized 15–25 ms intervals it
brackets Device/SASH/rendering/guard reads and two counter reads with frame
reads. It retains both raw brackets. The accepted witness requires:

- Stable frame, state, SASH header and rendering phase; active/ready, no precache.
- Native SASH mode; dedicated-server and optional overdraw guards both zero.
- A run-derived `K`, calibrated by a stable same-frame rendering-one to
  rendering-zero transition with SASH count incremented once and unchanged
  vector allocation. All three runs independently establish `K=88`.
- `rendering == 0 && frame - SASH_count == K`, two identical counter reads,
  and a previously unseen frame/count identity.
- No silent rebasing after offset drift or a completed-counter contradiction.

Eight synthetic tests exercise missing calibration, next-frame reset, active
failure, vector relocation, completed-counter changes, duplicate rows, skipped
frames and legal changing counters during rendering. They test the reader's
acceptance rules, not every path through the closed-source renderer. The three
observed first offset discontinuities occur only during shutdown, after 117 s;
these and subsequent samples are excluded.

Counters represent inspected native submission attempts: primitives, submitted
vertex-count/range sums and DIP/DP calls. They are not unique geometry, GPU work,
query counts or proof of complete D3D9-call coverage. Accepted samples remain
biased toward longer completed-phase dwell; polling jitter does not remove it.

| Scene-relative region | Default accepted / observed frame span | D9VK | Visibility ON |
|---|---:|---:|---:|
| 40–55 s | 41 / 665 | 3 / 1,257 | 25 / 669 |
| 80–95 s | 25 / 289 | 29 / 499 | 22 / 304 |
| 102–110 s | 1 / 157 | 6 / 330 | 11 / 159 |

No recorded active/readiness/precache violations occur in these regions.
Coverage is not a whole-window census. In particular, one default tail sample
cannot characterize the final ten seconds.

## Observed camera-matched submissions

Matching uses unique, mutual-nearest, monotone pairs, at most 0.5 m, one degree
and two seconds apart. Counts are never interpolated. The three default/D9VK
late pairs cluster around default scene seconds 88–92; their actual maximum
camera errors are 0.071 m and 0.384 degrees.

| Default scene time | Default draws | D9VK draws | Draw ratio | Primitive ratio |
|---|---:|---:|---:|---:|
| 88.585 s | 5,000 | 2,859 | 1.749 | 1.897 |
| 89.663 s | 5,265 | 3,289 | 1.601 | 1.698 |
| 91.789 s | 5,633 | 3,492 | 1.613 | 1.741 |

These are three camera states in one diagnostic pair, not three repetitions
or a late-window average. They contradict equal counted work **at these states**.

Within dxmt9, nine late matched OFF/ON pairs have draw ratios of
1.009–1.557, median 1.140, and primitive ratios of 1.002–1.630, median 1.245.
Three middle-region pairs are effectively unchanged (draw ratio 0.998–1.000).
Visibility-ON/D9VK supplies three different late pairs with draw ratios
1.016–1.275, median 1.029. These are different matched populations: do not
subtract their medians to claim that visibility closes a particular fraction
of the D9VK gap. The expanded candidate matching pool includes two seconds
outside each nominal region; it is distinct from the coverage table above.

## Same-provider native CPU, separately qualified

The OFF/ON CPU comparison uses seven camera knots and the H454 limits of two
metres/three degrees, with monotone correspondence and at most 1.1 s between
bracketing observations. It interpolates camera location only, then prices all
four **observed** candidate CPU endpoint combinations. The named Encode and
Primary workers must each have one unchanged handle and monotone native CPU
counters at every observed CPU snapshot. Unnamed workers are not called Replay.

| Camera region | Thread | Default CPU ms / SASH frame | Visibility ON |
|---|---|---:|---:|
| 40–55 s | Encode | 13.334 | 13.164–13.360 |
| Same | Whole Primary | 19.105 | 19.290–19.560 |
| 80–95 s | Encode | 24.415 | 22.264–22.563 |
| Same | Whole Primary | 41.749 | 36.798–36.908 |
| 102–110 s | Encode | 29.745 | 25.028–25.145 |
| Same | Whole Primary | 47.387 | 34.746–34.871 |

Thus the observed named Encode difference is 1.85–2.15 ms/frame in the first
late region and 4.60–4.72 ms/frame in the last region. These are endpoint
sensitivity ranges from **one sequential diagnostic pair**, not confidence
intervals or guaranteed removable CPU. The last region qualifies for the CPU
trajectory comparison despite insufficient completed-counter coverage; its
CPU result does not grant additional submission-count evidence.

Primary includes game and Wine; its reduction cannot be assigned to PE alone.
Do not add overlapping thread CPU into frame latency, divide these all-frame
budgets by sparse counter averages, or turn the comparison into an FPS bound.
The flag changes query outcomes, membership/publication policy and downstream
workload together; this does not isolate the cost of each avoided draw.

Screenshots show the scene, but are not exact or full-sequence pixel evidence.
Explicit screenshot intervals are excluded from CPU comparisons. The catalogue
baseline/ON +120 s screenshots occur around scene +30 s, outside selected CPU
windows. No stack sampler runs during H455.

The actual D9VK PE reader uses about 0.013 core in the late region and its
Python controller about 0.008 core. ON uses about 0.014 and 0.009 respectively.
These exclude wineserver and indirect interference. The original `helper_cpu`
field sampled the Wine launcher, not `observe.exe`; its zero value is rejected
as overhead evidence. Corrected ON observation and a separately identity-bound
D9VK helper series supply the actual-reader figures. The diagnostic is not
claimed to have zero perturbation.

## Decision

### Reverse-order confirmation

The follow-up runs visibility ON r2, then default OFF r2, with the same reader,
five binaries, requested/final config and controls. Both catalogue runs pass.
The four dxmt9 runs are two direction checks, **not a contiguous ABBA**: the
D9VK diagnostic separates the first OFF/ON pair. No new stack profiler or
product observer is enabled. Fixed camera and acceptance limits are retained.

| 102–110 s camera region | Default Encode CPU ms/frame | Visibility ON | Observed reduction |
|---|---:|---:|---:|
| First, OFF then ON | 29.745 | 25.028–25.145 | 4.600–4.716 |
| Second, ON then OFF | 30.963 | 24.852–24.920 | 6.044–6.112 |

The effect retains its direction with reversed execution order. Same-policy
camera comparisons also expose drift: OFF r2 is 0.819–1.218 ms/frame above OFF
r1, while ON r2 is 0.396–0.473 below ON r1. These two repeats do not define a
confidence interval, drift bound or a universal 4.6–6.1 ms removable amount.
They make the tail Encode CPU candidate substantially better supported than
the initial single pair. Whole Primary decreases too, but remains game/Wine/
renderer inclusive and cannot rank PE against Encode.

The reverse 80–95 s CPU comparison is **rejected**, despite passing camera and
active-state checks. An unnamed native thread handle has 1.333498 s cumulative
CPU at scene 91.108 s, then 0.007684 s at 92.641 s. This is consistent with
handle reuse, but its identity is unproved. The original all-endpoint-thread
monotonicity gate rejects the region rather than silently dropping this thread
or treating negative CPU as zero. The same-policy OFF comparison rejects that
region too. Named Primary/Encode continuity checks pass in the accepted tail.

The reverse completed-counter comparison is a separate evidence population:
nine 80–95 s camera pairs have draw-ratio median 1.257 (range 0.871–1.458), and
two 102–110 s pairs have ratios 1.512 and 1.518. The first range includes a
counterexample to uniform per-frame reduction. These samples do not repair the
rejected CPU interval or establish whole-window draw totals.

### Remaining decision

Visibility-dependent workload now has a repeated tail Encode CPU effect. Keep
its query/publication costs and full-route pixel gates separate; do not promote
the candidate from CPU data alone. Visibility qualification is CPU-workload
work as well as GPU semantics work.

H451's existing capture29 remains useful for controlled execution, but has no
binding to this late camera region. Neither that capture's small copy saving
nor the present sparse samples determine the largest overall dxmt9-owned
single-thread cost. Default Primary attribution and the candidate's complete
semantic/GPU qualification remain open.

The next ownership discriminator is a conservative Primary renderer bound,
not another broad FPS loop. Existing H453 body scopes cannot provide it for
default code. A clockless outer-call marker plus independently scheduled wall
samples could bound renderer-possible residency, then use
`renderer CPU <= min(native Primary CPU, possible-residency wall time)`.
It must cover original prologue/epilogue and synchronous callees, retain failed
or uncertain samples conservatively, and qualify perturbation. This is an
unimplemented measurement design, not evidence that PE is below Encode.

All measured processes have stopped. The recovered D9VK app-local DLL is moved
into its artifact, leaving the application clear; shared Wine baseline hashes
are verified. Environment overrides were child-local and no default is changed.
