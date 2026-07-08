---
domain: shader-codegen
workload: 3DMark05 GT1
subcategory: offline
order: 01
title: Offline Metal Codegen Baseline
date: undated
type: measurement
status: rejected
source: specs/perfomance.plan.md#L7596-L7689
---

# Offline Metal Codegen Baseline

**Question / hypothesis.** Independently of runtime A/B, what compiler-visible IR
shape do the top translated vertex shaders actually have? If the Apple compiler
already removes the source-visible temp array, source-visible width cannot own
the Xcode VS buffer-write bucket.

**Method.** Redump the top force-visible shader pairs without keeping a raw
gputrace: `run_3dmark05_perf_probe.sh --suffix offline-codegen-shaders-r1
--frame 60 --encoder-breakdown-seq 60 --timeout 90 --no-gputrace --dump-shaders`.
Match dumped MSL to force-visible Xcode/dxmt joined rows with
`analyze_shader_dumps.py --require-matches`, then compile the top MSL with
Apple's Metal toolchain (`xcrun metal` + metallib, no `.air`/`.metallib` left
behind) via `scripts/tools/analyze_metal_shader_codegen.py --top 3`.

**Result.** Compiler-visible IR shape is stable across the top three VS rows:

| Rank | Seq/enc | Xcode VS write | Xcode B/inv | IR return | IR scratch | Xcode/IR return | Xcode/IR scratch |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | `60/2` | `981.202MiB` | `1602.6B` | `184B` | `128B` | `8.71x` | `12.52x` |
| 2 | `60/1` | `421.188MiB` | `1151.1B` | `184B` | `128B` | `6.26x` | `8.99x` |
| 3 | `60/0` | `224.963MiB` | `1542.8B` | `184B` | `128B` | `8.38x` | `12.05x` |

- The IR has only one `128B` local scratch (matching `outTexcoord[8]`) while the
  MSL source still declares `float4 r[32]` — the compiler already DCE'd the
  large temp array, explaining why `DXMT9_TRIM_VERTEX_TEMPS=1` changed source
  shape without moving the counter.
- The compiler-visible VS return aggregate is `184B`, still `6.3x`–`8.7x`
  smaller than Xcode's per-invocation VS write.

**Verdict.** Rejected (source-visible shape as owner). The Apple compiler
already removes `r[32]`; visible scratch is only `128B`; the surviving Xcode
bucket is `6.3x`–`8.7x` larger than the IR stage return. Owner is hidden Apple
vertex/tiler/parameter/primitive backend storage below the MSL/AIR-visible shape.

**Related.** [shader-codegen](index.md) · [shader-codegen-temps.01](shader-codegen-temps.01.md) · [shader-codegen-scratch.01](shader-codegen-scratch.01.md) ·
[shader-codegen-offline.02](shader-codegen-offline.02.md) (next: structural VSOut variants) ·
[hidden-backend-storage](../hidden-backend-storage/index.md) · [vsout-layout](../vsout-layout/index.md)
