---
type: "Spec Requirements"
title: "Benchmarks Requirements"
description: "Benchmarks requirements and compatibility contracts."
tags: [specs, benchmarks, requirements]
---

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
| D9VK + MoltenVK | Alternative D3D9-to-Vulkan-to-Metal translation |

**R-BENCH-3.2** Reference stack measurements are stored in
`benchmarks/baselines/<stack-name>.json` and committed. They are updated
deliberately, not automatically.

**R-BENCH-3.3** The benchmark report must show dxmt9 results alongside
reference stack results as a ratio: `dxmt9 / wined3d` and `dxmt9 / d9vk_mvk`.
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

## 6. Actual PE Setter Economy

**R-BENCH-6.1** The PE setter economy lane must call the public D3D9 COM
interface in a real PE process under the qualified Wine runtime. It is a
separate on-demand lane, not the native provider benchmark. Record executable
architecture and loaded DLL identities; host arm64 execution must not be
reported as arm64 PE execution.

**R-BENCH-6.2** Constant-setter timing must separate register family, range
length and equality shape. Warm storage and prebuilt immutable inputs precede
timing. Every measured call must contribute to HRESULT validation. Exact
getters outside the timed loop must validate the initial/final values and
surrounding registers. Equal inputs use distinct source addresses; changed
inputs alternate so every call changes the intended register subset.

**R-BENCH-6.3** Measure wall time and explicitly qualified CPU scope using
Windows APIs outside the call loop, preserving raw ticks, frequency, call
count and process/thread identity. The bounded lane uses `GetProcessTimes`;
it must not claim thread-exclusive or PE-exclusive CPU. A Wine
`GetThreadTimes` result must not be presumed thread-local without qualifying
the actual runtime implementation. Do not add per-call clocks or renderer observers,
subtract a zero-count control as an assumed overhead, or interpret warm
setter-only timing as draw/flush, whole-frame CPU or FPS.

**R-BENCH-6.4** Qualification must reject missing/duplicate/unexpected cells,
failed state/HRESULT checks, nonpositive or malformed timings and incomplete
termination. Preserve matched image, fixture, environment and output hashes
for repeated comparisons. Local setter economy complements, and must not
replace, rendering refinement, actual GPU readback and wild CPU/FPS gates.

## 7. Actual PE Draw-Parent Economy

**R-BENCH-7.1** The draw-parent lane must exercise public `DrawPrimitive` and
`DrawIndexedPrimitive` COM entry points in a qualified PE process, with a
setter-only control. Record the loaded module, both vtable entry RVAs, PE
architecture and full image identities. Private helper invocation and native
host surrogates must not be reported as public-parent measurements.

**R-BENCH-7.2** Warm, bounded groups must separate timed setters/draws from
setup, seed draw, query issue, GPU drain and readback. Preserve raw QPC ticks,
frequency and exact operation denominators; never automatically subtract the
setter-only or empty-clock control. A separate trace run must verify balanced
group boundaries and absence of recorder flushes inside every timed interval.
Trace timing must not be mixed with observer-disabled timing.

**R-BENCH-7.3** Every call must contribute to HRESULT validation. Outside the
timed interval, independent input snapshots, initial/final getters, VB/IB
bindings and exact pixel readback must qualify the selected fixture, followed
by successful cleanup and final device Release zero. Constant counts 3 and 4
must have exact range/canary checks. FFP pixels must not be presented as proof
of programmable-shader constant consumption.

**R-BENCH-7.4** The strict parser must reject incomplete, duplicate, unexpected
or malformed cells, invalid identity/timing/state receipts and missing final
termination. Report explicit process CPU ticks for the whole row, including
setup, warm-up, drains and oracle work; this is not exclusive PE/thread CPU.
Repeated results require matched fixture, recipe and runtime-image receipts.
Local-parent timing does not establish a full-frame CPU/FPS improvement,
stable run-to-run variance or performance-lane promotion.
