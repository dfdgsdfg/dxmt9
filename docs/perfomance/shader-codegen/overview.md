---
domain: shader-codegen
workload: 3DMark05 GT1
title: "Shader Codegen — translated-VS temp/scratch trim and offline Metal compiler inspection - Current Overview"
type: domain-overview
status: current
updated: 2026-07-29
source: docs/perfomance/shader-codegen/log.md; docs/perfomance/overview-3dmark05-gt1.md; agents/rules/shader_codegen.rules.md
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

> **Every row below cites a leaf now marked `outdated: retired-journal`.** None
> of these rejections can be re-checked today; they are kept because they record
> which codegen-visible shapes were already tried. The open item further down is
> the only live entry in this domain.

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H1 | The conservative `float4 r[32]` translated temp array inflates the VS write bucket | rejected | shader-codegen-temps.01 |
| H2 | The conservative `float4 outTexcoord[8]` output scratch inflates the bucket | rejected | shader-codegen-scratch.01 |
| H3 | Compiler-visible IR (return + scratch) is large enough to own the bucket | rejected | shader-codegen-offline.01 |
| H4 | The Metal compiler cannot see VSOut structural reductions, so source width is the lever | rejected | shader-codegen-offline.02 |

## Open Item — DEF Overlay Constant-Copy Removal (unmeasured)

Commit `d63f7a65` (2026-07-28) removes a codegen shape this domain's earlier
rejections never covered. A single `DEF` used to disqualify the zero-copy
constant-buffer alias, because the DEF literal had to overwrite an entry of a
mutable local array; shaders combining relative addressing with a `DEF`
therefore copied the whole bound category per invocation. `float4 cFloat[256]`
is `4,096 B` of per-thread stack — device memory — that Apple's MSL→AIR pipeline
cannot register-allocate. The fix keeps the read-only pointer alias, hoists the
DEF literals into immutable locals, and selects the literal at each relative
read site with a nested ternary.

Scope, measured before the fix on 3DMark05 GT1 frame60 encoder `60/1`: 8 of 17
vertex variants emit that copy and carry `297,935` of `797,864` LRU64
invocations (`37.3%`); `297,935 × 4,096 B` is `96.4%` of the `1,265,398,976 B`
of VS device-memory writes on that encoder, with Partial Render Count `0`.

Status: **visual gate passed, GPU effect unmeasured.** Baseline-vs-candidate GT1
captures at matching frame ordinals show character models present and correctly
skinned; 81 of 92 GT1 shaders are byte-identical and exactly the 11 register-file
shaders changed. There is no `.gputrace` or encoder-counter export for the
post-fix build. Do not record this as a win until VS device writes, VS
invocations, and GPU time are shown moving together on that encoder. The figures
above are pre-fix attribution, not a result.

This is also the first live entry in this domain since the rejections above; it
does not reopen any of them, because it is not a source-visible `VSOut` or temp
shape — it is a compiler spill caused by an emitted local array, the class
`agents/rules/shader_codegen.rules.md` was written to prevent.

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

> 4 of the 4 leaves listed below are marked `outdated:` and open with a banner naming the ground. They are history, not re-checkable evidence.
