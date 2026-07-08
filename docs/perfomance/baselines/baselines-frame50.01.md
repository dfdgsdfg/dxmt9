---
domain: baselines
workload: 3DMark05 GT1
subcategory: frame50
order: 01
title: Current Normal Frame50 Gputrace/Xcode Replay (canonical baseline)
date: 2026-06-04
type: measurement
status: accepted
source: specs/perfomance.plan.md#L1916-L2015
---

# Current Normal Frame50 Gputrace/Xcode Replay (canonical baseline)

**Question / hypothesis.** Establish the canonical current-source frame50
GPU/counter baseline that the frame50 A/B locality experiments are measured
against.

**Method.**
```bash
scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix current-normal-frame50-gputrace-r1 \
  --frame 50 \
  --timeout 420
```
Process terminated after the frame capture was written, so the summary is
`partial-log` (no `result.json`). Xcode replay completed, performance data
embedded into `analysis/frame50-performance.gputrace`, Counters opened,
draw-counter profiling finished, `frame50-counters-xcode.csv` exported, then
the finalizer produced `frame50-xcode-dxmt-joined-summary.csv` and
`frame50-xcode-dxmt-bottleneck-report.md`.

**Result.** Xcode Summary: 4 command buffers, 10 render encoders, 396 draws,
`2,146,296` vertices, `35.024ms` effective GPU time.
- Total GPU `35.024ms`; top 3 `34.390ms` / `98.19%`.
- Top-3 buffer write `1628.055MiB`; top-3 VS buffer write `1627.372MiB`;
  top-3 device write `1676.020MiB`.
- Top-3 VS bytes/invocation `1447.9B`; expected visible VSOut `184B`/vertex.
- VS buffer / expected VSOut `7.9x`; VS buffer / stream0 input `33.1x`.
- Named tiled buffer total `29.312MiB`; **hidden backend write estimate
  `1597.615MiB`** (`0.982x` of VS buffer write = `98.2%`).
- dxmt CPU writer bytes `0.444MiB`; transient vertex/index `0.000MiB`.
- Hot rows: `50/2` `19.919ms` `56.87%` (187 draws, 642,001 VS inv, `981.206MiB` VS write);
  `50/1` `8.594ms` `24.54%` (156 draws, 383,688 VS inv, `421.204MiB`);
  `50/0` `5.877ms` `16.78%` (42 draws, 152,895 VS inv, `224.962MiB`).
- Partial-log run-level: `present_encoded=1380`, `draw_calls=1013783`,
  `render_pass_begin=16246`, `render_pass_tile_preservation_bytes=175298519040`,
  `render_pass_same_key_reentry_preservation_bytes=70388809728`,
  `encode_draw_cpu_ms=20886.483` (~20.9s), `submit_draw_cpu_ms=4278.712`,
  `transient_upload_bytes=1169880116`, `argbuf_hybrid_bytes_per_encoder` ~`1.17GB`,
  `queue_sequence_wait_ms=0`, `map_buffer_wait_ms=0`.

(Task brief's "encode 17.3s" matches the older run-level
[baselines-runlevel.01](baselines-runlevel.01.md) `encode_draw_cpu_ms=17342.358`; this partial-log
capture's own encode figure is `20886.483ms`.)

**Verdict.** Accepted as **THE frame50 A/B baseline**. Same top-3-encoder /
hidden-VS-write shape as frame120 and frame60: top-3 ≈ total buffer write,
dxmt CPU writers explain essentially none of it, hidden backend estimate is
`98.2%` of the VS write. No matching shaders dumped (`0` matched VS/PS) so it is
a GPU/counter baseline, not a shader-liveness proof.

**Related.** [baselines](../baselines.md) · [overview-3dmark05-gt1](../overview-3dmark05-gt1.md) · [baselines-frame120.01](baselines-frame120.01.md) ·
[baselines-runlevel.01](baselines-runlevel.01.md) · [baselines-frame50.02](baselines-frame50.02.md) (sanity refresh) ·
[hidden-backend-storage](../hidden-backend-storage.md) (hidden estimate `1597.6MiB`) ·
[index-cache-locality](../index-cache-locality.md) (opaque/screen-blend frame50 proofs use this baseline) ·
[vsout-layout](../vsout-layout.md) (rejects `184B` visible width as owner).
