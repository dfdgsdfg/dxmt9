# Backend Design

---

## 1. Module Structure

```mermaid
graph TD
    subgraph PE["Win32 / PE side (Wine)"]
        CORE["D3D9 core\nDeviceState + COM objects"]
        REC["PE CommandRecorder\nPOD CommandChunk builder"]
        THUNKS["winemetal bridge ABI\ncommitChunk()\nresource ops\nframe-token waits"]
        SHADER_THUNK["shader compiler thunks\n(D3DBC → SPIR-V → MSL)"]
    end

    subgraph Unix["macOS / unix lib side"]
        IMPORT["Chunk importer\nvalidate + retain handles"]
        CQ["CommandQueue\n(32-slot execution ring, 3 threads)"]
        AEC["ArgumentEncodingContext\n(encode thread)"]
        ALLOC["Ring allocators\n(argbuf, replay, staging, copy-temp)"]
        PSO["PSOCache\n(MTLRenderPipelineState)\n+ DSS cache"]
        SHAD["ShaderCache\n(compiled MSL → MTLLibrary)\nDisk: MTLBinaryArchive"]
        TRANS["Shader translator\nvkd3d-shader + SPIRV-Cross\nOR direct D3DBC→MSL"]
        PRES["Presenter\n(CAMetalLayer blit)"]
        RESALLOC["ResourceAllocator\n(MTLBuffer, MTLTexture)"]
    end

    CORE --> REC
    REC -->|one bridge call per committed chunk| THUNKS
    THUNKS --> IMPORT
    IMPORT --> CQ
    THUNKS --> RESALLOC
    SHADER_THUNK --> TRANS
    TRANS --> SHAD
    CQ --> AEC
    CQ --> ALLOC
    AEC --> PSO
    AEC --> SHAD
    AEC --> PRES
```

### 1.1 Upstream DXMT Alignment Target

The backend target mirrors upstream DXMT's useful split: API calls record command
work first, and Metal execution happens later on queue-owned encode/completion
threads. dxmt9 differs only in the bridge payload format: D3D9 command data crosses
the Wine PE/unix boundary as POD records that import into queue-local replay actions.

```mermaid
flowchart TD
    subgraph DXMT["Upstream DXMT D3D11 shape"]
        DXCtx["D3D11 immediate context\nEmitST / EmitOP"]
        DXChunk["CommandChunk\ncommand list"]
        DXQueue["CommandQueue\nencode + finish threads"]
        DXMetal["Metal wrapper calls"]
        DXCtx --> DXChunk --> DXQueue --> DXMetal
    end

    subgraph DXMT9["dxmt9 target shape"]
        D9Dev["D3D9 PE device\nDeviceState shadow"]
        D9Chunk["PE CommandChunk\nPOD records + handles"]
        D9Bridge["winemetal commitChunk()\none unix call"]
        D9Import["unix importer\nvalidate + retain"]
        D9Queue["CommandQueue\nencode + finish threads"]
        D9Metal["Metal wrapper calls"]
        D9Dev --> D9Chunk --> D9Bridge --> D9Import --> D9Queue --> D9Metal
    end

    subgraph NonGoal["Non-goal"]
        SetCall["SetRenderState / SetTexture"] --> PerCall["per-call unix submit"]
        DrawCall["DrawPrimitive"] --> PerCall
    end
```

The alignment requirement is about ownership and timing, not source-level identity.
Upstream DXMT remains the architectural target shape even where dxmt9 uses a
different record representation:

- PE owns D3D API semantics, state shadowing, getters, state blocks, and command
  packet construction.
- The Wine bridge owns ABI marshalling only.
- The unix importer owns packet validation, POD record import, handle lookup, and
  handle retention. It rejects malformed records before queue ownership begins.
- `CommandQueue` owns execution chunk lifecycle, chunk scheduling, encode-thread and
  finish-thread progression, Metal command-buffer lifetime, completion, sequence
  fences, and frame-latency token allocation/signaling.
- `Presenter` owns `CAMetalLayer` access, drawable acquisition, layer pacing, the
  back-buffer-to-drawable copy, and `presentDrawable` encoding.

No PE COM object, Objective-C object, process-local pointer, or per-call mutable
scratch object participates in unix-side encode ownership. The backend consumes
compact imported records and state structs; it does not reach back into the D3D9
device object to interpret commands.

---

## 2. Command Queue

The command queue is the central coordinator. It decouples PE-side D3D9 command
recording from unix-side Metal encoding.

There are two chunk forms:

- **PE CommandChunk:** a POD command stream produced by the core without calling
  Metal or ObjC APIs.
- **Execution chunk:** the unix-side queue slot that owns retained handles,
  imported records, transient allocator ranges, and the eventual Metal command
  buffer.

```mermaid
graph LR
    subgraph PE["PE side"]
        WT["Application thread"]
        REC["CommandRecorder\nappend POD records"]
        PCHUNK["PE CommandChunk\nDraw/Clear/Present/etc."]
    end

    subgraph Bridge["Wine PE/unix bridge"]
        COMMIT["commitChunk()\none unix call"]
    end

    subgraph Ring["Unix execution ring"]
        I["Import slot\nretain handles"]
        E["Encode slot\nencode thread"]
        G["GPU-running slot"]
        F["Free slot"]
    end

    subgraph Threads
        ET["Encode thread\nreplays imported records → MTLCommandBuffer"]
        FT["Finish thread\nwaits GPU done → releases chunk"]
    end

    WT --> REC
    REC --> PCHUNK
    PCHUNK --> COMMIT
    COMMIT --> I
    I --> E
    ET -->|MTLCommandBuffer.commit| G
    G -->|GPU completion| FT
    FT -->|reset chunk| F
```

**PE CommandChunk** holds:
- A compact command header array and POD payload arena
- Opaque backend handles for buffers, textures, shaders, swap chains, and queries
- No COM pointers, ObjC pointers, unix-side object pointers, or lambdas
- Command-count and byte-size fields used to bound tail latency

**Execution chunk** holds:
- Imported command records and queue-local replay actions owned by the unix side
- Four ring sub-allocators: `staging` (CPU-visible readback), `copy_temp` (GPU private
  blit), `argbuf` (argument buffers), `replay_store` (imported replay storage)
- A sequence ID used to determine when in-flight resources can be released
- Optional frame token metadata when the chunk contains a present

**Submission flow:**

1. D3D9 calls update PE-side state or append records to the current PE chunk.
2. On `Present`, readback, query ordering, resource hazard, or chunk limit, the core
   commits the PE chunk through `commitChunk()`.
3. The unix importer validates the records, retains referenced backend handles, and
   assigns a sequence ID. If the chunk contains a present, it also assigns a frame
   token.
4. The encode thread dequeues the execution chunk and replays records against
   `ArgumentEncodingContext`, producing Metal commands.
5. The encode thread commits the `MTLCommandBuffer`.
6. The finish thread waits for GPU completion, signals sequence/frame fences, releases
   handles, and returns ring allocator memory.

### 2.1 Bridge Granularity Target

```mermaid
flowchart LR
    subgraph Bad["Non-goal"]
        A1["DrawPrimitive"] --> A2["unix-call submitDraw"]
        A3["SetTexture"] --> A4["unix-call setTexture"]
        A5["SetRenderState"] --> A6["unix-call setRenderState"]
    end

    subgraph Good["Target"]
        B1["Many Set* + Draw* calls"] --> B2["PE CommandChunk"]
        B2 --> B3["single commitChunk unix-call"]
        B3 --> B4["unix CommandQueue"]
    end
```

The backend may keep narrow per-operation entry points for tests or bootstrap, but
the default Wine runtime path is chunk submission. A design that sends every D3D9
draw/state call through `WINE_UNIX_CALL` is non-compliant with this performance
target.

### 2.2 Chunk Lifecycle and Ownership

```mermaid
stateDiagram-v2
    [*] --> PERecording : current PE chunk
    PERecording --> PESealed : present / readback / query / size limit
    PESealed --> Importing : commitChunk()
    Importing --> Queued : validate records\nretain handles\nassign seqId
    Queued --> QueuedPresent : contains PresentCommand\nassign frameToken
    Queued --> Encoding : encode thread dequeues
    QueuedPresent --> Encoding : encode thread dequeues
    Encoding --> GPU : commandBuffer.commit()
    GPU --> Completed : finish thread observes completion
    Completed --> [*] : release handles\nreturn allocators\nsignal seq/frame fences

    note right of PERecording
        PE memory may be reused only
        after commitChunk copies or
        imports the records safely.
    end note
```

Ownership rules:

- The PE chunk owns its POD payload until `commitChunk()` returns.
- The importer must copy or take ownership of every record and retained handle needed
  after `commitChunk()` returns.
- Execution chunks own backend handle references until completion.
- Frame tokens exist only for chunks containing a present command.
- Sequence IDs track all chunk completion; frame tokens track present-bearing chunk
  completion.

### 2.2.1 Sub-CommandBuffer Chain (G axis target)

`R-BACK-2.29` permits a single execution chunk to commit through a chain
of `MTLCommandBuffer` instances on the same `MTLCommandQueue`. The
chunk's `seqId` covers the chain; `completedSeqId` advances only when
the **last** sub-CB reaches Completed.

```mermaid
stateDiagram-v2
    [*] --> Encoding : encode thread dequeues chunk
    Encoding --> SubCBOpen : encode N records
    SubCBOpen --> SubCBCommit : split policy fires
    SubCBCommit --> SubCBOpen : open next sub-CB on same queue
    SubCBCommit --> EncodingPresent : on present record (last sub-CB)
    EncodingPresent --> ChainTailCommit : encode presentDrawable + commit()
    ChainTailCommit --> ChainGPU : driver runs all sub-CBs in order
    ChainGPU --> ChainCompleted : finish thread observes last sub-CB completion
    ChainCompleted --> [*] : advance seqId / frameToken / reclaim
```

Split policy rules (R-BACK-2.31 deterministic):

- A split decision must read only imported record fields, retained
  handle metadata, and queue-local state captured at chunk admit time.
- Candidate triggers (any may be chosen, mixing is allowed if the
  composite predicate is still deterministic):
  - **per-N records** — every N records since the last commit, where
    N is a fixed encode-thread constant or env-driven knob.
  - **per-render-pass** — commit at every `flushRender` whose split
    reason is not `Final`.
  - **per-X bytes** — commit when total encoded byte count crosses a
    threshold (requires byte accounting in encode thread).
- Wallclock-time-based or GPU-feedback-based triggers are forbidden.
  They violate R-BACK-2.16 / R-BACK-2.31 determinism.

Present and chain-tail rule (R-BACK-2.30):

- The present-bearing record's encoder must be the **last** sub-CB.
  Any preceding sub-CB must not call `presentDrawable`, must not
  acquire a drawable, and must not advance the present-frame-token.
- `splitBeforeBlockingPresent()` (existing path) becomes a special
  case of mid-chunk split — the policy that always closes a sub-CB
  immediately before the present encoder opens. Other split policies
  fire earlier in the chunk.

Reclaim rule (R-BACK-2.32):

- All chunk-owned resources (transient slabs, retained handles, arg
  buffers) remain pinned until the chain's last sub-CB completes.
- Sub-CB GPU completion order is guaranteed by Metal's same-queue
  in-order submission. The queue tracker must not re-derive ordering
  per sub-CB.

Failure mode:

- A sub-CB that fails (`WMTCommandBufferStatusError`) marks the entire
  chunk failed. The chain's tail still must run completion handlers so
  the present token does not stall. The error is reported via the
  `gpu_command_buffer_errors` counter (M5) at the failure site, not
  per-sub-CB.

### 2.2.2 Producer / Encode Overlap — Goal, Reference Models, and Reverted Carriers

Scope (R-BACK-2.35–R-BACK-2.41). This section states the overlap **goal** and
records the diagnostic / historical run-ahead carriers (A/B/C below) that proved
the P4 wait can move but were reverted. **The production design is
§2.2.3 (EncodeSession / open-render-encoder pass streaming, R-BACK-2.42–2.50);
the A/B/C carriers here are not the production path.**

The goal: the producer (CS/submit thread) and the encode thread overlap the
completion thread's present-completion wait, so per-present wall time approaches
`max(producer, encode, present-wait)` instead of their sum. `Present` stays the
only synchronization point (R-BACK-2.37); offscreen work runs ahead under it.

Reference models (apply equally to the §2.2.3 production design):

- **DXVK D3D9.** The D3D9 device records command chunks and dispatches them to
  `DxvkCsThread` (`dxvk/src/dxvk/dxvk_cs.*`). `EmitCsChunk()` enqueues a chunk;
  `Flush()` appends fence/flush work and dispatches the current CS chunk
  (`dxvk/src/d3d9/d3d9_device.cpp`). The useful model is asynchronous CS-thread
  run-ahead with explicit synchronization only for flush, query, resource, or
  present-facing fences. dxmt9 should borrow the timing shape, not Vulkan's
  barrier or submission objects.
- **DXMT.** `CommandQueue::CommitCurrentChunk()` publishes a chunk to an encode
  thread, `CommitChunkInternal()` creates one Metal command buffer for that
  chunk, encodes it, and commits it; the finish thread waits completion and
  signals frame-latency fences (`dxmt/src/dxmt/dxmt_command_queue.*`). The
  presenter acquires the drawable inside present encoding
  (`dxmt/src/dxmt/dxmt_presenter.cpp`). dxmt9 keeps this ownership split but
  must add a finer CPU-ready/coalescing stage because simple early chunk publish
  maps too directly to extra Metal command buffers on Apple GPUs.

Terminology (**CPU-ready** / **Encode-ready** / **Present tail**) and the session
state model are defined once in §2.2.3; that section is authoritative. In short:
**CPU-ready** = imported/replayed records, snapshots, retained handles, allocator
ranges, and sequence metadata are queue-owned but no Metal command-buffer,
drawable, or present token has been chosen; the **Present tail** is the only unit
that acquires a drawable, encodes `presentDrawable`, and signals the frame token.

Diagnostic / historical carriers (all reverted — see Verification below):

| Carrier | What ran ahead | CB boundary chosen by | Why not production |
|---|---|---|---|
| **A. Render-pass-boundary publish** (R-BACK-2.36, diagnostic) | writing chunk directly published at offscreen pass / barrier boundaries | submit thread | one CB per published slot unless C re-merges; H54/H56 reject the draw-count form |
| **B. CpuReady staging** (R-BACK-2.40) | PE→unix replay, snapshots, retention, allocator ownership, queue submission | encoder, later | necessary readiness primitive, but slot-coalescing alone still re-fragments passes |
| **C. Multi-slot coalescing** (R-BACK-2.41) | several CPU-ready / early non-present slots | encoder, grouping slots into one CB chain | **superseded by §2.2.3** — slot-level coalescing closes the active render encoder at each source, producing the H115 final same-key reopen; only an `EncodeSession` that carries the open encoder (R-BACK-2.43) preserves locality |

The production path is therefore not "publish slots and re-merge" (A+C) but the
§2.2.3 EncodeSession contract, which makes the Metal encode session — not the
source slot — own render-pass lifetime. A/B/C remain useful only as the
diagnostic A/B knobs and the readiness/coalescing primitives the session reuses.

Ownership (the diagnostic A/B/C carriers; the production split is in §2.2.3):

- **Submit/CS thread** — may choose diagnostic direct-publish points (A) and
  stages CPU-ready units (B); in the production path it does not choose Metal
  command-buffer boundaries and never touches the drawable or present token.
- **Encode thread** — chooses `MTLCommandBuffer` chain boundaries (B/C) and owns
  R-BACK-2.30 present-tail attachment and the R-BACK-2.33 chain cap. In the
  reverted slot-coalescing carrier (C) it opened/closed a render encoder at every
  source boundary; §2.2.3 supersedes that — the production EncodeSession **carries
  one open render encoder across compatible sources** (R-BACK-2.43) and closes it
  only at semantic pass boundaries, which is what makes the locality gate passable.
- **Presenter** — sole owner of the drawable and present token; the
  present-bearing tail is the only frame-latency carrier and the only code path
  that performs drawable acquire + `presentDrawable` (R-BACK-2.37).
- **Finish thread** — reclaims on `completedSeqId` only (R-BACK-2.32); early
  CPU-ready or committed higher-seqId work cannot retire a resource a
  lower-seqId present CB still uses, because `completedSeqId` is monotone in
  submission order (R-BACK-2.38).

Invariants the overlap must preserve:

- **Present-only sync** — no non-present commit allocates/advances a present
  token (`PresentFrameLatency` `CommitNonPresent`).
- **Locality** — per-present CB / render-pass / tile-preservation shape
  unchanged vs. single-publish (R-BACK-2.36); the arbitrary-draw-count carrier
  and any one-CB-per-slot carrier are rejected for this reason.
- **Lifetime** — early CPU-ready or committed work marks retained resources
  against its `seqId` before visibility to the encode thread (`ResourceLifetime`
  `NoUseAfterFree`, R-BACK-2.38).
- **Tail-Present staging** — GT1 H86 shows the current Present-published slot is
  already a large tail-Present stream (`~329` commands and `~739` draw items per
  present before the final Present command, `100%` tail-Present slots). The
  production candidate is therefore not the existing diagnostic
  `DXMT9_SPLIT_PRESENT_CHUNK` path, which publishes pre-Present work and Present
  as separate chunks, but a CPU-ready staging form where the encoder can still
  choose a coalesced tail command-buffer / render-pass shape.

Existing reusable primitives (built; not wired to a production carrier): the
queue already exposes the multi-source ownership/completion surface the §2.2.3
design needs — `dequeueReadySlotBatch()` (move several consecutive ready slots to
`Encoding` without allocating), `runEncodeBatchLoop()` /
`IRenderBackend::onChunkBatchReady()` (default backend delegates a single-source
batch to `onChunkReady` and returns `nullopt` for empty/multi-source batches),
and `PendingCompletion` CB-to-N-`seqId` expansion. The production encode loop
still calls `runEncodeIteration()` / `onChunkReady()`; these primitives change no
default behavior. The opt-in tail-Present batch path consumes source slots
through `EncodeSession` refs instead of materializing a copied aggregate
`ChunkSlot`, but remains diagnostic until the §2.2.3 gates pass. The §2.2.3
completion contract (R-BACK-2.49) is authoritative for how one Metal tail expands
to ordered per-source `seqId` completion.

Verification (the full ordered gate set is in §2.2.3 *Verification Shape*; in
brief):

- Native/fake backend — semantic boundary detection, fail-open prefix submit,
  ordered source completion, and no inline completion of unsubmitted work.
- TLA — `PresentFrameLatency` (`CommitNonPresent` / `CommitPresent`,
  `OutstandingPresentBound`), `ResourceLifetime` (`NoUseAfterFree`),
  `QueueLifecycleRefinement` (`SeqIdAssignmentSafety`, `BoundedInflight`),
  `ConcurrentProgressSignals`, and `EncodeSessionCompletion`
  (`NoInlineCompletionOfSessionSources`, `PresentCompletionAfterTail`,
  `OrderedCompletionExpansion`). `EncodeSessionCompletion.tla` models the
  R-BACK-2.49 refinement where one Metal tail expands into several ordered
  per-source `seqId` completions.
- Runtime visual/locality — output must pass the visual gate at non-increasing
  `command_buffers_per_present` / `passes_per_present` / `tile_preservation_mib`
  / final same-key reopens (H57 locality gates in
  `scripts/tools/compare_3dmark05_perf_counters.py`).
- Runtime no-gputrace — only after visual/locality is clean, counters must show
  `completion_wait_with_enqueue_ms` ↑ or `completion_wait_without_enqueue_ms` ↓
  (H43 overlap gate).
- Reverted carriers — the A/C and B prototypes (`DXMT9_OFFSCREEN_RUN_AHEAD` plus
  ready-slot coalescing, then CPU-ready staging) moved the P4 wait but failed
  locality, total-wait, and visual-correctness gates and were reverted; current
  source honors none of those historical envs. The opt-in open-CB
  `EncodeSession` carrier is separate diagnostic infrastructure and remains
  gated by the §2.2.3 promotion requirements, not a reintroduced A/C/B carrier.
- Performance model — `docs/perfomance/present-pacing.md` (H10 under-pipelining,
  H54/H56 draw-count-carrier rejection, H57 locality gates, H73 run-ahead design
  gate, H108–H116/H134–H136 open-CB and render-session-carry failures).

### 2.2.3 EncodeSession / Open-Render-Encoder Pass Streaming

The ideal overlap target is not "keep a command buffer open." It is to make
the **Metal encode session** the unit that owns render-pass lifetime. D3D9
command order remains source-order; the optimization is that queue source
boundaries stop being mistaken for Metal render-pass boundaries.

```mermaid
flowchart LR
    subgraph Sources["Queue-owned source storage"]
        S0["source A\nrecords + payload arena\nretained handles\nseqId A"]
        S1["source B\nrecords + payload arena\nretained handles\nseqId B"]
        ST["Present tail\nseqId C + frame token"]
    end

    subgraph Session["EncodeSession"]
        CB["current MTLCommandBuffer"]
        RE["active MTLRenderCommandEncoder"]
        KEY["attachment key\nload/store state\nhazard sets"]
        SH["dirty/shadow state\nargbuf state\nstream/IB shadows"]
        META["source list\n[A,B,C]\ncallbacks + samples"]
    end

    S0 -->|"spans/views"| Session
    S1 -->|"spans/views"| Session
    ST -->|"tail only"| Session
    CB --> RE
    RE --> KEY
    KEY --> SH
    SH --> META
```

The session owns:

| State | Why it must be session-owned |
|---|---|
| Current `MTLCommandBuffer` | Several source `seqId`s may be backed by one Metal tail |
| Active render/blit/compute encoder | Metal allows only one active encoder; source boundaries must not close it |
| Attachment key, pending clear, load/store proof | Deferred clears and `DontCare` proofs belong to the logical pass |
| Exact read/write hazard sets | The next source can continue only if exact overlap rules permit |
| Dirty render/shader/viewport/scissor state | Carrying a render encoder also carries its dynamic state shadow |
| Argbuf/direct-cbuf/resource-array state | Argument tables and dirty cbuf regions must not be reset at a source boundary |
| Stream/IB binding shadows | Avoid rebinding or losing the concrete dynamic backing selected for prior draws |
| Sidecars, visibility samples, GPU sample cursor | Diagnostic observations publish at logical pass/session boundaries |
| Post-commit callbacks and completion sources | Tail completion expands to ordered source completion |

The session does **not** own or copy source payload bytes. Source slots retain
their imported record arrays, payload arenas, large uniform payloads, binding
snapshots, and handle-reference arrays. The session keeps compact source
metadata and views:

```text
SessionSourceRef {
  slotIndex
  seqId
  recordSpan
  retainedHandleRange
  allocatorRanges
  flags: hasPresent, isTail, mayAcquireDrawable
}
```

This is the DOD boundary for pass streaming: coalescing is a vector of source
references plus one session state object, not a merged heap object or a deep
copy of `ChunkSlot`.

Load/store proof lookahead follows the same boundary. The encoder may scan a
call-local suffix of already selected/retained sources when choosing store
actions for a logical pass, but the suffix is not stored in `EncodeSession`.
If the queue has not selected a future source, the proof remains source-local
or defensive.

#### Semantic Boundary Rules

`EncodeSession` continues an active render encoder across sources only when
the D3D9 command stream has no observable boundary at that point. It must end
the active encoder for these events:

| Boundary | Reason |
|---|---|
| RT/depth/stencil/sample-count change | Metal render-pass descriptor changes |
| Non-foldable clear | D3D9 observes clear order; load action no longer represents it |
| Active RT sampled or read by later draw | Exact RAW hazard requires split/barrier |
| Blit, resolve, readback, map, or query ordering point | Metal encoder kind changes or app-visible observation |
| Resource-initializer wait requiring `encodeWaitForEvent` | Metal wait cannot be encoded with render encoder open |
| Present tail | Drawable acquire, blit/copy, `presentDrawable`, and frame token attach here |
| Capture/sidecar pass-end probe | Diagnostic explicitly observes pass-end contents |
| Session fail-open/final close | No tail is available or queue is draining |

Non-boundaries:

| Internal event | Required behavior |
|---|---|
| PE/unix `commitChunk()` | Import boundary only; no Metal pass implication |
| CPU-ready source boundary | Source metadata boundary only |
| Queue `seqId` boundary | Completion/lifetime boundary only |
| Payload byte threshold or draw-count threshold | Diagnostic trigger only unless it proves a semantic boundary |
| End of an `encodeChunk()` helper call | Must not imply `flushRender(Final)` in the session path |

The frame-latency policy may move the explicit present-completion wait from the
current `Present` return path to the next `Present` tail gate as an opt-in
run-ahead carrier. This is not equivalent to disabling the boundary: the final
Present tail still commits immediately, the next frame may build CPU-ready
offscreen sources while that tail completes, and the next `Present` tail drains
the previous target before allowing another present tail to be queued. The
optimization exists only to create source availability for R-BACK-2.40 and
R-BACK-2.43; it does not relax ordered completion, Present semantic boundaries,
or the locality gates above.

```mermaid
sequenceDiagram
  participant Q as Queue
  participant S as EncodeSession
  participant R as Render encoder
  participant P as Presenter
  participant C as Completion

  Q->>S: attach source A (seq A)
  S->>R: begin pass K, encode draws
  Q->>S: attach source B (seq B)
  S->>R: continue pass K if exact hazards clean
  Q->>S: attach Present tail (seq C)
  S->>R: end logical pass once
  S->>P: acquire drawable + encode present
  S->>S: commit Metal tail
  S-->>C: one Metal completion
  loop A,B,C in source order
    C->>Q: publish completed seqId
  end
```

#### Fail-Open Publication

A session may not hide visible frame work while waiting for a future tail. If
the carrier cannot attach a present tail or another compatible source at its
bounded release point, it finalizes and submits the prefix as a normal
non-present session.

```mermaid
stateDiagram-v2
  [*] --> Idle
  Idle --> ActiveSession: first source dequeued
  ActiveSession --> ActiveSession: append compatible source
  ActiveSession --> TailSubmit: present tail available
  ActiveSession --> PrefixSubmit: bounded release / no tail
  ActiveSession --> AbortBeforeEncode: source not consumed

  TailSubmit --> Complete: submit tail CB
  PrefixSubmit --> Complete: submit prefix CB
  AbortBeforeEncode --> Idle
  Complete --> Idle: completion expands seqIds
```

The forbidden state is "encoded visible head, no Metal submit, inline
completion." Completion-source expansion is allowed only after Metal completion
of the command buffer containing those source commands.

#### Metal API Constraints

Pass streaming is constrained by Metal's object model:

- a command buffer has at most one active encoder at a time;
- `endEncoding` is final for that encoder;
- `encodeWaitForEvent`, blit/compute work, and present encoding require the
  active render encoder to be closed first;
- command buffers committed to the same `MTLCommandQueue` execute in commit
  order, so a session can rely on tail completion as the ordered fence;
- `nextDrawable` and `presentDrawable` belong only to the present tail.

The design implication is that session state must be explicit. A helper that
only passes an injected `WMT::CommandBuffer` through `encodeChunk()` is
insufficient because the active render encoder and pass-local state are still
lost at helper return.

#### Minimal-Copy Contract

The session path is a scheduling and lifetime change, not a payload-copying
merge.

| Data | Ownership in the ideal path |
|---|---|
| Imported command records | Source slot arena; session stores span |
| Variable payload arena | Source slot arena; session stores offset/size view |
| Uniform payloads | Queue-local handle/slab; draw references handle |
| Binding snapshots | Source-owned compact payload; draw references snapshot handle/span |
| Retained resource refs | Source slot retention table; session references range |
| Completion metadata | Session source-ref vector; copied only as small metadata |
| Metal transient cbuf bytes | Materialized only when Metal API needs a bound buffer |

The hot path should remain linear: iterate sources, iterate records, update one
session state object, emit Metal commands. It must not concatenate all source
payloads, rebuild canonical draw state for already imported records, or copy
large uniform/resource arrays just because several sources share a session.

#### Verification Shape

The session model needs evidence through four ordered gates:

| Level | Required proof |
|---|---|
| Native/fake backend | semantic boundary detection, fail-open prefix submit, ordered source completion, no inline completion of unsubmitted work |
| TLA/refinement | one Metal completion event expands to ordered per-source `GpuComplete`; resources reclaim only after represented source seqIds complete |
| Runtime visual/locality | output passes the visual gate, and final same-key reopen, CB/pass count, load/store MiB, and tile preservation do not increase |
| Runtime no-gputrace | `completion_wait_with_enqueue_ms` rises or `completion_wait_without_enqueue_ms` falls after the visual/locality gate is clean |

Only after those gates pass should Xcode replay counters be spent. The Xcode
question is then whether the session changed GPU-side hot rows; the no-gputrace
question decides whether the carrier is a valid average-FPS/P4 mechanism.

### 2.3 Data-Oriented Transform Boundaries

The backend command path is a sequence of data transforms. Each stage owns explicit
data and hands the next stage compact immutable records or queue-local state structs.
This keeps the unix encoder deterministic and prevents hidden dependencies on PE COM
objects or ad-hoc per-call state.

```mermaid
flowchart LR
    PEState["PE DeviceState shadow\nD3D9 semantics"] --> REC["recordCommand()\nCanonicalDrawState + DrawRunDesc\ncompact POD records"]
    REC --> POD["PE CommandChunk\nheaders + payload arena\nopaque handles"]
    POD --> IMP["Importer\nvalidate, canonicalize,\nretain handles"]
    IMP --> IR["ImportedChunk\nrecords + FlatDrawStateView\nuniform handle + resource refs"]
    IR --> REPLAY["Replay transform\nrecords + queue state\n→ encoder ops"]
    REPLAY --> ENC["Encode transform\nencoder ops + caches\n→ Metal commands"]
```

Transform boundaries:

- **Record:** PE code converts D3D9 API calls and device state into POD records. This
  is the only stage allowed to inspect COM objects or D3D9-facing state blocks.
- **Import:** unix code validates record headers, payload sizes, offsets, version
  fields, and handle liveness, then retains backend resources into queue-owned
  imported structs.
- **Replay:** the encode thread consumes imported records plus queue-local state such
  as the active encoder, deferred clears, hazard filters, and allocator cursors.
- **Encode:** `ArgumentEncodingContext`, PSO/DSS/shader caches, and the presenter
  convert replay decisions into Metal commands.

Replay/encode helpers should be deterministic functions where possible:

```
nextEncoderState = replay(record, importedState, priorEncoderState)
psoKey           = makePsoKey(flatDrawStateView)
argLayout        = buildArgumentLayout(flatDrawStateView, resourceBindings)
```

Allowed inputs are immutable imported records, retained backend handles, explicit
queue-local state structs, cache interfaces, and allocator cursors. Disallowed inputs
are PE COM objects, PE `DeviceState` pointers, implicit globals that affect command
semantics, and per-call scratch state not represented in the execution chunk.

### 2.4 CommandChunk Storage and Wire ABI

`CommandChunk` is a data-oriented wire container, not an object graph. The PE side
builds a fixed header plus contiguous sections:

```
CommandChunkWire {
    ChunkHeader          header
    CommandRecordHeader  records[header.recordCount]
    uint8_t              payloadArena[header.payloadBytes]
    HandleTableEntry     handleTable[header.handleCount]
}
```

The record header array is fixed-width and cache-friendly so the importer can scan
record kinds, sizes, offsets, and handle indices linearly before touching payloads.
Payload bytes are stored in a single arena owned by the chunk. Variable-sized command
data is addressed by `{payloadOffset, payloadSize}` pairs, never by pointers.

Wire headers are POD structs with fixed integer fields:
- `ChunkHeader`: magic, ABI version, header size, record count, payload byte count,
  handle-table count, flags, and reserved fields that must be zero.
- `CommandRecordHeader`: opcode, header size, payload offset, payload size,
  first-handle index, handle count, alignment exponent or flags, and reserved fields
  that must be zero.
- Handle table entries are opaque integer backend handles plus kind tags where
  needed for validation.

The bridge ABI must define byte order, field sizes, alignment, and packing explicitly.
All wire structs are naturally aligned to their fixed ABI alignment, and every
payload starts at an offset aligned for the payload schema. Import rejects chunks when
`sizeof`/`alignof` static assertions, negotiated header sizes, or runtime offset
checks do not match the ABI contract.

Version policy:
- The importer accepts only compatible `ChunkHeader.abiVersion` values negotiated at
  backend initialization.
- Unknown opcodes in a compatible version are rejected for normal execution. They may
  be skipped only when the record carries an explicit ignorable/diagnostic flag and
  its payload bounds validate.
- Reserved fields must be zero so future versions can extend the ABI without making
  old importers silently reinterpret data.

Handle and payload rules:
- Records refer to resources through indices into the chunk handle table. The importer
  validates index ranges, kind compatibility, liveness, and duplicate references before
  retaining backend objects.
- The handle table is immutable after the PE chunk is sealed. Imported records store
  compact retained handle references or queue-local indices, not raw bridge handles
  that require repeated lookup on the encode thread.
- Payload arena ranges must satisfy `payloadOffset + payloadSize <= payloadBytes`
  without integer overflow. Nested offsets inside a payload are relative to that
  payload and must be bounds-checked before use.
- Payload schemas must not contain process-local pointers, Objective-C object
  references, COM pointers, vtables, virtual bases, `std::function`, C++ lambdas,
  allocator-owned containers, or host-endian opaque blobs with implicit layout.

Imported records preserve the same data-oriented shape: contiguous arrays for record
headers/decoded structs, compact handle-reference arrays, and arena-backed variable
payloads. Replay should walk these arrays linearly where possible; command-specific
decode may canonicalize into queue-local POD structs but must not allocate one heap
object per command.

Hot-path allocation policy:
- PE recording appends into pre-reserved chunk arrays and arenas. If capacity is
  exhausted, the recorder seals and submits the chunk or grows only outside the draw
  hot path.
- Import may copy into a queue slot's preallocated storage or ring allocator ranges.
  It must not allocate unbounded per-command heap memory for ordinary draw/state
  records.
- Encode and GPU-facing hot paths use queue/ring allocators for argument buffers,
  staging, copy-temp, and imported replay storage. Cache misses may allocate cache
  objects, but steady-state replay of imported records must not allocate from the
  system heap.

### 2.5 Draw-Run Batch Compatibility

Draw-run batching coalesces adjacent draws that share one canonical render state
into a single execution record, so the encoder binds state once and issues N draw
calls. Each queued submission carries a `{stateGeneration,stateLane}` stamp. The
stamp is the producer's witness that two `DrawRunSubmission`s were snapshotted
from the same stable cache object (`cachedBaseDrawStateForSubmissionBatch()`),
which is sufficient for them to share one canonical state. Per-draw differences
ride `DrawParam`, the binding-override / snapshot payloads, and per-draw uniform
payload identity. The per-draw binding rides a `DrawBindingOverride` built from
the live `DeviceState`, so `state.hot` is binding-agnostic
(`clearDrawStateBindingFields`).

The default path materializes a canonical state only when the stamp changes from
the previous queued submission, then elides the `state.hot` / `shaderLayout`
copy for same-stamp continuations. `CommandQueue::submitDrawRunBatch` keeps the
normal compatibility policy: same-stamp candidates use the generation fast path,
other materialized candidates fall back to the deep compatibility compare, and
an elided candidate may inherit compatibility from the previous accepted draw
when it shares that draw's stamp. This removes materialized-but-discarded
non-front state without reducing the normal compatibility batch width.

The opt-in `DXMT9_DRAWRUN_GROUP_BY_GEN_LANE=1` path is stricter diagnostic
batching: it groups adjacent submissions only when their stamps match, so no
deep compatibility fallback is used. It remains useful for paired A/B runs that
want to isolate stamp-only batching behavior from the default broader grouping.

`ChunkSlot::appendDrawRunBatch` stores the batch-front state once. For same-stamp
continuations, the only canonical state materialized is the one the batch keeps
or a materialized candidate needed by the normal deep-compare path — the
no-discarded-materialization floor for the same-stamp draw-state class
(R-ARCH-7.2, carrier part of R-ARCH-7.4).

The same carrier also stamps `uniformGeneration` and has an opt-in uniform
payload elision path that can reuse the previous `DrawUniformHandle` when
adjacent submissions share both state stamp and uniform generation. 3DMark05 GT1
currently rejects this as an optimization target: the state path elides hundreds
of thousands of non-front submissions, but `d3d9_snapshot_uniform_elided` remains
zero because the same-state groups still change uniform generation.

```mermaid
flowchart LR
  Draw["per-draw snapshot"] --> Same{"same {generation,lane}\nas previous submission?"}
  Same -->|yes| Elide["carry DrawParam + binding\n(no state.hot / shaderLayout copy)"]
  Same -->|no| Front["materialize state\n(FlatDrawStateRecord + shaderLayout)"]
  Elide --> Group["submitDrawRunBatch:\nnormal compat policy"]
  Front --> Group
  Group --> Fast["same-stamp fast path\nor accepted-previous transitivity"]
  Group --> Deep["deep compare for\nmaterialized different-stamp candidates"]
  Fast --> Append["appendDrawRunBatch:\nstore batch-front state once"]
  Deep --> Append
```

**Invariants.**

- *Stamp soundness.* Equal `{generation, lane}` implies byte-equal batch-consumed
  state. Only `front().state` is consumed downstream — by `makeDrawPsoSubview`,
  the `DrawRunInvariant` (render / decl / texture / sampler / stream masks and
  hashes), resource marking (`markDrawResources`), the back-buffer handle, and
  encode; every batch-front read addresses `front()`, never `back()`. The stamp
  covers every field those readers touch: constant-value drift within a generation
  is safe (none read the constant hashes — constants ride the per-draw
  `DrawUniformHandle`), and binding-field drift is safe (binding-agnostic
  `state.hot` clears those fields and the encoder binds from the per-draw
  `DrawBindingOverride`). Any field a mid-generation refresh (uniform refresh,
  binding-layout stride refresh) can change while the stamp stays equal, and that
  a front reader also consumes, forces a new generation or is excluded from the
  elided record.
- *Run-front availability.* Every queue batch front is materialized. Snapshot
  elision is only relative to the previous queued submission, so the first
  submission in the pending vector and every stamp-change submission owns a
  state. If normal compatibility grouping accepts a materialized different-stamp
  submission and the following submission is elided from it, the queue accepts
  that elided candidate through the already accepted previous draw rather than
  trying to deep-compare missing state.

**Strict stamp-only grouping.** `DXMT9_DRAWRUN_GROUP_BY_GEN_LANE=1` changes the
queue scan to read only the stamp, never a non-front `state.hot`, so an elided
continuation never needs a materialized state for a state-reading fallback.
Batching granularity is therefore snapshot identity: a stamp-run is as long as
the stable-state-generation run that produced it, so the producer keeps the
cache snapshot stable across compatible draws to keep runs long.

**Evidence counters.** The default path exposes
`d3d9_snapshot_state_materialized`,
`d3d9_snapshot_state_materialized_bytes`, `d3d9_snapshot_state_elided`, and
`d3d9_snapshot_state_elided_bytes` alongside the existing
`d3d9_snapshot_state_copy_cpu_ms`, draw-run batch counters, and
`submit_draw_run_batch_discarded_state_{records,bytes}`. A valid regression
check must show discarded state stays zero for same-stamp continuations and that
copy-byte elimination does not come with a larger batching/encode loss.

---

## 3. Encoder Lifecycle

```mermaid
flowchart TD
    A["Backend receives draw run\n+ FlatDrawStateView\n+ DrawUniformHandle"] --> B{Active encoder type\n== Render?}
    B -->|No| NEWPASS["End current encoder\nCreate new RenderEncoderData\nwith attachments from draw state RTs"]
    B -->|Yes| C{Same attachments\nas active encoder?}
    C -->|No| NEWPASS
    C -->|Yes| D{Hazard check:\nexact buf/tex\nread-write handle sets}
    D -->|Conflict| BARRIER["Insert barrier\nor split encoder"]
    D -->|Clean| MERGE["Continue same encoder"]
    NEWPASS --> ENCODE
    MERGE --> ENCODE
    BARRIER --> NEWPASS

    ENCODE["Encode draw:\n1. setRenderPipelineState\n2. setDepthStencilState\n3. setViewport / setCullMode / ...\n4. setVertexBuffer (argbuf)\n5. setFragmentBuffer (argbuf)\n6. drawIndexedPrimitives / drawPrimitives"]

    style MERGE fill:#9f9,stroke:#333
    style NEWPASS fill:#f9f,stroke:#333
    style BARRIER fill:#ff9,stroke:#333
```

**Clear folding:** A `ClearDesc` received before any draw on a given render target is
stored as deferred. When the first draw opens a render pass for those attachments, the
deferred clear is applied as `MTLLoadActionClear` rather than opening a separate pass.

**Encoder types:** Three encoder kinds may be active during a frame:
- `RenderEncoderData` — for draw calls
- `BlitEncoderData` — for `UpdateSurface`, `UpdateTexture`, `StretchRect`, mipmap gen
- `ComputeEncoderData` — reserved for future compute-based features (triangle fan
  expansion, point sprite expansion)

Only one encoder may be active at a time. Switching kind requires `endEncoding`.

Surface operation replay details are specified in
[`surface-ops/design.md`](surface-ops/design.md). The command queue consumes imported
POD records and emits blit, render-pass, resolve, and readback actions; surface
operation data does not cross the bridge as closures or lambdas.

Per-frequency draw-uniform layout details are specified in
[`draw-uniforms/design.md`](draw-uniforms/design.md). The encoder splits the
historical 9 KB `DrawUniforms` slab into per-stage and per-frequency UBOs
(`VsConsts`, `PsConsts`, `FfpVsConsts`, `FfpPsConsts`) plus an inline 16 B
`DrawVolatile` push, gated by a backend-owned dirty bitmask derived during
chunk-record import.

Render-pass `MTLLoadAction` / `MTLStoreAction` selection (TBDR tile
preservation policy) is specified in
[`render-pass-actions/design.md`](render-pass-actions/design.md). The encoder
maintains a per-CommandQueue "touched" attachment-handle set so first-use
color attachments DontCare-load, and runs an in-chunk look-ahead at
`flushRender` time to DontCare-store depth/stencil and color attachments
that are about to be cleared or overwritten. `R-BACK-2.6` is amended in
that spec to allow conditional `MTLStoreActionDontCare` on RT change.

### 3.1 `IDecisionRecorder` divergence seam (R-BACK-39.3)

The modern-renderer transition (`specs/d3d9-renderer/`) needs a way to compare
the modern path's per-chunk decisions against what the traditional encode path
above would have decided, behind `DXMT9_RENDERER_LOG_DIVERGENCE=1`. The
backend owns the recording interface because the decisions being compared
(pass begin, per-draw PSO/draw, per-attachment load/store action, encoder
split) are encode-lifecycle decisions described in this section. This is the
cross-spec ownership noted in `specs/d3d9-renderer/design.md` §15.4.

`IDecisionRecorder` (`src/dxmt9/render/decision_recorder.hpp`) is a small,
POD-friendly recording interface with four record points that mirror the real
encode decision sites:

| Record point | Mirrors |
|---|---|
| `recordPassBegin(rt0, depth, colorCount)` | `encoders::beginRenderPass` attachment selection |
| `recordDraw(psoKey, count, flags)` | `encoders::encodeDraw` |
| `recordLoadStore(handle, load, storeOrProof)` | `RenderPass{Depth,Color}StoreProof` selection |
| `recordEncoderSplit(reason)` | `perf::EncoderSplitReason` boundary |

`DecisionRecord` is a flat POD (kind enum + three `u64` lanes) with no owning
pointers, so a recorded sequence is trivially copyable and comparable.
`VectorDecisionRecorder` appends records to a `std::vector` it exclusively
owns, and `compareDecisions(modern, reference)` is a pure helper that reports
whether two sequences diverge and at which index.

**Side-effect neutrality (R-BACK-39.3).** A recorder is a pure observer: an
implementation must not call any Metal API, allocate/commit an
`MTLCommandBuffer`, invoke the presenter, mutate the PSO/shader cache or
`MTLBinaryArchive`, touch `MTLHeap` residency or retained-handle sets, or
update queue-level fence state. A divergence run must never change which Metal
commands a non-divergence run would emit. `VectorDecisionRecorder` satisfies
this by mutating only its own storage.

**L0 scope / L1 deferral.** At L0 the FrameGraph backend is a pure delegate
(byte-identical to traditional), so there is nothing to diverge yet. L0 lands
only the interface, the POD record, the vector-backed recorder, the env
resolver (`resolveLogDivergence` / `logDivergenceEnabledFromEnv`), and the
unit-tested `compareDecisions` helper. The full dry-run engine — walking a
chunk through a recorder-only adapter that reproduces the `TraditionalBackend`
decision sequence and emitting per-chunk divergence points — is **deferred to
L1**, when FrameGraph builds a DAG and a real divergence exists to detect.

---

## 4. Argument Buffer Binding

The production Stage 2 argument-buffer path is constants-only. When the
capability gate (`argumentBuffersSupport >= Tier2` and Apple GPU family support)
holds, texture-free and texture-bound draws select the Stage 2 PSO variant and
bind one slot-30 argument buffer for the four per-frequency constant-buffer
pointers:

```
Stage 2 ArgbufLayout (slot 30):
┌──────────────────────────────────────────────┐
│ id(0): constant VsConsts*                    │
│ id(1): constant FfpVsConsts*                 │
│ id(2): constant PsConsts*                    │
│ id(3): constant FfpPsConsts*                 │
└──────────────────────────────────────────────┘
```

Texture and sampler resources intentionally stay on the direct Metal encoder
lane (`setVertexTexture` / `setVertexSamplerState` /
`setFragmentTexture` / `setFragmentSamplerState`). The shader source for
texture-bound Stage 2 draws therefore combines a slot-30 argbuf uniform
parameter with direct `[[texture(N)]]` / `[[sampler(N)]]` parameters. This is
the current hot path contract: Stage 2 removes the direct slot 0/3 uniform
bindings, but it does not move texture or sampler resources into Metal
argument-buffer resource arrays.

```mermaid
flowchart TD
    STATE["FlatDrawStateView"]
    GATE{"argbuf hybrid gate?"}
    S1["Stage 1 PSO\nuniforms: direct slot 0/3\ntextures/samplers: direct"]
    S2["Stage 2 PSO\nuniforms: slot 30 argbuf\ntextures/samplers: direct"]
    CBUF["updateDirtyArgbufRegions\nids 0..3 only"]
    RES["binding packet\npre-resolved texture/sampler handles"]
    DIRECT["set*Texture / set*SamplerState"]
    DRAW["draw"]

    STATE --> GATE
    GATE -->|"no"| S1 --> DRAW
    GATE -->|"yes"| S2
    S2 --> CBUF --> DRAW
    S2 --> RES --> DIRECT --> DRAW
```

The fully packed "texture/sampler resource-array" lane remains an experimental
candidate, not the default path. It can be attractive for draw streams that bind
many textures/samplers and reuse the same binding packet, but it does not make
Metal-side writes disappear: the encoder still has to patch the argument buffer,
retain or mark resources resident, and keep sampler/resource lifetimes valid
until command-buffer completion. For small binding counts or frequently changing
D3D9 sampler state, direct `setTexture` / `setSamplerState` calls may be cheaper
and are much easier to validate.

The last resource-array attempt faulted on a representative texture corpus case.
Current fault candidates are:

- sampler objects not created or bridged with argument-buffer resource support
  (`supportArgumentBuffers` / resource-id path);
- sampler lifetime not retained through the command-buffer completion sequence;
- texture or sampler `MTLResourceID` encoding mismatch across the WMT bridge;
- missing or incorrectly scoped `useResource` / `useHeap` residency for
  argbuf-pointed resources;
- MSL/argument-encoder layout mismatch for resource arrays or root argbuf
  reference shape.

Any future promotion of texture/sampler resource arrays must land behind a
separate feature flag and prove, in order: texture-only argbuf sampling,
sampler-only argbuf sampling, combined 2D sampling, cube/3D/volume coverage,
sampler filter/address/sRGB/mip coverage, and paired Stage 1 vs Stage 2
shader-corpus pixel equality. Until that evidence exists, the default Stage 2
path stays constants-only with direct texture/sampler binding.

---

## 5. PSO Cache

```mermaid
graph TD
    DD["FlatDrawStateView"] --> KEY["PSO key extraction:\n• VS function (shader hash + variant)\n• FS function (shader hash + variant)\n• Vertex descriptor layout\n• RT pixel formats × 4\n• Blend state per attachment\n• Sample count\n• Alpha-to-coverage flag"]
    KEY --> LOOKUP{In PSO\ncache?}
    LOOKUP -->|Hit| USE["Use cached MTLRenderPipelineState"]
    LOOKUP -->|Miss| COMPILE["Compile async on thread pool\nMTLRenderPipelineDescriptor\n→ newRenderPipelineStateWithDescriptor"]
    COMPILE --> STORE["Store in cache\n(hash map keyed by PSO key)"]
    STORE --> USE
    USE --> ENC["Encode: setRenderPipelineState"]
```

The PSO cache is an in-memory hash map (key → `MTLRenderPipelineState`). It is
populated on first use. Cache entries are never evicted during a session (PSO count
is bounded by the state space of real D3D9 applications).

The `MTLDepthStencilState` cache is separate and keyed by the DSS key (depth +
stencil compare/write state). DSS creation is cheap; the cache prevents redundant
object allocation.

### 5.1 Cache Prewarm From `MTLBinaryArchive`

Prewarming populates PSO and shader-function caches at device init by
deserializing entries already present in the on-disk `MTLBinaryArchive`,
before the first encoded draw. This eliminates first-run stutter when an app
revisits previously seen pipeline states.

```mermaid
flowchart LR
    INIT["Device::init"] --> MODE{"prewarm mode"}
    MODE -->|"shipping"| FULL["full archive load:\nenumerate archive entries\n→ instantiate PSO + DSS\n→ populate caches"]
    MODE -->|"dev"| LAZY["lazy: do not load;\ncompile-on-first-miss with\non-disk archive serving as input"]
    MODE -->|"debug-disabled"| OFF["never read archive"]
    FULL --> READY["caches ready before first draw"]
    LAZY --> READY
    OFF --> READY

    READY --> ENC["encode-thread draw"]
    ENC --> MISS{"PSO cache miss?"}
    MISS -->|"hit"| USE["setRenderPipelineState"]
    MISS -->|"miss"| CMP["compile + write back to archive"]
    CMP --> USE
```

Mode selection (`R-BACK-3.8`) is a runtime config. Counters expose
`prewarmEntriesLoaded`, `prewarmLoadNs`, `prewarmFailureClass`,
`coldCompileCountAfterWarm`. The archive identity (path + version stamp) is
included in present diagnostics so support reports can confirm prewarm hit
the expected file. Failure to load the archive is non-fatal: the cache stays
empty and the path falls through to compile-and-write.

---

## 6. Shader Translation

```mermaid
flowchart LR
    BC["D3DBC bytecode\n(SM 1.x / 2.0 / 3.0)"]
    FFP["FFPKey\n(VS or PS)"]

    BC --> PRE["Preprocessing:\n1. Half-pixel offset injection (VS)\n2. Version token parse\n3. TRIANGLEFAN already gone (core)\n4. Optional debug-only VS Y flip"]
    PRE --> VKD3D["vkd3d_shader_compile()\nsource: D3D_BYTECODE\ntarget: SPIRV_BINARY"]
    VKD3D --> SPIRVCROSS["spirv_cross::CompilerMSL\n→ MSL source string"]
    FFP --> FFGEN["FFP shader generator\n→ MSL source string directly"]
    SPIRVCROSS --> COMPILE["[device newLibraryWithSource:]\nor precompiled .metallib"]
    FFGEN --> COMPILE
    COMPILE --> DISKCACHE["Disk cache\nMTLBinaryArchive\nkey: SHA-1(bytecode + variant)"]
    DISKCACHE --> FN["id<MTLFunction>"]
```

Pixel-shader texture sampling is intentionally absent from the normal
coordinate-system preprocessing list. D3D texture coordinates are passed through
to Metal sampling as D3D UVs; a global pixel V flip is only emitted when the
diagnostic `DXMT_DEBUG_FORCE_PIXEL_V_FLIP` source-contract path is enabled.

**Shader variant keys** for programmable shaders:
- Vertex: (bytecode_hash, input_layout_hash, rasterization_disabled)
- Pixel: (bytecode_hash, alpha_test_enable, alpha_test_func, fog_mode, clip_planes_mask, tile_ffp_mode)

Two draws with the same bytecode but different variant keys require separate
compiled `MTLFunction` objects. The `tile_ffp_mode` bit is set when the
fragment program is targeted at the tile-shader FFP path (§13); it forces a
distinct `MTLFunction` from the portable variant of the same bytecode.

### 6.1 Cross-Process Binary Archive

The `MTLBinaryArchive` (`R-BACK-4.8`) is shared across dxmt9 instances.

**Path & identity**
- Path: `${cache_root}/dxmt9-shaders-v${ABI}.${gpu_family}.metallib-archive`.
  The ABI version and GPU family are baked into the filename so a version or
  device-family mismatch never reads from a stale archive — they read from a
  separate file or no file at all.
- `${cache_root}` resolution order: `$DXMT9_CACHE_DIR`, then
  `$XDG_CACHE_HOME/dxmt9`, then `${HOME}/Library/Caches/dxmt9`.
- Entry key: SHA-1 over (bytecode | variant key | dxmt9 archive ABI version).

**Concurrency model (POSIX `flock`)**
- Writers acquire `LOCK_EX` on the archive file before `serializeToURL:`.
- Readers acquire `LOCK_SH` while loading; if the lock is unavailable for
  more than a short timeout (e.g., 100 ms), the loader treats the archive
  as unreadable and falls through to compile-and-write per `R-BACK-3.7`.
- A reader that holds `LOCK_SH` will see a consistent snapshot — Metal's
  archive serialization writes the whole file atomically via a temp file
  and rename, so partial reads are not possible.

**Versioning rule**
- The dxmt9 archive ABI version is bumped whenever any of these change:
  the MSL emitter output, the variant-key encoding, the
  `MTLBinaryArchive` schema, or the FFP key bit layout.
- A version bump produces a new archive filename; the old archive is left
  on disk and reclaimed by the per-version retention policy (default: keep
  the two most recent ABI versions, delete older).
- A device that bumps to a new ABI starts with an empty archive and warms
  it from compile fall-throughs.

**Failure modes**
| Failure | Detection | Action |
|---|---|---|
| File missing | `[NSURL checkResourceIsReachable]` | start with empty archive; not an error |
| File present, wrong magic / corrupt | `MTLBinaryArchive` initializer returns `nil` with `NSError` | log `prewarmFailureClass = "corrupt"`, rename file to `*.corrupt`, start empty |
| File present, schema mismatch (Metal version drift) | initializer returns `nil` with version error | log `prewarmFailureClass = "schema"`, rename to `*.outdated`, start empty |
| File present, GPU family mismatch | filename includes family; cannot happen by construction | n/a |
| Lock unavailable beyond timeout | `flock` returns `EWOULDBLOCK` | log `prewarmFailureClass = "lock_busy"`, fall through to compile-only |
| Mid-write crash by another process | Metal's atomic rename leaves either old or new file intact | next reader sees a valid archive (old or new), no recovery needed |
| Disk full on write | `serializeToURL:` returns error | log; archive grows on next successful write attempt |

A failure is non-fatal under all conditions — the runtime path always falls
through to compile-and-write. Counters distinguish failure classes so
support reports identify which path took the slow start.

**Configuration**
- Prewarm mode (`R-BACK-3.8`) is selected by build flag and runtime override:
  - default for shipping builds (release): `full`.
  - default for dev builds (`-Ddev_build=true`): `lazy`.
  - debug runtime: `disabled` via env var `DXMT9_PREWARM=off`.
- Counters: `prewarmEntriesLoaded`, `prewarmLoadNs`, `prewarmFailureClass`,
  `coldCompileCountAfterWarm`, `archiveBytes`. Present diagnostics expose
  the resolved archive path and the prewarm mode.

---

## 7. Resource Allocation Model

```mermaid
graph TD
    subgraph Pools["Storage mode selection"]
        DEFAULT_DYN["DEFAULT + DYNAMIC\n→ MTLStorageModeShared\n(CPU+GPU, coherent)"]
        DEFAULT_STATIC["DEFAULT + no DYNAMIC\n→ MTLStorageModePrivate\n(GPU only, fastest)"]
        MANAGED_P["MANAGED\n→ system-memory backup\n+ MTLStorageModeShared GPU copy\nuploaded on first use / after unlock"]
        SYSTEMMEM_P["SYSTEMMEM / SCRATCH\n→ malloc (no MTLBuffer)\nCPU only"]
    end

    subgraph Dynamic["Dynamic buffer renaming (LOCK_DISCARD)"]
        CURR["current_allocation"]
        POOL["free_pool (FIFO)\nretired allocations"]
        NEW["new_allocation\n(suballocated from ring\nor new MTLBuffer)"]
        CURR -->|"LOCK_DISCARD"| POOL
        POOL --> NEW
        NEW --> CURR
    end
```

**Texture upload path (MANAGED / SYSTEMMEM → GPU):**

The path bifurcates on `MTLDevice.hasUnifiedMemory` per `R-BACK-5.7`:

*Unified-memory devices (Apple Silicon, `hasUnifiedMemory == YES`)*:
1. Application `Lock()`s the texture surface and writes pixel data into the
   `MTLStorageModeShared` backing.
2. `Unlock()` marks the surface "uploaded" — no staging copy and no blit are
   needed. CPU-written bytes are GPU-visible immediately because both sides
   share the same physical memory.
3. The next render encoder that samples the texture sees the updated data.
   No fence is required for visibility (Metal's encoder ordering is enough);
   a fence is only required if the texture is concurrently written and read
   across encoders, which is not the upload case.

*Discrete-style devices (Intel iGPU / AMD dGPU on Mac, `hasUnifiedMemory == NO`)*:
1. Application `Lock()`s the texture surface and writes pixel data into the
   CPU-accessible (`MTLStorageModeManaged`) backing.
2. `Unlock()` marks the surface dirty.
3. On first draw that samples the texture, a blit encoder copies the dirty
   region from the managed staging buffer to the private `MTLTexture`.
4. The blit is fenced so the render encoder that follows reads the updated
   data.

Counter `managedTextureUploadBlitCount` advances only on the discrete path;
on Apple Silicon it must remain 0 across a workload's lifetime. A non-zero
value on a `hasUnifiedMemory` device is a regression of `R-BACK-5.7`.

### 7.1 Pool / Usage → Storage Mode Matrix

The mapping in `R-BACK-5.7` is reproduced here as a single-page reference.
The matrix differs between unified-memory devices (Apple Silicon: M1/M2/M3,
all hasUnifiedMemory) and discrete-style devices (Intel/AMD on Mac).

```mermaid
flowchart TD
    R["createBuffer / createTexture\n(pool, usage)"] --> CHK{"pool"}
    CHK -->|"DEFAULT + RT/DS"| PRIV["MTLStorageModePrivate\n(both unified + discrete)"]
    CHK -->|"DEFAULT + DYNAMIC"| RING["MTLStorageModeShared\n+ rename ring (§7.2)"]
    CHK -->|"DEFAULT (other)"| PRIV
    CHK -->|"MANAGED"| UM{"hasUnifiedMemory?"}
    UM -->|"yes (Apple Silicon)"| SHA["MTLStorageModeShared\nNO staging copy"]
    UM -->|"no"| MGD["MTLStorageModeManaged\n+ staging copy"]
    CHK -->|"SYSTEMMEM"| HOST["host malloc + Shared upload buffer"]
    CHK -->|"SCRATCH"| MAL["host malloc only\nnever reaches GPU"]
```

Apple Silicon's unified memory makes the `MANAGED` staging copy redundant —
both CPU and GPU view the same bytes. Detection uses `MTLDevice.hasUnifiedMemory`
at device init; the per-resource path branches once at create time and never
re-checks. On discrete-style Mac (legacy Intel iGPU + AMD dGPU) the path
preserves the conventional staging copy.

### 7.2 `D3DUSAGE_DYNAMIC` Rename Ring

Per `R-BACK-5.8`, dynamic buffers do not block on GPU completion when
`D3DLOCK_DISCARD` rotates allocations:

```mermaid
flowchart LR
    LD["D3DLOCK_DISCARD"] --> POOL{"free ring slot?"}
    POOL -->|"yes"| ROT["rotate to next allocation\nno wait"]
    POOL -->|"no"| FRESH["allocate fresh MTLBuffer\nappend to ring"]
    ROT --> WR["caller writes new data"]
    FRESH --> WR
    WR --> SUB["submit draw referencing new alloc"]
    SUB --> COMPLETE["GPU completes;\nseqId advances\nfreed alloc returns to ring tail"]
```

The ring is per-buffer-handle, not global, to keep rename cost local.
Capacity grows on demand and never shrinks during a session.

### 7.3 `MTLHeap` Small-Resource Pooling

Per `R-BACK-5.9` / §14, small textures and small non-dynamic buffers allocate
from `MTLHeap` instances rather than direct `newBufferWithLength` /
`newTextureWithDescriptor`.

```mermaid
flowchart TD
    REQ["createTexture(desc)"] --> SIZE{"footprint ≤ heapThreshold\n+ usage compatible?"}
    SIZE -->|"no"| DIRECT["direct allocation\n(unchanged path)"]
    SIZE -->|"yes"| FAM["select heap family\nby (storage mode, usage class)"]
    FAM --> CAP{"family has capacity?"}
    CAP -->|"yes"| ALLOC["heap.makeTexture(desc)"]
    CAP -->|"no"| GROW["allocate new heap\n(geometric growth, capped)"]
    GROW --> ALLOC
    ALLOC --> RES["mark resident\nat heap level once"]
    RES --> RET["return texture handle"]
```

Heap families (`R-BACK-14.1`):

| Family | Storage mode | Members |
|---|---|---|
| `priv-tex` | `MTLStorageModePrivate` | `D3DPOOL_DEFAULT` non-RT non-DS small textures |
| `shared-tex-um` | `MTLStorageModeShared` (unified memory only) | `MANAGED` / `SYSTEMMEM` small textures on Apple Silicon |
| `shared-buf` | `MTLStorageModeShared` | non-dynamic vertex/index/constant buffers below threshold |

Render targets, depth buffers, and dynamic-rename buffers stay on direct
allocation. Heap residency tracking elides per-texture residency calls
because the heap is the residency unit.

---

## 8. Presentation

```mermaid
sequenceDiagram
    participant App as Application thread
    participant Rec as PE CommandRecorder
    participant BR as winemetal bridge
    participant CQ as CommandQueue
    participant ET as Encode thread
    participant FT as Finish thread
    participant PR as Presenter
    participant CL as CAMetalLayer
    participant MTL as Metal

    App->>Rec: Present()
    Rec->>Rec: append PresentCommand\ncommit current chunk
    Rec->>BR: commitChunk(chunk)
    BR->>CQ: import chunk
    CQ->>CQ: allocate frameToken
    opt boundary policy applies
        CQ->>CQ: waitFrameLatency(frameToken, maxLatency)\nwaits for older present completion
    end
    CQ-->>BR: present accepted after boundary policy
    BR-->>Rec: commitChunk returns
    Rec-->>App: Present returns

    ET->>PR: encode PresentCommand(frameToken)
    PR->>CL: nextDrawable\n(blocks when drawable/vsync-limited)
    ET->>MTL: blitCommandEncoder: copy backbuffer → drawable.texture
    ET->>MTL: commandBuffer.presentDrawable(drawable)
    ET->>MTL: commandBuffer.commit()
    FT->>CQ: signal frameToken\non command buffer completion
```

The back buffer is a private `MTLTexture` owned by the swap chain. On present, it
is copied to the `CAMetalLayer` drawable via a blit encoder. The drawable is obtained
just before the blit — not before the frame — to minimize latency.

Present instrumentation must identify which source was selected before drawable
work starts. The counter set records explicit/current-backbuffer selection, source
validity, source handle/texture, source size, format, sample count, pass source
size, and destination size. Current SFIV investigation notes expect the valid
1280x720 source lane to stay clean before async acquire or preacquire policies are
retuned.

### 8.1 Frame Latency Token

Present pacing is based on a queue-owned frame token:

```mermaid
stateDiagram-v2
    [*] --> Recorded : PresentCommand appended
    Recorded --> Submitted : commitChunk assigns frameToken
    Submitted --> Encoding : encode thread dequeues chunk
    Encoding --> Presented : presentDrawable encoded and committed
    Presented --> Completed : finish thread observes command buffer completion
    Completed --> [*] : frameLatencyFence signals frameToken

    note right of Submitted
        The application may wait for
        frameToken - maxFrameLatency
        when the boundary policy applies.
    end note
```

The token is not the chunk dequeue ID and not the point where the encode thread starts
work. It becomes signaled only after the Metal command buffer carrying the present has
completed. This mirrors the useful part of upstream DXMT's `CommandQueue` frame
latency fence while keeping drawable and layer ownership inside the presenter.

---

## 9. Dependency Tracking

Consecutive encoders may have data dependencies. The backend tracks resource access
per encoder with exact buffer/texture handle sets derived from imported record
hazards and current draw bindings.

For each encoder transition, the backend checks:
- `new_read ∩ prev_write` - read-after-write: must synchronize
- `new_write ∩ prev_write` - write-after-write: must synchronize
- `new_write ∩ prev_read` - write-after-read: must synchronize

If an exact set intersection exists, a Metal resource barrier or encoder split is
emitted. Bloom/probabilistic overlap checks may remain as counters to measure what
the old approximation would have done, but they must not decide default pass splits
because false positives create avoidable render-pass churn.

### 9.1 Exact vs Bloom — Why Exact Is the Default

DXMT uses Bloom filters (`PartitionedBloomFilter64<16>` × {buf,tex} × {r,w})
for hazard tracking. dxmt9 uses exact handle sets. This is a deliberate
divergence with a measurable cost/benefit balance, written down here so a
future reader does not relitigate it without the underlying numbers.

```mermaid
flowchart LR
    subgraph BLOOM["Pure Bloom (DXMT)"]
        BCheck["O(1) hash check"]
        BFP["false positives → unnecessary splits"]
        BFP -->|"on TBDR"| BFlush["each spurious split = tile flush\n~10–200μs"]
        BCheck -->|"~k hash ops"| BFast["per-check cost: low"]
    end
    subgraph EXACT["Pure exact (dxmt9 default)"]
        ECheck["O(n) set membership"]
        EZero["zero false positives"]
        EZero -->|"on TBDR"| ENoFlush["no spurious tile flushes"]
        ECheck -->|"n = 5–20 typical"| EFast["per-check cost: 5–50ns ≈ negligible"]
    end
    subgraph HYBRID["Bloom-pre-filter + exact (option, allowed by R-BACK-2.28)"]
        HBloom["Bloom 'definitely no'\n→ skip exact"]
        HExact["Bloom 'maybe' → exact"]
        HBloom --> HFast["common case: O(1)"]
        HExact --> HCorrect["uncommon case: exact ground truth"]
    end
```

| Axis | Pure Bloom | Pure exact (current) | Hybrid (Bloom→exact) |
|---|---|---|---|
| Per-check CPU | O(k) hash ops | O(n), n ≈ 5–20 typical → 5–50ns | O(k) common, O(n) rare |
| Memory per encoder | ~32 B fixed | grows with handle count | ~32 B + handle set |
| False-positive splits | yes (workload-dependent) | never | never (Bloom only filters "definitely no") |
| TBDR cost from FP | 0.05–4 ms/frame typical | 0 | 0 |
| Debuggability | "split happened, unknown reason" | "split caused by handle X" | "split caused by handle X" |
| Determinism | hash-function-dependent | yes | yes |
| Counter signal value | split count is noisy | split count = clean regression signal | split count = clean regression signal |

The decision rests on three observations:

1. **n is small in D3D9 workloads.** Encoders typically accumulate 5–20
   resource handles. O(n) membership at that scale is 5–50ns per check —
   negligible compared to the 10–200μs cost of one spurious tile flush
   on Apple Silicon TBDR. The CPU "saving" Bloom offers is small and the
   GPU cost it can introduce is not.

2. **"Encoder split count" is a regression signal.** §7 of `archicture/design.md`
   declares it. Bloom false positives turn that signal into noise. The whole
   architecture leans on clean signals to detect drift; weakening this one
   propagates uncertainty into benchmark interpretation.

3. **Debugging.** When an encoder splits, the question "why" must be
   answerable from the recorded state. Exact sets answer it; Bloom does not.

### 9.2 Bloom as Diagnostic and Optional Pre-Filter

`R-BACK-2.28` allows Bloom in two roles:

- **Diagnostic counter** (always permitted): maintain a Bloom in parallel
  with the exact set; record `bloomFalsePositiveCount` whenever the Bloom
  signaled "maybe" but the exact check found no overlap. This produces
  empirical FP-rate data without affecting split decisions.
- **Pre-filter** (permitted, not currently implemented): a "definitely no"
  Bloom result short-circuits the exact membership lookup. A "maybe" result
  falls through to exact. Splits are still decided by exact; Bloom never
  forces one. This preserves the current contract while collapsing the
  common-case CPU cost to O(1).

The pre-filter is a benchmark-driven addition. It is not in the current
implementation because the exact membership cost has not been shown to be
hot. If profiling on a real workload puts membership lookups in the top
encode-thread costs, the pre-filter becomes the next step. Until then it is
implementation complexity (two synchronized data structures, bimodal
latency) without a measured win.

### 9.3 What We Lose by Not Going Pure Bloom

For completeness, the costs we accept by sticking with exact:

- Encoder memory grows with handle count rather than staying bounded at
  ~32 B. For typical D3D9 encoders this is a few hundred bytes, not pages.
- The membership lookup is O(n) rather than O(k). At n ≤ 20 this is below
  measurement noise on the encode thread; at hypothetical n ≥ 200 it would
  start to matter, and the §9.2 pre-filter becomes the response.

These costs are accepted because the corresponding wins (zero FP, clean
regression signal, debuggability, determinism) are visible in everyday
operation, while the costs are bounded by the small-n regime D3D9 actually
exhibits.

---

## 10. Fixed-Function Lighting Constants Layout

The fixed-function vertex shader receives a uniform buffer with this layout:

```
FFPVertexUniforms {
    float4x4  worldViewMatrix       // D3DTS_WORLD × D3DTS_VIEW
    float4x4  projMatrix            // D3DTS_PROJECTION
    float3x3  normalMatrix          // inverse transpose of worldViewMatrix upper 3×3
    float4x4  textureMatrix[8]      // D3DTS_TEXTURE0–7
    float2    invViewportSize       // (1/width, 1/height) for half-pixel fixup

    struct Light {
        float4  diffuse
        float4  specular
        float4  ambient
        float4  position_vs         // in view space
        float4  direction_vs        // in view space, normalized
        float   atten0, atten1, atten2
        float   range
        float   theta, phi, falloff // spot params
    } lights[8]

    float4    globalAmbient
    float4    materialDiffuse
    float4    materialAmbient
    float4    materialSpecular
    float4    materialEmissive
    float     materialSpecularPower

    float4    fogColor
    float     fogStart, fogEnd, fogDensity
}
```

---

## 11. Clip Plane Design

D3D9 supports up to 6 user clip planes (`D3DRS_CLIPPLANEENABLE` bitmask,
`SetClipPlane(index, float[4])`). Metal surfaces these as vertex shader
`[[clip_distance]]` outputs.

### Uniform layout

Clip planes are stored in the fixed-function uniform buffer and also injected into
the programmable shader constant block:

```
// Appended to FFPVertexUniforms when any clip plane is enabled:
float4  clipPlane[6]   // in clip space (post-projection)
```

D3D9 clip planes are specified in world space. The core must transform them to
clip space before passing to the backend:

```
clipPlane_clip[i] = transpose(inverse(worldViewProj)) * clipPlane_world[i]
```

### Vertex shader emission

When `clipPlaneMask != 0`, the vertex shader must output `[[clip_distance]]` values.

**For fixed-function shaders:** the backend generates code in the FFP vertex shader:
```metal
vertex VSOut ff_vertex(...) {
    VSOut out;
    // ... standard transform ...
    out.position = projPos;
    out.clipDist[0] = dot(projPos, uniforms.clipPlane[0]);  // if bit 0 set
    out.clipDist[1] = dot(projPos, uniforms.clipPlane[1]);  // if bit 1 set
    // ... up to 6
    return out;
}
```

**For programmable shaders:** the translator injects a post-transform epilog after
the shader's `oPos` write. The clip plane values are passed as additional float4
constants above the normal constant register range (or via a separate small buffer).

### `[[clip_distance]]` declaration in MSL

```metal
struct VertexOut {
    float4 position [[position]];
    float  clipDist [[clip_distance]] [6];
    // ... other varyings
};
```

Only the enabled clip planes (per `clipPlaneMask`) need to be written; others may be
zero. Metal clips the primitive when any `clipDist[i] < 0`.

### Variant key

`clipPlaneMask` (6-bit) is part of the vertex shader variant key. Shaders compiled
without clip planes must not be reused for draws with clip planes active.

---

## 12. MSAA Design

D3D9 `D3DPRESENT_PARAMETERS.MultiSampleType` and `CreateRenderTarget` with
`MultiSample` parameter enable multisample antialiasing.

### Supported sample counts

Report the following in `CheckDeviceMultiSampleType()`:
- `D3DMULTISAMPLE_NONE` (1×) — always supported
- `D3DMULTISAMPLE_2_SAMPLES` (2×) — supported if `[device supports:MTLSampleCount2]`
- `D3DMULTISAMPLE_4_SAMPLES` (4×) — supported if `[device supports:MTLSampleCount4]`
- `D3DMULTISAMPLE_8_SAMPLES` (8×) — optional; check `MTLSampleCount8`

### Multisample render target

A multisample render target is a `MTLTexture` with `sampleCount > 1` and
`textureType = MTLTextureType2DMultisample`. It cannot be sampled directly.

The corresponding `MTLRenderPassDescriptor` attachment:
```objc
att.texture     = msaaTexture          // multisample texture
att.storeAction = MTLStoreActionMultisampleResolve
att.resolveTexture = resolveTexture    // single-sample resolve target
```

### Resolve target

Each multisample render target has an associated single-sample **resolve texture**
(`MTLTexture` with `sampleCount = 1`). The resolve texture is what the application
samples when it uses the render target as a texture.

```
MsaaRenderTarget {
    MTLTexture  msaaTex      // sampleCount = N, MTLStorageModePrivate
    MTLTexture  resolveTex   // sampleCount = 1, MTLStorageModePrivate
}
```

When the render pass closes (`endEncoding`):
- `storeAction = MTLStoreActionMultisampleResolve` automatically resolves `msaaTex`
  → `resolveTex`.
- Subsequent `SetTexture()` calls bind `resolveTex` for sampling.

### `GetRenderTargetData` on MSAA surface

Must resolve first (if not already resolved), then read back `resolveTex` via staging
buffer. The application cannot directly access the multisample texture data.

---

## 13. Tile-Shader FFP (Apple Silicon Fast Path)

The tile-shader FFP path runs D3D9 fixed-function fragment effects (fog,
alpha test, alpha-to-coverage) at the Metal **tile stage** instead of the
fragment stage on `MTLGPUFamilyApple3+`. Tile-stage execution lives in
on-chip tile memory and has no fragment-shader dispatch cost; for FFP-heavy
D3D9 frames this collapses two passes (fragment fog + framebuffer write)
into one tile pass.

### 13.1 Selection Flow

```mermaid
flowchart TD
    PASS["render-pass desc built\n(attachments, FFPKeyPS, GPU family)"] --> CAP{"GPU family\n≥ Apple3?"}
    CAP -->|"no"| PORT["portable fragment FFP\n(unchanged path)"]
    CAP -->|"yes"| KEY{"FFPKeyPS uses\ntile-eligible state?\n(fog, alpha-test, A2C only)"}
    KEY -->|"no"| PORT
    KEY -->|"yes"| PRECISION{"reference values\nin tile-precision range?"}
    PRECISION -->|"no"| FALLBACK["fallback:\nportable path\nincrement tileFfpFallbackCount(reason)"]
    PRECISION -->|"yes"| TILE["tile-shader FFP path"]
    TILE --> CMP["select MSL with\ntile_ffp_mode=1 variant"]
    PORT --> CMP_P["select MSL with\ntile_ffp_mode=0 variant"]
    CMP --> ENC["encoder uses\nMTLTileRenderPipelineState"]
    CMP_P --> ENC2["encoder uses\nMTLRenderPipelineState (fragment)"]
```

Selection is per-render-pass (`R-BACK-13.1`). A pass that begins as tile-FFP
cannot mid-pass switch to portable; mismatched draws within the pass force
either a pass split or a fall-through to portable for the whole pass.

### 13.2 MSL Generation

Two distinct generators emit two distinct MSL sources from the same
`FFPKeyPS`:

| Path | Generator | Output | Pipeline state |
|---|---|---|---|
| Portable | `makeFfpPixelSource()` | fragment function | `MTLRenderPipelineState` |
| Tile | `makeFfpTilePixelSource()` (new) | tile function | `MTLTileRenderPipelineState` |

The tile generator emits programmable-blending tile code that reads
attachment color directly (Apple-Silicon-only feature) instead of writing
through `out.color = ...`. This avoids round-tripping through tile memory.

PSO key includes `tile_ffp_mode` bit (`R-BACK-13.3`); the same `FFPKeyPS`
produces two compiled function pairs, one in each cache.

### 13.3 Conformance Boundary

Tile-shader FFP must produce results bit-identical to the portable path
(`R-BACK-13.2`). Where Metal's tile stage cannot match D3D9 corner cases:

| Corner | Reason class | Fallback action |
|---|---|---|
| Alpha-test reference outside `[0, 255]` integer range | `precision` | mark pass portable |
| Fog mode with non-linear scale at high z | `precision` | mark pass portable |
| Programmable blend interaction with PS-emitted alpha-to-coverage | `unsupported_state` | mark pass portable |
| Non-Apple-Silicon device | `gpu_family` | always portable, no counter increment |

Conformance evidence (`R-BACK-13.5`) is taken from the portable path only;
the tile path is judged equivalent by bit-identity to portable per
`R-BACK-13.2`. Conformance regressions in the tile path manifest as
visible-pixel differences under shader-runner readback tests, which keep
running both paths and require equality.

### 13.4 Counters

| Counter | Meaning |
|---|---|
| `tileFfpPassCount` | render passes encoded via tile path |
| `portableFfpPassCount` | render passes encoded via portable path |
| `tileFfpFallbackCount` | passes that started tile-eligible but fell back |
| `tileFfpFallbackByReason{precision \| unsupported_state \| gpu_family \| mid_pass_ineligible}` | fallback breakdown |
| `tileFfpMidPassResplitCount` | passes split because a draw mid-pass became ineligible |
| `tileFfpPassMs` / `portableFfpPassMs` | encoding time per path (sanity check) |

A regression to the portable path on an Apple Silicon device is observable
as a drop in `tileFfpPassCount` ratio without a corresponding
`tileFfpFallbackCount` increase (i.e., the selector skipped tile path for an
unaccounted reason).

### 13.5 Metal API Model

The tile-shader FFP path uses Metal's tile-render pipeline, which is a
distinct compile target from the standard render pipeline.

```mermaid
flowchart LR
    SRC["makeFfpTilePixelSource(FFPKeyPS)\n→ MSL with kernel function\nusing imageblock + threadgroup"]
    SRC --> LIB["[device newLibraryWithSource:]"]
    LIB --> FN["id<MTLFunction> (tile kernel)"]
    FN --> DESC["MTLTileRenderPipelineDescriptor\n• tileFunction = FN\n• threadgroupSizeMatchesTileSize = YES\n• colorAttachments[i].pixelFormat"]
    DESC --> CMP["[device newRenderPipelineStateWithTileDescriptor:]"]
    CMP --> TPS["MTLRenderPipelineState (tile variant)\n— shares interface with fragment PSO\nbut compiled separately"]
```

Pipeline state objects:

| Object | Used by | Notes |
|---|---|---|
| `MTLRenderPipelineState` (fragment variant) | `setRenderPipelineState:` | the portable path; unchanged |
| `MTLRenderPipelineState` (tile variant) | `setRenderPipelineState:` then `dispatchThreadsPerTile:` | encoded via the tile descriptor; produces the same opaque type but compiled with `newRenderPipelineStateWithTileDescriptor:` |
| `MTLTileRenderPipelineDescriptor` | compile-time only | dxmt9 holds it briefly during compile, never at draw time |

Both variants are cached in the same `MTLRenderPipelineState` cache (§5),
keyed by the PSO key including `tile_ffp_mode` (`R-BACK-3.3`). Variant
selection happens at draw encode time via the bit in the key.

The MSL emitter (`makeFfpTilePixelSource`) writes a tile kernel that:

- declares an `imageblock<TileColor>` to read attachment color directly
  (Apple Silicon programmable-blending).
- expresses fog blend / alpha-test / A2C as an arithmetic + conditional
  pass over the imageblock value before write-back.
- does **not** call `discard_fragment()`; alpha-test rejection becomes a
  blend-with-prior-color of the rejected fragment (alpha-test on tile is
  semantically equivalent to discarding pre-blend).

### 13.6 Mid-Pass Eligibility Policy

`R-BACK-13.1` says tile vs portable is decided per render-pass. But pass
boundaries are decided by RT changes / explicit hazards — within one pass
a state mutation can change FFP eligibility (e.g., an alpha-test reference
flips from in-range to out-of-range, or a non-tile-supported fog mode is
selected mid-pass).

```mermaid
flowchart TD
    OPEN["pass opens with first draw\n→ selector chooses tile vs portable"] --> ENC["encoder open;\npipeline = chosen variant"]
    ENC --> NEXT{"next draw"}
    NEXT -->|"FFP key still tile-eligible"| ENC
    NEXT -->|"FFP key becomes ineligible"| FORK{"current path?"}
    FORK -->|"portable already"| ENC
    FORK -->|"tile"| SPLIT["end encoder + open new encoder\n(portable variant)\nincrement tileFfpMidPassResplitCount"]
    SPLIT --> ENC
```

Rules:

- A pass opened on the **portable** path stays portable even if subsequent
  draws would have been tile-eligible. Promoting mid-pass is not allowed
  because the portable encoder cannot retroactively become a tile encoder.
- A pass opened on the **tile** path that hits an ineligible draw must end
  the current encoder and start a new portable encoder. This is an
  encoder split, counted in `tileFfpMidPassResplitCount`. The split is
  treated as an exact-hazard split for `R-BACK-2.28` purposes (counter
  signal stays clean — false-positive splits remain forbidden).
- `tileFfpFallbackByReason{mid_pass_ineligible}` advances on the same
  event for the by-reason breakdown.

### 13.7 Floating-Point Precision Boundary

`R-BACK-13.2` requires "bit-identical to the portable fragment path." This
is achievable only when both paths use the same arithmetic precision.

Precision rules:

- Both paths emit MSL with `float` (32-bit IEEE 754) for fog, alpha-test,
  and A2C arithmetic — never `half`. Apple's MSL compiler may demote to
  `half` on TBDR by default for fragment shaders; the dxmt9 emitter
  **explicitly types the affected variables as `float`** to prevent the
  demotion.
- The tile-stage emitter additionally uses `imageblock<TileColor>` with
  `TileColor` defined as `float4` (not `half4`) for color attachments
  whose pixel format is wider than 8-bit-per-channel. For 8-bit
  attachments (`A8R8G8B8` etc.) `half4` is acceptable because the
  attachment quantization already discards precision below `half`.
- Fog blends are computed in linear space; sRGB attachments are decoded
  before the blend and re-encoded after, identically on both paths.
- A precision divergence that breaks bit-identity (e.g., a float
  rounding-mode quirk on a specific GPU family) must be encoded as a
  conformance test failure → `tileFfpFallbackByReason{precision}`
  increment + the affected pass falls back. The portable path remains
  the authoritative oracle.

A future Metal version that adds full programmable-blending precision
parity may relax these rules; until then, dxmt9's tile path opts into
`float` precision unconditionally for FFP arithmetic.

---

## 14. Resource Heap Pooling Design

Backs the `MTLHeap` requirements in §7.3, `R-BACK-5.9`, `R-BACK-5.10`, and
§14 of `requirements.md`. The goal is to reduce per-resource residency and
allocation overhead for D3D9's small-texture working set (lightmaps, decals,
glyph atlases, particle sprites — typically ≤ 64 KB each, hundreds to
thousands per frame).

### 14.1 Heap Family Lifecycle

```mermaid
stateDiagram-v2
    [*] --> Available: createHeap(family, storageMode, capacity)
    Available --> Filling: makeBuffer / makeTexture from heap
    Filling --> Filling: more allocations within capacity
    Filling --> Full: capacity reached
    Full --> Growing: new heap created (geometric growth)
    Growing --> Filling: subsequent allocations target new heap
    Filling --> Reclaiming: all members destroyed,\ncompletedSeqId ≥ lastUsedSeqId
    Full --> Reclaiming: same condition
    Reclaiming --> [*]: heap released
```

Geometric growth (`R-BACK-5.10`) caps total heap memory by capping the total
heap count rather than per-heap size. Heaps grow doubling up to a soft
ceiling (`heapMaxBytesPerFamily`); past the ceiling, new heaps allocate at
the ceiling size.

### 14.2 Allocation Path

```mermaid
sequenceDiagram
    participant App as Application
    participant Be as Backend createTexture
    participant H as Heap manager
    participant MTL as Metal

    App->>Be: createTexture(W, H, fmt, pool=DEFAULT)
    Be->>Be: estimate footprint(desc)
    alt footprint > threshold OR ineligible usage
        Be->>MTL: newTextureWithDescriptor (direct)
        MTL-->>Be: MTLTexture
    else footprint ≤ threshold AND eligible
        Be->>H: alloc(family=priv-tex, desc)
        alt heap has capacity
            H->>MTL: heap.makeTexture(desc)
        else no capacity
            H->>MTL: newHeapWithDescriptor (grow)
            MTL-->>H: new MTLHeap
            H->>MTL: heap.makeTexture(desc)
        end
        MTL-->>H: MTLTexture (heap-backed)
        H-->>Be: MTLTexture + heap-backed flag
    end
    Be-->>App: texture handle
```

### 14.3 Residency

Direct allocations call `useResource:usage:` per resource per encoder
(when needed). Heap-backed allocations call `useHeap:` once per heap per
encoder. For frames with hundreds of small textures, this collapses
hundreds of `useResource:` calls into a handful of `useHeap:` calls.

The encoder maintains a per-encoder `usedHeaps` set; `useHeap:` is issued
the first time any member of that heap is bound, and per-resource residency
calls on heap members are elided.

### 14.4 Counters

| Counter | Meaning |
|---|---|
| `heap.{family}.heapCount` | live heap instances for the family |
| `heap.{family}.bytesAllocated` | total backing storage |
| `heap.{family}.allocCount` | resources allocated from this family |
| `heap.{family}.directFallbackCount` | eligible-by-size resources that took direct path due to a usage-flag mismatch |
| `heap.{family}.fragmentationFailureCount` | `heap.makeTexture` returned nil despite heap not being "full" by accounting |
| `heap.{family}.compactionCount` | how many times a heap was retired and freed because all members were destroyed |
| `useHeapPerEncoder` | average count per encoder (target: small) |
| `useResourcePerEncoder` | average count per encoder (target: dropping after heap adoption) |

### 14.5 Fragmentation Policy

A `MTLHeap` is a contiguous backing region; allocations and frees create
holes. Even when `usedSize < currentAllocatedSize`, a fresh
`heap.makeTexture(desc)` can return `nil` because the largest free hole is
smaller than the requested footprint.

```mermaid
flowchart TD
    REQ["heap.alloc(family, desc)"] --> TRY{"heap.makeTexture()"}
    TRY -->|"non-nil"| OK["return heap-backed texture"]
    TRY -->|"nil & heap usedSize < cap"| FRAG["fragmentation:\nlargest free hole < desc footprint"]
    FRAG --> NEXT{"another heap in family\nwith capacity?"}
    NEXT -->|"yes, try next"| TRY
    NEXT -->|"no"| GROW["allocate new heap (geometric)"]
    GROW --> ASSIGN["new alloc targets fresh heap"]
    TRY -->|"nil & heap full by accounting"| GROW
```

Rules:

- **No mid-heap compaction.** Metal does not expose a heap-compact API and
  resources cannot be relocated once allocated. Fragmentation is absorbed
  by adding a new heap to the family, never by moving existing allocations.
- **Failed allocation walks the family.** If the first heap returns nil,
  the next eligible heap in the family is tried before allocating a new
  heap. A bookkeeping `usedSize` per heap is a hint, not a hard predictor
  of `makeTexture` success — always probe.
- **`fragmentationFailureCount` advances** when `heap.makeTexture` returns
  nil but `usedSize < currentAllocatedSize`. Sustained growth indicates the
  family's allocation pattern produces too many holes; tune the threshold
  or add a per-family eviction policy as a follow-up.
- **Compaction by retirement** is the only reclaim mechanism. When all
  members of a heap are destroyed and `completedSeqId ≥ lastUsedSeqId` for
  the heap (`R-BACK-14.4`), the heap is released. Subsequent allocations
  do not re-target the released heap; they go to a remaining heap or grow
  a new one. `compactionCount` advances per release.

### 14.6 Allocation Failure Beyond Family Growth

If creating a new heap (`newHeapWithDescriptor:`) itself fails — typically
out-of-memory at the device level — the heap manager falls through to
direct allocation (`R-BACK-5.9`'s "fall through to direct allocation"
clause). This is recorded as `heap.{family}.directFallbackCount` and
`heap.{family}.heapAllocFailureCount`. Subsequent allocations retry heap
allocation; the manager does not give up after one failure because the
underlying memory pressure may be transient.

If direct allocation also fails, the backend returns
`D3DERR_OUTOFVIDEOMEMORY` to the D3D9 caller per the existing
out-of-memory contract. There is no "swap to system memory" fallback —
that is the D3D9 application's responsibility via pool selection.

A regression manifests as `useResourcePerEncoder` rising on workloads
previously dominated by heap allocation, or
`heap.{family}.directFallbackCount` spiking when an eligibility heuristic
becomes overly conservative.
