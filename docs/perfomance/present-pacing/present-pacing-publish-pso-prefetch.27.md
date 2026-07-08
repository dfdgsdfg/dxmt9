---
title: Present Pacing 27 - Encode-Slot PSO Prefetch Default
date: 2026-06-14
status: accepted-default
source: experiments/output/app-d3d9-3dmark05-present-publish-split-lowoverhead-r1-20260614/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-slot-pso-prefetch-lowoverhead-r1-20260614/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-slot-pso-prefetch-lowoverhead-r2-20260614/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-slot-pso-prefetch-default-lowoverhead-r1-20260614/3dmark05-perf-summary.md
---

# Present Pacing 27 - Encode-Slot PSO Prefetch Default

**Question.** Can the Phase 26 placement signal become the default without
turning into per-draw pipeline lookup fallback?

**Result.** Yes for the current GT1 low-overhead gate. The new default keeps
PSO handles available (`encode_draw_pso_prefetch_handle_missing=0`) while
removing publish-time PSO prefetch from `submitPresent()`.

| Metric | Old publish prefetch | New default | Delta |
|---|---:|---:|---:|
| `submit_present_cpu_ms / present` | `2.753ms` | `0.270ms` | `-2.483ms` |
| `prepare_slot_pso_prefetch_cpu_ms / present` | `2.497ms` | `0.000ms` | `-2.497ms` |
| `encode_slot_pso_prefetch_cpu_ms / present` | n/a | `2.605ms` | n/a |
| `encode_draw_pipeline_lookup_cpu_ms / present` | `0.551ms` | `0.524ms` | `-0.027ms` |
| `completion_wait_ms / present` | `30.153ms` | `28.644ms` | `-1.509ms` |
| `completion_wait_without_enqueue_ms / present` | `29.989ms` | `27.621ms` | `-2.368ms` |
| `completion_no_enqueue_stage_publish_to_encode_dequeue_p50_ms` | `3.924ms` | `0.390ms` | `-3.534ms` |
| Warm FPS p50 | `17.064` | `17.722` | `+0.658` |
| Warm FPS avg | `17.628` | `18.345` | `+0.717` |

**Decision.** Promote encode-slot PSO prefetch as the default placement. This
is the first current low-overhead P4/P2 change in this lane that both names a
specific serialized owner and improves sampled FPS while preserving the
prefetched-handle encode path.

The remaining average-FPS model is now:

- Present replay no longer spends ~`2.5ms/present` in PSO prefetch.
- The work is now visible as `encode_slot_pso_prefetch_cpu_ms`, so future
  encode-stage reductions should include it in the budget.
- `completion_no_enqueue_stage_encode_dequeue_to_command_buffer_commit_p50_ms`
  rises because the prefetch moved there; the net FPS still improves because
  the publish gap and completion wait fall.
- The next target is either reducing encode-slot prefetch cost itself or
  overlapping more of the encode-side work, not reverting to publish-time
  prefetch.

**Related.** [present-pacing-publish-pso-prefetch.26](present-pacing-publish-pso-prefetch.26.md) ·
[state-churn-encode-encode-phase.70](../state-churn-encode/state-churn-encode-encode-phase.70.md) · [present-pacing](../present-pacing.md).
