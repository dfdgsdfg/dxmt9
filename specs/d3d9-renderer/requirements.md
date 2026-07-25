---
type: "Spec Requirements"
title: "D3D9 Renderer (Modern Path) Requirements"
description: "D3D9 Renderer requirements and compatibility contracts."
tags: [specs, d3d9-renderer, requirements]
---

# D3D9 Renderer (Modern Path) Requirements

This spec defines a **modern rendering path** that consumes the same PE
`CommandChunk` stream as `specs/backend/` but produces Metal work through a
Frame Graph DAG, Object/Mesh shader scheduling, and GPU-driven indirect
dispatch. It is designed to **coexist with the existing 1:1 command-translation
path** (the current `specs/backend/` design). The renderer is selected
**once per process** at backend factory construction (R-BACK-31.1,
R-BACK-31.6) — there is no per-chunk or mid-frame switching. Per-app
selection is achieved by having the experiment runner read the
`experiments/CATALOGUE.toml` `render_mode` field for the app and inject
the resolved value into the spawned process's `DXMT9_RENDER_MODE`
environment variable before the wine process starts.

Scope boundary:

- `specs/d3d9/` continues to own D3D9 COM contracts and state-shadow semantics.
- `specs/backend/` continues to own the traditional 1:1 encoder path and the
  shared Metal infrastructure (PSO cache, ring allocators, command queue, shader
  translator, presenter).
- This spec adds a **second consumer** for committed chunks: the modern
  renderer. Both consumers share resource allocation, PSO cache, shader
  translation, command queue, completion fences, and presentation.

Motivation: see `docs/perfomance/overview-3dmark05-gt1.md`. GT1 on M1 is GPU
bound by a hidden vertex-stage / tiler / parameter-buffer write bucket
(~1.6 GiB/frame) that does not respond to source-visible varying width, render
state toggles, or primitive-order reorder probes. The modern path exists to
attack the remaining backend-shape and pass-store levers that the 1:1 path
cannot reach: pass coalescing, memoryless transient attachments, mesh-shader
dispatch, GPU-driven cull, and ICB-batched submission.

---

## 1. Scope and Goals

**R-BACK-30.1** The traditional renderer must remain available as an explicit
rollback. The default Wine runtime path is `framegraph + progressive` with only
the proven `passcoalesce` production feature enabled. Empty, `0`,
`traditional`, and invalid `DXMT9_RENDER_MODE` values select the traditional
1:1 backend (`specs/backend/requirements.md`). Memoryless, DCE, generic reorder,
mesh, bindless, object scheduling, and GPU-driven execution remain disabled
unless a later requirement and acceptance gate explicitly promotes them.

**R-BACK-30.2** The modern renderer must consume the same `CommandChunk` stream
the traditional path consumes. The PE side must not branch its recording shape
based on the active renderer; chunk format identity is the cross-renderer
contract.

**R-BACK-30.3** The modern renderer in **`strict` `compat_profile`** must
produce frames that are observably equivalent to the traditional path for
every D3D9-conformant draw sequence, subject to the precision and ordering
exceptions documented under §9. "Observable equivalence" means the same
swap-chain backbuffer contents at the boundary of each `Present` call
within the precision limits already accepted by `R-BACK-1.1`. The
`progressive` and `aggressive` profiles **opt out** of byte-exact
observable equivalence in exchange for bounded performance optimizations;
they retain functional D3D9 behavior (no crashes, no GPU faults, no
hangs, no resource lifetime violations, no application-visible HRESULT
divergence) but may diverge at the swap-chain boundary within the
explicit limits documented per feature (§4 stale-persistent artifact,
§5 reorder side-effects, §7.2 cull conservatism).

**R-BACK-30.4** The modern renderer must not regress D3D9 conformance
pass rates relative to the traditional path **for the `strict`
`compat_profile`**. The conformance suite must run in `traditional` and
in `framegraph` + `strict` modes with identical pass/fail counts.
Conformance under `progressive` and `aggressive` profiles is **not** a
release gate; those profiles are evaluated by separate per-feature
parity / route-equality gates (R-BACK-41.6) and by the explicit bounded
artifact contracts (§4 R-BACK-33.4, §5, §7.2).

**R-BACK-30.7** A `progressive` or `aggressive` feature that ships in a
user-facing default must clear its per-feature proof and wild-test artifact
contracts. The global `progressive` default contains only `passcoalesce`;
semantic-relaxation features such as memoryless are never implied by the
profile and still require an explicit per-app policy (R-BACK-40.1,
R-BACK-40.4).

**R-BACK-30.5** The modern renderer must be implemented behind a single
`IRenderBackend` interface that the traditional path also implements. Both
backends must be selectable at process startup; no runtime path may bypass the
interface.

**R-BACK-30.6** Goals explicitly **out of scope** for this spec:
- Replacing the traditional path.
- Modifying the D3D9 frontend or `CommandChunk` format.
- Changing the bridge ABI (winemetal commit calls).
- Introducing new Wine PE-side dependencies.

---

## 2. Dual-Path Coexistence

**R-BACK-31.1** The renderer must be selected by a single process-level
environment variable `DXMT9_RENDER_MODE` with values:

| Value | Meaning |
|---|---|
| `traditional` | the existing `specs/backend/` rollback path |
| `framegraph` | the modern path defined by this spec (default when unset), with feature subset selected by `DXMT9_RENDERER_FEATURES` |

Empty, `0`, and unparseable values must fall back to `traditional`. An
unparseable non-empty value must produce a single warning log line. Value
resolution must happen once at backend factory construction and must not change
for the process lifetime.

**R-BACK-31.2** The catalogue (`experiments/CATALOGUE.toml`) must accept a
`render_mode` field on each `[[app]]` entry with the same value set as
`R-BACK-31.1`. The catalogue value is consumed by the experiment runner
**before** the wine process starts; the runner reads the catalogue entry
for the app it is launching and injects the resolved value as the
`DXMT9_RENDER_MODE` environment variable in the process's environment block.
When `render_mode` is omitted, the runner injects `framegraph`. The runtime
itself never reads the catalogue: by the time the backend factory runs
(R-BACK-31.1), `DXMT9_RENDER_MODE` is already set to the catalogue's resolved
value or a user override. A direct `wine app.exe` launch with the variable
unset reaches the same `framegraph` default in the runtime resolver.

**R-BACK-31.3** A new env var `DXMT9_RENDERER_FEATURES` must accept a
comma-separated list of feature tokens that enable individual modern-path
features when `DXMT9_RENDER_MODE=framegraph`. Tokens:

| Token | Feature |
|---|---|
| `dce` | dead-pass elimination (chunk-conservative; off when token absent) per spec.md §5.1 |
| `passcoalesce` | same-RT / depth re-entry coalescing per §5 |
| `memoryless` | transient RT virtual-attachment promotion per §4 |
| `objectschedule` | object shader as draw scheduler per §6 |
| `mesh` | mesh shader draw path per §6 |
| `gpudriven` | ICB-based indirect dispatch per §7 (CPU-prefilled when `objectschedule` absent; GPU-emitted when `objectschedule` present) |
| `bindless` | bindless texture/sampler heap per §8 |

Unknown tokens must be ignored with a single warning log line. Under the
default progressive profile, an unset variable enables only `passcoalesce`.
An empty string or `0` enables no modern features; in that state the modern
backend must behave as a thin wrapper that issues the same Metal calls as the
traditional backend. Under `strict`, every token is rejected and the resolved
feature set is empty.

**R-BACK-31.4** Feature tokens must enforce these dependencies:

- `mesh` requires `bindless`.
- `gpudriven` requires `mesh` and `bindless`.
- `objectschedule` requires `mesh` and `bindless` but does **not** require
  `gpudriven`.

`objectschedule` without `gpudriven` runs the scheduler on the CPU and emits
mesh dispatches through the CPU-side Metal API; the CPU-side prototype is the
L2 acceptance shape (§12). `objectschedule` with `gpudriven` runs the
scheduler as a GPU compute pre-pass and emits mesh dispatches through ICB;
that is the L3 acceptance shape. Selecting a dependent token without its
dependency must fail backend construction with a clear log line; the process
must then fall back to `traditional`.

**R-BACK-31.5** A per-draw fallback policy must be exposed as
`DXMT9_RENDERER_FALLBACK={allow|strict}` (default `allow`). Under `allow` the
modern path may route an individual draw through the traditional encoder API
inside the same Metal render pass when the draw is not compatible with the
selected modern features. Under `strict` an incompatible draw must fail the
entire chunk to the traditional path. Compatibility classifiers per §6 / §7.

**R-BACK-31.8** Per-draw fallback under `allow` must execute inside the
`MTLRenderCommandEncoder` the modern path opened for the current pass — the
modern linearizer owns the encoder lifecycle. The backend must therefore
expose an **`IExternalDrawEmitter`** contract on the
`ArgumentEncodingContext` defined in `specs/backend/spec.md` §4: the AEC
must split its draw-emission code path from its pass-lifecycle code path so
the modern linearizer can call the draw-emission path with an
externally-owned encoder argument. The traditional path keeps its existing
ownership shape; the external emitter is an additional entry point, not a
replacement.

The fallback API must also pass an **`ExternalPassContext`** carrying the
per-pass and per-queue state the AEC traditionally maintains and on which
the draw-emission code depends: the touched-color / touched-depth
attachment set defined by `specs/backend/render-pass-actions/requirements.md`
§5, the cross-frame resource-retention map, the active load/store action
state for the current encoder, the queue-local `completedSeqId`, and any
PSO-cache / sampler-cache lookups that are normally satisfied by AEC's own
state. Without this context the "same Metal call sequence" parity
guarantee in R-BACK-30.3 and R-BACK-39.1 is not enforceable: the load/store
proofs, hazard tracking, and retention contracts in
`specs/backend/requirements.md` R-BACK-2.* and
`specs/backend/render-pass-actions/requirements.md` R-BACK-15.* depend on
state the external emitter must observe and update. If the modern path
cannot construct an `ExternalPassContext` faithful to the current chunk
(for example, mid-pass fallback after `passcoalesce` reorder broke a
touched-set assumption), the renderer must fall back the **whole render
pass**, not a single draw — emitting the entire pass through the
traditional path while still owning the surrounding chunk DAG. Design.md
§15 specifies the contract; the pass-level escape valve is spec.md §15.5.

**R-BACK-31.6** Switching renderers between presents is not supported. The
backend factory resolves the renderer at process init and the choice persists
for the lifetime of the device. Hot-swap between renderers within a frame is
explicitly disallowed.

**R-BACK-31.7** Both renderers must share the unix-side `CommandQueue`, the
PSO cache, the shader translator, the ring allocators, the presenter, and the
completion fence layer defined in `specs/backend/requirements.md`. The modern
path must not allocate a parallel command queue or a parallel presenter.

**R-BACK-31.9** Sharing the PSO cache, the shader cache, and the
`MTLBinaryArchive` between the traditional and modern paths must **not**
collide their entries. Every cache key consumed by either path must
include a **renderer-variant namespace tag** (`RendererVariantKey`):

| Component | Tag |
|---|---|
| backend mode | `Traditional` / `FrameGraph` |
| shader stage | `Vertex` / `Pixel` / `Mesh` / `Object` (mesh and object are FrameGraph-only) |
| feature variant | bitmask of active feature tokens that affect shader codegen (`bindless`, `mesh_clusters`, `gpudriven`) |
| layout variant | `argbuf_hybrid_mode`, `tile_ffp_mode`, etc. (existing backend variant bits) |

The `RendererVariantKey` must be a leading component of every cache key —
PSO, shader IR, MSL source, `MTLBinaryArchive` entry — so that a
`traditional` cache lookup can never resolve to a modern-path compiled
variant and vice versa. Existing backend variant bits (e.g.
`ShaderVariantKey::argbufHybridMode`) are subordinate to the
`RendererVariantKey`; this avoids any modern-path lane silently
overwriting or shadowing a traditional-path entry in the on-disk archive
or in the in-memory cache. The L0 refactor (§12, R-BACK-41.1) lands
this key extension together with the renderer factory; see spec.md
§16 for the shared-cache namespace implementation.

---

## 3. Frame Graph Semantics

**R-BACK-32.1** When the modern path is active, the chunk consumer must build
one Frame Graph DAG **per `CommandChunk`**. The DAG window covers exactly the
records delivered in a single `onChunkReady` call. Multiple chunks must not
merge into a shared DAG even when no `Present` separates them, and a single
chunk that spans a `Present` boundary must keep its `Present` record as one
node inside the DAG. Nodes represent passes (render, compute, blit, present,
sync). Edges represent resource read/write dependencies internal to the chunk.

**R-BACK-32.8** Per-chunk fence semantics from
`specs/backend/requirements.md` (chunk `completedSeqId`, deferred resource
reclaim, query and readback wait) must hold under the modern path
unchanged. The linearizer must submit each chunk's optimized DAG as a single
ordered Metal call sequence whose final completion advances the same
`completedSeqId` the traditional path would advance for that chunk;
cross-chunk reordering is forbidden, and chunks must be processed in
submission order. The Frame Graph must not defer or coalesce a fence signal
across the chunk boundary.

**R-BACK-32.2** Frame Graph construction must be deterministic: the same chunk
input and retained resource-alias mapping must always produce the same DAG, the
same optimizer outcome, and the same Metal call sequence. The renderer must not
depend on wall clock, thread scheduling, or any non-deterministic source.

**R-BACK-32.3** The Frame Graph must track per-resource access logs. For every
texture, depth buffer, and buffer referenced by a chunk record, the graph
records the pass index, the access kind (`read|write|read_write|preserve|
clear`), and the stage (`vertex|fragment|compute|copy`). Resource handles use
the same opaque-handle space as the traditional path. A render-target or
depth/stencil surface that aliases a texture must use the owning texture handle
as its hazard identity, because attachment writes name the surface handle while
shader reads name the texture handle. Standalone surfaces retain their surface
handle. This canonicalization must not change `AttachmentSet`, which continues
to identify the exact bound surfaces used for pass compatibility.

**R-BACK-32.9** The Frame Graph dependency edge set must be a complete hazard
model: it must record **true (RAW: write → later read)**, **anti (WAR: read →
later write)**, and **output (WAW: write → later write)** dependencies between
passes for every resource. This is a correctness contract for the
edge-consuming optimizer passes: `reorder` (R-BACK-32.5 / design §5.6) orders
passes by the edge set and `passcoalesce` (R-BACK-34 / design §5.4) relocates
intervening passes by reachability over the edge set, so a RAW-only edge set
would let either pass move a write before a prior read or swap two writes and
violate the observable-equivalence contract (R-BACK-30.3). Every edge points
`earlier_pass → later_pass`; self-edges (same pass) and duplicate
`(src, dst, resource)` edges are omitted. A `progressive`/`aggressive` profile
must not enable `reorder` or `passcoalesce` unless the active builder produces
WAR and WAW edges (a builder that emits RAW only must keep both passes
disabled). The DAG observe/debug-export path (R-BACK-39.7) does not consume
edges, so it is unaffected by edge completeness.

**R-BACK-32.4** The Frame Graph must finalize the current chunk's DAG when any
of these boundaries occur inside the chunk: an in-chunk `Lock`/`Unlock`
record, a `GetRenderTargetData` record, a `StretchRect` that crosses
CPU-visible memory, a `Present` record, or an explicit
`DXMT9_RENDERER_FLUSH_PRESENT_INTERVAL=N` debug override. End-of-chunk is
always a finalize boundary by `R-BACK-32.1`. Finalize serializes the DAG up
to that boundary; remaining records in the same chunk start a fresh sub-DAG
that submits before the next chunk's `onChunkReady`.

**R-BACK-32.5** The Frame Graph optimizer must run a fixed pipeline of passes
in this order:

1. Resource lifetime computation (first-use → last-use within chunk).
2. Pass coalescing (§5).
3. Memoryless virtual-attachment promotion (§4) — runs **after**
   passcoalesce so single-pass collapse is observable; promotion candidates
   are decided here, alias allocation happens here.
4. Dead-pass elimination (chunk-conservative; §spec.md 5.1) — runs
   **after** memoryless so it can use memoryless-eligibility as one of
   its cross-chunk safety gates; off when the `dce` feature token is
   absent.
5. Pass reordering within dependency edges.
6. Load/store action selection — runs **after** reorder, because reorder
   can change which pass is the first or last access of an attachment.
   Running load/store before reorder would mis-select `Load`, `Store`, or
   `DontCare` for the final linearized order.

Each optimizer pass must be independently switchable by a feature token;
pass order must not change behind a token; turning a pass off must produce a
correct, slower graph. The reordering follows from R-BACK-33.2's
requirement that memoryless eligibility check the single-Metal-pass
collapse (which only exists after passcoalesce), spec.md §5.1's
requirement that DCE consult memoryless eligibility for cross-chunk
safety, and the load/store contracts in
`specs/backend/render-pass-actions/requirements.md` §3-§6 which depend on
first/last access of the final pass order.

**R-BACK-32.6** The Frame Graph must produce **identical Metal output** to the
traditional path when no features are enabled (`DXMT9_RENDERER_FEATURES=`).
This identity is the parity baseline used by §10 validation.

**R-BACK-32.7** Frame Graph emission must use the existing unix-side encoder
APIs in `specs/backend/spec.md` §4-§5. The graph linearizes the optimized
DAG and calls the same encoder primitives the traditional path calls. Pass
boundaries map 1:1 to `MTLRenderCommandEncoder` lifetimes per `R-BACK-2.4`.

---

## 4. Memoryless Transient Attachments (Virtual Aliases)

**R-BACK-33.1** The modern path must **not** change the storage mode of any
D3D9-allocated `MTLTexture`. The shared `ResourceAllocator` defined in
`specs/backend/requirements.md` R-BACK-5.* fixes storage mode at creation and
keeps `MTLStorageModePrivate` for D3D9 RT/DS surfaces for the resource
lifetime. The modern path's memoryless optimization is expressed as a
**per-pass virtual attachment**, allocated from a separate transient
attachment pool, not as a residency change on the persistent D3D9 resource.

**R-BACK-33.2** Eligibility for virtual-attachment routing is gated by
**prior-frame observation**, not by future-chunk prediction. A D3D9 RT/DS
surface becomes a memoryless candidate only when, across the most recent
`DXMT9_RENDERER_MEMORYLESS_OBSERVATION_FRAMES` frames (default `8`), **all**
of the following held in **every** observed frame:

- no `IDirect3DSurface9::LockRect` / `UnlockRect` referenced the surface,
- no `IDirect3DDevice9::GetRenderTargetData` referenced the surface,
- no `StretchRect` source or destination outside the producing render pass
  referenced the surface,
- the surface stayed allocated `D3DPOOL_DEFAULT`,
- the surface was not bound as the swap-chain backbuffer,
- the surface was written and read only within a single coalesced Metal
  render pass per chunk (no cross-pass or cross-chunk read of the
  surface's stored contents),
- the surface was either reallocated or fully overwritten before the next
  read (so its content is not consumed across frames).

The renderer must not promote a surface until the full observation window
of N frames has passed; the first N frames after device creation cannot
promote. Per-app `compat_profile=strict` short-circuits this gate to
"never promote" (§11). A surface that fails any frame's observation must
reset its observation counter to zero.

**R-BACK-33.3** When R-BACK-33.2 admits a surface and the in-chunk DAG
shows the producing pass and every consuming pass collapse into a single
Metal render pass after `passcoalesce` (§5), the renderer must allocate a
per-pass `MTLStorageModeMemoryless` **alias texture** from a transient
attachment pool sized to the pass's attachment footprint. The pass's
color / depth writes target the alias; the persistent backing receives no
writes for the pass. The persistent surface allocator is not modified or
reallocated. The transient pool reuses alias textures across passes within
a chunk only when descriptor shape (format, sample count, width, height,
usage) matches exactly.

`MTLStorageModeMemoryless` content **does not survive a Metal render pass
boundary**. A surface whose DAG does not collapse to one pass cannot be
routed through an alias; it must use its persistent backing.

**R-BACK-33.4** Post-fact recovery via alias→persistent blit is
**impossible**: by the time a later chunk observes the persistent backing,
the alias has been discarded by Metal and its contents are gone. The
renderer must not encode such a blit. The correctness contract is instead:

- On observed misclassification (a Lock / readback / sample of the
  persistent backing arrives after promotion), the current chunk's
  persistent backing is **stale**; the application sees the value the
  persistent texture held before the aliased pass. The renderer must
  not pretend otherwise.
- The misclassified surface must immediately reset its observation
  counter to zero (R-BACK-33.2). It cannot be promoted again until a
  full new observation window passes.
- The misclassification incident must increment a counter so the artifact
  is observable in benchmarks (§10).

This is a **bounded, one-incident visual artifact** acceptable under
`progressive` and `aggressive` profiles; `strict` profiles eliminate it by
never promoting (R-BACK-38.6). Per-app catalogue entries with
`compat_profile=aggressive` and the required semantic relaxation accept the
risk. The default progressive profile does not enable memoryless
(R-BACK-40.1).

**R-BACK-33.5** A same-pass tile-shader copy from alias to persistent — run
inside the encoder before pass end — is the only correctness-safe recovery
path available on Apple Silicon TBDR, but it doubles the pass's
fragment-stage work and defeats the bandwidth motivation. The current spec
does **not** require this path; it is recorded in spec.md §16 as an open
research path for a future layer.

**R-BACK-33.6** When `memoryless` is disabled, the optimizer must not
allocate alias textures and must route all writes to the persistent backings.
The feature is a pure opt-in optimization; with the feature off, the modern
path's RT/DS routing must be byte-identical to the traditional path.

**R-BACK-33.7** A counter family `framegraph_virtual_attachment_emitted`,
`_observation_frames_completed`, `_promotion_blocked_observation`,
`_dropped_via_lock`, `_dropped_via_readback`,
`_misclassification_stale_persistent`, and `_bytes_avoided_per_chunk`
must be emitted per §10. The `_bytes_avoided` accounting must not
double-count when two aliases reuse the same pool slot within a chunk.
`_misclassification_stale_persistent` is the canonical correctness gauge
for benchmark regression review; non-zero values must be investigated.

See `docs/perfomance/render-pass-store/render-pass-store-memoryless.01.md`
for the open design discussion that motivated this section.

---

## 5. Pass Coalescing

**R-BACK-34.1** When the `passcoalesce` feature is enabled, the optimizer must
identify pass pairs `(P_a, P_b)` where `P_a` and `P_b` write the same RT and
the same depth attachment and where every pass between them in the DAG has no
dependency edge to `P_a` or `P_b`. Such pairs must be candidates for
coalescing into a single Metal render pass.

**R-BACK-34.2** A candidate must be coalesced when:
- the intervening passes can be reordered before `P_a` or after `P_b` without
  violating any dependency edge,
- the intervening tile preservation cost (estimated from
  `render_pass_tile_preservation_bytes` per-pass) exceeds the additional pass
  reorder cost (zero for true reorder, otherwise sum of altered store/load
  bytes).

The cost comparison must use a single fixed-precision integer arithmetic; the
optimizer must not depend on floating-point cost models.

**R-BACK-34.3** When two passes coalesce, the resulting render pass must use
`MTLLoadActionLoad` on the first record and `MTLStoreActionStore` on the
last record unless §4 promotes the attachment to memoryless. Intermediate
draws must execute in their original submission order (no draw reordering
inside a coalesced pass).

**R-BACK-34.4** Coalescing must be observable through a counter family
`framegraph_pass_coalesced_count`, `_bytes_avoided`, and
`_reorder_distance_max`. A pass that coalesces must record the maximum
reorder distance applied so a long-distance reorder can be inspected.

**R-BACK-34.5** A pass that crosses a §3.4 flush boundary cannot be coalesced
with anything on the other side of the boundary. The optimizer must enforce
this without exception.

---

## 6. Object / Mesh Shader Path

**R-BACK-35.1** When `mesh` is enabled, the renderer must compile, for selected
D3D9 vertex shader hashes, a **mesh shader pipeline state object** in addition
to the existing translated vertex/fragment PSO. The mesh PSO accepts a
`DrawDescriptor` (defined in `spec.md`) as input and produces vertices and
primitives into mesh shader output buffers consumed by the fragment shader.

**R-BACK-35.2** Mesh shader compilation must use a **cluster strategy**: D3D9
vertex shaders are grouped by signature (FVF, output varying width, instruction
count bucket) into clusters; each cluster shares a templated mesh shader that
reads a per-draw uniform indirection. Clusters that prove insufficient for a
specific shader hash may promote to a per-hash mesh PSO; promotion must be
recorded as `framegraph_mesh_pso_promoted` and capped by
`DXMT9_RENDERER_MESH_PSO_CAP` (default `64`).

**R-BACK-35.3** L2 / L3 (§12) mesh-eligibility is intentionally narrow at
spec landing. A draw is **mesh-eligible at L2/L3** when **all** of:

- the primitive topology is `TRIANGLELIST` (`D3DPT_TRIANGLELIST`);
  `TRIANGLESTRIP`, `TRIANGLEFAN`, `LINELIST`, `LINESTRIP`, `POINTLIST`
  are **not** L2/L3-eligible at first landing and must take the
  traditional encoder path,
- the draw is **indexed** (`DrawIndexedPrimitive*`); non-indexed
  `DrawPrimitive*` is **not** L2/L3-eligible at first landing,
- the vertex declaration has no stream output / transform feedback semantics,
- the vertex shader exists or is FFP-generated (FFP allowed),
- no software vertex processing is required,
- the primitive count exceeds `DXMT9_RENDERER_MESH_PRIMITIVE_THRESHOLD`
  (default `64`),
- the draw's resource binding footprint fits the modern path's
  `DrawDescriptor` shape (see spec.md §3.3): VB stream count ≤ D3D9
  `MaxStreams` (`16`, per `specs/d3d9/caps/`), texture binding count ≤ D3D9
  sampler stage count (`20` = 16 PS + 4 VS, per
  `specs/d3d9/requirements.md` `R-CORE-*` sampler stage contract), and
  sampler state count ≤ same. The `DrawDescriptor` shape must size every
  binding field at D3D9 maxima so the eligibility check is a cap test, not
  an under-sized descriptor fallback.

The topology / non-indexed restriction is a deliberate landing narrowness:
mesh-shader codegen, primitive-assembly templates, and
`DrawDescriptor.ib_count`-based dispatch sizing all assume indexed
triangle-list. Future expansion to TRIANGLESTRIP and non-indexed draws is
recorded as a separate descriptor extension in spec.md §3.3 / §16 open
questions; until that expansion lands and ships acceptance evidence per
R-BACK-41.6, every non-`TRIANGLELIST` / non-indexed draw must take the
traditional encoder path under `R-BACK-31.5` `allow` mode.

Below the primitive threshold or outside the descriptor footprint or
outside the topology / indexed subset, the draw must take the traditional
encoder path under `R-BACK-31.5` `allow` mode.

**R-BACK-35.4** Object-scheduling runs in one of two modes depending on
which feature tokens are present together (R-BACK-31.4):

- **CPU-side mode** — `objectschedule` enabled, `gpudriven` **not** enabled:
  the scheduler runs on the unix-side encode thread immediately before
  each mesh-eligible pass is linearized. It iterates the per-pass
  `DrawDescriptor` array on the CPU, performs frustum culling against the
  bounding box per descriptor, performs cross-draw batching subject to
  R-BACK-35.6, and emits mesh dispatches through the CPU-side Metal API
  (`MTLRenderCommandEncoder.drawMeshThreadgroups`). **No object shader
  compute pre-pass runs and no ICB is allocated.**
- **GPU-side mode** — `objectschedule` **and** `gpudriven` both enabled:
  an object shader runs as a compute pre-pass at the start of every
  mesh-eligible render pass. It receives the per-pass `DrawDescriptor`
  array, performs the same frustum culling and batching, and emits mesh
  dispatches into an ICB. `executeCommandsInBuffer` then runs the ICB.

The CPU-side and GPU-side modes share the algorithm but differ on where
it executes. The L2 acceptance shape in §12 corresponds to CPU-side mode;
L3 corresponds to GPU-side mode. Selecting `objectschedule` alone (without
`gpudriven`) must **not** trigger any GPU compute pre-pass or ICB
allocation.

**R-BACK-35.5** Object shader culling must be **conservative**: a draw
may only be culled when **both** (a) the draw has a valid bounding box
from one of the explicit sources documented in spec.md §3.3
(`flags.bbox_valid == 1`), **and** (b) that bounding box has no overlap
with the view frustum. Marginal cases (touching frustum planes) must
default to dispatch. A draw without a valid bbox
(`flags.bbox_valid == 0`) must always dispatch — guessing a bbox would
silently drop visible draws and break D3D9 correctness. The CommandChunk
contract per R-BACK-30.6 is preserved; bbox population is the modern
path's own responsibility from one of the spec.md §3.3 sources, not a
PE-side addition. Culling metrics must surface as `framegraph_object_cull_*`
counters (§10), and the no-valid-bbox case must increment
`framegraph_object_cull_skipped_no_bbox` so the lost-win can be
measured separately from cull-survivor draws.

**R-BACK-35.6** D3D9 draw-call order and per-draw side effects must be
preserved. The default object shader behavior is **one mesh dispatch per
D3D9 draw call**. Cross-draw batching of mesh dispatches into a single
dispatch is permitted **only** when the optimizer proves the batched draws
are order-independent under all of:

- identical PSO and identical render state,
- identical bindless texture / sampler key set,
- depth test enabled, depth write enabled, depth function strict-less or
  strict-greater (opaque depth-writer set),
- no blend (blend disabled or `BlendOp=Add` with `SrcBlend=One` /
  `DestBlend=Zero`),
- no stencil write,
- no scissor or clip-plane state change,
- no `discard_fragment()` source in the fragment shader.

A draw outside this strict subset interrupts batching; the next contiguous
run must start as a fresh batch under the same proof. Order-dependent
side-effect classes — transparency, additive blend, alpha-test discard,
stencil writes — must dispatch one D3D9 draw per mesh dispatch even when
all other batching criteria are satisfied.

**R-BACK-35.7** A render pass may mix mesh and traditional draws under
`R-BACK-31.5` `allow` mode. The mixed-path encoder must end the object shader /
mesh dispatch run, switch to traditional encoder API for the incompatible
draw, then resume mesh dispatch for the next eligible run.

---

## 7. GPU-Driven Dispatch (ICB)

**R-BACK-36.1** When `gpudriven` is enabled, the renderer must emit
`MTLIndirectCommandBuffer` instances executed by `executeCommandsInBuffer`
at the end of each render pass. The ICB population path depends on whether
`objectschedule` is also enabled:

- `gpudriven` **without** `objectschedule`: the CPU pre-fills the ICB on
  the encode thread by iterating the per-pass `DrawDescriptor` array and
  encoding indirect commands directly. No object shader compute pre-pass
  runs.
- `gpudriven` **with** `objectschedule`: the object shader compute
  pre-pass (spec.md §7.2) reads `DrawDescriptor[]`, performs frustum
  cull and batching, and writes indirect commands into the ICB.
  `executeCommandsInBuffer` then runs the ICB.

ICB size must be sized at construction to the worst-case draw count from
the pass `DrawDescriptor` array; allocation must not grow during pass
execution.

**R-BACK-36.2** ICB PSO selection must use Metal's actual API contract,
not a pre-bound "indirection table". The Metal `MTLIndirectRenderCommand`
type exposes its own `setRenderPipelineState:` method
(`MTLIndirectCommandEncoder.h`), and `MTLIndirectCommandBufferDescriptor`
exposes only `inheritPipelineState` (`MTLIndirectCommandBuffer.h`).
The renderer must choose **one** of these two modes per pass and record
which mode was chosen as a counter dimension:

- **Per-command PSO encode** (default): each `MTLIndirectRenderCommand`
  encoded into the ICB carries its own `setRenderPipelineState:` call,
  selecting the mesh PSO appropriate for that command's source draw.
  This is the path that supports a heterogeneous mesh PSO mix inside
  one pass. The renderer must enable the corresponding ICB descriptor
  bit set in `MTLIndirectCommandTypes` so the per-command PSO encode
  is legal at ICB build time.
- **Single-PSO subpass with inheritance**: when the optimizer determines
  every mesh-eligible draw in a pass uses the same mesh PSO, the ICB
  may be encoded with `inheritPipelineState = YES` and rely on the
  outer `MTLRenderCommandEncoder.setRenderPipelineState:` call. This
  path produces smaller ICB entries and is observed via
  `framegraph_icb_subpass_single_pso`. A pass whose mesh PSO set is
  not size-1 must use the per-command PSO encode mode for the whole
  pass.

A pre-bound "PSO indirection table" outside `MTLIndirectRenderCommand`
is **not** a Metal-supported concept and must not appear in the
implementation. Sub-pass splits, when used, are a CPU-side splitting of
the pass `DrawDescriptor` array, not a Metal-side PSO table cap.

**R-BACK-36.3** ICB execution must not cross render pass boundaries. Each
render pass has its own ICB; cross-pass batching is the optimizer's job
(§3, §5), not the GPU dispatcher's.

**R-BACK-36.4** When `gpudriven` is disabled, the modern path must not allocate
ICB instances. The selected `mesh` and `objectschedule` features without
`gpudriven` must encode mesh dispatches through the CPU-side direct API.

---

## 8. Bindless Infrastructure

**R-BACK-37.5** Mesh / GPU-driven paths consume raw GPU addresses for
vertex buffers, index buffers, and constant buffers stored inside
`DrawDescriptor` (spec.md §3.3 `ib_pointer`, `vb_pointers`,
`vs_cb_pointer`, `ps_cb_pointer`). These raw-pointer buffers do **not**
acquire residency through the bindless heap defined by R-BACK-37.1; the
modern path must therefore explicitly track and assert residency for
every buffer it points the GPU at. At each render pass open, the
linearizer must:

- compute the set of unique buffer handles referenced by any
  `DrawDescriptor` in the pass's slice (IB + each non-zero VB stream +
  each non-zero constant buffer pointer);
- call `encoder.useResource(buffer, .read, stage_mask)` for every
  handle in that set, with `stage_mask` covering the consumer stages
  (vertex for VB/IB, vertex+fragment for cbuf depending on which
  shader stage consumes the constants);
- register every handle in the chunk-local **retained handle set** so
  the same cross-frame retention contract that `specs/backend/` enforces
  (`R-BACK-2.*` retention map, deferred destroy via
  `Pool::reclaimCompleted()`) applies identically to mesh-path
  resources;
- include these handles in the per-pass hazard tracking so a later
  same-pass read-after-write conflict triggers a hazard split exactly
  the way the traditional path's `R-BACK-2.4` exact-tracking does.

The `useResource` calls must be deduplicated within the pass (no
duplicate `useResource` per handle), and across mesh+traditional draws
in a mixed pass the linearizer must merge the mesh-path residency set
with the AEC's traditional-path residency set so the encoder receives
one combined call per handle. Buffers backed by `MTLHeap` (the existing
small-resource heap pool from `R-BACK-5.9`) must still be made resident
via the heap-level `useHeap` call the traditional path already issues;
the modern path must not bypass that call.

**R-BACK-37.6** Mesh-path buffer residency reuses the existing
`specs/backend/` residency infrastructure: the unix-side `Pool` owns
the buffer handles, the chunk-local retained handle set extends
`Pool::retainForChunk`, and the deferred destroy queue (`destroyPending`)
sees mesh-path retention as one more contributor. The modern path must
**not** introduce a parallel buffer lifetime manager or a parallel
ring allocator for the same buffer handles. Tests must verify that a
buffer referenced by a mesh dispatch and a traditional draw in the same
pass appears in exactly one residency call and in exactly one retained
handle set entry.

**R-BACK-37.1** When `bindless` is enabled, the renderer must maintain a
per-frame **bindless texture heap** and a **bindless sampler heap**.
`DrawDescriptor` (spec.md §3.3) stores per-stage slot identifiers wide
enough to address the configured heap caps from R-BACK-37.3
(`BINDLESS_TEXTURES_MAX` default `4096`, `BINDLESS_SAMPLERS_MAX` default
`1024`): the implementation must use at least `uint16_t` for both texture
and sampler slot fields, and may extend to `uint32_t` if R-BACK-37.3
caps are raised. `uint8_t` slot fields are **not** sufficient and are
explicitly disallowed because the sampler cap exceeds `255`. The
`DrawDescriptor` must additionally carry a per-draw
`bindless_key_hash` (`uint64_t`) that hashes the active texture+sampler
binding set in stable canonical order; the object shader (spec.md §7.3)
uses this hash for the R-BACK-35.6 batching predicate without
re-walking the slot arrays. Heap residency must follow the residency
contract in `specs/backend/requirements.md` for shared resources; the
modern path must not introduce a parallel residency model.

**R-BACK-37.2** Bindless heap entries must be deduplicated per frame: the same
D3D9 texture or sampler bound to multiple draws must occupy a single heap
slot. Slot reuse across frames is allowed when the underlying handle is
unchanged.

**R-BACK-37.3** Bindless heap size must be capped by
`DXMT9_RENDERER_BINDLESS_TEXTURES_MAX` (default `4096`) and
`DXMT9_RENDERER_BINDLESS_SAMPLERS_MAX` (default `1024`). Overflow must fall
back the offending draw to the traditional path under `R-BACK-31.5` `allow`
mode; under `strict` mode it must fail the chunk.

**R-BACK-37.4** The bindless infrastructure must not require Metal 3 if mesh
shaders are disabled. Argument buffer Tier 2 must be sufficient for the
plain bindless path without mesh.

---

## 9. D3D9 Semantic Preservation

**R-BACK-38.1** All D3D9 ordering, refcount, and visibility contracts in
`specs/d3d9/requirements.md` must hold under the modern path. Frame Graph
reordering and pass coalescing must respect those contracts; an unprovable
case must default to the traditional path under `allow` or fail the chunk
under `strict`.

**R-BACK-38.2** Occlusion queries and event queries must be honored at their
issue point. The Frame Graph must not reorder a query through a draw it
observes. Query results must be readable through the same
`IDirect3DQuery9::GetData` semantics regardless of renderer.

**R-BACK-38.3** A `IDirect3DSurface9::LockRect` of a render-target surface
must produce the same byte contents the traditional path would produce.
Memoryless promotion (§4) is conditioned on the surface never being locked.

**R-BACK-38.4** `Present` must always emit a present command in submission
order. The Frame Graph may merge passes within a present interval but must
never delay the present itself or change its visible timing.

**R-BACK-38.5** Transparent geometry ordering must be preserved. Object
shader batching (§6.6) may not reorder draws within a render pass; this
preserves D3D9 transparency ordering by construction.

**R-BACK-38.6** When an app sets a `compat_profile` of `strict` in the
catalogue, every optimizer pass that could reorder, merge, alias, or change
storage residency must default to off. `strict` profiles must be functionally
equivalent to **feature-set empty**: the optimizer runs only the load/store
action selection pass (which restates the rules from
`specs/backend/render-pass-actions/requirements.md` without further mutation),
with no DCE, no coalesce, no reorder, and no virtual-attachment alias
allocation. `passcoalesce` itself is reorder/merge and therefore is **not**
part of the `strict` baseline.

---

## 10. Validation, Parity, and Observability

**R-BACK-39.1** A **parity test harness** must accept the same D3D9 capture
input and replay it through both renderers, comparing:
- swap-chain backbuffer contents at every present (`exact`, `lsb1`, or
  `ssim` policy as in the experiments framework),
- `dxmt9-perf` counter family equality (the subset that is renderer-mode
  invariant must match exactly),
- D3D9 `Lock` contents for any surface accessed during the capture.

Parity tests must be runnable from CI per `R-BACK-39.4`.

**R-BACK-39.2** New counters reserved for the modern path must follow the
`framegraph_*` prefix. Required counters at minimum:

```
framegraph_pass_count
framegraph_pass_coalesced_count
framegraph_pass_bytes_avoided
framegraph_virtual_attachment_emitted
framegraph_virtual_attachment_observation_frames_completed
framegraph_virtual_attachment_promotion_blocked_observation
framegraph_virtual_attachment_dropped_via_lock
framegraph_virtual_attachment_dropped_via_readback
framegraph_virtual_attachment_misclassification_stale_persistent
framegraph_virtual_attachment_bytes_avoided_per_chunk
framegraph_object_cull_emitted
framegraph_object_cull_dropped
framegraph_object_cull_skipped_no_bbox
framegraph_object_cull_batch_runs
framegraph_mesh_dispatch_count
framegraph_mesh_pso_promoted
framegraph_icb_commands_emitted
framegraph_icb_subpass_splits
framegraph_icb_subpass_single_pso
framegraph_icb_subpass_multi_pso
framegraph_bindless_texture_unique
framegraph_bindless_sampler_unique
framegraph_bindless_overflow_traditional_fallback
framegraph_draw_fallback_traditional
framegraph_draw_fallback_whole_pass
framegraph_draw_fallback_strict_failed
framegraph_chunk_fallback_mid_pass_discovery
framegraph_dce_dropped
framegraph_dce_preserved_unprovable
framegraph_optimizer_dag_build_us
framegraph_optimizer_optimize_us
```

The `framegraph_virtual_attachment_*` family is the single canonical name set
for §4's virtual-attachment optimization. Earlier drafts referenced
`framegraph_memoryless_rt_*`; that name is **not** used and must not appear
in counter table, callsite, audit, or `docs/perfomance` references.

Counter additions must follow the audit gate in
`scripts/check/audit_perf_counter_callsites.py` (one production callsite per
counter or removed from the table).

**R-BACK-39.3** Per-frame divergence logging must be available behind
`DXMT9_RENDERER_LOG_DIVERGENCE=1`. When set, the modern path must run a
**dry-run recorder** of the traditional path in parallel — never the
real traditional path. The dry-run recorder reproduces the
`TraditionalBackend` decision sequence (PSO selected, load/store action
chosen, encoder split point, etc.) by walking the same chunk through a
recorder-only adapter that **must not** call any Metal API, must not
allocate any `MTLCommandBuffer`, must not invoke the presenter, must not
mutate the PSO/shader cache or `MTLBinaryArchive`, must not touch the
`MTLHeap` residency or retained handle sets, and must not update
queue-level fence state. The recorder operates on a sidecar copy of the
state the traditional path would have read — state the
`TraditionalBackend` exposes through a new `IDecisionRecorder` interface
in `specs/backend/spec.md` — so capturing it has zero observable
side-effect on the shared queue, cache, presenter, or resource
infrastructure. The modern path then compares its own decision sequence
against the recorder's and logs each divergence point with chunk-record
index and decision kind. Divergence logging is for development only and
is not required to be production performance neutral; it is, however,
required to be **side-effect neutral** so a divergence run never
modifies shared state that a non-divergence run would observe.

**R-BACK-39.4** Continuous Integration must run the conformance suite under
both `traditional` and `framegraph` modes. Each mode's results must be
recorded separately in CI artifacts. A mode-specific regression must block
merge of any change that affects the renderer.

**R-BACK-39.5** The wild-test catalogue (`experiments/CATALOGUE.toml`) must
support per-app `render_mode` override so the renderer can be A/B-tested per
title. The override is resolved **once per app process**: it selects the
renderer at the moment the experiment runner spawns the wine process for
the targeted app, and the choice persists for that process's lifetime per
R-BACK-31.1 and R-BACK-31.6. A/B-testing two `render_mode` values for the
same app therefore means launching two app processes (one per value), not
hot-swapping within a single live process. Documented exceptions where an
app requires one specific renderer must be listed inline in the catalogue
with reason.

**R-BACK-39.6** A pixel-exact regression gate must run under
`DXMT9_RENDERER_PARITY_GATE=1`: when enabled, every present compares the two
renderers' backbuffer and fails the run on the first sample-level divergence
(by the policy specified in the catalogue entry). Default-off; used only by
the parity harness and the per-PR regression run.

**R-BACK-39.7** The Frame Graph must be serializable as a development-only
debug artifact behind `DXMT9_RENDERER_DUMP_DAG=<path>`. When set, each
`onChunkReady` invocation must emit, for the chunk it processed, **two**
DAG snapshots — `pre-opt` (immediately after the builder runs, R-BACK-32.1)
and `post-opt` (immediately after the optimizer pipeline runs,
R-BACK-32.5) — so the optimizer's effect is diffable. Each snapshot must
record, per `PassNode`: kind, attachment set (color handles + depth),
draw range, dominant state profile, and resolved load/store policy; per
`ResourceNode`: handle, the chronological access log (`pass_index`,
`access_kind`, `stage`), `first_use_pass`, `last_use_pass`, and residency
class; and the full producer→consumer edge set keyed by resource. The
primary serialization format is **JSON** (one object per chunk, framed by
`frame_id` and chunk seq id) so the artifact joins the existing
`scripts/tools` / `analysis/` perf tooling. Optional human-visual
renderings — Graphviz `.dot` and/or Mermaid `flowchart` — may be derived
from the same in-memory snapshot when listed in
`DXMT9_RENDERER_DUMP_DAG_FORMATS` (comma list; default `json`). Mermaid is
the preferred visual form because the `docs/perfomance/` knowledge graph and
these specs already render Mermaid inline, so a dumped DAG pastes directly
into a leaf doc with no Graphviz toolchain. Every selected format is derived
from one serialization pass — no format re-walks the DAG. The dump path is
**side-effect neutral** in the same sense as R-BACK-39.3: writing it must
not call any Metal API, must not allocate command buffers or aliases, must
not mutate the PSO/shader cache, residency, retained-handle, or fence
state, and must not change which Metal commands a non-dump run would emit.
This artifact is the pass/resource-level observability surface for the
render-pass re-entry and store/load investigations tracked in
`docs/perfomance/render-pass-store/`; it is **not** a pixel-level
final-writer oracle (per-pixel ownership after blend/depth remains a
rasterization-downstream property outside the DAG's scope).

The DAG debug dump is **backend-agnostic**: it must be available whenever
`DXMT9_RENDERER_DUMP_DAG` is set, on BOTH the `traditional` and `framegraph`
render modes, decoupled from `DXMT9_RENDER_MODE`. Because the dump is a pure
observation side-channel — which backend actually encodes a chunk is irrelevant
to it — both backends own the shared observer (`render::DagObserver`, spec.md
§2.1 / §3.5) and invoke it from `onChunkReady` before delegating to
`encoders::encodeChunk`. On the `traditional` path the DAG is built purely for
observation against default (all-off) optimizer options — the order-preserving
parity baseline — and the traditional encode path stays unchanged and
byte-identical; the dump is gated purely on `DXMT9_RENDERER_DUMP_DAG`, so a run
without it pays only one cached-optional check and emits the identical Metal
command stream. The frame-window filter (`DXMT9_RENDERER_DUMP_DAG_FRAME` /
`_RADIUS`, below) applies identically on both backends.

A real app (3DMark05, etc.) emits thousands of chunks per frame and would
flood the dump directory if every chunk's DAG were serialized. The dump may
therefore be scoped to a single frame with `DXMT9_RENDERER_DUMP_DAG_FRAME=N`,
a 1-based inter-present frame number. A "frame" is the inter-present interval:
a chunk belongs to the current frame, and a chunk that **contains** a Present
is the last chunk of that frame (the frame counter advances after such a
chunk). When the filter is set, only chunks belonging to frame N are
serialized; all other chunks must early-out **before** the FrameGraph is
built, so a filtered-out chunk costs only a cheap Present scan and no build /
serialize / file-write work. When the filter is unset (or `0` / non-numeric),
behavior is unchanged — every chunk is dumped, as before; real apps should set
the filter to avoid flooding the dump directory. The frame counter is
encode-thread-local (single writer). The `frame_id` stamped into the dump
filename and JSON is this inter-present frame number; the chunk seq id remains
the JSON `chunk_seq_id`.

The single-frame filter may be widened to a ±radius **window** with
`DXMT9_RENDERER_DUMP_DAG_FRAME_RADIUS=R`, a non-negative integer (default `0`,
i.e. single frame). When `DXMT9_RENDERER_DUMP_DAG_FRAME=N` and the radius is
`R`, every chunk whose inter-present frame falls in the **inclusive** window
`[max(1, N-R), N+R]` is dumped; chunks outside that window early-out before the
FrameGraph is built, exactly as the single-frame filter does. The low end is
clamped at `1` because inter-present frames are 1-based, so no `frame 0` is ever
selected (e.g. `N=1, R=5` dumps frames `1..6`). A radius of `0`, an empty
value, or a non-numeric value resolves to `0` (single frame). The radius is
ignored when no target frame is set (an unset target still dumps every chunk).
`framegraph::resolveDumpDagFrameRadius(env)` is the pure resolver and
`dumpDagFrameRadius()` reads the env once with the `dumpDagFrame()` static-const
pattern.

**Analysis-only post-opt optimizer override (`DXMT9_RENDERER_DUMP_DAG_OPTIMIZE`).**
The observer's `post-opt` snapshot may be driven by an operator-selected set of
optimizer passes instead of the owning backend's resolved options, so a
device-gated "what would `passcoalesce` do" experiment can be run on real
captured frames (e.g. 3DMark05 GT1). `DXMT9_RENDERER_DUMP_DAG_OPTIMIZE` is a
comma-separated token list (`passcoalesce`, `reorder`, `dce`, `memoryless`;
unknown tokens ignored). When set, the `post-opt` `runOptimizer` uses an
`OptimizerOptions` with exactly the named gated passes enabled (lifetime +
loadstore remain always-on); when unset, the `post-opt` snapshot keeps the
current behavior (the backend's options). The `pre-opt` snapshot is unaffected —
it remains the un-optimized builder baseline — so the `pre`/`post` diff is
exactly what the chosen passes did. This knob is **analysis-only and MUST NOT
affect the Metal encode**: the `render::DagObserver` is a pure observation
side-channel that never drives encoding, so the override cannot change rendered
output and only changes the serialized `post-opt` DAG and its `framegraph_*`
observe counters. It is **independent of `DXMT9_RENDERER_FEATURES` /
`compat_profile`** (which gate the production encode) precisely because it is
analysis-only. `framegraph::resolveDumpDagOptimize(env)` is the pure resolver
(nullptr / empty → `std::nullopt` = "use backend options"; a set-but-all-unknown
env still resolves to an all-off override because the operator opted in
explicitly) and `dumpDagOptimizeOverride()` reads the env once with the
`dumpDagFrame()` static-const pattern.

**Optional per-draw D3D9 detail (`DXMT9_RENDERER_DUMP_DAG_DRAWS`).** The JSON
dump may carry an L1-DEBUG per-pass `draws_detail` array — one entry per
draw-call ordinal in the pass's `DrawRange` — when
`DXMT9_RENDERER_DUMP_DAG_DRAWS` is set (repo env-flag semantics: a non-empty
string that is not `0`). Each entry is a BOUNDED summary resolved from the
source `core::ChunkSlot` hot state the linearizer already reads (not decoded
geometry): `command_index`, `draw_ordinal`, `primitive_type`,
`primitive_count`, `vs_hash`, `ps_hash` (the same VS/PS hashes the 3DMark05
indexed-probe CSV reports), `texture_mask`, the key render states
`alpha_blend` / `z_enable` / `z_write` / `z_func` / `alpha_test` / `cull`
(read with `core::flatStateOr` on the hot `FlatStateSet`, using the encoder's
`DrawDebugRecord` defaults), and `stream0_stride`. This is **DEBUG-ONLY and
JSON-only**: it resolves only when the export is given the source ChunkSlot AND
the flag is set, so when either is absent the JSON is byte-identical to the
historical output and the device-free golden serializers (which pass no slot)
are unaffected; it is a pure read that touches no Metal state and leaves the
encode byte-identical, exactly like the rest of R-BACK-39.7. `draws_detail` is
explicitly **L1-debug**: it is NOT the deferred L2 production `DrawDescriptor`
(spec.md §3.3) that carries per-draw geometry/bindings for the
mesh / GPU-driven path — that remains a SEPARATE, deferred production data
structure. `framegraph::resolveDumpDagDraws(env)` is the pure resolver and
`dumpDagDraws()` reads the env once with the `dumpDagFrame()` static-const
pattern.

---

## 11. Compatibility Profiles

**R-BACK-40.1** The `compat_profile` field in `experiments/CATALOGUE.toml`
must accept the values `strict`, `progressive`, and `aggressive`:

| Profile | Optimizer DCE / coalesce / reorder | Memoryless alias | Mesh / objectschedule / gpudriven | Per-draw fallback |
|---|---|---|---|---|
| `strict` | **all disabled** (load/store rules only) | **disabled** (no aliasing) | **disabled** | always → traditional |
| `progressive` | allowed within `R-BACK-32.2` deterministic / `R-BACK-34.*` proof gates | **NOT auto-enabled** — requires per-app `semantic_relaxations` list (see R-BACK-40.4) | mesh + objectschedule allowed; gpudriven gated on Metal 3 + macOS 14 | on incompat → traditional (per-draw) |
| `aggressive` | full | **NOT auto-enabled** — same `semantic_relaxations` gate as `progressive` | full | on incompat → traditional, then downgrade profile to `strict` for the remaining device lifetime per R-BACK-40.3 |

The default for an app without a `compat_profile` entry is `progressive`.
Its implicit feature set contains only `passcoalesce`. Explicit `strict`
remains the feature-empty, source-order rollback; `memoryless` and every later
layer remain opt-in even under progressive.

**R-BACK-40.5** A `strict` profile that selected `framegraph` mode must
delegate **all** Metal emission to `TraditionalBackend`. The
`FrameGraphBackend` may construct a DAG for sidecar observation purposes
(divergence logging, counter emission, telemetry), but it must not call
any encoder API, must not allocate any ICB, alias texture, or bindless
heap, and must not consume any AEC method beyond what `TraditionalBackend`
calls. The Metal command stream produced under `strict` `framegraph` must
be byte-identical to the stream produced under `traditional` for the
same chunk input, including load/store action choices (which come from
`TraditionalBackend`'s code path, not from any modern optimizer pass).
This is the parity baseline used by R-BACK-39.1. The `dce`, `passcoalesce`,
`memoryless`, `mesh`, `objectschedule`, `gpudriven`, and `bindless`
feature tokens must all be rejected under `compat_profile=strict` even
if the user injected them via `DXMT9_RENDERER_FEATURES`; rejection is
logged once and the run continues in pure delegation mode.

**R-BACK-40.4** Memoryless virtual-attachment promotion is a **semantic
relaxation**, not a generic optimization: per R-BACK-33.4 a
misclassification leaves the persistent backing stale, which violates
the LockRect / GetRenderTargetData byte-equivalence contract
(R-BACK-38.3) for one frame. Selecting `compat_profile=progressive` or
`compat_profile=aggressive` is therefore **insufficient** to enable the
`memoryless` feature on its own; the catalogue entry must additionally
list `memoryless_one_frame_artifact` in a `semantic_relaxations` array
on the `[[app]]` table. Without that explicit relaxation, the runtime
must reject the `memoryless` token (or strip it from
`DXMT9_RENDERER_FEATURES` with a single warning log line) regardless of
`compat_profile`. Other future semantic-relaxation features (reorder
side-effects, cull conservatism) must follow the same explicit-list
pattern; generic profile escalation must never silently activate them.
This aligns with `docs/perfomance/render-pass-store/render-pass-store-memoryless.01.md`
which records conservative-default and prior-frame-observation as the
correctness contract.

**R-BACK-40.2** A `strict` profile that selects `framegraph` mode must be
behaviorally indistinguishable from `traditional` at the swap-chain
boundary. This is the regression safety net: a `strict` `framegraph` run is
the parity baseline against which `progressive` and `aggressive` are
measured.

**R-BACK-40.3** A profile may be downgraded but not upgraded at runtime. An
`aggressive` profile that observes a strict fallback (`R-BACK-40.1` row 3
column 5) must record the downgrade and apply `strict` semantics for the
remainder of the device lifetime. Re-promotion requires a new process.

---

## 12. Phasing Markers

These markers exist so future contributors know what subset of this spec is
landed and what is staged. They are not implementation milestones; they are
requirement dependency layers.

**R-BACK-41.1 (Layer 0 — Refactor)**: extract `IRenderBackend`; introduce
`backend_factory` driven by `DXMT9_RENDER_MODE`. The L0 `framegraph + strict`
baseline is a no-feature wrapper that produces identical Metal output to
`traditional`. Parity harness, divergence logging, and CI matrix must land in
this layer. Without this layer the rest of the spec cannot be tested.

**R-BACK-41.2 (Layer 1 — Frame Graph)**: enable `passcoalesce` as the default
L1 feature; memoryless remains a separate semantic-relaxation feature. No mesh,
no GPU-driven. The Frame Graph builder, optimizer pipeline, and resource
virtualization land in this layer. The DAG debug-export (R-BACK-39.7) also
lands in this layer, since it is the first layer where a real DAG exists to
serialize. This is the first layer that can produce a measurable GT1 delta on
its own.

**R-BACK-41.3 (Layer 2 — Mesh and Bindless)**: enable `bindless` and `mesh`
features. Mesh PSO cluster compilation, bindless heaps, mixed-path encoder
land. Object scheduler may exist as a CPU-side prototype that emits mesh
dispatches directly.

**R-BACK-41.4 (Layer 3 — GPU-Driven)**: enable `gpudriven` and
`objectschedule` features. Object shader runs as GPU compute pre-pass;
ICB-based mesh dispatch lands.

**R-BACK-41.5** Each layer after L1 must pass `R-BACK-39.1` parity against the
prior layer's accepted profile before it is enabled by default. The promoted
L1 passcoalesce subset additionally requires complete duplicate-free replay,
alias-aware RAW/WAR/WAW ordering, order-aware Store proof, source-order
fallback, and clean GT1/GT2/GT3 wild captures including the known GT3 glitch
window. Device-backed pixel parity remains a required evidence-closure task.
A later layer that cannot prove parity may ship as opt-in for diagnostic work
but must not become a default in any `compat_profile`.

**R-BACK-41.6** Mesh / object / GPU-driven acceptance must **not** require a
GT1 performance number as a precondition. The performance investigation root
(`docs/perfomance/overview-3dmark05-gt1.md`) classifies the current
mesh/object/GPU-driven track as "bridge-only, route/emitter missing,
reduced-counter A/B required" — direct GT1 spend on this track is gated on
that route/emitter work landing first. L2 and L3 acceptance must therefore
be expressed as:

- **route equality**: for every D3D9 draw routed through the modern path, the
  per-draw counter family records the same number of mesh dispatches the
  optimizer predicted, with zero dropped or duplicated draws compared to
  traditional;
- **per-encoder counter equality**: bound resources, PSO selection, viewport,
  and scissor recorded for each pass match the traditional path's recorded
  values under the same chunk input;
- **reduced-counter A/B**: a fixed `framegraph_*` counter subset (drawn from
  R-BACK-39.2) must match between the two renderers on the conformance
  capture set; GT1 frame-time delta is not a pass criterion until the
  conformance-set match is published.

GT1 frame-time measurement is permitted at any layer for diagnostic
investigation but must not be cited as an acceptance gate for promoting a
layer into a `compat_profile` default.

---

## 13. References

- `specs/d3d9/requirements.md` — D3D9 frontend contracts that all renderers
  must respect.
- `specs/backend/requirements.md` — traditional 1:1 path, shared Metal
  infrastructure, counter and audit gates.
- `specs/archicture/requirements.md` — project-wide DOD and provenance.
- `specs/experiments/requirements.md` — catalogue format, wild-run pass
  criteria, screenshot comparison policies.
- `docs/perfomance/overview-3dmark05-gt1.md` — investigation root that
  motivated the modern path.
- `docs/perfomance/render-pass-store/render-pass-store-memoryless.01.md` —
  open proposal recorded against the §4 surface.
- `docs/research/dxvk-d3d9.md`, `docs/research/dxmt.md` — comparable
  translation-layer architectures (none implement the modern path).
- `agents/rules/environment_variables.rules.md` — master `DXMT*` env
  reference; renderer env vars listed here must register there.
