# Shader Codegen — Generated MSL Is a Hot Path

Rules for the D3D9→MSL translator (`src/dxmt9/dxmt9_shader_metal_ir.cpp`,
`dxmt9_ffp_shaders.cpp`, `dxmt9_shader_sources.cpp`). The repository's DOD
conventions cover C++ hot paths; this file extends the same discipline to the
**emitted shader source**, whose costs live in the Metal compiler and GPU
counters, not in any CPU profile.

## Rule: no large or dynamically-sized local arrays in emitted shaders

Problem (H226, 2026-07-13): translated shaders materialized the whole D3D9
constant register file per invocation. Discovered on SFIV; fixed by `7abaa20e`.

```metal
// Before: correct-for-every-case abstract-machine emulation.
float4 cFloat[137];
for (uint i = 0; i < 137u; ++i) { cFloat[i] = psConsts.psFloatConst[i]; }
cFloat[0] = float4(0.0f, 0.0f, 0.0f, 1.0f);  // def c0 overlay

// After: usage-analyzed in-place reads + hoisted DEF literals.
const float4 dxmt9_cdef0 = float4(0.0f, 0.0f, 0.0f, 1.0f);
... psConsts.psFloatConst[136] ...   // non-DEF registers read in place
```

A `float4 arr[N]` local beyond the register budget is silently spilled to
stack (= device memory) by the Metal compiler: SFIV paid 5.6GB/frame of
fragment buffer writes, 90% integer ALU (copy-loop addressing), and
Sync-Wait-Memory-dominated profiles — three fullscreen quads at ~30ms each,
while the source diff looked like one harmless `for` line. Result of the fix:
SFIV CB GPU p50 `73 -> 2.4ms`, presents `1,740 -> 5,100`.

**Rules:**
- Emit per-use in-place reads from `constant` buffers instead of copying
  register files; hoist compile-time immediates (DEF/DEFI/DEFB) to immutable
  scalars/vector locals. Materialize an array only when the shader provably
  needs it (relative addressing over a def-overlaid range, runtime writes to
  the constant file), and regression-pin that fallback with a source-contract
  test.
- Any translator change that introduces a local array sized by a *shader
  property* (max register index, declared temps, output slots) must justify
  the worst-case size against spill risk. The pure max-index and output-usage
  analyses remain available, but translated VS `r[]` and `outTexcoord[]` keep
  their conservative default sizes after the rejected runtime trims were
  retired — do not add new members of this class.
- Source-contract tests prove *shape*, not *cost*. For emission-pattern
  changes, pair them with a GPU-counter gate (Xcode encoder counters or
  xctrace `metal-gpu-intervals`): watch stage "Buffer Bytes Written",
  integer-ALU share, and Sync Wait Memory. A fragment stage writing device
  buffer bytes at all is a red flag — D3D9 pixel shaders cannot write
  buffers; such traffic is compiler spill.
- Per-line Xcode shader profiles attribute prologue cost to the function
  signature line. A signature line owning >90% with a
  wait/load/integer-dominated mix means "prologue data motion", not "the
  function is slow" — inspect what the prologue materializes.

## Related

- `docs/perfomance/present-pacing/log.md` rows H224-H226 — the full
  attribution chain (variant tails → in-place constant reads).
- `agents/rules/metal_debugging.rules.md` — counter export discipline.
- `agents/rules/environment_variables_encoder.rules.md` — the trim knobs.
