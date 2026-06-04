---
domain: hidden-backend-storage
workload: 3DMark05 GT1
subcategory: density
order: 01
title: VS Buffer Write Density
date: undated
type: measurement
status: accepted
source: specs/perfomance.plan.md#L5450-L5520
---

# VS Buffer Write Density

**Question / hypothesis.** Per VS invocation, how many bytes does the hidden
bucket cost, and is that explainable by visible varyings or by Xcode's named
tiled-buffer counters? Tests whether the bucket is a simple stage-out width
problem or vertex-stage memory/spill traffic.

**Method.** Re-read the labeled `frame60` Xcode export
(`current-normal-gputrace-r1`); derive bytes-per-invocation, VS/tiled ratio,
and VS L1/LLC write split per top-three encoder.

**Result.** Top-three encoders write ~`1.627GiB` over ~`1.18M` VS invocations =
~`1448` bytes per VS invocation. Same rows write only ~`29.5MiB` through
`Tiled Vertex Buffer Bytes` + `Tiled Vertex Buffer Primitive Blocks Bytes`, so
the bucket is ~`55x` larger than the named tiled counters.

| Encoder | VS buffer write | B / VS inv | B / primitive | VS/tiled | Varyings/frag |
|---:|---:|---:|---:|---:|---:|
| `60/2` | `981.177MiB` | `1602.6` | `2642.3` | `40.2x` | `9.691` |
| `60/1` | `421.213MiB` | `1151.1` | `1931.0` | `118.2x` | `1.941` |
| `60/0` | `224.974MiB` | `1542.9` | `2424.6` | `150.0x` | `0.000` |
| top-3 | `1627.365MiB` | `1447.9` | `2385.3` | `55.2x` | n/a |

Vertex-stage memory-dominated, not ALU: top-3 VS L1 write `407.154MiB`,
VS LLC write `1651.164MiB`, weighted vertex-stage time `96.13%`, weighted VS
ALU limiter only `2.39%`, weighted VS buffer-write limiter `21.94%`.

**Verdict.** accepted (density confirms hidden owner). `Varyings Per Fragment`
cannot be the only driver: `60/0` writes `224.974MiB` with `0.000` varyings per
fragment. `DXMT9_TRIM_UNUSED_VARYINGS=1` produced no material VS-write drop.
The `~1448 B/invocation` is too large for simple `VSOut` and dwarfs named tiled
counters — treat as broad vertex-stage/tiler scratch or spill-like device
traffic. Low VS ALU limiter (`2.39%`) means ALU reduction is not the next
target unless it also shrinks the vertex-stage memory footprint.

**Related.** [[hidden-backend-storage-attribution.01]] · [[hidden-backend-storage]] ·
[[hidden-backend-storage-model.01]] · [[vsout-layout]] · [[shader-codegen]] ·
[[tvb-mechanism-proof]] · [[overview-3dmark05-gt1]]
