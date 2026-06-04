# Mini-Replay Bisection — the apparatus that isolated per-draw vertex-stage write amplification

> Part of the 3DMark05 GT1 GPU-bottleneck investigation. Root map: [[overview]].

## Scope & question

This domain owns the **methodology**: build a standalone, row-local mini-replay
harness that consumes real captured index/stream/cbuf/depth payloads plus dumped
shaders, reproduce the original hot-encoder `VS Buffer Device Memory Bytes Written`
pressure at encoder scale outside Wine/D3D9, then bisect it down to individual
shader-pair / draw windows. It answers: *which concrete inputs reproduce the GT1
hidden vertex-stage write bucket, and at what granularity is it owned?* This is
the apparatus that made [[tvb-mechanism-proof]] possible and that backs the
[[index-cache-locality]] win.

## Hypotheses & verdicts

| # | Hypothesis | Verdict | Evidence |
|---|-----------|---------|----------|
| H1 | Hot-row attribution / shaders / draw identity are ready; only geometry bytes are missing | tooling (gap found) | [[mini-replay-bisection-harness.01]] |
| H2 | Runtime can dump replayable index/stream/cbuf bytes for a hot window without mutating state | tooling | [[mini-replay-bisection-payload.01]] |
| H3 | Runner can replay a multi-PSO slice with stream1 + dynamic cbuf slots | tooling | [[mini-replay-bisection-harness.02]] |
| H4 | Captured payloads alone reproduce the original ~1 GiB VS-write scale/shape | inconclusive (class yes, scale no) | [[mini-replay-bisection-replay.01]] |
| H5 | Fragment overdraw (missing per-draw scissor) is the gap | rejected (real fix, but VS write unmoved) | [[mini-replay-bisection-replay.02]] |
| H6 | Depth attachment content (clear scalar or real D24X8) owns the amplification | rejected | [[mini-replay-bisection-depth.01]] |
| H7 | The wider encoder2 (113-draw) sequence reproduces vertex-stage dominance | accepted | [[mini-replay-bisection-replay.03]] |
| H8 | The pressure is a single late draw/state transition | rejected (additive, independent windows) | [[mini-replay-bisection-bisect.01]] |
| H9 | The cost is per-draw geometry/shader-pair amplification, not alpha/scissor | accepted | [[mini-replay-bisection-pair.01]] |

## Verification methods

- **`plan_3dmark05_mini_replay.py`** — joins joined-summary + shader-dump +
  indexed probe CSVs into a readiness table; `--geometry-dir` validates payload
  triplets. Proves what inputs exist before a replay.
- **`--dump-indexed-geometry` / `--dump-indexed-geometry-cbufs`** (probe wrapper)
  — implies `DXMT9_MEASURE_INDEX_REUSE=1`, does not mutate primitive order,
  writes `.index.bin` / `.stream0.bin` / `.streamN.bin` / `.vsconsts.bin` /
  `.psconsts.bin` / `.ffpvs.bin` / `.ffpps.bin` + `.meta` under
  `analysis/geometry/`. Shader filters `--dump-indexed-geometry-vs/-ps` select a
  shader pair (row-local draw windows are not robust cross-run selectors).
- **`DXMT9_DUMP_DEPTH_ATTACHMENT_HANDLE` / `_PATH`** (wrapper
  `--dump-depth-attachment-handle/-seq/-enc/-path`) — blits a live GPU-side
  depth/stencil target to a readback buffer with `.json` metadata, capturing
  D24X8 that the old BMP `DXMT_DUMP_GPU_TEXTURE_*` upload hook cannot.
- **`run_3dmark05_mini_replay.py`** — rewrites dumped MSL off `buffer(30)` argbuf
  into standalone cbuf slots, scans bindings to pick free slots, compiles an
  Obj-C++ Metal app, binds real payloads, multi-PSO, per-draw scissor,
  `--depth-clear`, `--depth-input`, `--primitive-order`, `--draw-order`,
  `--color-output` PPM readback, `--capture-path`.
- **`--encoder-draw-indices`** (manifest builder) — captures non-contiguous
  encoder-local draws (e.g. a shader pair `[14,15,18,19,21]`) without widening the
  geometry gate.
- **Software LRU cache estimate vs Xcode counters** — the runner's LRU16/32/64
  miss estimate is cross-checked against Xcode `VS Invocations` /
  `VS Buffer Device Memory Bytes Written` so reorder candidates are accepted only
  when software misses and Xcode counters drop together.

## Experiment dependency graph

```mermaid
flowchart TD
  Plan["readiness-planner\nhot-row ready, geometry bytes MISSING"] --> Pay["payload-scout\n--dump-indexed-geometry(+cbufs)\n60/2 win 71..188, 16 triplets"]
  Pay --> Multi["multi-PSO-harness\nstream1 + dynamic cbuf slots\n6-draw smoke OK"]
  Multi --> R16["16-draw-replay\nVS write 31.974MiB (class, not scale)\nfragment-dominated"]
  R16 --> Scissor["scissor-fix\nGPU 3.71->1.50ms, FS 22.1M->3.0M\nVS write unchanged 31.98MiB"]
  Scissor --> Depth["depth-probes\ndepth=0 + raw D24X8\nVS write ~31.98MiB"]
  Depth --> R113["wider-encoder2 (113-draw)\nGPU 18.115ms, VS write 1090.901MiB\n1710B/VS inv"]
  R113 --> Bisect["bisection\n0..13 cold, 14..27 hot 4026B/inv\nadditive independent sources"]
  Bisect --> Pair["pair-local\nfea7/a091 + dee2/2f20\n99.8% with 7/14 draws"]
  Pair --> TVB["feeds [[tvb-mechanism-proof]]"]

  classDef accepted fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef rejected fill:#f8d7da,stroke:#a33,color:#600
  classDef open fill:#fff3cd,stroke:#a80,color:#640
  classDef tooling fill:#e8f0ff,stroke:#476cb6,color:#0d1833

  class Plan,Pay,Multi tooling
  class R16 open
  class Scissor,Depth rejected
  class R113,Bisect,Pair,TVB accepted
```

## Results synthesis

Settled: a standalone mini-replay reproduces the **class** of GT1 VS-write traffic
from captured payloads, but the **scale and shape** require the wider encoder2
sequence. The 3-draw / 16-draw slices stayed at ~32 MiB and fragment-dominated;
fixing per-draw scissor and supplying real D24X8 depth corrected fragment work but
left VS buffer write fixed at ~31.98 MiB — cleanly **ruling out fragment overdraw
and depth content as artifacts/owners**. The full 113-draw encoder2 replay finally
matched the hot row (18.115 ms, 1090.901 MiB, 1710 B/VS inv vs original 20.327 ms,
981.171 MiB, 1602.5 B/VS inv). Bisection showed the pressure is **additive across
independent draw windows** (not a single transition), and pair-local captures
localized 99.8% of the first hot window to two shader pairs / large indexed draws
with per-draw additivity within 0.44% — i.e. **per-draw vertex-stage write
amplification keyed by geometry × shader pair**, with named tiled counters far
below the VS-write bucket. An actual-read VSOut liveness trim moved VS write only
-0.01%, so visible varying width is not the owner ([[vsout-layout]]).

This is the apparatus that **isolated the per-draw vertex-stage write
amplification** and so enabled the [[tvb-mechanism-proof]] (geometry-locked
original-vs-`cache-opt-lru32` replays) and the accepted
[[index-cache-locality]] production win. **Firmware-TVB caveat:** small standalone
replays can legitimately report `0 MiB` for `VS Buffer Device Memory Bytes Written`
because the firmware Parameter Buffer never spills at that scale (the `0..13`
prefix is the local example) — this is why scale, not just class, had to be
reproduced before the mechanism could be proven; see [[tvb-mechanism-proof]].

## Cross-references

- [[tvb-mechanism-proof]] — the accepted mechanism this apparatus enabled; PB-spill / 0 MiB caveat.
- [[index-cache-locality]] — the production win validated by geometry-locked mini-replays.
- [[hidden-backend-storage]] — the TVB model the replays measure (named tiled << VS write).
- [[primitive-reorder-diagnostics]] — sort-min-index / cache-opt order knobs tested under geometry-locked replay.
- [[vsout-layout]] — pair-liveness VSOut trim rejected (-0.01%) inside the hot window.
- [[render-pass-store]] — depth re-entry / attachment content rejected as owner.
- [[overview]] — root map and priority DAG.
