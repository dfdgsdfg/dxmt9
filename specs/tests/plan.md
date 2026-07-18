# Tests Plan: Programmable Shader Draw Coverage

This plan tracks the missing coverage for programmable shader draw execution.
The goal is not only to test shader translation, but to prove that shader-bound
draws select the correct vertex/index/resource path across D3D9 state changes.

Current evidence is split across three layers:

- `tests/native/shader/shader_transform_spec.cpp` and
  `tests/native/core/core_shader_translator_spec.cpp` cover D3DBC decode and
  generated MSL/source contracts.
- `tests/native/backend/encode_draw_recorder_spec.cpp` covers draw encoder
  command ordering, stream binding, UP/preuploaded data selection, and indexed
  issue paths without executing Metal.
- `tests/shader_runner/corpus/*.shader_test` covers GPU-visible shader results
  through readback.

The gap is the cross-product between these layers: many shader opcodes and
basic draw paths are tested, but fewer tests prove that real programmable draws
with varied declarations, stream layouts, index sources, and state transitions
produce the expected pixels and avoid stale fixed-function behaviour.

---

## 1. Current Coverage Baseline

### 1.1 Native draw encoder coverage

`dxmt9-encode-draw-recorder-spec` currently covers:

- Non-indexed `DrawPrimitive` command issue and start-vertex offset folding.
- Indexed primitive topology and `UInt16`/`UInt32` index width boundaries.
- Bound vertex plus bound index command ordering.
- Bound vertex plus UP/preuploaded user index selection.
- UP/preuploaded vertex plus UP/preuploaded index selection.
- Indexed UP vertex handling without folding `startVertex` into the vertex
  buffer offset.
- Programmable VS extra stream binding before draw.
- Missing programmable extra stream skipping without stale Metal slot binding.
- Sparse stream offsets on indexed programmable draws.
- Argbuf hybrid mode keeping direct texture/sampler binds.
- Programmable indexed draw with an FFP-decodable declaration and 3DMark05-style
  blend/texture-mask heuristic staying on direct `DrawIndexedPrimitives`.
- Mixed shader draw input binding for programmable VS + FFP PS, FFP VS +
  programmable PS, null VS + programmable PS, and programmable VS + null PS.
- Programmable FVF-to-vertex-declaration and vertex-declaration-to-FVF
  transitions without stale layout stride reuse.

This is good first-line coverage for encoder policy and command order. It does
not prove GPU-visible pixel results for most combinations.

### 1.2 Shader translator and corpus coverage

The shader corpus has broad opcode/readback coverage and includes many pixel
shader tests plus a smaller number of vertex shader tests. It covers important
runtime behaviours such as texture reads, render-state interaction, fog, MRT,
depth output, VFACE, and selected VS-specific cases.

The corpus is weaker for draw input variation:

- Most tests use a standard quad helper rather than varied index paths.
- One programmable indexed color readback probe now covers a bound index buffer
  with non-zero `startIndex`.
- One color-only multi-stream programmable readback probe now covers separate
  position and color streams, including a non-zero color offset.
- One FFP-decodable `POSITION+NORMAL` declaration readback probe now proves a
  programmable VS can read the `NORMAL` attribute directly instead of falling
  through an FFP layout interpretation.
- FFP/prog shader transitions now have two readback probes for FFP ->
  programmable and programmable -> FFP sequencing.
- Programmable declaration-to-declaration transition now has a readback probe
  that changes the `NORMAL` offset/stride between sequential draws.
- `SetStreamSourceFreq` is API-state covered by
  `test_stream_source_frequency_state` and native-verified through core draw
  submission, Metal instance arguments, and generated MSL addressing. The
  remaining gap is a shader-runner or PE pixel/readback probe because those
  harnesses do not yet expose instanced stream setup.

### 1.3 D3D9 conformance and experiments

The D3D9 conformance tests cover shader object API, constants, declarations,
stream-source validation, and selected visual behaviours. The experiments tree
contains programmable D3D9 samples, but these are not a systematic regression
matrix unless wired into automated evidence with pass/fail criteria.

---

## 2. Coverage Matrix

The programmable shader draw matrix should be tracked by feature axis. Each row
needs at least one fast native assertion and, where pixels are the only reliable
oracle, at least one shader-runner or conformance readback probe.

| Axis | Existing evidence | Gap | Target evidence |
|---|---|---|---|
| VS/PS binding mode | Full programmable VS+PS appears in native and corpus tests; `testMixedShaderPathsBindProgrammableDrawInputs` covers VS prog + PS FFP, VS FFP + PS prog, VS null + PS prog, VS prog + PS null | Closed for current matrix | Keep in native quick set |
| Declaration source | FVF/key tests, vertex-decl hash tests, programmable FVF<->decl recorder transitions, color/multistream readback, `POSITION+NORMAL` FFP-decodable readback, declaration A->B readback, and `testGenericDeclUsagesDecodeCleanly` for TANGENT/BINORMAL/TESSFACTOR/FOG/DEPTH/SAMPLE generic programmable inputs | Closed for supported runtime usages currently exposed by shader-runner; uncommon usages are accepted as generic programmable VS semantics while FFP continues to consume only the fixed-function subset | Add more runtime usage probes only when new supported usages need pixel-level oracles |
| Index source | Native bound/UP/preuploaded index coverage plus shader-runner bound-index and indexed-UP programmable readback | Closed for bound and UP GPU-visible paths; preuploaded user-data selection is command-recorder evidence because shader-runner has no preuploaded command | Add readback only if a preuploaded shader-runner command is introduced |
| Vertex source | Native bound/UP/preuploaded vertex coverage plus indexed-UP programmable readback | Closed for bound and UP GPU-visible paths; preuploaded vertex selection is command-recorder evidence | Add readback only if a preuploaded shader-runner command is introduced |
| Multi-stream | Native stream slot/offset coverage plus color-only and textured readback probes; missing extra stream has recorder coverage | Closed for current stable pixel oracles | A missing-stream pixel oracle remains conditional because undefined/default attribute behaviour is not a stable visual contract here |
| Instancing | PE API-state validation via `test_stream_source_frequency_state`; native tests pin provider-to-core state, state-block restore, draw instance count, per-stream divisors, Metal draw arguments, and `instance_id / divisor` shader addressing | Runtime visual evidence is still missing | Add a shader-runner or PE conformance readback that distinguishes multiple instances and a non-1 instance-data divisor |
| Argbuf mode | Binding/source-contract tests, recorder coverage for indexed and multi-stream programmable draws with argbuf enabled, and default-vs-Stage1 indexed VS-constant readback | Closed for constants/direct resource lanes in the current default matrix | Add resource-array opt-in readback crossed with indexed programmable draw if that lane becomes default or per-corpus toggles are added |
| State transition | State snapshot tests, isolated draw tests, FFP->prog, prog->FFP, prog decl A->B readback probes, and native missing-stream stale-bind coverage | Closed for deterministic evidence currently available | Add visual missing-stream case only if shader-runner gains a stable negative oracle |
| FFP/prog boundary | 3DMark05 heuristic regression test plus negative native tests for mixed shader paths, FFP-decodable programmable declarations, and FVF/decl transition stale-layout prevention | Closed for the known 3DMark05/bloom regression class | Add new negative tests when another FFP-only encoder branch is found |

---

## 3. Work Plan

### Phase A: Native policy matrix

Add low-cost recorder/state tests first. These catch path-selection regressions
without GPU readback noise.

1. Add mixed shader path cases to `encode_draw_recorder_spec.cpp`.
   - Programmable VS + fixed-function PS.
   - Fixed-function VS + programmable PS.
   - Null VS + programmable PS.
   - Programmable VS + null PS.
   - Acceptance: command issue path, draw volatile values, stream binds, and
     texture/sampler binds match the selected shader context.

2. Add declaration transition cases.
   - FVF draw followed by vertex declaration draw.
   - Vertex declaration draw followed by FVF draw.
   - Programmable shader with FFP-decodable FVF/decl.
   - Generic usage declaration that programmable shaders accept but FFP decode
     should not reinterpret.
   - Acceptance: pipeline key/layout hash changes when it must, stale FFP layout
     does not affect programmable draw policy.

3. Expand indexed draw matrix.
   - Bound vertex + bound index.
   - Bound vertex + UP index.
   - UP vertex + UP index.
   - Preuploaded vertex/index.
   - `UInt16` and `UInt32`.
   - `baseVertexIndex`, `startIndex`, and `startVertex` edge cases.
   - Acceptance: direct indexed draws stay indexed unless an explicit FFP-only
     expansion policy applies.

4. Add argbuf cross-product checks.
   - Re-run representative non-indexed, indexed, multi-stream, and high sampler
     programmable draw cases with `argbufHybridMode=true`.
   - Acceptance: constants use argbuf slot 30, texture/sampler/stream resources
     stay on their direct Metal lanes unless the resource-array mode explicitly
     owns them.

### Phase B: Shader-runner draw-input probes

Add GPU-visible probes for cases where native command recording cannot prove
the final result.

1. Programmable indexed geometry readback.
   - Draw two triangles with a non-linear index order.
   - Encode expected colors so wrong `startIndex`, index width, or base vertex
     changes visible pixels.

2. Multi-stream programmable readback.
   - Stream 0: position.
   - Stream 1: color or texcoord.
   - Optional stream 2: secondary texcoord.
   - Pixel shader outputs interpolated values from the non-zero stream.
   - Acceptance: wrong stream slot/offset/stride changes the probe color.

3. Declaration diversity readback.
   - Color attribute load.
   - Normal-like float3 attribute load.
   - Generic/tangent-like usage if supported by translator path.
   - FFP-decodable declaration bound with programmable shaders.
   - Acceptance: shader input values match the declaration, not stale FVF decode.

4. State transition readback.
   - Draw FFP solid/texture, then programmable shader in the same target.
   - Draw programmable shader, then FFP.
   - Draw programmable shader with declaration A, then declaration B.
   - Acceptance: second draw output proves no stale shader, declaration, texture,
     or stream binding leaked from the first draw.

### Phase C: Instancing and stream frequency

Implement minimal evidence for `SetStreamSourceFreq` after the normal draw matrix
is stable.

1. Native state/encoder tests.
   - Validate stream frequency state capture.
   - Validate instance-rate stream binding and draw issue metadata.

2. Runtime/conformance visual probe.
   - Stream 0 per-vertex position.
   - Stream 1 per-instance color or offset.
   - Draw at least two instances with different expected pixels.
   - Acceptance: both instances render with distinct per-instance data.

If the backend does not yet implement full instancing semantics, tests should be
committed as documented expected failures only if the local harness supports
that status clearly. Otherwise keep them in a tracked gap until implementation
work starts.

### Phase D: Automation and reporting

1. Add focused Meson test aliases for the programmable draw matrix.
   - Native quick set: recorder/state/translator specs.
   - Runtime set: selected shader-runner programmable draw probes.

2. Update `specs/gap.md` when each row gains durable evidence.

3. Keep experiments as manual smoke coverage unless each launcher has:
   - deterministic timeout,
   - pass/fail log extraction,
   - expected counters or pixel probes,
   - cleanup of large temporary artifacts.

---

## 4. Acceptance Criteria

Programmable shader draw coverage is considered adequate when all of the
following are true:

- Every row in the coverage matrix has at least one native deterministic test.
- Every GPU-visible draw-input behaviour that cannot be proven by command
  recording has at least one readback or conformance visual probe.
- FFP-only draw policies are guarded by negative tests proving they do not apply
  to programmable shader contexts.
- Argbuf on/off does not change resource binding semantics for representative
  programmable non-indexed, indexed, textured, and multi-stream draws.
- State transition tests prove that shader, declaration, stream, texture,
  sampler, and index state do not leak across FFP/prog or prog/prog draw
  boundaries.
- Instancing has either passing native plus visual evidence, or is explicitly
  tracked as unsupported/incomplete in `specs/gap.md`.

### 4.1 Current Acceptance Audit

Current state satisfies the acceptance criteria for the implemented programmable
draw surface:

- Native deterministic coverage exists for every implemented row through
  `dxmt9-encode-draw-recorder-spec`, `shader_argbuf_binding_value_spec`,
  `backend_pipeline_key_spec`, shader translator specs, and
  `shader_transform_spec`.
- GPU-visible readback exists for bound-index, indexed-UP, indexed VS-constant
  argbuf/default-vs-Stage1 parity, multi-stream color, FFP-decodable
  `POSITION+NORMAL`, declaration A->B, and FFP<->programmable sequencing.
- FFP-only draw policy leakage is guarded by the 3DMark05 programmable indexed
  heuristic test, mixed shader path recorder tests, FVF/decl transition tests,
  and FFP-decodable programmable readback.
- Argbuf constants are readback-validated against the Stage1 direct fallback for
  an indexed programmable draw; texture/sampler resource-array cross-product is
  not part of the default matrix and remains conditional on that opt-in lane.
- Instancing semantics are implemented and native-verified from provider state
  through Metal draw arguments and shader addressing. Pixel/readback evidence
  remains a test-coverage gap until the shader-runner DSL or PE conformance
  harness can bind stream-frequency state for a multi-instance draw.

---

## 5. Implemented And Deferred Test Names

Native backend:

- `testMixedShaderPathsBindProgrammableDrawInputs`
- `testProgrammableDrawFvfAndDeclTransitionsDoNotReuseLayout`
- `testProgrammableIndexedBlendHeuristicStaysDirect`
- `testProgrammableArgbufIndexedDrawKeepsDirectResourceLanes`
- `testProgrammableArgbufMultistreamDrawKeepsDirectResourceLanes`
- `testProgrammableVsBindsExtraBoundStreamBeforeDraw`
- `testProgrammableVsSkipsMissingExtraStreamWithoutStaleBind`
- `testIndexedProgrammableDrawPreservesSparseStreamOffsets`
- `testBoundVertexAndUserIndexOrdering`
- `testBoundVertexAndBoundIndexOrdering`
- `testUserVertexAndUserIndexOrdering`
- `testPreUploadedIndexedUserVertexIgnoresStartVertex`
- `testPreUploadedNonIndexedUserVertexFoldsStartVertex`

Shader runner corpus:

- `vs_specific/dxmt9_prog_indexed_color_readback.shader_test`
- `vs_specific/dxmt9_prog_indexed_up_color_readback.shader_test`
- `vs_specific/dxmt9_prog_indexed_vsconst_argbuf_readback.shader_test`
- `vs_specific/dxmt9_prog_multistream_color_readback.shader_test`
- `vs_specific/dxmt9_prog_ffp_decodable_normal_readback.shader_test`
- `vs_specific/dxmt9_prog_decl_transition_normal_readback.shader_test`
- `render_state/dxmt9_prog_after_ffp_state_transition_readback.shader_test`
- `render_state/dxmt9_ffp_after_prog_state_transition_readback.shader_test`

Deferred until runtime instancing exists:

- `vs_specific/dxmt9_prog_instancing_streamfreq_readback.shader_test`

---

## 6. Deferred Or Conditional Follow-ups

1. Extend core draw state and shader-runner DSL if programmable instancing /
   `SetStreamSourceFreq` is promoted from PE API-state support to runtime draw
   support.
2. Add a stream present -> missing state-transition readback probe if the
   shader-runner DSL grows an explicit missing-stream negative oracle.
3. Add generic declaration diversity readback for runtime-supported usages if
   title-specific evidence is needed beyond the current `COLOR`, `TEXCOORD`,
   and `NORMAL` probes. Keep `TANGENT`/`BINORMAL` in safe-reject coverage until
   Metal lowering exists.
4. Add preuploaded-style programmable indexed readback evidence if the
   shader-runner DSL grows an explicit preuploaded draw command.
5. Add resource-array argbuf runtime readback crossed with programmable indexed
   geometry if the shader-runner harness can toggle that opt-in lane per corpus
   run.
6. Re-run the native quick set after related changes:

```sh
meson test -C build-x86_64-builtin \
  dxmt9:dxmt9-encode-draw-recorder-spec \
  dxmt9:dxmt9-shader-argbuf-binding-value-spec \
  dxmt9:dxmt9-backend-pipeline-key-spec \
  dxmt9:dxmt9-core-shader-translator-spec \
  dxmt9:dxmt9-shader-transform-spec
```

7. Run the selected shader-runner probes after changing the runtime cases.

---

## 7. Progress Log

- `dxmt9-encode-draw-recorder-spec` now includes
  `testMixedShaderPathsBindProgrammableDrawInputs`, covering the four mixed
  VS/PS binding modes listed in Phase A.
- `dxmt9-encode-draw-recorder-spec` now includes
  `testProgrammableDrawFvfAndDeclTransitionsDoNotReuseLayout`, covering FVF to
  vertex declaration and vertex declaration to FVF transitions on programmable
  shader draws.
- `dxmt9-encode-draw-recorder-spec` now includes
  `testProgrammableArgbufIndexedDrawKeepsDirectResourceLanes` and
  `testProgrammableArgbufMultistreamDrawKeepsDirectResourceLanes`, covering
  indexed and multi-stream programmable draws with `argbufHybridMode=true`.
- `vs_specific/dxmt9_prog_indexed_color_readback.shader_test` covers a
  programmable VS+PS draw with bound vertex/index buffers and non-zero
  `startIndex`. The probe renders the green indexed triangle; drawing the first
  non-indexed vertices instead would hit the red/clear control geometry.
- `vs_specific/dxmt9_prog_indexed_up_color_readback.shader_test` covers a
  programmable VS+PS indexed-UP draw where both vertex and index data come from
  user-memory payloads. The index payload selects the green right-side triangle;
  using the first vertices directly would hit the red/clear control geometry.
- `vs_specific/dxmt9_prog_indexed_vsconst_argbuf_readback.shader_test` covers a
  programmable indexed draw whose VS color comes from `c0`, not vertex color.
  Running the same probe in the default and Stage1 corpus lanes compares the
  default argbuf constant path against the direct constant binding fallback.
- `vs_specific/dxmt9_prog_multistream_color_readback.shader_test` covers a
  programmable VS+PS draw where stream 0 carries position and stream 1 carries
  color at offset 4. The probe renders green; a stale slot/offset bind would
  read the red guard word or leave the target clear.
- `vs_specific/dxmt9_prog_ffp_decodable_normal_readback.shader_test` covers a
  programmable VS+PS draw with an FFP-decodable `POSITION+NORMAL` declaration;
  the VS reads `NORMAL` and routes it to color, proving programmable input
  binding uses shader DCL semantics.
- `render_state/dxmt9_prog_after_ffp_state_transition_readback.shader_test`
  and `render_state/dxmt9_ffp_after_prog_state_transition_readback.shader_test`
  cover FFP -> programmable and programmable -> FFP draw sequencing in the same
  render target.
- `vs_specific/dxmt9_prog_decl_transition_normal_readback.shader_test` covers
  programmable declaration A -> declaration B sequencing by changing the
  `NORMAL` offset from 16 to 20 between draws. The second draw would read the
  red guard component if the first declaration layout leaked.
- `SetStreamSourceFreq` programmable draws have native coverage for canonical
  state, state blocks, Metal instance count, stream divisors, and generated MSL
  `instance_id` addressing. Runtime pixel/readback coverage remains deferred
  until the shader-runner or PE harness exposes instanced stream setup.
- Focused validation command:

```sh
meson test -C build-x86_64-builtin dxmt9:dxmt9-encode-draw-recorder-spec
```

- Runtime validation command:

```sh
meson test -C build-x86_64-builtin \
  dxmt9:dxmt9-shader-corpus-vs_specific-dxmt9_prog_indexed_color_readback \
  dxmt9:dxmt9-shader-corpus-stage1-vs_specific-dxmt9_prog_indexed_color_readback \
  dxmt9:dxmt9-shader-corpus-vs_specific-dxmt9_prog_indexed_up_color_readback \
  dxmt9:dxmt9-shader-corpus-stage1-vs_specific-dxmt9_prog_indexed_up_color_readback \
  dxmt9:dxmt9-shader-corpus-vs_specific-dxmt9_prog_indexed_vsconst_argbuf_readback \
  dxmt9:dxmt9-shader-corpus-stage1-vs_specific-dxmt9_prog_indexed_vsconst_argbuf_readback \
  dxmt9:dxmt9-shader-corpus-vs_specific-dxmt9_prog_multistream_color_readback \
  dxmt9:dxmt9-shader-corpus-stage1-vs_specific-dxmt9_prog_multistream_color_readback \
  dxmt9:dxmt9-shader-corpus-vs_specific-dxmt9_prog_ffp_decodable_normal_readback \
  dxmt9:dxmt9-shader-corpus-stage1-vs_specific-dxmt9_prog_ffp_decodable_normal_readback \
  dxmt9:dxmt9-shader-corpus-vs_specific-dxmt9_prog_decl_transition_normal_readback \
  dxmt9:dxmt9-shader-corpus-stage1-vs_specific-dxmt9_prog_decl_transition_normal_readback \
  dxmt9:dxmt9-shader-corpus-render_state-dxmt9_prog_after_ffp_state_transition_readback \
  dxmt9:dxmt9-shader-corpus-stage1-render_state-dxmt9_prog_after_ffp_state_transition_readback \
  dxmt9:dxmt9-shader-corpus-render_state-dxmt9_ffp_after_prog_state_transition_readback \
  dxmt9:dxmt9-shader-corpus-stage1-render_state-dxmt9_ffp_after_prog_state_transition_readback
```
