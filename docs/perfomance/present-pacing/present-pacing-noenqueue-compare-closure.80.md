---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: compare-tooling
order: 80
title: No-Enqueue Before-Publish Closure Compare Metrics
date: 2026-06-18
type: instrumentation
status: accepted-tooling
source: scripts/tools/compare_3dmark05_perf_counters.py, tests/scripts/test_compare_3dmark05_perf_counters.py, experiments/output/app-d3d9-3dmark05-v003-current-baseline-r1-20260618/result.json, experiments/output/app-d3d9-3dmark05-v003-vs-const-setter-range-r1-20260618/result.json
related: docs/perfomance/present-pacing/present-pacing-serial-stage-compare-gates.38.md, docs/perfomance/present-pacing/present-pacing-noenqueue-inter-replay-gap.55.md, docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.150.md
---

# Present-Pacing 80 - No-Enqueue Before-Publish Closure Compare Metrics

## Question

The single-run summary already decomposes `commit entry -> publish` into
completed replay CPU, active replay CPU, inter-replay producer gap, and publish
wait. The A/B comparison report should show the same closure directly, otherwise
reviewers have to manually subtract summary tables before deciding whether a
candidate moved P4 or only moved a local CPU child.

## Tooling

`scripts/tools/compare_3dmark05_perf_counters.py` now derives these rows:

| Metric | Meaning |
|---|---|
| `no_enqueue_before_publish_completed_replay_cpu_ms_per_present` | CPU time from chunks fully replayed before the first publish after a no-enqueue wait |
| `no_enqueue_before_publish_active_replay_cpu_ms_per_present` | CPU time already spent in the active present-bearing chunk before that publish |
| `no_enqueue_before_publish_inter_replay_gap_ms_per_present` | wall time between completed chunk replay and the next `commit_chunk` entry before publish |
| `no_enqueue_before_publish_commit_publish_wait_ms_per_present` | queue publish wait immediately before publish |
| `no_enqueue_before_publish_closure_ms_per_present` | completed replay + active replay + inter-replay gap + publish wait |
| `no_enqueue_before_publish_residual_ms_per_present` | `commit entry -> publish` minus the closure |
| `no_enqueue_before_publish_*_share_pct` | closure component share of `commit entry -> publish` |
| `no_enqueue_before_publish_*_per_present` chunk/record rows | before-publish chunk shape and record mix |

The closure intentionally keeps `onBeforePublish` as a separate row. That
callback is sampled separately because it belongs to publish formation and can
otherwise double-count the `commit entry -> publish` stage.

## Sanity Check

The `v0.0.3`-anchored current setter-range scout remains noise-flat on the
targeted CPU owner, and the new compare rows make the P4 non-win visible:

| Metric | baseline | setter-range scout |
|---|---:|---:|
| `completion_wait_without_enqueue_ms_per_present` | `26.839` | `27.102` |
| `commit_chunk_replay_cpu_ms_per_present` | `8.039` | `8.030` |
| `encode_chunk_cpu_ms_per_present` | `11.311` | `11.164` |
| `no_enqueue_before_publish_completed_replay_cpu_ms_per_present` | `3.951` | `4.133` |
| `no_enqueue_before_publish_inter_replay_gap_ms_per_present` | `11.879` | `13.285` |
| `no_enqueue_before_publish_closure_ms_per_present` | `15.831` | `17.419` |
| `no_enqueue_before_publish_closure_share_pct` | `100.734%` | `100.829%` |

The closure is slightly above 100% because the component counters are sampled
around adjacent publish/replay timestamps and include small accounting overlap.
That is acceptable for attribution: the row still proves that the before-publish
wait is dominated by inter-replay producer gap, not queue publish wait or active
present-chunk replay.

## Interpretation

Future P4 candidates should be reviewed in this order:

1. The target CPU child moves in the existing P2/P3 rows.
2. `no_enqueue_before_publish_closure_ms_per_present` or its dominant component
   moves in the expected direction. Use
   `--require-no-enqueue-before-publish-closure-decrease`, or
   `--require-no-enqueue-before-publish-inter-replay-gap-decrease` when the
   candidate specifically targets PE/unix producer cadence.
3. H57 locality gates still pass: command-buffer count, render-pass count, and
   tile-preservation traffic do not rise.
4. Visual output still passes the `v0.0.3` safety anchor.

This does not add a new runtime counter. It promotes existing counters into the
A/B report and wrapper/finalizer gates so P4 regressions are visible without
opening two summaries side by side.

## Verification

- `python3 -m pytest tests/scripts/test_compare_3dmark05_perf_counters.py`
- `python3 -m pytest tests/scripts/test_3dmark05_probe_scripts.py -k pacing_compare`
- `git diff --check`

**Related.** [present-pacing-serial-stage-compare-gates.38](present-pacing-serial-stage-compare-gates.38.md) ·
[present-pacing-noenqueue-inter-replay-gap.55](present-pacing-noenqueue-inter-replay-gap.55.md) ·
[state-churn-encode-encode-phase.150](../state-churn-encode/state-churn-encode-encode-phase.150.md).
