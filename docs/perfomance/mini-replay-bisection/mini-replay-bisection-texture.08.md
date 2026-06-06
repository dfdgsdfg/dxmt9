---
domain: mini-replay-bisection
workload: 3DMark05 GT1
subcategory: texture
order: 08
title: Occlusion Oracle Feasibility Gate
date: 2026-06-06
type: code-audit
status: blocked-current-implementation
source: docs/perfomance/mini-replay-bisection/mini-replay-bisection-texture.07.md; src/d3d9/core_resources.cpp; src/d3d9/core_draw.cpp; src/d3d9/d3d9_pe_device_child_misc.cpp; src/winemetal/winemetal.h; src/winemetal/Metal.hpp; src/winemetal/unix/winemetal_private_api.mm; tests/native/core/core_device_lifecycle_spec.cpp; tests/conformance/d3d9/d3d9_queries.cpp
---

# Occlusion Oracle Feasibility Gate

**Question / hypothesis.** The primitive-conflict scout says the remaining
depth-read locality path needs final-color/final-writer or occlusion data. Can
the existing D3D9 occlusion query path or winemetal visibility API be reused as
a cheap production oracle for scoped `60/2` reorder?

**Method.**

1. Audited the PE query wrapper. `IDirect3DQuery9::Issue()` is ordered through
   the chunk recorder or a flush+bridge fallback; `GetData()` flushes before
   reading the core query.
2. Audited core query resolution. `Device::issueQuery(begin)` resets
   `activeOcclusionCount_` to `0`; draw submission increments it by submitted
   `primitiveCount`; `Device::issueQuery(end)` resolves that counter.
3. Checked tests. The native lifecycle test expects an occlusion result of `2`
   for `drawPrimitive(TriangleList, 2)`, proving the current core path is a
   primitive-submission compatibility counter, not a framebuffer visibility
   sample counter.
4. Audited winemetal visibility plumbing. `WMTRenderPassInfo` carries
   `visibility_buffer`, and `RenderCommandEncoder::setVisibilityResultMode()`
   exists, but the dxmt9 draw encoder does not bind a visibility buffer or issue
   visibility-mode commands for normal GT1 draw encoding.

```mermaid
flowchart TD
  Need["depth-read reorder candidate\nneeds final-color/final-writer proof"] --> Existing{"reuse existing D3D9\nocclusion query?"}
  Existing -- "current core path" --> Primitive["activeOcclusionCount += primitiveCount"]
  Primitive --> RejectD3D9["reject as oracle\nno framebuffer visibility signal"]

  Need --> Metal{"reuse Metal visibility?"}
  Metal -- "winemetal surface exists" --> Surface["visibilityResultBuffer\nsetVisibilityResultMode"]
  Surface --> Missing["dxmt9 draw encoder does not bind\nvisibility buffer or mode"]
  Missing --> Future["future instrumentation only\nnot current production gate"]

  Need --> Replay["mini-replay final-color oracle"]
  Replay --> Offline["works offline\nnot cheap runtime predicate"]

  RejectD3D9 --> Decision["do not promote depth-read reorder"]
  Future --> Decision
  Offline --> Decision

  classDef ok fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef warn fill:#fff3cd,stroke:#a80,color:#640
  classDef bad fill:#f8d7da,stroke:#a33,color:#600
  class Surface,Replay ok
  class Need,Metal,Existing,Future,Offline warn
  class Primitive,RejectD3D9,Missing,Decision bad
```

```mermaid
sequenceDiagram
  participant App as D3D9 app
  participant PE as PE query wrapper
  participant Core as dxmt9 core
  participant Draw as draw submit
  participant Metal as Metal encoder

  App->>PE: Issue(D3DISSUE_BEGIN)
  PE->>Core: ordered query begin
  Core->>Core: activeOcclusionCount_ = 0
  App->>Draw: DrawPrimitive / DrawIndexedPrimitive
  Draw->>Core: activeOcclusionCount_ += primitiveCount
  Draw->>Metal: normal draw encoding
  Note over Metal: no visibilityResultBuffer/mode in current dxmt9 draw path
  App->>PE: Issue(D3DISSUE_END)
  PE->>Core: ordered query end
  Core->>Core: resolve(activeOcclusionCount_)
  App->>PE: GetData()
  PE->>Core: flush + read
  Core-->>App: primitive-count-compatible DWORD
```

**Result.** The existing occlusion query path is not the required oracle. It is
useful for current app/query compatibility tests, but its resolved value is
derived from submitted primitive count. It does not know whether fragments pass
depth/stencil, whether alpha/blend changes final color, or whether later draws
overwrite a candidate's contribution.

Metal visibility is a plausible future scout, not an available gate. The lower
winemetal ABI already exposes the render-pass visibility buffer and
`setVisibilityResultMode`, so an experiment could be built without inventing a
new Metal primitive. However, production use would need a new feedback design:
visibility-buffer allocation, per-candidate offsets, mode toggles around the
candidate draws, readback or delayed-frame feedback, and a conservative policy
for count-positive draws. A zero count could prove "no samples passed at this
point"; it still would not prove broader final-color equivalence for every
count-positive, blended, or later-overwritten case.

```mermaid
stateDiagram-v2
  [*] --> OfflineReplay
  OfflineReplay --> RuntimeSelector: color-exact ranks exist
  RuntimeSelector --> RejectedSelector: runtime fields / non-color metrics overlap
  RejectedSelector --> ExistingQuery
  ExistingQuery --> QueryRejected: primitive-count only
  QueryRejected --> MetalVisibilityScout
  MetalVisibilityScout --> FutureWork: requires new visibility buffer feedback loop
  FutureWork --> ReorderBlocked
  ReorderBlocked --> [*]
```

**Interpretation.** This narrows the meaning of the continuing experiment. The
mini-replay ranks have found real locality gain, but the current implementation
does not have a cheap runtime signal that can separate rank-1 visible failure
from rank2-4 owner-masked exact passes. Existing D3D9 occlusion query state
cannot be repurposed for that signal. The remaining production choices are:

- use the diagnostic Metal visibility scout added in
  [[mini-replay-bisection-texture.09]] only for no-sample triage, then add
  final-color/final-writer proof for count-positive rows;
- find a stricter runtime-visible selector that does not depend on final color;
- stop spending reorder/Xcode budget here and return to non-reorder backend
  mechanisms.

**Related.** [[mini-replay-bisection]] ·
[[mini-replay-bisection-texture.07]] · [[index-cache-locality]] ·
[[hidden-backend-storage]].
