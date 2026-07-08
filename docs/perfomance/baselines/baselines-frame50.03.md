---
domain: baselines
workload: 3DMark05 GT1
subcategory: frame50
order: 03
title: Current Timeout No-Gputrace Scout
date: 2026-06-05
type: scout
status: accepted
source: specs/perfomance.plan.md#L34-L99
---

# Current Timeout No-Gputrace Scout

**Question / hypothesis.** Re-run the current tree through the standard wrapper
without `.gputrace` as a timeout-policy + run-level counter scout — refresh the
no-gputrace baseline, not an Xcode GPU proof.

**Method.**
```bash
scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix current-timeout-nogputrace-r1 \
  --frame 50 \
  --encoder-breakdown-seq 50 \
  --no-gputrace \
  --timeout 180 \
  --top 5
```

**Result.** Valid counter sample, not a wall-clock FPS sample:
`status=pass`, `timed_out=true`, `returncode=143` (supervisor terminated the
final-frame process group), `present_encoded=1440`,
`process_elapsed_sec=251.250` (includes capture delay + timeout tail; not FPS).

Vs the prior 1440-present `current-sanity-frame50-nogputrace-r1`:
- `draw_calls` `1,047,059` → `1,050,054` (`+0.29%`)
- `render_pass_begin` `16,876` → `16,867` (`-0.05%`)
- `render_pass_tile_preservation_bytes` `181,277,986,816` → `180,772,012,032` (`-0.28%`)
- `argbuf_hybrid_bytes_per_encoder` `1,206,864,168` → `1,215,105,424` (`+0.68%`)
- `transient_upload_bytes` `1,206,985,044` → `1,215,226,300` (`+0.68%`)
- `encode_draw_cpu_ms` `16,250.250` → `15,898.083` (`-2.17%`)
- `submit_draw_cpu_ms` `3,116.490` → `3,132.182` (`+0.50%`)
- `encode_draw_stream_bind_cpu_ms` `1,694.859` → `1,797.070` (`+6.03%`)
- `gpu_command_buffer_time_ms` `4,151.436` → **`4,193.474`** (`+1.01%`)
- `completion_wait_ms` `31,880.707` → `31,661.993` (`-0.69%`)

`3dmark05-index-cache-runtime-summary.md` reports `no-cache-runtime-activity`
(expected: no index-cache flag enabled).

**Verdict.** Accepted as a no-gputrace baseline refresh (2026-06-05). The
timeout path works as intended (artifacts produced, no manual kill); the
no-gputrace runtime shape is essentially unchanged from the prior sanity run, so
this alone does not justify a new Xcode capture. The next `.gputrace` budget
stays gated by the proof queue (accepted opaque-depth locality + explicit `lsb1`
screen-blend; broad depth-read reorder still needs a final-color oracle).

**Related.** [baselines](../baselines.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md) · [baselines-frame50.02](baselines-frame50.02.md) (prior scout) ·
[baselines-frame50.01](baselines-frame50.01.md) (canonical gputrace baseline) ·
[index-cache-locality](../index-cache-locality.md) (no-cache-runtime-activity reference) · [hidden-backend-storage](../hidden-backend-storage.md).
