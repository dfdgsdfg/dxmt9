---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: encode-phase
order: 195
title: Current Wall Review and Next Owner Split
date: 2026-06-20
type: review-instrumentation
status: accepted-current-direction
source: src/dxmt9/dxmt9_command_queue.cpp, src/dxmt9/dxmt9_perf_counters.cpp, src/dxmt9/dxmt9_perf_counters.hpp, scripts/tools/summarize_3dmark05_perf.py, experiments/output/app-d3d9-3dmark05-visual-current-wide-window-r1/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-h220-current-visual-p4-baseline-r1/3dmark05-perf-summary.md, docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.187.md, docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.193.md, docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.194.md, docs/perfomance/present-pacing/present-pacing-current-visual-p4.136.md, docs/perfomance/snapshot-cache/snapshot-cache-visual.04.md
related: docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.194.md, docs/perfomance/present-pacing/present-pacing-current-visual-p4.136.md, docs/perfomance/snapshot-cache/snapshot-cache-visual.04.md
---

# Encode Phase 195 - Current wall review and next-owner split

## Question

Has the GT1 work hit a hard wall, or have the recent experiments only closed
the most obvious copy/carrier branches?

## Answer

It is not a proven wall. The current evidence says several local CPU branches
are now closed as average-FPS levers, while the remaining frame owner is still
P4/no-enqueue cadence plus exposed replay/encode serial work.

Latest current-head evidence:

| Metric | Current wide-window scout |
|---|---:|
| `sampled_avg_fps` | `16.457` |
| `present_encoded` | `1,803` |
| `completion_wait_without_enqueue_ms_per_present` | `28.053` |
| `completion_wait_with_enqueue_ms_per_present` | `0.050` |
| ready-depth max | `1` |
| `commit_chunk_replay_cpu_ms_per_present` | `8.655` |
| `encode_chunk_cpu_ms_per_present` | `12.882` |
| `commit_chunk_queue_draw_submission_cpu_ms_per_present` | `3.878` |
| `submit_draw_run_batch_append_cpu_ms_per_present` | `1.272` |
| `submit_draw_run_batch_append_uniform_cpu_ms_per_present` | `0.655` |
| `submit_draw_run_batch_append_state_cpu_ms_per_present` | `0.331` |

The same run shows that the F1/N-1 state-copy cleanup is already active:
`d3d9_snapshot_state_elided=410,814`, saving `4.203GiB` of state-copy width, and
`submit_draw_run_batch_compat_same_generation_lane_incompatible=0`. That closes
more same-generation deep-compare/state-materialization work as a first-order
target.

Uniform snapshot elision is not the matching next lever. The run still has
`d3d9_snapshot_uniform_elided=0` and
`d3d9_snapshot_uniform_adjacent_same_generation=0`; same-state groups frequently
share fixed payload or PS constants, but the whole `uniformGeneration` changes.
The broader compact-uniform source thread also reached its intended local form
in H196/H187, then stayed FPS-flat and P4-bound.

## Bottleneck Split

```mermaid
flowchart TD
  A["Current GT1 state"] --> B{"Hard GPU wall?"}
  B -- "No proof" --> C["GPU-hot-frame lanes may still exist,\nbut current average FPS is not GPU-bound"]
  A --> D{"Closed local branches"}
  D --> E["N-1 state materialization\nactive: 410,814 elided"]
  D --> F["Adjacent uniform elision\nrejected: same generation = 0"]
  D --> G["Compact uniform carrier/source\nmechanism accepted, FPS rejected"]
  D --> H["Chunk-end carry\nmechanism accepted, P4/FPS rejected"]
  A --> I{"Open owners"}
  I --> J["P4/no-enqueue cadence\n~28ms/present without enqueue"]
  I --> K["Replay/publish serial work\nqueue submission + pending flush"]
  I --> L["Encode serial work\nargbuf/cbuf/table/pipeline paths"]
  J --> M["Need overlap or cadence change\nthat increases enqueue-during-wait\nor reduces no-enqueue closure"]
  K --> M
  L --> M
```

## Interpretation

The wall-like feeling comes from testing many byte-width improvements that
lower local CPU counters but leave P4 unchanged. That pattern is real, but it
does not prove no performance remains. It means the promotion gate has to be
stricter:

1. A local replay/encode cleanup must also move
   `completion_wait_without_enqueue`, `completion_wait_with_enqueue`, ready
   depth, or no-enqueue closure rows before it can be called an average-FPS
   lever.
2. A new overlap design must preserve command-buffer/render-pass/tile locality
   and must not repeat the closed-head/open-CB regressions.
3. A GPU `.gputrace` should be spent only after a candidate moves P4/locality
   gates or when the question is explicitly a GPU-hot-frame/backend-storage
   question.

## Next Gate

Reasonable next work:

- inspect queue lock / outer submit / batch-width residual under
  `commit_chunk_draw_batch_submit_cpu_ms`, because H194 only explained the
  forced resource-marking part. The first new discriminator is
  `submit_draw_run_batch_queue_lock_cpu_ms`, which times only the queue mutex
  acquisition in `submitDrawRunBatch*` before the existing append/resource/slot
  child counters run;
- keep uniform/state copy reductions as local cleanup only unless their run
  moves P4 rows;
- return to a render-pass-safe overlap carrier if the next target is average
  FPS rather than local CPU cleanup;
- keep `v0.0.3` as the visual gate, and treat any weapon/lighting artifact as
  visual-open until reproduced in a same-window capture.
