---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: tail-present-staging
order: 97
title: Tail-Present Complete-Prefix Selector
date: 2026-06-19
type: implementation
status: accepted-primitive-followed-by-h98
source: src/dxmt9/dxmt9_queue.hpp, src/dxmt9/dxmt9_queue.cpp, src/dxmt9/dxmt9_command_queue.cpp, src/dxmt9/render/tail_present_batch.hpp, src/dxmt9/render/tail_present_batch.cpp, tests/native/backend/queue_completion_sources_spec.cpp, tests/native/backend/render_backend_batch_contract_spec.cpp
related: docs/perfomance/present-pacing/present-pacing-tail-present-merge-primitive.96.md, docs/perfomance/present-pacing/present-pacing-tail-present-multi-head-audit.95.md, docs/perfomance/present-pacing/present-pacing-tail-present-staged-runtime.94.md
---

# Present Pacing / Tail-Present Complete-Prefix Selector 97

**Question.** After H96 made multi-head `ChunkSlot` merge safe, can the queue
dequeue selector prove that a complete `[head..., Present-only tail]` prefix is
available before consuming more than one ready source?

**Answer.** Yes. `QueueLifecycleController` now has a prefix-selector dequeue
primitive. The selector inspects the FIFO ready queue and the current slot span
before any state transition. If it returns a complete prefix length, those
sources move to `Encoding` together. If it returns zero, the queue falls back to
the legacy single-source dequeue. This keeps incomplete head-only batches from
reaching `onChunkBatchReady()` and being completed inline.

`render::selectTailPresentBatchPrefix()` is the first selector user. It accepts
only:

- one or more non-empty, non-present head slots;
- followed by a Present-only tail slot;
- with the tail inside caller scratch capacity.

`CommandQueue` now uses ring-sized scratch (`kCommandChunkCount`) for
`DXMT9_ENCODE_TAIL_PRESENT_BATCH=1`, so a future earlier-staging path can
release several staged heads plus the tail into one backend batch.

## Closed Gate

```mermaid
sequenceDiagram
  participant Q as QueueLifecycleController
  participant S as Prefix selector
  participant B as Backend batch encoder

  Q->>S: inspect readySlots + slots + maxCount
  alt complete [head..., Present-only tail]
    S-->>Q: prefix count > 1
    Q->>Q: transition exactly that prefix to Encoding
    Q->>B: encode sources as one tail batch
  else incomplete or tail outside scratch
    S-->>Q: 0
    Q->>Q: fallback to one source
    Q->>B: legacy single-source encode
  end
```

This closes the second H95/H96 implementation gate: multi-head tail batches now
have both a safe slot merge primitive and a safe complete-pattern dequeue
primitive.

## Test Coverage

Focused native coverage was added to:

- `dxmt9-queue-completion-sources-spec`
  - selector-provided complete prefix moves every selected source to
    `Encoding`;
  - selector rejection falls back to exactly one source and leaves later ready
    slots Pending/FIFO-visible.
- `dxmt9-render-backend-batch-contract-spec`
  - `[head, head, Present-only tail]` selects length 3;
  - tail outside scratch capacity selects 0;
  - head-only and pre-tail-present shapes select 0.

Verification run:

```sh
meson test -C build-arm64-nowine \
  dxmt9-queue-completion-sources-spec \
  dxmt9-render-backend-batch-contract-spec
```

Result: `2/2` passed.

## Remaining Gate

This is still not a P4 runtime promotion and does not justify `.gputrace`
budget by itself. Current `DXMT9_STAGE_TAIL_PRESENT_CHUNK=1` staging happens at
`submitPresent()` and can only create the historical one-head plus tail shape.
H98 follows this primitive with the first earlier pre-Present command-limit
stage trigger.

The next evidence step is a no-gputrace gate for the H98 runtime candidate:
P4 movement, CB/pass/tile locality, and `v0.0.3` visual safety.

```mermaid
flowchart TD
  H95["H95 audit\nmulti-head unsafe"] --> H96["H96\nChunkSlot merge/remap"]
  H96 --> H97["H97\ncomplete-prefix selector"]
  H97 --> Stage["H98: earlier pre-Present staging trigger"]
  Stage --> Gate["next: no-gputrace P4/locality/v0.0.3 gate"]
  Gate --> Xcode["only then: Xcode/gputrace budget"]

  classDef done fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef wait fill:#fff3cd,stroke:#a80,color:#640
  class H95,H96,H97 done
  class Stage,Gate,Xcode wait
```
