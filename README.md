# dxmt9

Wine / D3D9-to-Metal translation layer for macOS.

Translates Direct3D 9 API calls from applications running under Wine directly
into Metal — no Vulkan middle layer. Targets plain `IDirect3DDevice9`;
`IDirect3DDevice9Ex` is next.

Related projects: [DXMT](https://github.com/3Shain/dxmt) (D3D11/D3D12 Metal),
[d3d9-webgl](https://github.com/LostMyCode/d3d9-webgl) (D3D9 reference).

---

## Prerequisites

**macOS with Xcode Command Line Tools:**

```sh
xcode-select --install
```

**Homebrew packages** (see `Brewfile`):

```sh
brew bundle
```

This installs:

| Package | Purpose |
|---|---|
| `meson` + `ninja` | Build system |
| `llvm` | C++20 / ObjC++ compiler |
| `tla+-toolbox` (cask) | TLC model checker for formal verification |

---

## Build

```sh
meson setup build
meson compile -C build
meson test -C build
```

To use the Homebrew LLVM toolchain explicitly:

```sh
CC=$(brew --prefix llvm)/bin/clang \
CXX=$(brew --prefix llvm)/bin/clang++ \
meson setup build
```

---

## Verify TLA+ specs

The concurrent subsystems (command queue, resource lifetime, encoder lifecycle,
query resolution) are formally verified with TLC:

```sh
TLA2TOOLS_JAR="/Applications/TLA+ Toolbox.app/Contents/Eclipse/tla2tools.jar" \
  bash scripts/verify_tla.sh
```

Expected output: `Model checking completed. No error has been found.` for all
four modules. The same check runs as part of `meson test`.

---

## Repository layout

```
include/dxmt9/   Public headers (core.hpp, assert.hpp, winemetal.h)
src/             Implementation (core, backend sim + Metal, Wine bridge)
tests/           Smoke tests and core spec tests
specs/           Specifications and formal verification
  core/          D3D9 COM requirements and design
  backend/       Metal translation requirements and design
  verification/  TLA+ specs (CommandQueue, ResourceLifetime,
                 EncoderLifecycle, QuerySeqId) + .cfg model files
  gap.md         Spec–implementation gap tracker
scripts/         verify_tla.sh
```

---

## Status

| Layer | Status |
|---|---|
| Core (D3D9 COM surface, device state, draw calls) | Complete |
| Metal backend (command queue, PSO cache, FFP shaders, D3DBC translation) | Complete |
| Formal verification (TLC, all 4 specs) | Complete |
| `IDirect3DDevice9Ex` | Next |
