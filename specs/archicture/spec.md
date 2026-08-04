---
type: "Spec"
title: "Architecture Spec"
description: "Archicture spec, ownership, ordering, and evidence mapping."
tags: [specs, archicture, spec]
---

# Architecture Spec

This document describes the project-wide architecture that satisfies
[`requirements.md`](requirements.md). Subsystem designs remain authoritative for
local details; this file owns the whole-system shape, data boundaries, and
CPU/GPU sequencing, render flow, concurrency, and bottleneck analysis.

---

## 1. Ownership Map

```mermaid
flowchart TD
    subgraph API["Wine-visible API layers"]
        D7["d3d7 / ddraw shim"]
        D8["d3d8 shim"]
        D9["d3d9 PE frontend\nCOM ABI + DeviceState"]
    end

    subgraph PE["PE side"]
        VALID["Windows D3D validation\nHRESULTs, caps, state blocks"]
        REC["Command recorder\nPOD records + retained handles"]
        WIRE["Wire chunk builder\nheader + record table + handle table + payload arena"]
    end

    subgraph Bridge["winemetal bridge"]
        ABI["C ABI / unix-call dispatch\nmarshal only"]
    end

    subgraph Unix["unix provider"]
        IMPORT["Importer\nvalidate + canonicalize + retain"]
        QUEUE["CommandQueue\nchunk ring + seq IDs"]
        ENCODE["Encode thread\nFlatDrawStateView + DrawParam spans"]
        FINISH["Finish thread\ncompletion + reclaim"]
        PRESENT["Presenter\nCAMetalLayer + drawable tokens"]
    end

    subgraph Metal["Metal / GPU"]
        PSO["Pipeline/depth/cache lookup"]
        CB["MTLCommandBuffer"]
        GPU["GPU execution"]
    end

    D7 --> D9
    D8 --> D9
    D9 --> VALID --> REC --> WIRE --> ABI --> IMPORT --> QUEUE
    QUEUE --> ENCODE --> PSO --> CB --> GPU --> FINISH
    ENCODE --> PRESENT --> CB
```

Ownership is intentionally DXMT-shaped: API calls update state and record work;
Metal execution happens later on queue-owned threads. dxmt9 differs from
upstream DXMT where Wine requires a C ABI and POD wire records instead of
in-process C++ command objects.

---

## 2. Data Layout Strategy

The hot path follows a flat-record pipeline:

```mermaid
flowchart LR
    DS["PE DeviceState\nfixed tables + dirty/hash"] --> SNAP["CanonicalDrawState\ntransient value"]
    SNAP --> HOT["FlatDrawStateRecord\nhot resource/PSO key"]
    SNAP --> LAYOUT["DrawShaderLayoutContext\nshader/layout sidecar"]
    SNAP --> DEBUG["DrawDebugSnapshot\ndebug sidecar"]

    DP["DrawParam span\ntrivially copyable"] --> SLOT
    UP["DrawParamPayloadView span\nborrowed UP bytes"] --> ARENA

    subgraph SLOT["ChunkSlot SoA / AoSoA"]
        HDR["MetalCommandHeader[]"]
        HOTS["drawHotStates[]"]
        LAYOUTS["drawShaderLayouts[]"]
        DEBUGS["drawDebugSnapshots[]"]
        UNIF["drawUniformPayloads[]\n+ hash chains"]
        PARAMS["drawParams[]"]
        ARENA["drawPayloadArena bytes"]
        DRAWRUN["DrawRunCommandRecord[]"]
        OTHER["Clear/Copy/Present record arrays"]
    end

    SLOT --> VIEW["MetalCommandView\nFlatDrawStateView + spans"]
    VIEW --> ENC["Encoder decisions\npipeline, depth, resources, pass merge"]
```

The design uses SoA for queue-owned execution storage and AoSoA where a command
header indexes one type-specific record array. This keeps replay linear while
avoiding a single fat command union that carries every possible payload.

### 2.1 Boundary Forms

| Boundary | Form | Ownership rule |
|---|---|---|
| API layer to D3D9 frontend | COM calls + PE `DeviceState` | PE owns Windows-visible state and validation |
| D3D9 frontend to backend facade | `CanonicalDrawState`, `DrawUniformPayload`, `span<DrawParam>`, `span<DrawParamPayloadView>` | Borrowed spans are immediate-use only |
| CommandQueue to ChunkSlot | SoA arrays and byte arena | Queue owns copied records after append |
| ChunkSlot to encoder | `MetalCommandView`, `FlatDrawStateView`, `span<DrawParam>`, payload span | Borrowed view over queue-owned slot storage |
| PE bridge | POD wire blob | Header/table/arena offsets validated before import |
| Importer to queue | Compact imported records and handles | unix side owns retained handles and replay data |

### 2.2 Copy Policy

This is the design companion to the normative Minimal-Copy Policy
(`requirements.md` §7, R-ARCH-7.1–7.6). The floor is: each unique
encoder-visible record or payload reaches owned storage at most once, and only
when the encoder will read it.

Allowed copies (R-ARCH-7.1):

- borrowed span to queue-owned storage;
- user-provided UP memory into payload/staging arenas;
- PE wire sections into a contiguous C ABI blob;
- decoded/canonicalized records into queue-local storage;
- uniform payload intern append on cache miss.

Copies that should be treated as regressions:

- one heap allocation per ordinary draw after warm-up (R-ARCH-7.1);
- one bridge call per `Set*` or draw;
- copying PE COM state into backend execution records;
- storing process-local pointers in wire payloads;
- rebuilding large shader/layout payloads when hashes or handles are enough
  (R-ARCH-7.3);
- materializing per-draw state records that draw-run batching then discards —
  when a generation/lane stamp proves a shared canonical state, only the
  surviving record is materialized and per-draw deltas ride `DrawParam` +
  binding-override/snapshot payloads (R-ARCH-7.2);
- building a record in an intermediate carrier and copying it into the owned
  arena/SoA slot when the destination was already known — the target design is
  to construct directly into the owned slot instead (R-ARCH-7.4).

Construct-in-place and unified memory (R-ARCH-7.4, R-ARCH-7.5):

- Target: the producer builds the surviving canonical draw state and the
  per-draw uniform/constant bytes directly into their owned destinations — the
  ChunkSlot SoA slot and the shared-storage transient/argbuf allocation —
  rather than into a `DrawRunSubmission`-style carrier that is then copied in.
- Current status: dirty constant bytes are built directly into shared-storage
  transient/argbuf memory. Draw-run state now avoids materializing same-stamp
  N-1 non-front records in the default binding-agnostic snapshot path, and the
  queue accepts elided continuations without reducing normal compatibility
  grouping. The surviving front/materialized state still travels through
  `DrawRunSubmission` before `ChunkSlot::appendDrawRunBatch` stores it, so
  direct construction into the queue-owned slot remains open. Adjacent uniform
  payload elision is measured and rejected for 3DMark05 GT1:
  `d3d9_snapshot_uniform_elided=0`, so uniform snapshot reuse is not a live GT1
  target.
- A shared (`MTLStorageModeShared`) backing is the GPU-read allocation itself, so
  on Apple Silicon the in-place build of dirty constants is the upload; no
  separate CPU-struct-then-staging copy is required (R-BACK-5.7). The allocation
  is retained by queue sequence ID until GPU completion (R-ARCH-6.4); a reference
  handle does not extend its lifetime by itself.

The copy-class map marks each materialization point on the target warm draw path.
Green is a mandatory floor copy or construct-in-place materialization
(R-ARCH-7.1 / R-ARCH-7.5); blue is a move, reference, or direct bind that copies
no payload bytes. The diagram is the target warm draw path: the surviving front
draw state is constructed directly into the queue-owned slot (R-ARCH-7.2 /
R-ARCH-7.4), so no per-draw carrier copy appears. The current implementation is
short of that target for draw state; `gap.md` tracks the default same-stamp
state elision and the remaining direct-construct work.

```mermaid
flowchart TD
  DS["PE DeviceState"] --> Cache["producer cache<br/>built once per generation"]
  Cache --> C8{{"construct front state in place<br/>one surviving record"}}
  C8 --> SoA["ChunkSlot SoA<br/>queue-owned"]
  SoA --> C4{{"uniform intern<br/>on cache miss"}}
  SoA --> View["FlatDrawStateView<br/>reference, no copy"]
  View --> Enc["encode thread"]
  Enc --> C5{{"build dirty constants in place<br/>into Shared MTLBuffer"}}
  C5 --> GPU["GPU read"]
  PEw["PE chunk bytes"] --> C6{{"wire blob copy<br/>cross-boundary"}}
  C6 --> Imp["unix import, owned"]
  AppVB["app DEFAULT VB/IB"] --> Bind["bound directly<br/>NO copy"]
  Bind --> Enc
  UP["DrawPrimitiveUP memory"] --> C7{{"stage user memory"}}
  C7 --> SoA

  classDef floor fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef free fill:#e8eefc,stroke:#3559a8,color:#0b2239
  class C4,C5,C6,C7,C8 floor
  class View,Bind free
```

---

## 3. CPU-Bound Submission Sequence

The CPU-bound path should be dominated by validation, flat state/key creation,
record append, and one chunk commit. It should not block on Metal API work.

```mermaid
sequenceDiagram
    participant App as App / Wine thread
    participant D9 as d3d9 PE frontend
    participant State as DeviceState cache
    participant Rec as Recorder / direct submit
    participant Bridge as winemetal bridge
    participant Import as unix importer
    participant CQ as CommandQueue
    participant Slot as ChunkSlot

    App->>D9: Set* / Draw* / Clear / Present
    D9->>State: validate + mutate PE state
    D9->>State: build or reuse flat hashes
    D9->>Rec: DrawParam span + payload spans
    alt native runtime path
        Rec->>CQ: submitDrawRun(CanonicalDrawState, uniforms, spans)
        CQ->>Slot: copy to SoA arrays + payload arena
        Slot-->>CQ: DrawRunCommandRecord index
    else PE/unix bridge path
        Rec->>Rec: buildSparseStateV2 -> SparseStateV2Input
        Rec->>Rec: appendRecordV2 (seal + flush every N records)
        Rec->>Bridge: commitChunk(wire blob)
        Bridge->>Import: one unix-call with POD blob
        Import->>Import: validate header/table/ranges/handles
        Import->>Import: copy blob, retain wrappers, bulk-mark resources
        Import->>Worker: push raw chunk (bounded FIFO)
        Worker-->>CQ: replay records, publish slot (deferred)
        CQ->>Slot: append imported flat records
    end
    CQ-->>App: return after queue ownership
```

The bridge branch is two-phase since the commit-replay offload became the engine
default (`R-BACK-2.51`): validation, import, wrapper retention and bulk resource
marking stay synchronous on the app thread, and only record replay and slot
publish are deferred to the worker. `Rec` builds `SparseStateV2Input` directly —
there is no intermediate record format between the PE state shadow and the wire.

CPU-bound design rules:

- `Set*` updates PE state and invalidates derived flat-state caches; it does not
  call Metal.
- Ordinary draws submit `DrawParam` spans and payload spans. Queue append owns
  the copied data before returning.
- `ChunkSlot` arrays reserve capacity and retain capacity after `clear()`, so
  warm frames should not allocate per ordinary draw.
- Importer coalescing may use temporary `DrawParam` vectors. That cost is
  acceptable unless bridge-heavy benchmarks show it as CPU-bound.

---

## 4. GPU-Bound Execution Sequence

GPU-bound work starts after queue ownership. The encoder consumes views over
queue-owned data and can batch render passes when hazards allow.

```mermaid
sequenceDiagram
    participant CQ as CommandQueue
    participant Enc as Encode thread
    participant Slot as ChunkSlot
    participant Pool as ResourcePool
    participant Cache as Pipeline / depth / shader caches
    participant MTL as Metal command buffer
    participant GPU as GPU
    participant Fin as Finish thread
    participant Pres as Presenter

    CQ->>Enc: pending slot available
    Enc->>Slot: iterate MetalCommandHeader[]
    Slot-->>Enc: MetalCommandView
    Enc->>Pool: resolve handles + hazard metadata
    Enc->>Cache: lookup pipeline/depth/shader by flat keys
    alt compatible draw sequence
        Enc->>MTL: keep render encoder open
    else RT change or hazard
        Enc->>MTL: end encoder / start new encoder
    end
    Enc->>MTL: encode draw/copy/clear/present work
    Enc->>Pres: acquire/present drawable when present command exists
    Enc->>MTL: commit
    MTL->>GPU: execute
    GPU-->>Fin: completion
    Fin->>CQ: signal seq ID / frame token / reclaim slot
```

GPU-bound design rules:

- Pass merge/split decisions consume `FlatDrawStateView`, attachment keys, and
  exact hazard read/write handle sets, not PE state or Bloom false positives.
- Pipeline and depth decisions consume compact flat keys and shader/layout
  context.
- Uniform payloads are interned in slot-local storage and referenced by handles
  in draw-run records.
- Present metadata travels with the queued command and is completed through the
  queue sequence/frame-token path.

---

## 5. Render Flow

The render flow is the whole frame path, not just `Draw*`. It includes D3D9 state
mutation, draw/clear/copy/readback packet formation, queue ownership, Metal
encoding, GPU completion, and presentation.

```mermaid
flowchart TD
    APP["Application D3D9 calls"] --> API{"Command class"}

    API -->|"Set* / shader / stream / texture"| STATE["PE DeviceState\nfixed arrays + dirty/hash"]
    API -->|"Draw* / Draw*UP"| DRAWPREP["Build CanonicalDrawState\nDrawParam span + payload views"]
    API -->|"Clear"| CLEARPREP["Build ClearDesc"]
    API -->|"Update/Stretch/ColorFill"| COPYPREP["Build surface op record"]
    API -->|"GetRenderTargetData"| READPREP["Build readback request\nsynchronous boundary"]
    API -->|"Present"| PRESPREP["Build SwapDesc\npresent source handle"]

    STATE --> DRAWPREP
    DRAWPREP --> SUBMIT["Queue submit or bridge commit\nborrowed inputs consumed immediately"]
    CLEARPREP --> SUBMIT
    COPYPREP --> SUBMIT
    READPREP --> SUBMIT
    PRESPREP --> SUBMIT

    SUBMIT --> SLOT["ChunkSlot SoA/AoSoA\nheaders + draw state + params + arenas"]
    SLOT --> IMPORTED{"Source path"}
    IMPORTED -->|"native runtime"| QUEUE["CommandQueue pending slot"]
    IMPORTED -->|"PE/unix bridge"| WIRE["wire blob import\nvalidate + retain handles"]
    WIRE --> QUEUE

    QUEUE --> ENCODE["Encode thread iterates MetalCommandView"]
    ENCODE --> HAZARD{"same pass and no hazard?"}
    HAZARD -->|"yes"| KEEP["keep render encoder open"]
    HAZARD -->|"no"| SPLIT["end/start encoder\nload/store transition"]

    KEEP --> BIND["bind pipeline/depth/resources/uniforms"]
    SPLIT --> BIND
    BIND --> ENCODECMD{"Command kind"}
    ENCODECMD -->|"DrawRun"| DRAWMETAL["encode draw calls"]
    ENCODECMD -->|"Clear"| CLEARMETAL["fold clear as load action\nor encode clear"]
    ENCODECMD -->|"Copy/Stretch/ColorFill"| BLITMETAL["encode blit or render-pass op"]
    ENCODECMD -->|"Readback"| READMETAL["encode staging copy\nforce visibility"]
    ENCODECMD -->|"Present"| PRESENTMETAL["acquire drawable\nblit/present drawable"]

    DRAWMETAL --> CB["MTLCommandBuffer commit"]
    CLEARMETAL --> CB
    BLITMETAL --> CB
    READMETAL --> CB
    PRESENTMETAL --> CB
    CB --> GPU["GPU execution"]
    GPU --> FINISH["finish thread\nseq ID + frame token + reclaim"]
    FINISH --> APIRETURN["D3D9-visible completion\nonly waits when API requires it"]
```

Render-flow invariants:

- `Set*` stays CPU-side and only affects later command records.
- Draw preparation may build canonical values, but queue storage owns flat records
  and spans before the API call returns.
- Clear, copy, stretch, readback, and present use the same queue ordering model as
  draw work unless the D3D9 API requires a synchronous result.
- The encoder is the first point that may make Metal render-pass decisions.
- Completion is reported through sequence IDs and frame tokens, not by exposing
  Metal objects to the PE side.

---

## 6. Concurrency Model

The concurrency model is producer/consumer with bounded queue ownership. The
application-visible thread records D3D9 work and hands it to queue-owned storage;
the encode thread, Metal/GPU, and finish thread advance that work after the API
call has returned unless an explicit synchronization boundary applies.

```mermaid
flowchart TD
    subgraph Producer["Producer side"]
        APP["Application / Wine API thread"]
        PE["PE frontend\nDeviceState + recorder"]
        BRIDGE["winemetal bridge\ncommit/import call frame"]
    end

    subgraph Queue["Queue-owned domain"]
        WORKER["replay worker\nFIFO raw-chunk replay"]
        WRITER["queue writer\nslot admission + seq ID"]
        SLOT["ChunkSlot ring\nbounded in-flight work"]
        ENCODE["encode thread\nMetalCommandView replay"]
        FINISH["finish thread\ncompletion + reclaim"]
        WAITERS["explicit waiters\nqueries, readback, frame latency"]
    end

    subgraph Async["Asynchronous sidecars"]
        PSO["pipeline compile workers\ncache publication"]
        PRES["Presenter / layer access\ndrawable acquire + token"]
    end

    subgraph Device["Metal / GPU"]
        CB["MTLCommandBuffer"]
        GPU["GPU execution"]
    end

    APP --> PE
    PE -->|"borrowed spans / POD records"| BRIDGE
    BRIDGE -->|"validate + import + mark, synchronous"| WORKER
    WORKER -->|"queue ownership established"| WRITER
    WRITER --> SLOT
    SLOT --> ENCODE
    ENCODE --> CB
    ENCODE -. cache miss .-> PSO
    ENCODE -. present command .-> PRES
    CB --> GPU
    GPU --> FINISH
    FINISH -->|"seq ID / frame token"| WAITERS
    WAITERS -->|"only when API/back-pressure requires"| APP
    WRITER -. bounded pressure .-> WAITERS
```

### 6.1 Agents And Ownership

| Agent | Owns | May run in parallel with | Must not do |
|---|---|---|---|
| Application / Wine API thread | D3D9-visible call order, PE validation, PE `DeviceState` mutation | encode thread, GPU execution of older chunks, finish thread | call Metal directly or keep backend-owned pointers |
| PE recorder / bridge call frame | POD chunk construction, local wire blob lifetime | prior GPU work, async pipeline compilation | expose PE COM pointers or borrowed stack spans after return |
| Commit synchronous half (app thread) | wire validation, unix-owned blob copy + wrapper retention, bulk resource marking, raw-queue push | worker replaying *older* chunks, encode thread, GPU | hand off a record before validation and marking complete (`R-BACK-2.51(a)`) |
| Replay worker (device-owned) | FIFO replay of raw chunks, draw-submission construction, slot publish | application recording *later* chunks, encode thread on older slots, GPU | reorder or parallelize replay across chunks (`R-BACK-2.51(b)`); run while a direct device call observes unix state (`(d)`) |
| Queue writer / importer | slot admission, record validation, retained handles, sequence ID assignment | application recording future work, encode thread on older slots | enqueue unbounded work or accept malformed records |
| Encode thread | queue-local replay state, active Metal encoder, render-pass merge/split decisions | application submission, GPU execution of older command buffers | read PE `DeviceState` or expose partially encoded command buffers |
| Pipeline compile workers | sidecar PSO compilation and cache publication | encode thread and GPU work | change D3D9 command order or mutate queue records |
| Metal/GPU | asynchronous execution of committed command buffers | CPU recording and encoding later work | signal D3D9-visible completion directly |
| Finish thread | completed sequence IDs, frame-token signaling, resource reclaim | application submission and encode thread | free resources before their last-use sequence completes |
| Presenter / layer access | drawable acquisition, layer property updates, present token relation | encode thread up to the point of drawable use | let PE code own `CAMetalLayer` or `CAMetalDrawable` |

### 6.2 Fire-And-Forget Taxonomy

| Operation class | Return point | Synchronization policy |
|---|---|---|
| `Set*` state mutation | PE state is updated | no queue or GPU wait |
| Ordinary `Draw*` / `Clear` | command data is copied/imported into queue-owned storage | fire-and-forget after queue ownership |
| Queued surface copy / stretch / color fill | command is queued and referenced resources are retained | fire-and-forget unless the API requires a visible result |
| `Present` | present command is accepted after any configured frame-latency gate | may wait for older present tokens, but not for the submitted present's GPU completion by default |
| `GetRenderTargetData` / synchronous readback | destination data is CPU-visible and complete | explicit GPU/queue drain boundary |
| Query `GetData` without flush | current completion waterline is inspected | no forced wait |
| Query `GetData` with `D3DGETDATA_FLUSH` | queued work needed for the query has been committed and eventually completed | explicit progress boundary; may spin/poll according to D3D9 contract |
| `WaitForVBlank` | vblank/presenter wait completes | explicit display synchronization boundary |
| reset/lost-device drain and shutdown | in-flight queue work has reached the required safe point | explicit lifecycle synchronization boundary |

Fire-and-forget means the API call no longer needs the caller's transient memory
or PE object pointers after it returns. It does not mean resources may be freed
immediately; queue retention and sequence IDs continue to own lifetime.

### 6.3 CPU/GPU Overlap Sequence

```mermaid
sequenceDiagram
    participant App as App / Wine thread
    participant D9 as PE D3D9
    participant CQ as CommandQueue
    participant Enc as Encode thread
    participant GPU as Metal / GPU
    participant Fin as Finish thread
    participant Wait as Explicit waiter

    App->>D9: Draw/Clear/Present calls for frame N
    D9->>CQ: commit or submit chunk N
    CQ-->>D9: ownership accepted
    D9-->>App: return to app

    par app records frame N+1
        App->>D9: more Set*/Draw* calls
    and queue encodes frame N
        CQ->>Enc: dequeue chunk N
        Enc->>GPU: commit command buffer N
    and GPU completes older work
        GPU-->>Fin: completion for chunk N-1
        Fin->>CQ: advance completedSeqId / frame token
    end

    opt readback, query flush, frame-latency, ring-full, or shutdown
        App->>Wait: request explicit synchronization
        Wait->>CQ: wait for seq/frame/free-slot condition
        CQ-->>Wait: condition satisfied or shutdown
        Wait-->>App: API returns visible result
    end
```

### 6.4 Ordering And Safety Rules

- D3D9 command order is preserved by queue order and sequence IDs, even when CPU,
  encode, GPU, and finish work overlap.
- Back-pressure is deterministic: ring slots, chunk limits, and frame-latency
  tokens bound how far the CPU can run ahead.
- A command buffer completion may advance `completedSeqId`; a present-bearing
  command buffer completion may additionally advance the present frame token.
- Resource destruction is deferred until `completedSeqId` reaches the last use of
  the resource.
- The presenter can block on drawable acquisition or layer synchronization, but
  that wait is accounted as present/WSI pacing, not general draw submission.
- Sidecar parallelism such as pipeline compilation must publish results through
  thread-safe caches and must not reorder already accepted D3D9 work.

### 6.5 Pacing Independence

The three progress signals — `completedSeqId`, `presentCompletedSeqId` (frame
token), and ring-slot occupancy — advance independently. The only ordering
invariant relating them is `presentCompletedSeqId ≤ completedSeqId`, which
ensures a present token never advances ahead of the underlying command buffer's
completion. Beyond that invariant, the three are decoupled.

```mermaid
flowchart TD
    subgraph Sig["Progress signals (independent advance)"]
        C["completedSeqId\n(every command buffer)"]
        P["presentCompletedSeqId\n(present-bearing buffers only)"]
        R["ring-slot occupancy\n(chunk admission)"]
    end
    subgraph Cons["Consumers"]
        Q["query GetData / readback\n→ waits on completedSeqId"]
        FL["frame-latency gate\n→ waits on presentCompletedSeqId"]
        BP["queue writer back-pressure\n→ waits on free ring slot"]
    end
    C --> Q
    P --> FL
    R --> BP
    C -. invariant: P ≤ C .-> P
```

| Wait kind | Consumes | Must NOT block |
|---|---|---|
| `GetData(D3DGETDATA_FLUSH)` / `GetRenderTargetData` / readback drain | `completedSeqId` | present admission, present completion, frame-token advance |
| Frame-latency gate (`SetMaximumFrameLatency`) | `presentCompletedSeqId` | query resolution, readback completion, resource reclaim |
| Queue writer back-pressure | ring-slot occupancy | encode-thread or finish-thread progress on older slots |

Cross-axis blocking is a regression. Counters in §7 expose the spread between
the signals so that an unintended coupling (for example, a GPU stall in a
non-present command buffer that delays present-bearing completion) becomes
visible without wall-clock heuristics. Formal evidence: `QuerySeqId.tla` and
`PresentFrameLatency.tla` model the seqId and frame-token timelines as
separate variables sharing only the ordering invariant. The composite model
`ConcurrentProgressSignals.tla` (added 2026-05-09) directly proves the three
independence liveness properties — `NoQueryWaitBlocksPresent`,
`NoFrameLatencyBlocksQuery`, and `NoRingPressureBlocksPresentCompletion` —
under fairness on all six action families, with the `PacingOrdering`
invariant `presentCompletedSeqId ≤ completedSeqId` as the only relation
between the timelines.

---

## 7. Expected Performance Bottlenecks

The expected bottlenecks are workload-dependent. The architecture should identify
which stage dominates before changing data layout or synchronization policy.

> This section is the *taxonomy* — which stage can dominate, and what evidence
> settles it. For a stage-by-stage model of one real frame with the four views
> joined (sequence, state hand-off, thread concurrency, and measured per-stage
> cost), see [`docs/perfomance/frame-lifecycle.md`](../../docs/perfomance/frame-lifecycle.md).
> As of 2026-08-01 on 3DMark05 GT2 the largest owner is the encode thread. In a
> `41.10 ms` frame the encode thread is `~15.8 ms` (`~38%`, corrected for the
> `PerfScope` instrument family), the producer's D3D9 entry path is `9.14 ms`
> (`22.2%`), GPU is `1.90 ms` (`4.6%`), and the replay worker idles `25.77 ms`
> (`62.7%`) — no stage is saturated, so the frame is set by the serial
> produce → replay → encode → present chain. The earlier app-thread-saturated
> picture was superseded by the SWVP-probe removal in `83a0b085`, which cut the
> producer from `~41%` of the frame to `22%`; see
> [`docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.12.md`](../../docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.12.md).
> That is one workload, not a general verdict — GT1, GT3, and SFIV have
> different mixes.

```mermaid
flowchart LR
    CPU["CPU submission\nvalidation + state flatten"] --> BR["bridge/import\nwire blob + validation"]
    BR --> QS["queue storage\nSoA append + arenas"]
    QS --> ENC["encode/cache\npass merge + PSO/DSS lookup"]
    ENC --> GPU["GPU execution\nshader/fill/bandwidth"]
    GPU --> PRES["present\nCAMetalLayer drawable + frame token"]

    CPU -. counters .-> C1["submitDrawCpuNs\ndraw count\nstate dirty/hash reuse"]
    BR -. counters .-> C2["bridge ops/frame\nchunk commits\npayload bytes"]
    QS -. counters .-> C3["capacity growth\nuniform intern hit/miss\nUP bytes"]
    ENC -. counters .-> C4["encodeDrawCpuNs\nencodeChunkCpuNs\npipelineBuild\nencoder splits"]
    GPU -. counters .-> C5["frame P50/P95/P99\nGPU command-buffer time\nreadback stalls"]
    PRES -. counters .-> C6["present acquire wait\nboundary wait\ntoken wait\nsource valid/size"]
    CPU -. pacing .-> C7["seqVsPresentSpread\n(completedSeqId − presentCompletedSeqId)\nseqIdWaitNs / presentTokenWaitNs / ringSlotWaitNs\nper-axis (independent)"]
```

| Stage | Expected bottleneck when | Bound | Existing or required evidence | First response |
|---|---|---|---|---|
| PE validation and state flattening | many small draws or high `Set*` churn rebuild canonical state every draw | CPU | `submitDrawCpuNs`, draw count, state dirty/hash reuse counters | preserve fixed arrays and dirty masks; cache flat state slices before changing bridge shape |
| Wire blob build/import | bridge payload bytes or validation work grows with draw count instead of chunk count | CPU/boundary | bridge ops/frame, chunk commits, record count, payload bytes, import validation timing | keep chunk granularity; optimize direct section writes only after payload bytes dominate |
| Queue slot append | warm frames still grow vectors or copy large UP/uniform payloads per draw | CPU memory bandwidth | capacity-growth counters, UP bytes/frame, uniform intern hit/miss | reserve slot arrays, intern repeated uniform payloads, stage UP data through arenas |
| Queue mutex and ring pressure | writer waits or commit waits rise while GPU is not saturated | CPU/concurrency | queue writer wait, commit wait, slot occupancy, chunk count | adjust chunk flush policy and ring depth before adding new cross-thread paths |
| Encoder pass decisions | render-target changes, exact hazards, or clears split encoders too often | CPU/GPU | encoder split count, render-pass count, exact hazard/Bloom false-positive counters, encode chunk CPU time | keep exact hazard classification as the split source and fold clears/load actions |
| Pipeline/depth/shader cache | cold or state-alternating workloads rebuild PSO/DSS entries | CPU/stutter | `pipelineBuild`, cold/warm first-frame benchmark, cache hit/miss | stabilize flat keys, use `MTLBinaryArchive`, prewarm common variants |
| Resource upload/readback | texture streaming or readbacks force staging pressure or CPU waits | CPU/GPU sync | transient upload CPU time, sync wait, readback MB/s | use ring staging and batch copies; keep `GetRenderTargetData` as an explicit synchronous boundary |
| GPU shader/fill/bandwidth | command submission is cheap but GPU frame time dominates | GPU | GPU command-buffer time, frame P50/P95/P99, Metal capture | optimize render-pass merging, avoid redundant blits, then tune shader/format paths |
| Present/drawable pacing | frame token or drawable acquisition waits dominate frame time after present source is valid | display/WSI | present source valid/size, present acquire wait, boundary wait, token wait, pre-acquire hit/miss | tune drawable pre-acquire and frame-latency policy only after draw/hazard signals are clean |
| Pacing-axis coupling | a wait on one progress signal blocks progress on another beyond the `presentCompletedSeqId ≤ completedSeqId` invariant | CPU/sync regression | `seqVsPresentSpread`, per-axis wait counters (`seqIdWaitNs`, `presentTokenWaitNs`, `ringSlotWaitNs`), TLA+ liveness checks in `ConcurrentProgressSignals.tla` (composite), `QuerySeqId.tla` / `PresentFrameLatency.tla` (per-axis) | trace the offending waiter; restore independent advance per `R-ARCH-6.8`/`6.9` before tuning ring depth or frame-latency policy |

The highest-risk architecture regressions are still CPU-side: per-call bridge
fallback, per-draw heap growth after warm-up, and rebuilding large state payloads
instead of using flat hashes, handles, and interned slot-local storage. GPU-bound
workloads should be treated separately; changing the PE/unix boundary will not
fix shader, fill-rate, or drawable pacing limits.

---

## 8. Wire Blob Sequence

The PE/unix boundary uses a contiguous wire blob so the bridge can validate and
import command batches with one ABI call.

```mermaid
flowchart TD
    A["PE command records"] --> B["record table\nD9CCommandChunkWireRecordHeader[]"]
    A --> C["payload arena\nvariable tails"]
    A --> D["per-record handle ranges"]
    D --> E["handle table\nD9CCommandChunkWireHandleEntry[]"]
    B --> F["wire header\nversion + offsets + sizes"]
    E --> F
    C --> F
    F --> G["contiguous wire blob"]
    G --> H["bridge commit_chunk()"]
    H --> I["unix importer validation"]
    I --> J["queue-local records"]
```

Wire design rules:

- The blob layout is header, record table, handle table, payload arena.
- Record payloads are addressed by offset/size into the payload arena.
- Handle references are addressed by first/count into the handle table.
- Reserved fields are zero on produce and validated as zero on import.
- Final blob copies are acceptable at the C ABI boundary. They become a
  benchmark-driven optimization only if bridge payload bytes dominate CPU time.

---

## 9. CPU-Bound Versus GPU-Bound Signals

```mermaid
flowchart LR
    subgraph CPU["CPU-bound symptoms"]
        C1["high submit CPU time"]
        C2["bridge calls ~= D3D9 calls"]
        C3["ChunkSlot capacity growth after warm-up"]
        C4["low uniform interning hit rate"]
        C5["large UP bytes/frame"]
    end

    subgraph GPU["GPU-bound symptoms"]
        G1["long command-buffer GPU time"]
        G2["many render-encoder splits"]
        G3["pipeline/depth cache misses"]
        G4["blit/readback stalls"]
        G5["present drawable waits"]
    end

    subgraph Evidence["Evidence owners"]
        B["benchmarks"]
        T["native tests"]
        W["Wine PE conformance"]
        P["shader_runner readback"]
        L["TLA+ / assertions"]
    end

    C1 --> B
    C2 --> B
    C3 --> B
    C4 --> B
    C5 --> B
    G1 --> B
    G2 --> T
    G3 --> B
    G4 --> P
    G5 --> L
    W --> T
```

Architecture regressions are not limited to incorrect pixels. A rendering-correct
change can still fail if it regresses to per-call bridge submission, reintroduces
hot-path heap churn, or breaks DXMT-style queue ownership.

---

## 10. Reference And License Flow

```mermaid
flowchart TD
    DXMT["DXMT\nMIT"] --> DXMT_OK["May use/adapt with MIT notice preservation"]
    WINE["Wine D3D9 tests/source\nLGPL project"] --> WINE_ORACLE["Behavioral oracle only\nno Wine implementation code copy"]
    DXVK["DXVK / D9VK\nstructure reference"] --> DXVK_REF["Inspect architecture and algorithms only\nno code copy by project policy"]
    FINDING["Finding"] --> CLASSIFY{"What kind?"}
    CLASSIFY -->|"Behavior"| SPEC["Requirement/test oracle"]
    CLASSIFY -->|"Architecture"| DESIGN["Design rationale"]
    CLASSIFY -->|"Code"| REJECT["Reject unless separate license review approves"]
    DXMT_OK --> CLASSIFY
    WINE_ORACLE --> CLASSIFY
    DXVK_REF --> CLASSIFY
```

The license policy is conservative by design:

- MIT-compatible project code may incorporate DXMT MIT material with notices.
- Wine-oracle behavior is acceptable as an oracle; Wine implementation structure
  is not an architecture requirement.
- DXVK/D9VK are useful references for structure and tradeoffs, but dxmt9 does
  not copy their code.
- Any external artifact that becomes a committed test/corpus/fixture needs
  provenance metadata.

---

## 11. Verification Mapping

| Requirement area | Evidence |
|---|---|
| `R-ARCH-1.*` DXMT ownership | `specs/backend/spec.md`, command queue tests, import tests, gap row for merge readiness |
| `R-ARCH-2.*` DOD layout | `dxmt9-state-draw-transform-spec`, chunk/import specs, backend key/pipeline specs |
| `R-ARCH-3.*` boundaries | `dxmt9-chunk-record-v2-validation-spec`, `dxmt9-chunk-record-v2-replay-spec`, `dxmt9-resource-hazard-v2-spec`, TLA+ queue/resource models |
| `R-ARCH-4.*` provenance | specs review, conformance manifest provenance, license review before imports |
| `R-ARCH-5.*` verification | `specs/verification/`, `specs/tests/`, `specs/benchmarks/`, `specs/archicture/gap.md` |
| `R-ARCH-6.*` concurrency | `CommandQueue.tla`, `QueueLifecycleRefinement.tla`, `PresentFrameLatency.tla`, `ResourceLifetime.tla`, `EncoderLifecycle.tla` (exact handle sets + Bloom-as-diagnostic-only invariants), `QuerySeqId.tla`, `ConcurrentProgressSignals.tla` (pacing independence liveness), queue observer tests, wait/perf counters |

Current known partial evidence remains tracked in `specs/archicture/gap.md`, especially the
DOD / DXMT ownership acceptance row.
