---
domain: baselines
subcategory: frame50
order: 02
title: Current Sanity Frame50 No-Gputrace
date: 2026-06-04
type: scout
status: accepted
source: specs/perfomance.plan.md#L1482-L1536
---

# Current Sanity Frame50 No-Gputrace

**Question / hypothesis.** Confirm the frame50 runtime shape and counters are
stable across code changes without paying for a `.gputrace`/Xcode export.

**Method.**
```bash
scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix current-sanity-frame50-nogputrace-r1 \
  --frame 50 \
  --encoder-breakdown-seq 50 \
  --no-gputrace \
  --timeout 420
```
Screenshot is a normal GT1 frame. `result.json`: `status=pass`,
`timed_out=true`, `returncode=-15` after `491.18s` (timeout-terminated, not a
clean score). The captured HUD shows **FPS: 9**.

**Result.** Shape stability is the point. Frame `50` rows `0..3` match
`cache-opt-candidate-frame50-r4` exactly for draw count, vertex count, triangle
estimate, alpha/scissor state, stream/IB churn:

| Row | Draws | Vertices | Triangles | Alpha | Scissor | Stream h/o/s | IB |
|---|---:|---:|---:|---:|---:|---:|---:|
| `50/0` | 42 | 291,882 | 97,294 | 0 | 0 | 36/0/0 | 36 |
| `50/1` | 156 | 686,175 | 228,725 | 0 | 0 | 130/12/0 | 130 |
| `50/2` | 187 | 1,168,128 | 389,376 | 145 | 42 | 271/0/10 | 160 |
| `50/3` | 1 | 6 | 2 | 0 | 0 | 0/0/0 | 0 |

Run-level: `queue_sequence_wait_ms=0`, `map_buffer_wait_ms=0`,
`completion_wait_ms=31880.707`, `gpu_command_buffer_time_ms=4151.436`,
`encode_draw_cpu_ms=16250.250`, `submit_draw_cpu_ms=3116.490`,
`encode_draw_stream_bind_cpu_ms=1694.859`,
`render_pass_tile_preservation_bytes=181277986816`. Frame50 top rows still have
`0` transient vertex bytes and only sub-MiB argbuf/setVertexBytes traffic.

**Verdict.** Accepted as a no-gputrace shape baseline. FPS=9 confirms the path
is still slow; rows match prior samples, so this run does not change the primary
diagnosis (GPU = hidden vertex/tiler/parameter storage + render-pass
preservation; CPU = const-upload + stream/IB/texture churn). Superseded for GPU
proof by [[baselines-frame50.01]].

**Related.** [[baselines]] · [[overview]] · [[baselines-frame50.01]]
(canonical gputrace baseline) · [[baselines-frame50.03]] (later no-gputrace scout) ·
[[hidden-backend-storage]] · [[render-pass-store]] (tile preservation).
