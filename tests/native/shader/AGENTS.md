# tests/native/shader

Pure shader translator transform specs — D3DBC bytecode parsing /
classification / source-contract over the lower decoder API.

| Spec | Covers (R-* anchor) |
|------|---------------------|
| `shader_transform_spec.cpp` | D3DBC decode/classification fixtures (stage/version, opcodes, registers, swizzles/masks/modifiers, predicates, samplers, IF/ELSE/LOOP/REP/CALL flow, MAD/DP/CMP/SLT/SGE/POW/SINCOS/LOG/EXP/matrix/TEXLDD/TEXLDL source-contract matrix, deterministic unsupported relative-addressing errors) (`R-BACK-4.1-4.5`, `R-CORE-5.*`) |

## Running

```sh
meson test -C build-x86_64-builtin dxmt9-shader-transform-spec
```

## Conventions

- Single-spec sub-directory. Reserved for additions that operate on the
  shader-translator layer in isolation; broader IR/Metal-source asserts
  live in `tests/native/core/core_shader_translator_spec.cpp` (which
  needs `dxmt9_frontend_dep` and exercises higher-level translation),
  while this dir is the decoder-only baseline.
- Spec file: `<area>_spec.cpp` (snake_case).
- Test target name: `dxmt9-shader-transform-spec`.
