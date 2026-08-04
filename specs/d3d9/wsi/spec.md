---
type: "Spec"
title: "Window System Integration Spec"
description: "D3D9 / WSI spec, ownership, ordering, and evidence mapping."
tags: [specs, d3d9, wsi, spec]
---

# Window System Integration Spec

WSI maps a Win32 window handle (`HWND`) to a Wine-owned Metal presentation
surface and keeps the D3D9 swap chain synchronized with resize, reset, and
destruction. The Wine surface protocol is owned by
[`specs/winemetal/requirements.md`](../../winemetal/requirements.md); this
document owns the D3D9-facing integration.

## 1. Primary `HWND` to Layer Flow

The primary path uses the `winemac.drv` `ExtEscape` proposal tracked by
[Wine MR !11058](https://gitlab.winehq.org/wine/wine/-/merge_requests/11058)
and [upstream DXMT PR #166](https://github.com/3Shain/dxmt/pull/166). Its
current declaration is:

```c
#define MACDRV_ESCAPE_GET_SURFACE     6790
#define MACDRV_ESCAPE_RELEASE_SURFACE 6791

struct macdrv_escape_surface {
  uint64_t surface;
  uint64_t layer;
};
```

These values are revision-pinned compatibility declarations, not an accepted
Wine ABI until the Wine change is merged. The canonical declaration must live
in the compatibility header required by `R-WMB-13.1`; this excerpt is
explanatory.

```mermaid
sequenceDiagram
    participant D9 as d3d9.dll (PE)
    participant Mac as winemac.drv
    participant Bridge as winemetal_dxmt9.dll
    participant Provider as winemetal_dxmt9.so
    participant Presenter as CAMetalLayer presenter

    D9->>Mac: GetDC(HWND)
    D9->>Mac: ExtEscape(QUERYESCSUPPORT, GET_SURFACE)
    Mac-->>D9: supported
    D9->>Mac: ExtEscape(GET_SURFACE)
    Mac-->>D9: {surface, layer}
    D9->>Bridge: adopt_wsi_surface(opaque POD values)
    Bridge->>Provider: wine_unix_call(adopt)
    Provider->>Presenter: borrow CAMetalLayer
    Presenter-->>D9: presentation handle or failure
    D9->>Mac: ReleaseDC(HWND, HDC)
```

PE code owns GDI calls and treats both returned values as opaque `uint64_t`
tokens. The cold adoption call is separate from recorded command chunks. Only
the unix provider converts the nonzero `layer` value to a borrowed
`CAMetalLayer` reference.

If `QUERYESCSUPPORT` fails, dxmt9 may use the old `macdrv_functions` or direct-
symbol path only for an exact Wine build declared and proven as
`legacy-macdrv-symbols`. There is no generic `dlsym` fallback for current Wine
11.x builds.

## 2. Swap-Chain Lifecycle

### 2.1 Creation

At `CreateDevice()` or `CreateAdditionalSwapChain()`:

1. Validate and normalize `D3DPRESENT_PARAMETERS`.
2. Acquire the Wine client surface and layer for the selected `HWND`.
3. Pass the opaque layer token through the cold PE/unix adoption call.
4. Configure the borrowed layer and create the dxmt9-owned back buffer.
5. Store the Wine surface token in PE and an opaque presenter handle in the
   swap chain.

Failure to acquire or adopt the layer fails a windowed presenting swap chain
with `D3DERR_NOTAVAILABLE`. It must not return a device that silently presents
to no window.

### 2.2 Reset and Rebind

Reset acquires and validates the replacement surface first. The unix provider
then stops old drawable acquisition, drains or fences old layer users, and
atomically adopts the replacement. PE releases the old Wine surface only after
successful adoption. A failed candidate is released and the old valid binding
is preserved when D3D9 reset semantics allow rollback.

### 2.3 Destruction

On swap-chain or device destruction:

1. Stop drawable acquisition.
2. Drain or fence every command buffer that can reference the layer.
3. Destroy the unix presenter binding and return success to PE.
4. Issue `MACDRV_ESCAPE_RELEASE_SURFACE` exactly once with the retained surface
   token, then clear the PE token even if Wine reports a release failure.

Wine owns the client surface and `CAMetalLayer`. dxmt9 borrows the layer and
must never independently release it.

## 3. Presentation and Resize

Windowed mode sets `drawableSize` from the normalized back-buffer dimensions.
`displaySyncEnabled` follows `PresentationInterval`. Host-view resize may update
the drawable size without making a windowed device lost; reset still recreates
the back-buffer texture after old GPU use is quiescent.

The back buffer and drawable are distinct Metal objects. The back buffer is a
dxmt9-owned `MTLTexture`; `nextDrawable` is acquired only during present, and
the back buffer is copied or converted into the drawable. Consequently,
`DXMT9_LAYER_FRAMEBUFFER_ONLY` changes drawable policy but not D3D9 back-buffer
lock or readback behavior.

Fullscreen is outside the initial target and may fail cleanly with
`D3DERR_NOTAVAILABLE`. A future exclusive-fullscreen path requires an explicit
display-mode and surface-rebind contract.

## 4. PE / Unix Ownership

| Binary | Kind | WSI responsibility |
|---|---|---|
| `d3d9.dll` | PE DLL | D3D9 COM surface, `HWND`/HDC and `ExtEscape`, Wine surface-token lifetime |
| `winemetal_dxmt9.dll` | PE bridge | Validate and dispatch cold WSI adoption plus the generated dxmt9 ABI |
| `winemetal_dxmt9.so` | Wine unixlib | Borrow `CAMetalLayer`, own presenter state, drawable acquisition, Metal execution |

Recorded draw/state/resource packets remain pointer-free. The two Wine-issued
64-bit values cross only through the dedicated cold WSI bootstrap/rebind call
defined by `R-CORE-WSI-4.1` and never enter `CommandChunk`.

The suffix-qualified bridge names are required for upstream DXMT coexistence.
Upstream DXMT may continue to own `winemetal.dll` / `winemetal.so`; dxmt9 must
load only `winemetal_dxmt9.dll` / `winemetal_dxmt9.so` and verify its own ABI
handshake.

## 5. Deployment and Qualification

The runtime layout is:

- `d3d9.dll` in the selected Wine Windows or application-local directory;
- `winemetal_dxmt9.dll` in the matching Wine Windows or application-local
  directory;
- `winemetal_dxmt9.so` in the Wine unixlib or application-local directory.

The filename split does not make an unmodified Wine runtime compatible. The
primary WSI path additionally requires a Wine revision that implements the
accepted surface escape protocol. Until then, current Gcenx/Heroic Wine 11.x
without the Wine change is unsupported, while an exact Wine build qualified as
`legacy-macdrv-symbols` must retain the existing `macdrv_functions` fallback.

## 6. Evidence

Qualification must record:

- the exact Wine root and surface-protocol declaration;
- observed acquisition path (`extescape-v1`, `legacy-macdrv-symbols`, or
  `unavailable`);
- x64 and WoW64 creation, present, reset, and balanced destruction;
- loaded paths and ABI handshake for both suffix-qualified dxmt9 modules;
- a negative unsupported-Wine run that fails without a crash or black window;
- an upstream DXMT coexistence run proving independent D3D9 and D3D11 WSI.
