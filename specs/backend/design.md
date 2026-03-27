# Backend Design

---

## 1. Module Structure

```mermaid
graph TD
    subgraph PE["Win32 / PE side (Wine)"]
        IFACE["BackendDevice interface\n(called by core)"]
        THUNKS["winemetal thunks\n(Win32 → unix lib FFI)"]
        SHADER_THUNK["shader compiler thunks\n(D3DBC → SPIR-V → MSL)"]
    end

    subgraph Unix["macOS / unix lib side"]
        CQ["CommandQueue\n(32-slot ring, 3 threads)"]
        AEC["ArgumentEncodingContext\n(encode thread)"]
        ALLOC["Ring allocators\n(argbuf, lambda, staging, copy-temp)"]
        PSO["PSOCache\n(MTLRenderPipelineState)\n+ DSS cache"]
        SHAD["ShaderCache\n(compiled MSL → MTLLibrary)\nDisk: MTLBinaryArchive"]
        TRANS["Shader translator\nvkd3d-shader + SPIRV-Cross\nOR direct D3DBC→MSL"]
        PRES["Presenter\n(CAMetalLayer blit)"]
        RESALLOC["ResourceAllocator\n(MTLBuffer, MTLTexture)"]
    end

    IFACE --> THUNKS
    THUNKS --> CQ
    THUNKS --> RESALLOC
    SHADER_THUNK --> TRANS
    TRANS --> SHAD
    CQ --> AEC
    CQ --> ALLOC
    AEC --> PSO
    AEC --> SHAD
    AEC --> PRES
```

---

## 2. Command Queue

The command queue is the central coordinator. It decouples the Wine thread (which
calls the backend interface) from Metal encoding (which calls Metal API).

```mermaid
graph LR
    subgraph Ring["32-slot CommandChunk ring"]
        W["Write slot\n(Wine thread)"]
        E["Encode slot\n(encode thread)"]
        G["GPU-running slot"]
        F["Free slots"]
    end

    subgraph Threads
        WT["Wine thread\nwrites lambdas"]
        ET["Encode thread\nreplays lambdas → MTLCommandBuffer"]
        FT["Finish thread\nwaits GPU done → releases chunk"]
    end

    WT -->|emit lambda| W
    W -->|commit chunk| E
    ET -->|MTLCommandBuffer.commit| G
    G -->|GPU completion| FT
    FT -->|reset chunk| F
```

**CommandChunk** holds:
- A linked list of closures (lambdas) emitted by the Wine thread
- Four ring sub-allocators: `staging` (CPU-visible readback), `copy_temp` (GPU private
  blit), `argbuf` (argument buffers), `lambda_store` (closure heap)
- A sequence ID used to determine when in-flight resources can be released

**Submission flow:**

1. Wine thread calls `submitDraw(DrawDesc)`.
2. Backend emits a closure into the current chunk's lambda list. The closure captures
   resource handles (not COM pointers) and all state from `DrawDesc`.
3. On `present()` or when the chunk is full, the Wine thread commits the current chunk.
4. The encode thread dequeues the chunk and replays all closures against
   `ArgumentEncodingContext`, producing Metal commands.
5. The encode thread commits the `MTLCommandBuffer`.
6. The finish thread waits for GPU completion and resets the chunk (releasing handles,
   returning ring allocator memory).

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
    participant WT as Wine thread
    participant CQ as CommandQueue
    participant ET as Encode thread
    participant CL as CAMetalLayer
    participant MTL as Metal

    WT->>CQ: present(SwapDesc)
    CQ->>CQ: emit blit closure into chunk\ncommit chunk

    ET->>CL: nextDrawable (blocks on vsync if enabled)
    ET->>MTL: blitCommandEncoder: copy backbuffer → drawable.texture
    ET->>MTL: commandBuffer.presentDrawable(drawable)
    ET->>MTL: commandBuffer.commit()
```

The back buffer is a private `MTLTexture` owned by the swap chain. On present, it
is copied to the `CAMetalLayer` drawable via a blit encoder. The drawable is obtained
just before the blit — not before the frame — to minimize latency.

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
