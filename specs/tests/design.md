# Tests Design

---

## 1. Test Infrastructure: vkd3d shader_runner

The test infrastructure is built on the **vkd3d `shader_runner`** framework. The
framework has two parts: a backend-agnostic runner and pluggable backends.

```mermaid
graph TD
    subgraph Corpus["Test corpus"]
        ST["tests/shader_tests/*.shader_test\n(portable test files)"]
    end

    subgraph Runner["shader_runner (vkd3d)"]
        PARSE["Parse .shader_test\n— compile shader\n— set uniforms\n— issue draw quad\n— probe pixels"]
    end

    subgraph Backends
        D3D9["shader_runner_d3d9\n(Windows oracle)"]
        VK["shader_runner_vulkan\n(Linux reference)"]
        DXMT9["shader_runner_dxmt9\n(dxmt9 backend — new)"]
    end

    ST --> PARSE
    PARSE --> D3D9
    PARSE --> VK
    PARSE --> DXMT9
```

`shader_runner_dxmt9` drives the dxmt9 `BackendDevice` interface directly — no Wine,
no D3D9 COM. It is a native macOS executable.

---

## 2. `.shader_test` File Format

Each `.shader_test` file is a self-describing test case. The format is defined by
vkd3d and documented in `tests/shader_runner.c`.

```
[require]
shader model >= ps_2_0           ; skip if backend does not support this

[pixel shader]
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    return float4(uv.x, uv.y, 0.0, 1.0);
}

[test]
draw quad                        ; full-screen quad with UVs in [0,1]
probe (0,  0)  rgba(0.0, 0.0, 0.0, 1.0) 1    ; (x,y) pixel, ULP tolerance
probe (31, 0)  rgba(1.0, 0.0, 0.0, 1.0) 1
probe (0,  31) rgba(0.0, 1.0, 0.0, 1.0) 1
probe (31, 31) rgba(1.0, 1.0, 0.0, 1.0) 1
```

Key points:
- **`probe (x, y) rgba(...) N`** — compares the pixel at `(x, y)` to the expected
  RGBA value with a tolerance of `N` ULPs per channel (1 ULP in UNORM8 = 1/255).
- **`[pixel shader d3dbc-hex]`** — raw D3DBC bytecode as hex; used when the HLSL
  frontend cannot exercise a specific opcode directly.
- **`[require]`** — the runner skips the test if the backend does not meet the
  constraint (shader model, texture format, etc.).

---

## 3. Oracle Generation

Shader and rendering oracle values feed the test corpus. API conformance tests use
Wine's D3D9 tests as behavioural references. Neither category is regenerated
automatically — updating an oracle value or expected HRESULT requires code review.

### 3a. vkd3d `shader_runner_d3d9` (SM2/SM3)

Run the `.shader_test` file on a Windows host to produce reference probe values:

```sh
# On Windows (hardware or WARP):
shader_runner_d3d9.exe tests/shader_tests/sincos.shader_test
# When a probe fails, the runner prints the actual value — copy it as the expected.
```

### 3b. Wine `visual.c` (ps_1_x, FFP, corner cases)

`dlls/d3d9/tests/visual.c` (Wine, LGPL-2.1) contains hardcoded expected D3DCOLOR
values verified against real D3D9 hardware. To port a test:

1. Locate the Wine test function (e.g., `fog_test`, `alpha_test`, `ps_1_4_test`)
2. Extract the inline shader assembly string and expected `D3DCOLOR` values
3. Convert `D3DCOLOR` (0xAARRGGBB) to float RGBA for the `probe` line:
   ```
   D3DCOLOR 0xff804020  →  probe (x, y) rgba(0.502, 0.251, 0.125, 1.0) 2
   ```
4. Add citation comment: `// derived from Wine: visual.c:<function_name>`

**Oracle sources by category:**

| Category | Oracle source |
|---|---|
| SM2/SM3 arithmetic, texture, flow control | `shader_runner_d3d9` (Windows/WARP) |
| ps_1_1, ps_1_4 | Wine `visual.c` hardcoded colors |
| Fixed-function lighting, fog, texop | `shader_runner_d3d9` or `visual.c` |
| Half-pixel offset, winding order | Math derivation (exact) |
| COM lifetime, D3D9Ex QI, reset, stateblock | Wine `device.c`, `d3d9ex.c`, `stateblock.c` expected HRESULT/refcount behaviour |

---

## 4. `shader_runner_dxmt9` Backend

The backend must implement the vkd3d `struct shader_runner_ops` interface:

```c
struct shader_runner_ops {
    bool (*check_requirements)(struct shader_runner *, const struct shader_test_requirement *);
    bool (*compile_shader)(struct shader_runner *, const struct shader_bytecode *, ...);
    bool (*draw)(struct shader_runner *);
    void (*probe_pixel)(struct shader_runner *, unsigned x, unsigned y,
                        const struct vec4 *expected, unsigned tolerance);
    void (*destroy)(struct shader_runner *);
};
```

Internal flow:

```mermaid
sequenceDiagram
    participant R as shader_runner (framework)
    participant B as shader_runner_dxmt9

    R->>B: compile_shader(d3dbc bytecode, vs/ps)
    B->>B: dxmt9_winemetal_compile_shader() → ShaderBlob handle

    R->>B: draw(uniforms, textures, quad)
    B->>B: BackendDevice::submitDraw(DrawDesc{...})
    B->>B: BackendDevice::flush()

    R->>B: probe_pixel(x, y, expected, tolerance)
    B->>B: BackendDevice::readbackPixels() → staging
    B->>B: compare pixel[x,y] vs expected ± tolerance
    B-->>R: pass / fail + actual value
```

The backend creates a 64×64 RGBA8 render target for all tests. The draw quad
generates UVs from `[0,1]×[0,1]` across the full target.

---

## 5. Comparison Criteria

| Category | Method | Tolerance |
|---|---|---|
| Shader arithmetic | ULP per channel (UNORM8) | 1 ULP (≤ 1/255) |
| Fixed-function lighting | ULP per channel | 2 ULP |
| Transcendentals (SINCOS, LOG, EXP) | ULP per channel | 2 ULP |
| Texture sampling (bilinear) | ULP per channel | 2 ULP |
| Rasterization coverage | Exact pixel mask | 0 |
| Depth buffer | Float absolute | ≤ 1e-5 |

These tolerances match those used in the vkd3d `shader_runner` D3D9 backend and
Wine `visual.c` (`max_diff = 1` or `2` per channel).

---

## 6. Opcode Coverage Workflow

For each missing opcode in `translateSpirvToMsl()`:

1. Write a `.shader_test` in `tests/shader_tests/<opcode>.shader_test`
2. Run on `shader_runner_d3d9` (Windows) to generate oracle `probe` values
3. Commit the `.shader_test` with the oracle values — test is now **failing** on dxmt9
4. Implement the opcode in `translateSpirvToMsl()`
5. Run `meson test` — test must pass before the opcode is marked ✅ in `gap.md`

---

## 7. Fixed-Function Coverage Matrix

Each cell is one `.shader_test` (or `visual.c`-derived test). Updated as tests pass.

| Feature | vs_ffp | ps_ffp |
|---|---|---|
| No lighting, no texture | ✗ | ✗ |
| Directional light, diffuse | ✗ | — |
| Point light + attenuation | ✗ | — |
| Spot light | ✗ | — |
| 8 mixed lights | ✗ | — |
| Specular | ✗ | — |
| Fog LINEAR vertex | ✗ | ✗ |
| Fog EXP pixel | ✗ | ✗ |
| TEXOP MODULATE | — | ✗ |
| TEXOP ADD | — | ✗ |
| TEXOP ADDSIGNED | — | ✗ |
| TEXOP DOTPRODUCT3 | — | ✗ |
| TEXOP BUMPENVMAP | — | ✗ |
| Alpha test LESS | — | ✗ |
| Alpha test GREATEREQUAL | — | ✗ |
| TexCoordGen SPHEREMAP | ✗ | — |
| Texture transform PROJECTED | ✗ | ✗ |

(✗ = not yet passing; — = not applicable to that stage)

---

## 8. WSI Integration Test

The WSI test (`tests/wsi_present/`) is architecturally distinct from the rest
of the test suite: it is a cross-compiled Win32 PE executable that exercises
the full presentation stack end-to-end under Wine, including:

- `d3d9.dll` PE wrapper (COM entry points)
- `winemetal.dll` PE bridge/service module (Wine-visible thunk layer)
- `winemetal.so` unix-side Metal backend
- Legacy `macdrv_get_cocoa_view` or Heroic `macdrv_functions` HWND→Cocoa resolution
- `WineWindow -> contentView -> WineMetalView -> CAMetalLayer` fallback on Heroic Wine 11.5
- Metal `nextDrawable` + `presentDrawable` on a real `CAMetalLayer`

```mermaid
sequenceDiagram
    participant App as wsi_present_x64.exe
    participant D3D as d3d9.dll (PE)
    participant Bridge as winemetal.dll (PE bridge)
    participant Unix as winemetal.so
    participant Wine as winemac.drv

    App->>D3D: Direct3DCreate9()
    D3D->>Bridge: dxmt9c_factory_create()
    Bridge->>Unix: wine unix-call
    App->>D3D: CreateDevice(hwnd, ...)
    D3D->>Bridge: dxmt9c_factory_create_device(hwnd, ...)
    Bridge->>Unix: wine unix-call
    App->>D3D: Present()
    D3D->>Bridge: dxmt9c_device_present()
    Bridge->>Unix: wine unix-call
    Note over Unix: encodePresent: lookupLayerHandle(hwnd) → nil
    Unix->>Wine: resolve `macdrv_get_cocoa_view` or `macdrv_functions`
    Wine-->>Unix: NSView* or WineWindow*
    Note over Unix: WineWindow -> contentView -> createMetalView -> getMetalLayer
    Unix->>Unix: [layer nextDrawable] + present
```

### Build

```sh
PATH=~/llvm-mingw/bin:$PATH
x86_64-w64-mingw32-clang++ -o tests/wsi_present/wsi_present_x64.exe \
    tests/wsi_present/main.cpp -ld3d9 -luser32 -lgdi32
```

The resulting `wsi_present_x64.exe` is checked in as a pre-built binary so the
test can be run without a separate cross-compile step on the common
Rosetta/Wine64 path.

### Run

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

cp build-win32-x64-builtin/src/win32/d3d9.dll ~/.wine/drive_c/windows/system32/d3d9.dll
cp build-win32-x64-builtin/src/winemetal/winemetal.dll \
  <wine-root>/lib/wine/x86_64-windows/winemetal.dll
cp build-x86_64-builtin/src/winemetal/unix/winemetal.so \
  <wine-root>/lib/wine/x86_64-unix/winemetal.so
WINEDLLOVERRIDES="d3d9=n,b" wine tests/wsi_present/wsi_present_x64.exe
```

Requires a recent Wine64-capable build on macOS plus a Wine toolchain install
tree for the builtin bridge build. Heroic Wine 11.5 is the currently verified
host. The test is **not** part of `meson test` because it cannot run without
Wine.

### What is NOT tested by this path

| Scenario | Covered by |
|---|---|
| Metal shader correctness | `dxmt9-shader-corpus` |
| Device state, draw calls | `dxmt9-core-spec` |
| Present with null window (no WSI) | `dxmt9-core-spec` R-TEST-6.1 |
| x86_64 PE DLL loading | `wsi_present_x64.exe` under Wine64 |
| PE bridge ↔ unix module dispatch | `wsi_present_x64.exe` under Wine64 |
| WSI resolution (`macdrv_get_cocoa_view` or `macdrv_functions`) | `wsi_present_x64.exe` under Wine |
| Visual frame output | Manual observation |

---

## 9. Wine D3D9 Conformance Harness

The conformance harness is a set of small PE executables compiled with
llvm-mingw and run under Wine. Unlike `dxmt9-core-spec`, these tests exercise
the real exported `d3d9.dll` ABI, COM vtables, Wine DLL search behaviour, and
the PE bridge path.

```mermaid
sequenceDiagram
    participant Test as d3d9_conformance_*.exe
    participant D3D as d3d9.dll
    participant Bridge as winemetal.dll
    participant Unix as winemetal.so

    Test->>D3D: Direct3DCreate9 / Direct3DCreate9Ex
    Test->>D3D: COM calls under test
    D3D->>Bridge: coarse factory/device/resource calls
    Bridge->>Unix: Wine unix-call
    Unix-->>Bridge: HRESULTs / handles / present status
    D3D-->>Test: HRESULTs, COM refs, returned interfaces
```

### Porting Rules

- Keep each executable focused: `factory_ex`, `device_lifetime`,
  `stateblock`, and `reset_lost` are separate test programs.
- Copy only the minimal test logic needed from Wine; do not vendor whole Wine
  source files.
- Preserve expected HRESULTs, refcount probes, and comments explaining unusual
  D3D9 quirks.
- Add a provenance header near each ported test:

```c
/* [provenance]
 * source: wine/dlls/d3d9/tests/d3d9ex.c:test_qi_base_to_ex
 * upstream-commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 * oracle: Wine D3D9 test behaviour, validated against native D3D9
 */
```

### Required Executables

| Executable | Wine source anchors | Primary checks |
|---|---|---|
| `d3d9_factory_ex_x64.exe` | `d3d9ex.c` QI/display/LUID tests | base vs Ex QI, Ex-created normal devices, display-mode filters |
| `d3d9_device_lifetime_x64.exe` | `device.c` refcount/private-data/scene tests | public COM refcounts, `Get*` AddRef, scene transitions, private data |
| `d3d9_stateblock_x64.exe` | `stateblock.c` create/capture/apply tests | `D3DSBT_*` masks, recording invalid calls, apply/capture quirks |
| `d3d9_reset_lost_x64.exe` | `device.c` / `d3d9ex.c` reset tests | base vs Ex cooperative-level behaviour, default-pool invalidation |

The same source layout may also build x86 PE binaries when the configured Wine
runtime supports WoW64.

### Run

```sh
# App-local lane example.
mkdir -p build/conformance-stage
cp build-win32-x64/src/win32/d3d9.dll build/conformance-stage/
cp build-win32-x64/src/winemetal/winemetal.dll build/conformance-stage/
cp build-x86_64/src/winemetal/unix/winemetal.so build/conformance-stage/
cp tests/d3d9_conformance/d3d9_*_x64.exe build/conformance-stage/
(cd build/conformance-stage && wine d3d9_factory_ex_x64.exe)
```

For the builtin lane, install `d3d9.dll`, `winemetal.dll`, and `winemetal.so`
with the deployment helper, then run the same PE executables with
`WINEDLLOVERRIDES="d3d9=n,b"` as described in the deployment spec.

---

## 10. File Layout

```
tests/
├── smoke.cpp                     Bootstrap sanity test
├── core_spec.cpp                 Core API tests
├── d3d9_conformance/
│   ├── factory_ex.cpp            Wine d3d9ex.c-derived factory/Ex checks
│   ├── device_lifetime.cpp       Wine device.c-derived refcount/private-data/scene checks
│   ├── stateblock.cpp            Wine stateblock.c-derived stateblock checks
│   └── reset_lost.cpp            Wine device.c/d3d9ex.c-derived reset checks
├── wsi_present/
│   ├── main.cpp                  WSI integration test source (R-TEST-11.1)
│   ├── wsi_present.exe           Historical ARM64 PE smoke binary
│   └── wsi_present_x64.exe       Pre-built x86_64 PE smoke binary (validated)
├── shader_tests/
│   ├── MANIFEST.toml             Machine-readable corpus index (R-TEST-10.1)
│   ├── ...                       vkd3d-format .shader_test files (each with provenance block)
│   ├── arithmetic/
│   │   ├── mov.shader_test
│   │   ├── mad.shader_test
│   │   ├── dp3_dp4.shader_test
│   │   └── ...
│   ├── flow_control/
│   │   ├── if_else.shader_test
│   │   ├── rep_endrep.shader_test
│   │   ├── loop.shader_test
│   │   └── call_ret.shader_test
│   ├── transcendental/
│   │   ├── sincos.shader_test
│   │   ├── log_exp.shader_test
│   │   └── ...
│   ├── texture/
│   │   ├── tex_2d.shader_test
│   │   ├── texldd.shader_test
│   │   └── ...
│   ├── matrix/
│   │   ├── m4x4.shader_test
│   │   └── ...
│   ├── ffp/                      Fixed-function (shader_runner-based)
│   │   ├── lighting_directional.shader_test
│   │   ├── fog_linear.shader_test
│   │   └── ...
│   └── visual_c/                 Ported from Wine visual.c (ps_1_x + FFP)
│       ├── ps_1_4_test.shader_test     ; // derived from Wine: visual.c:ps_1_4_test
│       ├── fog_test.shader_test        ; // derived from Wine: visual.c:fog_test
│       ├── alpha_test.shader_test      ; // derived from Wine: visual.c:alpha_test
│       └── ...
├── shader_runner_dxmt9.cpp       dxmt9 backend for shader_runner
└── meson.build
scripts/
├── check_manifest.sh             Fails if MANIFEST.toml ↔ filesystem diverge
├── check_drift.sh                Reports .shader_test files behind upstream commit
└── sync_corpus.sh                Refreshes vkd3d-sourced files from a local checkout
```

The `shader_runner_dxmt9` binary is built as a Meson test target and run with each
`.shader_test` file as a separate test case. Each `meson test` invocation runs the
full corpus.

---

## 11. Provenance Block

Every `.shader_test` file opens with a provenance block. The block is pure comments
(`;` prefix) so the vkd3d parser ignores it.

**vkd3d-sourced test:**

```
; [provenance]
; source: vkd3d
; upstream-url: https://gitlab.winehq.org/wine/vkd3d
; upstream-commit: 5a47802
; oracle: shader_runner_d3d9
; oracle-env: Windows 11 / WARP
; oracle-date: 2026-03-29

[require]
shader model >= ps_2_0

[pixel shader]
...
```

**Wine visual.c port:**

```
; [provenance]
; source: wine/visual.c:fog_test
; upstream-url: https://github.com/wine-mirror/wine
; upstream-commit: d3a9f12
; oracle: real D3D9 hardware (recorded in Wine test history)
; oracle-date: 2026-03-29

[require]
shader model >= ps_1_1
...
```

**Math-derived test:**

```
; [provenance]
; source: dxmt9
; oracle: math-derivation
; oracle-date: 2026-03-29
```

The `upstream-commit` field enables drift detection: a script can compare each
file's recorded commit against the current vkd3d or Wine HEAD and flag files that
are behind.

---

## 12. Manifest

`tests/shader_tests/MANIFEST.toml` is the machine-readable index of the corpus.
Rows for upstream-sourced tests may also carry `upstream-commit` so the sync tool
can keep provenance and manifest state aligned.

### Format

```toml
# MANIFEST.toml — generated and hand-maintained
# Run `scripts/check_manifest.sh` to verify it matches the filesystem.

[[test]]
file    = "arithmetic/mad.shader_test"
source  = "vkd3d"
models  = ["ps_2_0", "vs_2_0", "ps_3_0", "vs_3_0"]
opcodes = ["MAD"]
status  = "passing"

[[test]]
file    = "flow_control/if_else.shader_test"
source  = "vkd3d"
models  = ["ps_2_0", "ps_3_0"]
opcodes = ["IF", "ELSE", "ENDIF"]
status  = "failing"          # implementation pending

[[test]]
file    = "visual_c/fog_test.shader_test"
source  = "wine/visual.c:fog_test"
models  = ["ps_1_1"]
opcodes = ["TEX", "MUL", "ADD"]
status  = "passing"
```

### Enforcement

A Meson custom target `dxmt9-manifest-check` runs `scripts/check_manifest.sh`
before the test suite:

```sh
# scripts/check_manifest.sh
# Fails if any .shader_test file is missing from MANIFEST.toml
# or if MANIFEST.toml lists a file that does not exist.
find tests/shader_tests -name "*.shader_test" | sort > /tmp/actual.txt
tomlq -r '.test[].file' tests/shader_tests/MANIFEST.toml | sort > /tmp/manifest.txt
diff /tmp/actual.txt /tmp/manifest.txt
```

The sync workflow is separate:

```sh
# Refresh vkd3d-sourced tests from a local upstream checkout.
DXMT_UPSTREAM_ROOT=/path/to/vkd3d bash scripts/sync_corpus.sh

# Report which tracked vkd3d files are behind that checkout.
DXMT_UPSTREAM_ROOT=/path/to/vkd3d bash scripts/check_drift.sh
```

### Queries

```sh
# Opcodes with no passing test (coverage gap):
tomlq -r '.test[] | select(.status != "passing") | .opcodes[]' MANIFEST.toml | sort -u

# Tests behind upstream vkd3d HEAD:
# (compare recorded provenance commits against the configured checkout)
DXMT_UPSTREAM_ROOT=/path/to/vkd3d scripts/check_drift.sh

# Count by shader model:
tomlq -r '.test[] | .models[]' MANIFEST.toml | sort | uniq -c
```
