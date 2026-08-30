/* dxmt9/device_c.h -- C ABI bridge between the PE winemetal_dxmt9.dll bridge and
 * the unix-side winemetal_dxmt9.so module. All types use stdint / plain C so this
 * header is safe to include from both Apple clang (Mach-O) and llvm-mingw
 * (PE) compilations.
 *
 * Enum fields carry raw D3D9 wire values (e.g. D3DFORMAT = 21 for A8R8G8B8).
 * Conversion to internal dxmt9 types happens inside device_c.cpp. */

#pragma once
#include <stdint.h>
#include <stddef.h>

#ifndef DXMT9_NODISCARD
#if defined(__cplusplus) && __cplusplus >= 201703L
#define DXMT9_NODISCARD [[nodiscard]]
#else
#define DXMT9_NODISCARD
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── opaque handles ──────────────────────────────────────────────────────── */

typedef struct D9CFactory    D9CFactory;
typedef struct D9CDevice     D9CDevice;
typedef struct D9CSwapChain  D9CSwapChain;
typedef struct D9CTexture    D9CTexture;
typedef struct D9CBuffer     D9CBuffer;
typedef struct D9CSurface    D9CSurface;
typedef struct D9CQuery      D9CQuery;
typedef struct D9CStateBlock D9CStateBlock;
typedef struct D9CVertexDecl D9CVertexDecl;
typedef struct D9CShader     D9CShader;

typedef struct D9CBufferDesc {
    uint32_t size;
    uint32_t usage;
    uint32_t pool;
    uint32_t fvf;
    uint32_t format;
} D9CBufferDesc;

/* ── plain C structs (D3D9 field layout, basic C types) ──────────────────── */

typedef struct D9CRect {
    int32_t left, top, right, bottom;
} D9CRect;

typedef struct D9CViewport {
    uint32_t x, y, width, height;
    float    minZ, maxZ;
} D9CViewport;

typedef struct D9CPresentParams {
    uint32_t backBufferWidth;
    uint32_t backBufferHeight;
    uint32_t backBufferFormat;        /* D3DFORMAT */
    uint32_t backBufferCount;
    uint32_t multiSampleType;         /* D3DMULTISAMPLE_TYPE */
    uint32_t multiSampleQuality;
    uint32_t swapEffect;              /* D3DSWAPEFFECT */
    uint64_t deviceWindow;            /* HWND as opaque u64 */
    uint32_t windowed;                /* BOOL */
    uint32_t enableAutoDepthStencil;  /* BOOL */
    uint32_t autoDepthStencilFormat;  /* D3DFORMAT */
    uint32_t flags;
    uint32_t fullScreenRefreshRateHz;
    uint32_t presentationInterval;
} D9CPresentParams;

/* Dedicated cold WSI bootstrap payload. The scalar tokens originate in
 * winemac.drv and are opaque to PE code; they never enter CommandChunk. */
typedef struct D9CWsiSurfaceBinding {
    uint32_t structSize;
    uint32_t protocol;
    uint64_t hwnd;
    uint64_t surfaceToken;
    uint64_t layerToken;
} D9CWsiSurfaceBinding;

#define D9C_WSI_SURFACE_PROTOCOL_EXTESCAPE_V1 1u
#define D9C_WSI_SURFACE_PROTOCOL_LEGACY_MACDRV_SYMBOLS 2u

typedef struct D9CDisplayModeEx {
    uint32_t width, height, refreshRate;
    uint32_t format;                  /* D3DFORMAT */
    uint32_t scanLineOrdering;        /* D3DDISPLAYSCANLINEORDERING */
} D9CDisplayModeEx;

typedef struct D9CAdapterIdentifier {
    char     driver[512];
    char     description[512];
    char     deviceName[32];
    uint64_t driverVersion;
    uint32_t vendorId, deviceId, subSysId, revision;
    uint8_t  deviceIdentifier[16];   /* GUID bytes */
    uint32_t whqlLevel;
} D9CAdapterIdentifier;

typedef struct D9CCaps {
    uint32_t deviceType;
    uint32_t adapterOrdinal;
    uint32_t caps, caps2, caps3;
    uint32_t presentationIntervals;
    uint32_t cursorCaps;
    uint32_t devCaps;
    uint32_t primitiveMiscCaps;
    uint32_t rasterCaps;
    uint32_t zCmpCaps;
    uint32_t srcBlendCaps, destBlendCaps, alphaBlendCaps;
    /*
     * gap.md §C.7: D3DCAPS9::AlphaCmpCaps lives in its own dedicated
     * slot (not in alphaBlendCaps above — D3D9 has no AlphaBlendCaps
     * member at all, the alphaBlendCaps slot existed only as a
     * mis-named carrier for AlphaCmpCaps and is kept for ABI
     * compatibility with the legacy d3d9.dll PE bridge but is no
     * longer the canonical source).
     */
    uint32_t alphaCmpCaps;
    uint32_t shadeCaps;
    uint32_t textureCaps;
    uint32_t textureFilterCaps;
    uint32_t cubetextureFilterCaps;
    uint32_t volumeTextureFilterCaps;
    uint32_t textureAddressCaps;
    uint32_t volumeTextureAddressCaps;
    uint32_t lineCaps;
    uint32_t maxTextureWidth, maxTextureHeight;
    uint32_t maxVolumeExtent;
    uint32_t maxTextureRepeat;
    uint32_t maxTextureAspectRatio;
    uint32_t maxAnisotropy;
    float    maxVertexW;
    float    guardBandLeft, guardBandTop, guardBandRight, guardBandBottom;
    float    extentsAdjust;
    uint32_t stencilCaps;
    uint32_t fvfCaps;
    uint32_t textureBlendCaps;       /* maps to D3DCAPS9.TextureOpCaps */
    uint32_t maxTextureBlendStages;  /* D3DCAPS9.MaxTextureBlendStages */
    uint32_t maxSimultaneousTextures;/* D3DCAPS9.MaxSimultaneousTextures */
    uint32_t vertexProcessingCaps;   /* D3DCAPS9.VertexProcessingCaps */
    uint32_t maxActiveLights;        /* D3DCAPS9.MaxActiveLights */
    uint32_t maxUserClipPlanes;
    uint32_t maxVertexBlendMatrices;
    uint32_t maxVertexBlendMatrixIndex;
    float    maxPointSize;
    uint32_t maxPrimitiveCount;
    uint32_t maxVertexIndex;
    uint32_t maxStreams;
    uint32_t maxStreamStride;
    uint32_t vertexShaderVersion;
    uint32_t maxVertexShaderConst;
    uint32_t pixelShaderVersion;
    float    pixelShader1xMaxValue;
    uint32_t devCaps2;
    float    maxNpatchTessellationLevel;
    uint32_t reserved5;
    uint32_t masterAdapterOrdinal;
    uint32_t adapterOrdinalInGroup;
    uint32_t numberOfAdaptersInGroup;
    uint32_t declTypes;
    uint32_t numSimultaneousRTs;
    uint32_t stretchRectFilterCaps;
    /* D3DVSHADERCAPS2_0 */
    uint32_t vs20Caps, vs20DynamicFlowControlDepth;
    uint32_t vs20NumTemps, vs20StaticFlowControlDepth;
    /* D3DPSHADERCAPS2_0 */
    uint32_t ps20Caps, ps20DynamicFlowControlDepth;
    uint32_t ps20NumTemps, ps20StaticFlowControlDepth;
    uint32_t ps20NumInstructionSlots;
    uint32_t vertexTextureFilterCaps;
    uint32_t maxVShaderInstructionsExecuted;
    uint32_t maxPShaderInstructionsExecuted;
    uint32_t maxVertexShader30InstructionSlots;
    uint32_t maxPixelShader30InstructionSlots;
} D9CCaps;

typedef struct D9CMatrix { float m[16]; } D9CMatrix;

typedef struct D9CColorRGBA { float r, g, b, a; } D9CColorRGBA;

typedef struct D9CMaterial {
    D9CColorRGBA diffuse, ambient, specular, emissive;
    float        power;
} D9CMaterial;

typedef struct D9CLight {
    uint32_t     type;            /* D3DLIGHTTYPE */
    D9CColorRGBA diffuse, specular, ambient;
    float        position[3];
    float        direction[3];
    float        range, falloff;
    float        attenuation0, attenuation1, attenuation2;
    float        theta, phi;
} D9CLight;

typedef struct D9CLockedRect {
    int32_t pitch;
    void*   bits;
} D9CLockedRect;

typedef struct D9CSurfaceDesc {
    uint32_t format;              /* D3DFORMAT */
    uint32_t resourceType;        /* D3DRESOURCETYPE */
    uint32_t usage;               /* D3DUSAGE flags */
    uint32_t pool;                /* D3DPOOL */
    uint32_t multiSampleType;     /* D3DMULTISAMPLE_TYPE */
    uint32_t multiSampleQuality;
    uint32_t width, height, depth;
} D9CSurfaceDesc;

typedef struct D9CVertexElement {
    uint16_t stream, offset;
    uint8_t  type, method, usage, usageIndex;
} D9CVertexElement;

/* Fixed-width command packet fields. Handles are split into two uint32_t
 * lanes so the packet layout is identical for 32-bit PE, WoW64, and 64-bit
 * unix-side consumers. */
#define D9C_DRAW_PACKET_MAX_RENDER_STATES 64
#define D9C_DRAW_PACKET_MAX_TEXTURES 20
#define D9C_DRAW_PACKET_MAX_STREAMS 16
#define D9C_DRAW_PACKET_MAX_RENDER_TARGETS 4
/* Phase 12: per-draw delta caps for the per-stage / per-sampler scalar
 * setter arrays. 64 covers the full TSS type space (~30) × stage 0 +
 * a typical multi-stage frame's typical TSS churn; the actual upper
 * bound (8 stages × 30 types = 240) would over-allocate the packet
 * for the common case. PE side flushes the chunk if a Set call would
 * exceed the cap. */
#define D9C_DRAW_PACKET_MAX_TSS 64
#define D9C_DRAW_PACKET_MAX_SAMPLER 64
#define D9C_DRAW_PACKET_MAX_TRANSFORMS 16
#define D9C_DRAW_PACKET_MAX_LIGHTS 8

typedef struct D9CWireHandle {
    uint32_t lo;
    uint32_t hi;
} D9CWireHandle;

/* Phase 12: per-stage TSS / per-sampler scalar deltas (mirrors the
 * D9CCommandChunkWireRenderState shape). PE recorder accumulates dirty
 * (stage,type)→value tuples; the next record ships them as a sparse canonical
 * section and the server-side dispatcher applies via dxmt9c_device_set_*. */
typedef struct D9CDrawPacketTextureStageState {
    uint32_t stage;
    uint32_t type;
    uint32_t value;
} D9CDrawPacketTextureStageState;

typedef struct D9CDrawPacketSamplerState {
    uint32_t sampler;
    uint32_t type;
    uint32_t value;
} D9CDrawPacketSamplerState;

/* Phase 12: SetTransform delta. Each entry is a state enum + 4×4
 * matrix = 4+64 bytes. Per-frame typically a handful (View, Projection,
 * a few World/Texture transforms), so 16 covers normal frames. */
typedef struct D9CDrawPacketTransform {
    uint32_t state;
    uint32_t reserved;
    D9CMatrix matrix;
} D9CDrawPacketTransform;

/* R-BACK-2.52 (inline const delta, opt-in via DXMT9_PE_INLINE_CONST_DELTA):
 * register-file caps for the six per-draw const-delta sections below. These
 * mirror the D3D9 register-file limits enforced PE-side by
 * D3D9DeviceImpl::kVsConstFMax / kVsConstIMax / kVsConstBMax / kPsConstFMax /
 * kPsConstIMax / kPsConstBMax (d3d9_pe_device.cpp) — a section can never
 * legally exceed one full register file, so a producer that disagrees with
 * these caps has a schema bug, not a legitimately wider register file. */
#define D9C_DRAW_PACKET_MAX_CONST_VS_F 256
#define D9C_DRAW_PACKET_MAX_CONST_VS_I 16
#define D9C_DRAW_PACKET_MAX_CONST_VS_B 16
#define D9C_DRAW_PACKET_MAX_CONST_PS_F 224
#define D9C_DRAW_PACKET_MAX_CONST_PS_I 16
#define D9C_DRAW_PACKET_MAX_CONST_PS_B 16

/* Canonical wire order for the six inline const-delta sections — also the
 * order of the six canonical constant sections. Mirrors the
 * PeConstShadowBlock field order (d3d9_pe_const_shadow.hpp: vsConstF,
 * vsConstI, vsConstB, psConstF, psConstI, psConstB) and the
 * D9C_COMMAND_RECORD_SET_*_CONST_* record-type grouping below. */
enum {
    D9C_DRAW_PACKET_CONST_DELTA_VS_F = 0,
    D9C_DRAW_PACKET_CONST_DELTA_VS_I = 1,
    D9C_DRAW_PACKET_CONST_DELTA_VS_B = 2,
    D9C_DRAW_PACKET_CONST_DELTA_PS_F = 3,
    D9C_DRAW_PACKET_CONST_DELTA_PS_I = 4,
    D9C_DRAW_PACKET_CONST_DELTA_PS_B = 5,
    D9C_DRAW_PACKET_CONST_DELTA_COUNT = 6,
};

/* R-BACK-2.52 (inline const delta, opt-in via DXMT9_PE_INLINE_CONST_DELTA):
 * the register-file caps above bound how many shader constants one record may
 * carry inline instead of shipping a standalone
 * D9C_COMMAND_RECORD_SET_*_CONST_* record.
 *
 * The fixed-size section header this comment used to describe
 * (D9CDrawPacketConstDeltaSection, carried on the fat D9CDrawPrimitivePacket)
 * was deleted with the legacy record format. Constants now ride the canonical sparse
 * constant sections, which are absent entirely rather than present-and-invalid
 * when nothing is folded, so the byte-identical-off-path property the old
 * design had to engineer is structural here. Element sizes are unchanged:
 * F/I = 16 bytes/register, B = 4 bytes/register. */

/* Canonical wire format: delta + PE shadow → effective state via
 * ordered server replay.
 *
 * Each draw record's "effective state" — what the GPU will see when
 * the draw issues — is the result of REPLAYING every preceding
 * record's state delta in chunk order against a server-side shadow.
 * A single packet does NOT carry a self-contained snapshot of the
 * full BaseDrawState; the Valid / Mask / Count fields express only
 * what changed since the last record. The PE recorder's own shadow
 * (default ON via Phase 22) is the authoritative source of truth on
 * the PE side, and the server's D9CDevice mirrors it via per-record
 * applyDrawPacketState dispatch.
 *
 * Effective-state recipe at draw N:
 *   server_shadow_after(N) = applyDeltas(server_shadow_after(N-1),
 *                                         packet_N.delta)
 * The encoder reads server_shadow_after(N) when issuing draw N.
 *
 * Run-coalescing (drawPrimitiveRun fast path) applies the first
 * record's delta as the run base, then accepts later records whose
 * delta is empty or repeats the same base delta. A delta-empty packet
 * means the effective state is unchanged from the prior draw; a
 * repeated base delta is idempotent against the run base. The delta
 * encoding is load-bearing for this optimization.
 *
 * Full-snapshot mode (DXMT9_PE_DRAW_FULL_SNAPSHOT=1, Phase 16) is a
 * DEBUG / STRESS knob — not the canonical wire form. When set, every
 * field is forced valid + populated from the PE shadow, making each
 * packet self-contained; every texture/stream mask bit is set and unbound
 * slots carry null handles so a snapshot also performs unbinds. This costs
 * wire bandwidth + disables run-coalescing. Use for: stress testing, debugging out-of-order
 * replay, environments where importer statelessness matters more
 * than wire efficiency. */

/* Per-record handle ranges. The PE recorder derives the deduplicated set of
 * direct, non-null references encoded by each command payload and appends it
 * as one canonical contiguous table slice. The importer requires an exact
 * bidirectional match between payload references and that slice before replay.
 * Effective unix-state dependencies not encoded by the delta remain covered
 * by normal per-draw resource marking. */
enum {
    D9C_CHUNK_HANDLE_KIND_TEXTURE = 0,
    D9C_CHUNK_HANDLE_KIND_SURFACE = 1,
    D9C_CHUNK_HANDLE_KIND_BUFFER = 2,
    D9C_CHUNK_HANDLE_KIND_SHADER = 3,
    D9C_CHUNK_HANDLE_KIND_VERTEX_DECL = 4,
    D9C_CHUNK_HANDLE_KIND_QUERY = 5,
};

/* Canonical pointer-free command chunk wire format (numeric version 2,
 * R-BACK-2.54 / R-BACK-2.55). It uses a table/table/arena organization. All
 * offsets are byte offsets
 * from the start of the chunk blob or record payload as documented by the
 * field. The supported PE and unix targets are little-endian; C++ consumers
 * pin that assumption in device_c_chunk_schema.hpp.
 *
 * Resource identity is never a D9C* address. Payload fields contain an
 * absolute uint32_t index into D9CCommandChunkWireHandleEntry[], and the
 * indexed entry carries the device-local object ID plus generation. Null uses
 * D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX and consumes no table entry. */
#define D9C_COMMAND_CHUNK_VERSION 2u
#define D9C_COMMAND_CHUNK_WIRE_VERSION 2u
#define D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE 48u
#define D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE 32u
#define D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE 16u
#define D9C_COMMAND_CHUNK_WIRE_SECTION_DESC_SIZE 16u
#define D9C_COMMAND_CHUNK_WIRE_DRAW_HEADER_SIZE 56u
#define D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX 0xffffffffu
#define D9C_COMMAND_CHUNK_RECORD_FLAG_NONE 0u
#define D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT 0x00000001u
#define D9C_COMMAND_CHUNK_CAP_CURRENT 0x00000002u
#define D9C_COMMAND_CHUNK_DEFAULT_WIRE_VERSION 2u
#define D9C_COMMAND_CHUNK_TRANSPORT_CONTIGUOUS 0x00000001u
#define D9C_COMMAND_CHUNK_TRANSPORT_SEGMENTED_V1 0x00000002u
#define D9C_COMMAND_CHUNK_SEGMENTED_TRANSPORT_V1 1u
#define D9C_COMMAND_CHUNK_MAX_TOTAL_WIRE_BYTES (32u * 1024u * 1024u)

typedef struct D9CCommandChunkWireHeader {
    uint32_t version;
    uint32_t headerSize;
    uint32_t recordHeaderSize;
    uint32_t handleEntrySize;
    uint32_t recordTableOffset;
    uint32_t recordCount;
    uint32_t handleTableOffset;
    uint32_t handleCount;
    uint32_t payloadArenaOffset;
    uint32_t payloadArenaSize;
    uint32_t reserved0;
    uint32_t reserved1;
} D9CCommandChunkWireHeader;

/* Fixed-role SegmentedTransportV1 descriptor. The canonical V2 header is
 * passed by value; each role token names only the live bytes of one client
 * span. recordBytes and handleBytes exclude the canonical zero-alignment gaps
 * between tables. The unix importer reconstructs those gaps from the header
 * offsets, so PE vectors never need to carry padding or expose bytes past
 * their logical ends. No role table or variable-width descriptor is
 * permitted. */
typedef struct D9CCommandChunkSegmentedTransportV1 {
    D9CCommandChunkWireHeader header;
    D9CWireHandle records;
    uint32_t recordBytes;
    uint32_t recordReserved;
    D9CWireHandle handles;
    uint32_t handleBytes;
    uint32_t handleReserved;
    D9CWireHandle payload;
    uint32_t payloadBytes;
    uint32_t payloadReserved;
    uint64_t renderTapeCaptureToken;
    uint64_t renderTapeEventOrdinal;
} D9CCommandChunkSegmentedTransportV1;

#define D9C_COMMAND_CHUNK_SEGMENTED_TRANSPORT_V1_SIZE 112u

typedef struct D9CCommandChunkWireRecordHeader {
    uint32_t type;
    uint32_t flags;
    uint32_t payloadOffset;
    uint32_t payloadSize;
    uint32_t firstHandle;
    uint32_t handleCount;
    uint32_t reserved0;
    uint32_t reserved1;
} D9CCommandChunkWireRecordHeader;

typedef struct D9CCommandChunkWireHandleEntry {
    uint32_t kind;
    /* Nonzero full-width registry generation. The unix registry requires an
     * exact {kind, objectId, generation} match before retain or replay, and
     * retires a slot at UINT32_MAX rather than wrapping this field. */
    uint32_t generation;
    uint64_t objectId;
} D9CCommandChunkWireHandleEntry;

typedef struct D9CWireObjectIdentity {
    uint32_t kind;
    uint32_t generation;
    uint64_t objectId;
} D9CWireObjectIdentity;

/* One-time per-device PE/unix command-chunk negotiation. Each side fills its
 * supported/preferred pair for the canonical wire version and independently
 * for transport. A forced preference must be selected exactly or device
 * initialization fails; no device changes grammar after this exchange. */
typedef struct D9CCommandChunkNegotiation {
    uint32_t peSupportedVersions;
    uint32_t pePreferredVersion;
    uint32_t unixSupportedVersions;
    uint32_t selectedVersion;
    /* Transport negotiation is independent from canonical wire-version
     * negotiation. These slots were reserved in the original 32-byte
     * exchange so adding SegmentedTransportV1 cannot change D9C V2. */
    union {
        uint32_t peSupportedTransports;
        uint32_t reserved0;
    };
    union {
        uint32_t pePreferredTransport;
        uint32_t reserved1;
    };
    union {
        uint32_t unixSupportedTransports;
        uint32_t reserved2;
    };
    union {
        uint32_t selectedTransport;
        uint32_t reserved3;
    };
} D9CCommandChunkNegotiation;

typedef struct D9CCommandChunkWireSectionDesc {
    uint16_t kind;
    uint16_t elementSize;
    uint32_t count;
    uint32_t payloadOffset;
    uint32_t byteSize;
} D9CCommandChunkWireSectionDesc;

/* Fixed prefix shared by DrawPrimitive, DrawIndexedPrimitive, both UP draw
 * forms, and APPLY_STATE. Fields unused by the selected opcode must be zero.
 * sectionTableOffset and sectionPayloadOffset are relative to the start of
 * this record payload. */
typedef struct D9CCommandChunkWireDrawHeader {
    uint32_t flags;
    uint32_t primitiveType;
    int32_t baseVertex;
    uint32_t minVertex;
    uint32_t numVertices;
    uint32_t startVertex;
    uint32_t startIndex;
    uint32_t primitiveCount;
    uint32_t stride;
    uint32_t indexFormat;
    uint32_t sectionCount;
    uint32_t sectionTableOffset;
    uint32_t sectionPayloadOffset;
    uint32_t reserved0;
} D9CCommandChunkWireDrawHeader;

enum {
    D9C_COMMAND_CHUNK_SECTION_RENDER_STATE = 1,
    D9C_COMMAND_CHUNK_SECTION_TEXTURE = 2,
    D9C_COMMAND_CHUNK_SECTION_STREAM = 3,
    D9C_COMMAND_CHUNK_SECTION_SHADER = 4,
    D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT = 5,
    D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER = 6,
    D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET = 7,
    D9C_COMMAND_CHUNK_SECTION_DEPTH_STENCIL = 8,
    D9C_COMMAND_CHUNK_SECTION_VIEWPORT = 9,
    D9C_COMMAND_CHUNK_SECTION_SCISSOR = 10,
    D9C_COMMAND_CHUNK_SECTION_MATERIAL = 11,
    D9C_COMMAND_CHUNK_SECTION_CLIP_PLANE = 12,
    D9C_COMMAND_CHUNK_SECTION_TEXTURE_STAGE_STATE = 13,
    D9C_COMMAND_CHUNK_SECTION_SAMPLER_STATE = 14,
    D9C_COMMAND_CHUNK_SECTION_TRANSFORM = 15,
    D9C_COMMAND_CHUNK_SECTION_LIGHT = 16,
    D9C_COMMAND_CHUNK_SECTION_LIGHT_ENABLE = 17,
    D9C_COMMAND_CHUNK_SECTION_VS_CONST_F = 18,
    D9C_COMMAND_CHUNK_SECTION_VS_CONST_I = 19,
    D9C_COMMAND_CHUNK_SECTION_VS_CONST_B = 20,
    D9C_COMMAND_CHUNK_SECTION_PS_CONST_F = 21,
    D9C_COMMAND_CHUNK_SECTION_PS_CONST_I = 22,
    D9C_COMMAND_CHUNK_SECTION_PS_CONST_B = 23,
    D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA = 24,
    D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA = 25,
    D9C_COMMAND_CHUNK_SECTION_COUNT = 25,
};

enum {
    D9C_COMMAND_CHUNK_SHADER_STAGE_VERTEX = 0,
    D9C_COMMAND_CHUNK_SHADER_STAGE_PIXEL = 1,
};

enum {
    D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF = 0,
    D9C_COMMAND_CHUNK_VERTEX_INPUT_DECLARATION = 1,
};

typedef struct D9CCommandChunkWireRenderState {
    uint32_t state;
    uint32_t value;
} D9CCommandChunkWireRenderState;

typedef struct D9CCommandChunkWireTextureBinding {
    uint32_t slot;
    uint32_t valid;
    uint32_t handleIndex;
    uint32_t reserved0;
} D9CCommandChunkWireTextureBinding;

typedef struct D9CCommandChunkWireStreamBinding {
    uint32_t slot;
    uint32_t valid;
    uint32_t handleIndex;
    uint32_t offset;
    uint32_t stride;
    uint32_t frequency;
    uint32_t reserved0;
} D9CCommandChunkWireStreamBinding;

typedef struct D9CCommandChunkWireShaderBinding {
    uint32_t stage;
    uint32_t valid;
    uint32_t handleIndex;
    uint32_t reserved0;
} D9CCommandChunkWireShaderBinding;

typedef struct D9CCommandChunkWireVertexInput {
    uint32_t valid;
    uint32_t kind;
    /* FVF value. Declaration entries carry the effective FVF too so replay
     * can preserve ordered SetFVF-then-SetVertexDeclaration semantics. */
    uint32_t value;
    uint32_t handleIndex;
} D9CCommandChunkWireVertexInput;

typedef struct D9CCommandChunkWireIndexBinding {
    uint32_t valid;
    uint32_t handleIndex;
} D9CCommandChunkWireIndexBinding;

typedef struct D9CCommandChunkWireRenderTargetBinding {
    uint32_t slot;
    uint32_t valid;
    uint32_t handleIndex;
    uint32_t reserved0;
} D9CCommandChunkWireRenderTargetBinding;

typedef struct D9CCommandChunkWireDepthStencilBinding {
    uint32_t valid;
    uint32_t handleIndex;
} D9CCommandChunkWireDepthStencilBinding;

typedef struct D9CCommandChunkWireClipPlane {
    uint32_t slot;
    uint32_t reserved0;
    float values[4];
} D9CCommandChunkWireClipPlane;

typedef struct D9CCommandChunkWireLight {
    uint32_t slot;
    uint32_t reserved0;
    D9CLight light;
} D9CCommandChunkWireLight;

typedef struct D9CCommandChunkWireLightEnable {
    uint32_t slot;
    uint32_t enabled;
} D9CCommandChunkWireLightEnable;

/* Constant section payloads begin with this range header, followed by
 * count*elementSize register bytes. The enclosing section kind selects
 * VS/PS and float/int/bool. */
typedef struct D9CCommandChunkWireConstantRange {
    uint32_t startRegister;
    uint32_t registerCount;
} D9CCommandChunkWireConstantRange;

/* Fixed non-draw canonical payloads. Variable arrays or bytes follow at the
 * record-relative offsets carried by the fixed payload. */
typedef struct D9CCommandChunkWireSetConst {
    uint32_t startRegister;
    uint32_t registerCount;
} D9CCommandChunkWireSetConst;

typedef struct D9CCommandChunkWireClear {
    uint32_t flags;
    uint32_t colorARGB;
    float z;
    uint32_t stencil;
    uint32_t rectCount;
    uint32_t rectOffset;
} D9CCommandChunkWireClear;

typedef struct D9CCommandChunkWirePresent {
    uint64_t hwnd;
    uint32_t flags;
    uint32_t hasSrc;
    uint32_t hasDst;
    /* Absolute index of the generation-qualified swapchain source surface in
     * this record's handle slice. Legacy records carry no handles and zero. */
    uint32_t sourceHandleIndex;
    D9CRect src;
    D9CRect dst;
} D9CCommandChunkWirePresent;

typedef struct D9CCommandChunkWireStretchRect {
    uint32_t srcHandleIndex;
    uint32_t dstHandleIndex;
    uint32_t hasSrcRect;
    uint32_t hasDstRect;
    uint32_t filter;
    uint32_t reserved0;
    D9CRect srcRect;
    D9CRect dstRect;
} D9CCommandChunkWireStretchRect;

typedef struct D9CCommandChunkWireColorFill {
    uint32_t surfaceHandleIndex;
    uint32_t colorARGB;
    uint32_t hasRect;
    uint32_t reserved0;
    D9CRect rect;
} D9CCommandChunkWireColorFill;

typedef struct D9CCommandChunkWireUpdateTexture {
    uint32_t srcHandleIndex;
    uint32_t dstHandleIndex;
} D9CCommandChunkWireUpdateTexture;

typedef struct D9CCommandChunkWireUpdateSurface {
    uint32_t srcHandleIndex;
    uint32_t dstHandleIndex;
    uint32_t hasSrcRect;
    uint32_t hasDstPoint;
    D9CRect srcRect;
    D9CRect dstPoint;
} D9CCommandChunkWireUpdateSurface;

typedef struct D9CCommandChunkWireQueryIssue {
    uint32_t queryHandleIndex;
    uint32_t flags;
} D9CCommandChunkWireQueryIssue;

typedef struct D9CCommandChunkWireReadback {
    uint32_t srcHandleIndex;
    uint32_t dstHandleIndex;
} D9CCommandChunkWireReadback;

typedef struct D9CCommandChunkWireReszDepthResolve {
    uint32_t msaaDepthHandleIndex;
    uint32_t intzDestHandleIndex;
} D9CCommandChunkWireReszDepthResolve;

typedef struct D9CCommandChunkWireGenerateMipmaps {
    uint32_t textureHandleIndex;
} D9CCommandChunkWireGenerateMipmaps;

enum {
    D9C_COMMAND_RECORD_DRAW_PRIMITIVE = 1,
    D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE = 2,
    D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP = 3,
    D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP = 4,
    /* Note: IDs 5..13 (per-call Set* setter-replay records) were removed
     * — that shape violated the recorder design. Set* state setters are
     * PE-shadow-only and travel through DrawRecord/APPLY_STATE deltas
     * instead of being emitted as separate backend commands.
     *
     * Variable-size const-array uploads. Each appends `count*kElemSize`
     * bytes after the fixed header — float[count*4] for *F, int32[count*4]
     * for *I, uint32[count] for *B. PE shadow accumulates dirty ranges
     * across Set*Constant* calls; flushPendingConsts() emits ONE record
     * per (stage,type) covering the merged dirty range right before the
     * next draw record (or at chunk seal). So a frame with 30 sequential
     * SetVsConstF calls between two draws costs 1 record, not 30. */
    D9C_COMMAND_RECORD_SET_VS_CONST_F = 14,
    D9C_COMMAND_RECORD_SET_VS_CONST_I = 15,
    D9C_COMMAND_RECORD_SET_VS_CONST_B = 16,
    D9C_COMMAND_RECORD_SET_PS_CONST_F = 17,
    D9C_COMMAND_RECORD_SET_PS_CONST_I = 18,
    D9C_COMMAND_RECORD_SET_PS_CONST_B = 19,
    /* Standalone ordering ops — recorder design's "Clear / Present /
     * Query / Surface" branch. All fire-and-forget surface-shape ops
     * are chunk-recorded; only sync-result calls (Lock-readback / Query
     * GetData) remain on flush+bridge because they need to return data
     * synchronously to the PE caller. */
    D9C_COMMAND_RECORD_CLEAR = 20,
    D9C_COMMAND_RECORD_PRESENT = 21,
    D9C_COMMAND_RECORD_STRETCH_RECT = 22,
    D9C_COMMAND_RECORD_COLOR_FILL = 23,
    D9C_COMMAND_RECORD_UPDATE_TEXTURE = 24,
    D9C_COMMAND_RECORD_UPDATE_SURFACE = 25,
    /* Phase 20: Query::Issue (D3DISSUE_BEGIN / D3DISSUE_END) is
     * RECORDABLE — fire-and-forget at the PE level, ordering record
     * inside the chunk. Server applies it via dxmt9c_query_issue
     * during chunk replay.
     *
     * Query::GetData is NOT recordable — it's a SYNCHRONOUS READ
     * BOUNDARY. The caller blocks on the result (S_OK / S_FALSE /
     * D3DERR_*) and reads bytes back. Implementation flushes the
     * pending recorder (so prior QUERY_ISSUE records are committed)
     * then bridges via dxmt9c_query_get_data which synchronously
     * inspects completedSeqId vs the query's recorded seqId. This
     * isn't a deficiency — async recording would just defer the
     * inevitable wait to a worse spot. */
    D9C_COMMAND_RECORD_QUERY_ISSUE = 26,
    /* Phase 24: GetRenderTargetData (RT → CPU-mappable surface). The PE
     * caller is synchronous — the call doesn't return until the data
     * is in dst — but routing it through the chunk record stream keeps
     * ordering atomic with surrounding draws/clears in the SAME chunk.
     * PE-side commits the chunk synchronously after appending this
     * record (Present pattern), so the chunk-commit boundary IS the
     * readback boundary. */
    D9C_COMMAND_RECORD_READBACK = 27,
    /* Phase 28: standalone state-delta record. Carries the same sparse canonical
     * state sections a draw record does, with no draw header fields — the
     * state delta is the whole point. Emitted BEFORE chunk-barrier records
     * (Clear / Present / surface ops / readback) when the PE recorder
     * has pending hot state that the barrier needs applied server-
     * side. Replaces the legacy "bridge-emit pending hot state"
     * pattern that violated the chunk-mode invariant (Set* never
     * crosses PE/unix in the default path). */
    D9C_COMMAND_RECORD_APPLY_STATE = 28,
    /* R-FORMAT-11: RESZ MSAA depth-resolve. The exact D3D9 idiom
     * SetRenderState(D3DRS_POINTSIZE, 0x7FA05000) is a *command* — resolve
     * the bound multisampled depth source into the bound INTZ depth texture.
     * Fire-and-forget (no synchronous result), so it travels as a chunk
     * record like the other surface ops rather than a flush+bridge call.
     * Its 8-byte payload carries two uint32 handle-table indices, matching the
     * canonical readback payload shape.
     * GPU correctness (MSAA depth + INTZ readback) is deferred runtime
     * validation; this record only carries the (source, destination) pair. */
    D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE = 29,
    /* R-FORMAT-16: ordered automatic-mipmap generation. The texture handle
     * identifies the one-public-level AUTOGEN object whose backend record owns
     * the complete hidden pyramid. Replay lowers this to a Metal blit command
     * in the current command buffer before the following sampling draw. */
    D9C_COMMAND_RECORD_GENERATE_MIPMAPS = 30,
};

/* Const-array upload semantics. `kind` selects which dxmt9c_device_set_*_const_*
 * the importer calls; the PE side encodes it as the matching
 * D9C_COMMAND_RECORD_SET_*_CONST_* record type. The fixed-size header struct
 * this annotated is gone -- the payload is a canonical constant section now -- but the
 * type-selects-the-setter contract is unchanged. */

/* R-BACK-2.52: inline const-delta encode/decode helpers. These compute
 * where a Draw* record's six optional const-delta sections' payload bytes
 * live on the wire and validate their register ranges; they do not decide
 * WHEN to fold constants into a draw (PE recorder) or apply sections to
 * server-side state (unix importer) — see specs/backend/spec.md
 * "Inline Const Delta (opt-in)" and specs/backend/requirements.md
 * R-BACK-2.52 for those call sites.
 *
 * Payload placement is no longer chained by hand. The fixed-size Draw* records
 * that used to carry const-delta sections after their trailing vertex/index
 * region were deleted with the legacy format; constants now ride the canonical sparse
 * constant sections, which appendSparseRecord lays out with every other
 * section. What survives here is the range arithmetic these helpers do. */

/* Register-file cap for a const-delta section kind
 * (D9C_DRAW_PACKET_CONST_DELTA_*). Returns 0 for an unrecognized kind,
 * which can therefore never be a valid section. */
static inline uint32_t d9c_draw_packet_const_delta_section_cap(uint32_t kind) {
    switch (kind) {
    case D9C_DRAW_PACKET_CONST_DELTA_VS_F: return D9C_DRAW_PACKET_MAX_CONST_VS_F;
    case D9C_DRAW_PACKET_CONST_DELTA_VS_I: return D9C_DRAW_PACKET_MAX_CONST_VS_I;
    case D9C_DRAW_PACKET_CONST_DELTA_VS_B: return D9C_DRAW_PACKET_MAX_CONST_VS_B;
    case D9C_DRAW_PACKET_CONST_DELTA_PS_F: return D9C_DRAW_PACKET_MAX_CONST_PS_F;
    case D9C_DRAW_PACKET_CONST_DELTA_PS_I: return D9C_DRAW_PACKET_MAX_CONST_PS_I;
    case D9C_DRAW_PACKET_CONST_DELTA_PS_B: return D9C_DRAW_PACKET_MAX_CONST_PS_B;
    default: return 0u;
    }
}


/* R-BACK-2.52(c): validates one section's register range against the D3D9
 * register-file cap for its kind, overflow-safe. A `valid=1` section MUST
 * have registerCount > 0 — a zero-length "valid" range is malformed
 * (producers use valid=0 for "no section" instead). Callers MUST run this
 * before retaining or applying a section's payload bytes; a failing range
 * is a malformed packet and must reject the whole chunk exactly like any
 * other wire bounds violation. */
static inline int d9c_draw_packet_const_delta_section_range_valid(
    uint32_t kind, uint32_t startRegister, uint32_t registerCount) {
    const uint32_t cap = d9c_draw_packet_const_delta_section_cap(kind);
    if (cap == 0u || registerCount == 0u) return 0;
    return startRegister <= cap && registerCount <= cap - startRegister;
}

/* ── Record-kind semantics ───────────────────────────────────────────────────
 *
 * These live D9C_COMMAND_RECORD_* kinds use the canonical pointer-free payload
 * structs above. Every non-null object reference is an absolute uint32 handle-
 * table index whose exact {kind, objectId, generation} identity is resolved
 * before any retain, dispatch, or state mutation.
 * ─────────────────────────────────────────────────────────────────────────── */

/* Standalone clear record. Variable-size: rect array (D9CRect[count])
 * follows the fixed header at rectOffset. count==0 → full-target clear. */

/* Standalone present record. The PE Present(...) call drains pending
 * state + const dirty ranges, appends this record, then commits the
 * chunk synchronously — so the chunk's submission boundary IS the
 * Present boundary. dirty-region payload is omitted (rarely consumed
 * by the backend). The optional src/dst rects ride inline; hasX flags
 * select whether the matching D9CRect is meaningful (zeroed otherwise). */

/* Standalone surface ops reference surfaces through canonical handle-table
 * indices. Rects ride inline; hasX flags select whether the matching D9CRect
 * is meaningful. */

/* Standalone surface-to-surface region copy. dstPoint encodes only an
 * (x, y) offset on the destination — represented as a D9CRect with
 * left/top set and right/bottom equal (importer reads only left/top).
 * hasSrcRect / hasDstPoint mirror StretchRect's hasX flag pattern. */

/* Standalone Query::Issue record. queryHandleIndex selects a
 * D9C_CHUNK_HANDLE_KIND_QUERY entry and flags is the caller's D3DISSUE_* value.
 * The direct query reference rides the record's canonical handle slice; the
 * PE pending-chunk retainer and offload queue each AddRef it for their own
 * ownership interval. Queries remain outside the core resource pool. */

/* Standalone readback record (RT surface → CPU-mappable destination).
 * srcHandleIndex and dstHandleIndex select surface entries from the canonical
 * handle table. Replay dispatches via dxmt9c_device_get_render_target_data,
 * which internally encodes the copy + waits for GPU completion. The HRESULT
 * propagates through commit_chunk's per-record failure short-circuit, so PE
 * receives the readback's actual return code. */

/* Standalone RESZ depth-resolve record. Its live payload is the 8-byte
 * D9CCommandChunkWireReszDepthResolve: msaaDepthHandleIndex selects the bound
 * multisampled depth surface and intzDestHandleIndex selects the bound INTZ
 * destination texture. RESZ writes resolved depth into the stage-0 INTZ
 * texture. Fire-and-forget: like
 * StretchRect/ColorFill it is a surface-op ordering barrier, NOT a
 * synchronous read boundary, so the PE caller does not block on a result. */

/* Ordered automatic-mipmap generation record. textureHandleIndex selects a
 * texture entry from the canonical handle table. Replay enqueues generation
 * in source order on the active Metal command buffer; it is a coordinator
 * blit boundary, not a PE-visible synchronous completion point. */

/* Standalone state-delta record. The packet's draw fields are unused
 * (set to zero by chunkBarrierFlush); only state-delta fields apply.
 * Importer dispatches applyDrawPacketState only — no draw call.
 * Used to carry pending hot state into the chunk before a barrier
 * record (Clear / surface op / readback) so the barrier observes the
 * effective server state. */

typedef struct D9CCommandChunk {
    uint32_t version;
    uint32_t recordCount;
    uint32_t recordBytes;
    D9CWireHandle records;
    uint32_t handleCount;
    D9CWireHandle handles;
    uint64_t renderTapeCaptureToken;
    uint64_t renderTapeEventOrdinal;
} D9CCommandChunk;

typedef enum D9CRenderTapeIdentityCaptureStatus {
    D9C_RENDER_TAPE_IDENTITY_CAPTURE_NONE = 0,
    D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE = 1,
    D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED = 2,
} D9CRenderTapeIdentityCaptureStatus;

typedef struct D9CRenderTapeIdentityCaptureResult {
    uint32_t status;
    uint32_t sourceCount;
    uint32_t rangeCount;
    uint32_t reserved0;
    uint64_t captureToken;
    uint64_t byteCount;
    uint64_t eventOrdinal;
    uint64_t settlementSourceOrdinal;
    uint64_t settlementSeqId;
    uint32_t settlementCount;
    uint32_t reserved1;
    uint32_t settlementEntrySize;
    uint32_t reserved2;
    uint64_t settlementTableOffset;
} D9CRenderTapeIdentityCaptureResult;

typedef struct D9CRenderTapeIdentitySettlementEntry {
    uint64_t eventOrdinal;
    uint64_t rawOrdinal;
    uint64_t buildGeneration;
    uint64_t firstSourceOrdinal;
    uint64_t tailSeqId;
    uint32_t sourceCount;
    uint32_t reserved0;
} D9CRenderTapeIdentitySettlementEntry;

typedef struct D9CRenderTapeIdentitySourceEntry {
    uint64_t eventOrdinal;
    uint64_t sourceOrdinal;
    uint64_t seqId;
    uint64_t captureToken;
    uint32_t firstRecord;
    uint32_t recordCount;
    uint32_t firstRange;
    uint32_t rangeCount;
} D9CRenderTapeIdentitySourceEntry;

typedef struct D9CRenderTapeIdentityRangeEntry {
    uint64_t eventOrdinal;
    uint64_t sourceOrdinal;
    uint64_t seqId;
    uint64_t logicalPassId;
    uint32_t firstRecord;
    uint32_t recordCount;
    uint32_t dagPassIndex;
    uint32_t passKind;
} D9CRenderTapeIdentityRangeEntry;

/* Capture-only PresentComplete output result. This fixed, pointer-free POD
 * crosses the PE/unix boundary only after the captured PRESENT chunk has
 * drained. `sha256` hashes tightly packed canonical logical Present output
 * rows at the captured descriptor extent. The finish call copies those same
 * bytes into an exact-capacity top-level bridge buffer and retains no caller
 * memory after return. */
typedef enum D9CRenderTapePresentCaptureStatus {
    D9C_RENDER_TAPE_PRESENT_CAPTURE_NONE = 0,
    D9C_RENDER_TAPE_PRESENT_CAPTURE_COMPLETE = 1,
    D9C_RENDER_TAPE_PRESENT_CAPTURE_FAILED = 2,
} D9CRenderTapePresentCaptureStatus;

typedef struct D9CRenderTapePresentCaptureResult {
    uint32_t status;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t byteCount;
    uint8_t sha256[32];
} D9CRenderTapePresentCaptureResult;

/* Capture-only source-before-Present result. The source is the retained
 * swap-chain backbuffer that fed the captured Present, read back after the
 * Present has drained. Bytes are tightly packed at the source extent. */
typedef enum D9CRenderTapePresentSourceCaptureStatus {
    D9C_RENDER_TAPE_PRESENT_SOURCE_CAPTURE_NONE = 0,
    D9C_RENDER_TAPE_PRESENT_SOURCE_CAPTURE_COMPLETE = 1,
    D9C_RENDER_TAPE_PRESENT_SOURCE_CAPTURE_FAILED = 2,
} D9CRenderTapePresentSourceCaptureStatus;

typedef struct D9CRenderTapePresentSourceCaptureResult {
    uint32_t status;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t byteCount;
    uint8_t sha256[32];
} D9CRenderTapePresentSourceCaptureResult;

/* Capture-only, synchronous D24X8 arm-boundary snapshot. The output pointer
 * is a top-level bridge argument (never nested inside D9CWireHandle) and the
 * unix provider validates capacity before copying. Encoding version 1 is
 * tightly packed little-endian float32 depth produced by a Metal shader; the
 * physical depth/stencil allocation layout is never exposed on the wire. */
enum {
    D9C_RENDER_TAPE_D24X8_ENCODING_FLOAT32_LE_V1 = 1u,
};

typedef enum D9CRenderTapeD24X8SnapshotStatus {
    D9C_RENDER_TAPE_D24X8_SNAPSHOT_NONE = 0,
    D9C_RENDER_TAPE_D24X8_SNAPSHOT_COMPLETE = 1,
    D9C_RENDER_TAPE_D24X8_SNAPSHOT_STALE_GENERATION = 2,
    D9C_RENDER_TAPE_D24X8_SNAPSHOT_DESCRIPTOR_MISMATCH = 3,
    D9C_RENDER_TAPE_D24X8_SNAPSHOT_UNSUPPORTED = 4,
    D9C_RENDER_TAPE_D24X8_SNAPSHOT_CAPACITY_MISMATCH = 5,
    D9C_RENDER_TAPE_D24X8_SNAPSHOT_READBACK_FAILED = 6,
} D9CRenderTapeD24X8SnapshotStatus;

typedef struct D9CRenderTapeD24X8SnapshotRequest {
    D9CWireObjectIdentity identity;
    D9CSurfaceDesc surface;
    uint32_t encodingVersion;
    uint32_t reserved0;
} D9CRenderTapeD24X8SnapshotRequest;

typedef struct D9CRenderTapeD24X8SnapshotResult {
    uint32_t status;
    uint32_t encodingVersion;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t physicalFormat;
    uint64_t byteCount;
} D9CRenderTapeD24X8SnapshotResult;

/* Capture-only, synchronous color resource snapshot. The identity names an
 * exact generation-qualified standalone X8R8G8B8 surface or a supported
 * texture subresource (X8R8G8B8 2D or R32F cube face). Version 1 is the
 * tightly packed logical four-byte pixel representation. The destination is
 * a validated top-level bridge buffer; no nested pointer crosses the PE/unix
 * boundary. */
enum {
    D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1 = 1u,
};

typedef enum D9CRenderTapeColorSnapshotStatus {
    D9C_RENDER_TAPE_COLOR_SNAPSHOT_NONE = 0,
    D9C_RENDER_TAPE_COLOR_SNAPSHOT_COMPLETE = 1,
    D9C_RENDER_TAPE_COLOR_SNAPSHOT_STALE_GENERATION = 2,
    D9C_RENDER_TAPE_COLOR_SNAPSHOT_DESCRIPTOR_MISMATCH = 3,
    D9C_RENDER_TAPE_COLOR_SNAPSHOT_UNSUPPORTED = 4,
    D9C_RENDER_TAPE_COLOR_SNAPSHOT_CAPACITY_MISMATCH = 5,
    D9C_RENDER_TAPE_COLOR_SNAPSHOT_READBACK_FAILED = 6,
} D9CRenderTapeColorSnapshotStatus;

typedef struct D9CRenderTapeColorSnapshotRequest {
    D9CWireObjectIdentity identity;
    D9CSurfaceDesc surface;
    uint32_t subresource;
    uint32_t encodingVersion;
} D9CRenderTapeColorSnapshotRequest;

typedef struct D9CRenderTapeColorSnapshotResult {
    uint32_t status;
    uint32_t encodingVersion;
    uint32_t subresource;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint32_t reserved0;
    uint64_t byteCount;
} D9CRenderTapeColorSnapshotResult;

/* ── factory ─────────────────────────────────────────────────────────────── */

DXMT9_NODISCARD D9CFactory* dxmt9c_factory_create(void);
void        dxmt9c_factory_addref(D9CFactory*);
uint32_t    dxmt9c_factory_release(D9CFactory*);

uint32_t dxmt9c_factory_adapter_count(D9CFactory*);
DXMT9_NODISCARD int32_t  dxmt9c_factory_get_adapter_identifier(D9CFactory*, uint32_t adapter,
                                                D9CAdapterIdentifier* out);
uint32_t dxmt9c_factory_get_adapter_mode_count(D9CFactory*, uint32_t adapter,
                                                uint32_t fmt);
DXMT9_NODISCARD int32_t  dxmt9c_factory_enum_adapter_modes(D9CFactory*, uint32_t adapter,
                                            uint32_t fmt, uint32_t mode,
                                            uint32_t* outW, uint32_t* outH,
                                            uint32_t* outRefresh, uint32_t* outFmt);
DXMT9_NODISCARD int32_t  dxmt9c_factory_get_adapter_display_mode(D9CFactory*, uint32_t adapter,
                                                  uint32_t* outW, uint32_t* outH,
                                                  uint32_t* outRefresh, uint32_t* outFmt);
uint64_t dxmt9c_factory_get_adapter_monitor(D9CFactory*, uint32_t adapter);
DXMT9_NODISCARD int32_t  dxmt9c_factory_check_device_type(D9CFactory*, uint32_t adapter,
                                           uint32_t devType, uint32_t adapterFmt,
                                           uint32_t backFmt, uint32_t windowed);
DXMT9_NODISCARD int32_t  dxmt9c_factory_check_device_format(D9CFactory*, uint32_t adapter,
                                             uint32_t fmt, uint32_t usage);
DXMT9_NODISCARD int32_t  dxmt9c_factory_check_device_format2(D9CFactory*, uint32_t adapter,
                                              uint32_t fmt, uint32_t usage,
                                              uint32_t resourceType);
DXMT9_NODISCARD int32_t  dxmt9c_factory_check_device_multisample(D9CFactory*, uint32_t adapter,
                                                  uint32_t fmt, uint32_t msType,
                                                  uint32_t windowed);
DXMT9_NODISCARD int32_t  dxmt9c_factory_get_caps(D9CFactory*, uint32_t adapter, D9CCaps* out);
DXMT9_NODISCARD int32_t  dxmt9c_factory_get_adapter_luid(D9CFactory*, uint32_t adapter,
                                          uint32_t* lowPart, int32_t* highPart);

DXMT9_NODISCARD D9CDevice* dxmt9c_factory_create_device(D9CFactory*, uint32_t adapter,
                                         const D9CPresentParams*, uint32_t behaviorFlags,
                                         const D9CDisplayModeEx* fullscreenMode);
DXMT9_NODISCARD int32_t dxmt9c_factory_create_device2(D9CFactory*, uint32_t adapter,
                                       const D9CPresentParams*, uint32_t behaviorFlags,
                                       const D9CDisplayModeEx* fullscreenMode,
                                       D9CDevice** outDevice);

/* ── device ──────────────────────────────────────────────────────────────── */

void     dxmt9c_device_addref(D9CDevice*);
uint32_t dxmt9c_device_release(D9CDevice*);

DXMT9_NODISCARD int32_t dxmt9c_device_negotiate_command_chunk(
    D9CDevice*, D9CCommandChunkNegotiation*);

DXMT9_NODISCARD int32_t  dxmt9c_device_get_caps(D9CDevice*, D9CCaps* out);
DXMT9_NODISCARD int32_t  dxmt9c_device_test_cooperative_level(D9CDevice*);
DXMT9_NODISCARD int32_t  dxmt9c_device_check_device_state(D9CDevice*, uint64_t destWindow);
DXMT9_NODISCARD int32_t  dxmt9c_device_reset(D9CDevice*, const D9CPresentParams*);
DXMT9_NODISCARD int32_t  dxmt9c_device_reset_ex(D9CDevice*, const D9CPresentParams*,
                                 const D9CDisplayModeEx*);
DXMT9_NODISCARD int32_t  dxmt9c_device_present(D9CDevice*, const D9CRect* src, const D9CRect* dst,
                                uint64_t destWindowOverride, const void* dirtyRegion,
                                uint32_t flags);
DXMT9_NODISCARD int32_t  dxmt9c_device_begin_scene(D9CDevice*);
DXMT9_NODISCARD int32_t  dxmt9c_device_end_scene(D9CDevice*);

/* Per-device gamma ramp shadow. The 768 uint16_t payload (3 channels x 256
 * entries) is copied into the server-side core::Device, where snapshotSwapDesc
 * embeds it into every SwapDesc the unix-side Presenter consumes. Null `ramp`
 * is a no-op (matches the void-return D3D9 contract — there is no error
 * channel for callers to observe). */
void     dxmt9c_device_set_gamma_ramp(D9CDevice*, const uint16_t* ramp);

DXMT9_NODISCARD int32_t  dxmt9c_device_clear(D9CDevice*, uint32_t count, const D9CRect* rects,
                              uint32_t flags, uint32_t colorARGB, float z,
                              uint32_t stencil);

DXMT9_NODISCARD int32_t  dxmt9c_device_set_viewport(D9CDevice*, const D9CViewport*);
void     dxmt9c_device_get_viewport(D9CDevice*, D9CViewport*);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_scissor_rect(D9CDevice*, const D9CRect*);
void     dxmt9c_device_get_scissor_rect(D9CDevice*, D9CRect*);

DXMT9_NODISCARD int32_t  dxmt9c_device_set_transform(D9CDevice*, uint32_t state, const D9CMatrix*);
DXMT9_NODISCARD int32_t  dxmt9c_device_get_transform(D9CDevice*, uint32_t state, D9CMatrix*);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_material(D9CDevice*, const D9CMaterial*);
DXMT9_NODISCARD int32_t  dxmt9c_device_get_material(D9CDevice*, D9CMaterial*);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_light(D9CDevice*, uint32_t index, const D9CLight*);
DXMT9_NODISCARD int32_t  dxmt9c_device_light_enable(D9CDevice*, uint32_t index, uint32_t enable);

DXMT9_NODISCARD int32_t  dxmt9c_device_set_render_state(D9CDevice*, uint32_t state, uint32_t value);
uint32_t dxmt9c_device_get_render_state(D9CDevice*, uint32_t state);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_texture_stage_state(D9CDevice*, uint32_t stage,
                                                uint32_t type, uint32_t value);
uint32_t dxmt9c_device_get_texture_stage_state(D9CDevice*, uint32_t stage,
                                                uint32_t type);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_sampler_state(D9CDevice*, uint32_t sampler,
                                          uint32_t type, uint32_t value);
uint32_t dxmt9c_device_get_sampler_state(D9CDevice*, uint32_t sampler, uint32_t type);

DXMT9_NODISCARD int32_t  dxmt9c_device_set_clip_plane(D9CDevice*, uint32_t index,
                                       const float plane[4]);
DXMT9_NODISCARD int32_t  dxmt9c_device_get_clip_plane(D9CDevice*, uint32_t index, float plane[4]);

DXMT9_NODISCARD int32_t  dxmt9c_device_set_fvf(D9CDevice*, uint32_t fvf);
uint32_t dxmt9c_device_get_fvf(D9CDevice*);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_vertex_declaration(D9CDevice*, D9CVertexDecl*);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_stream_source(D9CDevice*, uint32_t stream,
                                          D9CBuffer*, uint32_t offset, uint32_t stride);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_stream_source_freq(D9CDevice*, uint32_t stream,
                                               uint32_t freq);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_indices(D9CDevice*, D9CBuffer*);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_texture(D9CDevice*, uint32_t stage, D9CTexture*);

DXMT9_NODISCARD int32_t  dxmt9c_device_set_vertex_shader(D9CDevice*, D9CShader*);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_pixel_shader(D9CDevice*, D9CShader*);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_vs_const_f(D9CDevice*, uint32_t start,
                                       const float* data, uint32_t count);
DXMT9_NODISCARD int32_t  dxmt9c_device_get_vs_const_f(D9CDevice*, uint32_t start,
                                       float* data, uint32_t count);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_ps_const_f(D9CDevice*, uint32_t start,
                                       const float* data, uint32_t count);
DXMT9_NODISCARD int32_t  dxmt9c_device_get_ps_const_f(D9CDevice*, uint32_t start,
                                       float* data, uint32_t count);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_vs_const_i(D9CDevice*, uint32_t start,
                                       const int32_t* data, uint32_t count);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_ps_const_i(D9CDevice*, uint32_t start,
                                       const int32_t* data, uint32_t count);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_vs_const_b(D9CDevice*, uint32_t start,
                                       const uint32_t* data, uint32_t count);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_ps_const_b(D9CDevice*, uint32_t start,
                                       const uint32_t* data, uint32_t count);

DXMT9_NODISCARD int32_t  dxmt9c_device_set_render_target(D9CDevice*, uint32_t index, D9CSurface*);
DXMT9_NODISCARD D9CSurface* dxmt9c_device_get_render_target(D9CDevice*, uint32_t index);
DXMT9_NODISCARD int32_t  dxmt9c_device_set_depth_stencil(D9CDevice*, D9CSurface*);
DXMT9_NODISCARD D9CSurface* dxmt9c_device_get_depth_stencil(D9CDevice*);

DXMT9_NODISCARD int32_t  dxmt9c_device_draw_primitive(D9CDevice*, uint32_t type,
                                       uint32_t startVertex, uint32_t count);
DXMT9_NODISCARD int32_t  dxmt9c_device_commit_chunk(D9CDevice*, const D9CCommandChunk*);
DXMT9_NODISCARD int32_t  dxmt9c_device_commit_chunk_segmented(
    D9CDevice*, const D9CCommandChunkSegmentedTransportV1*);
DXMT9_NODISCARD int32_t  dxmt9c_device_reserve_render_tape_present_capture(D9CDevice*);
DXMT9_NODISCARD int32_t  dxmt9c_device_finish_render_tape_present_capture(
    D9CDevice*, D9CRenderTapePresentCaptureResult* out, void* bytes,
    uint64_t capacity);
DXMT9_NODISCARD int32_t
dxmt9c_device_finish_render_tape_present_source_capture(
    D9CDevice*, D9CRenderTapePresentSourceCaptureResult* out, void* bytes,
    uint64_t capacity);
DXMT9_NODISCARD int32_t dxmt9c_device_finish_render_tape_identity_capture(
    D9CDevice*, uint64_t captureToken,
    D9CRenderTapeIdentityCaptureResult* out, void* bytes, uint64_t capacity);
void dxmt9c_device_cancel_render_tape_present_capture(D9CDevice*);
DXMT9_NODISCARD int32_t dxmt9c_device_capture_render_tape_d24x8_snapshot(
    D9CDevice*, const D9CRenderTapeD24X8SnapshotRequest* request,
    D9CRenderTapeD24X8SnapshotResult* out, void* bytes, uint64_t capacity);
DXMT9_NODISCARD int32_t dxmt9c_device_capture_render_tape_color_snapshot(
    D9CDevice*, const D9CRenderTapeColorSnapshotRequest* request,
    D9CRenderTapeColorSnapshotResult* out, void* bytes, uint64_t capacity);
DXMT9_NODISCARD int32_t  dxmt9c_device_draw_indexed_primitive(D9CDevice*, uint32_t type,
                                               int32_t baseVertex, uint32_t minVertex,
                                               uint32_t numVertices, uint32_t startIndex,
                                               uint32_t count);
DXMT9_NODISCARD int32_t  dxmt9c_device_draw_primitive_up(D9CDevice*, uint32_t type, uint32_t count,
                                          const void* data, uint32_t stride);
DXMT9_NODISCARD int32_t  dxmt9c_device_draw_indexed_primitive_up(D9CDevice*, uint32_t type,
                                                  uint32_t minVertex, uint32_t numVertices,
                                                  uint32_t count, const void* indexData,
                                                  uint32_t indexFmt,
                                                  const void* vertexData, uint32_t stride);

DXMT9_NODISCARD int32_t  dxmt9c_device_update_surface(D9CDevice*, D9CSurface* src,
                                       const D9CRect* srcRect,
                                       D9CSurface* dst, const D9CRect* dstPt);
DXMT9_NODISCARD int32_t  dxmt9c_device_update_texture(D9CDevice*, D9CTexture* src, D9CTexture* dst);
DXMT9_NODISCARD int32_t  dxmt9c_device_stretch_rect(D9CDevice*, D9CSurface* src, const D9CRect* srcRect,
                                     D9CSurface* dst, const D9CRect* dstRect, uint32_t filter);
DXMT9_NODISCARD int32_t  dxmt9c_device_color_fill(D9CDevice*, D9CSurface*, const D9CRect*, uint32_t colorARGB);
DXMT9_NODISCARD int32_t  dxmt9c_device_get_render_target_data(D9CDevice*, D9CSurface* rt, D9CSurface* dst);

DXMT9_NODISCARD int32_t     dxmt9c_device_set_maximum_frame_latency(D9CDevice*, uint32_t);
uint32_t    dxmt9c_device_get_maximum_frame_latency(D9CDevice*);
DXMT9_NODISCARD int32_t     dxmt9c_device_wait_for_vblank(D9CDevice*, uint32_t swapChainIndex);
DXMT9_NODISCARD int32_t     dxmt9c_device_check_device_multisample(D9CDevice*, uint32_t fmt,
                                                    uint32_t msType, uint32_t windowed);
DXMT9_NODISCARD D9CSwapChain* dxmt9c_device_get_swap_chain(D9CDevice*, uint32_t index);
uint32_t    dxmt9c_device_get_swap_chain_count(D9CDevice*);
DXMT9_NODISCARD D9CSwapChain* dxmt9c_device_create_additional_swap_chain(D9CDevice*,
                                                          const D9CPresentParams*);
DXMT9_NODISCARD int32_t dxmt9c_swapchain_adopt_wsi_surface(
    D9CSwapChain*, const D9CWsiSurfaceBinding*);
DXMT9_NODISCARD int32_t dxmt9c_swapchain_teardown_wsi_surface(D9CSwapChain*);

/* ── resource creation ───────────────────────────────────────────────────── */

DXMT9_NODISCARD D9CTexture* dxmt9c_device_create_texture(D9CDevice*, uint32_t w, uint32_t h,
                                          uint32_t levels, uint32_t usage,
                                          uint32_t fmt, uint32_t pool);
DXMT9_NODISCARD D9CTexture* dxmt9c_device_create_cube_texture(D9CDevice*, uint32_t size,
                                               uint32_t levels, uint32_t usage,
                                               uint32_t fmt, uint32_t pool);
DXMT9_NODISCARD D9CTexture* dxmt9c_device_create_volume_texture(D9CDevice*, uint32_t w, uint32_t h,
                                                 uint32_t d, uint32_t levels,
                                                 uint32_t usage, uint32_t fmt,
                                                 uint32_t pool);
DXMT9_NODISCARD D9CBuffer*  dxmt9c_device_create_vertex_buffer(D9CDevice*, uint32_t length,
                                                uint32_t usage, uint32_t fvf,
                                                uint32_t pool);
DXMT9_NODISCARD D9CBuffer*  dxmt9c_device_create_index_buffer(D9CDevice*, uint32_t length,
                                               uint32_t usage, uint32_t fmt,
                                               uint32_t pool);
DXMT9_NODISCARD D9CTexture* dxmt9c_device_create_texture_shared(D9CDevice*, uint32_t w, uint32_t h,
                                                 uint32_t levels, uint32_t usage,
                                                 uint32_t fmt, uint32_t pool,
                                                 uint64_t* sharedHandle);
DXMT9_NODISCARD D9CTexture* dxmt9c_device_create_cube_texture_shared(D9CDevice*, uint32_t size,
                                                      uint32_t levels, uint32_t usage,
                                                      uint32_t fmt, uint32_t pool,
                                                      uint64_t* sharedHandle);
DXMT9_NODISCARD D9CTexture* dxmt9c_device_create_volume_texture_shared(D9CDevice*, uint32_t w, uint32_t h,
                                                        uint32_t d, uint32_t levels,
                                                        uint32_t usage, uint32_t fmt,
                                                        uint32_t pool,
                                                        uint64_t* sharedHandle);
DXMT9_NODISCARD D9CBuffer*  dxmt9c_device_create_vertex_buffer_shared(D9CDevice*, uint32_t length,
                                                       uint32_t usage, uint32_t fvf,
                                                       uint32_t pool,
                                                       uint64_t* sharedHandle);
DXMT9_NODISCARD D9CBuffer*  dxmt9c_device_create_index_buffer_shared(D9CDevice*, uint32_t length,
                                                      uint32_t usage, uint32_t fmt,
                                                      uint32_t pool,
                                                      uint64_t* sharedHandle);
DXMT9_NODISCARD D9CSurface* dxmt9c_device_create_render_target(D9CDevice*, uint32_t w, uint32_t h,
                                                uint32_t fmt, uint32_t msType,
                                                uint32_t msQuality, uint32_t lockable,
                                                uint64_t* sharedHandle);
DXMT9_NODISCARD D9CSurface* dxmt9c_device_create_depth_stencil(D9CDevice*, uint32_t w, uint32_t h,
                                                uint32_t fmt, uint32_t msType,
                                                uint32_t msQuality, uint32_t discard,
                                                uint64_t* sharedHandle);
DXMT9_NODISCARD D9CSurface* dxmt9c_device_create_offscreen_surface(D9CDevice*, uint32_t w, uint32_t h,
                                                    uint32_t fmt, uint32_t pool,
                                                    uint64_t* sharedHandle);

DXMT9_NODISCARD D9CShader*  dxmt9c_device_create_vertex_shader(D9CDevice*, const uint32_t* bytecode);
DXMT9_NODISCARD D9CShader*  dxmt9c_device_create_pixel_shader(D9CDevice*, const uint32_t* bytecode);

DXMT9_NODISCARD D9CVertexDecl* dxmt9c_device_create_vertex_declaration(D9CDevice*,
                                                         const D9CVertexElement*);

DXMT9_NODISCARD D9CQuery*      dxmt9c_device_create_query(D9CDevice*, uint32_t type);
DXMT9_NODISCARD D9CStateBlock* dxmt9c_device_create_state_block(D9CDevice*, uint32_t type);
DXMT9_NODISCARD int32_t        dxmt9c_device_begin_state_block(D9CDevice*);
DXMT9_NODISCARD int32_t        dxmt9c_device_end_state_block(D9CDevice*, D9CStateBlock**);

/* ── swap chain ──────────────────────────────────────────────────────────── */

void     dxmt9c_swapchain_addref(D9CSwapChain*);
uint32_t dxmt9c_swapchain_release(D9CSwapChain*);
DXMT9_NODISCARD int32_t  dxmt9c_swapchain_present(D9CSwapChain*, const D9CRect* src,
                                   const D9CRect* dst, uint64_t destWindow,
                                   const void* dirtyRegion, uint32_t flags);
DXMT9_NODISCARD D9CSurface* dxmt9c_swapchain_get_back_buffer(D9CSwapChain*, uint32_t index,
                                              uint32_t type);
DXMT9_NODISCARD D9CSurface* dxmt9c_swapchain_get_depth_stencil(D9CSwapChain*);
DXMT9_NODISCARD int32_t  dxmt9c_swapchain_get_present_params(D9CSwapChain*, D9CPresentParams*);

/* ── texture ─────────────────────────────────────────────────────────────── */

void     dxmt9c_texture_addref(D9CTexture*);
uint32_t dxmt9c_texture_release(D9CTexture*);
DXMT9_NODISCARD int32_t dxmt9c_texture_get_wire_identity(
    D9CTexture*, D9CWireObjectIdentity* out);
DXMT9_NODISCARD int32_t  dxmt9c_texture_lock_rect(D9CTexture*, uint32_t level, D9CLockedRect* out,
                                   const D9CRect*, uint32_t flags);
DXMT9_NODISCARD int32_t  dxmt9c_texture_unlock_rect(D9CTexture*, uint32_t level);
DXMT9_NODISCARD D9CSurface* dxmt9c_texture_get_surface_level(D9CTexture*, uint32_t level);
uint32_t dxmt9c_texture_get_level_count(D9CTexture*);
DXMT9_NODISCARD int32_t  dxmt9c_texture_get_level_desc(D9CTexture*, uint32_t level, D9CSurfaceDesc*);
DXMT9_NODISCARD int32_t  dxmt9c_texture_generate_mip_sublevels(D9CTexture*);
uint32_t dxmt9c_texture_set_lod(D9CTexture*, uint32_t lod);
DXMT9_NODISCARD int32_t  dxmt9c_texture_sample_2d(D9CTexture*, uint32_t level,
                                      float u, float v, float* outRgba4);
DXMT9_NODISCARD int32_t  dxmt9c_texture_set_palette(D9CTexture*,
                                      const uint32_t* argbEntries,
                                      uint32_t entryCount);

/* ── buffer ──────────────────────────────────────────────────────────────── */

void     dxmt9c_buffer_addref(D9CBuffer*);
uint32_t dxmt9c_buffer_release(D9CBuffer*);
DXMT9_NODISCARD int32_t dxmt9c_buffer_get_wire_identity(
    D9CBuffer*, D9CWireObjectIdentity* out);
DXMT9_NODISCARD int32_t  dxmt9c_buffer_lock(D9CBuffer*, uint32_t offset, uint32_t size,
                             void** data, uint32_t flags);
DXMT9_NODISCARD int32_t  dxmt9c_buffer_unlock(D9CBuffer*);
DXMT9_NODISCARD int32_t  dxmt9c_buffer_get_desc(D9CBuffer*, D9CBufferDesc*);

/* ── surface ─────────────────────────────────────────────────────────────── */

void     dxmt9c_surface_addref(D9CSurface*);
uint32_t dxmt9c_surface_release(D9CSurface*);
DXMT9_NODISCARD int32_t dxmt9c_surface_get_wire_identity(
    D9CSurface*, D9CWireObjectIdentity* out);
DXMT9_NODISCARD int32_t  dxmt9c_surface_lock_rect(D9CSurface*, D9CLockedRect*, const D9CRect*, uint32_t flags);
DXMT9_NODISCARD int32_t  dxmt9c_surface_unlock_rect(D9CSurface*);
DXMT9_NODISCARD int32_t  dxmt9c_surface_get_desc(D9CSurface*, D9CSurfaceDesc*);
DXMT9_NODISCARD D9CTexture* dxmt9c_surface_get_container_texture(D9CSurface*);

/* ── shader ──────────────────────────────────────────────────────────────── */

void     dxmt9c_shader_addref(D9CShader*);
uint32_t dxmt9c_shader_release(D9CShader*);
DXMT9_NODISCARD int32_t dxmt9c_shader_get_wire_identity(
    D9CShader*, D9CWireObjectIdentity* out);
DXMT9_NODISCARD int32_t  dxmt9c_shader_get_bytecode(D9CShader*, void* data, uint32_t* size);

/* ── vertex declaration ──────────────────────────────────────────────────── */

void     dxmt9c_vdecl_addref(D9CVertexDecl*);
uint32_t dxmt9c_vdecl_release(D9CVertexDecl*);
DXMT9_NODISCARD int32_t dxmt9c_vdecl_get_wire_identity(
    D9CVertexDecl*, D9CWireObjectIdentity* out);

/* ── query ───────────────────────────────────────────────────────────────── */

void     dxmt9c_query_addref(D9CQuery*);
uint32_t dxmt9c_query_release(D9CQuery*);
DXMT9_NODISCARD int32_t dxmt9c_query_get_wire_identity(
    D9CQuery*, D9CWireObjectIdentity* out);
DXMT9_NODISCARD int32_t  dxmt9c_query_issue(D9CQuery*, uint32_t flags);
DXMT9_NODISCARD int32_t  dxmt9c_query_get_data(D9CQuery*, void* data, uint32_t size, uint32_t flags);
uint32_t dxmt9c_query_get_data_size(D9CQuery*);
uint32_t dxmt9c_query_get_type(D9CQuery*);

/* ── state block ─────────────────────────────────────────────────────────── */

void     dxmt9c_stateblock_addref(D9CStateBlock*);
uint32_t dxmt9c_stateblock_release(D9CStateBlock*);
DXMT9_NODISCARD int32_t  dxmt9c_stateblock_capture(D9CStateBlock*);
DXMT9_NODISCARD int32_t  dxmt9c_stateblock_apply(D9CStateBlock*);

/* ── vertex declaration ──────────────────────────────────────────────────── */

DXMT9_NODISCARD int32_t  dxmt9c_vdecl_get_declaration(D9CVertexDecl*, D9CVertexElement* out,
                                       uint32_t* count);

#ifdef __cplusplus
}
#endif
