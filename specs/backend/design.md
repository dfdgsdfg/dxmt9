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
        ALLOC["Ring allocators\n(argbuf, lambda, staging, copy-temp)"]
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
threads. dxmt9 differs only in the bridge payload format: D3D9 uses POD records
instead of C++ lambda captures because records cross the Wine PE/unix boundary.

```mermaid
flowchart TD
    subgraph DXMT["Upstream DXMT D3D11 shape"]
        DXCtx["D3D11 immediate context\nEmitST / EmitOP"]
        DXChunk["CommandChunk\nlambda command list"]
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

The alignment requirement is about ownership and timing, not source-level identity:

- PE owns D3D API semantics, state shadowing, getters, state blocks, and command
  packet construction.
- The Wine bridge owns ABI marshalling only.
- The unix importer owns packet validation and handle retention.
- `CommandQueue` owns chunk execution, Metal command-buffer lifetime, completion,
  and frame-latency token signaling.
- `Presenter` owns drawable acquisition and `presentDrawable` encoding.

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
        ET["Encode thread\nreplays records/closures → MTLCommandBuffer"]
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
- Imported command records or decoded closures owned by the unix side
- Four ring sub-allocators: `staging` (CPU-visible readback), `copy_temp` (GPU private
  blit), `argbuf` (argument buffers), `lambda_store` (closure heap)
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

---

## 3. Encoder Lifecycle

```mermaid
flowchart TD
    A["Backend receives DrawDesc"] --> B{Active encoder type\n== Render?}
    B -->|No| NEWPASS["End current encoder\nCreate new RenderEncoderData\nwith attachments from DrawDesc.rts"]
    B -->|Yes| C{Same attachments\nas active encoder?}
    C -->|No| NEWPASS
    C -->|Yes| D{Hazard check:\nBloom filter on\nbuf/tex read-write sets}
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

---

## 4. Argument Buffer Binding

All shader resources for a draw call are packed into a single GPU-side argument
buffer. The shader accesses all inputs through this one buffer pointer.

```
Argument Buffer layout (per draw):
┌────────────────────────────────────┐
│ Vertex buffer slots                │  {gpu_address, stride, size}[] × 16
│ VS constant buffer (256 × float4)  │  or gpu_address if >4 KB
│ PS constant buffer (224 × float4)  │
│ VS samplers                        │  handle[] × 16
│ PS samplers                        │  handle[] × 16
│ VS textures                        │  handle[] × 16
│ PS textures                        │  handle[] × 16
└────────────────────────────────────┘
```

The argument buffer is suballocated from the `argbuf` ring allocator per draw call.
The Metal encoder receives one `setVertexBufferOffset` / `setFragmentBufferOffset`
call pointing to the base of this buffer.

---

## 5. PSO Cache

```mermaid
graph TD
    DD["DrawDesc"] --> KEY["PSO key extraction:\n• VS function (shader hash + variant)\n• FS function (shader hash + variant)\n• Vertex descriptor layout\n• RT pixel formats × 4\n• Blend state per attachment\n• Sample count\n• Alpha-to-coverage flag"]
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

---

## 6. Shader Translation

```mermaid
flowchart LR
    BC["D3DBC bytecode\n(SM 1.x / 2.0 / 3.0)"]
    FFP["FFPKey\n(VS or PS)"]

    BC --> PRE["Preprocessing:\n1. Half-pixel offset injection (VS)\n2. Version token parse\n3. TRIANGLEFAN already gone (core)"]
    PRE --> VKD3D["vkd3d_shader_compile()\nsource: D3D_BYTECODE\ntarget: SPIRV_BINARY"]
    VKD3D --> SPIRVCROSS["spirv_cross::CompilerMSL\n→ MSL source string"]
    FFP --> FFGEN["FFP shader generator\n→ MSL source string directly"]
    SPIRVCROSS --> COMPILE["[device newLibraryWithSource:]\nor precompiled .metallib"]
    FFGEN --> COMPILE
    COMPILE --> DISKCACHE["Disk cache\nMTLBinaryArchive\nkey: SHA-1(bytecode + variant)"]
    DISKCACHE --> FN["id<MTLFunction>"]
```

**Shader variant keys** for programmable shaders:
- Vertex: (bytecode_hash, input_layout_hash, rasterization_disabled)
- Pixel: (bytecode_hash, alpha_test_enable, alpha_test_func, fog_mode, clip_planes_mask)

Two draws with the same bytecode but different variant keys require separate
compiled `MTLFunction` objects.

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
1. Application `Lock()`s the texture surface and writes pixel data.
2. `Unlock()` marks the surface dirty.
3. On first draw that samples the texture, a blit encoder copies the dirty region
   from the CPU-accessible buffer to the private `MTLTexture`.
4. The blit is fenced so the render encoder that follows reads the updated data.

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
    CQ-->>BR: frameToken
    BR-->>Rec: frameToken
    Rec->>BR: waitFrameLatency(frameToken, maxLatency)

    ET->>PR: encode PresentCommand(frameToken)
    PR->>CL: nextDrawable\n(blocks when drawable/vsync-limited)
    ET->>MTL: blitCommandEncoder: copy backbuffer → drawable.texture
    ET->>MTL: commandBuffer.presentDrawable(drawable)
    ET->>MTL: commandBuffer.commit()
    FT->>CQ: signal frameToken\non command buffer completion
    CQ-->>BR: latency wait satisfied
    BR-->>App: Present returns
```

The back buffer is a private `MTLTexture` owned by the swap chain. On present, it
is copied to the `CAMetalLayer` drawable via a blit encoder. The drawable is obtained
just before the blit — not before the frame — to minimize latency.

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
        frameToken - maxFrameLatency.
    end note
```

The token is not the chunk dequeue ID and not the point where the encode thread starts
work. It becomes signaled only after the Metal command buffer carrying the present has
completed. This mirrors the useful part of upstream DXMT's `CommandQueue` frame
latency fence while keeping drawable and layer ownership inside the presenter.

---

## 9. Dependency Tracking

Consecutive encoders may have data dependencies. The backend tracks resource access
per encoder using partitioned Bloom filters on buffer and texture handles.

For each encoder transition, the backend checks:
- `new_read ∩ prev_write` — read-after-write: must synchronize
- `new_write ∩ prev_write` — write-after-write: must synchronize
- `new_write ∩ prev_read` — write-after-read: must synchronize

If the filter indicates a possible conflict (Bloom may have false positives), a Metal
resource barrier or encoder split is emitted. False positives result in unnecessary
barriers (safe); false negatives are prevented by the over-approximation property of
the Bloom filter.

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
