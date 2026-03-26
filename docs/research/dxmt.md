# DXMT Architecture Research Notes

Sources: github.com/3Shain/dxmt (source inspection), deepwiki.com/3Shain/dxmt.

---

## Overview

DXMT is a Metal-based translation layer for Direct3D 11 and Direct3D 10 on macOS, designed
to run Windows 3D applications through Wine. It is a native C++ project — not a browser
wrapper — and its architecture closely parallels the problem space of dxmt9.

The implementation is split across five major source modules: `d3d11/` (COM-facing API),
`dxmt/` (Metal translation core), `airconv/` (shader compiler), `winemetal/` (Wine bridge),
and `dxgi/` (DXGI factory/adapter).

---

## Module Dependency Graph

```mermaid
graph TD
    subgraph Win32["Win32 / PE side (runs in Wine)"]
        D3D11["d3d11/\nID3D11Device, ID3D11DeviceContext\nCOM implementations"]
        DXGI["dxgi/\nIDXGIFactory, IDXGIAdapter\nIDXGISwapChain"]
        D3D10["d3d10/\nshim delegating to d3d11/"]
        NVAPI["nvapi/ nvngx/\nstub DLLs"]
    end

    subgraph Core["Core translation layer"]
        DXMT["dxmt/\nArgumentEncodingContext\nCommandQueue, CommandChunk\nBuffer, Texture, Dynamic, Staging"]
        AIRCONV["airconv/\nDXBC → Apple AIR (LLVM IR)\nSM50Initialize / SM50Compile"]
        UTIL["util/\nRc&lt;T&gt;, logging, config\nthreading, SHA-1"]
    end

    subgraph Bridge["Wine ↔ macOS bridge"]
        WM["winemetal/\nC API surface (winemetal.h)\nThunks: Win32 → unix lib"]
        NATIVE["nativemetal/\nmacOS-native Metal impl\nWMT:: C++ wrappers"]
    end

    D3D11 --> DXMT
    D3D11 --> AIRCONV
    D3D11 --> WM
    DXGI --> WM
    DXMT --> WM
    AIRCONV --> WM
    WM --> NATIVE
    NATIVE -->|"Metal framework\nObjective-C"| GPU[(GPU)]
    D3D11 --> UTIL
    DXMT --> UTIL
```

---

## Source Directory Structure

```
src/
├── airconv/          DXBC → AIR (Apple Intermediate Representation) shader compiler
│   ├── dxbc_converter.{cpp,hpp}         Core DXBC instruction converter
│   ├── dxbc_converter_cfg.cpp           Control flow graph (LOOP/IF/CALL)
│   ├── dxbc_converter_gs.cpp            Geometry shader → Metal mesh shader
│   ├── dxbc_converter_ts.cpp            Tessellation shader conversion
│   ├── air_operations.{cpp,hpp}         AIR (LLVM IR) emitters
│   ├── air_signature.{cpp,hpp}          Function signature / argument table
│   ├── airconv_public.h                 Public C API (SM50Initialize, SM50Compile)
│   ├── metallib_writer.{cpp,hpp}        .metallib binary writer
│   └── darwin/ nt/                      Platform-specific AIR builder variants
│
├── d3d11/            D3D11 COM interface implementation
│   ├── d3d11_device.{cpp,hpp}           ID3D11Device5 implementation
│   ├── d3d11_context_imm.cpp            Immediate context (all Draw/Map/Clear calls)
│   ├── d3d11_context_state.hpp          D3D11ContextState (all pipeline stage state)
│   ├── d3d11_pipeline.{cpp,hpp}         Pipeline descriptor + compiled PSO types
│   ├── d3d11_pipeline_cache.{cpp,hpp}   PSO cache (hash map keyed by descriptor)
│   ├── d3d11_shader.{cpp,hpp}           Shader lifecycle + variant system
│   ├── d3d11_texture.{cpp,hpp}          ID3D11Texture1/2/3D implementations
│   ├── d3d11_buffer.cpp                 ID3D11Buffer implementation
│   ├── d3d11_view.hpp                   SRV / RTV / DSV / UAV wrappers
│   ├── d3d11_state_object.{cpp,hpp}     Blend / DS / Rasterizer / Sampler state
│   ├── d3d11_input_layout.{cpp,hpp}     Input layout
│   └── d3d11_swapchain.{cpp,hpp}        IDXGISwapChain4 implementation
│
├── dxmt/             Core Metal translation layer
│   ├── dxmt_device.{cpp,hpp}            Device wrapper (owns MTLDevice + CommandQueue)
│   ├── dxmt_context.{cpp,hpp}           ArgumentEncodingContext (encode thread)
│   ├── dxmt_command.{cpp,hpp,metal}     Command structs + Metal-side replay
│   ├── dxmt_command_list.hpp            CommandList<Context> lambda linked list
│   ├── dxmt_command_queue.{cpp,hpp}     CommandQueue (32-slot ring, 3 threads)
│   ├── dxmt_buffer.{cpp,hpp}            Buffer + BufferAllocation + BufferView
│   ├── dxmt_texture.{cpp,hpp}           Texture + TextureAllocation + TextureView
│   ├── dxmt_dynamic.{cpp,hpp}           Triple-buffering for USAGE_DYNAMIC resources
│   ├── dxmt_staging.{cpp,hpp}           StagingResource (CPU-readable)
│   ├── dxmt_format.{cpp,hpp}            DXGI → MTLPixelFormat conversion
│   ├── dxmt_residency.hpp               Per-encoder resource residency tracking
│   ├── dxmt_deptrack.hpp                Bloom-filter dependency tracking
│   ├── dxmt_ring_bump_allocator.hpp     Ring sub-allocators (4 types)
│   ├── dxmt_presenter.{cpp,hpp}         CAMetalLayer blit + present
│   ├── dxmt_shader_cache.{cpp,hpp}      MTLBinaryArchive disk shader cache
│   └── dxmt_pipeline_cache.hpp          In-memory PSO cache
│
├── winemetal/        Wine ↔ macOS bridge
│   ├── winemetal.h                      C API surface for Win32 side
│   ├── Metal.hpp                        C++ wrappers (WMT:: namespace)
│   ├── winemetal_thunks.{c,h}           Win32 DLL → unix lib thunks
│   ├── airconv_thunks.{c,h}             Shader compiler cross-process calls
│   └── unix/winemetal_unix.c            macOS-native implementations
│
├── dxgi/             DXGI factory/adapter/output COM implementation
├── d3d10/            D3D10 shim (delegates to d3d11/)
├── nvapi/ nvngx/     NVIDIA API stubs
└── util/             Rc<T>, logging, config, SHA-1, thread, bloom filter
```

---

## Command Recording and Encoding Pipeline

The central design insight: the main (Wine) thread never touches Metal objects. All Metal
API calls happen on a dedicated encode thread. Communication uses a ring of command
chunks, each holding a linked list of captured C++ lambdas.

```mermaid
sequenceDiagram
    participant App as Application
    participant Ctx as ImmediateContext<br/>(Wine thread)
    participant CQ as CommandQueue<br/>(32-chunk ring)
    participant ET as Encode Thread
    participant AEC as ArgumentEncodingContext
    participant MTL as Metal API

    App->>Ctx: DrawIndexedPrimitive(...)
    activate Ctx
    Ctx->>Ctx: Update D3D11ContextState<br/>(in-memory, no Metal objects)
    Ctx->>CQ: EmitOP(lambda capturing Rc<Buffer>/Rc<Texture>)
    note over CQ: placement-new lambda into<br/>cpu_command_allocator ring<br/>→ linked into CommandChunk::list_enc
    deactivate Ctx

    App->>Ctx: Present()
    Ctx->>CQ: CommitCurrentChunk()

    activate ET
    ET->>CQ: dequeue CommandChunk
    ET->>AEC: chunk.encode(cmdbuf, enc)
    activate AEC

    loop for each lambda in chunk
        AEC->>AEC: startRenderPass(attachments)<br/>check merge with prev encoder
        AEC->>AEC: encodeRenderCommand wmtcmd_setpso
        AEC->>AEC: encodeVertexBuffers(slot_mask)
        AEC->>AEC: encodeRenderCommand wmtcmd_drawindexed
    end

    AEC->>MTL: flushCommands(cmdbuf)<br/>walk encoder list → MTLRenderCommandEncoder
    deactivate AEC

    MTL->>MTL: cmdbuf.commit()
    deactivate ET

    MTL-->>ET: completion handler (GPU done)
    ET->>CQ: chunk.reset() — destruct lambdas, release Rc<>
```

---

## Command Queue Ring Architecture

```mermaid
graph LR
    subgraph Ring["CommandQueue — 32-slot chunk ring"]
        C0["Chunk 0\n(writing)"]
        C1["Chunk 1\n(encoding)"]
        C2["Chunk 2\n(GPU running)"]
        C3["Chunks 3–31\n(free)"]
    end

    subgraph Allocators["Per-chunk ring allocators"]
        SA["staging_allocator\nCPU-visible readback"]
        CA["copy_temp_allocator\nGPU private blits"]
        AB["argbuf_allocator\nargument buffers, 4 MB chunks"]
        LA["cpu_command_allocator\nlambda storage, 4 MB chunks"]
    end

    subgraph Threads["Three threads"]
        MT["Main thread\nwrites via emitcc()"]
        ET["Encode thread\ndecodes → MTLCommandBuffer"]
        FT["Finish thread\nwaits GPU → signals CpuFence"]
    end

    MT -->|EmitOP / EmitST| C0
    C0 -->|CommitCurrentChunk| C1
    ET -.->|reads| C1
    C1 -->|cmdbuf.commit| C2
    C2 -->|completion handler| FT
    FT -->|chunk.reset| C3

    C0 --- LA
    C0 --- AB
```

---

## Encoder Merging and Dependency Tracking

DXMT avoids redundant Metal encoders by merging consecutive passes with identical
attachments and by folding `Clear()` into `MTLLoadActionClear`.

```mermaid
flowchart TD
    A["startRenderPass(attachments, argbuf_size)"] --> B{Previous encoder\nis also Render?}
    B -->|No| NEW["Create new RenderEncoderData\nappend to encoder linked list"]
    B -->|Yes| C{Same attachments\nRT + DS match?}
    C -->|No| NEW
    C -->|Yes| D{Dependency check\nbuf/tex read-write\nBloom filter}
    D -->|WAW or WAR conflict| BARRIER["Emit barrier\nor new encoder"]
    D -->|No conflict| E{Prev is ClearEncoder?}
    E -->|Yes| MERGE_CLEAR["Fold clear into\nloadAction=Clear\non new encoder"]
    E -->|No| MERGE["Merge: reuse\nsame RenderEncoderData"]

    style NEW fill:#f9f,stroke:#333
    style MERGE fill:#9f9,stroke:#333
    style MERGE_CLEAR fill:#9f9,stroke:#333
    style BARRIER fill:#ff9,stroke:#333
```

Each `EncoderData` carries four `PartitionedBloomFilter64<16>` sets:
`buf_read`, `buf_write`, `tex_read`, `tex_write`. `checkEncoderRelation()` uses these
filters to detect potential hazards across encoder boundaries — probabilistic but fast,
with false positives producing unnecessary barriers (safe) rather than missing hazards.

---

## Resource Lifetime Model

```mermaid
stateDiagram-v2
    [*] --> Alive: CreateBuffer / CreateTexture\n(COM + Rc created)
    Alive --> InFlight: lambda captures Rc copy
    InFlight --> Alive: lambda destructs\n(Rc count drops)
    Alive --> Renamed: DynamicBuffer::rename()\nMap DISCARD → swap allocation
    Renamed --> Alive: old Rc fully released\nby in-flight lambdas
    Alive --> [*]: COM Release() → Rc → 0\nMTLBuffer/MTLTexture freed

    note right of InFlight
        CommandChunk::AllocationRefTracking
        holds Allocation* list.
        GPU done → chunk.reset()
        → lambda destructors run
        → Rc refs drop
    end note
```

**Ownership layers:**
```
Application (COM AddRef/Release)
  └─ IDirect3DResource9 (COM object)
        └─ Rc<Buffer> / Rc<Texture>           D3D11 COM layer
              └─ Rc<BufferAllocation>          current physical MTLBuffer
                    └─ WMT::Buffer handle

Lambda in CommandChunk::list_enc
  └─ Rc<BufferAllocation>  (captured copy)    keeps allocation alive during GPU work
        └─ released when chunk.reset() runs after GPU completion
```

---

## Argument Buffer Binding Model

DXMT does not call `setBuffer`/`setTexture` per slot. All resources are packed into
one GPU-side argument buffer per render pass, and the shader receives one buffer pointer.

```
Argument Buffer (argbuf_allocator, shared storage mode)
┌──────────────────────────────────────────┐
│ Vertex buffers: {gpu_addr, stride, len}[] │  slots 0–15
│ Constant buffers: gpu_addr[]             │  CBs: indices 0–31
│ Samplers: handle[]                       │  indices 32–63
│ SRVs: {tex_handle, buf_handle, width}[]  │  indices 128+ (stride 3)
│ UAVs: {handle, counter, metadata}[]      │  indices 512+ (stride 3)
└──────────────────────────────────────────┘
         ↑
setVertexBufferOffset(argbuf_base, slot=0)
setFragmentBufferOffset(argbuf_base, slot=0)
```

Slot layout from `MTL_SHADER_REFLECTION`:
- `ArgumentBufferBindIndex` — which Metal buffer slot holds the argument buffer
- `ConstantBufferTableBindIndex` — offset within argbuf for CB table
- `SRVSlotMaskHi/Lo`, `SamplerSlotMask`, `UAVSlotMask` — which slots are live

---

## Shader Translation Pipeline (airconv)

DXMT compiles DXBC SM4/SM5 directly to Apple AIR (LLVM IR with Metal metadata),
bypassing text-based MSL entirely.

```mermaid
flowchart LR
    DXBC["DXBC bytecode\nSM 4.0 / 5.0"] --> SM50["SM50Initialize()\nsm50_shader_t\n+ reflection"]
    SM50 --> CONV["convertDXBC()\nper-instruction\nDXBC → LLVM IR\nvia AIRBuilder::emit*()"]
    CONV --> CFG["CFG analysis\nbasic blocks\ndominators"]
    CFG --> AIR["LLVM Module\n.air / Apple AIR"]
    AIR --> ML[".metallib binary\nmetallib_writer"]
    ML --> CACHE["Disk cache\nMTLBinaryArchive\nSHA-1 keyed"]
    CACHE --> LIB["id<MTLLibrary>"]

    GS["Geometry Shader\nVS+GS → Object+Mesh\nrequires Metal 3.1"]
    TS["Tessellation\nVS+HS → Object shader\nDS → Mesh shader"]
    CONV -.-> GS
    CONV -.-> TS
```

**Note for dxmt9:** airconv targets SM4/SM5 only. For D3D9 (SM 1–3), use the
vkd3d-shader → SPIRV-Cross pipeline described in `shader-translation.md`.

### Shader Variant System

The same DXBC is compiled multiple times for different specializations:

| Variant type | Specialization keys |
|---|---|
| `ShaderVariantVertex` | Input layout + GS passthrough flags + rasterization disabled |
| `ShaderVariantPixel` | Sample mask + dual-source blend + depth output disable + UNORM mask |
| `ShaderVariantTessVS+HS` | Paired compilation → single object shader |
| `ShaderVariantDomainDS` | Compiled as mesh shader |
| `ShaderVariantGS` | VS+GS paired: object + mesh shader |

---

## PSO Compilation (Async)

```mermaid
sequenceDiagram
    participant Ctx as ImmediateContext
    participant PC as PipelineCache
    participant TP as ThreadPool
    participant MTL as Metal API

    Ctx->>PC: GetCompiledPipeline(MTL_GRAPHICS_PIPELINE_DESC)
    PC->>PC: hash(desc) → lookup
    alt cache miss
        PC->>TP: ThreadpoolWork::Submit(CompilePSO)
        note over TP: async compilation
    end
    PC-->>Ctx: Rc<MTLCompiledGraphicsPipeline>

    Ctx->>Ctx: DrawCall → GetPipeline()
    alt PSO ready
        Ctx->>MTL: setRenderPipelineState(pso)
    else still compiling
        Ctx->>Ctx: spin-wait (rare, first draw only)
    end
```

---

## Present / Swapchain Path

```mermaid
sequenceDiagram
    participant App
    participant SC as IDXGISwapChain4
    participant PR as dxmt_presenter
    participant CL as CAMetalLayer
    participant MTL as Metal API

    App->>SC: Present(SyncInterval, Flags)
    SC->>CQ: CommitCurrentChunk()
    note over CQ: encode blit: backbuffer → drawable
    CQ->>CL: nextDrawable (blocks if pool empty = vsync)
    CQ->>MTL: MTLBlitCommandEncoder: copy texture → drawable.texture
    CQ->>MTL: commandBuffer.presentDrawable(drawable)
    CQ->>MTL: commandBuffer.commit()
```

---

## Key Design Patterns for dxmt9

| Pattern | DXMT Implementation | Apply to dxmt9 |
|---|---|---|
| Producer-consumer command queue | Main thread writes lambdas; encode thread replays | Keep Metal off Wine thread |
| Deferred encoder + merging | `startRenderPass()` checks attachments + Bloom deps | Map BeginScene/EndScene and RT changes to pass boundaries |
| Bloom filter dependency tracking | `EncoderDepSet` = `PartitionedBloomFilter64<16>` | Detect cross-pass hazards cheaply |
| Resource renaming | `Buffer::rename()` swaps allocation atomically | D3D9 DISCARD = rename, not stall |
| Argument buffers | All slots packed into one GPU buffer per pass | Saves per-resource `setBuffer` overhead |
| Async PSO compilation | `ThreadpoolWork`; draw blocks only on first miss | Cache D3D9 pipeline states similarly |
| COM over Rc | COM layer uses AddRef/Release; core uses Rc | Maintain boundary for dxmt9 COM objects |
| Wine unix lib thunks | `__wine_unix_call` → macOS-native code | Required for Wine-hosted builds |
| Shader disk cache | `MTLBinaryArchive` keyed by SHA-1 | Apply to compiled MSL/metallib |

---

## Key Files to Study

| Concern | DXMT file |
|---|---|
| Command recording / lambda emission | `dxmt/dxmt_command_queue.hpp`, `dxmt/dxmt_command_list.hpp` |
| Encoder management + merging | `dxmt/dxmt_context.{cpp,hpp}` |
| Metal command structs | `dxmt/dxmt_command.{hpp,metal}` |
| Buffer / Texture lifetime | `dxmt/dxmt_buffer.hpp`, `dxmt/dxmt_texture.hpp` |
| Dynamic resource triple-buffering | `dxmt/dxmt_dynamic.{cpp,hpp}` |
| Residency / dependency tracking | `dxmt/dxmt_residency.hpp`, `dxmt/dxmt_deptrack.hpp` |
| Shader compilation entry point | `airconv/airconv_public.h`, `airconv/airconv_context.cpp` |
| D3D state → Metal PSO | `d3d11/d3d11_pipeline.{cpp,hpp}`, `d3d11/d3d11_shader.hpp` |
| Present path | `dxmt/dxmt_presenter.{cpp,hpp}`, `d3d11/d3d11_swapchain.cpp` |
| Wine bridge | `winemetal/winemetal.h`, `winemetal/Metal.hpp` |

---

## Sources

- Official repository: https://github.com/3Shain/dxmt
- DeepWiki analysis: https://deepwiki.com/3Shain/dxmt
