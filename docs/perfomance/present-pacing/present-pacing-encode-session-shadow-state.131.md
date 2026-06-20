---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: implementation-scaffold
order: 131
title: Encode Session Shadow State
date: 2026-06-20
type: implementation-scaffold
status: accepted-prerequisite
source: src/dxmt9/dxmt9_draw_encoder.mm, docs/perfomance/present-pacing/present-pacing-encode-session-injection-api.130.md, docs/perfomance/present-pacing/present-pacing-render-pass-carry-contract.128.md
related: docs/perfomance/present-pacing/present-pacing-encode-session-injection-api.130.md, docs/perfomance/present-pacing/present-pacing-encode-session-state-scaffold.129.md, docs/perfomance/present-pacing/present-pacing-render-pass-carry-contract.128.md
---

# Present Pacing / Encode Session Shadow State 131

**Question.** After H130 added an opaque session injection API, which remaining
`encodeChunk()` locals still had to move into the session owner before a real
render-pass carry candidate could be safe?

**Answer.** The encoder-local shadow/cache state now lives in
`EncodeChunkSessionStorage` alongside the active Metal encoders and attachment
state. Default behavior is still one-shot: `encodeChunk()` closes the render
encoder, emits diagnostics, and resets an injected session before returning.

## Moved State

| State | Why it belongs to the session |
|---|---|
| `lastArgbufPayload*` | argbuf reopen decisions depend on the previous draw in the same Metal render encoder |
| `ArgbufCbufCache` | cbuf table reuse is encoder-local and must not reset at staged-source boundaries |
| `StreamIbStagingCache` | staged stream/IB reuse is scoped to the active encoder breakdown row |
| `TextureSamplerBindShadow` | direct texture/sampler bind elision is valid only until an encoder boundary |
| `ActiveEncoderBreakdown` | sidecar rows must describe the logical render encoder, not source fragments |
| `VisibilityScoutPass` | visibility result callbacks are tied to the active render encoder |
| `renderEncoderIndex` | pass labels, sidecars, and samples need monotonic session-local encoder IDs |

```mermaid
flowchart TD
  A["EncodeChunkSessionStorage"] --> B["active render/blit encoder"]
  A --> C["attachment key + hazard"]
  A --> D["dirty state + draw-state key"]
  A --> E["argbuf payload/cbuf cache"]
  A --> F["texture/sampler shadow"]
  A --> G["encoder breakdown + visibility scout"]
  A --> H["render encoder index"]

  I["Current encodeChunk()"] --> A
  I --> J["flushRender(Final)"]
  J --> K["reset injected session"]
```

## Non-Claims

- This does not enable render-pass carry.
- This does not improve FPS.
- This does not change default command-buffer or render-pass counts.
- This does not justify `.gputrace` until a later opt-in candidate passes the
  H128 no-gputrace and visual gates.

## Verification

Focused native coverage compiled the Objective-C++ encoder path after moving
the extra session fields:

```sh
meson test -C build-arm64-nowine dxmt9-queue-completion-sources-spec
```

## Next Gate

The next implementation step is still an explicit session finalizer. It must
close a carried render encoder, emit the sidecars/callbacks/samples attached to
that logical encoder, and clear the session before the queue submits, aborts,
or completes a pending open-CB record.
