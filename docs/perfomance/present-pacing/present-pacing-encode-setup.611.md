---
domain: present-pacing
workload: STALKER CoP H502/r5 original captured input
title: "H611 — declaration decode and PSO-label cost are bounded secondary owners"
type: experiment
status: bounded-attribution
updated: 2026-10-16
source: traces/stkcop-encode-setup-h611/analysis.json; traces/stkcop-encode-setup-h611/runs.json; traces/stkcop-encode-setup-h611/build.json; traces/stkcop-encode-setup-h611/native-tests.json; traces/stkcop-encode-setup-h611/detail-tests.json
related: docs/perfomance/present-pacing/present-pacing-encode-phases.610.md; docs/perfomance/present-pacing/present-pacing-encode-owner.609.md
---

# H611 — declaration decode and PSO-label cost

The two concrete setup candidates from [H610](present-pacing-encode-phases.610.md)
account for about 7% of observed draw-loop wall on the fixed captured source.
Neither explains the large loop as a whole. No product optimization, live
transfer, FPS improvement or removable CPU budget is established.

## Experiment and qualification

Private H602-default archives retain H609 coarse root/loop scopes. Only two
optional detailed scopes are added to `src/dxmt9/dxmt9_draw_encoder_draw.mm`,
`encodeDraw`: declaration/FVF decode plus fixed-function classification, and
the complete runtime PSO-label block. The latter includes variant hashing,
texture-property queries and formatting/setLabel; it is not just string work.
CPU endpoints remain root-level. Per-operation clocks measure wall, not CPU.

Fixed order O0,D0,D1,O1 uses the same binary and H502/r5 input, detail
OFF/ON/ON/OFF, all roots selected, product counters OFF and fresh processes.
No runs are replaced or appended. Local sequence8 has 526 DrawRuns and 530
subrange loops. Counts below are actual scope entries, not inferred draws.

All four original source/output SHA-256 comparisons, seven-source mapping,
670 created/released wrappers and query transcript controls pass. The common
source/output SHA is
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
Native coverage is 12 ARM64/x86_64 controls, 22 corrupt-trace rejections and
four detail-switch controls. Independent raw decoding conserves all 36 roots.
Build provenance checks exactly three changed archive members, install names,
loaded images and reversible draw-source edits. Product sources and Wine
staging are unchanged. No rendering/lifetime policy transition is introduced.

## Measurements

Milliseconds per execution of captured sequence8, **not live ms/frame**:

| Run | Root CPU interval | Whole-loop wall | Decode wall / calls | PSO-label wall / calls |
|---|---:|---:|---:|---:|
| O0 | 36.693–36.717 | 13.142 | OFF | OFF |
| D0 | 36.786–36.810 | 13.277 | 0.335 / 3125 | 0.634 / 524 |
| D1 | 37.062–37.088 | 13.794 | 0.327 / 3125 | 0.642 / 524 |
| O1 | 37.029–37.054 | 13.461 | OFF | OFF |

The two scopes total 0.969ms in each detailed run, or 7.30% / 7.03% of the
observed loop wall. PSO naming runs on base-state rebind, not on all 3125 draws.
Each child CPU lower bound is zero: root complementary wall uncertainty is
larger than these short scopes. Their wall values are conservative measured
CPU upper bounds, not CPU point estimates or guaranteed removable amounts.

Detailed-versus-coarse finite contrasts span −0.184 to +0.652ms for loop wall
and −0.268 to +0.395ms for root CPU. Unlike H610's many-boundary observer,
this population does not separate detail overhead from run variation. This
does not prove zero observer cost. Do not subtract a universal tax, compare
H610/H611 absolute times as paired policy results, or scale by live draw counts.

## Dependency audit and decision

`src/dxmt9/dxmt9_ffp_shaders.cpp`, `decodeFixedFunctionVertexLayout`, consumes
declaration elements (or FVF when elements are absent) and effective stream0
stride. `applyDrawBindingOverride` changes that stride in the shader-layout
copy. Reuse by VS identity alone is unsound. Classification separately reads
the shader-context kind. Buffer content/offset is not a decoder dependency.

`shaderVariantHashForDraw` combines VS/PS hashes, declaration/render-state
hashes, texture/render-target masks, vertex/pixel constant hashes and resolved
texture properties, plus the fragmentless probe variant. PSO identity alone
does not qualify the label. Preserve existing capture labels; this experiment
does not justify deleting diagnostic semantics or adding a stateful cache.

Deprioritize these two operations as the explanation for the large CPU gap.
Next split H610's index/issue region into actual recorded Metal draw emission
and surrounding index/volatile/diagnostic preparation. The current region
contains both; calling all of it driver cost is unsupported. Retain coarse
controls and original input oracles. H487 scratch, H497 raster lookup and H499
VS-shadow failed gates remain closed. The global default-tail largest owner
and a qualified removable budget remain open.
