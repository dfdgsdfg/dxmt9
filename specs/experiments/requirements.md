# Experiments Requirements

Experiments are isolated, self-contained programs that validate a specific aspect of
the translation layer in isolation — before or independently of Wine integration.
Each experiment has a defined input, an expected output, and a pass/fail criterion.

---

## 1. Shader Translation Correctness

**R-EXP-1.1** For each supported shader model (vs_1_1, ps_1_4, vs_2_0, ps_2_0,
vs_3_0, ps_3_0), there must be at least one experiment that:
- Takes a known D3DBC bytecode program as input
- Translates it through the backend shader pipeline
- Renders a known geometric input with that shader
- Compares the rendered output to a reference image (pixel-exact or within tolerance)

**R-EXP-1.2** The reference images for shader translation experiments must be
produced by a conformant D3D9 implementation (e.g., Windows + WARP device) or by
inspection of the shader math, not by the dxmt9 backend itself.

**R-EXP-1.3** Shader translation experiments must cover:
- Float arithmetic (MAD, MUL, DP3, DP4, RSQ, RCP)
- Texture sampling (2D, cube, with bilinear and point filtering)
- Control flow (IF/ELSE, REP/ENDREP, LOOP/ENDLOOP for SM 2.0+)
- Output registers (oPos, oD0/oD1, oT0–oT7 for pre-3.0; generic o# for SM 3.0)
- Source modifiers (negate, abs, swizzle)
- PS: color and alpha output (oC0–oC3, oDepth)

---

## 2. Fixed-Function Pipeline Correctness

**R-EXP-2.1** There must be an experiment for each major fixed-function feature:
- Directional lighting (single light, diffuse only)
- Point lighting (attenuation, with and without specular)
- Spot lighting (inner/outer cone angles)
- Multiple lights (up to 8, mixed types)
- Texture combine operations: MODULATE, ADD, ADDSIGNED, BUMPENVMAP, DOTPRODUCT3
- Fog: linear, exp, exp2 in both vertex-fog and pixel-fog modes
- Alpha test: all eight compare functions
- Texture coordinate generation: CAMERASPACENORMAL, SPHEREMAP, CAMERASPACEPOSITION

**R-EXP-2.2** Each fixed-function experiment must validate that the `FFPKey` correctly
captures the relevant state: two setups that differ only in the tested feature must
produce different rendered outputs.

---

## 3. Half-Pixel Offset

**R-EXP-3.1** There must be an experiment that renders a 1×1 pixel quad at each
corner of a render target and verifies that exactly the expected pixels are covered.
This validates that the half-pixel correction produces correct rasterization alignment
between D3D9 screen-space positions and Metal pixel centers.

**R-EXP-3.2** The experiment must test both programmable shaders (vs_2_0 writing oPos)
and pre-transformed vertices (`D3DFVF_XYZRHW`).

---

## 4. Coordinate System

**R-EXP-4.1** There must be an experiment that renders a known triangle in clip space
and verifies winding order: a triangle with D3D9 clockwise vertex order (front-face)
must not be culled when `D3DRS_CULLMODE = D3DCULL_CCW` (D3D9 default), and must be
culled when `D3DRS_CULLMODE = D3DCULL_CW`.

**R-EXP-4.2** There must be an experiment verifying that NDC clip space is identical
between D3D9 and Metal: a triangle at `z = 0.5` in clip space must render without
near/far clip and must appear at the correct depth in the depth buffer.

---

## 5. Resource Mapping and Synchronization

**R-EXP-5.1** There must be an experiment demonstrating that `D3DLOCK_DISCARD` on a
dynamic vertex buffer returns new memory that is not visible to a draw call already
submitted but not yet completed, and that the new data is visible to the subsequent
draw call.

**R-EXP-5.2** There must be an experiment demonstrating that texture data written via
`Lock`/`Unlock` on a `D3DPOOL_MANAGED` texture is correctly uploaded and visible in
a subsequent texture sample.

---

## 6. Presentation

**R-EXP-6.1** There must be an experiment that creates a swap chain, renders a colored
frame, and calls `Present()`. The pass criterion is that the window displays the
rendered color without tearing or corruption.

**R-EXP-6.2** There must be an experiment verifying that `D3DSWAPEFFECT_DISCARD`
behavior is correct: back buffer contents after `Present()` are not required to be
preserved (the experiment must not assume preservation).

---

## 7. Regression Scope

**R-EXP-7.1** Every experiment in `tests/` must be runnable without Wine — as a
native macOS executable that drives the backend directly, without D3D9 COM layer
involvement. This keeps the experiment loop fast and decoupled from Wine integration.

**R-EXP-7.2** All experiments must be deterministic: given the same input, the same
output must be produced on every run. Non-determinism from async PSO compilation must
be masked by running experiments after a warm-up pass.
