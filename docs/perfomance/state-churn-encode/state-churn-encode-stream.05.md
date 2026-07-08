---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: stream
order: 05
title: Stream/IB Binding Tuple Structure
date: 2026-06-06
type: validation
status: accepted-gate
source: traces/app-d3d9-3dmark05-stream-extra-bindings-r1/analysis/frame60-stream-ib-backend-churn.md; traces/app-d3d9-3dmark05-stream-extra-bindings-r1/analysis/frame60-stream-ib-backend-churn.csv; experiments/output/app-d3d9-3dmark05-stream-extra-bindings-r1/3dmark05-perf-indexed-probe-draws.csv; scripts/tools/analyze_stream_ib_backend_churn.py; tests/scripts/test_analyze_stream_ib_backend_churn.py; src/dxmt9/dxmt9_draw_encoder.mm; src/dxmt9/dxmt9_resource_pool.cpp; src/d3d9/core_buffer.cpp
---

# Stream/IB Binding Tuple Structure

**Question / hypothesis.** [state-churn-encode-stream.04](state-churn-encode-stream.04.md) proved that frame60
hot rows are stream/IB handle-churn-dominant. Is that churn an unbounded
allocation problem, or a bounded binding tuple alternation that can be isolated
with a handle-stable A/B?

**Method.** Extended `scripts/tools/analyze_stream_ib_backend_churn.py` so the
optional probe-draw join reports:

- complete draw binding tuple top-N and transition classes,
- stream0/IB pair deltas,
- stream0/first-extra-stream/IB triplet deltas,
- consecutive pair/triplet ratios.

The report was regenerated from
`experiments/output/app-d3d9-3dmark05-stream-extra-bindings-r1/` into
`traces/app-d3d9-3dmark05-stream-extra-bindings-r1/analysis/frame60-stream-ib-backend-churn.md`.

**Result.**

| Row | Tuple changes | Unique tuples | Transition classes | Consecutive s0/IB pairs | Consecutive s0/s1/IB triplets | Top delta |
|---|---:|---:|---|---:|---:|---|
| `60/2` | `160/187` | `58` | `s0+ib+extra` `111`, `s0+ib` `49`, `same` `26` | `168/187` (`0.898`) | `132/187` (`0.706`) | pair `+2x168`, triplet `+1/+2x132` |
| `60/1` | `130/156` | `86` | `s0+ib` `130`, `same` `25` | `156/156` (`1.000`) | `0/0` | pair `+2x156` |
| `60/0` | `36/42` | `25` | `s0+ib` `36`, `same` `5` | `42/42` (`1.000`) | `0/0` | pair `+2x42` |

This says the hot rows are not allocating endlessly. They alternate through a
bounded set of draw-local geometry objects. In `60/2`, `132/187` draws use the
consecutive core-handle shape `stream0`, `stream1 = stream0 + 1`,
`IB = stream0 + 2`; `168/187` draws at least preserve the `stream0`,
`IB = stream0 + 2` pair. Rows `60/1` and `60/0` have no active extra stream in
the probe-draw rows, but every draw has the same `stream0`, `IB = stream0 + 2`
pair pattern.

```mermaid
flowchart TD
  A["probe-draw telemetry"] --> B["draw tuple = stream0 + extra streams + IB"]
  B --> C{"hot-row tuple structure"}
  C -- "many unique, no repeats" --> D["allocation churn hypothesis"]
  C -- "bounded tuples, short runs" --> E["binding alternation hypothesis"]
  E --> F["60/2: 58 tuples / 187 draws\nmax run 6, avg run 1.161"]
  E --> G["60/0,60/1: stream0/IB pair +2 for every draw"]
  F --> H["handle-stable A/B must change Metal buffer identity\nwithout changing geometry/order/state"]
  G --> H
```

**Code implication.** The current binding path already skips redundant Metal
calls when the same `(MTLBuffer, offset)` is still bound in the same slot. The
hot rows still churn because the draw data lives behind different buffer
records and therefore different Metal buffer identities. `core::Device` creates
one core handle per D3D buffer, `dxmt9::Pool::createBuffer()` creates the
corresponding WMT buffer, and the draw encoder binds each stream/IB record's
live buffer. A stronger bind cache cannot make different handles look stable
without lying about the data.

```mermaid
sequenceDiagram
  participant App as D3D9 app
  participant Core as core::Device
  participant Pool as dxmt9 Pool
  participant Enc as Draw encoder
  participant Metal as Metal encoder

  App->>Core: Create stream0 / stream1 / IB buffers
  Core->>Pool: createBuffer(desc) per D3D buffer
  Pool-->>Core: distinct core handles and WMT buffers
  App->>Enc: DrawIndexedPrimitive
  Enc->>Enc: resolve stream0, extra stream, IB BufferRecord
  Enc->>Metal: setVertexBuffer(stream0)
  Enc->>Metal: setVertexBuffer(stream1)
  Enc->>Metal: drawIndexedPrimitives(IB)
  Note over Enc,Metal: cache skip only helps if the same Metal buffer+offset repeats
```

**Verdict.** Accepted as a sharper gate. The next valid stream/IB experiment is
not "add another bind-cache skip"; it is a handle-stable no-gputrace A/B that
presents the same geometry bytes through fewer/stable Metal buffer identities
while holding draw order, index order, VS invocation count, render state, and
visible shader layout fixed. Plausible mechanisms are per-row tuple packing or
resource-level coalescing/staging; both change data layout and must be guarded
by visual correctness before any Xcode counter spend. If such an A/B cannot
reduce handle changes without changing those denominators, stream/IB should be
rejected as the hidden-backend owner and the investigation should move back to
TVB/parameter-storage shape.

```mermaid
stateDiagram-v2
  [*] --> RawHandleChurn
  RawHandleChurn --> TupleStructureKnown
  TupleStructureKnown --> BindCacheRejected: different buffers, not redundant binds
  TupleStructureKnown --> HandleStableAB: pack/coalesce/stage geometry
  HandleStableAB --> XcodeCandidate: handle churn reduced and frame shape stable
  HandleStableAB --> StreamIBRejected: no byte/invocation movement
  XcodeCandidate --> [*]
```

**Related.** [state-churn-encode-stream.04](state-churn-encode-stream.04.md) · [state-churn-encode](../state-churn-encode.md) ·
[hidden-backend-storage](../hidden-backend-storage.md) · [hidden-backend-storage-shape.11](../hidden-backend-storage/hidden-backend-storage-shape.11.md) ·
[index-cache-locality](../index-cache-locality.md).
