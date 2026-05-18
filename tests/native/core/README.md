# tests/native/core

D3D9 frontend specs — pure value transforms over the dxmt9 frontend layer.
Each spec is a standalone executable wired into Meson via
`tests/native/core/meson.build`.

| Spec | Covers (R-* anchor) |
|------|---------------------|
| `core_device_lifecycle_spec.cpp` | Device init, Reset, fullscreen/windowed transition, ComWrappers/ComWrappersEx parity (`R-CORE-2.*`) |
| `core_device_com_spec.cpp` | IDirect3DDevice9 / IDirect3DDevice9Ex COM wrapper round-trip (`R-CORE-1.*`, `R-CORE-10.*`) |
| `core_device_coverage_spec.cpp` | Raster/sampler border/index-draw policy, cube/programmable texture coverage (`R-CAPS-*`, `R-CORE-3.*`) |
| `core_ffp_state_key_spec.cpp` | `makeFfpVertexKey()` / `makeFfpPixelKey()` determinism + visual-port coverage (`R-CORE-5.*`) |
| `core_format_caps_spec.cpp` | Format conversion / present interval / device caps mapping (`R-FORMAT-*`, `R-CAPS-*`) |
| `core_shader_translator_spec.cpp` | Pixel/vertex shader source-contract: depth output, sampler register, input/output semantic, default no-flip + V-flip + Y-flip variants (`R-CORE-5.*`, `R-BACK-4.*`) |
| `state_draw_transform_spec.cpp` | `makeCanonicalDrawStateFromState()` end-to-end across draw args / viewport / RS/SAMP/TSS / clip planes / shader refs / streams / FVF / RT/DS (`R-CORE-11.*`, `R-ARCH-2.*`) |
| `dod_state_format_spec.cpp` | DOD state record + format pack/unpack POD invariants (`R-ARCH-2.*`) |
| `draw_uniforms_layout_spec.cpp` | Per-frequency uniform struct layout asserts (`R-BACK-12.16-12.18`) |
| `draw_uniforms_dirty_spec.cpp` | DirtyMask + range counter contract (`R-BACK-12.8-12.12`) |
| `chunk_record_micro_spec.cpp` | CPU-only chunk-build microbenchmark (V1 audit B1) |

Shared helpers in `core_spec_fixtures.hpp` (harness, byte builders, recording
backend, D3D9 bytecode constants, token builders).

## Running

```sh
meson test -C build-x86_64-builtin dxmt9-core-device-lifecycle-spec
```

Two `dxmt9-core-shader-translator-spec` variants run under env-var gates
(`DXMT9_CORE_SPEC_SOURCE_CONTRACT_ONLY=1` + `DXMT_DEBUG_FORCE_PIXEL_V_FLIP=1`
or `DXMT_DEBUG_FLIP_VERTEX_Y=1`) to validate orientation contracts.

## Conventions

- Spec files: `<area>_spec.cpp` (snake_case).
- Test target name: `dxmt9-core-<name>` (kebab).
- New specs follow the pattern of existing siblings (include
  `core_spec_fixtures.hpp`, `int main()` runs only this bucket's tests,
  preserve env-var gates if adding source-contract variants).
