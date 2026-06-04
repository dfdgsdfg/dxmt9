---
domain: state-churn-encode
subcategory: binding
order: 01
title: Binding Override Encode Optimization
date: undated
type: experiment-run
status: accepted
source: specs/perfomance.plan.md#L3989-L4070
---

# Binding Override Encode Optimization

**Question / hypothesis.** Do per-draw `DrawBindingOverride` payloads convert the
historical stream/IB state-delta breaks into larger draw-runs and cut encode CPU
without moving cost elsewhere or increasing churn?

**Method.** No-gputrace A/B:
`app-d3d9-3dmark05-binding-override-base-skip-nogputrace-r1` vs
`app-d3d9-3dmark05-submit-batch-normalized-fastcompare-nogputrace-r1`. The code
change: binding overrides no longer force a full base-state rebind when active
and current draw keys are compatible after ignoring stream/IB/constant fields.
Extra-stream stride changes still force base rebind (strides are baked into the
generated VS source); stream0 stride is safe via `DrawVolatile`.

**Result.**

| Metric | Before | After | Delta |
|---|---:|---:|---:|
| `draw_calls` | 1,051,353 | 1,051,189 | -0.02% |
| `render_pass_begin` | 16,886 | 16,886 | 0.00% |
| `backend_draw_run_batch_records_per_group` | 1.885 | 1.884 | -0.02% |
| `encode_draw_stream_bind_cpu_ms` | 2620.016 | 1830.639 | **-30.13%** |
| `encode_draw_cpu_ms` | 18899.770 | 16927.368 | **-10.44%** |
| `submit_draw_cpu_ms` | 3031.493 | 2999.525 | -1.05% |
| `gpu_command_buffer_time_ms` | 4086.988 | 4088.416 | +0.03% |

Binding overrides present and stable; draw/pass/GPU-command-buffer shape
essentially unchanged.

**Verdict.** Accepted (CPU win). Stream-bind encode CPU dropped `-30.13%` and
total encode CPU `-10.44%` with no churn increase — but GPU command-buffer time
moved only `+0.03%`. This is a CPU-throughput win, orthogonal to the GPU
bottleneck. It fixes the dominant stream+IB-only state-delta break class.

**Related.** [[state-churn-encode]] · motivated by [[state-churn-encode-statedelta.03]] ·
[[state-churn-encode-churn.01]] (the override mechanism) ·
[[state-churn-encode-batch.01]] (later recheck — GPU still flat) ·
[[hidden-backend-storage]] (GPU bottleneck unmoved).
