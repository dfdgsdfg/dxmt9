---
domain: index-cache-locality
subcategory: opaque
order: 04
title: Current Opaque-Depth Opt-in No-Gputrace Scout
date: 2026-06-05
type: experiment-run
status: accepted
source: specs/perfomance.plan.md#L101-L178
---

# Current Opaque-Depth Opt-in No-Gputrace Scout

**Question / hypothesis.** Re-validate that the accepted opaque-depth locality
opt-in is still active and correctly scoped on the *current* tree (post layout-stride),
as a no-gputrace runtime-health scout.

**Method.** `run_3dmark05_perf_probe.sh --suffix current-opaque-depth-index-cache-nogputrace-r1
--frame 50 --encoder-breakdown-seq 50 --no-gputrace --optimize-opaque-depth-index-cache
--optimize-opaque-depth-index-cache-min-gain-pct 10 --timeout 180 --top 5`.
Timeout-finalized pass (`returncode=143`, `present_encoded=1440`).

**Result.** Opt-in is not a no-op: `cache_lookups=198`, `runtime_applied_draws=102`,
`runtime_skipped_draws=96`, `cache_hits=102`, `cache_rejected_hits=96`,
`candidate_miss_delta32=-126,083` (`-27.41%` LRU32), `probe_draw_rows=198`.
Run-level vs baseline: `draw_calls +0.06%`, `render_pass_begin +0.03%`,
`gpu_command_buffer_time_ms 4,193.474→4,216.350` (`+0.55%`),
`completion_wait_ms 31,661.993→28,240.918` (`-10.80%`),
`encode_draw_stream_bind_cpu_ms 1,797.070→2,604.054` (`+44.91%`, diagnostic-run overhead).

**Verdict.** Accepted (mechanism active on current tree). Not a replacement for the
Xcode proof; CPU stream-bind delta is diagnostic-run overhead, not profile-default
evidence. Do not flip the shared `perf` profile from this scout alone.

**Related.** [[index-cache-locality]] · prev: [[index-cache-locality-opaque.03]]
· next: [[index-cache-locality-opaque.05]] · [[tvb-mechanism-proof]].
