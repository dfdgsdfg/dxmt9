---
type: "Spec Requirements"
title: "Shader IR and Translator Requirements"
description: "D3D9 / Shader requirements and compatibility contracts."
tags: [specs, d3d9, shader, requirements]
---

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

**R-CORE-SHADER-1.1** The shader translator must expose an in-memory IR value type
that represents one decoded D3D9 shader module. The IR is the only required
interchange format between bytecode decode and MSL emission; the IR is not
required to be SPIR-V, AIR, LLVM IR, DXIL, or any other external standard IR.

> Implementation note: the current struct is named `SpirvModule`
> (`src/dxmt9/dxmt9_d3d9_bytecode.hpp`) for historical reasons. The spec-level
> term is *ShaderIR*. The struct must not be confused with a SPIR-V module;
> `words` carries the raw D3D9 token stream, not SPIR-V opcodes.

**R-CORE-SHADER-1.2** The IR must carry, at minimum:

- the raw shader token stream (for hashing and archive replay);
- the shader stage (`Vertex` / `Pixel`);
- the shader-model major / minor version;
- a decoded instruction array with operands, swizzles, masks, modifiers, and
  the `_pp` (partial precision) hint preserved per-instruction;
- the per-sampler `TextureType` inferred from `dcl_*` sampler declarations;
- a stable 64-bit hash derived from the raw token stream.

**R-CORE-SHADER-1.3** The IR must be a value type: copyable, comparable for
equality through its hash, and free of pointers into caller-owned memory after
construction. Decoded instructions must not retain spans into the original
bytecode buffer; the IR must own its instruction storage.

**R-CORE-SHADER-1.4** The IR hash must be deterministic across processes and
builds for the same byte-for-byte input shader. The hash is the primary cache
key input; consumers may extend it (see §6) but must not replace it.

**R-CORE-SHADER-1.5** Decoding malformed or unsupported shader bytecode must
report a translator error and must not produce a partially populated IR. The
translator must not silently emit MSL for an IR that failed to decode every
instruction.

**R-CORE-SHADER-1.6** A separate FFP shader source path may bypass IR construction
and emit MSL directly from `FFPKeyVS` / `FFPKeyPS`. The FFP path must still
produce a hash-keyed cache entry and must honour every semantic requirement in
§2.

---

## 2. Semantic Translation Contract

This section consolidates D3D9 → Metal semantic rewrites that the translator
owns. They were previously split between `specs/d3d9/spec.md` §5 (FFP key),
§7 (half-pixel offset), and §8 (alpha test); the translator must now treat
them as a single coherent contract.

### 2.1 Fixed-Function Pipeline Key

**R-CORE-SHADER-2.1** When no programmable VS or PS is bound, the D3D9 core must
derive `FFPKeyVS` and `FFPKeyPS` from `DeviceState` and hand them to the
backend as a `ShaderRef`. The key is a value type and contains no pointers,
handles, or COM references.

**R-CORE-SHADER-2.2** Two FFP keys that compare equal must produce byte-identical
MSL. Two FFP keys that compare unequal must produce a separately cached
shader; the translator must not collapse semantically distinct keys.

### 2.2 Half-Pixel Offset

**R-CORE-SHADER-2.3** Programmable vertex shaders must have a half-pixel-offset
fixup injected before MSL emission. The fixup adds `oPos.xy += c_fixup.xy *
oPos.w`, where `c_fixup` is `(1 / viewportWidth, 1 / viewportHeight)` supplied
through the constant-buffer slot reserved for translator-injected values.

**R-CORE-SHADER-2.4** Fixed-function shaders must include the same half-pixel
fixup in the FFP emit path.

**R-CORE-SHADER-2.5** `D3DFVF_XYZRHW` (pre-transformed) inputs must be converted
to Metal NDC inside the vertex shader using the viewport `Width` / `Height`
supplied through the translator-injected constant block. The translator must
not assume the application has done the NDC conversion itself.

### 2.3 Texture V-Axis Policy

**R-CORE-SHADER-2.6** The translator must not introduce a global `v = 1.0 - v`
rewrite around `texld`, `TEX`, `TEXLDB`, `TEXLDP`, `TEXLDD`, or `TEXLDL`. D3D9
texture coordinate orientation must be preserved verbatim from the bytecode.

**R-CORE-SHADER-2.7** Vertical-inversion symptoms must be triaged in resource
upload / readback orientation, surface addressing, or viewport mapping. They
must not be papered over by a translator-side flip.

### 2.4 Alpha Test

**R-CORE-SHADER-2.8** `D3DRS_ALPHATESTENABLE` with `D3DRS_ALPHAFUNC` and
`D3DRS_ALPHAREF` must be emitted as a `discard_fragment()` conditional at the
end of the pixel shader. The reference value is supplied through the
translator-injected constant block.

**R-CORE-SHADER-2.9** Each distinct `(alphaTestEnable, alphaTestFunc)` pair must
produce a separately cached shader variant. The alpha-test state must
participate in the IR cache key (§6).

### 2.5 Debug Bisect Flags

**R-CORE-SHADER-2.10** The translator must support the following debug-only
toggles, off by default, with scope strictly limited as listed:

| Flag | Scope |
|---|---|
| `DXMT_DEBUG_FLIP_VERTEX_Y` | translated VS clip-space output only |
| `DXMT_DEBUG_FORCE_PIXEL_V_FLIP` | translated PS texture sampling only |
| `DXMT_DEBUG_FORCE_FRAGMENT_COLOR` | translated PS final colour write only |
| `DXMT_DEBUG_FORCE_FULLSCREEN_VERTEX` | translated VS position output only |

These flags must not bleed into other semantic decisions; in particular,
`DXMT_DEBUG_FLIP_VERTEX_Y` must not disable the half-pixel fixup.

### 2.6 SM 1.x Output Clamp

**R-CORE-SHADER-2.11** Pixel shaders with shader-model major version `1`
(`ps_1_0` through `ps_1_4`) MUST clamp their colour output to `[0, 1]`
before emitting it as the final fragment colour. SM 2.0 and SM 3.0
pixel shaders MUST NOT be clamped. The clamp is a translator
obligation, not a render-state setting: D3D9 specifies SM 1.x output
as `[0, 1]`-saturated, MSL does not clamp by default, and wined3d's
GLSL emitter applies the same clamp for the same reason. The clamp
must respect the per-output precision plan: a `Float` output is
clamped against `0.0` / `1.0`, a `Half` output against `0.0h` / `1.0h`.

### 2.7 Reciprocal Edge Semantics

**R-CORE-SHADER-2.12** `RCP` lowering MUST preserve the sign of its source.
The translator MUST NOT clamp the source to a positive epsilon. A zero source
maps to infinity according to the D3D9 instruction contract.

**R-CORE-SHADER-2.13** `RSQ` lowering MUST take the absolute value of its
source before the reciprocal square root. It MUST NOT clamp zero to an
epsilon; a zero source maps to infinity. Both vertex and pixel lowering paths
must implement the same edge semantics.

**R-CORE-SHADER-2.14** `POW` lowering MUST evaluate `abs(src0)^src1` and
replicate the scalar result. Both the GPU emitter and the PE software vertex
interpreter MUST apply the absolute value before exponentiation.

**R-CORE-SHADER-2.15** A pixel-shader `vPos` / `D3DSPR_MISCTYPE` position
input MUST expose D3D9 integer pixel-center coordinates. Because Metal
`[[position]]` exposes half-integer pixel centers, the translator MUST subtract
`(0.5, 0.5)` from `xy` only. It MUST preserve Metal-provided `z` and `w`.
The Wine `d3d9/visual.c:test_fragment_coords` fractional-position oracle and
absolute-coordinate probes under viewport, scissor, and MRT state gate this
conversion.

**R-CORE-SHADER-2.16** Shader-model 2 and 3 `TEX` lowering MUST preserve the
instruction-control field. `TEXLDP` MUST divide a non-cube sampling coordinate
by its fourth component before selecting the texture-dimensional coordinate.
Outside the FETCH4 compatibility route, `TEXLDB` MUST use the fourth component
as an instruction LOD bias, added to the bound sampler's
`D3DSAMP_MIPMAPLODBIAS`. FETCH4's gather operation suppresses both bias sources,
matching its D3D9 compatibility behavior. Unsupported or combined control
encodings MUST fail closed instead of silently sampling as ordinary `TEX`.

---

## 3. Precision and VSOut Layout

This section is the shader-layer contract that backs the FP16 and VSOut-trim
candidates. The rationale and rejected-hypothesis history are documented in
`spec.md` §8 (Performance-Driven Decisions Log).

### 3.1 Per-Register Precision

**R-CORE-SHADER-3.1** The IR analysis surface must expose, for every shader
register, a precision classification of `Float` or `Half`. The default
classification of every register is `Float`. `Half` is opt-in.

**R-CORE-SHADER-3.2** A register classified as `Half` must be emitted as a
`half` / `half2` / `half3` / `half4` MSL type. A register classified as
`Float` must be emitted as `float` / `float2` / `float3` / `float4`.

**R-CORE-SHADER-3.3** The translator must preserve the D3D9 `_pp` (partial
precision) instruction modifier through decoding. The precision pass (§4.4)
may treat `_pp` as a hint that the result register is safe to classify as
`Half`, subject to mandatory-Float rules below. `_pp` is necessary but not
sufficient for `Half`: a register or output field may be classified `Half`
only when every reaching write into that value is half-safe and every
consumer either accepts `Half` or has an explicit boundary cast. If a register
has mixed `_pp` and non-`_pp` writes and the pass cannot prove component-local
safety, the whole register must remain `Float`. In particular, preserving the
modifier must not by itself emit an eager `float -> half -> float` destination
conversion; without a proved `Half` classification, execution remains full
precision.

### 3.2 Mandatory Float Regions

**R-CORE-SHADER-3.4** The following must always be classified as `Float`,
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

**R-CORE-SHADER-3.5** The emitted VSOut layout MUST be parameterised by a
`VSOutLayout` value type produced either by the VSOut liveness pass (§4.5)
when it is enabled, or by a conservative default when it is disabled. The
conservative default MUST emit every field the VS writes, plus the
mandatory-Float fields in §3.2. Changing the default to a trimmed layout is
a behaviour change and MUST go through §3.10's promotion gate, not silent
adoption.

**R-CORE-SHADER-3.6** When the VSOut liveness pass is enabled, the emitted
layout MUST equal the pair-local union: fields written by the VS and read by
the paired FS, plus the mandatory-Float fields in §3.2. A field that is
written by the VS but not read by the paired FS MAY then be omitted; a field
that the FS reads MUST NOT be omitted regardless of VS reachability.

**R-CORE-SHADER-3.7** The VSOut precision policy is per-field. A field may be
classified `Half` only when:

- the VS writer has no Float-mandatory ancestry into that field; and
- the FS reader does not require Float at the consumer site; and
- the field is not the position output.

**R-CORE-SHADER-3.8** Position output precision must always be `Float`
regardless of policy (a duplicate of §3.4 for emphasis: this is the most
load-bearing invariant for correctness).

### 3.4 Opt-In Policy

**R-CORE-SHADER-3.9** Half-precision emission must remain opt-in until the
audit gates in §8 are green. The prior shipping opt-in, `DXMT9_FS_HALF_PRECISION`,
was a text-rewrite pass targeting the fragment shader body that compiled only
~33% of SFIV's fragment shaders; it was documented **EXPERIMENTAL — NOT
FUNCTIONAL** and has been removed (see `agents/rules/environment_variables_encoder.rules.md`
history). This requirement remains open for a future design: the IR-level
precision pass described in §4.4 is the intended supersession path and must
land before any half-precision opt-in ships again.

**R-CORE-SHADER-3.10** Promotion of half precision (or of any layout-changing
opt-in pass) to default-on MUST require all of the following:

- the IR-level precision pass (§4.4) replaces the text-rewrite pass;
- the multi-axis correctness oracle (§8.3) passes across every state class
  it enumerates, on a paired VS+FS replay, above the active-pixel coverage
  gate;
- at least one Xcode encoder export shows
  `VS Buffer Device Memory Bytes Written` decreasing from off → on at the
  3DMark05 GT1 baseline scale documented in `spec.md` §8.1.

Until every condition above is met, the opt-in MUST remain off in production
builds. Promotion without measurable counter movement is forbidden — a
correctness-equivalent rewrite that does not move the counter is correctness
debt without payoff.

---

## 4. Analysis and Transformation Passes

### 4.1 Pass Purity

**R-CORE-SHADER-4.1** Every IR analysis or transformation pass must be a pure
value transform: input is the IR (and optionally a prior pass's plan
output); output is a value-type plan, annotation set, or transformed IR.
Passes must not call Metal, Objective-C, Wine, or any system framework.
Passes must not depend on per-process or per-device state.

**R-CORE-SHADER-4.2** Passes must be deterministic. The same input IR must
produce the same plan bytes across runs and across processes.

**R-CORE-SHADER-4.3** Pass output is a candidate input to the IR cache key. A
pass whose output changes the emitted MSL must contribute to the cache key
(§6).

### 4.2 Required Passes

**R-CORE-SHADER-4.4** The IR analysis surface must expose:

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
  drives fragment `r[]` sizing and remains advisory for the conservative
  vertex array.
- **Vertex output scratch usage**: highest mapped or indexed texcoord output;
  remains a pure analysis even though the default vertex `outTexcoord[]`
  scratch stays at the conservative maximum.

**R-CORE-SHADER-4.5** The required-pass set is the minimum needed for a correct
default emit. Removing any of these passes must fail an audit test.

### 4.3 Optional / Opt-In Passes

**R-CORE-SHADER-4.6** Optional passes are tracked in `specs/d3d9/gap.md` until their
correctness oracle (§8) is green:

- VSOut liveness (current opt-in: `DXMT9_TRIM_UNUSED_VARYINGS`).
- Precision inference (target opt-in: a new IR-level flag; the prior
  text-rewrite carrier `DXMT9_FS_HALF_PRECISION` was removed as non-functional).

**R-CORE-SHADER-4.7** An optional pass must contribute to the cache key only
when it is enabled. When it is disabled, its absence must not change the
emit relative to an IR cached before the pass was introduced.

### 4.4 Precision Inference Pass

**R-CORE-SHADER-4.8** A precision inference pass must accept the IR (and the
paired FS stage-in plan for VS, or paired VSOut plan for FS) and produce a
plan with three fields:

- per-temp-register `Precision`;
- per-output-register `Precision`;
- a list of `(instruction index, operand index)` cast-insertion sites where
  the emitter must inject a `float ↔ half` conversion.

The plan must be conservative for mixed writes. A temp or output register may
be classified `Half` only if all reaching writes are either `_pp` writes or
come from a pass-owned opt-in source such as VSOut-only half precision, and no
mandatory-Float ancestry reaches that register. Otherwise the register must be
classified `Float`.

**R-CORE-SHADER-4.9** The precision plan must satisfy §3.4 (mandatory Float
regions) as a precondition of MSL emission. A plan that violates §3.4 must
not be passed to the emitter.

### 4.5 VSOut Liveness Pass

**R-CORE-SHADER-4.10** A VSOut liveness pass must accept the VS IR and the FS
pixel-input semantics and emit a `VSOutLayout` value carrying per-field
emit / omit decisions for texcoords, color, secondary color, fog factor,
point size, and position.

**R-CORE-SHADER-4.11** The VSOut liveness plan must be the sole authority for
the emitted VSOut struct shape. The emitter must not add a field that the
liveness plan omitted, and must not omit a field the plan emitted.

---

## 5. MSL Emission

**R-CORE-SHADER-5.1** The MSL emitter must accept the IR, the set of analysis
plans (§4), and the `ShaderSourceContext` (FFP variant key, prelude
options) and produce a complete MSL translation unit string. Output is one
string per shader stage.

**R-CORE-SHADER-5.2** Emission must be deterministic. Same inputs must produce
byte-identical MSL output across runs, processes, and architectures.

**R-CORE-SHADER-5.3** Emission must not perform decode-level validation.
Validation is the decoder's responsibility (§1.5) and the pass layer's
responsibility (§4.9). The emitter assumes its inputs are well-formed.

**R-CORE-SHADER-5.4** Emission must not introduce non-determinism such as
hash-map iteration order, timestamps, or pointer addresses. Internal
ordering must be deterministic (e.g. fixed-size arrays indexed by IR
register number, sorted vectors).

**R-CORE-SHADER-5.5** The emitter must produce MSL that compiles under the
shipping Apple `metal` toolchain for every shader in the dxmt9 conformance
corpus. Failure to compile is a translator bug, not an application bug.

**R-CORE-SHADER-5.6** Every spec-defined variant axis MUST be classified as
exactly one of:

- **Function constant**: same emitted MSL, value supplied at PSO build
  time via `[[function_constant(id)]]` or pushed via a uniform / constant
  slot, no new `MTLLibrary` compile, no new cache key entry. Use this for
  axes whose only effect is a scalar value (alpha-test reference, fog
  parameters, viewport fixup constants, `c_fixup` for half-pixel offset).
- **Library variant**: different emitted MSL, new cache key entry,
  separate `MTLBinaryArchive` slot, separate compile. Use this only for
  axes whose effect changes code shape (FFP key, alpha-test
  enable / func, SM-version clamp, half-precision opt-in, half-pixel
  emit mode, V-flip / fragment-colour debug toggles).

A pass plan whose output changes emitted MSL bytes MUST be classified
library-variant and MUST contribute to the cache key (§6). A pass plan
whose output only changes scalar values pushed at runtime MUST be
classified function-constant and MUST NOT contribute to the cache key.
Misclassification is a contract violation: a library variant treated as
a function constant produces wrong output; a function constant treated
as a library variant explodes the archive.

---

## 6. Cache, Hash, and Archive

**R-CORE-SHADER-6.1** The cached shader hash key must be a deterministic
function of:

- the IR hash (§1.4);
- the `ShaderSourceContext` (FFP variant key, prelude options, argbuf
  layout id);
- every enabled optional pass's plan-output (§4.7);
- the alpha-test variant key (§2.9);
- the half-precision opt-in state (a future precision-inference opt-in
  flag, once implemented; the removed `DXMT9_FS_HALF_PRECISION` text-rewrite
  carrier no longer participates);
- any other env-derived debug toggle that changes emitted MSL bytes.

**R-CORE-SHADER-6.2** Two cache keys that compare equal must correspond to
byte-identical MSL. Two cache keys that compare unequal must not collide
with probability detectable in normal operation (64-bit-or-wider hash).

**R-CORE-SHADER-6.3** Archive load and save must respect
`DXMT_DISABLE_SHADER_ARCHIVE`. When disabled, the translator must continue
to function without archive persistence.

**R-CORE-SHADER-6.4** Archive prewarm must honour `DXMT9_PREWARM=full|lazy|
disabled`. The selected mode must be resolved once at process init.

---

## 7. Diagnostics and Debug Knobs

**R-CORE-SHADER-7.1** `DXMT_DUMP_SHADER_BYTECODE_DIR`, when set, must cause the
decoder to write each decoded shader's raw token stream to the named
directory keyed by IR hash. The dump format must round-trip through the
decoder.

**R-CORE-SHADER-7.2** `DXMT_DUMP_SHADER_DIR`, when set, must cause the emitter
to write each emitted MSL translation unit to the named directory keyed by
(IR hash, source hash). The source hash must be sufficient to disambiguate
multiple plan combinations that share an IR hash.

**R-CORE-SHADER-7.3** The shader layer's contract here is to expose stable
IR hashes (R-CORE-SHADER-1.4) and stable source hashes (R-CORE-SHADER-7.2)
in a form that backend perf-attribution tooling can consume as opaque keys.
The consumer contract for any specific perf env var (for example
`DXMT9_PERF_ENCODER_BREAKDOWN`) belongs to the backend / perf-attribution
spec and MUST NOT be redefined here.

**R-CORE-SHADER-7.4** All translator-owned environment variables must appear in
`agents/rules/environment_variables.rules.md` with documented default,
scope, and intended use. A new translator env var that does not appear in
that file must fail review.

---

## 8. Audit and Test Surface

**R-CORE-SHADER-8.1** Every required pass (§4.2) must have a golden test under
the `dxmt9-shader-transform-spec` (or equivalent) target. Each test takes a
fixed IR (or hand-built `SpirvModule`) and asserts the produced plan
bytes against a snapshot.

**R-CORE-SHADER-8.2** Each emitter must have a determinism test that runs the
same IR / plan combination twice and asserts byte-equal MSL output.

**R-CORE-SHADER-8.3** Half-precision and other optional passes whose output
changes observable pixel behaviour MUST have a **multi-axis** correctness
oracle as a precondition for default-on promotion (§3.10). A shader-local
fixed-grid comparison is insufficient. The oracle MUST cover:

- a **paired VS + FS replay** (not a standalone shader); the relevant
  precision boundary is between stages, so single-stage tests miss the
  hazard surface;
- a **state-class taxonomy** that enumerates at minimum
  `{depth-write opaque, depth-read no-blend, depth-read blend-off,
  depth-read alpha-blend, depth-read screen-blend}`; previous validation
  work showed that depth-read / color-write reorders fail in classes
  hidden by depth-write-opaque coverage;
- an **active-pixel coverage gate**: per-replay active-pixel ratio MUST
  exceed a documented threshold, because sparse-coverage replays produced
  false-negative passes in prior validation work and MUST NOT count as
  evidence;
- **per-output tolerance**: colour ≤ 1 LSB per channel; depth exact (any
  rounding changes z-fight ordering); alpha exact (alpha rounding cascades
  through blend equations);
- **counter gate**: at least one Xcode encoder export proving that
  `VS Buffer Device Memory Bytes Written` (or the analogous counter for
  the pass under test) actually moves in the intended direction at
  baseline scale.

**R-CORE-SHADER-8.4** Existing audits must continue to enforce:

- counter-table consistency for any new perf counter the IR layer surfaces
  (`scripts/check/audit_perf_counter_table.py`);
- counter-callsite presence
  (`scripts/check/audit_perf_counter_callsites.py`);
- environment-variable documentation as in §7.4;
- **Wine D3D9 PE conformance** as the public-ABI oracle. The PE binaries
  `tests/conformance/d3d9/dxmt9-d3d9-conformance.exe` (mapped in
  `tests/conformance/d3d9/MANIFEST.toml` to Wine `dlls/d3d9/tests/`
  sources) gate translator correctness against the same shader inputs
  third-party D3D9 implementations are validated against. A translator
  change that fails a previously-passing manifest entry is a regression;
  a change that asks for a passing entry to be downgraded requires an
  explicit gap.md row.
- **vkd3d-shader `.shader_test` corpus** as the SM 1.x / 2.x / 3.x
  decoder oracle for D3DBC edge cases (relative addressing, predicate
  registers, ps_1_x texture instructions, partial precision hints,
  TEXKILL, TEXLDP / TEXLDB instruction controls, vPos / vFace). Imported
  fixtures MUST record upstream source, URL, commit, model, opcode coverage,
  and license scope per
  `agents/rules/codebase_conventions.rules.md` License And Reference
  Policy. Refreshes MUST include a drift check against a local vkd3d checkout
  or explicitly document why the fixture is frozen. The corpus is a behaviour
  reference, not an ABI dependency, and importing vkd3d source code is out of
  scope.

**R-CORE-SHADER-8.5** The precision inference pass (§4.4), once implemented,
MUST have a property or model-checking validation suite. The suite MUST prove
or exhaustively test, over a bounded IR model, that:

- the `{Half, Float}` lattice converges monotonically toward `Float`;
- mandatory-Float regions (§3.2) are never classified `Half`;
- mixed `_pp` / non-`_pp` reaching writes remain `Float` unless a documented
  component-local proof exists;
- every precision boundary crossing has a corresponding cast-insertion site.

**R-CORE-SHADER-8.6** The VSOut liveness pass (§4.5) MUST have an exhaustive
set-equation validation over the finite semantic-field domain. For every
generated `(VS_written, FS_read, mandatory)` tuple, the emitted layout MUST
equal `(VS_written ∩ FS_read) ∪ mandatory`, position MUST be emitted, and no
FS-read field may be omitted.

**R-CORE-SHADER-8.7** Cache-key and variant-axis completeness MUST be validated
by an exhaustive axis-toggle suite. For every spec-defined axis, the suite MUST
show that:

- axes classified as library variants change the cache key whenever they change
  emitted MSL bytes;
- axes classified as function constants or runtime constants do not create a
  new MSL cache key;
- equal cache keys correspond to byte-identical MSL output;
- every spec axis is classified into exactly one variant bucket.

**R-CORE-SHADER-8.8** D3DBC decoder safety MUST be validated with grammar-based
fixtures or fuzzing plus sanitizer coverage. The suite MUST assert that
malformed bytecode never produces a partial IR, token advancement cannot read
past the supplied bytecode, opcode operand counts are respected, and vkd3d
drift-check fixtures cover imported decoder-oracle cases.

**R-CORE-SHADER-8.9** Emitter determinism and pass purity MUST be validated by
repeat-run and static-audit coverage. The same `(IR, plan, ShaderSourceContext)`
triple MUST produce byte-identical MSL; pass and emitter code MUST NOT depend
on process time, pointer identity, unordered container iteration, Wine, Metal,
or environment variables except for documented `ShaderSourceContext` inputs.

**R-CORE-SHADER-8.10** **SM 1.x output clamp version gating.** A property-based
test MUST assert, for every pixel shader IR in a generated population:

```
emit(IR) contains SM1xClampForm  ⟺  IR.major == 1
```

SM 2.0 and SM 3.0 pixel shaders MUST NOT be clamped; SM 1.x pixel shaders
MUST always be clamped (R-CORE-SHADER-2.11). The clamp form is precision-
aware (`clamp(out, 0.0, 1.0)` for Float-emitted output, `clamp(out, 0.0h,
1.0h)` for Half-emitted output per R-CORE-SHADER-3.7); the property checks
for the presence of either form. The generator MUST include adversarial
cases at the SM boundary: a `ps_1_4` IR mutated to `ps_2_0` must lose the
clamp, and the reverse must add it.

**R-CORE-SHADER-8.11** A requirement in this document that is not yet fully
implemented or not yet fully evidenced must have a row in `specs/d3d9/gap.md`.
TODOs must not be hidden inside `requirements.md` or `spec.md`.
