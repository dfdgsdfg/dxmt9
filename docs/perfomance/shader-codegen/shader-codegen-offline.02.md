---
domain: shader-codegen
workload: 3DMark05 GT1
subcategory: offline
order: 02
title: Offline Live-VSOut Variant Codegen
date: undated
type: measurement
status: rejected
outdated: retired-journal
source: specs/perfomance.plan.md#L7691-L7754
---

# Offline Live-VSOut Variant Codegen

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** Does the Metal compiler actually see structural VSOut
reductions in the top translated VS rows? If it does, but runtime trims did not
move the counter, then visible VSOut return width cannot be the first-order owner.

**Method.** `scripts/tools/analyze_metal_shader_variants.py --top 3` generates
structural MSL variants from a shader-dump summary, compiles each with Apple's
Metal toolchain, and compares compiler-visible IR via objdump. Offline classifier
only — it cannot replace a runtime Xcode counter A/B. Variants: `original`,
`live-vsout` (fields the paired FS actually reads), `position-only` (lower bound).
Input dump `app-d3d9-3dmark05-current-normal-gputrace-r1`.

**Result.**

| seq/enc | Variant | Kept VSOut fields | VSOut / IR return | IR alloca |
|---|---|---|---:|---:|
| `60/2` | original | all 13 fields | `184B / 184B` | `128B` |
| `60/2` | live-vsout | position, texcoord0, fogFactor | `36B / 36B` | `128B` |
| `60/2` | position-only | position | `16B / 16B` | `0B` |
| `60/1` | original | all 13 fields | `184B / 184B` | `128B` |
| `60/1` | live-vsout | position, texcoord0, fogFactor | `36B / 36B` | `128B` |
| `60/1` | position-only | position | `16B / 16B` | `0B` |
| `60/0` | original | all 13 fields | `184B / 184B` | `128B` |
| `60/0` | live-vsout | position, color, secondaryColor, fogFactor | `52B / 52B` | `0B` |
| `60/0` | position-only | position | `16B / 16B` | `0B` |

- The compiler does see the structural reduction: IR return drops `184B` → `36B`
  or `52B` for the live set, and `position-only` further removes the visible
  `outTexcoord[8]` scratch (`128B` → `0B`).
- Yet prior runtime `DXMT9_TRIM_UNUSED_VARYINGS=1` and `point_size` probes did
  not move the Xcode VS buffer-write bucket.

**Verdict.** Rejected (visible VSOut width as first-order owner). Offline proves
the compiler handles the structural change, but the runtime trim did not move the
bucket — so the surviving owner is below source-visible VSOut return width:
hidden vertex/tiler/parameter storage. The fix must change hidden backend
pressure, not merely shrink MSL-visible return structs.

**Related.** [shader-codegen](index.md) · [shader-codegen-offline.01](shader-codegen-offline.01.md) (prior step) ·
[vsout-layout](../vsout-layout/index.md) (confirms the runtime trim probes it owns were rejected) ·
[hidden-backend-storage](../hidden-backend-storage/index.md)
