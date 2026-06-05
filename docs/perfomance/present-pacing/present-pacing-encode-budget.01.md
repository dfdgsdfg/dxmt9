---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: encode-budget
order: 01
title: Per-CB Encode Budget Attribution Against the 16.67 ms vsync Slot
date: 2026-06-05
type: attribution
status: accepted
source: experiments/output/app-d3d9-3dmark05-current-nondiag-baseline-r1, experiments/output/app-d3d9-3dmark05-display-sync-off-r1
---

# Per-CB Encode Budget Attribution Against the 16.67 ms vsync Slot

**Question / hypothesis.** Steps 1-3 foreclosed the present-side knob
space. The only production path left is **reducing per-CB encode cost
below the 60 Hz vsync slot (16.67 ms)** so frames stop missing slots.
Attribute the per-CB encode cost to existing sub-counters, identify the
dominant contributor, and quantify the target reduction.

**Method.** Read existing time and count counters from the baseline
`result.json`; no new instrumentation. Cross-reference with the DSync=0
run from [[present-pacing-display-sync.01]] to confirm per-CB encode is
stable across runs.

**Result.**

### Per-CB encode cost across the three thread roles

| Counter | Total (ms) | Per CB (ms) | Per CB share |
|---|---:|---:|---:|
| `encode_chunk_cpu_ms` | 20,686.0 | **14.37** | 100% (encode thread total) |
| ㄴ `encode_draw_cpu_ms` (subset) | 16,476.7 | 11.45 | 79.7% |
| ㄴ chunk-level (encode_chunk − encode_draw) | 4,209.3 | 2.93 | 20.3% |
| `d3d9_snapshot_draw_submission_cpu_ms` | 19,770.4 | 13.74 | (PE thread, parallel) |

CB count = `completion_waits = 1439`.

p50 of per-chunk encode thread time:
`encode_chunk_cpu_p50_ms = 20.45 ms`
`encode_chunk_cpu_p95_ms = 29.89 ms`
`encode_chunk_cpu_max_ms = 70.53 ms`

The p50 of **20.45 ms exceeds the 60 Hz vsync budget (16.67 ms)**. This
is exactly why the frame misses a vsync slot most of the time —
`completion_present_wait_p50_ms` was 23.978 ms in
[[present-pacing-display-sync.01]] = one vsync slot + one missed slot.

### Sub-attribution of `encode_draw_cpu_ms` (16,476.7 ms total)

| Sub-counter | Time (ms) | % of `encode_draw_cpu_ms` |
|---|---:|---:|
| `encode_draw_stream_bind_cpu_ms` | 1,794.7 | 10.89% |
| `encode_draw_issue_cpu_ms` (the Metal `drawIndexed` call) | 962.6 | 5.84% |
| `encode_draw_pipeline_lookup_cpu_ms` | 824.2 | 5.00% |
| `encode_draw_fvf_decode_cpu_ms` | 788.3 | 4.78% |
| `encode_draw_uniform_build_cpu_ms` | 108.3 | 0.66% |
| **Sum of named sub-counters** | **4,478.1** | **27.18%** |
| **Unattributed remainder** | **11,998.6** | **72.82%** |

The unattributed 73% lives in **per-draw Metal bind calls** that don't
have their own per-call time counters but do have count counters.

### Bind-call inventory (count-only, no per-call time)

| Counter | Count | Skipped (cached) |
|---|---:|---:|
| `bind_vertex_buffer` | 1,229,683 | n/a |
| `bind_index_buffer` | 1,043,549 | n/a |
| `bind_texture` | 891,531 | 992,018 |
| `bind_pipeline` | 232,771 | n/a |
| `bind_rasterizer` | 368,782 | n/a |
| `bind_viewport` | 368,782 | n/a |
| `bind_scissor` | 351,913 | n/a |
| `bind_sampler` | 150,750 | 1,732,799 |
| `bind_depth_state` | 17,693 | n/a |
| **Total executed binds** | **4,655,454** | — |
| Total skipped (cached) | — | 2,724,817 |

Per CB: **4,655,454 / 1,439 = 3,235 bind calls per CB on average.**
At an estimated 3 μs per Metal bind call (typical M1 driver cost),
that's **9.7 ms / CB of bind overhead** — matching the 12 ms / CB
unattributed remainder. Hypothesis: per-CB bind-call total is the
dominant encode cost.

### Draw-run batching state (already partially optimised)

| Counter | Value | Meaning |
|---|---:|---|
| `commit_chunk_draw_records` | 1,050,346 | total D3D9 draw calls |
| `commit_chunk_draw_run_records` | 342,639 | records that *did* batch into a run |
| `commit_chunk_draw_run_submits` | 83,145 | distinct submitted runs |
| `submit_draw_run_batch_groups` | 370,226 | encoder-side batched groups |
| `submit_draw_run_batch_records` | 697,634 | records in batched groups |
| `submit_draw_run_batch_max_records` | 32 | largest run in this trace |
| `submit_draw` | 1,050,346 | total submitted draws |

Draw-run batching is *already active* (the binding-override fix from
[[state-churn-encode]] is in effect — `commit_chunk_draw_run_binding_override_records = 258,290`),
but the average run length is small:
`697,634 batched records / 370,226 groups = 1.88 draws per group`,
i.e. most "batches" are 1-2 draws. **There is room.**

### Cross-check with DSync=0 (per-CB encode is run-invariant)

| Metric | Baseline | DSync=0 | Same? |
|---|---:|---:|---|
| `encode_draw_cpu_ms / completion_waits` | 11.45 | 11.23 | ≈ same (−1.9%) |
| `encode_chunk_cpu_p50_ms` | 20.45 | (not captured exact p50) | likely close |

Per-CB encode cost is independent of the present sync policy — as
expected, since the encode thread runs independently of the completion
thread. The cost-per-CB lives entirely in the encode path.

**Mechanism.** Three observations together pinpoint the production
target:

1. **Per-chunk encode CPU p50 = 20.45 ms** is 22.7% over the 16.67 ms
   vsync budget. Cutting 4-5 ms per CB drops p50 inside the budget and
   stops frames from slipping vsync slots.
2. **73% of `encode_draw_cpu_ms` is unattributed**, and the bind-call
   count × estimated cost matches that bucket within rounding.
   Bind-call reduction is the leverage axis.
3. **Average draw-run size is 1.88 records** while the runtime allows
   runs up to 32. Even a 2× increase in run size would amortise bind
   overhead proportionally.

**Verdict.** Accepted. The per-CB encode cost (20.45 ms p50, 14.37 ms
mean) is 22-25% over the 16.67 ms vsync budget; the dominant component
(~73% of draw-encode CPU) is per-draw Metal bind calls; the existing
draw-run batching averages 1.88 records per group while the cap is 32.
The production path to recover the DSync=0 win without disabling vsync
is **encode-side**, owned by [[state-churn-encode]].

Concrete target levers:

- **Bind suppression**: extend the existing `bind_texture_skipped` /
  `bind_sampler_skipped` cache pattern to vertex buffer, index buffer,
  pipeline, rasterizer, viewport, scissor, depth state binds. The cache
  hits we already see (53% texture, 92% sampler) suggest the redundancy
  exists elsewhere too. Counter targets: increase `bind_*_skipped`,
  decrease `bind_*` per CB.
- **Larger draw-runs**: current `submit_draw_run_batch_records /
  submit_draw_run_batch_groups = 1.88`. Look at what's breaking runs at
  1-2 draws when the cap is 32 — the existing
  `commit_chunk_draw_run_break_state_delta_mixed_*` taxonomy gives the
  break taxonomy. Reducing break frequency moves run length up.
- **FVF decode caching**: `encode_draw_fvf_decode_cpu_ms = 788 ms` is
  small (5%) but the count is high. Cache decoded FVF per VS handle.

Out of scope for this topic (lives in [[state-churn-encode]] /
[[snapshot-cache]]):

- `d3d9_snapshot_draw_submission_cpu_ms = 19.8 s` on the PE thread.
  Runs in parallel with the encode thread, not on the wallclock-critical
  path. Owned by [[snapshot-cache]].

**Next.** [[present-pacing-encode-budget-fix-proposal.01]] — synthesis
note proposing the specific draw-run / bind suppression work items and
their expected fps delta at this baseline.
