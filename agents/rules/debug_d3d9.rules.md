---
description: D3D9 draw, shader, FVF, vertex declaration, render-state, topology, and frame evidence debugging rules
paths:
  - "src/d3d9/**"
  - "src/dxmt9/**"
  - "tests/native/**"
  - "tests/shader_runner/**"
  - "specs/d3d9/**"
  - "specs/tests/**"
globs: "{src,tests,specs}/**/*"
alwaysApply: false
---

# D3D9 Render Debugging Rules

Use this for D3D9 rendering intent: draw calls, topology, shaders, FVF/vertex
declarations, streams, constants, render states, fixed-function behaviour, and
frame/readback evidence.

## Test Intent, Not Just Normalized Values

A D3D9 draw can be represented differently after normalization. Tests must prove
that the next boundary receives the same rendering intent, not merely that an
enum changed.

**Rules:**
- For topology conversion, assert generated vertices or indices. `TriangleFan`
  normalized to `TriangleList` is incomplete unless payloads such as
  `{0,1,2,0,2,3}` are checked.
- For UP draws, verify copied vertex/index bytes and their lifetime, not only
  draw counts.
- For non-UP draws, verify bound buffer handles, offsets, strides, base vertex,
  start index, and generated payload policy at the backend/encoder boundary.
- Rendered pixels are useful evidence, but boundary-value tests should still
  assert the exact data crossing core, bridge, importer, backend, and encoder
  layers.

## Shader And Vertex Evidence

Shader, FVF, vertex declaration, and animation/skinning bugs are often semantic
mapping bugs before they are Metal compiler bugs.

**Rules:**
- Check D3DBC source-contract tests before relying on shader-runner pixels.
- Verify `D3DDECLUSAGE`, usage index, stream, offset, stride, declaration type,
  FVF layout, and shader `dcl_*` expectations as explicit values.
- For vertex blending/skinning, assert weights, indices, world matrices,
  indexed-vs-non-indexed mode, and FVF beta fields. A single rendered triangle
  is not enough for broad rigging coverage.
- Do not copy Wine shader literals or helper bodies; use Wine/vkd3d facts only
  as external behavioural oracle data with provenance.

## Render-State And Capture Triage

State toggles can isolate a bug, but they are not fixes. Keep diagnostic knobs
bounded and disabled by default.

**Rules:**
- Use draw filters (`DXMT9_DRAW_SEQ_*`, `DXMT9_DRAW_ORDINAL_*`), forced shader
  colors, cull/scissor/depth toggles, and capture paths to bisect. Remove or
  gate any new knob through `environment_variables.rules.md`.
- Internal backbuffer dumps prove runtime output at a frame; window capture
  proves compositor/WSI output. Keep the source explicit in `debug_result.json`.
- If an internal frame was requested but unavailable, prefer a skipped-frame
  sidecar with reason and counters over silent absence.
- Release-default paths must not allocate, format logs, write files, or cross the
  bridge for diagnostics unless the feature is explicitly enabled.

## Related

- `agents/rules/debug_windows.rules.md` - public D3D9 ABI and HRESULT/COM
  behaviour.
- `agents/rules/debug_metal.rules.md` - Metal capture and command-buffer
  diagnostics.
- `agents/rules/environment_variables.rules.md` - diagnostic knobs.
- `specs/tests/design.md` - boundary-value and render-intent test contracts.
