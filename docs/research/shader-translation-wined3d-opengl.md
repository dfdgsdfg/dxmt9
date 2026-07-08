# Wine wined3d D3D9 → OpenGL Shader Translation Research Notes

Sources: Wine source tree (`dlls/wined3d/`, `dlls/d3d9/`), WineHQ developer
documentation, OpenGL 3.x / 4.x specifications, GL_ARB_get_program_binary.

Sibling research notes: `shader-translation-asahi-agx.md` (Apple AGX native
model), `shader-translation-vkd3d-moltenvk.md` (SPIR-V detour for Metal),
`wine-d3d9.md` (Wine present / buffering path).

---

## Scope

This note documents how Wine's `wined3d.dll` translates **D3D9 shader bytecode
and fixed-function state into OpenGL** (GLSL or legacy ARB programs), and what
dxmt9 can learn from that translation without copying it.

The useful boundary is narrow:

- wined3d is the oldest production-grade D3D9 reimplementation; its shader
  translator has been hardened against thousands of titles across ~15 years.
- wined3d is LGPL. dxmt9 project code is MIT-compatible. Wine source is a
  **behavioural and architectural reference only** — implementation must
  remain clean-room. `agents/rules/codebase_conventions.rules.md` lists Wine
  D3D9 tests as a behavioural oracle and explicitly forbids importing Wine
  implementation source.
- wined3d targets OpenGL (and a developing Vulkan backend), not Metal. The
  GLSL output and binding model do not map directly to MSL.
- dxmt9's value-add is *not* re-implementing wined3d's translator; it is
  translating to MSL with Apple-Silicon-specific concerns (TVB/PB
  pressure, post-transform cache locality, half precision) that wined3d
  does not need to think about.

dxmt9 already runs Wine `d3d9/tests/{device,visual,d3d9ex,stateblock}.c`
binaries as PE conformance evidence (see `specs/tests/`). Treat this note as
the architectural counterpart: when a Wine test passes against dxmt9, this
note explains *what wined3d's translator did internally* for the same
shader, so divergences are easier to diagnose.

---

## High-Level Stack

```mermaid
flowchart LR
    App["D3D9 application"] --> D3D9["wine d3d9.dll\n(thin shim)"]
    D3D9 --> W3D["wined3d.dll"]
    W3D --> CS["wined3d command stream\nworker thread"]
    CS --> Pick{"backend\nselected"}
    Pick -->|GL Core (default)| GL["GLSL backend\nglsl_shader.c"]
    Pick -->|legacy ARB| ARB["ARB program backend\narb_program_shader.c"]
    Pick -->|Vulkan (in dev)| VK["Vulkan backend\n(SPIR-V via vkd3d-shader)"]
    GL --> Driver["host GL driver"]
    ARB --> Driver
    VK --> VKD["host Vulkan driver"]

    Bytecode["D3DBC SM 1.x / 2.x / 3.x"] --> Decode["shader_sm1.c\nshader_sm4.c"]
    Decode --> W3DIR["wined3d shader IR\n(struct wined3d_shader_instruction)"]
    W3DIR --> Pick

    FFPState["D3DRS / SetTextureStageState /\nFVF / SetTransform"] --> FFPKey["FFP key derivation\nin wined3d"]
    FFPKey --> FFPGen["runtime-generated GLSL\nffp_gl.c"]
    FFPGen --> GL
```

Two pipelines feed the GLSL backend: programmable shaders go through the
bytecode decoder and the wined3d shader IR; fixed-function state is hashed
into an FFP key and a GLSL shader is generated on demand.

---

## Wine D3D9 Shader Pipeline

### D3DBC Decode

`dlls/wined3d/shader_sm1.c` parses Direct3D shader model 1.x, 2.x, and 3.x
bytecode tokens into wined3d's internal instruction stream. (`shader_sm4.c`
handles SM 4.x / 5.x for D3D10+ and is not used on the D3D9 path.)

The decoder normalises:

- opcode + operand tokens into a uniform `wined3d_shader_instruction`
  record;
- relative addressing of constant registers;
- `_pp` (partial precision) hints, predicate registers, loop counters;
- `dcl_sampler` declarations into a per-sampler texture-type table;
- semantic mappings (`vs_3_0` explicit semantics, fixed semantics for
  earlier shader models).

The decoded IR is consumed by every backend (GLSL, ARB, and the developing
Vulkan path). This is the same architectural pattern dxmt9 uses: one
decoded IR, multiple emit targets.

### wined3d Shader IR

The IR is a value type: instruction array + per-shader metadata
(declarations, semantic table, used-constant range, sampler table, version,
flags). It is not SSA or CFG-based; analysis is forward sweeps over the
instruction array, the same shape as dxmt9's `SpirvModule` /
`D3DDecodedInstruction` vector (`specs/d3d9/shader/spec.md` §2).

What wined3d records that is worth noting:

- per-shader **constant-usage map** (which `c#` ranges are live);
- per-sampler **texture type** (`2D`, `CUBE`, `VOLUME`) gated through
  `tex2D` / `texCUBE` opcode decoding;
- a `wined3d_shader_signature` for input / output semantics across SM 3
  generic semantic strings;
- a per-shader **flag set** that records features the backend must
  compensate for (e.g. uses `vFace`, uses `dst_modifier_centroid`).

This metadata, not the raw token stream, is what drives shader-variant
keying.

### GLSL Backend

`dlls/wined3d/glsl_shader.c` is the main GLSL emitter. Notable patterns:

- per-shader-variant **compilation context** built from the live state
  (programmable VS + paired PS, FFP state, render-state bits the shader
  must compensate for);
- one **GLSL program object per (VS, PS, context-key) combination**;
- a **per-program uniform layout** the backend pushes per-draw (constants,
  position fixup, alpha-ref, fog, sampler texture types, color clamping
  ranges);
- emission threads register lifetimes through stable temporary names
  (`R0..Rn`, `T0..Tn`) so the GLSL compiler sees clean dataflow;
- output variables are emitted into a `VS_OUT { ... } _vs_out;` block to
  bridge vertex outputs to pixel inputs through interface-block matching.

The emitter walks the wined3d IR once and produces GLSL text. There is no
intermediate SPIR-V or SSA form; the emitter is a single pass with
contextual lookups (sampler types, constant usage) into the IR's metadata.

### ARB Legacy Backend

`dlls/wined3d/arb_program_shader.c` emits `GL_ARB_vertex_program` /
`GL_ARB_fragment_program` assembly for GPUs / drivers without GLSL
support. It is now mostly historical (default since OpenGL Core profile)
but kept for low-end paths.

Worth noting only as an architectural data point: the same wined3d IR
feeds both backends. The dispatch picks an emitter per device capability,
not per shader.

### FFP Shader Generation

`dlls/wined3d/ffp_gl.c` (with helpers in `state.c`, `glsl_shader.c`) hashes
the fixed-function state into an **FFP key** and generates a GLSL program
that emulates D3D9 fixed function on the programmable pipeline. The key
covers:

- lighting enable, per-light enable + type, material colour sources;
- vertex blend mode, indexed vertex blend;
- per-stage texture coordinate generation (TCI flags), texture transform
  flags;
- per-stage colour / alpha op + arg sources (`D3DTSS_COLOROP`,
  `D3DTSS_COLORARG{1,2}`, etc.);
- alpha test enable + function;
- fog mode + vertex/pixel fog selection.

The FFP key set in wined3d is the *same conceptual key set* that dxmt9
documents at `specs/d3d9/shader/spec.md` §3.1. The actual bit-packing
and field names differ.

---

## Stage Translation Specifics

### Position Output, NDC, and the Y Flip

OpenGL clip space matches D3D clip space on the Y axis (both have `+Y`
up). Wined3d still injects a **position fixup**, because:

- D3D pixel centres are at integer coordinates; GL pixel centres are at
  half-integers. The half-pixel offset must be added before clip-space
  conversion when emulating older D3D apps that expect integer centres.
- D3D z is `[0, 1]`; GL clip-space z is historically `[-1, 1]` (NDC). For
  most workloads wined3d emits a `z = z * 2 - 1` rewrite, unless
  `GL_ARB_clip_control` is available and `glClipControl(GL_LOWER_LEFT,
  GL_ZERO_TO_ONE)` is set — then the rewrite is omitted.
- `D3DFVF_XYZRHW` (pre-transformed) inputs reach the VS in screen space
  with `1/w`; the VS converts them to clip space using viewport
  dimensions supplied via a uniform.

For dxmt9 the equivalent decisions live in `specs/d3d9/shader/spec.md`
§3.2 (half-pixel) and §3.3 (V-axis policy). Both projects share the same
structural decision: own the position fixup in the translator, don't
hide it in the application's shader code.

### Color Clamp For SM 1.x

SM 1.x specifies pixel-shader output clamping to `[0, 1]`. GLSL does not
clamp by default. Wined3d emits an explicit `clamp(out, 0.0, 1.0)` at the
end of SM 1.x pixel shaders. The clamp is gated on the shader's bytecode
version so SM 2 / 3 shaders are not penalised.

dxmt9's translator must apply the same clamp for SM 1.x correctness; MSL
also does not clamp by default. The clamp is a translator obligation,
not a render-state setting.

### Alpha Test

GL Core profile (3.2+) removed fixed-function alpha test. Wined3d
implements `D3DRS_ALPHATESTENABLE` by emitting a `discard;` conditional
at the end of the pixel shader, gated on
`(alphaTestEnable, alphaTestFunc)`. Each distinct enable / function
combination produces a separately-keyed shader variant.

dxmt9 does the same thing in MSL (`discard_fragment()` instead of
`discard;`). See `specs/d3d9/shader/spec.md` §3.4.

### Fog

D3D9 has both vertex-evaluated and pixel-evaluated fog. Wined3d's GLSL
emitter:

- moves vertex-evaluated fog math into the VS and outputs a `fogFactor`
  varying;
- moves pixel-evaluated fog into the PS, sampling `fogFactor` from the
  interpolated VS output;
- handles fog colour and fog start/end through uniforms.

The FFP key bits `fogMode`, `fogFromVertex`, and `rangeFog` are mirrored
in dxmt9's FFP key (`spec.md` §3.1).

### Constants and Uniforms

D3D9 has separate float, integer, and bool constant register files. GL
has uniforms. Wined3d:

- packs `c0..cN` into a GLSL `uniform vec4 vs_c[NUM];` array;
- updates only the dirty range via `glUniform4fv` calls (range tracking
  in `dlls/wined3d/state.c`);
- handles `i0..iN` (int4) and `b0..bN` (bool) as smaller uniform arrays.

Wined3d historically used naive per-call uniform pushes; later versions
moved hot constants into uniform buffer objects (UBOs) backed by
persistent-mapped `GL_ARB_buffer_storage` allocations to reduce
`glUniform*` driver cost.

dxmt9 evolved past this point: per-frequency constants live in the
per-frequency uniform split (`specs/backend/draw-uniforms/`) and are
pushed via `setVertexBytes` / argbuf hybrid. The pattern is parallel —
the dxmt9 split is just a Metal-native version of the same observation
wined3d eventually made.

### Texture Stages and Samplers

D3D9 has 8 sampler stages with 8 sampler states (filter, address mode,
mipmap LOD bias, max anisotropy, border colour, sRGB, max/min mip
level). GL has separate sampler objects (`GL_ARB_sampler_objects`,
core in 3.3) and texture objects.

Wined3d binds a texture and a sampler object per stage. The GLSL emitter
declares `uniform sampler2D ps_sampler0;` (or `samplerCube`, `sampler3D`
based on the IR's sampler type table). Sampler-state changes flip the
bound sampler object, not the GLSL program.

This separation is one wined3d architectural strength: sampler-state
changes do not invalidate the shader cache. dxmt9 needs the same
property (see argbuf resource array lane in
`specs/backend/render-pass-actions/` and the
`MTLSamplerDescriptor.supportArgumentBuffers` discussion in
`agents/rules/test_wild.rules.md` open issues).

### Vertex Declaration and Stream Sources

`IDirect3DVertexDeclaration9` is translated to a GL **vertex array
object** (VAO) plus `glVertexAttribPointer` / `glVertexAttribFormat`
calls. Multiple streams map to multiple VBO bindings; `SetStreamSourceFreq`
maps to instanced rendering with `glVertexAttribDivisor`.

`D3DFVF_*` is decoded into the same vertex declaration shape before
binding.

The wined3d pattern is "build VAO once per declaration + bound-stream
combination, cache by key". This is the same shape dxmt9 uses for
`PipelineState` keying on vertex declaration + stream layout.

---

## Backend Architecture

### Command Stream Thread

`dlls/wined3d/cs.c` runs every backend op (state apply, draw, present)
on a worker thread. The D3D9 caller thread fills a command-stream packet
buffer and signals the worker. This is the architectural ancestor of
DXVK's CS thread and of dxmt9's `CommandChunk` / unix importer split.

The shader translator runs **on the CS thread**, not the caller thread.
That keeps the application thread free of GLSL compile latency.

### State-Key Shader Variants

Wined3d's hot-path observation, repeated for fifteen years: **the
emitted shader depends on more than the bytecode**. Render-state bits
the shader must compensate for (SM 1.x colour clamp, alpha-test enable /
function, point sprite enable, fog vertex/pixel mode, sampler texture
types, sRGB write) are folded into a shader-compile key. Wined3d caches
one compiled GLSL program per (`VS hash`, `PS hash`, `state key`)
combination.

dxmt9 inherits this observation in `specs/d3d9/shader/requirements.md`
§6: the cache key is composed from IR hash + ShaderSourceContext + every
enabled-pass plan + alpha-test variant key + debug toggles. The
underlying lesson is the same: state that the shader compiles
*differently* for must be part of the key, even when it is not in the
bytecode.

### No PSO in GL

OpenGL has no single pipeline state object. Bindings are spread across
program, VAO, sampler objects, FBO, depth/stencil state, blend state,
viewport, and scissor — each set independently and validated lazily by
the driver. Wined3d tracks every state bit, only re-emits the GL calls
that changed since the last draw, and inserts implicit fixups
(framebuffer completeness checks, sRGB framebuffer flag, etc.).

This is the architectural choice dxmt9 explicitly does not follow. Metal
requires a single `MTLRenderPipelineState` per shader-state combination;
dxmt9 keys PSOs on a flat state vector. Wined3d's per-call state
re-binding pattern is what dxmt9's `FlatDrawStateRecord` exists to
collapse.

### Program Cache

`GL_ARB_get_program_binary` (core in OpenGL 4.1) lets wined3d cache
compiled GLSL programs to disk. The first run pays the GLSL compile
cost; subsequent runs `glProgramBinary` straight from the cache file.

The hit rate depends on driver stability — driver updates invalidate the
cache. Wined3d hashes the GL driver version into the on-disk filename so
old binaries are not loaded after a driver upgrade.

dxmt9's `MTLBinaryArchive` plays the same role with similar lifecycle
(`DXMT9_PREWARM`, `DXMT_DISABLE_SHADER_ARCHIVE` env knobs in
`agents/rules/environment_variables.rules.md`). The hash composition
described in `specs/d3d9/shader/spec.md` §7 follows the same logic:
any change to the emitted code must change the key.

---

## What Wine Already Did Well That dxmt9 Should Inherit

| Wined3d pattern | dxmt9 equivalent |
|---|---|
| One decoded IR feeds multiple backends. | dxmt9's `SpirvModule` could similarly serve future targets. |
| State-key shader variants instead of per-call shader patches. | dxmt9 cache key composition in `specs/d3d9/shader/spec.md` §7. |
| FFP key as a value type hashed into the program cache. | `FFPKeyVS` / `FFPKeyPS` in `specs/d3d9/shader/spec.md` §3.1. |
| Translator-injected constant slot for position fixup, alpha ref, fog. | Same pattern in dxmt9 (constant slot for `(1/vpW, 1/vpH)` etc.). |
| CS thread isolates shader compile latency. | dxmt9 unix importer / queue split. |
| Disk-backed program binary cache keyed on translator + driver version. | `MTLBinaryArchive` with version-mixed hash. |

Wine D3D9 tests have been the public oracle for these patterns since
~2005. `dxmt9-d3d9-conformance` already runs the same test binaries
against dxmt9, so the correctness boundary is shared without sharing
implementation code.

---

## What Not To Copy

- **No Wine source**. wined3d is LGPL; dxmt9 implementation code is
  MIT-compatible. Architectural patterns are public knowledge; specific
  source is not.
- **GL-specific binding patterns** (lazy state validation, separate
  sampler objects, separate VAOs and program objects, `glUniform*` per
  uniform). Metal collapses these into PSO + argument buffers; copying
  the GL split into Metal is anti-idiomatic.
- **Wined3d's per-context global-state model**. GL state is per-context;
  Metal state is per-encoder. Treating Metal like a stateful context
  (set this then issue draw) reintroduces the validation cost a PSO is
  supposed to amortise.
- **The ARB legacy backend**. It is historical; do not invest in an
  equivalent.
- **String-based GLSL emission as a final answer**. Wine emits GLSL text
  because the GL driver compiles it. Metal also compiles MSL text, so
  dxmt9 emits text — but this is not the long-term ceiling. If a future
  refactor introduces an internal SSA / typed IR pass (e.g. for the FP16
  precision pass in `specs/d3d9/shader/spec.md` §4), it should be
  driven by Metal-side observability, not by mimicking GLSL emission.

---

## Reference Checklist For dxmt9

When a Wine d3d9 test passes against wined3d's GL backend but fails
against dxmt9 (or vice versa), this checklist names the wined3d source
location that is the most likely architectural counterpart:

| Test failure shape | wined3d file to inspect | dxmt9 file to inspect |
|---|---|---|
| Shader decode mismatch | `shader_sm1.c` | `dxmt9_shader_decoder.cpp` |
| GLSL / MSL emission divergence | `glsl_shader.c` | `dxmt9_shader_metal_ir.cpp` |
| FFP key derivation | `state.c`, `ffp_gl.c` | `dxmt9_ffp_shaders.cpp` |
| Vertex declaration handling | `vertexdeclaration.c`, `context_gl.c` | core D3D9 declaration path + backend PSO build |
| Half-pixel / NDC fixup | `glsl_shader.c::shader_glsl_load_constants` | `dxmt9_shader_metal_ir.cpp` half-pixel inject |
| Alpha test variant | `glsl_shader.c::shader_glsl_generate_pshader` end-of-shader block | translator alpha-test discard emit |
| SM 1.x colour clamp | `glsl_shader.c` SM 1.x clamp insertion | translator output-clamp policy |
| Sampler-state-only change | `sampler.c`, `wined3d_context_gl_apply_sampler_state` | argbuf sampler binding lane |
| Constant push frequency | `cs.c` constant emit + `glsl_shader.c` uniform layout | per-frequency draw uniforms (`specs/backend/draw-uniforms/`) |

This is for *triage*, not for code lifting. Read wined3d's behaviour,
implement dxmt9's response.

---

## Open Questions

- Does wined3d implement any GLSL `precision mediump float` / `lowp`
  policy for SM 1.x? If yes, does it correlate with `_pp` hints, and
  does it produce a measurable register-pressure or varying-packing win
  on any host GL driver? (Relevant to dxmt9's FP16 hypothesis in
  `specs/d3d9/shader/spec.md` §4.)
- How does wined3d handle the SM 1.x texture-stage register file (`t0`,
  `t1`, ...) under SM 1.4 dependent reads, and how does it interact with
  the GL sampler-object binding sweep?
- For `D3DFVF_XYZRHW` plus a programmable VS, does wined3d branch on
  shader version or rewrite the bytecode? (dxmt9 currently relies on the
  translator-injected constant block; the wined3d approach is a useful
  cross-check.)
- The developing Vulkan backend in wined3d uses vkd3d-shader for D3DBC →
  SPIR-V. Does it consume the same wined3d IR or run a parallel decode
  path? If parallel, why?
- Wined3d's GLSL program cache hit-rate after driver upgrade is bounded
  by the GL driver's binary stability. Is there a comparable
  `MTLBinaryArchive` stability story across macOS minor versions worth
  measuring?

---

## References

| Source | Path / link | Notes |
|---|---|---|
| Wine source tree | `~/workspaces/wine/dlls/wined3d/` | clean-room reference only; do not lift code |
| `shader_sm1.c` | `dlls/wined3d/shader_sm1.c` | D3DBC SM 1.x / 2.x / 3.x decoder |
| `glsl_shader.c` | `dlls/wined3d/glsl_shader.c` | GLSL emitter; SM 1.x colour clamp, alpha-test discard, fog, half-pixel |
| `arb_program_shader.c` | `dlls/wined3d/arb_program_shader.c` | legacy ARB assembly emitter |
| `ffp_gl.c` | `dlls/wined3d/ffp_gl.c` | FFP key derivation and runtime GLSL generation |
| `cs.c` | `dlls/wined3d/cs.c` | command-stream worker; shader compile runs here |
| `state.c` | `dlls/wined3d/state.c` | render-state → backend mapping; FFP key bits |
| WineHQ developer docs | https://wiki.winehq.org/D3D | high-level architecture, contributor guidelines |
| OpenGL 4.6 spec | https://registry.khronos.org/OpenGL/specs/gl/glspec46.core.pdf | reference for `glClipControl`, sampler objects, program binaries |
| `GL_ARB_get_program_binary` | https://registry.khronos.org/OpenGL/extensions/ARB/ARB_get_program_binary.txt | program binary cache primitive |
| `GL_ARB_clip_control` | https://registry.khronos.org/OpenGL/extensions/ARB/ARB_clip_control.txt | optional `[0,1]` z range and lower-left origin |
| dxmt9 conformance manifest | `tests/conformance/d3d9/MANIFEST.toml` | Wine d3d9 test ↔ dxmt9 conformance mapping |
