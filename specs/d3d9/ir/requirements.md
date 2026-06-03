# Shader IR and Translator Requirements

The dxmt9 shader translator turns D3D9 vertex and pixel shader bytecode (SM1.x /
SM2.x / SM3.x) into a Metal Shading Language (MSL) translation unit that the
backend can compile, archive, and bind to a Metal pipeline. This spec covers
the **intermediate representation (IR)** that sits between the decoder and the
MSL emitter, the analysis and transformation passes that consume it, the
emission contract, and the precision / VSOut policy that the translator must
honour.

Scope:

- D3D9 programmable VS / PS bytecode decoding and MSL emission.
- Fixed-function (FFP) shader generation, where the IR layer accepts a
  derived `FFPKeyVS` / `FFPKeyPS` instead of a bytecode blob.
- Half-pixel offset injection, alpha-test fixup, `D3DFVF_XYZRHW` NDC fixup,
  and other D3D9 → Metal semantic rewrites that live inside the translator.
- Shader cache hashing and archive integration.

Out of scope (owned by other specs):

- Pipeline state object selection, draw encoding, and per-draw binding —
  `specs/backend/draw-uniforms/` and the rest of `specs/backend/`.
- D3D9 COM-side shader objects (`IDirect3DVertexShader9`,
  `IDirect3DPixelShader9`) — `specs/d3d9/requirements.md` §10 and §11.
- D3D9 texture / surface format mapping that drives `texture2d<T>` element
  type — `specs/d3d9/formats/`.
- Presentation, swap chain, and `CAMetalLayer` ownership — `specs/d3d9/wsi/`.
- Wine `winemetal` bridge ABI — `specs/winemetal/`.

The implementation structure remains DXMT-compatible: the IR, all analysis
passes, and MSL emission are stateless value transforms (`R-ARCH-*` data-
oriented design), exercisable without Wine, Metal, or GPU timing. Runtime
shader-pixel probes are evidence, not a substitute for IR-level tests.

Traceability:

- `R-CORE-6.*` (in `specs/d3d9/requirements.md` §6 Shaders) — D3D9 shader COM
  lifetime, programmable / FFP boundary, and the parent purity contract
  (`R-CORE-6.7`) that this spec refines.
- `R-CORE-7.2` — `D3DFVF_XYZRHW` NDC conversion responsibility (delegated to
  the translator; see §2.3 here).
- `R-BACK-*` (in `specs/backend/`) — PSO build, archive lifecycle, and draw
  encoding consume the emitted MSL and the resulting `MTLBinaryArchive`.

---

## 1. IR Data Model

**R-CORE-IR-1.1** The shader translator must expose an in-memory IR value type
that represents one decoded D3D9 shader module. The IR is the only required
interchange format between bytecode decode and MSL emission; the IR is not
required to be SPIR-V, AIR, LLVM IR, DXIL, or any other external standard IR.

> Implementation note: the current struct is named `SpirvModule`
> (`src/dxmt9/dxmt9_d3d9_bytecode.hpp`) for historical reasons. The spec-level
> term is *ShaderIR*. The struct must not be confused with a SPIR-V module;
> `words` carries the raw D3D9 token stream, not SPIR-V opcodes.

**R-CORE-IR-1.2** The IR must carry, at minimum:

- the raw shader token stream (for hashing and archive replay);
- the shader stage (`Vertex` / `Pixel`);
- the shader-model major / minor version;
- a decoded instruction array with operands, swizzles, masks, modifiers, and
  the `_pp` (partial precision) hint preserved per-instruction;
- the per-sampler `TextureType` inferred from `dcl_*` sampler declarations;
- a stable 64-bit hash derived from the raw token stream.

**R-CORE-IR-1.3** The IR must be a value type: copyable, comparable for
equality through its hash, and free of pointers into caller-owned memory after
construction. Decoded instructions must not retain spans into the original
bytecode buffer; the IR must own its instruction storage.

**R-CORE-IR-1.4** The IR hash must be deterministic across processes and
builds for the same byte-for-byte input shader. The hash is the primary cache
key input; consumers may extend it (see §6) but must not replace it.

**R-CORE-IR-1.5** Decoding malformed or unsupported shader bytecode must
report a translator error and must not produce a partially populated IR. The
translator must not silently emit MSL for an IR that failed to decode every
instruction.

**R-CORE-IR-1.6** A separate FFP shader source path may bypass IR construction
and emit MSL directly from `FFPKeyVS` / `FFPKeyPS`. The FFP path must still
produce a hash-keyed cache entry and must honour every semantic requirement in
§2.

---

## 2. Semantic Translation Contract

This section consolidates D3D9 → Metal semantic rewrites that the translator
owns. They were previously split between `specs/d3d9/design.md` §5 (FFP key),
§7 (half-pixel offset), and §8 (alpha test); the translator must now treat
them as a single coherent contract.

### 2.1 Fixed-Function Pipeline Key

**R-CORE-IR-2.1** When no programmable VS or PS is bound, the D3D9 core must
derive `FFPKeyVS` and `FFPKeyPS` from `DeviceState` and hand them to the
backend as a `ShaderRef`. The key is a value type and contains no pointers,
handles, or COM references.

**R-CORE-IR-2.2** Two FFP keys that compare equal must produce byte-identical
MSL. Two FFP keys that compare unequal must produce a separately cached
shader; the translator must not collapse semantically distinct keys.

### 2.2 Half-Pixel Offset

**R-CORE-IR-2.3** Programmable vertex shaders must have a half-pixel-offset
fixup injected before MSL emission. The fixup adds `oPos.xy += c_fixup.xy *
oPos.w`, where `c_fixup` is `(1 / viewportWidth, 1 / viewportHeight)` supplied
through the constant-buffer slot reserved for translator-injected values.

**R-CORE-IR-2.4** Fixed-function shaders must include the same half-pixel
fixup in the FFP emit path.

**R-CORE-IR-2.5** `D3DFVF_XYZRHW` (pre-transformed) inputs must be converted
to Metal NDC inside the vertex shader using the viewport `Width` / `Height`
supplied through the translator-injected constant block. The translator must
not assume the application has done the NDC conversion itself.

### 2.3 Texture V-Axis Policy

**R-CORE-IR-2.6** The translator must not introduce a global `v = 1.0 - v`
rewrite around `texld`, `TEX`, `TEXLDB`, `TEXLDP`, `TEXLDD`, or `TEXLDL`. D3D9
texture coordinate orientation must be preserved verbatim from the bytecode.

**R-CORE-IR-2.7** Vertical-inversion symptoms must be triaged in resource
upload / readback orientation, surface addressing, or viewport mapping. They
must not be papered over by a translator-side flip.

### 2.4 Alpha Test

**R-CORE-IR-2.8** `D3DRS_ALPHATESTENABLE` with `D3DRS_ALPHAFUNC` and
`D3DRS_ALPHAREF` must be emitted as a `discard_fragment()` conditional at the
end of the pixel shader. The reference value is supplied through the
translator-injected constant block.

**R-CORE-IR-2.9** Each distinct `(alphaTestEnable, alphaTestFunc)` pair must
produce a separately cached shader variant. The alpha-test state must
participate in the IR cache key (§6).

### 2.5 Debug Bisect Flags

**R-CORE-IR-2.10** The translator must support the following debug-only
toggles, off by default, with scope strictly limited as listed:

| Flag | Scope |
|---|---|
| `DXMT_DEBUG_FLIP_VERTEX_Y` | translated VS clip-space output only |
| `DXMT_DEBUG_FORCE_PIXEL_V_FLIP` | translated PS texture sampling only |
| `DXMT_DEBUG_FORCE_FRAGMENT_COLOR` | translated PS final colour write only |
| `DXMT_DEBUG_FORCE_FULLSCREEN_VERTEX` | translated VS position output only |

These flags must not bleed into other semantic decisions; in particular,
`DXMT_DEBUG_FLIP_VERTEX_Y` must not disable the half-pixel fixup.

---

## 3. Precision and VSOut Layout

This section is the IR-side contract that backs the FP16 and VSOut-trim
investigations recorded in `specs/perfomance.plan.md`. Findings that drive
these requirements are documented in `design.md` §8 (Performance-Driven
Decisions Log).

### 3.1 Per-Register Precision

**R-CORE-IR-3.1** The IR analysis surface must expose, for every shader
register, a precision classification of `Float` or `Half`. The default
classification of every register is `Float`. `Half` is opt-in.

**R-CORE-IR-3.2** A register classified as `Half` must be emitted as a
`half` / `half2` / `half3` / `half4` MSL type. A register classified as
`Float` must be emitted as `float` / `float2` / `float3` / `float4`.

**R-CORE-IR-3.3** The translator must preserve the D3D9 `_pp` (partial
precision) instruction modifier through decoding. The precision pass (§4.4)
may treat `_pp` as a hint that the result register is safe to classify as
`Half`, subject to mandatory-Float rules below.

### 3.2 Mandatory Float Regions

**R-CORE-IR-3.4** The following must always be classified as `Float`,
overriding any `_pp` hint or opt-in policy:

- VS position output (`oPos`, `o0` in vs_3_0).
- PS depth output (`oDepth`).
- Any texture-coordinate register at a sample site. MSL's
  `texture2d<T>::sample(sampler, float2 coord)` overload requires float
  texture coordinates regardless of `T`.
- Any register sourced by an instruction whose MSL counterpart requires
  float (e.g. `dsx`, `dsy` in fragment shaders that are not part of an
  inlinable half-overload set).
- Any register that crosses a stage boundary where the consumer requires
  float (e.g. a VS output read by a PS stage-in that is classified as
  Float).

### 3.3 VSOut Layout

**R-CORE-IR-3.5** The set of VS outputs emitted in the `VSOut` struct must
be **pair-local**: it depends on both the VS and the paired FS stage-in
reads. The translator must compute the union of fields that are written by
the VS and read by the FS, and emit only that union.

**R-CORE-IR-3.6** A VSOut field that is *written* by the VS but *not read*
by the paired FS may be omitted from the emitted layout. This is the basis
for the VSOut liveness pass (§4.5).

**R-CORE-IR-3.7** The VSOut precision policy is per-field. A field may be
classified `Half` only when:

- the VS writer has no Float-mandatory ancestry into that field; and
- the FS reader does not require Float at the consumer site; and
- the field is not the position output.

**R-CORE-IR-3.8** Position output precision must always be `Float`
regardless of policy (a duplicate of §3.4 for emphasis: this is the most
load-bearing invariant for correctness).

### 3.4 Opt-In Policy

**R-CORE-IR-3.9** Half-precision emission must remain opt-in until the
audit gates in §8 are green. The currently shipping opt-in is
`DXMT9_FS_HALF_PRECISION`, which targets the fragment shader body. The
present implementation is a text-rewrite pass marked **EXPERIMENTAL** in
`dxmt9_shader_sources.hpp`; only ~33% of SFIV's fragment shaders compile
under it. The IR-level precision pass described in §4.4 is the supersession
path.

**R-CORE-IR-3.10** Promotion of half precision to default-on must require
all of the following:

- the IR-level precision pass (§4.4) replaces the text-rewrite pass;
- a per-shader correctness oracle (§8.3) passes for the targeted corpus;
- VS buffer-write counter improvement is measured in
  `specs/perfomance.plan.md` against the 2026-06-04 baseline (1627 MiB).

Until those conditions are met, half precision must not be enabled by
default in production builds.

---

## 4. Analysis and Transformation Passes

### 4.1 Pass Purity

**R-CORE-IR-4.1** Every IR analysis or transformation pass must be a pure
value transform: input is the IR (and optionally a prior pass's plan
output); output is a value-type plan, annotation set, or transformed IR.
Passes must not call Metal, Objective-C, Wine, or any system framework.
Passes must not depend on per-process or per-device state.

**R-CORE-IR-4.2** Passes must be deterministic. The same input IR must
produce the same plan bytes across runs and across processes.

**R-CORE-IR-4.3** Pass output is a candidate input to the IR cache key. A
pass whose output changes the emitted MSL must contribute to the cache key
(§6).

### 4.2 Required Passes

**R-CORE-IR-4.4** The IR analysis surface must expose:

- **Constant usage**: which `c`-register ranges are read; whether constants
  are mutable; presence of float, int, and bool constant declarations.
- **Sampler usage**: per-sampler-slot boolean indicating whether the slot is
  referenced.
- **Vertex output semantics**: per-output mapping to D3DDECLUSAGE values
  (position, texcoord, color, secondary color, fog, point size), preserving
  usage index.
- **Pixel input semantics**: matching per-input mapping with centroid
  interpolation flag.
- **Max written temp register index**: highest `r`-register index written;
  drives `r[]` array sizing in the emitter.

**R-CORE-IR-4.5** The required-pass set is the minimum needed for a correct
default emit. Removing any of these passes must fail an audit test.

### 4.3 Optional / Opt-In Passes

**R-CORE-IR-4.6** Optional passes are tracked in `specs/gap.md` until their
correctness oracle (§8) is green:

- VSOut liveness (current opt-in: `DXMT9_TRIM_UNUSED_VARYINGS`).
- Vertex temp-array trim (current opt-in: `DXMT9_TRIM_VERTEX_TEMPS`).
- VS output scratch trim (current opt-in:
  `DXMT9_TRIM_VS_OUTPUT_SCRATCH`).
- Precision inference (target opt-in: extends
  `DXMT9_FS_HALF_PRECISION`).

**R-CORE-IR-4.7** An optional pass must contribute to the cache key only
when it is enabled. When it is disabled, its absence must not change the
emit relative to an IR cached before the pass was introduced.

### 4.4 Precision Inference Pass

**R-CORE-IR-4.8** A precision inference pass must accept the IR (and the
paired FS stage-in plan for VS, or paired VSOut plan for FS) and produce a
plan with three fields:

- per-temp-register `Precision`;
- per-output-register `Precision`;
- a list of `(instruction index, operand index)` cast-insertion sites where
  the emitter must inject a `float ↔ half` conversion.

**R-CORE-IR-4.9** The precision plan must satisfy §3.4 (mandatory Float
regions) as a precondition of MSL emission. A plan that violates §3.4 must
not be passed to the emitter.

### 4.5 VSOut Liveness Pass

**R-CORE-IR-4.10** A VSOut liveness pass must accept the VS IR and the FS
pixel-input semantics and emit a `VSOutLayout` value carrying per-field
emit / omit decisions for texcoords, color, secondary color, fog factor,
point size, and position.

**R-CORE-IR-4.11** The VSOut liveness plan must be the sole authority for
the emitted VSOut struct shape. The emitter must not add a field that the
liveness plan omitted, and must not omit a field the plan emitted.

---

## 5. MSL Emission

**R-CORE-IR-5.1** The MSL emitter must accept the IR, the set of analysis
plans (§4), and the `ShaderSourceContext` (FFP variant key, prelude
options) and produce a complete MSL translation unit string. Output is one
string per shader stage.

**R-CORE-IR-5.2** Emission must be deterministic. Same inputs must produce
byte-identical MSL output across runs, processes, and architectures.

**R-CORE-IR-5.3** Emission must not perform decode-level validation.
Validation is the decoder's responsibility (§1.5) and the pass layer's
responsibility (§4.9). The emitter assumes its inputs are well-formed.

**R-CORE-IR-5.4** Emission must not introduce non-determinism such as
hash-map iteration order, timestamps, or pointer addresses. Internal
ordering must be deterministic (e.g. fixed-size arrays indexed by IR
register number, sorted vectors).

**R-CORE-IR-5.5** The emitter must produce MSL that compiles under the
shipping Apple `metal` toolchain for every shader in the dxmt9 conformance
corpus. Failure to compile is a translator bug, not an application bug.

---

## 6. Cache, Hash, and Archive

**R-CORE-IR-6.1** The cached shader hash key must be a deterministic
function of:

- the IR hash (§1.4);
- the `ShaderSourceContext` (FFP variant key, prelude options, argbuf
  layout id);
- every enabled optional pass's plan-output (§4.7);
- the alpha-test variant key (§2.9);
- the half-precision opt-in state (`DXMT9_FS_HALF_PRECISION` and any
  future VSOut precision opt-in flag);
- any other env-derived debug toggle that changes emitted MSL bytes.

**R-CORE-IR-6.2** Two cache keys that compare equal must correspond to
byte-identical MSL. Two cache keys that compare unequal must not collide
with probability detectable in normal operation (64-bit-or-wider hash).

**R-CORE-IR-6.3** Archive load and save must respect
`DXMT_DISABLE_SHADER_ARCHIVE`. When disabled, the translator must continue
to function without archive persistence.

**R-CORE-IR-6.4** Archive prewarm must honour `DXMT9_PREWARM=full|lazy|
disabled`. The selected mode must be resolved once at process init.

---

## 7. Diagnostics and Debug Knobs

**R-CORE-IR-7.1** `DXMT_DUMP_SHADER_BYTECODE_DIR`, when set, must cause the
decoder to write each decoded shader's raw token stream to the named
directory keyed by IR hash. The dump format must round-trip through the
decoder.

**R-CORE-IR-7.2** `DXMT_DUMP_SHADER_DIR`, when set, must cause the emitter
to write each emitted MSL translation unit to the named directory keyed by
(IR hash, source hash). The source hash must be sufficient to disambiguate
multiple plan combinations that share an IR hash.

**R-CORE-IR-7.3** `DXMT9_PERF_ENCODER_BREAKDOWN` must surface, for every
draw it attributes, the bound VS and PS IR hashes and (when shader dump is
enabled) the corresponding source hash. This is the lever the Xcode-↔-dxmt
join report uses to attribute GPU encoder cost to a specific shader.

**R-CORE-IR-7.4** All translator-owned environment variables must appear in
`agents/rules/environment_variables.rules.md` with documented default,
scope, and intended use. A new translator env var that does not appear in
that file must fail review.

---

## 8. Audit and Test Surface

**R-CORE-IR-8.1** Every required pass (§4.2) must have a golden test under
the `dxmt9-shader-transform-spec` (or equivalent) target. Each test takes a
fixed IR (or hand-built `SpirvModule`) and asserts the produced plan
bytes against a snapshot.

**R-CORE-IR-8.2** Each emitter must have a determinism test that runs the
same IR / plan combination twice and asserts byte-equal MSL output.

**R-CORE-IR-8.3** Half-precision and other optional passes whose output
changes observable pixel behaviour must have a correctness oracle: a
deterministic shader-runner test that compares half-emitted output against
float-emitted output across a reference input set, with a documented
tolerance. The oracle is a precondition for default-on promotion
(§3.10).

**R-CORE-IR-8.4** Existing audits must continue to enforce:

- counter-table consistency for any new perf counter the IR layer surfaces
  (`scripts/check/audit_perf_counter_table.py`);
- counter-callsite presence
  (`scripts/check/audit_perf_counter_callsites.py`);
- environment-variable documentation as in §7.4.

**R-CORE-IR-8.5** A requirement in this document that is not yet fully
implemented or not yet fully evidenced must have a row in `specs/gap.md`.
TODOs must not be hidden inside `requirements.md` or `design.md`.
