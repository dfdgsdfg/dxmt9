---
type: "Spec Gap"
title: "D3D8 Gap"
description: "Implementation and evidence gaps for the D3D8 shim."
tags: [specs, gap, d3d8]
---

# D3D8 Gap

Domain-owned implementation and evidence gap tracker. Use the [root gap index](../gap.md) for cross-domain rollup.

## D3D8 Layer (`d3d8.dll`)

No implementation exists yet. All R-D3D8-1.x through R-D3D8-11.x are not started.

| Area | Status | Spec |
|---|---|---|
| `Direct3DCreate8` entry point + `ValidatePixelShader` stub | ❌ | R-D3D8-1.1, R-D3D8-1.2 |
| `IDirect3D8` factory (delegate to D3D9) | ❌ | R-D3D8-2.1–2.3 |
| Vertex shader handle table + `CreateVertexShader` | ❌ | R-D3D8-3.1–3.6 |
| Pixel shader handle table + `CreatePixelShader` | ❌ | R-D3D8-3.7–3.10 |
| D3D8 vertex declaration parser (`D3DVSD_*` → `D3DVERTEXELEMENT9`) | ❌ | R-D3D8-4.1–4.6 |
| State block token table | ❌ | R-D3D8-5.1–5.6 |
| `SetRenderTarget(RT, DS)` split | ❌ | R-D3D8-6.1–6.3 |
| `CreateRenderTarget` / `CreateDepthStencilSurface` signature fix | ❌ | R-D3D8-7.1–7.2 |
| `CreateImageSurface` → `CreateOffscreenPlainSurface` | ❌ | R-D3D8-7.3 |
| `CopyRects` → loop of `UpdateSurface` | ❌ | R-D3D8-7.4 |
| TSS → sampler state remapping (10 states) | ❌ | R-D3D8-8.1–8.2 |
| `Present` / `GetBackBuffer` signature fix | ❌ | R-D3D8-9.1–9.2 |
| Capabilities clamping to D3D8 limits | ❌ | R-D3D8-10.1–10.2 |
| Pass-through methods + resource wrapper unwrapping | ❌ | R-D3D8-11.1 |

---
