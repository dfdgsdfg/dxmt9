---
domain: present-pacing
workload: STALKER CoP Day
title: "H325: benchmark-owned FPS windows confirm late stalls"
type: evidence
status: measured
updated: 2026-09-19
source: experiments/output/app-d3d9-stkcop-bench-poll-receipt-on-r648-day/daybenchmark.result; experiments/output/app-d3d9-stkcop-bench-receipt-poll-cost-r649-day/daybenchmark.result
related: present-pacing-query-poll-receipts.323.md; present-pacing-query-receipt-cost.324.md
---

# H325: early throughput and late stalls are different regimes

After committing H324 as `21bbc10c`, the user reports fast early/middle scenes
but severe late stuttering. Existing benchmark-owned `.result` files already
contain per-frame FPS: no new runtime instrumentation or wild run is needed
to test that observation.

`scripts/tools/summarize_stkcop_fps.py` reconstructs elapsed time by accumulating
`1 / frame_fps`. Frame count divided by that duration reproduces the reported
average: R648 2,559 / 125.556 seconds = 20.381 FPS; R649 2,481 / 129.995 seconds
= 19.085 FPS. An arithmetic mean of frame FPS is incorrect (R649 would appear
to average 24.613 FPS). Input validation rejects nonpositive/nonfinite FPS,
noncontiguous frame IDs and disagreement with the reported average.

```sh
scripts/run_python.sh scripts/tools/summarize_stkcop_fps.py \
  experiments/output/app-d3d9-stkcop-bench-poll-receipt-on-r648-day/daybenchmark.result \
  experiments/output/app-d3d9-stkcop-bench-receipt-poll-cost-r649-day/daybenchmark.result
```

Default windows are 10 seconds; `--window-seconds` changes them and `--json`
exports structured stdout. Time zero is the first recorded benchmark frame,
not process launch, source identity or an authoritative camera timestamp.
Windows count frame completions in `(start, end]`; the final window uses its
actual shorter duration. Frame-time percentiles and stalls are assigned by
completion, including frames crossing a window boundary. Counts can therefore
differ by a boundary frame from an overlapping-time estimator. The two runs
are not guaranteed scene-aligned at equal reconstructed times.

## Observed windows

| Reconstructed seconds | R648 FPS | R649 FPS | R649 p95 frame ms | R649 max frame ms | R649 frames >100ms |
|---|---:|---:|---:|---:|---:|
| 0–10 | 25.70 | 26.00 | 50.7 | 145.6 | 1 |
| 10–20 | 29.20 | 27.60 | 48.5 | 72.1 | 0 |
| 20–30 | 27.50 | 26.90 | 46.6 | 70.2 | 0 |
| 30–40 | 26.00 | 24.50 | 64.0 | 74.6 | 0 |
| 40–50 | 35.90 | 33.80 | 41.0 | 72.1 | 0 |
| 50–60 | 27.20 | 25.80 | 52.3 | 81.9 | 0 |
| 60–70 | 20.70 | 19.80 | 67.6 | 253.3 | 2 |
| 70–80 | 14.30 | 15.80 | 125.7 | 366.1 | 14 |
| 80–90 | 12.00 | 10.70 | 214.0 | 432.4 | 30 |
| 90–100 | 7.60 | 5.10 | 490.0 | 634.5 | 41 |
| 100–110 | 11.80 | 10.10 | 181.1 | 337.9 | 20 |
| 110–120 | 12.90 | 12.60 | 201.5 | 647.4 | 15 |
| 120–end | 9.18 | 9.40 | 226.2 | 424.5 | 34 |

The early 0–60-second throughput is 28.58 FPS (R648) / 27.43 FPS (R649).
Both already show the late collapse, including R648 with the receipt observer
OFF. Thus the new receipt observer is not necessary for the tail to occur.
R649's 90–100-second window has 51 completed frames, of which 41 exceed 100ms
and 13 exceed 250ms: this is severe stuttering, not only a modest average-FPS
reduction. Its worst recorded frame is about 647ms.

These files do not prove improvement over a pre-promotion baseline. R648 has a
loading-stage focus restore; R649 has receipt diagnostics, and no matched
same-session A/B exists. Neither runtime's average qualifies a policy delta.
The user report of improved early/middle responsiveness is consistent with the
current intra-run shape, not independent proof of an optimization gain.

The user additionally reports late slowdown in other implementations, especially
in approximately the last ten seconds. No implementation-qualified frame traces
were supplied for that observation. It strengthens the shared-workload/engine
hypothesis, but does not establish the cause, equal slowdown severity, or a
GPU bottleneck. Do not classify the tail itself as a dxmt9-specific regression.
Compare any retained reference `.result` files at matched settings and scene
boundaries before claiming excess dxmt9 tail cost; reconstructed elapsed time
alone is not a cross-provider scene identity.

H324 also places most sampled replay-wait time after its intermediate report,
but it has no timestamp/source join to these benchmark windows. Do not equate
the observations causally. Next diagnostic scope: the late region, with
source-qualified long replay waits and backend queue-poll attribution, rather
than whole-run averages or immediate additional production changes.

Verification: `scripts/run_python.sh tests/scripts/test_stkcop_fps_windows.py`
passes three tests, including partial/boundary windows, empty windows caused by
a long frame, and invalid inputs. No runtime code or README FPS changes.
