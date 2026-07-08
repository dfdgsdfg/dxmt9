---
domain: shader-codegen
workload: 3DMark05 GT1
title: "Shader Codegen — translated-VS temp/scratch trim and offline Metal compiler inspection - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/shader-codegen/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/shader-codegen/index.md; docs/perfomance/shader-codegen/log.md
---

# Shader Codegen — translated-VS temp/scratch trim and offline Metal compiler inspection - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `shader-codegen.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain attacks the hidden `VS Buffer Device Memory Bytes Written` bucket
from the **translated-shader codegen** angle: does the conservative shape of
dxmt9's translated vertex shaders (the 32-slot `float4 r[]` temp array, the
8-slot `outTexcoord[]` output scratch, the wide `VSOut` return struct) inflate
the bucket? It pairs runtime A/B trims with offline Apple Metal compiler/IR
inspection (`xcrun metal`, metallib, objdump) to separate source-visible MSL
shape from what the compiler and backend actually emit. The unanimous result:
codegen-visible shape is **not** the owner — the bucket lives below the
AIR-visible shape.

## Latest Conclusions

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H1 | The conservative `float4 r[32]` translated temp array inflates the VS write bucket | rejected | [shader-codegen-temps.01](shader-codegen-temps.01.md) |
| H2 | The conservative `float4 outTexcoord[8]` output scratch inflates the bucket | rejected | [shader-codegen-scratch.01](shader-codegen-scratch.01.md) |
| H3 | Compiler-visible IR (return + scratch) is large enough to own the bucket | rejected | [shader-codegen-offline.01](shader-codegen-offline.01.md) |
| H4 | The Metal compiler cannot see VSOut structural reductions, so source width is the lever | rejected | [shader-codegen-offline.02](shader-codegen-offline.02.md) |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [shader-codegen-offline.02 - Offline Live-VSOut Variant Codegen](shader-codegen-offline.02.md)
- [shader-codegen-temps.01 - Vertex Temp Array Trim Probe](shader-codegen-temps.01.md)
- [shader-codegen-scratch.01 - VS Output Scratch Array Trim Probe](shader-codegen-scratch.01.md)
- [shader-codegen-offline.01 - Offline Metal Codegen Baseline](shader-codegen-offline.01.md)
