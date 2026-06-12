# DXMT9 Performance Bottleneck Model

Date: 2026-06-05

Scope:

- macOS Wine D3D9 path using dxmt9 PE `d3d9.dll`, PE `winemetal.dll`,
  and unix `winemetal.so`.
- General dxmt9 performance model, not a title-specific investigation.
- Current backend structure: chunked D3D9 command recording, encode-thread
  replay, Metal render/pass encoding, sub-command-buffer chaining, and
  present-frame pacing.
- This document keeps the existing directory spelling, `docs/perfomance/`,
  to match the current repository path.

## Bound Legend

- CPU-bound: app thread, Wine PE thunking, D3D9 state validation, command
  recording, bridge/import validation, resource marking, encode-thread command
  generation, pipeline lookup, FVF/declaration lowering, and CPU-side upload
  bookkeeping.
- GPU/driver-bound: Metal command-buffer execution, render-pass tile
  load/store, pipeline compilation, CAMetalLayer drawable acquisition, present
  scheduling, compositor pacing, and driver-side residency/allocation work.
- Sync-bound: CPU waits caused by ring pressure, frame-latency tokens,
  drawable availability, query/readback fences, or GPU completion. These are
  CPU stalls whose root cause may be CPU backlog, GPU work, driver pacing, or
  present pacing.

## Current dxmt9 Shape

```mermaid
flowchart TD
  subgraph PECPU["PE-side CPU: D3D9 calls record into the current chunk"]
    App[D3D9 app]
    D3D9PE[PE d3d9.dll]
    Recorder[Command recorder]
    ChunkBuffer[current chunk buffer]
    ChunkBoundary[chunk boundary: flush/present/ring pressure]
  end

  subgraph ImportBoundary["PE/unix boundary: crossed at chunk import, not per D3D9 call"]
    UnixCall[Wine unix-call bridge]
  end

  subgraph UnixCPU["Unix-side CPU: import, queue lifecycle, encode replay"]
    WinemetalSO[winemetal.so Metal transport]
    Core[dxmt9 core import]
    CQ[CommandQueue]
    ChunkRing[32-slot chunk ring]
    Pending[ready chunk queue]
    EncodeThread[encode thread]
    Replay[chunk replay planner]
    DrawEnc[draw/blit/present encoders]
  end

  subgraph GPUDriver["GPU/driver-bound: Metal + CAMetalLayer + compositor"]
    SubCB[1..N Metal command buffers per chunk]
    RenderPass[Metal render command encoders]
    Presenter[Presenter drawable acquisition]
    Layer[CAMetalLayer nextDrawable]
    Present[commandBuffer presentDrawable]
  end

  subgraph Sync["Sync-bound progress signals"]
    Completed[completedSeqId]
    PresentCompleted[presentCompletedSeqId]
    PresentToken[present frame token]
    BoundaryWait[present/query/readback boundary waits]
    Reclaim[resource/transient reclaim]
  end

  App --> D3D9PE --> Recorder --> ChunkBuffer --> ChunkBoundary --> UnixCall
  UnixCall --> Core --> CQ --> ChunkRing --> Pending --> EncodeThread --> Replay --> DrawEnc
  Core -. Metal resource/device transport .-> WinemetalSO
  DrawEnc -. Metal object access .-> WinemetalSO
  DrawEnc --> RenderPass --> SubCB
  DrawEnc --> Presenter --> Layer --> Present --> SubCB
  SubCB --> Completed
  SubCB --> PresentCompleted
  Completed --> Reclaim
  PresentCompleted --> PresentToken
  PresentToken --> BoundaryWait
  Completed -. query/readback fence .-> BoundaryWait

  classDef cpu fill:#eaf4ff,stroke:#2f6fad,color:#0b2239
  classDef gpu fill:#fff0d6,stroke:#b26b00,color:#2b1900
  classDef sync fill:#ffe8e8,stroke:#b64242,color:#2b0d0d
  class App,D3D9PE,Recorder,ChunkBuffer,ChunkBoundary,UnixCall,WinemetalSO,Core,CQ,ChunkRing,Pending,EncodeThread,Replay,DrawEnc cpu
  class SubCB,RenderPass,Presenter,Layer,Present gpu
  class Completed,PresentCompleted,PresentToken,BoundaryWait,Reclaim sync
```

### Current Sequence

```mermaid
sequenceDiagram
  participant App as D3D9 app
  participant PE as PE d3d9 recorder
  participant Bridge as PE/unix chunk import
  participant CQ as CommandQueue
  participant QLC as QueueLifecycleController
  participant Encode as encode thread
  participant Presenter as Presenter
  participant Metal as Metal command buffers
  participant Complete as completion path

  Note over App,PE: CPU-bound hot path: D3D9 state validation and chunk packet recording
  Note over Bridge,CQ: CPU-bound chunk boundary: unix-call import, validation, resource marking, queue publish
  Note over Encode,Metal: CPU/GPU boundary: replay, render-pass decisions, PSO lookup, binds, draws, commits
  Note over Presenter,Complete: Driver/sync-bound: drawable acquire, present pacing, completion fences

  loop many D3D9 calls inside one chunk
    App->>PE: Draw / Clear / Copy / Lock
    PE->>PE: append records, payloads, and resource references
  end

  App->>PE: Present / Flush / chunk rollover
  PE->>Bridge: publish chunk buffer
  Bridge->>CQ: import chunk records and submit resource work
  CQ->>QLC: reserve writer slot
  CQ->>QLC: publish pending chunk
  QLC-->>Encode: dequeue ready chunk
  Encode->>Encode: validate/import view
  Encode->>Encode: build draw runs where possible
  Encode->>Encode: encode render passes and binds

  loop per render-pass boundary until cap
    Encode->>Metal: optional mid-chunk commit
  end

  Encode->>Presenter: acquire drawable for present-bearing tail
  Encode->>Metal: final commit for chunk seqId
  Complete->>Metal: observe completion
  Complete->>QLC: advance completedSeqId
  Complete->>CQ: advance presentCompletedSeqId when present-bearing
  CQ-->>Bridge: release present/query/readback boundary waiters
  Bridge-->>PE: return from boundary wait when required
```

### Current Chunk State

```mermaid
stateDiagram-v2
  [*] --> Free
  Free --> Writing: writer slot reserved / CPU
  Writing --> Pending: chunk published / CPU
  Pending --> Encoding: encode thread dequeues / CPU
  Encoding --> GPU: tail command buffer submitted
  GPU --> Free: tail completion advances seqId and reclaims slot

  Writing --> Free: empty commit
  Encoding --> Free: no-work chunk completes inline

  note right of Writing
    PE-side app/API work.
    Hot spots: packet construction,
    state normalization, validation,
    resource handle retention.
  end note

  note right of Encoding
    CPU encode work.
    Hot spots: draw-run formation,
    PSO lookup, FVF/declaration decode,
    stream/IB/texture/sampler binds,
    transient uploads.
    per-render-pass mid-chunk split
    can create a 1..N sub-CB chain.
  end note

  note right of GPU
    Slot state stays GPU while the
    committed tail command buffer is
    pending. Earlier sub-CBs are ordered
    on the same Metal queue; tail
    completion is the public seqId
    completion point. Present attaches
    to the final command buffer only.
    Sync waits must key off the
    chunk final completion, not an
    intermediate sub-command buffer.
  end note
```

### Current Buffering Strategy

```mermaid
flowchart TD
  subgraph CPURecord["CPU-bound: app thread + command recording"]
    App[D3D9 app thread]
    State[D3D9 state snapshots]
    Payloads[constant/upload payload arenas]
    Handles[resource handle retention]
    ChunkRing[CommandQueue chunk ring]
  end

  subgraph EncodeSide["CPU-bound encode side"]
    Import[imported chunk view]
    Planner[replay planner]
    DrawRun[draw-run builder]
    Encoders[draw/blit/present encoders]
    Transient[transient upload slabs]
  end

  subgraph GPUDriver["GPU/driver-bound Metal side"]
    Pass[render pass encoders]
    CBChain[sub-command-buffer chain]
    Drawable[CAMetalLayer drawable]
    Present[presentDrawable]
  end

  subgraph SyncWait["Sync-bound progress and lifetime"]
    Completion[completion callbacks]
    CompletedSeq[completedSeqId]
    PresentSeq[presentCompletedSeqId]
    Token[frame-latency token]
    Reclaim[resource/transient reclaim]
  end

  App --> State --> Payloads --> ChunkRing
  Handles --> ChunkRing
  ChunkRing --> Import --> Planner --> DrawRun --> Encoders
  Encoders --> Transient
  Encoders --> Pass --> CBChain
  Encoders --> Drawable --> Present --> CBChain
  CBChain --> Completion --> CompletedSeq --> Reclaim
  Completion --> PresentSeq --> Token

  classDef cpu fill:#eaf4ff,stroke:#2f6fad,color:#0b2239
  classDef gpu fill:#fff0d6,stroke:#b26b00,color:#2b1900
  classDef sync fill:#ffe8e8,stroke:#b64242,color:#2b0d0d
  class App,State,Payloads,Handles,ChunkRing,Import,Planner,DrawRun,Encoders,Transient cpu
  class Pass,CBChain,Drawable,Present gpu
  class Completion,CompletedSeq,PresentSeq,Token,Reclaim sync
```

## General Bottleneck Map

```mermaid
flowchart TD
  Start[dxmt9 frame or chunk] --> CPUFront[CPU front-end]
  Start --> Encode[encode thread]
  Start --> GPU[Metal/GPU]
  Start --> Sync[sync and pacing]

  CPUFront --> BridgeCost[PE/unix bridge call boundary]
  CPUFront --> CommitReplay[commit_chunk import/replay/submit]
  CPUFront --> RecordCost[packet construction and resource retention]
  CPUFront --> LockCost[buffer lock/map/shadow copy]

  Encode --> RunCost[draw-run formation failures]
  Encode --> StateCost[per-draw state rebuild]
  Encode --> UploadCost[transient upload pressure]
  Encode --> BindCost[stream/IB/texture/sampler bind churn]
  Encode --> PSOCost[PSO lookup and cold compile]

  GPU --> PassCost[render-pass split and tile preservation]
  GPU --> ShaderCost[shader/register/texture cost]
  GPU --> ResidencyCost[heap/resource residency]

  Sync --> PresentCost[drawable and present pacing]
  Sync --> CompletionCost[command-buffer completion wait]
  Sync --> QueryCost[query/readback/hazard flush waits]
  Sync --> RingCost[chunk ring pressure]

  RunCost --> RootA[Candidate A: record shape prevents batching]
  UploadCost --> RootB[Candidate B: payload frequency too high]
  PassCost --> RootC[Candidate C: pass split policy too eager]
  PresentCost --> RootD[Candidate D: present boundary hides real bottleneck]
  LockCost --> RootE[Candidate E: 32-bit pointer compatibility copy path]

  classDef hot fill:#ffe8e8,stroke:#b64242,color:#2b0d0d
  classDef cpu fill:#eaf4ff,stroke:#2f6fad,color:#0b2239
  classDef gpu fill:#fff0d6,stroke:#b26b00,color:#2b1900
  classDef sync fill:#f3e8ff,stroke:#6b42b6,color:#1d0d2b
  class RootA,RootB,RootC,RootD,RootE hot
  class CPUFront,BridgeCost,CommitReplay,RecordCost,LockCost,Encode,RunCost,StateCost,UploadCost,BindCost,PSOCost cpu
  class GPU,PassCost,ShaderCost,ResidencyCost gpu
  class Sync,PresentCost,CompletionCost,QueryCost,RingCost sync
```

## Primary Bottleneck Classes

| Class | Symptom counters | Typical root cause | Preferred fix direction |
|---|---|---|---|
| Commit chunk replay | `bridge_commit_latency_ns`, `commit_chunk_import_cpu_ms`, `commit_chunk_handle_cpu_ms`, `commit_chunk_replay_cpu_ms`, `commit_chunk_queue_draw_submission_cpu_ms`, `commit_chunk_draw_batch_submit_cpu_ms`, draw-run child timers, `submit_draw_run_*_cpu_ms`, `submit_draw_run_batch_*_cpu_ms` | The synchronous `commit_chunk` call is spending time in unix-side record replay, draw-run scanning, snapshot/draw submission construction, queued submission flushing, or queue slot append/resource-mark/chunk-commit work. A large historical `bridge_commit_latency_ns` value is not necessarily raw PE/unix bridge overhead. | Split replay and submit children first; optimize snapshot/draw submission, record dispatch, draw-run scan, constant-upload pass-through, submission batch construction, resource marking, and chunk append/publish cost before changing the ABI. |
| Snapshot cache invalidation | `d3d9_snapshot_cache_lookup_cpu_ms`, `d3d9_snapshot_cache_miss_cpu_ms`, `d3d9_snapshot_cache_uniform_refresh_cpu_ms`, `d3d9_snapshot_uniform_build_calls`, `draw_uniform_payload_appends`, `draw_packet_declared_nonbinding`, `draw_packet_actual_nonbinding`, `draw_packet_redundant_nonbinding`, `draw_packet_redundant_uniform` | D3D9 snapshot submission rebuilds shader layout, hot state, or uniform payload too often. Current GT1 proof rejects broad same-value non-binding deltas (`redundant_nonbinding=0`), and the accepted cache-hit uniform refresh fast path proves a large part is real component payload construction/hash rather than redundant invalidation. | Keep cache-hit shader-constant refresh on the fast path. Remaining work is miss hot-build, VS indexed constant fallback, and stronger proof before revisiting invalidation policy. |
| Per-draw CPU encode | `encode_draw_cpu_ms`, `bind_*`, `pipeline_lookup`, `fvf_decode` | Draws are replayed one by one even when state is stable. | Improve draw-run formation, cache decoded state, skip redundant binds. |
| Payload/upload pressure | `transient_upload_calls`, `transient_upload_bytes`, `uniform_*`, payload arena counters | Stable constants or state are uploaded at draw frequency. | Split stable/volatile payloads, coalesce slab reservations, skip duplicate payload copies. |
| Render-pass churn | `render_pass_begin`, split reason counters, tile preservation bytes, store/load action counters | RT/depth/clear/hazard decisions force too many pass boundaries. | Exact hazard tracking, better clear/load/store proof, coalesce same RT/depth passes when legal. |
| Present pacing | `present_acquire_wait_ms`, `present_boundary_wait_ms`, `completion_present_wait_ms` | Drawable availability or frame-latency token dominates wall time. | Keep pacing counters separate; tune latency only after encode/GPU attribution is clear. |
| Command-buffer grain | `command_buffers`, `sub_command_buffers`, `chunk_subcb_count_max`, `gpu_command_buffer_time_ms` | 1 CB can serialize GPU behind encode; too many CBs add tile/commit cost. | Current default: per-render-pass split with cap=4; validate by workload. |
| Lock/map compatibility | `map_buffer_total_ms`, `map_buffer_wait_ms`, `d3d9_buffer_lock_*` | 32-bit app needs low4GB CPU-visible pointer; native storage may not be 32-bit safe. | Reuse low4GB CPU shadows; free on resource destroy, not every lock. |
| Cold PSO/archive miss | `pipeline_build_*`, `cold_compile_count_after_warm`, archive counters | Shader/state variants are not prewarmed or are over-specialized. | Better archive prewarm, reduce PSO key churn, specialize only when win is proven. |

## Current 3DMark05 GT1 Calibration

The 3DMark05 GT1 investigation is the current high-signal calibration case for
this general model. The root map is [[overview-3dmark05-gt1]]. Its latest gate
state separates GPU frame-time, CPU encode cost, and wallclock present pacing:

| General class | 3DMark05 GT1 status | Decision |
|---|---|---|
| GPU hidden backend storage | Dominant GPU frame limiter. Xcode VS-buffer write tracks VS invocations, not dxmt CPU writers, visible `VSOut`, or fragment volume. | Reduce VS invocations when semantics allow; do not chase visible varying width as the first-order owner. [[hidden-backend-storage]] |
| Opaque-depth index locality | Accepted production-shaped GPU win: historical target `50/0+50/1` GPU `-18.39%`, VS invocations `-14.12%`, VS write `-16.79%`; refreshed frame60 target `60/0+60/1` GPU `-10.64%`, VS invocations `-14.12%`, VS write `-16.77%`. | Keep as opt-in locality path. The current opaque proof reattaches the movement to refreshed rows, but this is still not a shared `perf` default until index-setup CPU cost is lower or a broader runtime gate proves net positive. [[index-cache-locality-opaque.08]], [[index-cache-locality-proofinput.01]], [[index-cache-locality]] |
| Screen-blend index locality | Strong measured movement, but destination-dependent; current gate is missing movement/semantic image inputs. | Keep as historical exact/`lsb1` proof artifact until the proof is reattached or regenerated; do not generalize to broad depth-read reorder. [[index-cache-locality-screenblend.05]], [[index-cache-locality-proofinput.01]], [[index-cache-locality-screenblend.04]] |
| Broad depth-read reorder | Blocked by final-color correctness. | Needs a real final-color/final-writer oracle before another Xcode budget; the current D3D9 occlusion path is primitive-count only. [[mini-replay-bisection-semantic.01]], [[mini-replay-bisection-texture.08]] |
| Non-reorder backend-shape | Current candidates rejected or unproven. | Half-VSOut and scoped `live-vsout` stayed flat in Xcode; the refreshed gate closes stale shader-output smokes. Future candidates need a new below-visible backend mechanism or bytes/inv preflight. [[hidden-backend-storage-shape.13]] |
| Present pacing | Wallclock limiter, separate from GPU frame limiter. | Historical `DXMT9_DISABLE_VSYNC=1` remains an opt-in for sync-paced workloads, but current GT1 direct is already immediate; use `present_schedule_*` counters before treating vsync-off as a lever. [[present-pacing]] |
| Per-draw CPU encode | Orthogonal to GPU limiter, still important for wallclock. Commit_chunk stage counters now show large historical bridge latency can be replay-owned rather than ABI-owned. | Focus on draw-run break reduction, commit_chunk replay internals, snapshot rebuild, and measured bind/state churn rather than assuming bind-cache hit rates or raw bridge overhead. [[state-churn-encode]] |

```mermaid
flowchart TD
  Workload["3DMark05 GT1"] --> GPU["GPU frame time"]
  Workload --> Wall["process wallclock / fps"]
  Workload --> CPU["CPU encode"]

  GPU --> Hidden["hidden vertex/tiler/backend storage\nACCEPTED owner"]
  Hidden --> Locality["post-transform locality\naccepted lever"]
  Locality --> Opaque["opaque-depth opt-in\nnot default yet"]
  Locality --> Screen["screen-blend historical exact/lsb1\ncurrent proof reattach needed"]
  Locality --> Broad["broad depth-read rejected\nfinal-color blocker"]
  Hidden --> Backend["non-reorder backend-shape\nneeds new mechanism"]

  Wall --> Present["present completion pacing\ncurrent GT1 already immediate"]
  CPU --> Encode["draw-run/commit_chunk replay/snapshot/state churn\northogonal but open"]

  classDef good fill:#e8f5e8,stroke:#4d8b4d,color:#102a10
  classDef warn fill:#fff3d6,stroke:#b98222,color:#2a1b00
  classDef bad fill:#ffe8e8,stroke:#b64242,color:#2b0d0d
  class Hidden,Locality,Opaque,Present good
  class Workload,GPU,Wall,CPU,Screen,Backend,Encode warn
  class Broad bad
```

## Ideal Design

The ideal dxmt9 design is not "minimum command buffers" or "maximum batching"
in isolation. It is a bounded pipeline where each boundary carries enough work
to amortize overhead but still exposes enough progress for the GPU and
presenter to run ahead.

```mermaid
flowchart LR
  App[D3D9 app] --> Recorder[record compact packets]
  Recorder --> Import[validate/import once]
  Import --> Planner[build replay plan]
  Planner --> Runs[coalesce draw runs]
  Runs --> Stable[bind stable state once]
  Stable --> Volatile[push small volatile payloads]
  Volatile --> Passes[coalesce legal render passes]
  Passes --> CBs[commit bounded sub-CB chain]
  CBs --> Present[present on final CB]
  CBs --> Reclaim[reclaim on final chunk completion]

  Recorder -. avoid .-> Bad1[large mutable state blobs per draw]
  Runs -. avoid .-> Bad2[const uploads breaking every run]
  Passes -. avoid .-> Bad3[false hazard pass splits]
  CBs -. avoid .-> Bad4[unbounded sub-CB chains]
  Present -. avoid .-> Bad5[present waits hiding encode/GPU attribution]

  classDef good fill:#e8ffe8,stroke:#3c8f3c,color:#0d2b0d
  classDef bad fill:#ffe8e8,stroke:#b64242,color:#2b0d0d
  class Recorder,Import,Planner,Runs,Stable,Volatile,Passes,CBs,Present,Reclaim good
  class Bad1,Bad2,Bad3,Bad4,Bad5 bad
```

### Design Principles

- Keep chunk `seqId` as the lifetime and synchronization unit.
- Allow a chunk to emit 1..N Metal command buffers, but complete/reclaim only
  on the final command buffer.
- Attach present metadata to the final command buffer only.
- Keep mid-chunk split decisions deterministic and record-driven.
- Prefer exact hazard and dependency proofs over probabilistic or broad flushes.
- Move stable data out of the per-draw hot path; keep per-draw payloads small.
- Treat present pacing as a separate axis from encode cost and GPU execution.
- Use env knobs for A/B experiments, but keep production defaults conservative
  and backed by counters.

## Solution Trade-Offs

| Direction | Pros | Cons / risks | Validation needed |
|---|---|---|---|
| Draw-run coalescing across benign records | Reduces per-draw encode, bind, and lookup overhead. | Incorrect if constants/state/resources change visibility between draws. | Record-sequence audit, state/payload hash stability, image tests. |
| Stable/volatile payload split | Shrinks upload bytes and CPU writes. | Requires shader layout support and PSO compatibility discipline. | Shader source tests, layout ABI tests, perf counters for byte reduction. |
| Redundant bind suppression | Low-risk CPU win when encoder state is stable. | Encoder-side cache can become stale after pass/PSO/resource transitions. | Recorder tests for command order, Metal validation, per-bind counters. |
| Per-render-pass sub-CB chain cap | Lets GPU start before chunk tail is fully encoded. | Extra command buffers can increase tile store/load and commit overhead. | `sub_command_buffers`, `chunk_subcb_count_max`, GPU time, tile counters. |
| Render-pass coalescing | Reduces tile preservation and pass setup. | Hard correctness boundary around clears, resolves, hazards, and depth reuse. | Split-reason counters, exact hazard tests, pixel/golden validation. |
| Aggressive store-action proof | Saves tile stores when attachments are dead. | Wrong proof causes missing depth/color data in later passes. | Store/load counters, targeted depth/RT reuse tests, frame capture. |
| Low4GB shadow reuse | Avoids repeated VM allocation for 32-bit locks. | Shadow capacity and dirty/readback rules must stay correct. | Lock/unlock tests, map wait counters, app compatibility runs. |
| Archive/prewarm expansion | Reduces runtime PSO compile stalls. | Archive growth and stale variants can mask key explosion. | Cold/warm run comparison, archive hit rate, compile count after warm. |

## Current Default Policy

```mermaid
stateDiagram-v2
  [*] --> PerRenderPass
  PerRenderPass --> SplitAllowed: render-pass boundary
  SplitAllowed --> Split: subCB count < cap
  SplitAllowed --> Continue: subCB count >= cap
  Split --> PerRenderPass: continue encoding chunk
  Continue --> FinalCommit: chunk tail
  PerRenderPass --> FinalCommit: no more split points
  FinalCommit --> Completed: final CB completion

  note right of PerRenderPass
    Production default:
    DXMT9_MID_CHUNK_COMMIT_POLICY=per-render-pass
    DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS=4
  end note

  note right of FinalCommit
    Present, completion signaling,
    and resource reclaim attach to
    the chunk tail only.
  end note
```

This default is a compromise:

- Better than `off` when long encode tails would otherwise delay GPU start.
- Safer than unbounded per-pass splitting because the cap bounds tile
  preservation and command-buffer commit overhead.
- Still workload-dependent. Heavy render targets, MSAA, many attachments, or
  high store/load pressure can erase the pipelining win.

## Experiment And Validation Areas

```mermaid
flowchart TD
  Baseline[Baseline perf run] --> Attribute[Attribute wall time]
  Attribute --> CPU[CPU encode/front-end]
  Attribute --> GPU[GPU/render-pass/shader]
  Attribute --> Sync[present/completion/sync]

  CPU --> C1[draw-run break taxonomy]
  CPU --> C2[payload bytes and hash stability]
  CPU --> C3[bind/lookup/decode sub-counters]

  GPU --> G1[render-pass split reason matrix]
  GPU --> G2[tile preservation and store/load actions]
  GPU --> G3[shader/pass ranking by Metal capture]

  Sync --> S1[present acquire vs boundary vs completion]
  Sync --> S2[query/readback wait isolation]
  Sync --> S3[sub-CB cap A/B matrix]

  C1 --> Change[Candidate change]
  C2 --> Change
  C3 --> Change
  G1 --> Change
  G2 --> Change
  G3 --> Change
  S1 --> Change
  S2 --> Change
  S3 --> Change

  Change --> Verify[Correctness + perf validation]
  Verify --> Counters[Counter deltas]
  Verify --> Images[image/golden checks]
  Verify --> Trace[Metal trace when GPU-bound]

  classDef measure fill:#eaf4ff,stroke:#2f6fad,color:#0b2239
  classDef action fill:#e8ffe8,stroke:#3c8f3c,color:#0d2b0d
  classDef verify fill:#fff0d6,stroke:#b26b00,color:#2b1900
  class Baseline,Attribute,CPU,GPU,Sync,C1,C2,C3,G1,G2,G3,S1,S2,S3 measure
  class Change action
  class Verify,Counters,Images,Trace verify
```

### Measurement Coverage Checklist

Before changing a performance policy, the relevant path should be measurable
with counters that distinguish work from waits:

- Render-pass begin/end and split reason counters distinguish normal pass reuse
  from forced splits.
- Pipeline cache hit/miss/build counters expose state churn and shader variant
  pressure.
- Draw call, indexed draw, primitive, and vertex counters quantify replay
  volume.
- Bind churn counters track vertex buffers, index buffers, textures, samplers,
  depth state, viewport, scissor, rasterizer, and pipeline binds.
- Upload counters track transient call count, bytes, CPU time, and uniform
  frequency classes.
- Hazard counters distinguish exact overlap from legacy broad/probabilistic
  overlap decisions.
- Present source counters identify selected source validity, source size,
  destination size, handle, texture, format, and sample count.
- Completion buckets separate draw, present, compatibility, shader bucket,
  completion-watcher dequeue age/status, and command-buffer execution time
  where available.
- Frame/present pacing counters separate drawable acquire, present boundary,
  frame-token wait, query/readback wait, and chunk ring pressure.

```mermaid
flowchart LR
  DrawRun[draw/run records] --> Encoder[draw encoder]
  Encoder --> Pass[render pass begin/end]
  Encoder --> Pipeline[pipeline cache lookup]
  Encoder --> Bind[Metal bind calls]
  Encoder --> Issue[Metal draw issue]
  Encoder --> Upload[transient/uniform upload]

  Pass --> Split[split reason counters]
  Pipeline --> HitMiss[pipeline hit/miss/build]
  Bind --> BindCounters[bind churn counters]
  Issue --> Volume[draw/primitive/vertex counters]
  Issue --> ShaderBucket[VS/PS/variant bucket]
  Upload --> UploadCounters[call/byte/time counters]

  ShaderBucket --> QueueDiag[chunk diagnostics]
  QueueDiag --> Completion[completion wait buckets]
  Completion --> Pacing[present/query/ring pacing buckets]
```

### Present Path Ownership

The target present path is not "remove frame latency". It is to avoid using
encode-thread progress as the pacing primitive and to keep ownership clear.

```mermaid
flowchart TD
  App[D3D9 Present] --> Device[DeviceImpl public D3D9 policy]
  Device --> Queue[CommandQueue present packet]
  Queue --> Flush[commit draw/present chunk]
  Queue -. optional token .-> Acquire[Presenter drawable acquisition]
  Acquire -. drawable token .-> PresentPacket[present encode packet]
  Flush --> PresentPacket
  PresentPacket --> MTLPresent[commandBuffer presentDrawable]
  MTLPresent --> Completion[completion path]
  Completion --> FrameFence[present-completed frame token]
  Device --> LatencyWait[wait frameId - effectiveLatency]
  FrameFence --> LatencyWait

  subgraph Ownership["ownership"]
    PresenterOwn[Presenter: CAMetalLayer and drawable tokens]
    QueueOwn[CommandQueue: ordering, packet execution, completion, frame token]
    DeviceOwn[DeviceImpl: D3D9 latency value and callbacks]
    SwapOwn[SwapChain: presenter and backbuffer metadata]
  end
```

Ownership rules:

- `Presenter` owns CAMetalLayer properties, drawable acquisition state, and
  any future drawable-token mechanism.
- `CommandQueue` owns chunk ordering, present packet execution, completion
  signaling, frame-latency token advancement, and present-boundary waits.
- `DeviceImpl` supplies public D3D9 latency values and presentation callbacks;
  it should not own acquire or boundary mechanics.
- `SwapChain` owns the `Presenter` and passes per-present source/backbuffer
  metadata.

### Reference Backend Shapes

DXVK and Wine are useful references for ownership boundaries, not direct
templates to copy.

```mermaid
flowchart TD
  subgraph DXVK["DXVK D3D9 reference shape"]
    DXVKPresent[D3D9 Present] --> DXVKFlush[flush command list]
    DXVKFlush --> DXVKAcquire[acquire WSI image]
    DXVKAcquire --> DXVKSubmit[submit present work]
    DXVKSubmit --> DXVKFence[frame-latency fence]
    DXVKFence --> DXVKWait[wait frameId - actualLatency]
  end

  subgraph Wine["Wine D3D9 reference shape"]
    WinePresent[d3d9 swapchain Present] --> Wined3D[wined3d swapchain present]
    Wined3D --> CS[command stream packet]
    CS --> Backend[backend present]
    Backend --> Platform[platform swap/flush]
  end

  subgraph DXMT9["dxmt9 target interpretation"]
    Q[CommandQueue owns ordering] --> P[Presenter owns drawable]
    P --> T[present token]
    T --> F[present-completed frame fence]
  end

  DXVKFence -. design reference .-> F
  CS -. command-stream reference .-> Q
```

Lessons to preserve:

- Frame-latency waits should be explicit fence/token rules, not accidental
  waits on encode-thread progress.
- Present acquisition, present packet execution, and latency waiting are
  separate concerns.
- The app-facing D3D9 layer should not synchronously wait for all GPU work on
  every immediate present unless the API contract requires it.

### Negative Findings To Preserve

These prior findings should stay visible because they prevent repeating
attractive but weak fixes:

- Direct indexed draws are the production path. Expanding indexed draws into a
  transient non-indexed stream is a diagnostic/compatibility lane, not a default
  performance fix.
- Broad hazard filters are useful as diagnostics, but render-pass split
  decisions should be exact-handle driven where possible; false positives create
  avoidable encoder churn.
- Chunk splitting is a tail-latency and GPU-overlap tool, not a universal
  throughput fix. Smaller chunks can reduce sequence tails while increasing
  completion, ring, and command-buffer overhead.
- Present async/preacquire paths should remain opt-in until draw, pass, hazard,
  and present-source counters show that present pacing is the actual limiter.
- Hoisting or caching uniform construction is not automatically a win if the
  remaining hot cost is writing large shared-memory payloads or adding
  indirection to a hot loop.
- Process-level FPS is useful for end-to-end comparison, but it includes startup,
  window creation, device/resource creation, teardown, and final flush. It must
  not be used alone for renderer attribution.

### Required Measurement Discipline

- Report throughput and wall time, but do not use process-level FPS alone for
  attribution.
- Separate CPU encode time, GPU command-buffer time, present/drawable wait,
  completion wait, and query/readback waits.
- Compare medians and p95/p99 where possible; single runs are useful for
  direction but not for default-policy decisions.
- Keep A/B knobs scoped and explicit:
  - `DXMT9_MID_CHUNK_COMMIT_POLICY`
  - `DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS`
  - draw-run or payload experimental toggles
  - present/frame-latency toggles
- Treat Metal frame captures as the decisive tool when counters show GPU time
  but cannot attribute it to pass, shader, texture, or tile behavior.

## Recommended Investigation Order

1. Attribute wall time into CPU encode, GPU execution, and sync/present waits.
2. If CPU encode dominates, inspect draw-run breaks, payload upload frequency,
   bind churn, FVF/declaration decode, and PSO lookup.
3. If GPU execution dominates, rank render passes, tile preservation, store/load
   actions, shader families, texture sampling, and overdraw.
4. If sync dominates, split present acquire, present boundary, completion
   dequeue age/status, query/readback, and ring pressure before changing
   latency policy.
5. Only after attribution is stable, choose a design change and validate with
   counters plus image/golden correctness checks.

## Open Areas

- Draw-run planner support for benign interleaved records such as stable
  constant uploads.
- Payload-liveness model that proves when a constant/upload record can be
  hoisted, merged, or skipped.
- Render-pass coalescing proof for same RT/depth sequences with clears and
  depth reuse.
- Better pass/shader GPU attribution from Xcode `.gputrace` captures and GPU
  counters.
- Workload-specific sub-CB cap validation across render-target sizes, MSAA,
  attachment counts, and present rates.
- Cold/warm PSO archive quality, including variant explosion from shader
  liveness and compatibility flags.
