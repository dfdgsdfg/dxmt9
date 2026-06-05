---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: encode-budget
order: 02
title: Production Fix Proposal — Encode-Side Reduction To Recover DSync=0 Fps Under Vsync
date: 2026-06-05
type: synthesis
status: proposed
source: present-pacing-display-sync.01, present-pacing-frame-latency.01, present-pacing-async-acquire.01, present-pacing-encode-budget.01, state-churn-encode
---

# Production Fix Proposal — Encode-Side Reduction To Recover DSync=0 Fps Under Vsync

**Question.** Steps 1-4 narrowed the GT1 wallclock bottleneck. What is
the production-safe path to recover the +199% fps observed under
`DXMT9_LAYER_DISPLAY_SYNC=0`, and how confident is the expected delta?

**Method.** Synthesise the four prior measurements. No new run.

**Result.**

### Funnel of decisive findings

```
Step 1 — DSync=0 disables CAMetalLayer display sync
         scene wallclock 251.07 s → 83.02 s     (-66.9%, fps ×2.99)
         per-CB wait p50 essentially unchanged   (23.978 → 23.688 ms)
         CB throughput 5.73 → 10.11 CB/s         (+76.5%)
                       ↓
Step 2 — MAX_FRAME_LATENCY=3 + CAP=0 (vsync on)
         scene wallclock 251.07 → 251.26 s       (+0.07%, noise)
         present_boundary_wait 0 → 526 ms        (env confirmed reached)
         Verdict: rejected — queue depth ≠ throughput under vsync
                       ↓
Step 3 — PRESENT_ASYNC_ACQUIRE=1
         scene wallclock 251.07 → 251.61 s       (+0.22%, noise)
         present_acquire_wait 153.6 → 96.0 ms    (-37.5%, axis < 0.5%)
         encode_draw_cpu +8.3%                   (acquire moved to encode)
         Verdict: rejected — acquire axis not load-bearing
                       ↓
Step 4 — encode budget attribution (no env change)
         encode_chunk_cpu_p50 = 20.45 ms
         vsync slot           = 16.67 ms
         over budget by       = +3.78 ms (+22.7%)

         encode_draw_cpu breakdown:
           named sub-counters (stream_bind + issue + pipeline_lookup +
                               fvf_decode + uniform_build) = 27%
           unattributed remainder                          = 73%
                                                          ≈ per-draw
                                                            Metal bind
                                                            calls

         draw-run state:
           avg records / batch group = 1.88 (cap = 32)
           binding-override fix already landed
```

### Why the present-side knob space is foreclosed

Three independent toggles tested:

| Toggle | Direct effect | Wallclock effect |
|---|---|---:|
| `DXMT9_LAYER_DISPLAY_SYNC=0` | disables compositor pacing | **−66.9%** (diagnostic; breaks visual sync) |
| `DXMT9_MAX_FRAME_LATENCY=3 + CAP=0` | deeper in-flight queue | +0.07% (noise) |
| `DXMT9_PRESENT_ASYNC_ACQUIRE=1` | acquire moved off completion path | +0.22% (noise) |

The DSync=0 result proves the upside exists (≈3× fps). The other two
toggles prove that under `displaySyncEnabled=YES`, no present-side
configuration recovers it. The compositor paces presents at the display
refresh rate; queue depth and acquire timing do not change that rate.
The only remaining axis is the per-CB encode cost that determines
whether each frame fits the 16.67 ms vsync slot.

### Concrete production work items

The target: drop **per-chunk encode CPU p50 from 20.45 ms to ≤ 16.67 ms**
(Δ ≥ 3.78 ms / CB, ≥ 18.5% reduction). Below the budget, frames stop
slipping vsync slots; the per-CB completion wait collapses from
~24 ms (one slot + one missed) to ~17 ms (one slot), and wallclock
scales proportionally.

#### Work item A — extend bind-skipped cache pattern (highest-confidence)

Today only `bind_texture` and `bind_sampler` have `_skipped` counters
and a redundant-bind cache. Skip rates are 53% (texture) and 92%
(sampler), confirming the pattern works. Per-CB bind inventory:

```
bind_vertex_buffer      1,229,683 ÷ 1,439 CBs = ~855 / CB (no cache)
bind_index_buffer       1,043,549 ÷ 1,439 CBs = ~725 / CB (no cache)
bind_texture              891,531 ÷ 1,439 CBs = ~620 / CB (53% skip)
bind_rasterizer           368,782 ÷ 1,439 CBs = ~256 / CB (no cache)
bind_viewport             368,782 ÷ 1,439 CBs = ~256 / CB (no cache)
bind_scissor              351,913 ÷ 1,439 CBs = ~245 / CB (no cache)
bind_pipeline             232,771 ÷ 1,439 CBs = ~162 / CB (no cache)
bind_sampler              150,750 ÷ 1,439 CBs = ~105 / CB (92% skip)
bind_depth_state           17,693 ÷ 1,439 CBs = ~12  / CB (no cache)
```

At an estimated 3 μs / bind, the seven non-cached classes (vertex
buffer, index buffer, rasterizer, viewport, scissor, pipeline, depth
state) total **~7.9 ms / CB**. If a redundant-state cache mirroring
the texture/sampler pattern matches the same 50%+ skip rate, the
saving is ~4 ms / CB — enough to fit the vsync budget single-handedly.

Counter targets for proof:

```
bind_vertex_buffer_skipped > 0   (new counter, same style as texture)
bind_index_buffer_skipped  > 0
bind_pipeline_skipped      > 0
bind_rasterizer_skipped    > 0
bind_viewport_skipped      > 0
bind_scissor_skipped       > 0
bind_depth_state_skipped   > 0
encode_chunk_cpu_p50_ms   ≤ 16.67
process_elapsed_sec       ≤ 167  (= 251 / 1.5 conservative)
```

#### Work item B — extend draw-run length (orthogonal multiplier)

Current `submit_draw_run_batch_records / submit_draw_run_batch_groups =
697,634 / 370,226 = 1.88 records / group`. The cap is 32. The
[[state-churn-encode]] taxonomy already identifies the break classes
(`commit_chunk_draw_run_break_state_delta_mixed_*`). The largest
sub-bucket today is the `mixed_pair_stream_texture` and
`mixed_pair_stream_ib` family — same-stream sequences interrupted by
texture or IB changes.

Targeted reductions in those break classes would raise mean run length.
Each doubling of run length amortises the constant-per-run cost
(currently ~83 k submits × per-run overhead) by 2×.

#### Work item C — FVF decode caching (smaller, but cheap)

`encode_draw_fvf_decode_cpu_ms = 788 ms` over 1,050 k draws = 0.75 μs /
draw. Caching by VS handle removes most of it (~700 ms total = 0.5 ms /
CB). Low-impact alone but cheap to land alongside A.

#### Out of scope for this topic

- `d3d9_snapshot_draw_submission_cpu_ms = 19.8 s` (PE-thread D3D9 state
  snapshot) runs in parallel with encode; owned by
  [[snapshot-cache]].
- Index-cache locality (the existing −13.86% GPU win) is GPU-side and
  orthogonal; the index-locality work and the encode work compose
  additively. Owned by [[index-cache-locality]].
- `DXMT9_LAYER_DISPLAY_SYNC=0` as a fallback opt-in flag — possible but
  unrecommended; produces tearing.

### Expected fps delta if Work A lands

Conservative assumption: bind-suppression cache hits 50% on
vertex/index buffers (matching texture's 53%) and 30% on the others.

```
saved per CB     ≈ 7.9 ms × 0.45 = 3.6 ms (midpoint)
new p50 encode  ≈ 20.45 - 3.6   = 16.85 ms
new wait p50    ≈ 16.67 ms (fits one slot)
scene wallclock ≈ 251 / (24 / 16.67) × correction ≈ 175 s
implied fps     ≈ 1.44× baseline (+44%)
```

If bind-suppression also reaches the sampler-class 92% skip rate
asymptote on the cached binds (less likely but possible), the upside
extends toward the DSync=0 ceiling (+199%).

**Verdict.** Proposed. Two pieces of work tracked in the
[[state-churn-encode]] topic should land **bind-skipped cache for
vertex_buffer / index_buffer / pipeline / rasterizer / viewport /
scissor / depth_state** (Work A) and a follow-up **draw-run break
reduction targeting the `mixed_pair_stream_*` taxonomy** (Work B).
Acceptance criterion is `encode_chunk_cpu_p50_ms ≤ 16.67 ms` on
3DMark05 GT1; expected wallclock impact +44% (conservative) to +199%
(if matching DSync=0 ceiling) without disabling display sync.

**Out of scope of *this* synthesis** (sized but not executed here):
the actual C++ implementation of the bind-cache extension, the
state-churn-encode break-class reduction, and the meson tests that
gate the new `bind_*_skipped` counters. Those land in
[[state-churn-encode]].
