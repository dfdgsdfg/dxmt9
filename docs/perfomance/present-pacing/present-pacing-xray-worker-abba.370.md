---
domain: present-pacing
workload: STALKER CoP Day high 1280x720
title: "H370: Observer-OFF worker-count ABBA"
type: evidence
status: positive-performance-signal-visual-audio-open
updated: 2026-09-26
source: traces/app-d3d9-stkcop-worker-abba-h370/{receipt.jsonl,analysis.json,run.py,analyze.py}; experiments/output/app-d3d9-stkcop-bench-worker-abba-h370-1-a/{result.json,daybenchmark.result}; experiments/output/app-d3d9-stkcop-bench-worker-abba-h370-2-b/{result.json,daybenchmark.result,actual.png}; experiments/output/app-d3d9-stkcop-bench-worker-abba-h370-3-b/{result.json,daybenchmark.result,actual.png}; experiments/output/app-d3d9-stkcop-bench-worker-abba-h370-4-a/{result.json,daybenchmark.result}
related: present-pacing-xray-worker-yield.369.md; present-pacing-tail-yield-owner.368.md
---

# H370: Fewer X-Ray helpers improve both orderings, but do not eliminate stalls

Four sequential catalogue runs use Sikarugir, Day/high, 1280x720 windowed,
identical staged binary hashes, and the perf profile with counters OFF.
No PE sampler, frame observer, System Trace, build or test runs concurrently.
The usual launcher screenshot remains enabled in all four runs. No runtime,
catalogue default or README baseline is changed.

A is the existing default command line. B adds only the existing engine
argument `-max-threads 2`. The supervisor removes inherited DXMT/MTL-prefixed
environment selectors before setting the recorded recipe. Order is A–B–B–A;
each run completes before the next launches. There is no thermal stabilization
measurement or automatic proof that the user never changed focus.

## Benchmark-owned results

FPS comes from `daybenchmark.result`; `summarize_stkcop_fps.py` verifies its
reported average against frame count divided by the sum of per-frame durations.
CPU profiler estimates are not used as throughput measurements.

| Order | Policy | Average FPS | Whole-run p95 frame ms | Whole-run p99 frame ms | Frames >250ms |
|---|---|---:|---:|---:|---:|
| 1 | A: default | 25.425 | 82.16 | 163.98 | 13 |
| 2 | B: max-threads 2 | 28.337 | 73.81 | 147.68 | 6 |
| 3 | B: max-threads 2 | 27.617 | 75.79 | 170.19 | 11 |
| 4 | A: default | 22.000 | 107.50 | 249.97 | 28 |

Arithmetic means of run averages are A **23.713 FPS**, B **27.977 FPS**,
or **+17.98%**. Both B values exceed both A values, and both forward and reverse
adjacent pairs favor B. With only two samples per policy and substantial A
drift (25.425 to 22.000), this is a positive reproducibility signal, not a
confidence interval or a guaranteed 18% gain. H369's diagnostic ~41% difference
is not a product-performance estimate and is superseded by this OFF comparison.

## Last 30 reconstructed seconds

Select frames whose reconstructed completion timestamp is in the last 30
seconds. A frame crossing the boundary contributes its full duration to the
latency distribution. These are benchmark-relative tail windows, not exact
matched scene or source identities; frame counts differ with throughput.

| Order | Policy | Tail frames | Tail p95 ms | Tail p99 ms | Tail max ms | Tail frames >250ms |
|---|---|---:|---:|---:|---:|---:|
| 1 | A | 370 | 149.85 | 278.63 | 407.30 | 5 |
| 2 | B | 417 | 131.06 | 221.58 | 295.39 | 2 |
| 3 | B | 392 | 155.77 | 240.27 | 282.23 | 2 |
| 4 | A | 348 | 181.53 | 277.19 | 318.77 | 7 |

B improves tail p99 and >250ms counts in both adjacent comparisons, but does
not remove stuttering. B3's tail p95 exceeds A1's; B2 has a whole-run maximum
of 632.10ms outside this tail. Do not claim every latency measure improves.

## Health and decision

All four catalogue results are PASS, perf counters are recorded as `0`, and
the complete staged artifact dictionaries match. Unix SHA prefix remains
`829f3d304b314d38`; PE32 remains `4d30258d6e7d0c3e`. With counters disabled,
no zero-GPU-error-counter claim is made.

Both B screenshots were inspected: they show rendered buildings, vegetation
and sky without an obvious black frame or gross geometry corruption. They
are different viewpoints, not a pixel oracle and not proof against temporal
flicker. No user-qualified visual/audio report arrived during this sequence;
audio recovery and complete visual validation remain open.

Keep `-max-threads 2` as an explicitly labeled STALKER configuration candidate,
not a dxmt9 scheduling default or a global Wine yield patch. The tested change
reduces engine helpers, not dxmt9 encode workers. Next obtain visual/audio
confirmation, then decide whether to publish a separate tuned result. Remaining
late stalls still warrant guest waiter/waker attribution for the primary's
NtWaitForAlertByThreadId site; the helper-count result does not identify that
wait owner or prove the entire tail is an idle-yield problem.
