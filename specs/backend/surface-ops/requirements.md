# Surface Operations Requirements

Surface operations are non-draw D3D9 commands that copy, scale, fill, or read back
surface data. They are recorded into the normal backend command stream and replayed
by the unix-side command queue.

The top-level backend requirement IDs remain in
[`../requirements.md`](../requirements.md#9-surface-operations) as `R-BACK-9.*`.
This file expands those requirements into API contracts and validation policy.

---

## 1. Operations in Scope

| D3D9 API | Backend operation | Summary |
|---|---|---|
| `UpdateSurface(src, srcRect, dst, dstPoint)` | `SurfaceCopyRecord` | Copy a SYSTEMMEM surface region to a DEFAULT surface without stretching. |
| `UpdateTexture(src, dst)` | `SurfaceCopyRecord` sequence | Copy dirty mip levels and faces from a SYSTEMMEM texture to a DEFAULT texture. |
| `StretchRect(src, srcRect, dst, dstRect, filter)` | `StretchRectRecord` | Copy between DEFAULT surfaces, using a fast blit or a scaled render pass. |
| `ColorFill(surface, rect, color)` | `ColorFillRecord` | Fill a DEFAULT surface or render target region with a D3D color. |
| `GetRenderTargetData(rt, dst)` | `ReadbackRecord` | Copy a render target into SYSTEMMEM or SCRATCH storage and wait for completion. |

---

## 2. D3D9 API Contracts

### 2.1 `UpdateSurface`

`IDirect3DDevice9::UpdateSurface(pSourceSurface, pSourceRect, pDestSurface, pDestPoint)`
must follow the D3D9 upload contract:

- The source surface must be in `D3DPOOL_SYSTEMMEM`.
- The destination surface must be in `D3DPOOL_DEFAULT`.
- Source and destination formats must be compatible. The backend must not perform
  format conversion.
- The copied source and destination regions must have identical width and height.
  Stretching is invalid for `UpdateSurface`.
- The destination point must produce a region fully contained by the destination
  surface.
- If the source format requires CPU-side conversion, the core must complete that
  conversion before recording the command. The backend receives Metal-native bytes.

### 2.2 `UpdateTexture`

`IDirect3DDevice9::UpdateTexture(pSourceTexture, pDestTexture)` must be equivalent
to applying `UpdateSurface` to every dirty mip level, cube face, or array slice that
belongs to the texture pair:

- The source texture must be in `D3DPOOL_SYSTEMMEM`.
- The destination texture must be in `D3DPOOL_DEFAULT`.
- Texture type, dimensions, mip count, face/slice count, and compatible format must
  match the D3D9 contract.
- Each recorded copy must preserve source row pitch and slice pitch exactly.
- Dirty tracking may reduce the copied set, but it must never skip data that D3D9
  makes visible after `UpdateTexture` returns.

### 2.3 `StretchRect`

`IDirect3DDevice9::StretchRect(pSrc, pSrcRect, pDst, pDstRect, Filter)` must support
valid DEFAULT-pool surface copies, including same-size copies and scaled copies:

- Source and destination rectangles must be fully contained by their surfaces.
- Same-size copies use copy semantics and must not apply filtering.
- Scaled copies must map `D3DTEXF_NONE` and `D3DTEXF_POINT` to nearest filtering,
  and `D3DTEXF_LINEAR` to linear filtering.
- Unsupported filters, unsupported source/destination usage, or unsupported format
  pairs must return `D3DERR_INVALIDCALL` from the core before backend execution.
- Format compatibility is limited to identical or storage-compatible formats that
  the selected Metal path can copy or render without conversion.

### 2.4 `ColorFill`

`IDirect3DDevice9::ColorFill(pSurface, pRect, color)` must fill a DEFAULT-pool plain
surface or render target:

- A null rectangle means the full surface.
- A non-null rectangle must be contained by the destination surface.
- The D3D color must be converted to the destination format's clear/fill value before
  or during encoding without changing D3D9-visible channel semantics.
- Unsupported formats or non-DEFAULT destinations must return `D3DERR_INVALIDCALL`.

### 2.5 `GetRenderTargetData`

`IDirect3DDevice9::GetRenderTargetData(pRenderTarget, pDestSurface)` is a synchronous
readback contract:

- The source must be a render target in `D3DPOOL_DEFAULT`.
- The destination must be a compatible SYSTEMMEM or SCRATCH surface.
- Source and destination dimensions and formats must be compatible with the D3D9
  readback contract.
- Multisample render targets must be resolved to a single-sample texture before
  readback.
- The call must not return until all prior GPU writes that affect the source are
  complete and the destination CPU memory contains the copied data.

---

## 3. Format and Validation Policy

Surface operation validation is owned by the core and the unix-side importer:

- The core validates D3D9 API rules, pool restrictions, rectangle bounds, mip/face
  enumeration, usage restrictions, and format compatibility before recording.
- The PE recorder emits fixed-layout POD records only. Records contain scalar
  dimensions, pitches, offsets, flags, colors, filter values, and handle-table
  indices; they must not contain COM pointers, Objective-C pointers, C++ lambdas,
  `std::function`, vtables, or process-local pointers.
- The unix importer validates record opcode, payload size, reserved fields, handle
  kinds, handle liveness, pitch/offset bounds, and payload arena ranges before queue
  ownership begins.
- Backend encoding assumes imported records are already canonicalized and retained.
  It may reject stale or malformed handles discovered at import, but it must not
  reinterpret D3D9 COM objects while replaying.
- CPU-side format conversion belongs outside the backend surface operation records.
  Backend records describe bytes already suitable for the selected Metal resource.

---

## 4. Command Chunk Recording

All surface operations cross the Wine PE/unix boundary as ordinary `CommandChunk`
POD records:

- `UpdateSurface`, each `UpdateTexture` sub-copy, same-size `StretchRect`, scaled
  `StretchRect`, and `ColorFill` append records to the current chunk.
- Records reference resources by chunk handle-table indices plus schema-defined
  resource kinds.
- Variable data is addressed by payload offsets and sizes inside the chunk arena.
- The unix importer turns validated wire records into retained imported POD records
  and queue-local replay actions. No closure, lambda, or process-local callable
  crosses the bridge.
- Surface operations share the normal sequence timeline with draw, clear, and
  present commands.

---

## 5. Readback Stall and Ordering

`GetRenderTargetData` is the only surface operation that forces an immediate GPU
completion wait:

1. The core records a `ReadbackRecord` in the current chunk.
2. The core immediately commits the chunk at that ordering point.
3. The backend imports the record, retains the source and staging resources, encodes
   the texture-to-buffer copy, and commits the Metal command buffer.
4. The calling Wine thread waits for the sequence ID associated with the committed
   chunk.
5. After completion, CPU-visible staging data is copied into the destination
   SYSTEMMEM or SCRATCH surface before the D3D9 call returns.

The wait must include all prior commands in submission order that can affect the
source render target. The backend must not satisfy the readback from an older
snapshot or from a command buffer that has only been encoded but not completed.

---

## 6. Per-Operation Bridge Prohibition

Surface operations must not introduce a per-operation Wine bridge path:

- The default runtime path for `UpdateSurface`, `UpdateTexture`, `StretchRect`, and
  `ColorFill` is chunk recording plus `commitChunk()` at the normal chunk boundary.
- `GetRenderTargetData` may force an immediate chunk commit and sequence wait, but
  the command itself still crosses as a POD chunk record.
- Compatibility helpers or test-only entry points may exist, but they must not become
  the Wine runtime hot path.
- A design that issues one `WINE_UNIX_CALL` per surface copy, per mip upload, per
  fill, or per readback sub-step is non-compliant.
