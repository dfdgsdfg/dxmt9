---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: implementation-gate
order: 107
title: Open-CB Injected Command Buffer
date: 2026-06-19
type: implementation-note
status: accepted-primitive
outdated: evidence-missing
source: src/dxmt9/dxmt9_draw_encoder.hpp, src/dxmt9/dxmt9_draw_encoder.mm, tests/native/backend/render_backend_batch_contract_spec.cpp
related: docs/perfomance/present-pacing/present-pacing-open-cb-encode-options.106.md, docs/perfomance/present-pacing/present-pacing-encoded-tail-record-merge.105.md
---

# Present Pacing / Open-CB Injected Command Buffer 107

> **Outdated — every artifact this leaf cites in `source:` is gone from disk.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question.** What is the next minimal encoder-side gate after H106's split
guards?

**Answer.** `encodeChunk()` now accepts an optional owned
`WMT::CommandBuffer` through `EncodeChunkOptions::commandBuffer`. When absent,
the current behavior is unchanged: the encoder asks `CommandQueue` for a fresh
command buffer. When present, the encoder appends the chunk into the supplied
command buffer and transfers that same handle into the returned
`QueueSubmissionRecord`.

This is still only an implementation primitive. No runtime P4/FPS claim follows
until a queue/backend path actually pre-encodes a pre-Present head, retains its
sources through H104, merges the final tail record through H105, and passes the
no-gputrace locality plus `v0.0.3` visual gates.

## Contract

```mermaid
flowchart TD
  A["Tail-Present overlap candidate"] --> B{"EncodeChunkOptions.commandBuffer?"}
  B -- "No" --> C["Default path\nqueue.newCommandBuffer()"]
  B -- "Yes" --> D["Adopt supplied open CB"]

  C --> E["Current renderer behavior\nmid-chunk policy unchanged"]
  D --> F["Append work into existing CB"]
  F --> G["Force internal split/commit off\nfor injected CB"]
  G --> H["Return record carrying same CB"]
  H --> I["H105 tail merge requires same CB handle"]
  I --> J["One final tail commit\nexpanded completion sources"]
```

## Safety Notes

| Area | Meaning |
|---|---|
| Default callers | unchanged; `EncodeChunkOptions{}` has no injected command buffer |
| Mid-chunk commits | automatically disabled for an injected command buffer |
| Present acquire split | automatically disabled for an injected command buffer |
| Capture semantics | the supplied command buffer may have been created before chunk-begin capture, so a future runtime caller must audit capture start timing before relying on `.gputrace` output |
| Test coverage | native contract test locks the default fresh-CB option shape; non-null fake `WMT::Reference` handles are deliberately not used |

The key policy is conservative: an injected command buffer must remain open
inside `encodeChunk()`. If the caller wants the old split behavior, it should
use the default fresh-CB path instead of the open-CB carrier.
