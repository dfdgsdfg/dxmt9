---
type: "Spec Gap"
title: "D3D7 Gap"
description: "Implementation and evidence gaps for the D3D7 / DirectDraw shim."
tags: [specs, gap, d3d7]
---

# D3D7 Gap

Domain-owned implementation and evidence gap tracker. Use the [root gap index](../gap.md) for cross-domain rollup.

## D3D7 / DirectDraw 7 Layer (`ddraw.dll`)

No implementation exists yet. All R-D3D7-1.x through R-D3D7-10.x are not started.

| Area | Status | Spec |
|---|---|---|
| `DirectDrawCreate` / `DirectDrawCreateEx` entry points | ❌ | R-D3D7-1.1–1.4 |
| `IDirectDraw7` (SetCooperativeLevel, CreateSurface, etc.) | ❌ | R-D3D7-2.1–2.11 |
| `IDirect3D7` (EnumDevices, CreateDevice, CreateVertexBuffer) | ❌ | R-D3D7-3.1–3.6 |
| `IDirectDrawSurface7` classification (primary / depth / tex / RT / cube / mip) | ❌ | R-D3D7-4.1–4.9 |
| `IDirectDrawSurface7` ops (Lock, Unlock, Blt, BltFast, Flip) | ❌ | R-D3D7-5.1–5.9 |
| `IDirect3DDevice7` draw calls (DrawPrimitive, DrawPrimitiveVB, etc.) | ❌ | R-D3D7-6.1–6.7 |
| `IDirect3DDevice7` state (SetRenderTarget, SetTransform, SetRenderState, etc.) | ❌ | R-D3D7-6.8–6.18 |
| `IDirect3DVertexBuffer7` (Lock, Unlock, GetDesc) | ❌ | R-D3D7-7.1–7.5 |
| Vertex count → primitive count conversion | ❌ | R-D3D7-8.1 |
| `DDPIXELFORMAT` → `D3DFORMAT` parser | ❌ | R-D3D7-9.1 |
| D3D7 render state / transform mapping tables | ❌ | R-D3D7-6.12, R-D3D7-6.11 |
| Strided vertex packing (`DrawPrimitiveStrided`) | ❌ | R-D3D7-6.4–6.5 |
| Mip chain traversal (`GetAttachedSurface`) | ❌ | R-D3D7-4.8 |

---
