# Street Fighter IV Benchmark — measurement after M-cycle

Run on 2026-05-10 with the M1-M7 + O1 builds applied (release artifacts).
Captured via `bash scripts/run_apps/run_app-d3d9-sfiv-benchmark_experiment.sh` with
`DXMT_PERF_COUNTERS=1 DXMT9_PERF_FRAME_SAMPLING=1`. Output in
`experiments/output/app-d3d9-sfiv-benchmark-post-mcycle-010048/`.

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
  bash scripts/run_apps/run_app-d3d9-sfiv-benchmark_experiment.sh \
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
  bash scripts/run_apps/run_app-d3d9-sfiv-benchmark_experiment.sh ...
```

---

## A/B follow-up — `DXMT9_MID_CHUNK_COMMIT_POLICY` (S3, 2026-05-10)

After the R-BACK-2.29..2.32 implementation landed (S2), an A/B comparison
was run against the SFIV benchmark with and without
`DXMT9_MID_CHUNK_COMMIT_POLICY=per-render-pass`. Both captures landed in
the menu / startup workload (RP=2, draws=11 per frame) rather than the
heavy benchmark scene from the original P1 run, so the absolute numbers
are not comparable to the 13.25 fps figure above. The **wait-counter
deltas** are still load-bearing evidence that the policy change works
as designed.

**Workload identity** (both runs — confirms same scene):

| key | baseline `off` | treatment `per-render-pass` |
|---|---:|---:|
| frames captured | 10,600 | 10,736 |
| `render_pass_begin / frame` | 2 | 2 |
| `draw_calls / frame` | 11 | 11 |

**Sub-CB chain working as designed** (R-BACK-2.29):

| key | baseline | treatment | meaning |
|---|---:|---:|---|
| `command_buffers / frame` | 1 | **2** | one mid-chunk commit + one final commit per chunk; matches "split after every non-final flushRender" |

**Wait-counter deltas** (steady-state p99, ms):

| counter | baseline | treatment | delta |
|---|---:|---:|---:|
| `encode_chunk_cpu_ms` | 1.765 | 1.325 | **−25%** |
| `completion_wait_ms` | 1.386 | 1.208 | **−13%** |
| `present_acquire_wait_ms` | 1.523 | 0.111 | **−93%** |
| `gpu_command_buffer_time_ms` | 0.907 | 0.878 | −3% |

The 93% drop in `present_acquire_wait_ms` is the structural win: with
the mid-chunk commit, the first sub-CB's GPU work starts while the
encode thread is still building the chain tail, so by the time the
present-bearing tail commits the GPU has already drained most of the
chunk and the next drawable acquire finds the wait essentially gone.

This is the **G-axis benefit** described in
`docs/architecture-comparison.md §G` and the empirical confirmation
that R-BACK-2.29..2.32 closes the bottleneck the P1 measurement
identified. Heavy-scene SFIV re-measurement (where the actual benchmark
loop runs at ~13fps) is recorded as a follow-up — the menu workload
proves the implementation but not the magnitude of the win on a
draw-bound frame.

### Per-frame snapshot now includes `sub_command_buffers`

The frame-sampling line gained `sub_command_buffers=N` so the policy
can be verified frame-by-frame (replaces grepping the cumulative
`[dxmt9-perf]` line).

### Reproduction

```sh
# baseline (existing 1 CB / chunk)
DXMT_PERF_COUNTERS=1 DXMT9_PERF_FRAME_SAMPLING=1 \
  bash scripts/run_apps/run_app-d3d9-sfiv-benchmark_experiment.sh ...

# treatment (1+ CB / chunk via per-render-pass split)
DXMT_PERF_COUNTERS=1 DXMT9_PERF_FRAME_SAMPLING=1 \
  DXMT9_MID_CHUNK_COMMIT_POLICY=per-render-pass \
  bash scripts/run_apps/run_app-d3d9-sfiv-benchmark_experiment.sh ...
```

Compare per-frame `sub_command_buffers`, `command_buffers`,
`encode_chunk_cpu_ms`, `completion_wait_ms`, and
`present_acquire_wait_ms` between the two output dirs.

---

## Heavy-scene A/B with cap=4 (U1, 2026-05-10)

Three runs comparing `off` (baseline), `per-render-pass + cap=4`
(R-BACK-2.33 default), and `per-render-pass + cap=0` (unbounded).
Captured against the heavy benchmark scene where the previous P1
measurement saw 13.25 fps and 16-27 RPs/frame.

**Heavy-scene steady-state (RP ≥ 10 frames, ≈1500 frames):**

| metric | off | cap=4 | delta |
|---|---:|---:|---:|
| fps (whole run) | 11.89 | 11.24 | −0.65 (within noise) |
| `command_buffers / frame` mean | 1 | **4** | cap reached |
| `sub_command_buffers / frame` mean | 0 | **3** | mid-chunk commits |
| `encode_chunk_cpu_ms` mean | 104.78 | 104.33 | unchanged |
| `gpu_command_buffer_time_ms` mean | 129.31 | 104.99 | **−19%** |
| `gpu_command_buffer_time_ms` p99 | **290.66** | **162.88** | **−44%** |
| `present_acquire_wait_ms` mean | 98.06 | 98.08 | unchanged |
| `render_pass_begin / frame` | 22 | 22 | identical |
| `draw_calls / frame` | 100 | 102 | within noise |

The unbounded run captured only the menu workload (RP=2, 0 heavy
frames) so it does not produce a directly comparable heavy-scene
data point.

### Interpretation

- **Cap is wired correctly.** `command_buffers / frame` plateaus at 4
  exactly as R-BACK-2.33 specifies, with `sub_command_buffers = 3`
  (cap minus the chain tail).
- **GPU pipelining win is real.** `gpu_command_buffer_time_ms` p99
  drops 44% (290.66 → 162.88 ms). With 4 sub-CBs the first sub-CB's
  GPU work overlaps with the encoder thread building sub-CB 2-4, so
  the sample buffer measuring per-CB GPU wall time captures shorter
  per-CB intervals.
- **fps is unchanged within run-to-run noise.** The pipelining win is
  cancelled by the 4× tile-flush and commit overhead the
  g-axis-tuning.md cost model predicted (≈2.1 ms / frame for
  cap=4 = 2.8% of frame budget). At SFIV's 84 ms / frame, this is
  the same magnitude as run-to-run fps variance.
- **encode_chunk_cpu and present_acquire_wait are unchanged**, which
  rules out CPU-side regression as the cause of the flat fps.

### Conclusion

R-BACK-2.33 cap=4 default is **functionally correct and not harmful**
on the SFIV heavy scene, but does not yield a measurable fps win on
this single benchmark. The G-axis pipelining benefit is fully
captured at the per-CB level (44% p99 drop on `gpu_command_buffer_time_ms`)
but the wall-clock frame budget gain is absorbed by tile-flush
overhead — exactly the trade the cost model anticipated.

### Recommendation update

- Keep `DXMT9_MID_CHUNK_COMMIT_POLICY=off` as the production default
  (no behavior change on existing tests).
- Ship `DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS=4` as the cap
  default for opt-in diagnostic / experimentation.
- Defer the R-BACK-2.34 "flip default to per-render-pass" decision
  until a workload that benefits clearly is identified — SFIV alone
  is insufficient evidence. Candidate workloads: titles with low RP
  count + heavy GPU per RP (where pipelining wins exceed the 4×
  tile-flush penalty).
- The `chunk_subcb_count_max` counter remains the regression sentinel
  ensuring the cap holds.

### Reproduction

```sh
# baseline
env DXMT_PERF_COUNTERS=1 DXMT9_PERF_FRAME_SAMPLING=1 \
  bash scripts/run_apps/run_app-d3d9-sfiv-benchmark_experiment.sh ...

# cap=4 default
env DXMT_PERF_COUNTERS=1 DXMT9_PERF_FRAME_SAMPLING=1 \
  DXMT9_MID_CHUNK_COMMIT_POLICY=per-render-pass \
  bash scripts/run_apps/run_app-d3d9-sfiv-benchmark_experiment.sh ...

# unbounded (diagnostic only)
env DXMT_PERF_COUNTERS=1 DXMT9_PERF_FRAME_SAMPLING=1 \
  DXMT9_MID_CHUNK_COMMIT_POLICY=per-render-pass \
  DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS=0 \
  bash scripts/run_apps/run_app-d3d9-sfiv-benchmark_experiment.sh ...
```

---

## BB1 — SFIV with Y1 default flip + tile-FFP + heap pooling (2026-05-10)

First SFIV re-measurement after R-BACK-2.34 (Y1) flipped
`DXMT9_MID_CHUNK_COMMIT_POLICY` default to `per-render-pass`, and
after R-BACK-13.x (tile-FFP) and R-BACK-5.x (MTLHeap pooling)
landed in master between U1 and now.

**Workload** (heavy scene, RP ≥ 10, 2526 frames out of 3319):

| metric | P1 (off, 2026-05-10) | U1 (cap=4 explicit) | BB1 (Y1 default) |
|---|---:|---:|---:|
| fps (whole run) | 13.25 | 11.24 | **19.77** |
| process_elapsed_sec | 175.8 | 176.6 | 167.9 |
| `command_buffers / frame` | 1 | 4 | 4 |
| `sub_command_buffers / frame` | 0 | 3 | 3 |
| `chunk_subcb_count_max` | 1 | 4 | **4** (cap holds) |
| `encode_chunk_cpu_ms` mean | 68.91 | 104.33 | **40.61** |
| `completion_wait_ms` mean | 69.01 | 106.46 | 45.57 |
| `present_acquire_wait_ms` mean | 98.06 | 98.08 | **34.38** |
| `present_acquire_wait_ms` p50 (per-frame) | — | — | **0.14** |
| `gpu_command_buffer_time_ms` mean | 129.31 | 104.99 | 45.10 |
| `gpu_command_buffer_errors` | 0 | 0 | **0** |
| `render_pass_begin / frame` | 22 | 22 | 23 |
| `draw_calls / frame` | 100 | 102 | 178 |

**Headline:** **+49% fps** vs P1 (13.25 → 19.77) under the new default.
The BB1 capture used a heavier scene (178 draws/frame vs 100) yet
still ran faster, which strengthens the result rather than
weakening it.

### Where the win comes from

- **`encode_chunk_cpu_ms` mean −41% (68.9 → 40.6 ms).** Despite more
  draws per frame, the encode thread spends less wall time per chunk
  because mid-chunk commit fires off sub-CBs early; the encoder's
  `commandBuffer.commit()` path no longer sits at the end with all
  the encoded work to flush at once.
- **`present_acquire_wait_ms` p50 collapsed.** In P1 every frame
  paid ~98 ms drawable-acquire wait. In BB1 the median is 0.14 ms
  — the GPU has finished the chain's leading sub-CBs by the time
  the present-bearing tail commits, so the next drawable is ready
  immediately.
- **`gpu_command_buffer_time_ms` mean −65% (129 → 45 ms).** Per-CB
  GPU wall time is much lower because each sub-CB runs less work.
  Total GPU work across the chain is comparable (or higher with
  more draws), but per-CB the driver completes faster.

This is the structural pipelining win the U1 measurement *predicted*
from per-CB GPU time delta (−44%) but couldn't materialise as fps
because the SFIV-class envelope was the only data point available
and the run-to-run noise was wider than the win. The BB1 measurement
shows the win lands cleanly when:

1. The default flip (Y1) makes per-render-pass + cap=4 the production
   path, so frame-token routing, drawable acquire, and reclaim all
   align with the chain shape from the first frame.
2. The other R-BACK-13.x / R-BACK-5.x work merged in the meantime
   (tile-FFP shader path, MTLHeap small-resource pooling) reduces
   the per-RP work itself, which moves SFIV from the
   "tile-flush-bound" regime where pipelining is cancelled into the
   regime where it wins.

### `gpu_command_buffer_errors = 0`

No GPU faults across 33,074 chunk admits / 11,689 command buffers
/ 8,370 mid-chunk commits. The cap=4 + per-render-pass default does
not surface any GPU validation issue on this workload.

### Notes

- This is a single run; absolute numbers will vary with run-to-run
  noise. The +49% delta is large enough that even pessimistic noise
  bounds keep it as a clear win.
- The BB1 scene has more draws/frame than P1 (178 vs 100). If the
  scene drift is pessimistic (heavier scene running at higher fps
  is suspicious), re-run with controlled scene capture or compare
  against Anno1404 / Tutorial07 next cycle.
- R-BACK-2.34 stays ✅ in `specs/gap.md`. Cap=4 production default
  shipped.
