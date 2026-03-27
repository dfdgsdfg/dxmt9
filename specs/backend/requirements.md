# Backend Requirements

The backend receives `DrawDesc` / `ClearDesc` / `SwapDesc` from the core and is
responsible for translating them into correct Metal commands. It knows nothing about
D3D9 COM objects.

---

## 1. Correctness of Translation

**R-BACK-1.1** Every `DrawDesc` submitted by the core must produce rendering results
that are equivalent to what a conformant D3D9 implementation on the same geometry and
state would produce, within the precision limits of the Metal backend GPU.

**R-BACK-1.2** The results of `submitDraw()` must be visible in the render target
before the next `present()` call on the same swap chain commits.

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
encoding. All per-frame memory (argument buffers, lambda storage, temporary staging)
must be drawn from pre-allocated ring allocators.

**R-BACK-2.4** The encode thread must group draw calls sharing the same render
targets into a single `MTLRenderCommandEncoder` where no hazard (write-after-read,
write-after-write) exists between them. Splitting into multiple encoders is correct
but unnecessary — the requirement is that merging must be considered.

**R-BACK-2.5** A `Clear()` issued before any draw call on a render target must be
expressed as `MTLLoadActionClear` on the render pass descriptor, not as a separate
blit or draw. This is a performance requirement; the clear results must be identical
to a mid-scene fill-rect clear.

**R-BACK-2.6** A render target change (`SetRenderTarget`) during a scene must
terminate the current `MTLRenderCommandEncoder` and begin a new one. The previous
render target's store action must be `MTLStoreActionStore`.

---

## 3. Pipeline State Objects

**R-BACK-3.1** The backend must cache compiled `MTLRenderPipelineState` objects.
Two `DrawDesc`s that produce the same PSO key must receive the same
`MTLRenderPipelineState` object without recompilation.

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

**R-BACK-4.6** Clip planes enabled via `D3DRS_CLIPPLANEENABLE` must be emitted as
`[[clip_distance]]` outputs in the vertex shader. This is a Metal hardware feature
that must not be emulated with fragment shader discards.

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

---

## 6. Presentation

**R-BACK-6.1** `present()` must display the most recently rendered frame on the
associated window. It must block until a drawable is available from `CAMetalLayer`
when `D3DPRESENT_INTERVAL_ONE` (vsync) is requested.

**R-BACK-6.2** `present()` with `D3DPRESENT_INTERVAL_IMMEDIATE` must not wait for
vsync. `CAMetalLayer.displaySyncEnabled` must be `NO` for this mode.

**R-BACK-6.3** After `present()` returns, the back buffer contents are undefined
(consistent with `D3DSWAPEFFECT_DISCARD`). The next render pass targeting the back
buffer must use `MTLLoadActionDontCare` or `MTLLoadActionClear`, not `MTLLoadActionLoad`.

---

## 7. Thread Safety

**R-BACK-7.1** The backend interface (`submitDraw`, `submitClear`, `present`,
`createBuffer`, etc.) must be safe to call from a single thread (the Wine/application
thread). No concurrent calls from multiple threads are required.

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
