# Render-Pass Load/Store Action Requirements

dxmt9 must reduce Apple-Silicon TBDR tile preservation cost by avoiding
unnecessary `MTLLoadActionLoad` and `MTLStoreActionStore` on render-pass
boundaries that do not require content preservation.

These requirements describe the contract owned by the backend encode path
(`beginRenderPass`, encoder split decisions in `encodeChunk`) and the queue's
per-command-buffer tracking state. The design that satisfies them lives in
`design.md`.

Traceability: `R-BACK-15.1` through `R-BACK-15.16`. Cross-references:
`specs/backend/requirements.md` `R-BACK-2.5` (clear-as-load-action),
`R-BACK-2.6` (RT-change encoder split — amended below), `R-BACK-6.3`
(post-present back-buffer DontCare-load), and
`docs/perfomance-bottleneck.md` (empirical motivation: SFIV 1272-frame
benchmark, 75% of render passes pay full Load+Store cost,
~150 MB/frame of pure tile preservation traffic).

This spec amends `R-BACK-2.6`. The amendment text is in section 7.

---

## 1. Default Policy

### 1.1 Default load action (`R-BACK-15.1`)

When no other rule in this spec applies, color, depth, and stencil
attachments must use `MTLLoadActionLoad` for the load action.

### 1.2 Default store action (`R-BACK-15.2`)

When no other rule in this spec applies, color, depth, and stencil
attachments must use `MTLStoreActionStore` for the store action. MSAA
resolve attachments override to `MTLStoreActionMultisampleResolve` per the
existing contract.

### 1.3 Clear-as-load-action precedence (`R-BACK-15.3`)

When a deferred clear targets the attachment as the first operation of the
render pass, the load action must be `MTLLoadActionClear` per `R-BACK-2.5`.
This rule has higher priority than any DontCare rule below: a clear-bound
attachment must not also be marked DontCare.

---

## 2. Color RT Freshness Tracking

### 2.1 Color RT first-use DontCare-load (`R-BACK-15.4`)

A color attachment whose contents have not been written this command buffer
and that is not subject to clear-as-load-action must use
`MTLLoadActionDontCare` on the first render pass that targets it within the
command buffer.

The encoder must maintain a per-command-buffer set of "touched" color
attachment handles. The set is empty at `encodeChunk` entry. When a render
pass closes with `MTLStoreActionStore` (or `MTLStoreActionMultisampleResolve`)
on a color attachment, that attachment's handle joins the touched set. When
a render pass opens, attachments not in the touched set use DontCare-load
unless `R-BACK-15.3` selects Clear.

### 2.2 Touched set invalidation (`R-BACK-15.5`)

The touched set must be invalidated for an attachment handle when:

- The handle is the source or destination of a `StretchRect` /
  `SurfaceCopy` / `ColorFill` / `Readback` operation that overwrites it.
  After such an operation, the next render pass on that handle is a "first
  use" and may DontCare-load.
- The handle is unbound and replaced by a different texture via D3D9
  resource reset or recreation. Implementations may treat handle
  reallocation as set invalidation since the new handle does not appear in
  the touched set.

### 2.3 Cross-frame correctness (`R-BACK-15.6`)

`R-BACK-15.4` must not be applied across frames merely because the previous
frame's command buffer ended. If an application reads a render target in
frame N+1 that was rendered in frame N, the implementation must respect
that read. dxmt9 already enforces this for the backbuffer via
`R-BACK-6.3` (post-present DontCare-load). For non-backbuffer
cross-frame-live targets, this spec relies on the touched-set living until
the application explicitly invalidates the handle (resource reset,
discard, or full overwrite). Implementations must not auto-invalidate the
touched set merely because a command buffer commits.

The touched set is therefore command-buffer-local for additions but
queue-local for retention: attachment handles touched in chunk N remain in
the set when chunk N+1 starts encoding, unless `R-BACK-15.5` invalidates
them.

---

## 3. Depth/Stencil Live-Out Tracking

### 3.1 Depth/stencil DontCare-store (`R-BACK-15.7`)

A depth or stencil attachment may use `MTLStoreActionDontCare` on render
pass close when no subsequent render pass within the same encoder-active
window samples or reads from it.

The conservative implementation must use `MTLStoreActionStore` (per the
default rule) unless the encoder can prove the attachment is not live-out.
Proofs that satisfy this requirement:

- The next pending operation that touches the depth/stencil texture is a
  `Clear` on the same handle. The previous contents are immediately
  discarded; storing them is wasted bandwidth.
- The next pending operation is a `SurfaceCopy` / `Readback` whose source
  is a different texture (i.e., the depth/stencil is unread).
- A scene-end signal (e.g., `EndScene`, present, swapchain reset) is
  pending and the application has not bound the depth/stencil for sampling
  in any pass since the last clear.

If none of those proofs are available, the depth/stencil store action must
remain `MTLStoreActionStore`.

### 3.2 Color RT live-out DontCare-store (`R-BACK-15.8`)

Symmetric rule for color attachments: a color attachment may use
`MTLStoreActionDontCare` when the encoder can prove the contents are not
read by any subsequent operation. The same proof set as `R-BACK-15.7`
applies. MSAA resolve overrides this rule: a color attachment with a
resolve target must use `MTLStoreActionMultisampleResolve`.

### 3.3 No look-ahead beyond the current chunk (`R-BACK-15.9`)

Live-out determination must use only operations already imported into the
current execution chunk. The encoder must not block waiting for future
chunks to determine the answer; a defensive `Store` is the correct fallback
when the next operation is unknown.

When a chunk ends without a definitive next-operation signal, the
attachment is treated as live-out and uses `Store`. The next chunk may
then DontCare-load if its first pass on the attachment is a clear or full
overwrite (per `R-BACK-15.4`).

---

## 4. Counters

### 4.1 Load-action histogram (`R-BACK-15.10`)

The `[dxmt9-perf]` line must report:

- `render_pass_load_action_load`
- `render_pass_load_action_clear`
- `render_pass_load_action_dontcare`

Each counts the number of color attachments that selected the matching
load action at `beginRenderPass` time. Sum across the three keys equals
the total color-attachment count across all render passes in the run.
Depth and stencil are counted into separate keys with the suffixes
`_depth_load` / `_depth_clear` / `_depth_dontcare` and
`_stencil_load` / `_stencil_clear` / `_stencil_dontcare`.

### 4.2 Store-action histogram (`R-BACK-15.11`)

Symmetric counters for store actions:

- `render_pass_store_action_store`
- `render_pass_store_action_dontcare`
- `render_pass_store_action_resolve` (MSAA)

Plus depth/stencil suffixes as in `R-BACK-15.10`.

### 4.3 Tile preservation savings estimate (`R-BACK-15.12`)

The `[dxmt9-perf]` line must report `render_pass_tile_preservation_bytes`,
an estimate of bytes that would have been read or written for tile
preservation under the legacy "always Load + always Store" policy. This is
computed at `beginRenderPass` time as `sum_over_attachments(width * height *
bytes_per_pixel * (load_was_load ? 1 : 0)) +
sum_over_attachments(width * height * bytes_per_pixel * (store_was_store ? 1
: 0))`. The counter is informational; success is a steady decrease of this
value across releases.

---

## 5. Safety Invariants

### 5.1 No DontCare on swapchain present source (`R-BACK-15.13`)

The texture that backs the swapchain back buffer at the time of `Present`
must not have its store action set to `DontCare` for the render pass that
last wrote it. This is required so the present blit / present pass can
read the back buffer contents. The encoder may use DontCare-store on the
back buffer only after `R-BACK-6.3` semantics apply (post-present).

### 5.2 No DontCare when MSAA resolve is active (`R-BACK-15.14`)

When a color attachment has a non-null `resolveTexture`, its store action
must remain `MTLStoreActionMultisampleResolve`; this spec does not relax
that contract.

### 5.3 No DontCare on locked surfaces (`R-BACK-15.15`)

If a surface has a pending lock or `GetRenderTargetData` call enqueued in
the current chunk, the encoder must not DontCare-store its contents. The
lock contract requires the contents be readable on the host side after
GPU completion.

### 5.4 Test coverage (`R-BACK-15.16`)

The `tests/native/backend/` corpus must add a deterministic test that
exercises each of `R-BACK-15.4` (first-use DontCare-load), `R-BACK-15.7`
(depth DontCare-store on next-clear), and `R-BACK-15.8` (color
DontCare-store on next-overwrite). The test asserts on the load/store
action selected for the corresponding `WMTRenderPassInfo` rather than on
GPU pixel output; pixel correctness is covered by the existing shader-
corpus tests, which must continue to pass with the new policy active.

---

## 6. Performance Contract

The post-implementation SFIV `-benchmark` measurement (1272 frames,
release build, current Apple Silicon hardware) must show:

- `render_pass_load_action_load` reduced by ≥ 30% versus the pre-spec
  baseline (current value: 17,793 of 24,995 color attachments use Load).
  Target: ≤ 12,000.
- `render_pass_store_action_store` for depth/stencil reduced by ≥ 50%
  versus the pre-spec baseline.
- `completion_present_wait_ms` reduced by ≥ 20% versus the post-`R-BACK-12`
  baseline (114,591 ms for 1272 frames). Target: ≤ 92,000 ms.
- `process_fps` increased by ≥ 30% versus the pre-spec baseline (current
  10 fps). Target: ≥ 13 fps.
- All 57 native unit tests remain green.
- Pixel output of `actual.png` for `dxmt9-perf-offscreen-heavy` remains
  byte-identical (full overwrite path, DontCare-load is a no-op there).

These targets are conservative; full vanilla-wined3d parity (~30 fps) is
the long-term goal but not gated on this spec alone — texture/shader
optimizations are tracked separately.

---

## 7. Amendment to `R-BACK-2.6`

The original `R-BACK-2.6` reads:

> A render target change (`SetRenderTarget`) during a scene must terminate
> the current `MTLRenderCommandEncoder` and begin a new one. The previous
> render target's store action must be `MTLStoreActionStore`.

The store-action clause is replaced by:

> The previous render target's store action defaults to
> `MTLStoreActionStore` and may be relaxed to `MTLStoreActionDontCare` only
> when the live-out proofs in `R-BACK-15.7` / `R-BACK-15.8` apply, the
> safety invariants in section 5 are satisfied, and the application
> contract for the resource (lock, present source, MSAA resolve) does not
> require preservation.

The encoder-split clause is unchanged: a render target change still
terminates the current encoder and begins a new one.

The amendment must land in `specs/backend/requirements.md` as part of the
implementation PR for this spec.
