#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

enum class PeRecorderFlushReason : std::uint32_t {
    Explicit = 0,
    CapacityPre,
    CapacityPost,
    Barrier,
    Present,
    Readback,
    Reset,
    StateBlock,
    Child,
    Destructor,
    StateMutation,
    // Clear/Draw have no producers since the DXMT9_PE_FLUSH_AFTER_CLEAR /
    // DXMT9_PE_FLUSH_AFTER_DRAW pacing probes were removed; the values stay
    // for stats-index numbering stability and historical log decoding.
    Clear,
    Draw,
    Count,
};

static constexpr std::size_t kPeRecorderFlushReasonCount =
    static_cast<std::size_t>(PeRecorderFlushReason::Count);

static constexpr std::size_t kPeCommandRecordTypeBucketCount = 30;
static constexpr std::size_t kPeRecorderInterAppendTopPairCount = 4;
static constexpr std::size_t kPeRecorderInterAppendTopCallFamilyCount = 2;
static constexpr std::size_t kPeRecorderInterAppendTopCallNameCount = 2;
static constexpr std::size_t kPeRecorderInterAppendTopCallTransitionCount = 2;

enum class PeHotStateSetterFamily : std::uint32_t {
    RenderTarget = 0,
    DepthStencil,
    ViewportScissor,
    Transform,
    MaterialLightClip,
    RenderState,
    TextureStageSampler,
    Texture,
    VertexInput,
    Shader,
    Count,
};

static constexpr std::size_t kPeHotStateSetterFamilyCount =
    static_cast<std::size_t>(PeHotStateSetterFamily::Count);

enum class PeInterAppendCallFamily : std::uint32_t {
    Unknown = 0,
    RenderTarget,
    DepthStencil,
    ViewportScissor,
    Transform,
    MaterialLightClip,
    RenderState,
    TextureStageSampler,
    Texture,
    VertexInput,
    Shader,
    VsConst,
    PsConst,
    Draw,
    Barrier,
    ScenePresent,
    Resource,
    Count,
};

static constexpr std::size_t kPeInterAppendCallFamilyCount =
    static_cast<std::size_t>(PeInterAppendCallFamily::Count);

enum class PeInterAppendCallName : std::uint32_t {
    Unknown = 0,
    BeginScene,
    EndScene,
    Clear,
    SetRenderTarget,
    GetRenderTarget,
    SetDepthStencilSurface,
    GetDepthStencilSurface,
    SetViewport,
    GetViewport,
    SetScissorRect,
    GetScissorRect,
    SetRenderState,
    SetTextureStageState,
    SetSamplerState,
    SetTexture,
    SetFVF,
    SetVertexDeclaration,
    SetStreamSource,
    SetStreamSourceFreq,
    SetIndices,
    SetVertexShader,
    SetPixelShader,
    SetVertexShaderConstantF,
    SetVertexShaderConstantI,
    SetVertexShaderConstantB,
    SetPixelShaderConstantF,
    SetPixelShaderConstantI,
    SetPixelShaderConstantB,
    DrawPrimitive,
    DrawIndexedPrimitive,
    DrawPrimitiveUP,
    DrawIndexedPrimitiveUP,
    ProcessVertices,
    GetBackBuffer,
    GetSwapChain,
    GetRasterStatus,
    ValidateDevice,
    SetSoftwareVertexProcessing,
    SetNPatchMode,
    SurfaceGetDesc,
    SurfaceLockRect,
    TextureGetSurfaceLevel,
    CubeTextureGetCubeMapSurface,
    VertexBufferLock,
    VertexBufferGetDesc,
    IndexBufferLock,
    IndexBufferGetDesc,
    QueryIssue,
    QueryGetData,
    StateBlockCapture,
    StateBlockApply,
    OtherChild,
    OtherGet,
    OtherSet,
    OtherCreate,
    Other,
    Count,
};

static constexpr std::size_t kPeInterAppendCallNameCount =
    static_cast<std::size_t>(PeInterAppendCallName::Count);

enum class PeInterAppendFocusPair : std::uint32_t {
    DrawIndexedToVsConstF = 0,
    DrawIndexedToApplyState,
    DrawIndexedToDrawIndexed,
    DrawIndexedToPsConstF,
    Count,
};

static constexpr std::size_t kPeInterAppendFocusPairCount =
    static_cast<std::size_t>(PeInterAppendFocusPair::Count);

struct PeRecorderStats {
    std::uint64_t commitCount = 0;
    std::uint64_t recordCountTotal = 0;
    std::uint64_t recordCountMax = 0;
    std::uint64_t payloadBytesTotal = 0;
    std::uint64_t payloadBytesMax = 0;
    std::uint64_t handleCountTotal = 0;
    std::uint64_t handleCountMax = 0;
    std::uint64_t chunkFillGapSamples = 0;
    std::uint64_t chunkFillGapNsTotal = 0;
    std::uint64_t chunkFillGapNsMax = 0;
    std::uint64_t chunkFirstRecordGapSamples = 0;
    std::uint64_t chunkFirstRecordGapNsTotal = 0;
    std::uint64_t chunkFirstRecordGapNsMax = 0;
    std::uint64_t chunkActiveFillSamples = 0;
    std::uint64_t chunkActiveFillNsTotal = 0;
    std::uint64_t chunkActiveFillNsMax = 0;
    std::uint64_t chunkInterAppendGapSamples = 0;
    std::uint64_t chunkInterAppendGapNsTotal = 0;
    std::uint64_t chunkInterAppendGapNsMax = 0;
    std::array<std::uint64_t,
               kPeCommandRecordTypeBucketCount *
                   kPeCommandRecordTypeBucketCount>
        chunkInterAppendPairSamples{};
    std::array<std::uint64_t,
               kPeCommandRecordTypeBucketCount *
                   kPeCommandRecordTypeBucketCount>
        chunkInterAppendPairNsTotal{};
    std::array<std::uint64_t,
               kPeCommandRecordTypeBucketCount *
                   kPeCommandRecordTypeBucketCount>
        chunkInterAppendPairNsMax{};
    std::array<std::uint64_t,
               kPeInterAppendFocusPairCount *
                   kPeInterAppendCallFamilyCount>
        chunkInterAppendFocusCallFamilySamples{};
    std::array<std::uint64_t,
               kPeInterAppendFocusPairCount *
                   kPeInterAppendCallFamilyCount>
        chunkInterAppendFocusCallFamilyNsTotal{};
    std::array<std::uint64_t,
               kPeInterAppendFocusPairCount *
                   kPeInterAppendCallFamilyCount>
        chunkInterAppendFocusCallFamilyNsMax{};
    std::array<std::uint64_t, kPeInterAppendFocusPairCount>
        chunkInterAppendFocusPhaseSamples{};
    std::array<std::uint64_t, kPeInterAppendFocusPairCount>
        chunkInterAppendFocusPreCallNsTotal{};
    std::array<std::uint64_t, kPeInterAppendFocusPairCount>
        chunkInterAppendFocusPreCallNsMax{};
    std::array<std::uint64_t, kPeInterAppendFocusPairCount>
        chunkInterAppendFocusInsideCallNsTotal{};
    std::array<std::uint64_t, kPeInterAppendFocusPairCount>
        chunkInterAppendFocusInsideCallNsMax{};
    std::array<std::uint64_t, kPeInterAppendFocusPairCount>
        chunkInterAppendFocusTailSplitSamples{};
    std::array<std::uint64_t, kPeInterAppendFocusPairCount>
        chunkInterAppendFocusPrevCallTailNsTotal{};
    std::array<std::uint64_t, kPeInterAppendFocusPairCount>
        chunkInterAppendFocusPrevCallTailNsMax{};
    std::array<std::uint64_t, kPeInterAppendFocusPairCount>
        chunkInterAppendFocusBetweenCallsNsTotal{};
    std::array<std::uint64_t, kPeInterAppendFocusPairCount>
        chunkInterAppendFocusBetweenCallsNsMax{};
    std::array<std::uint64_t,
               kPeInterAppendFocusPairCount *
                   kPeInterAppendCallFamilyCount>
        chunkInterAppendFocusBetweenCallFamilySamples{};
    std::array<std::uint64_t,
               kPeInterAppendFocusPairCount *
                   kPeInterAppendCallNameCount>
        chunkInterAppendFocusBetweenCallNameSamples{};
    std::array<std::uint64_t,
               kPeInterAppendFocusPairCount *
                   kPeInterAppendCallNameCount>
        chunkInterAppendFocusBetweenCallNameCpuNsTotal{};
    std::array<std::uint64_t,
               kPeInterAppendFocusPairCount *
                   kPeInterAppendCallNameCount>
        chunkInterAppendFocusBetweenCallNameCpuNsMax{};
    std::array<std::uint64_t,
               kPeInterAppendFocusPairCount *
                   kPeInterAppendCallFamilyCount *
                   kPeInterAppendCallFamilyCount>
        chunkInterAppendFocusBetweenCallTransitionSamples{};
    std::array<std::uint64_t,
               kPeInterAppendFocusPairCount *
                   kPeInterAppendCallFamilyCount *
                   kPeInterAppendCallFamilyCount>
        chunkInterAppendFocusBetweenCallTransitionNsTotal{};
    std::array<std::uint64_t,
               kPeInterAppendFocusPairCount *
                   kPeInterAppendCallFamilyCount *
                   kPeInterAppendCallFamilyCount>
        chunkInterAppendFocusBetweenCallTransitionNsMax{};
    std::array<std::uint64_t,
               kPeInterAppendFocusPairCount *
                   kPeInterAppendCallNameCount *
                   kPeInterAppendCallNameCount>
        chunkInterAppendFocusBetweenCallNameTransitionSamples{};
    std::array<std::uint64_t,
               kPeInterAppendFocusPairCount *
                   kPeInterAppendCallNameCount *
                   kPeInterAppendCallNameCount>
        chunkInterAppendFocusBetweenCallNameTransitionNsTotal{};
    std::array<std::uint64_t,
               kPeInterAppendFocusPairCount *
                   kPeInterAppendCallNameCount *
                   kPeInterAppendCallNameCount>
        chunkInterAppendFocusBetweenCallNameTransitionNsMax{};
    std::array<std::uint64_t, kPeInterAppendFocusPairCount>
        chunkInterAppendFocusBetweenCallBodyCalls{};
    std::array<std::uint64_t, kPeInterAppendFocusPairCount>
        chunkInterAppendFocusBetweenCallBodyCpuNsTotal{};
    std::array<std::uint64_t, kPeInterAppendFocusPairCount>
        chunkInterAppendFocusBetweenCallBodyCpuNsMax{};
    std::uint64_t chunkBridgeSamples = 0;
    std::uint64_t chunkBridgeNsTotal = 0;
    std::uint64_t chunkBridgeNsMax = 0;
    std::uint64_t recordAppendCalls = 0;
    std::uint64_t recordAppendCpuNsTotal = 0;
    std::uint64_t recordAppendCpuNsMax = 0;
    std::uint64_t recordAppendNoFlushCalls = 0;
    std::uint64_t recordAppendNoFlushCpuNsTotal = 0;
    std::uint64_t recordAppendNoFlushCpuNsMax = 0;
    std::uint64_t vsConstFSetterCalls = 0;
    std::uint64_t vsConstFSetterRegs = 0;
    std::uint64_t vsConstFSetterCpuNsTotal = 0;
    std::uint64_t vsConstFSetterCpuNsMax = 0;
    std::uint64_t psConstFSetterCalls = 0;
    std::uint64_t psConstFSetterRegs = 0;
    std::uint64_t psConstFSetterCpuNsTotal = 0;
    std::uint64_t psConstFSetterCpuNsMax = 0;
    std::uint64_t constFlushCalls = 0;
    std::uint64_t constFlushRecords = 0;
    std::uint64_t constFlushRegs = 0;
    std::uint64_t constFlushCpuNsTotal = 0;
    std::uint64_t constFlushCpuNsMax = 0;
    std::uint64_t vsConstFFlushRecords = 0;
    std::uint64_t vsConstFFlushRegs = 0;
    std::uint64_t vsConstFFlushCpuNsTotal = 0;
    std::uint64_t psConstFFlushRecords = 0;
    std::uint64_t psConstFFlushRegs = 0;
    std::uint64_t psConstFFlushCpuNsTotal = 0;
    std::uint64_t chunkBarrierFlushCalls = 0;
    std::uint64_t chunkBarrierConstCpuNsTotal = 0;
    std::uint64_t chunkBarrierConstCpuNsMax = 0;
    std::uint64_t applyStateBuildCalls = 0;
    std::uint64_t applyStateBuildCpuNsTotal = 0;
    std::uint64_t applyStateBuildCpuNsMax = 0;
    std::array<std::uint64_t, kPeHotStateSetterFamilyCount>
        hotStateSetterCalls{};
    std::array<std::uint64_t, kPeHotStateSetterFamilyCount>
        hotStateSetterDirty{};
    std::array<std::uint64_t, kPeHotStateSetterFamilyCount>
        hotStateSetterCpuNsTotal{};
    std::array<std::uint64_t, kPeHotStateSetterFamilyCount>
        hotStateSetterCpuNsMax{};
    std::array<std::uint64_t, kPeRecorderFlushReasonCount> flushReasons{};
    std::uint64_t drawPrimitiveUPCalls = 0;
    std::uint64_t drawIndexedPrimitiveUPCalls = 0;
    std::uint64_t upVertexBytes = 0;
    std::uint64_t upIndexBytes = 0;
};

inline const char* peRecorderFlushReasonName(PeRecorderFlushReason reason) {
    switch (reason) {
    case PeRecorderFlushReason::Explicit: return "explicit";
    case PeRecorderFlushReason::CapacityPre: return "capacity_pre";
    case PeRecorderFlushReason::CapacityPost: return "capacity_post";
    case PeRecorderFlushReason::Barrier: return "barrier";
    case PeRecorderFlushReason::Present: return "present";
    case PeRecorderFlushReason::Readback: return "readback";
    case PeRecorderFlushReason::Reset: return "reset";
    case PeRecorderFlushReason::StateBlock: return "stateblock";
    case PeRecorderFlushReason::Child: return "child";
    case PeRecorderFlushReason::Destructor: return "destructor";
    case PeRecorderFlushReason::StateMutation: return "state_mutation";
    case PeRecorderFlushReason::Clear: return "clear";
    case PeRecorderFlushReason::Draw: return "draw";
    case PeRecorderFlushReason::Count: break;
    }
    return "unknown";
}

struct PeCommandChunkCommitInfo {
    std::uint32_t recordCount = 0;
    std::uint32_t payloadBytes = 0;
    std::uint32_t handleCount = 0;
    std::uint32_t wireBytes = 0;
};
