# Backend Requirements

The backend receives committed command chunks from the core and is responsible for
translating their canonical draw state/run payloads (`CanonicalDrawState` plus
`DrawRunDesc` and `DrawUniformPayload`, or imported `FlatDrawStateView`
equivalents) and `ClearDesc` / `SwapDesc` payloads into correct Metal commands.
It knows nothing about D3D9 COM objects. `fixture::DrawDesc` is a tests/offline
helper only; it is not a production backend input contract.

---

## 1. Correctness of Translation

**R-BACK-1.1** Every production draw submitted by the core as
`CanonicalDrawState` plus `DrawRunDesc`, or as an imported `FlatDrawStateView`
equivalent, must produce rendering results that are equivalent to what a
conformant D3D9 implementation on the same geometry and state would produce,
within the precision limits of the Metal backend GPU.

**R-BACK-1.2** The results of a recorded draw command must be visible in the render
target before the next present command on the same swap chain commits.

**R-BACK-1.3** Draw calls must execute in submission order with respect to the same
render target. A draw call must not read stale color or depth data written by a prior
draw call targeting the same surface.

**R-BACK-1.4** The backend must not reorder operations that produce observable
side-effects on render targets, except where such reordering is invisible to the
application (e.g., merging render passes with identical attachments).

---

## 2. Command Encoding

**R-BACK-2.1** The Wine/application thread must not block on Metal API calls during
draw submission. Metal command encoding must occur on a dedicated encode thread.

**R-BACK-2.2** The application thread and the encode thread must synchronize only
through a bounded queue. The maximum number of frames the application thread can
outrun the encode thread is a configurable limit (default: 3).

**R-BACK-2.3** The encode thread must not allocate from the system heap during normal
encoding. All per-frame memory (argument buffers, imported replay storage, temporary
staging) must be drawn from pre-allocated ring allocators.

**R-BACK-2.4** The encode thread must group draw calls sharing the same render
targets into a single `MTLRenderCommandEncoder` where exact retained-resource
hazard tracking shows no read-after-write, write-after-write, or write-after-read
conflict between them. Splitting into multiple encoders is correct but unnecessary
when exact tracking is clean; the requirement is that merging must be considered.

**R-BACK-2.5** A `Clear()` issued before any draw call on a render target must be
expressed as `MTLLoadActionClear` on the render pass descriptor, not as a separate
blit or draw. This is a performance requirement; the clear results must be identical
to a mid-scene fill-rect clear.

**R-BACK-2.6** A render target change (`SetRenderTarget`) during a scene must
terminate the current `MTLRenderCommandEncoder` and begin a new one. The previous
render target's store action must be `MTLStoreActionStore`.

**R-BACK-2.7** The default Wine runtime path must submit work to the backend as
committed command chunks. Per-draw or per-state backend entry points may exist for
tests or bootstrap, but they must not be the hot path.

**R-BACK-2.8** A committed command chunk must cross the Wine PE/unix boundary with
one bridge operation. The backend must not require one `WINE_UNIX_CALL` per D3D9
`Set*`, `Draw*`, or `Clear` call.

**R-BACK-2.9** The unix-side importer must validate every command record before it
is queued for encoding. Invalid record kinds, invalid payload sizes, stale handles,
and malformed offsets must fail the chunk without dereferencing untrusted pointers.

**R-BACK-2.10** The command queue must enforce bounded chunk capacity by command
count and byte size. If the PE side submits a chunk larger than the negotiated
limits, the backend must reject it rather than allowing unbounded encode latency.

**R-BACK-2.11** The bridge ABI for committed chunks must remain POD and versioned.
The unix-side importer may translate validated records into queue-local replay
actions or direct encoder operations, but C++ lambdas and process-local pointers
must never cross the PE/unix boundary.

**R-BACK-2.12** Present-bearing chunks must carry explicit present metadata through
import, encoding, command-buffer commit, and completion. Non-present chunks advance
the normal sequence timeline only; they must not allocate frame-latency tokens.

**R-BACK-2.13** The command queue must own execution chunk lifecycle after import:
queue-slot assignment, encode-thread dispatch, command-buffer commit tracking,
finish-thread completion, allocator reclamation, sequence signaling, and
present-frame-token signaling.

**R-BACK-2.14** The importer must be the only backend stage that accepts raw
committed chunk bytes from the bridge. It must translate them into compact imported
records and retained backend handle references before the command queue may execute
the chunk.

**R-BACK-2.15** Backend replay and encode code must consume imported records and
explicit production state views (`FlatDrawStateView`, encoder state, resource
binding state, allocator cursors, cache keys, etc.). It must not consume PE COM
objects, PE `DeviceState` pointers, Objective-C objects from the bridge payload,
or ad-hoc per-call mutable state that is not owned by the execution chunk or
queue.

**R-BACK-2.17** Large draw-uniform payloads (shader constants, fixed-function
matrices, texture transforms, and clip planes) must be stored outside hot draw
state records and referenced by stable queue-local handles or spans. Hot
PSO/resource decisions must use hashes and compact flat records, not full constant
arrays.

**R-BACK-2.16** Replay transforms that derive encoder decisions from imported
records (pass merge/split, deferred clear application, hazard classification, PSO
key construction, argument-buffer layout) must be deterministic for identical
imported input records, retained resource metadata, and explicit queue-local state.
Backend caches may affect latency, but must not change the encoded command
semantics.

**R-BACK-2.17** The backend ownership split must match the DXMT target shape:
`CommandQueue` owns chunk execution, encode and finish threads, Metal command-buffer
lifetime, sequence fences, and frame tokens; `Presenter` owns drawable acquisition
and `presentDrawable` encoding; the importer owns validation and retention of POD
records and backend handles.

**R-BACK-2.18** A committed `CommandChunk` wire image must be composed of contiguous
POD storage: one fixed `ChunkHeader`, one fixed-width command record header array,
one payload arena, and one opaque handle table. Records must address variable data by
offset and size into the payload arena, not by process-local pointers.

**R-BACK-2.19** Wire headers and imported record headers must use fixed-size integer
fields with explicit ABI packing, alignment, and byte-order rules. The PE builder and
unix importer must enforce static `sizeof`/`alignof` checks for every wire header and
runtime checks for negotiated header sizes before any record payload is decoded.

**R-BACK-2.20** The importer must validate every payload range with overflow-safe
arithmetic. `payloadOffset + payloadSize` and all nested payload-relative ranges must
remain within the chunk payload arena and satisfy the alignment required by that
record schema.

**R-BACK-2.21** Command record schemas must be POD and fixed-layout. Wire records and
payloads must not contain COM pointers, Objective-C object pointers, unix-side object
pointers, vtables, polymorphic objects, lambdas, `std::function`, allocator-owned
containers, or any pointer that is meaningful only inside one process.

**R-BACK-2.22** Resource references inside a committed chunk must use indices into
the chunk handle table plus schema-defined kind information. The importer must reject
out-of-range indices, kind mismatches, stale handles, and records whose declared
handle ranges exceed the handle table before retaining any resource for execution.

**R-BACK-2.23** Imported records must be stored in cache-friendly contiguous arrays or
arena-backed POD storage owned by the execution chunk. The encode thread must be able
to replay records by linear iteration without performing bridge-handle lookups or
allocating one heap object per command.

**R-BACK-2.24** The command chunk ABI must be versioned at the chunk header level.
The importer must reject chunks with unsupported ABI versions. Unknown opcodes in a
compatible version must be rejected unless the record is explicitly marked ignorable
and its full payload and handle ranges validate without executing it.

**R-BACK-2.25** Reserved fields in chunk headers, record headers, and fixed payload
headers must be validated as zero on import. Non-zero reserved fields must reject the
chunk so future ABI extensions are not silently misinterpreted by older importers.

**R-BACK-2.26** PE-side recording must append ordinary draw/state commands into
pre-reserved contiguous chunk storage. Exhausting command or payload capacity must
seal/submit the current chunk or use a bounded slow path outside the steady-state draw
hot path; it must not introduce per-command system heap allocation in normal
recording.

**R-BACK-2.27** Import, replay, and encode hot paths must use execution-slot storage
or preallocated ring allocators for command records, retained handle references,
argument buffers, staging, copy-temp, and imported replay storage. Cache misses may
allocate cache objects, but steady-state CPU/GPU command replay must not allocate
from the system heap.

**R-BACK-2.28** Encoder split decisions for resource hazards must be based on exact
read/write handle-set overlap. Probabilistic Bloom overlap checks may remain only as
diagnostic counters for false-positive detection; Bloom false positives must not
force a render-pass split in the default path.

---

## 3. Pipeline State Objects

**R-BACK-3.1** The backend must cache compiled `MTLRenderPipelineState` objects.
Two production draw states (`CanonicalDrawState` / `FlatDrawStateView`) that
produce the same PSO key must receive the same `MTLRenderPipelineState` object
without recompilation.

**R-BACK-3.2** PSO compilation must not block draw submission on the hot path after
the cache is warm. The first draw call that requires a new PSO may stall; subsequent
draws with the same PSO must not.

**R-BACK-3.3** The PSO key must include exactly the states that Metal bakes into
`MTLRenderPipelineState`: vertex and fragment function identity (including variant
specialization), vertex descriptor layout, render target pixel formats, blend state
per attachment, sample count, and alpha-to-coverage.

**R-BACK-3.4** States that Metal encodes dynamically on the command encoder (cull
mode, fill mode, viewport, scissor, depth bias, stencil reference) must not be part
of the PSO key.

**R-BACK-3.5** The `MTLDepthStencilState` object must be cached separately from the
render PSO. Its key is the set of depth and stencil compare/write state (depth enable,
depth write, depth func, front/back stencil ops and masks).

**R-BACK-3.6** The backend must support async PSO compilation. A PSO may be compiled
on a background thread; the encode thread blocks on first use if compilation is not
yet complete.

**R-BACK-3.7** The PSO cache must be load-warmable from the on-disk
`MTLBinaryArchive` at device creation. Cache load must populate
`MTLRenderPipelineState` lookups for every (shader hash, variant) pair present
in the archive before the first draw is encoded. Load failures (missing file,
schema mismatch, unsupported GPU family) must fall back to fresh-compile
behavior without blocking device creation.

**R-BACK-3.8** Prewarm scope must be configurable: full archive load on device
init (default for shipping builds), lazy on first draw (default for dev), or
disabled (debug). The selected mode must be observable via a counter and the
chosen archive path/identity must be visible in present diagnostics.

---

## 4. Shader Translation

**R-BACK-4.1** The backend must accept D3D9 shader bytecode for SM 1.x, 2.0, and 3.0
vertex and pixel shaders and produce Metal shader functions that compute equivalent
results.

**R-BACK-4.2** The backend must accept `FFPKeyVS` and `FFPKeyPS` values from the core
and produce Metal vertex and fragment functions that implement D3D9's fixed-function
lighting, transform, texture combining, and fog behavior for those keys.

**R-BACK-4.3** Compiled shader functions (both from bytecode and from FFP keys) must
be cached persistently across process restarts, keyed by a stable hash of the input
(bytecode content hash or FFP key value plus variant parameters). Recompilation on
cache hit is not permitted.

**R-BACK-4.4** The half-pixel offset correction (as specified in core/design §7) must
be applied to every vertex shader before translation. The translation output must
include this correction.

**R-BACK-4.5** Alpha test (as specified in core/design §8) must be encoded in pixel
shader variants when required by the key. The pixel shader must execute
`discard_fragment()` for failing fragments before any color output.

**R-BACK-4.6** Programmable pixel-shader translation must not use a default
texture-coordinate V flip to compensate for D3D/Metal coordinate differences.
The generated MSL sampling path must preserve the D3D V coordinate unless the
diagnostic `DXMT_DEBUG_FORCE_PIXEL_V_FLIP` flag is enabled. This debug flag is
not part of normal rendering correctness and must stay independent from
`DXMT_DEBUG_FLIP_VERTEX_Y`.

**R-BACK-4.7** Clip planes enabled via `D3DRS_CLIPPLANEENABLE` must be emitted as
`[[clip_distance]]` outputs in the vertex shader. This is a Metal hardware feature
that must not be emulated with fragment shader discards.

**R-BACK-4.8** The shader compile path must persist every produced
`MTLLibrary` / `MTLFunction` to a process-shared on-disk
`MTLBinaryArchive`, keyed by SHA-1 of the input bytecode plus variant key.
Subsequent process starts must load from this archive before any compile
attempt; archive misses fall through to compile and write back. The archive
identity (file path, version) must be cross-process consistent so multiple
dxmt9 instances can warm each other's caches.

---

## 5. Resource Allocation

**R-BACK-5.1** `createBuffer()` must return a handle to a GPU-accessible `MTLBuffer`.
The storage mode must be appropriate for the pool and usage: `D3DPOOL_DEFAULT` with
no CPU writes maps to private storage; `D3DPOOL_DEFAULT` with `D3DUSAGE_DYNAMIC` maps
to shared or managed storage.

**R-BACK-5.2** `createTexture()` must return a handle to a `MTLTexture` with the
correct pixel format, dimensions, mip count, array count, and texture type (2D, cube,
3D, or array).

**R-BACK-5.3** `mapBuffer(handle, D3DLOCK_DISCARD)` must return a writable pointer to
a fresh, non-overlapping buffer region. The previous contents are undefined. The
implementation may return a new sub-allocation within a ring buffer.

**R-BACK-5.4** `mapBuffer(handle, D3DLOCK_NOOVERWRITE)` must return a pointer to the
current buffer allocation. The backend may assume the caller will not overwrite
in-flight regions. No synchronization against the GPU is required.

**R-BACK-5.5** `mapBuffer` with neither `DISCARD` nor `NOOVERWRITE` must wait until
the GPU has completed all commands that read from the buffer before returning.

**R-BACK-5.6** `destroyBuffer()` and `destroyTexture()` must not free the underlying
Metal object until all in-flight GPU commands that reference it have completed.

**R-BACK-5.7** D3D9 pool / usage combinations must map to Metal storage modes
according to the following contract. The mapping must be selected at resource
create time, must not change for the resource's lifetime, and must be
identical between buffers and textures of the same pool/usage class.

| D3D9 pool | D3D9 usage | Apple Silicon (unified memory) | Intel/AMD (discrete-style) |
|---|---|---|---|
| `D3DPOOL_DEFAULT` | none / `RENDERTARGET` / `DEPTHSTENCIL` | `MTLStorageModePrivate` | `MTLStorageModePrivate` |
| `D3DPOOL_DEFAULT` | `D3DUSAGE_DYNAMIC` | `MTLStorageModeShared` (rename ring) | `MTLStorageModeShared` (rename ring) |
| `D3DPOOL_MANAGED` | any | `MTLStorageModeShared` (no staging copy) | `MTLStorageModeManaged` (with staging) |
| `D3DPOOL_SYSTEMMEM` | any | host malloc + `MTLStorageModeShared` upload buffer | host malloc + staging |
| `D3DPOOL_SCRATCH` | any | host malloc only (never reaches GPU) | host malloc only |

Unified-memory detection must use `MTLDevice.hasUnifiedMemory`. The intent is
that on Apple Silicon, `MANAGED` pool resources do **not** require a separate
private-storage copy and CPU↔GPU upload — both sides see the same backing.

**R-BACK-5.8** `D3DUSAGE_DYNAMIC` buffer rename (on `D3DLOCK_DISCARD`) must
draw from a per-frame ring of `MTLStorageModeShared` allocations. A rename
that finds no free allocation in the ring must fall back to a fresh
`newBufferWithLength:options:` rather than blocking on prior GPU completion.

**R-BACK-5.9** Many small textures (sub-`64 KB` allocation footprint, e.g.
D3D9 lightmaps, decals, glyph atlases, particle sprites) must be backed by a
shared `MTLHeap` of appropriate storage mode rather than individual
`newTextureWithDescriptor:`. Heap residency tracking is per-heap, not
per-texture; per-texture residency calls on heap-backed textures must be
elided. Textures larger than the heap-eligibility threshold or with usage
flags incompatible with heap allocation (e.g., specific `framebufferOnly`
combinations) fall through to direct allocation.

**R-BACK-5.10** `MTLHeap` capacity must grow geometrically with a configurable
ceiling. Heap exhaustion must trigger allocation of a new heap rather than
falling back to direct allocation, so subsequent residency cost stays bounded.
Heap reclamation is deferred until all heap-backed textures are freed (same
DXMT-style sequence-ID gate as direct allocations).

---

## 6. Presentation

**R-BACK-6.1** A present command must display the most recently rendered frame on
the associated window. When `D3DPRESENT_INTERVAL_ONE` (vsync) is requested, drawable
availability and vsync pacing are handled by the presenter/encode path. The
application-facing wait is governed by frame-latency tokens, not by synchronous
drawable acquisition in the device object.

**R-BACK-6.2** A present command with `D3DPRESENT_INTERVAL_IMMEDIATE` must not wait
for vsync. `CAMetalLayer.displaySyncEnabled` must be `NO` for this mode.

**R-BACK-6.3** After `present()` returns, the back buffer contents are undefined
(consistent with `D3DSWAPEFFECT_DISCARD`). The next render pass targeting the back
buffer must use `MTLLoadActionDontCare` or `MTLLoadActionClear`, not `MTLLoadActionLoad`.

**R-BACK-6.4** A present-bearing command chunk must be assigned a monotonically
increasing frame token when it is accepted by the backend queue.

**R-BACK-6.5** Frame-latency waits must target queue/presenter completion of frame
tokens. A frame token is complete only after the Metal command buffer carrying that
present has completed.

**R-BACK-6.6** Frame-latency waits must not be satisfied merely because the encode
thread dequeued, began encoding, or committed the chunk. Encode progress and present
completion are separate timelines.

**R-BACK-6.7** `setMaxFrameLatency(n)` must configure how many present-bearing frame
tokens may remain incomplete. The default is 4. The effective value must be clamped
to the range accepted by the core requirements.

**R-BACK-6.8** The presenter owns drawable acquisition, layer synchronization, and
`presentDrawable` encoding. The command queue owns frame-token allocation and
completion signaling. The API/device layer must not own presenter timing state.

**R-BACK-6.9** Present diagnostics must expose the selected source validity, source
handle/texture identity, source size, format, sample count, render-pass source size,
and destination size. These counters must make a valid 1280x720 SFIV source
distinguishable from missing source, missing texture, resolve, or invalid-size
present failures before present-policy tuning is considered.

---

## 7. Thread Safety

**R-BACK-7.1** The backend interface (`commitChunk`, resource create/destroy,
map/unmap, shader compile, frame-token waits, etc.) must be safe to call from a
single thread (the Wine/application thread). No concurrent calls from multiple
threads are required.

**R-BACK-7.2** Internal backend threads (encode thread, completion thread) must not
be visible to or callable by the core.

**R-BACK-7.3** Resource destruction (`destroyBuffer`, `destroyTexture`) must be safe
to call while in-flight GPU work references the resource. The destruction must be
deferred until GPU completion.

---

## 8. Wine Bridge

**R-BACK-8.1** On Wine, the backend must use the `winemetal` unix-lib thunk mechanism
to cross the Win32/macOS boundary. Direct Objective-C or Metal API calls must not
appear in the PE-side code.

**R-BACK-8.2** The shader compilation path (D3DBC → SPIR-V → MSL, or direct
translation) must be callable from the Win32 side via the `airconv`-style thunk
interface.

**R-BACK-8.3** All Metal object handles passed across the boundary must be opaque
integer handles, not Objective-C object pointers.

**R-BACK-8.4** The thunk mechanism must be used at chunk/resource granularity, not
per D3D9 draw or state operation. The intended hot-path call shape is
`commitChunk()` plus coarse resource lifecycle/map operations.

**R-BACK-8.5** The Wine bridge must expose diagnostic counters for bridge calls and
time spent in each bridge class. At minimum this includes chunk commits, resource
create/destroy/map/unmap calls, frame-token waits, shader compiler calls, and any
compatibility per-call draw/state fallback.

---

## 9. Surface Operations

Detailed API contracts, validation rules, and bridge-recording policy are defined in
[`surface-ops/requirements.md`](surface-ops/requirements.md). The backend design for
Metal replay is defined in [`surface-ops/design.md`](surface-ops/design.md).

**R-BACK-9.1** `submitSurfaceCopy()` (used by `UpdateSurface` and `UpdateTexture`)
must correctly copy all specified mip levels and cube/array slices. Row pitch and
slice pitch from the source must be respected exactly. The operation must cross the
Wine PE/unix boundary as POD chunk records, not as per-copy bridge calls.

**R-BACK-9.2** `submitStretchRect()` must produce correct results for both same-size
(blit) and scaled (render pass) copies. The `MTLSamplerMinMagFilter` used for scaled
copies must match the requested `D3DTEXTUREFILTERTYPE`.

**R-BACK-9.3** `submitReadback()` (used by `GetRenderTargetData`) must block the
calling thread until the GPU has written the result to the staging buffer and all
prior writes to the source render target are visible. It must not return until the
data is CPU-readable and copied to the D3D9 destination allocation.

**R-BACK-9.4** `submitColorFill()` must produce a correctly filled surface region.
For full-surface fills, `MTLLoadActionClear` must be used. For partial fills, a
scissored render pass or fragment shader fill must be used.

**R-BACK-9.5** All surface operations must be recorded as command records in the
current chunk, except readback which must additionally commit the current chunk and
wait for completion before returning.

**R-BACK-9.6** Surface operation command records must be fixed-layout POD records
with handle-table references and payload offsets. They must not contain closures,
lambdas, process-local pointers, COM object pointers, or Objective-C object pointers.

---

## 10. Clip Planes

**R-BACK-10.1** When the production draw state has `clipPlaneMask != 0`, the
vertex shader must output `[[clip_distance]]` values. The number of active clip
distances must equal the number of set bits in `clipPlaneMask` (maximum 6).

**R-BACK-10.2** Clip plane uniforms (transformed to clip space by the core) must be
passed to the vertex shader via the fixed-function uniform buffer or a dedicated
small constant buffer.

**R-BACK-10.3** `clipPlaneMask` must be part of the vertex shader variant key. Draws
with different `clipPlaneMask` values must not share a compiled vertex shader.

---

## 11. Multisampling

**R-BACK-11.1** When a render target has `sampleCount > 1`, the `MTLRenderPassDescriptor`
must use a multisample texture as the color attachment with
`storeAction = MTLStoreActionMultisampleResolve` and a single-sample resolve texture.

**R-BACK-11.2** The `PSO.rasterSampleCount` must match the render target's sample
count. A draw call to a 4× MSAA render target must use a PSO compiled with
`rasterSampleCount = 4`.

**R-BACK-11.3** `GetRenderTargetData` on a multisample render target must resolve to
the single-sample texture first, then read back from the resolve texture.

---

## 13. Tile-Shader FFP (Apple Silicon Fast Path)

These requirements define an Apple-Silicon-specific accelerated path for D3D9
fixed-function fragment effects. They never replace the portable FFP fragment
path; they are an opt-in fast path selected per-frame based on GPU capability
and render-pass shape.

**R-BACK-13.1** On GPU families that support programmable blending and tile
shaders (`MTLGPUFamilyApple3` and later), the backend may emit FFP fog,
alpha-test, and alpha-to-coverage as tile-stage code instead of executing
them as fragment-stage `discard_fragment()` / fragment-stage scalar ops. The
choice between portable and tile-shader paths must be made per render-pass
descriptor, not per draw, so a pass cannot mix the two.

**R-BACK-13.2** Tile-shader FFP must produce results bit-identical to the
portable fragment path within the precision limits already permitted by
`R-BACK-1.1`. Where Metal's tile stage cannot reproduce a D3D9 corner case
(extreme alpha-test reference values, specific fog non-linear modes), the
selector must fall back to the portable path for that pass, not produce a
different result.

**R-BACK-13.3** The PSO key must include a tile-FFP-mode bit. Two draws with
the same FFP key but different tile-mode selection must compile separate
pipeline states. Tile-mode flips inside a pass are not permitted; they
require a render-pass split.

**R-BACK-13.4** Pass selection must be observable via counters
(`tileFfpPassCount`, `portableFfpPassCount`, `tileFfpFallbackCount`) so the
ratio of accelerated vs portable passes is measurable per workload. A
fallback (R-BACK-13.2) increments `tileFfpFallbackCount` and must record the
reason class (precision, unsupported state, GPU family).

**R-BACK-13.5** Tile-shader FFP must remain disabled on GPU families without
programmable blending support and on any non-Apple-Silicon configuration. The
spec does not require tile-shader FFP to be available; conformance evidence
is for the portable path only.

**R-BACK-13.6** Tile-shader FFP source must be a separate generator, not a
post-hoc transform of the portable FFP MSL. The two paths share the
`FFPKeyPS` value but produce distinct MSL, distinct `MTLFunction` handles,
and distinct cache entries.

---

## 14. Resource Heap Pooling

Beyond the per-resource `MTLHeap` rule in `R-BACK-5.9`, this section pins the
contract for D3D9 small-resource heap allocation policy.

**R-BACK-14.1** The backend must maintain at least three independent heap
families: private-storage textures (`D3DPOOL_DEFAULT` non-RT non-DS small
textures), shared-storage textures (`MANAGED` / `SYSTEMMEM` small textures on
unified memory), and shared-storage buffers (vertex/index buffers below the
direct-allocation threshold). Heap families must not be cross-allocated;
storage mode and usage flags must be compatible across all heap members.

**R-BACK-14.2** Heap eligibility thresholds must be configurable but must
default to values that capture D3D9's typical small-texture working set:
allocation footprint ≤ 64 KB and a usage class compatible with heap
allocation. Render targets, depth buffers, and dynamic-rename buffers must
always allocate directly, regardless of size.

**R-BACK-14.3** Heap capacity, allocation count, and eviction count must be
exposed as counters per family. A per-workload baseline must show heap
allocation absorbs the expected fraction of total `MTLTexture` /
`MTLBuffer` creates; a regression to direct allocation is observable.

**R-BACK-14.4** Heap-backed resource destruction must observe the same
deferred-destroy sequence-ID gate as direct allocations
(`R-BACK-5.6` / `R-BACK-7.3`). Heap reclamation cannot occur while any
heap-backed resource is referenced by an in-flight chunk.
