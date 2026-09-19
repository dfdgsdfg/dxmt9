---
domain: present-pacing
workload: STALKER CoP Day
title: "H330: post-Rosetta Pending and publication measurements"
type: evidence
status: partial
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-pending-off-r657-day/result.json; experiments/output/app-d3d9-stkcop-bench-pending-on-r658-day/result.json; experiments/output/app-d3d9-stkcop-bench-publication-dedup-on-r659-day/result.json; experiments/output/app-d3d9-stkcop-bench-pending-off-r657-day/daybenchmark.result; experiments/output/app-d3d9-stkcop-bench-pending-on-r658-day/daybenchmark.result; experiments/output/app-d3d9-stkcop-bench-publication-dedup-on-r659-day/daybenchmark.result
related: present-pacing-query-pending-publication.329.md
---

# H330: cost reduction is not a demonstrated FPS improvement

## Matched current-image measurements

After user-installed Rosetta restores x86 execution, all four controls use the
same five staged images (Unix hash `3262676c1b376796`, PE32 d3d9
`0f7861b84f493492`), on macOS 27.0 build 26A428. No rebuild, TLC or profiler
runs concurrently. Sikarugir cx24.0.7, Day High, `renderer_r2.5`, 1280x720,
perf profile, validation OFF, health counters ON. Actual occlusion and async
fences ON; END coalescing, query/receipt/backend/split, queue-mutex and frame
observers OFF. Exact launch policies are retained in each `run-policy.json`.
Do not combine these results with pre-OS-upgrade FPS as a code comparison.

| Run | Pending fast path | Publication dedup | Frames / seconds | FPS | Mark elapsed ms / submitted Present | Publications |
|---|---|---|---|---:|---:|---:|
| R657 | OFF | OFF | 2,486 / 127.801 | 19.452 | 0.880 | 30,350 |
| R658 | ON | OFF | 2,471 / 127.226 | 19.422 | 0.905 | 31,635 |
| R659 | ON | ON | 2,466 / 131.022 | 18.821 | 0.812 | 29,910 |
| R661 (reference repeat) | ON | OFF | 2,437 / 131.678 | 18.507 | 0.893 | 30,140 |

All runners PASS, all eight health counters are present and zero, and all
staged sizes/hashes match. Each capture contains the game with no obvious
corruption in the sampled scene. This is not continuous foreground evidence
or full-sequence pixel validation. No focus/input intervention was performed.

Pending's nominal FPS change is -0.15%; one pair cannot establish a speedup
or stable non-regression. Publication dedup has 10.3% lower observed marking
elapsed per submitted Present, but 3.1% lower FPS in this single comparison.
Publication count also differs: per-Present draw counts are close
(2,590 vs 2,594), but command buffers are 16.08 vs 15.46 and publication counts
are 31,635 vs 29,910. Therefore the entire marking delta cannot be attributed
to cache hits. Repeated controlled pairs are required before promotion.
Marking per counted publication is 73.26→69.35 microseconds (-5.3%), a
smaller delta than per Present; publications do not have identical resource
closures, so this is also a workload-normalized observation, not causal proof.

R661 repeats the R658 reference after R659 and the separate R660 diagnostic.
Its artifacts are under
`experiments/output/app-d3d9-stkcop-bench-publication-dedup-off-repeat-r661-day/`.
Marking returns to 0.893 ms/Present and 74.81 microseconds/publication, versus
R659 ON at 0.812 and 69.35. The observed ON reduction is 9.1–10.3% per Present
and 5.3–7.3% per publication against the two OFF controls. FPS, however,
drifts 19.422→18.507 (-4.7%) between identical-policy OFF controls; ON is
between them. This directly prevents interpreting the first ON FPS dip as
a proven cache regression or claiming an FPS gain. Sampled capture, health
and five-image identity checks also pass for R661. R661 counters span 2,526
submitted Presents, not its 2,437 benchmark frames.

`prepare_slot_resource_mark_cpu_ms` is an elapsed scope despite its name,
not isolated thread CPU. Counters span the process and are normalized by
submitted Presents (2,575 / 2,560 / 2,555), not benchmark result frames.
Do not sum overlapping scope times or infer saved frame time from them.

## Tail remains with both optimizations

Reconstructing 10-second windows with `summarize_stkcop_fps.py`:

| Benchmark elapsed window | R657 FPS | R658 FPS | R659 FPS |
|---|---:|---:|---:|
| 40–50 s | 37.0 | 35.9 | 36.2 |
| 60–70 s | 19.2 | 19.3 | 18.6 |
| 80–90 s | 10.6 | 10.3 | 9.0 |
| 90–100 s | 6.2 | 6.1 | 6.7 |
| 110–120 s | 9.6 | 14.7 | 9.3 |

These are reconstructed elapsed windows, not exact shared scene/frame IDs.
The late slowdown is not eliminated by Pending or publication reuse. This
does not yet distinguish more scene work, delayed submission, GPU/driver
completion or application-side polling. R660 separately enables sampled query
lifecycle, receipt/backend timing and per-Present deltas for that attribution;
its FPS must not be treated as observer-OFF performance evidence.

Neither selector is promoted; README remains unchanged.

## R660: completed diagnostic, not a performance control

Artifacts: `experiments/output/app-d3d9-stkcop-bench-pending-publication-diagnostic-r660-day/`
contains `run-policy.json`, `result.json`, `daybenchmark.result`, `actual.png`,
`dxmt9.log` (per-Present deltas) and `xrEngine_dxmt9.log` (query ledger).
Pending and publication reuse ON; query timeline ON with END stride 512,
backend/receipt sampled timing ON and frame sampling ON. Other policy and
five staged images match the controls. PASS, eight health counters zero,
sampled game capture looks normal. Its 19.128 FPS is diagnostic only.

### Pending and polling

The process-final report survives, unlike R656's prefix-only evidence:

- 538,242 backend calls; 511,710 covered-Pending, 26,532 slow.
- 512,746 Pending and 25,496 Complete returns. Coverage handles 95.07% of
  all backend calls and 99.80% of Pending returns without the large queue lock.
- Submission decisions: 848 new, 49 existing-target, 139 already submitted,
  zero rejected. The other 537,206 need no submission decision.
- Receipt: 538,243 attempts, one non-raw fallback, no error. The single global
  drain is an empty/no-END case, not a repeated tail drain.
- Backend queue-wait samples sum to 1.551 ms, maximum 0.420 ms, over 8,411
  backend samples (fast and slow combined). Pending receipt samples (8,002)
  have 9.424 ms replay-wait and 16.538 ms backend elapsed sums.

These are one-in-64 elapsed samples, not totals to scale into CPU/FPS cost.
They establish route use and do not support blaming repeated large-queue
mutex acquisition for the observed hundreds-of-milliseconds stalls. Bridge
crossing and game-side polling work outside these scopes remain unmeasured.

### Query lifecycle attribution

50/25,531 ENDs are retained across 132.963 seconds; stride omissions 25,481,
store saturation zero, missing-stage rows zero, unknown stages zero. All 50
have live first-S_OK and final rows. Stage differences below have no negative
values. Ordinary encode evidence evictions are 30,349; they are not dropped
END summaries. Sampling is END-count weighted, not time/frame weighted.

| Sample set | Rows | Median Ready depth | Publication→encode start ms | Publication→submit ms | Submit→completion ms | Completion→first S_OK ms |
|---|---:|---:|---:|---:|---:|---:|
| First 90 seconds from first retained END | 12 | 0 | 7.609 | 7.750 | 5.091 | 5.793 |
| Remaining sampled interval | 38 | 14.5 | 11.485 | 11.642 | 0.348 | 6.089 |

All 38 late samples are occlusion queries with explicit completion and value
zero. All record predecessor encode overlap. This describes the retained
sample, not every query or a correctness proof of zero values. Publication
residence, rather than encoding the tiny END source itself (all-row median
0.158 ms), dominates the measured path before submit. The late ready backlog
coexists with very short submit→completion for these sampled queries.

`actualSubmit` is after successful commit and completion handoff;
`completion` is after waterline/Tape settlement. Their difference includes
queue/driver and completion-thread work and is **not pure GPU execution**.
The 362.830 ms maximum completion→first-S_OK belongs to the first sampled
query, not the late tail. This interval includes when the application chooses
to poll; it does not measure lock acquisition alone. Query timestamps are not
joined to benchmark scene/frame IDs.

### Two distinct late behaviors

Per-Present output covers 2,588 encode-side Presents. The following windows
use logged frame IDs, exclude startup/loading, and are not benchmark `.result`
frame IDs. Counters are asynchronous interval deltas, not strict per-frame GPU
attribution; their scopes overlap and must not be added.

| Encode Present IDs | Mean interval ms | Draws/interval | CBs/interval | Passes/interval | Encode elapsed ms/interval | GPU CB duration sum ms/interval |
|---|---:|---:|---:|---:|---:|---:|
| 100–699 | 37.106 | 2,893.9 | 5.46 | 18.32 | 22.124 | 9.873 |
| 2000–2199 | 76.461 | 2,871.0 | 6.00 | 19.68 | 25.246 | 10.610 |
| 2200–2299 | 144.078 | 3,494.8 | 54.81 | 68.58 | 36.375 | 55.821 |
| 2287–2586 (last 300 active Presents) | 100.739 | 2,847.8 | 63.60 | 74.35 | 33.639 | 46.118 |

1. Before the split explosion, interval time already doubles with similar draw,
   pass and GPU duration totals. Frame 2176 takes 408.012 ms with five CBs,
   22.793 ms encode elapsed and 9.462 ms summed GPU duration. The unaccounted
   interval is not automatically PE, application CPU or a GPU stall: stage
   overlap and scheduling are not yet measured end-to-end.
2. Later, CB/pass fragmentation becomes a concrete additional cost signature.
   The final 300 intervals have similar draw/triangle volume to the early
   window but ~11.7x CB count, ~4.1x pass count, and higher aggregate GPU
   duration. Present-acquire elapsed remains ~0.06 ms/interval. Query samples
   in the later time interval show a ready backlog and dense occlusion ENDs.
   Temporal coincidence does not yet prove which boundary creates each CB.

The next targeted experiment should join late CB/pass close reasons and
occlusion END/FLUSH submission pressure to interval IDs. Separately sample
the application/PE/synchronous-Unix thread in the pre-fragmentation stall
window. Do not attribute both behaviors to resource marking or remove query
completion obligations to hide them.
