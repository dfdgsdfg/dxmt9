# dxmt9

Experimental Wine / D3D9-to-Metal translation layer.

This repository is being initialized from the same general problem space as
[DXMT](https://github.com/3Shain/dxmt): a native Metal backend for legacy
Direct3D applications running through Wine. [d3d9-webgl](https://github.com/LostMyCode/d3d9-webgl)
is the reference for D3D9 API coverage and fixed-function emulation patterns;
DXMT is the reference for repo structure and build workflow.

Status: bootstrap only. The tree currently contains a tiny buildable core plus
the folder layout for the eventual translation layer.

## Current layout

- `include/dxmt9/` - public headers
- `src/` - core implementation
- `tests/` - smoke tests
- `docs/` - design notes and roadmap

## Build

```bash
meson setup build
meson compile -C build
meson test -C build
```

## Next work

- Map the D3D9 surface area we need first.
- Define the resource and state model for Wine-facing COM objects.
- Add a Metal backend and command translation layer.
- Fill out the Wine DLL entry points once the core API shape settles.
