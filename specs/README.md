# Specifications

This directory contains specifications for dxmt9 — a Wine-hosted D3D9-to-Metal
translation layer. Specs describe *what* the system must be and do, not how to
build it.

---

## Terminology

These specs use three separate compatibility scopes:

- **DXMT-compatible architecture**: the implementation and deployment shape
  follows upstream DXMT's split (`d3d9.dll` -> `winemetal.dll` ->
  `winemetal.so`), bridge ABI style, and chunked command-submission model. This
  does not mean copying Wine's `dlls/d3d9` or wined3d internal architecture.
- **Wine runtime-compatible**: binaries load and run correctly inside Wine,
  including builtin PE DLL handling, normal/native DLL search, unixlib provider
  discovery, and target Wine runtime dependencies.
- **Windows D3D9-compatible behaviour, validated by Wine D3D9 tests**:
  public API results such as `HRESULT`s, refcounts, reset/lost-device state,
  export tables, private data, and D3D9Ex edge cases match Windows-observed
  behaviour. Wine's D3D9 tests are used as a behavioural oracle; they are not an
  implementation-structure requirement.
- **Data-oriented transform design**: DXMT-compatible ownership is preserved, but
  shader, state, draw, format, and resource-retention conversion paths should be
  explicit value-to-value transforms wherever possible. These transforms are the
  primary unit-test target; runtime Metal/readback tests are reserved for
  GPU-visible behaviour that source or descriptor inspection cannot prove.

---

## Structure

```
specs/
├── gap.md                  Spec–implementation gap tracker (what is / isn't built yet)
├── core/                   D3D9 API frontend for Wine-hosted applications
│   ├── requirements.md     D3D9 COM contracts, state machine rules, resource semantics
│   ├── design.md           COM object model, device state, core/backend boundary
│   ├── formats.md          D3DFMT → MTLPixelFormat mapping tables
│   ├── caps.md             D3DCAPS9 advertised values
│   ├── wsi.md              HWND → CAMetalLayer window integration
│   └── queries.md          GPU query design in deferred pipeline
├── d3d8/                   D3D8 shim layer (d3d8.dll → D3D9)
│   ├── requirements.md     Shader handle table, decl parser, API differences vs D3D9
│   └── design.md           Object model, handle table layout, TSS remapping
├── d3d7/                   D3D7 / DirectDraw 7 layer (ddraw.dll → D3D9)
│   ├── requirements.md     IDirectDraw7, IDirect3D7, IDirect3DDevice7, surface classification
│   └── design.md           DDSurface7 object model, RS/transform mapping, mip chain
├── backend/                Metal translation layer
│   ├── requirements.md     Translation correctness, command encoding, PSO cache
│   ├── design.md           Command queue, encoder lifecycle, resource allocation
│   └── surface-ops.md      UpdateSurface, StretchRect, ColorFill, GetRenderTargetData
├── deploy/                 Wine runtime and application-local packaging
│   ├── requirements.md     Runtime install, app-local DLL override, provider lookup
│   └── design.md           Artifact matrix, provider locator, packaging manifest
├── verification/           Formal verification (TLA+ model checking)
│   ├── requirements.md     What must be formally proven and why
│   ├── design.md           TLA+ approach, C++ binding, how to run TLC
│   └── tla/                TLA+ modules (checked with TLC model checker)
│       ├── CommandQueue.tla         3-thread ring buffer
│       ├── ResourceLifetime.tla     Deferred GPU resource destruction
│       ├── EncoderLifecycle.tla     MTLCommandEncoder state machine
│       └── QuerySeqId.tla           D3D9 query seq-ID fence
├── tests/                  Controlled correctness tests (oracle-based, pixel-exact)
│   ├── requirements.md     shader_runner corpus, Wine-derived D3D9 oracles, provenance, manifest
│   ├── design.md           shader_runner_dxmt9 backend, .shader_test format, MANIFEST.toml
│   └── dod.md              Test-area DoD inventory, evidence, remaining gaps, next criteria
├── experiments/            Wild integration tests (real D3D9 applications, fuzzy pass criteria)
│   ├── requirements.md     Catalogue, pass criteria, screenshot comparison
│   └── design.md           Launcher injection, SSIM comparison, failure triage
└── benchmarks/             Performance measurement and regression tracking
    ├── requirements.md     Workloads, reference stacks, regression policy
    └── design.md           Harness, timing, comparison script, baseline format
```

---

## Dependencies

### Reading specs

No tooling required — all specs are Markdown and TLA+.
Mermaid diagrams in Markdown render in GitHub, VS Code (Mermaid extension),
and the TLA+ Toolbox.

### Checking TLA+ specs

The `.tla` files in `specs/verification/tla/` are checked with **TLC**,
the model checker bundled with the TLA+ tools.

**Install (macOS):**

```sh
brew install tla-tools
```

**Install (any platform — Java required):**

```sh
curl -L -o tla2tools.jar \
  https://github.com/tlaplus/tlaplus/releases/latest/download/tla2tools.jar
```

**Run all specs:**

```sh
bash scripts/verify_tla.sh
```

**GUI alternative:** [TLA+ Toolbox](https://github.com/tlaplus/tlaplus/releases) —
open a `.tla` file, create a model using the values in the matching `.cfg`,
click **Run TLC**.

**Expected result:** `No error has been found.` for all four modules.

### TLA+ version

TLC 2.17 or later (ships with tla-tools 1.8+). Earlier versions may not
support all temporal property syntax used in the specs.
