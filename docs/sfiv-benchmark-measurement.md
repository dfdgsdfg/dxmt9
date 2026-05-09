# Street Fighter IV Benchmark — measurement after M-cycle

Run on 2026-05-10 with the M1-M7 + O1 builds applied (release artifacts).
Captured via `bash scripts/run_apps/run_sfiv_benchmark_experiment.sh` with
`DXMT_PERF_COUNTERS=1 DXMT9_PERF_FRAME_SAMPLING=1`. Output in
`experiments/output/street-fighter-iv-benchmark-post-mcycle-010048/`.

## Top-line

| Metric | Value |
|---|---|
| process_elapsed_sec | 176.79 |
| frames captured | 2343 |
| avg fps | **13.25** |
| frame budget | 75.5 ms |
| command buffers / frame | **1** (no pipelining) |
| render passes / frame | 16 mean / 27 max |
| draw calls / frame | 74 mean / 269 max |
| triangles / frame | 56k mean / 304k max |

(2243 steady-state frames, dropping the first 100 as warmup.)

## Frame-time decomposition (steady-state, ms)

| Counter | mean | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|---:|
| `encode_chunk_cpu_ms` | **68.91** | 98.25 | 116.07 | **132.26** | 314.24 |
| `completion_wait_ms` | **69.01** | 97.72 | 119.27 | **130.39** | 250.48 |
| `present_acquire_wait_ms` | 65.64 | 92.34 | 113.47 | 126.98 | 311.74 |
| `present_boundary_wait_ms` | 58.94 | 82.02 | 104.80 | 117.41 | 239.87 |
| `submit_draw_cpu_ms` | 1.30 | 0.30 | 3.07 | 27.62 | 52.20 |
| `encode_draw_cpu_ms` | 2.08 | 0.91 | 4.91 | 11.64 | 282.48 |
| `encode_draw_pipeline_lookup_cpu_ms` | 0.66 | 0.22 | 1.18 | 3.50 | 281.13 |
| `encode_draw_uniform_build_cpu_ms` | 0.13 | 0.07 | 0.25 | 1.55 | 12.51 |
| `encode_draw_stream_bind_cpu_ms` | 0.85 | 0.33 | 2.40 | 3.40 | 5.15 |
| `transient_upload_cpu_ms` | 0.19 | 0.07 | 0.31 | 3.24 | 17.28 |

## Key observation — encode-chunk dominates, and most of it is *not* per-draw

Sum of all encode-draw sub-counters at the mean:
`encode_draw + pipeline_lookup + uniform_build + fvf_decode + stream_bind +
issue + transient_upload ≈ 4.0 ms`.

`encode_chunk_cpu_ms = 68.9 ms` mean.

**~64 ms / frame (93% of encode_chunk) is unaccounted for in per-draw
counters.** Suspects, in order of likelihood:

1. **Render-pass setup** — 16-27 RPs/frame at ~3-4 ms each lines up
   neatly with 64 ms. Includes RP descriptor construction, color/depth
   load/store action computation, tile-preservation byte calculation,
   touched-color-handle bookkeeping (R-BACK-15.4).
2. **Hazard tracking** between draws / encoders. Counter exists but is
   probably swallowed in the chunk-encode wall and not broken out.
3. **State-block diff and bind-churn computation** between passes.

`completion_wait_ms` mean (69 ms) tracking `encode_chunk_cpu_ms` (69 ms)
suggests CPU-encode and GPU-execute are roughly equal-sized phases that
**run mostly concurrently**, but with `command_buffers=1/frame` neither
pipelines into the next frame. Frame time ≈ max(encode, GPU) + present
boundary stall.

## What the bottleneck *isn't*

- Per-draw work is small. encode_draw mean 2.1 ms / 74 draws ≈ 28 µs/draw.
- pipeline_lookup is fast (cache hits dominate).
- uniform builds, FVF decode, stream binds are all sub-ms.
- Pipeline cache: 0 builds during steady-state (warmup absorbed them).

## Bottleneck shift since the last measurement

Earlier session's note (`docs/perfomance-bottleneck.md`) characterized
SFIV as "GPU-bound (114s of 120s wall)". This run, with M-cycle work
applied:

- encode_chunk_cpu mean (69 ms) is now the dominant **CPU** cost.
- completion_wait (69 ms) tracks it — they're concurrent, not sequential.
- Frame budget (75 ms) is consumed by **the larger of CPU encode or GPU
  execute**, plus a present-boundary stall.

The shift is consistent with R-BACK-12 (per-frequency uniform layout)
and R-BACK-15 (load/store action policy) reducing previous CPU encode
work, but render-pass setup overhead now dominates.

## Gaps identified during this run

1. **Per-frame snapshot lacks GPU CB time and GPU errors** — fixed in
   the same commit that landed this note: `gpu_command_buffer_time_ms`,
   `gpu_command_buffer_time_samples`, `gpu_command_buffer_errors` now
   appear in every `[dxmt9-perf-frame …]` line. Re-running the
   benchmark with the rebuilt release artifacts will let us compare
   GPU wall time vs `completion_wait_ms` directly.
2. **`encode_chunk_cpu` lacks sub-counters** — the 64 ms of unaccounted
   encode work needs its own breakdown (proposed: `encode_render_pass_cpu_ms`,
   `encode_hazard_track_cpu_ms`, `encode_state_diff_cpu_ms`).
3. **Single command buffer per frame** — by design, but it serializes
   CPU-encode with GPU-completion. Multi-buffering would let the
   encode-thread start frame N+1 while GPU runs frame N.

## Next-cycle proposals

| ID | Item | Cost | Impact |
|---|---|---|---|
| Q1 | Sub-counters for `encode_chunk_cpu` (render-pass setup, hazard, state-diff buckets) | medium | resolves the 64 ms mystery |
| Q2 | Re-run SFIV with the per-frame snapshot extension (this commit) | small | gives GPU vs CPU concurrency picture |
| Q3 | Capture one frame as `.gputrace` and inspect render-pass count + descriptor cost in Xcode | small | empirical check on the render-pass-setup hypothesis |
| Q4 | Multi-buffering: 2-3 command buffers in flight | medium | hides GPU latency under CPU encode |

## Reproduction

```sh
DXMT_PERF_COUNTERS=1 DXMT9_PERF_FRAME_SAMPLING=1 \
  bash scripts/run_apps/run_sfiv_benchmark_experiment.sh \
  --binary "$HOME/.cache/dxmt9/sfiv-benchmark/extracted/Program Files/CAPCOM/STREETFIGHTERIV_BENCHMARK/StreetFighterIV_Benchmark.exe" \
  --pe-build-dir build-win32-x64-builtin-release/src/win32 \
  --runtime-pe-build-dir build-win32-x64-builtin-release/src/winemetal \
  --unix-build-dir build-x86_64-builtin-release/src \
  --timeout 150
```

To capture a single frame:

```sh
DXMT_PERF_COUNTERS=1 DXMT9_PERF_FRAME_SAMPLING=1 \
DXMT_METAL_CAPTURE_FRAME=2000 DXMT_METAL_CAPTURE_PATH=/tmp/sfiv.gputrace \
  bash scripts/run_apps/run_sfiv_benchmark_experiment.sh ...
```
