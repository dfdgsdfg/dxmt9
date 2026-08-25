---
type: "Spec Requirements"
title: "Tests Requirements"
description: "Tests requirements and compatibility contracts."
tags: [specs, tests, requirements]
---

# Tests Requirements

Tests are controlled programs that validate specific aspects of the translation layer
in isolation — before or independently of Wine integration.
Each test has a defined input, an expected output, and a pass/fail criterion.

The primary confidence path for shader, state, and draw transformations is fast
native unit testing of stateless data transforms. Runtime Metal/readback tests
are still required, but are reserved for behaviour that source or descriptor
inspection cannot prove: texture orientation after sampling, sampler
filtering/addressing, depth writes/tests, MRT routing, alpha/fog/sRGB and other
render-state interactions, synchronization, and WSI presentation.

Three complementary external reference/oracle sources are used. They provide
observable behaviour and corpus shape; they are not implementation sources for
MIT-owned dxmt9 code unless a separate license review explicitly approves an
import and the required notices are preserved.

**vkd3d `shader_runner`** (https://gitlab.winehq.org/wine/vkd3d, LGPL-2.1):
Portable `.shader_test` corpus shape with inline `probe` assertions. Covers
SM2/SM3 programmable shaders. dxmt9 owns a native runner
(`shader_runner_dxmt9`) that accepts a documented compatible subset plus
dxmt9-local extensions. It does not need to implement vkd3d's C backend ABI or
reuse vkd3d runner code. Reference values may be produced by running an
equivalent portable test through `shader_runner_d3d9` on Windows.

**Complementary — Wine `dlls/d3d9/tests/visual.c`**
(https://github.com/wine-mirror/wine, LGPL-2.1):
29,000-line rendering test suite covering ps_1_1 through ps_3_0, fixed-function
lighting, fog, alpha test, texture ops, and stateblock behaviour. Hardcoded expected
D3DCOLOR values were verified against real D3D9 hardware. Used where
`shader_runner_d3d9` does not reach: ps_1_x shaders and fixed-function corner
cases. Tests are re-expressed in dxmt9's own test format from observable
behaviour and cited oracle values; Wine source code, control flow, helper
structure, and bulk data tables are not copied into MIT-owned tests.

**API compatibility oracle — Wine `dlls/d3d9/tests/{device,d3d9ex,stateblock}.c`**
(https://github.com/wine-mirror/wine, LGPL-2.1):
API-level conformance tests that encode Windows D3D9-observed behaviour for D3D9
COM object lifetime, base vs Ex `QueryInterface()` behaviour, state block rules,
reset/lost-device semantics, adapter/display validation, presentation-parameter
validation, shared-handle policy, PE export compatibility, HRESULT propagation,
private data handling, format-conversion and multisample validation, queries,
resource wrapper edge cases, window/cursor integration, auxiliary exports,
D3D9On12 stub compatibility, and D3D9Ex user-memory resources.
These behaviours are re-expressed as small PE executables that run under Wine
against dxmt9's `d3d9.dll`. They are behavioural oracles, not a requirement to
copy Wine's `dlls/d3d9` implementation or test harness structure.

---

## 0. Stateless Transform Unit Suites

**R-TEST-0.1** dxmt9 must provide fast native unit suites for stateless
transform functions. These tests must not require Metal, Wine, D3D9 COM, GPU
readback, asynchronous backend scheduling, or wall-clock timing.

**R-TEST-0.2** Shader translation unit tests must validate D3DBC decode,
instruction classification, register semantics, source modifiers, write masks,
relative addressing, semantic mapping, control-flow lowering, and generated
MSL/IR text from plain bytecode inputs. These tests are the first-line coverage
for opcode and source-contract regressions; runtime shader probes are reserved
for behaviours that depend on GPU execution.

**R-TEST-0.3** Core state and draw-builder unit tests must validate conversion
from D3D9-visible state snapshots to immutable backend data: draw descriptors,
stream/index bindings, render-target/depth attachments, viewport/depth ranges,
clip planes, constants, textures, samplers, render states, texture-stage state,
and fixed-function inputs.

**R-TEST-0.4** Key/descriptor unit tests must validate deterministic generation
of FFP shader keys, PSO keys, depth-stencil descriptors, blend/MRT descriptors,
sampler descriptors, vertex layouts, color-write masks, sRGB state, and other
cache inputs. Equivalent input state must produce equivalent keys; intentionally
different D3D9 state must produce different keys where backend behaviour can
change.

**R-TEST-0.5** Runtime Metal/readback tests must not duplicate broad source or
descriptor assertions already covered by R-TEST-0.1 through R-TEST-0.4. They
must focus on behaviour that static inspection cannot prove: actual texture
orientation, sampler filtering/addressing, depth/stencil results, MRT routing,
alpha test, fog, color-write masks, sRGB conversion, synchronization, and WSI.

**R-TEST-0.6** Tests must preserve DXMT-shaped ownership. Shader translator
tests own bytecode-to-source transforms; core tests own state-to-draw-data
transforms; backend tests own descriptor encoding, resource lifecycle, and
GPU-visible behaviour; PE conformance tests own public D3D9 ABI, HRESULT,
refcount, and state-machine compatibility.

**R-TEST-0.7** Runtime `shader_runner` probes complement, but do not replace,
the stateless transform suites. A runtime probe may prove GPU-visible behaviour
such as sampled orientation, filtering, render-state interaction, or readback
results; it is not sufficient evidence for deterministic packet transforms,
shader lowering contracts, draw descriptor construction, cache-key generation,
or bridge/replay ordering. Those behaviours must have unit-level assertions over
plain data or deterministic fake-backend observations.

**R-TEST-0.8** Data-oriented transform tests must be deterministic and
allocation-aware. For any packet importer, chunk replay planner, draw-run
builder, bridge marshaler, or descriptor/key generator, the primary test must
feed explicit input records and assert exact output packets, ordering,
resource-reference sets, bridge operation counts, and allocation behaviour
without relying on Metal execution, timing, or Wine window state.

**R-TEST-0.9** Test evidence status must be tracked in `specs/tests/gap.md`, not in a
separate tests-only status inventory. Durable acceptance rules belong in this
requirements document, implementation mechanics belong in `specs/tests/spec.md`,
and current evidence, remaining gaps, and next acceptance focus belong in
`specs/tests/gap.md`.

---

## 1. Shader Translation Correctness

**R-TEST-1.1** dxmt9 must provide a native `shader_runner_dxmt9` runtime
readback harness. It must accept the dxmt9-documented `.shader_test` compatible
subset and dxmt9-local extensions needed for texture setup, geometry probes,
and render-state interactions. It must not depend on, embed, or require exact
vkd3d `shader_runner_ops` ABI compatibility.

**R-TEST-1.2** Oracle values for all `probe` assertions in `.shader_test` files must
be produced by running an equivalent portable test through `shader_runner_d3d9`
on a Windows host with a conformant D3D9 device (hardware or WARP), by a
clean-room math derivation, or by a documented Wine visual/API behavioural
oracle where the runner does not cover the feature. Oracle values must never be
derived from the dxmt9 backend itself.

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

**R-TEST-1.4** The `.shader_test` files must follow dxmt9's documented
compatible subset of the vkd3d text format:

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
implemented until the relevant transform unit tests pass. If the opcode has
GPU-visible behaviour that source or descriptor inspection cannot prove, a runtime
probe must pass as well.

**R-TEST-1.6** `shader_runner_dxmt9` must grow a dxmt9-local extended probe
layer for tests that need explicit texture setup, vertex-shader geometry
inspection, or render-state interaction beyond the portable vkd3d
`.shader_test` syntax. The extension must remain isolated from vendored vkd3d
test syntax so upstream corpus sync is not blocked by dxmt9-specific metadata.
The extension complements, but does not replace, the stateless transform unit
suites required by R-TEST-0.

**R-TEST-1.7** The extended probe layer must provide a texture setup DSL. The
minimum required coverage is:

- 2x2 and 4x4 textures with named texel colors for orientation and addressing
  probes;
- explicit mip-level contents for `texldl` and LOD selection tests;
- sampler state setup for address modes, min/mag/mip filters, and LOD bias;
- `texld`, `texldl`, and dependent-read pixel shader paths;
- actual pixel readback probes after the draw, not source-text inspection.

**R-TEST-1.8** The extended probe layer must provide vertex-shader geometry
probes. The minimum required coverage is:

- `POSITION` output mapping and clip-space orientation;
- `TEXCOORD` output mapping by semantic index, not raw output register index;
- `COLOR` / secondary color output mapping;
- viewport, half-pixel, and clip-space orientation interactions;
- actual pixel readback or an equivalent rendered geometry mask that proves the
  vertex output affected rasterization correctly.

**R-TEST-1.9** The extended probe layer must provide render-state interaction
probes. The minimum required coverage is alpha test, pixel shader `oDepth`, MRT
color outputs, fog interaction, color-write masks, and sRGB write/sampling
state. Each probe must combine shader output with the relevant D3D9 render state
and verify the final framebuffer result through readback.

**R-TEST-1.10** Extended texture, geometry, and render-state probes must converge
on the same pass criterion: the backend renders into a deterministic target,
performs real GPU readback, and compares expected pixels. Tests that only inspect
generated shader source are allowed only for debug source-contract flags such as
`DXMT_DEBUG_FORCE_PIXEL_V_FLIP` and `DXMT_DEBUG_FLIP_VERTEX_Y`, or in the
stateless transform unit suites required by R-TEST-0.

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

**R-TEST-2.2** For ps_1_x coverage (where the vkd3d D3D9 backend skips below
ps_2_0), oracle values may be validated against Wine `visual.c` hardcoded
expected colors. Each clean-room dxmt9 test must cite the originating Wine test
function name and license/provenance scope in a comment or provenance block
(see section 8). Exact Wine source snippets, control flow, and copied tables
are not MIT-owned dxmt9 code.

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

**R-TEST-4.3** There must be a programmable texture-orientation smoke test that
creates a 2x2 texture with distinct top-left, top-right, bottom-left, and
bottom-right colors, draws it through a `ps_2_0` `texld` path, performs
`GetRenderTargetData()` readback, and verifies all four rendered quadrants. The
test must fail if translated pixel shaders globally flip the V coordinate by
default.

**R-TEST-4.4** There must be native source-contract tests for the coordinate
debug flags. With `DXMT_DEBUG_FORCE_PIXEL_V_FLIP` unset, translated
programmable pixel shader source must not contain the forced `1.0f - v` sample
coordinate transform; with the flag set, the transform must be visible in the
generated source. Separately, `DXMT_DEBUG_FLIP_VERTEX_Y` must only affect
translated vertex shader source and must not be coupled to pixel shader
sampling.

---

## 5. Resource Mapping and Synchronisation

**R-TEST-5.1** There must be a test demonstrating that `D3DLOCK_DISCARD` on a dynamic
vertex buffer returns memory not visible to an already-submitted draw, and that the
new data is visible to the subsequent draw.

**R-TEST-5.2** There must be a test demonstrating that texture data written via
`Lock`/`Unlock` on a `D3DPOOL_MANAGED` texture is correctly uploaded and visible in
a subsequent texture sample.

**R-TEST-5.3** Imported chunk replay must have deterministic fake-backend or
queue-observer tests that prove replay order independently of Metal. The
observer must record every imported command kind, draw-run boundary, clear,
copy/readback, present, and flush in submission order, then assert the exact
sequence produced from a fixed input chunk.

**R-TEST-5.4** Imported replay resource lifetime tests must prove seq-id
pinning. A queue-facing observer or test snapshot API must record chunk seq IDs,
the resources pinned for each seq ID, and the corresponding release/completion
point. The pass criterion is that resources referenced by imported packets stay
retained until the queue has completed the seq ID that last uses them.

**R-TEST-5.5** Barrier and hazard ordering must be testable without GPU timing.
The test backend or queue observer must record upload, render, readback,
present, and hazard/barrier events in the order encoded by the replay path.
Tests must assert that required barriers occur before dependent use and that
readback/flush operations are not reordered across earlier writes.

**R-TEST-5.6** Hot-path replay tests must include allocation checks for
data-oriented paths. After any required setup or warm-up, importing and
replaying a fixed chunk or packet sequence must not allocate from the general
heap on the measured path, except for explicitly documented scratch growth that
is bounded, amortized, and covered by a separate capacity test.

**R-TEST-5.7** Wine bridge and C-ABI packet tests must assert bridge operation
counts as part of acceptance. For fixed public calls or imported replay inputs,
tests must record the number and order of bridge ops emitted across the PE to
unix boundary, including batching expectations. A passing runtime result alone
does not prove merge compatibility if the op stream regresses into per-state or
per-draw chatty calls.

**R-TEST-5.8** Deterministic packet transform coverage must exist for each
imported command class before runtime-only evidence is accepted. The suite must
cover valid packets, rejected malformed packets, truncation, variable-size
payloads, resource references, draw-run coalescing, state deltas, hazard scopes,
and preservation of command order after batching.

---

## 6. Presentation

**R-TEST-6.1** There must be a test that creates a swap chain, renders a colored frame,
and calls `Present()`. Pass criterion: the rendered color is readable via
`GetRenderTargetData()` before `Present()`.

---

## 7. Regression Scope

**R-TEST-7.1** All native regression tests must be runnable without Wine — as
native macOS executables that drive the backend directly, without D3D9 COM layer
involvement. Wine-required integration/conformance tests are explicitly scoped
to sections 11 and 12.

**R-TEST-7.2** All tests must be deterministic. Non-determinism from async PSO
compilation must be masked by running a warm-up pass before the measured draw.

**R-TEST-7.3** The dxmt9 `.shader_test` corpus must be tracked in
`tests/shader_runner/corpus/`. Upstream vkd3d files may be used as external
corpus references or third-party fixtures only when the manifest records their
license, source kind, license scope, and upstream commit. Clean-room dxmt9
tests are preferred for MIT-owned coverage. New upstream opcode coverage that
falls within the SM2/SM3 arithmetic and texture groups must be reviewed within
one release cycle and either re-expressed locally or tracked as an explicitly
licensed external fixture.

**R-TEST-7.4** Tests that claim data-oriented or DXMT-merge compatibility must
include explicit DoD evidence, not just green runtime probes. Required evidence
is: stateless transform assertions, fake-backend or queue-observer replay
traces, seq-id resource pinning checks, barrier/hazard order checks,
allocation-free hot-path checks, and bridge-op count/order checks where the PE
bridge is involved.

---

## 8. Wine visual.c Complementary Coverage

Wine `dlls/d3d9/tests/visual.c` is the complementary oracle for areas outside
`shader_runner`'s scope. It is not run directly; individual behaviours are
re-expressed as dxmt9-owned tests unless an explicitly licensed third-party
fixture is approved.

**R-TEST-8.1** Each test validated against `visual.c` must:
- Cite the originating Wine test function in a comment:
  `// behavioral oracle: Wine visual.c:<function_name>`
- Re-express the scenario in dxmt9-owned test data and control flow.
- Use oracle colours and tolerances only as expected observable results, with
  provenance recorded. If exact Wine literals, shader assembly, or tables are
  copied, the file must be marked as a third-party fixture with the LGPL scope
  preserved, not as MIT-owned project code.

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
following format. `source_kind` is one of `project-authored`,
`behavioral-oracle`, `structure-reference`, `third-party-fixture`, or
`implementation-source`. `license_scope` is one of `project-mit`,
`third-party-fixture`, or `external-not-vendored`.

```
; [provenance]
; source: vkd3d
; source_kind: third-party-fixture
; license: LGPL-2.1-or-later
; license_scope: third-party-fixture
; upstream-url: https://gitlab.winehq.org/wine/vkd3d
; upstream-commit: <git SHA of the vkd3d commit this file was taken from or last synced to>
; oracle: shader_runner_d3d9
; oracle-env: Windows 11 / WARP
; oracle-date: YYYY-MM-DD
```

For tests validated against Wine `visual.c`:

```
; [provenance]
; source: wine/visual.c:<function_name>
; source_kind: behavioral-oracle
; license: LGPL-2.1-or-later
; license_scope: external-not-vendored
; upstream-url: https://github.com/wine-mirror/wine
; upstream-commit: <git SHA>
; oracle: real D3D9 hardware (recorded in Wine test history)
; oracle-date: YYYY-MM-DD
```

For tests with math-derived oracle values:

```
; [provenance]
; source: dxmt9
; source_kind: project-authored
; license: MIT
; license_scope: project-mit
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

**R-TEST-10.1** `tests/shader_runner/corpus/MANIFEST.toml` must list every `.shader_test`
file in the corpus. Each entry must include:

```toml
[[test]]
file    = "arithmetic/mad.shader_test"
source  = "vkd3d"                        # vkd3d | wine/visual.c | dxmt9
source_kind = "third-party-fixture"       # project-authored | behavioral-oracle | structure-reference | third-party-fixture | implementation-source
license = "LGPL-2.1-or-later"
license_scope = "third-party-fixture"     # project-mit | third-party-fixture | external-not-vendored
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

---

## 11. WSI Integration Test

The WSI test exercises the full PE → COM → Wine → Metal presentation stack.
It is the only test category that **requires Wine** and **cannot run as a native
macOS executable** — it is therefore separate from the R-TEST-7.1 scope.

**R-TEST-11.1** There must be a cross-compiled Win32 PE test executable
(`build/wsi_present/wsi_present_x64.exe`) that:

1. Creates a Win32 window via `CreateWindow`.
2. Calls `Direct3DCreate9` (routed through our `d3d9.dll`).
3. Creates an `IDirect3DDevice9` with that window as `hDeviceWindow`.
4. Clears and presents 180 frames (red / green / blue, 60 frames each).
5. Exits with code 0 on success, non-zero on any `FAILED(hr)` result.

**R-TEST-11.2** The test executable must be built from source using llvm-mingw
targeting `x86_64-w64-mingw32` for the default macOS/Rosetta Wine64 path. It
must not depend on any pre-built binary or Wine-specific SDK — only the standard
`d3d9.h` / `windows.h` headers from llvm-mingw's sysroot.

**R-TEST-11.3** The pass criterion is:

| Check | Criterion |
|---|---|
| `Direct3DCreate9` | Returns non-null |
| `CreateDevice` | `SUCCEEDED(hr)` |
| `Present` × 180 | All `SUCCEEDED(hr)` |
| Exit code | 0 |
| Visual | Window cycles visibly red → green → blue (manual check) |

**R-TEST-11.4** The test must be runnable with a recent Wine64-capable build on
macOS that provides:

- `winemac.drv`
- Wine builtin-module support (`winebuild` + builtin import libs at build time)
- the `x86_64-windows` / `x86_64-unix` runtime module directories

It must not require a custom Wine fork, but it may require a separate Wine
toolchain install tree for building the builtin PE bridge.

**R-TEST-11.5** Installation procedure:

```sh
# Build-time Wine toolchain root must provide winebuild, libwinecrt0.a,
# libntdll.a, libdbghelp.a, winemac.so, and ntdll.so.
meson setup build-win32-x64-builtin \
  --cross-file cross/x86_64-windows.ini \
  -Dwine_builtin_dll=true \
  -Dwine_install_path=<wine-toolchain-root>
meson compile -C build-win32-x64-builtin

meson setup build-x86_64-builtin \
  --native-file cross/x86_64-macos.ini \
  -Dwine_install_path=<wine-toolchain-root>
meson compile -C build-x86_64-builtin

cp build-win32-x64-builtin/src/win32/d3d9.dll <wine-prefix>/drive_c/windows/system32/d3d9.dll
cp build-win32-x64-builtin/src/winemetal/winemetal_dxmt9.dll \
  <wine-root>/lib/wine/x86_64-windows/winemetal_dxmt9.dll
cp build-x86_64-builtin/src/winemetal/unix/winemetal_dxmt9.so \
  <wine-root>/lib/wine/x86_64-unix/winemetal_dxmt9.so
WINEDLLOVERRIDES="d3d9=n,b" wine build/wsi_present/wsi_present_x64.exe
```

**R-TEST-11.6** When neither the legacy `macdrv_get_cocoa_view` export nor the
`macdrv_functions` fallback table is available (for example outside Wine or
without `winemac.drv` loaded), `CreateDevice` must still succeed and `Present`
must not crash. The present is a no-op in this case (no visible output). This
behaviour is validated by the existing `dxmt9-core-spec` test suite which
exercises the present path with a null window handle.

---

## 12. Wine-Oracle D3D9 API Conformance Tests

The Wine-oracle D3D9 conformance subset exercises Windows D3D9-compatible,
application-visible COM and state-machine behaviour through the real PE ABI. It
is separate from native backend tests and requires Wine as the host runtime.

**R-TEST-12.1** There must be cross-compiled Win32 PE conformance executables
under `tests/conformance/d3d9/`. They must load `d3d9.dll` through Wine's normal
DLL search path so the same binaries can be used against the app-local and Wine
runtime builtin deployment lanes.

**R-TEST-12.2** Each clean-room conformance case must cite the Wine source file,
function name, upstream commit, source kind, license, and license scope in a
comment near the test body or manifest entry. The provenance must be
machine-readable in the same style as shader corpus provenance. Copying Wine
test harness code, helper structure, control flow, or bulk tables into
MIT-owned dxmt9 tests is forbidden unless the file is explicitly segregated as a
third-party LGPL fixture.

**R-TEST-12.3** The initial required subset from Wine `device.c` is:

| Wine function / area | Required coverage |
|---|---|
| `test_refcount` and related object lifetime checks | public COM refcounts for device children, implicit swap chains, back buffers, texture-level surfaces, and `Get*` methods |
| scene tests | `BeginScene()` / `EndScene()` success and invalid-call transitions |
| display-mode and factory validation tests | adapter index, display format, valid-vs-invalid device type, multisample quality-level behaviour, invalid multisample enum handling, `CheckDeviceType`, `CheckDeviceFormat`, `CheckDeviceFormatConversion`, mode enumeration return codes, and `GetAdapterDisplayMode()` alpha-format normalization |
| presentation-parameter tests | invalid `SwapEffect`, `BackBufferCount`, `D3DSWAPEFFECT_COPY` count, `PresentationInterval` combinations, `BackBufferWidth/Height == 0` window-derived normalization, `BackBufferFormat == UNKNOWN` handling, and caller-visible `D3DPRESENT_PARAMETERS` mutation |
| reset/lost-device tests | `Reset()`, default-pool invalidation, managed/systemmem survival, reset clearing scene state, and cooperative-level transitions |
| private-data tests | `SetPrivateData`, `GetPrivateData`, `FreePrivateData`, `D3DSPD_IUNKNOWN` ownership, failed-set preservation, missing GUID size preservation, and coverage for every resource wrapper |
| shared-handle tests | non-Ex `E_NOTIMPL`, Ex pool/resource-class error codes, D3D9Ex user-memory texture/offscreen-surface success cases, lock pointer/pitch validation, and no silent ignore of non-null shared handles |
| query tests | `CreateQuery(type, NULL)` support probes, invalid query type return codes and out-pointer preservation, `GetDataSize()` values, pre-issue `GetData()` data writes, short-buffer handling, and occlusion/timestamp-disjoint public data sizes |
| resource wrapper tests | texture/surface/volume `GetContainer()` and `GetLevelDesc()` behaviour, invalid level handling, `LockRect()` / `LockBox()` invalid rectangles, double lock/unlock cases, block-compressed alignment, `GetDC()` / `ReleaseDC()`, `SetLOD()` / `GetLOD()`, autogen mipmap filter/generation, and `D3DFMT_UNKNOWN` creation failures |
| window and cursor tests | `SetCursorProperties()`, `SetCursorPosition()`, cursor clipping, device window reset, focus/device window message handling, fullscreen/windowed style changes, destroyed-window behaviour, and desktop-window creation |
| auxiliary compatibility tests | `Direct3DShaderValidatorCreate9`, `D3DPERF_*`, `DebugSetMute`, and `Direct3DCreate9On12` loader-safe behaviour |

**R-TEST-12.4** The initial required subset from Wine `d3d9ex.c` is:

| Wine function / area | Required coverage |
|---|---|
| base vs Ex `QueryInterface()` tests | `Direct3DCreate9()` objects reject Ex interfaces; `Direct3DCreate9Ex()` objects expose base and Ex interfaces |
| Ex-created normal device | `IDirect3D9Ex::CreateDevice()` returns a device that can QI `IDirect3DDevice9Ex` |
| adapter LUID/display-mode tests | `GetAdapterLUID`, `GetAdapterDisplayModeEx`, `EnumAdapterModesEx`, filter and size validation |
| Ex create/reset validation tests | `CreateDeviceEx` and `ResetEx` fullscreen-mode size, windowed/fullscreen relation, and mode/back-buffer size matching |
| Ex reset/device-state tests | `ResetEx`, `CheckDeviceState`, Ex cooperative-level behaviour, and resource survival differences |
| Ex swap-chain tests | `IDirect3DSwapChain9Ex` exposure, `GetDisplayModeEx` size validation, `GetLastPresentCount`, and `GetPresentStatistics` zeroing behaviour |
| `test_user_memory` | `D3DPOOL_SYSTEMMEM` user-memory texture and offscreen-surface success, `LockRect` pointer/pitch identity, and invalid level/pool/resource-class failures |
| Ex window tests | `test_wndproc`, `test_wndproc_windowed`, `test_window_style`, desktop-window creation, and Ex-specific reset/window-message behaviour |
| Ex resource and frame-latency tests | `test_format_unknown`, resource-access/sysmem draw cases, pinned buffers, `SetMaximumFrameLatency()` / `GetMaximumFrameLatency()` validation, and present-statistics stubs |

**R-TEST-12.5** The initial required subset from Wine `stateblock.c` is:

| Wine function / area | Required coverage |
|---|---|
| `CreateStateBlock(D3DSBT_ALL)` | exact captured state subset and documented D3D9 quirks |
| `D3DSBT_VERTEXSTATE` / `D3DSBT_PIXELSTATE` | type-specific state masks |
| `BeginStateBlock()` / `EndStateBlock()` | delta recording, nested-recording invalid calls, and no-active-recording invalid calls |
| `Capture()` / `Apply()` | invalid calls while recording, restore ordering, derived-cache invalidation, render-target interactions, shaders/constants, lights, transforms, render states, texture stage states, sampler states, textures, autogen-mipmap bits, vertex declarations/FVF, and stream/index bindings |

**R-TEST-12.6** These conformance tests are allowed to fail during test-first
implementation, but any failing case must be listed in `specs/tests/gap.md` with the
corresponding Wine source anchor and implementation owner area.

**R-TEST-12.7** The conformance subset must not require Windows access
violations for invalid pointers. If Wine documents a crash-only invalid-pointer
case, the dxmt9 test must either skip it or assert dxmt9's specified clean
failure path from R-CORE-9.5.

**R-TEST-12.8** Object creation tests must assert exact `HRESULT` propagation
when validation or backend setup fails. A generic null object result is not
sufficient unless the public COM method also returns the Windows
D3D9-compatible failure code for that scenario.

**R-TEST-12.9** A PE export smoke test must `LoadLibrary("d3d9.dll")` and
`GetProcAddress()` every required D3D9 export used by Windows/Wine applications:
`Direct3DCreate9`, `Direct3DCreate9Ex`, `Direct3DShaderValidatorCreate9`,
`D3DPERF_BeginEvent`, `D3DPERF_EndEvent`, `D3DPERF_GetStatus`,
`D3DPERF_QueryRepeatFrame`, `D3DPERF_SetMarker`, `D3DPERF_SetOptions`,
`D3DPERF_SetRegion`, `DebugSetMute`, and loader-safe `Direct3DCreate9On12`.
The test must also assert Windows D3D9-compatible no-op return behaviour for
the `D3DPERF_*` functions that can be called safely.

**R-TEST-12.10** Factory validation tests must cover Wine-test-observed Windows
D3D9 edge cases that are easy to regress: identical
`CheckDeviceFormatConversion()` source and destination formats return `D3D_OK`,
unsupported conversions return `D3DERR_NOTAVAILABLE`, invalid multisample enum
values return `D3DERR_INVALIDCALL`, `D3DMULTISAMPLE_NONE` reports one quality
level, and unsupported but well-formed multisample requests preserve/write
`pQualityLevels` according to the Wine behavioural oracle.

**R-TEST-12.11** D3D9Ex user-memory tests must be validated against Wine
`dlls/d3d9/tests/d3d9ex.c:test_user_memory`. They must assert successful
`D3DPOOL_SYSTEMMEM` `CreateTexture()` with exactly one mip level,
`CreateOffscreenPlainSurface()` caller-memory creation, `LockRect()` pointer and
pitch identity, and Windows D3D9-compatible failures for invalid levels,
unsupported pools, cube/volume textures, and vertex/index buffers.

**R-TEST-12.12** Query conformance tests must be validated against Wine
`dlls/d3d9/tests/device.c:test_query_support`, `test_occlusion_query`, and
`test_timestamp_query`. They must cover support probing with
`CreateQuery(type, NULL)`, invalid query enums, unchanged out pointers on
creation failure, public `GetDataSize()` values, pre-issue `GetData()` writes,
short-buffer writes without overrun, `D3DGETDATA_FLUSH`, and the
timestamp-disjoint `BOOL` result size.

**R-TEST-12.13** Resource wrapper conformance tests must be validated against Wine
`device.c` resource tests including `test_surface_get_container`,
`test_volume_get_container`, `test_lod`, `test_getdc`, `test_surface_blocks`,
`test_volume_locking`, `test_mipmap_gen`, `test_filter`, and
`test_format_unknown`. The dxmt9 ports must prioritise observable COM/HRESULT,
pitch, pointer, and data-preservation behaviour over Wine's internal storage
layout.

**R-TEST-12.14** Window and cursor conformance tests must be validated against Wine
`device.c` / `d3d9ex.c` tests covering `test_cursor`, `test_cursor_pos`,
`test_cursor_clipping`, `test_wndproc`, `test_wndproc_windowed`,
`test_window_style`, `test_device_window_reset`, `test_destroyed_window`, and
desktop-window creation. Tests that depend on platform-specific window-manager
messages may allow a narrow Wine-host variance, but must still assert stable
D3D9 API return values and device/window ownership state.

**R-TEST-12.15** Auxiliary export tests must cover more than symbol presence.
`Direct3DShaderValidatorCreate9()` must be called and its returned validator
must be exercised for loader-safe `QueryInterface`, `AddRef`, `Release`,
`Begin`, `Instruction`, and `End` behaviour. `D3DPERF_*` and `DebugSetMute`
must be callable no-op exports with Wine/Windows-compatible return values for
safe inputs.

**R-TEST-12.16** D3D9On12 compatibility tests must be validated against Wine
`device.c:test_d3d9on12`. Because dxmt9 does not implement D3D12 interop, the
required behaviour is loader-safe and query-safe failure: the export must be
present, unsupported interface queries must return `E_NOINTERFACE` with a
cleared output pointer, `GetD3D12Device(NULL)` must return `E_INVALIDARG` if the
interface is exposed, and unwrap/return methods must fail without touching
backend Metal state.

**R-TEST-12.17** Presentation-parameter tests must assert caller-visible
normalisation after successful `CreateDevice()`, `CreateDeviceEx()`, `Reset()`,
and `ResetEx()`. Required cases include zero back-buffer dimensions, zero
back-buffer count, unknown back-buffer format in windowed mode, fullscreen mode
matching, and preservation of invalid input values on failure where the Wine
oracle observes preservation.

**R-TEST-12.18** Device utility and creation-flag tests must include Wine-oracle
coverage for `GetDirect3D(NULL)`, `GetDeviceCaps(NULL)`, `GetCreationParameters`,
`GetAvailableTextureMem`, `EvictManagedResources`, `ValidateDevice`,
`GetRasterStatus`, `SetDialogBoxMode`, `D3DCREATE_FPU_PRESERVE`,
`D3DCREATE_MULTITHREADED`, and `D3DCREATE_NOWINDOWCHANGES`. Where Metal cannot
provide a native equivalent, the test asserts the Windows D3D9-compatible stub
or validation result rather than backend behaviour.

**R-TEST-12.19** Stateblock conformance ports must use the Wine
`stateblock.c:test_state_management` matrix as the coverage checklist. The
minimum passing set includes render states, texture stage states, sampler
states, transforms, material, lights, shader constants, vertex and pixel
shaders, textures, render targets, stream sources, index buffers, vertex
declarations, generated FVF declarations, and the interaction between state
blocks and reset/resource lifetime.

**R-TEST-12.20** The PE conformance suite must have its own machine-readable
manifest under `tests/conformance/d3d9/MANIFEST.toml`. Each entry must include
the executable, local test function, Wine source anchor, upstream commit,
required deployment lanes (`app-local`, `builtin`, or both), architecture
targets (`x86`, `x64`), status, owning implementation area, mapped R-TEST-12
requirements, and explicit acceptance / DoD criteria. Entries for implemented
local scaffolds must also name the source file that contains the test function.
The manifest is the authoritative gap list for Wine-oracle D3D9 API
conformance and must be validated by the native manifest check before changes
are accepted.

**R-TEST-12.21** The PE conformance manifest must be queryable by a checked-in
status-report tool. The tool must summarize status counts, group next actions
by `scaffolded`, `failing`, `partial`, `passing`, `skipped`, and `todo`, expose
a Mermaid output for roadmap reviews, and provide a nonzero full-support gate
for release/merge readiness. The full-support gate must pass only when every
manifest case is `passing`; it must not infer support from local scaffolds or
partial lane evidence.
