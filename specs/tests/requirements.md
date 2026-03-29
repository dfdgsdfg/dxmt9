# Tests Requirements

Tests are controlled programs that validate specific aspects of the translation layer
in isolation — before or independently of Wine integration.
Each test has a defined input, an expected output, and a pass/fail criterion.

Two complementary test sources are used:

**Primary — vkd3d `shader_runner`** (https://gitlab.winehq.org/wine/vkd3d, LGPL-2.1):
Portable `.shader_test` files with inline `probe` assertions. Covers SM2/SM3
programmable shaders. dxmt9 adds one backend (`shader_runner_dxmt9`); the same files
run against `shader_runner_d3d9` on Windows to produce oracle values.

**Complementary — Wine `dlls/d3d9/tests/visual.c`**
(https://github.com/wine-mirror/wine, LGPL-2.1):
29,000-line rendering test suite covering ps_1_1 through ps_3_0, fixed-function
lighting, fog, alpha test, texture ops, and stateblock behaviour. Hardcoded expected
D3DCOLOR values were verified against real D3D9 hardware. Used where
`shader_runner_d3d9` does not reach: ps_1_x shaders and fixed-function
corner cases. Tests are ported into dxmt9's own test format rather than run directly.

---

## 1. Shader Translation Correctness

**R-TEST-1.1** dxmt9 must provide a `shader_runner_dxmt9` backend that implements the
vkd3d `shader_runner` backend interface. It must accept the same `.shader_test` files
as the existing `shader_runner_d3d9` and `shader_runner_vulkan` backends.

**R-TEST-1.2** Oracle values for all `probe` assertions in `.shader_test` files must
be produced by running the same file through `shader_runner_d3d9` on a Windows host
with a conformant D3D9 device (hardware or WARP). Oracle values must never be derived
from the dxmt9 backend itself.

**R-TEST-1.3** The `.shader_test` corpus must cover the following opcode groups for
`ps_2_0`, `vs_2_0`, `ps_3_0`, and `vs_3_0`:

| Group | Opcodes | Coverage required |
|---|---|---|
| Arithmetic | MOV, ADD, SUB, MUL, MAD, RCP, RSQ, ABS, NRM, SGN | One test per opcode |
| Vector | DP3, DP4, CRS, MIN, MAX | One test per opcode |
| Comparison | CMP, CND, SGE, SLT | One test per opcode |
| Transcendental | POW, LOG, EXP, SINCOS | One test per opcode |
| Texture | TEX, TEXLDD, TEXLDL, DSX, DSY | One test per opcode |
| Matrix | M4x4, M4x3, M3x4, M3x3, M3x2 | One test per opcode |
| Flow control | IF/ELSE/ENDIF, REP/ENDREP, LOOP/ENDLOOP, CALL/RET | One test per construct |
| VS-specific | MOVA (address register), SETP | One test per opcode |
| Source modifiers | negate, abs, swizzle, partial swizzle | One test per modifier |
| Write masks | `.x`, `.xy`, `.xyz`, `.xw`, out-of-order | One test per case |

**R-TEST-1.4** The `.shader_test` files must follow the vkd3d format:

```
[require]
shader model >= ps_2_0

[pixel shader]
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    return float4(uv, 0.0, 1.0);
}

[test]
draw quad
probe (0, 0) rgba(0.0, 0.0, 0.0, 1.0) 1       ; 1 ULP tolerance
probe (31, 31) rgba(1.0, 1.0, 0.0, 1.0) 1
```

For opcodes where the HLSL frontend cannot exercise the instruction directly, raw
D3DBC hex blobs (`[pixel shader d3dbc-hex]`) may be used.

**R-TEST-1.5** Each opcode listed in R-TEST-1.3 that is not yet implemented in
`translateSpirvToMsl()` must have a corresponding `.shader_test` file committed
*before* the implementation. Tests are written first, failing, then the opcode is
implemented until the test passes (test-first for each opcode).

---

## 2. Fixed-Function Pipeline Correctness

**R-TEST-2.1** There must be `.shader_test` files (or equivalent Wine `visual.c`-style
tests for ps_1_x) for each major fixed-function feature:

- Directional lighting (single light, diffuse only)
- Point lighting (attenuation, with and without specular)
- Spot lighting (inner/outer cone angles)
- Multiple lights (up to 8, mixed types)
- Texture combine operations: MODULATE, ADD, ADDSIGNED, BUMPENVMAP, DOTPRODUCT3
- Fog: linear, exp, exp2 in both vertex-fog and pixel-fog modes
- Alpha test: all eight compare functions
- Texture coordinate generation: CAMERASPACENORMAL, SPHEREMAP, CAMERASPACEPOSITION

**R-TEST-2.2** For ps_1_x coverage (where the vkd3d D3D9 backend skips below ps_2_0),
oracle values must be taken from Wine `visual.c` hardcoded expected colors. Each
ported test must cite the originating Wine test function name in a comment (see
section 8).

**R-TEST-2.3** Each fixed-function test must validate that the `FFPKey` correctly
captures the relevant state: two setups that differ only in the tested feature must
produce different rendered outputs.

---

## 3. Half-Pixel Offset

**R-TEST-3.1** There must be a `.shader_test` that renders a 1×1 pixel quad at each
corner of a 16×16 render target and verifies that exactly the expected pixels are
covered (zero tolerance on coverage).

**R-TEST-3.2** The test must cover both programmable shaders (`vs_2_0` writing `oPos`)
and pre-transformed vertices (`D3DFVF_XYZRHW`).

---

## 4. Coordinate System

**R-TEST-4.1** There must be a test verifying winding order: a clockwise triangle
(D3D9 front-face) must not be culled with `D3DCULL_CCW` and must be culled with
`D3DCULL_CW`.

**R-TEST-4.2** There must be a test verifying that a triangle at `z = 0.5` in clip
space renders at the correct depth in the depth buffer.

---

## 5. Resource Mapping and Synchronisation

**R-TEST-5.1** There must be a test demonstrating that `D3DLOCK_DISCARD` on a dynamic
vertex buffer returns memory not visible to an already-submitted draw, and that the
new data is visible to the subsequent draw.

**R-TEST-5.2** There must be a test demonstrating that texture data written via
`Lock`/`Unlock` on a `D3DPOOL_MANAGED` texture is correctly uploaded and visible in
a subsequent texture sample.

---

## 6. Presentation

**R-TEST-6.1** There must be a test that creates a swap chain, renders a colored frame,
and calls `Present()`. Pass criterion: the rendered color is readable via
`GetRenderTargetData()` before `Present()`.

---

## 7. Regression Scope

**R-TEST-7.1** All tests must be runnable without Wine — as native macOS
executables that drive the backend directly, without D3D9 COM layer involvement.

**R-TEST-7.2** All tests must be deterministic. Non-determinism from async PSO
compilation must be masked by running a warm-up pass before the measured draw.

**R-TEST-7.3** The vkd3d `.shader_test` files used by dxmt9 must be tracked in
`tests/shader_tests/` and kept in sync with the upstream vkd3d corpus. New opcode
tests added upstream that fall within the SM2/SM3 arithmetic and texture groups must
be pulled in within one release cycle.

---

## 8. Wine visual.c Complementary Coverage

Wine `dlls/d3d9/tests/visual.c` is the complementary oracle for areas outside
`shader_runner`'s scope. It is not run directly; individual tests are ported into
dxmt9's test suite.

**R-TEST-8.1** Each test ported from `visual.c` must:
- Cite the originating Wine test function in a comment:
  `// derived from Wine: visual.c:<function_name>`
- Use the same hardcoded expected D3DCOLOR as the original test, converted to
  float RGBA for `probe` assertions.
- Preserve the original `max_diff` tolerance (typically 1–2 per channel).

**R-TEST-8.2** The following `visual.c` test functions must be ported:

| Wine function | What it covers |
|---|---|
| `test_sanity` | Basic clear + readback sanity |
| `lighting_test` | Directional, point, spot lights; specular; material |
| `fog_test` | Vertex fog, pixel fog, linear/exp/exp2 |
| `alpha_test` | All eight `D3DCMP_*` functions |
| `texture_transform_test` | Projected textures, texture matrix |
| `texop_test` | All `D3DTOP_*` texture combine operations |
| `texbem_test` | `D3DTOP_BUMPENVMAP` |
| `ps_1_4_test` | ps_1_4 shaders: texld, arithmetic |
| `fixed_function_varying_test` | Vertex color diffuse/specular in FFP |
| `vshader_version_varying_test` | vs_1_1 output registers |

**R-TEST-8.3** Wine `visual.c` coverage fills the gap below ps_2_0. No `visual.c`
test should duplicate a `.shader_test` that already covers the same behaviour at
SM2/SM3 level — the two sources are additive, not redundant.

**R-TEST-8.4** When a `visual.c` test references a feature that is also covered by
a newer `shader_runner` test at a higher shader model, the `visual.c` test is
retained for ps_1_x regression coverage only and must be annotated accordingly.

---

## 9. Provenance

Every `.shader_test` file must carry a machine-readable provenance block as
leading comments so that the origin and trustworthiness of each oracle value can
be audited without reading the test body.

**R-TEST-9.1** Each `.shader_test` file must begin with a provenance block in the
following format:

```
; [provenance]
; source: vkd3d
; upstream-url: https://gitlab.winehq.org/wine/vkd3d
; upstream-commit: <git SHA of the vkd3d commit this file was taken from or last synced to>
; oracle: shader_runner_d3d9
; oracle-env: Windows 11 / WARP
; oracle-date: YYYY-MM-DD
```

For tests ported from Wine `visual.c`:

```
; [provenance]
; source: wine/visual.c:<function_name>
; upstream-url: https://github.com/wine-mirror/wine
; upstream-commit: <git SHA>
; oracle: real D3D9 hardware (recorded in Wine test history)
; oracle-date: YYYY-MM-DD
```

For tests with math-derived oracle values:

```
; [provenance]
; source: dxmt9
; oracle: math-derivation
; oracle-date: YYYY-MM-DD
```

**R-TEST-9.2** The `oracle-env` field must identify the hardware or software
rasterizer used to produce the reference values. Acceptable values:
- `Windows <version> / WARP` — Microsoft software rasterizer (preferred; deterministic)
- `Windows <version> / <GPU model>` — specific hardware (note: may have driver quirks)
- `math-derivation` — analytically computed; no hardware involved

**R-TEST-9.3** When a `.shader_test` file is updated (oracle values changed, new
probes added), the `upstream-commit` and `oracle-date` fields must be updated to
reflect the new state. Stale provenance is treated as a test defect.

**R-TEST-9.4** Oracle values produced by the dxmt9 backend itself are forbidden.
The `oracle` field must never read `shader_runner_dxmt9`.

---

## 10. Manifest

A machine-readable manifest tracks every test file in the corpus, enabling coverage
reports, upstream sync checks, and gap analysis without running the tests.

**R-TEST-10.1** `tests/shader_tests/MANIFEST.toml` must list every `.shader_test`
file in the corpus. Each entry must include:

```toml
[[test]]
file    = "arithmetic/mad.shader_test"
source  = "vkd3d"                        # vkd3d | wine/visual.c | dxmt9
models  = ["ps_2_0", "ps_3_0"]           # shader models exercised
opcodes = ["MAD"]                        # D3DBC opcodes under test
status  = "passing"                      # passing | failing | skipped
```

**R-TEST-10.2** The manifest must be updated atomically with any change to the
corpus: adding, removing, or renaming a `.shader_test` file without updating
`MANIFEST.toml` is a build error (enforced by a Meson custom target that diffs
the manifest against the filesystem).

**R-TEST-10.3** The manifest `status` field must reflect the last known run result
on the dxmt9 backend. A `failing` entry is permitted (test-first workflow) but
must have a corresponding open implementation task. A `passing` entry that begins
failing is a regression.

**R-TEST-10.4** The manifest must be queryable to produce:
- List of all opcodes with no passing test (coverage gap report)
- List of tests whose `upstream-commit` differs from the current vkd3d HEAD
  (upstream drift report)
- Count of passing / failing / skipped tests per shader model
