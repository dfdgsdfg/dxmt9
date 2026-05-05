# Benchmarks Requirements

Benchmarks measure the performance of dxmt9 and compare it against reference
stacks. A benchmark failure means a measurable performance regression, not a
correctness issue.

---

## 1. Scope

**R-BENCH-1.1** Benchmarks must be runnable without Wine as native macOS
executables that drive the dxmt9 backend directly.

**R-BENCH-1.2** Each benchmark must be deterministic: same workload, same
machine, same result within ±2% run-to-run variance.

**R-BENCH-1.3** Benchmarks must not overlap with tests or experiments. A
benchmark exercises the same code paths as a test but measures time, not
correctness.

---

## 2. Workloads

**R-BENCH-2.1** The following workloads must be benchmarked:

| Workload | Metric | What it measures |
|---|---|---|
| Draw call throughput | Draws/sec (CPU-bound) | Stack overhead per draw call |
| State change cost | Draws/sec alternating RS | Shadow + PSO cache efficiency |
| PSO compile — cold | ms to first frame | Shader compile latency, empty cache |
| PSO compile — warm | ms to first frame | Disk cache (`MTLBinaryArchive`) hit rate |
| Texture upload throughput | MB/s | `mapBuffer` + upload path |
| Readback throughput | MB/s | `GetRenderTargetData` staging path |
| Bridge op budget | bridge ops/frame and ops/draw | PE/unix hot-path batching stays DXMT-shaped |
| Frame time — simple scene | ms/frame (P50, P95, P99) | End-to-end frame consistency |
| Frame time — heavy scene | ms/frame (P50, P95, P99) | Under GPU load |

**R-BENCH-2.2** Draw call throughput must be measured with:
- 1,000 draws/frame, identical state (measures minimum overhead)
- 1,000 draws/frame, alternating render state (measures PSO cache hit path)
- 1,000 draws/frame, unique shader per draw (measures PSO cache miss path)

**R-BENCH-2.3** Bridge operation budget must be measured for the same draw
throughput scenarios, plus a mixed workload containing `Clear`, `Present`,
`UpdateTexture`, `StretchRect`, and `GetRenderTargetData`. The report must break
counts down by class: chunk commits, coarse resource/surface operations,
frame-token waits, and compatibility per-call fallback.

**R-BENCH-2.4** Hot-path acceptance for DXMT merge readiness is: D3D9 `Set*`,
`Draw*`, and ordinary `Clear` traffic records into committed chunks, with no
per-call PE/unix bridge operation except for documented synchronous readback,
resource creation/destruction, or compatibility fallback paths.

**R-BENCH-2.5** Benchmarks that exercise command chunks must report both the
logical D3D9 operation count and the observed bridge operation count. This is the
data-oriented acceptance signal that batching did not regress to one bridge call
per state change or draw.

**R-BENCH-2.6** The architecture bottleneck report must include counters or
derived metrics for CPU submission time, bridge operations, chunk commits,
payload bytes, allocation or capacity growth after warm-up, uniform interning
hit/miss, encode CPU time, pipeline builds, queue writer/ring back-pressure
waits, synchronous waits, readback throughput, and present acquire/boundary/token
waits.

---

## 3. Reference Stacks

**R-BENCH-3.1** Each workload must be measured against at least two reference
stacks on the same machine:

| Stack | Why |
|---|---|
| wined3d (Wine default D3D9 on macOS) | Baseline — what users have today |
| DXVK + MoltenVK | Best alternative direct translation |

**R-BENCH-3.2** Reference stack measurements are stored in
`benchmarks/baselines/<stack-name>.json` and committed. They are updated
deliberately, not automatically.

**R-BENCH-3.3** The benchmark report must show dxmt9 results alongside
reference stack results as a ratio: `dxmt9 / wined3d` and `dxmt9 / dxvk_mvk`.
A ratio > 1.0 means dxmt9 is faster.

---

## 4. Regression Policy

**R-BENCH-4.1** A dxmt9 performance regression is defined as any workload
metric worsening by more than **5%** relative to the stored dxmt9 baseline.

**R-BENCH-4.2** The dxmt9 baseline (`benchmarks/baselines/dxmt9.json`) must
be updated explicitly when a performance change is intentional. The commit
message must state the reason.

**R-BENCH-4.3** Benchmarks are not part of `meson test` (they are slow and
machine-dependent). They are run on demand via:

```sh
meson compile -C build dxmt9-bench
build/dxmt9-bench --output benchmarks/results/latest.json
scripts/bench_compare.sh benchmarks/baselines/dxmt9.json benchmarks/results/latest.json
```

---

## 5. Baseline Storage Format

**R-BENCH-5.1** Baseline files use JSON:

```json
{
  "stack": "dxmt9",
  "date": "2026-03-29",
  "machine": "Apple M-series (arm64)",
  "results": {
    "draw_throughput_identical":    { "value": 1850000, "unit": "draws/sec" },
    "draw_throughput_state_change": { "value":  920000, "unit": "draws/sec" },
    "pso_cold_first_frame":         { "value":     340, "unit": "ms" },
    "pso_warm_first_frame":         { "value":      12, "unit": "ms" },
    "bridge_ops_per_1000_draws":    { "value":       1, "unit": "ops" },
    "frame_time_simple_p50":        { "value":    2.1,  "unit": "ms" },
    "frame_time_simple_p99":        { "value":    3.8,  "unit": "ms" }
  },
  "counters": {
    "d3d9_ops":            1000,
    "chunk_commits":       1,
    "resource_ops":        0,
    "surface_ops":         0,
    "frame_waits":         0,
    "compat_fallback_ops": 0,
    "total_bridge_ops":    1
  }
}
```

**R-BENCH-5.2** The `machine` field must identify the CPU/GPU class, not a
specific device serial. Baselines are per-architecture class (Apple Silicon
vs Intel Mac), not per individual machine.

**R-BENCH-5.3** Bridge-op metrics must include a companion `counters` object with
the raw class counts used to derive ratios. At minimum: `d3d9_ops`,
`chunk_commits`, `resource_ops`, `surface_ops`, `frame_waits`,
`compat_fallback_ops`, and `total_bridge_ops`.
