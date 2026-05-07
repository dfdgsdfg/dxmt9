# Draw Uniforms Layout Requirements

dxmt9 must split per-draw shader uniforms and fixed-function state across
multiple Metal binding slots so that the per-draw write volume is bounded by
the small set of fields that actually change per draw, not by the size of the
full uniform working set.

These requirements describe the contract owned by the backend encode path
and the generated MSL shader sources. The PE D3D9 layer is unchanged: it
records D3D9 `Set*` semantics into chunk records as today; backend-owned
dirty masks and binding categories are derived during chunk import. The
design that satisfies these requirements lives in `design.md`.

Traceability: `R-BACK-12.1` through `R-BACK-12.21`. Cross-references:
`specs/backend/design.md` (encode path), `specs/d3d9/design.md` (state
shadow source), `docs/perfomance-bottleneck.md` (empirical motivation).

---

## 1. Per-Frequency Split

### 1.1 Per-stage separation (`R-BACK-12.1`)

Vertex-shader-only state (`vs*Const`, FFP transforms, clip planes,
`halfPixelFixup`, viewport, `DrawVolatile`) must not be bound to the fragment
stage. Fragment-shader-only state (`ps*Const`, fog, alpha test, textureFactor)
must not be bound to the vertex stage.

### 1.2 Shader / FFP separation (`R-BACK-12.2`)

Shader-stage constants (`vsFloatConst`, `vsIntConst`, `vsBoolConst`,
`psFloatConst`, `psIntConst`, `psBoolConst`) must live in a different binding
slot from FFP state (`ffpWorldViewProj`, `ffpTextureTransforms`, fog scalars,
alpha test, textureFactor). Apps that only use the shader pipeline must not
incur an FFP write per draw, and FFP-only apps must not incur a shader-constant
write per draw beyond the range they actually use.

### 1.3 Volatile / stable separation (`R-BACK-12.3`)

Fields that vary per draw within a `DrawRun` must live in `DrawVolatile`,
emitted to the GPU through `setVertexBytes` (Metal inline data) rather than
through the transient slab. Fields that are run-stable must live in one of
the per-frequency UBOs and must not be re-written per draw.

The set of per-draw volatile fields is exactly:

- `vertexBaseIndex`
- `vertexStreamOffset`
- `vertexStreamStride`

Any field added to `DrawVolatile` in a later revision must be justified as
genuinely per-draw (i.e., not derivable from `FlatDrawStateView` alone).

### 1.4 No field duplication (`R-BACK-12.4`)

Each shader-visible field must reside in exactly one of `VsConsts`,
`PsConsts`, `FfpVsConsts`, `FfpPsConsts`, or `DrawVolatile`. Duplicating a
field across buffers (e.g., a "stable copy + volatile copy") is forbidden;
shader-side reads must have a single source of truth.

---

## 2. Binding Slot Contract

### 2.1 Stable slot assignment (`R-BACK-12.5`)

The MSL shader source emitted by `dxmt9_shader_sources.cpp` must declare the
following bindings, and the encoder must bind to the matching slots:

| Slot | Stage | Buffer | Notes |
|---:|---|---|---|
| 0 | vertex | `VsConsts` | replaces existing `[[buffer(0)]] DrawUniforms` for VS |
| 0 | fragment | `PsConsts` | replaces existing `[[buffer(0)]] DrawUniforms` for FS |
| 1 | vertex | stream0 | unchanged from current contract |
| 3 | vertex | `FfpVsConsts` | new |
| 3 | fragment | `FfpPsConsts` | new |
| 5 | vertex | `DrawVolatile` | new, via `setVertexBytes` |

Slot 2 and slots 4, 6+ remain reserved.

### 2.2 Texture and sampler slots unchanged (`R-BACK-12.6`)

`[[texture(0..7)]]` and `[[sampler(0..7)]]` bindings on the fragment stage
must continue to follow the existing contract in `specs/d3d9/design.md`.
This spec does not modify resource binding.

### 2.3 Vertex stream slot unchanged (`R-BACK-12.7`)

VS slot 1 must continue to host the active vertex stream's transient or
resident buffer slice. The pre-existing UP-vertex / UP-index slab paths
(documented in `specs/backend/design.md`) are unaffected.

---

## 3. Dirty Tracking

### 3.1 Bitmask granularity (`R-BACK-12.8`)

The encoder context must maintain a per-encoder dirty bitmask covering at
minimum the following categories:

- `VS_F`, `VS_I`, `VS_B`
- `PS_F`, `PS_I`, `PS_B`
- `FFP_VS_TRANSFORMS`, `FFP_VS_CLIP`, `FFP_VS_VIEWPORT`
- `FFP_PS_FOG`, `FFP_PS_ALPHA`, `FFP_PS_TEX_FACTOR`

Granularity finer than category (per-field bits) is not required; granularity
coarser than category (e.g., a single "all FFP" bit) is forbidden.

### 3.2 Bit set on chunk-record import (`R-BACK-12.9`)

The backend importer must derive the matching dirty bit from imported chunk
records (`D9C_COMMAND_RECORD_APPLY_STATE`, transform records, light records,
constant-set records) and OR it into the encoder's `DirtyMask`. The PE-side
`DeviceState` shadow must not reference Metal binding categories; PE remains
responsible only for recording the D3D9 state change. No new bridge call is
introduced; the existing record schema already carries the necessary
information.

### 3.3 Range counters on shader-constant set records (`R-BACK-12.10`)

When the importer applies a constant-set record from the chunk
(originating from `SetVertexShaderConstantF/I/B` or
`SetPixelShaderConstantF/I/B`), it must update a backend-owned per-stage
range counter as `max(prev, start + count)`. The encoder must upload only
the `[0..maxChanged]` prefix of the matching `vsFloatConst` / `vsIntConst` /
`vsBoolConst` / `psFloatConst` / `psIntConst` / `psBoolConst` array, not the
full register file.

### 3.4 Bit cleared after upload (`R-BACK-12.11`)

After the encoder completes the slab build and `setVertexBuffer` /
`setFragmentBuffer` for a category, it must clear the matching dirty bit.
Subsequent draws that do not change the category must reuse the existing
binding without re-uploading.

### 3.5 All-dirty on encoder reinit (`R-BACK-12.12`)

When the encoder starts a new render pass, switches to a pipeline whose
binding layout differs, or otherwise loses its sticky binding state, it must
treat every category as dirty and re-bind from the next draw onward. Dirty
state must not leak across encoder generations.

---

## 4. Slab and Push Lifetime

### 4.1 Reuse existing transient slab (`R-BACK-12.13`)

`VsConsts`, `PsConsts`, `FfpVsConsts`, and `FfpPsConsts` slices must be
sub-allocated from the same `CommandQueue::reserveTransientBuffer` /
`uploadTransientBuffer` pool used today for `DrawUniforms` and UP buffers.
A separate slab pool must not be introduced.

### 4.2 Sequence-id keyed reclaim (`R-BACK-12.14`)

Slices must be tracked through the existing `transientBufferAllocations_`
list and reclaimed by `reclaimTransientBuffersUnlocked` when the GPU
completion watermark passes the slice's seq-id. The same lifetime invariants
that govern `DrawUniforms` slab slices apply unchanged.

### 4.3 Push-constant lifetime (`R-BACK-12.15`)

`DrawVolatile` data is consumed by `setVertexBytes` inline; the host-side
buffer for the 16 B struct lives on the encoder thread stack and may be
overwritten as soon as the `setVertexBytes` call returns. No GPU lifetime
tracking is required for `DrawVolatile`.

---

## 5. Layout Invariants

### 5.1 Host-MSL layout match (`R-BACK-12.16`)

The host-side `VsConsts`, `PsConsts`, `FfpVsConsts`, `FfpPsConsts`,
`DrawVolatile` C++ struct layouts in `dxmt9_draw_state.hpp` must match the
MSL declarations emitted by `dxmt9_shader_sources.cpp::makeShaderPrelude()`
field for field, byte for byte. Drift is a regression; a unit test under
`tests/native/core` must enforce field offsets and total sizes.

### 5.2 Reflected in pipeline cache key (`R-BACK-12.17`)

`MTLBinaryArchive` and the pipeline cache key must continue to incorporate
the shader source hash so that a layout-changing revision automatically
invalidates stale entries. No additional invalidation logic is required.

### 5.3 Stable struct sizes documented (`R-BACK-12.18`)

Each of the five structures must declare a `static_assert` on its
`sizeof()` matching a specified value. The values are recorded in
`design.md` and updated only when the layout intentionally changes.

---

## 6. Performance Contract

### 6.1 Per-draw write bound (`R-BACK-12.19`)

In a `DrawRun` of N draws that does not change any tracked category, the
total bytes written to GPU shared memory by uniform-related work must be
`N * sizeof(DrawVolatile) = N * 16` plus a constant overhead independent of
N for the run-once stable slab build. The current default behaviour writes
`N * sizeof(DrawUniforms) ≈ N * 9080`, so the new contract is a >99%
reduction for state-stable runs.

### 6.2 Per-draw API call bound (`R-BACK-12.20`)

In a `DrawRun` of N draws that does not change any tracked category, the
encoder must emit at most `N * 2` Metal binding-class calls
(`setVertexBytes` + `drawPrimitives*`), with no `setBuffer*` calls beyond
the first draw of the run. State-changing draws may emit up to four
additional `setBuffer*` calls (one per dirty category), which is the
worst-case and must be rare in measured workloads.

### 6.3 Counter coverage (`R-BACK-12.21`)

The existing `[dxmt9-perf]` line must report:

- `uniform_vs_consts_bytes`, `uniform_ps_consts_bytes`,
  `uniform_ffp_vs_bytes`, `uniform_ffp_ps_bytes` — per-encoder uploaded
  byte totals so the dirty-gating model can be validated.
- `uniform_volatile_pushes` — count of `setVertexBytes` calls.

These extend `dxmt9_perf_counters.hpp` along the same pattern as the
existing `transient_upload_*` keys.

---

## 7. Out-of-Scope Constraints

The following are explicitly not required by this spec; capture them in
follow-up specs if adopted:

- Argument-buffer consolidation (DXMT-style packed argbuf at a single slot).
- `MTLHeap`-resident stable structs referenced via 64-bit GPU pointer
  through a Tier-2 argbuf.
- Range-heap coalescing of sparse shader-constant updates (Wine GLSL
  pattern). The simpler `maxChanged*` counter must be the implementation
  unless a workload demonstrates fragmentation that the simpler counter
  cannot exploit.
- Compile-time field-to-offset parameter binding (Wine vkd3d-shader
  pattern). The dxmt9 shader generator owns source emission directly, so
  the equivalent is the category routing in `dxmt9_shader_translator.cpp`.

These exclusions apply unless and until a measured workload shows the
simpler model insufficient.
