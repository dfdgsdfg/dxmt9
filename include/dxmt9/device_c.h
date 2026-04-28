/* dxmt9/device_c.h -- C ABI bridge between the PE winemetal.dll bridge and
 * the unix-side winemetal.so module. All types use stdint / plain C so this
 * header is safe to include from both Apple clang (Mach-O) and llvm-mingw
 * (PE) compilations.
 *
 * Enum fields carry raw D3D9 wire values (e.g. D3DFORMAT = 21 for A8R8G8B8).
 * Conversion to internal dxmt9 types happens inside device_c.cpp. */

#pragma once
#include <stdint.h>
#include <stddef.h>

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
    uint32_t width, height;
} D9CSurfaceDesc;

typedef struct D9CVertexElement {
    uint16_t stream, offset;
    uint8_t  type, method, usage, usageIndex;
} D9CVertexElement;

/* Fixed-width command packet fields. Handles are split into two uint32_t
 * lanes so the packet layout is identical for 32-bit PE, WoW64, and 64-bit
 * unix-side consumers. */
#define D9C_DRAW_PACKET_MAX_RENDER_STATES 64
#define D9C_DRAW_PACKET_MAX_TEXTURES 16
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

typedef struct D9CDrawPacketRenderState {
    uint32_t state;
    uint32_t value;
} D9CDrawPacketRenderState;

/* Phase 12: per-stage TSS / per-sampler scalar deltas (mirrors the
 * D9CDrawPacketRenderState shape). PE recorder accumulates dirty
 * (stage,type)→value tuples; the next packet ships them and the
 * server-side dispatcher applies via dxmt9c_device_set_*. */
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

typedef struct D9CDrawPacketStreamSource {
    D9CWireHandle buffer;
    uint32_t offset;
    uint32_t stride;
} D9CDrawPacketStreamSource;

/* Canonical wire format: delta + PE shadow.
 *
 * Each *Valid / *Mask / *Count field carries the DELTA since the last
 * draw packet — only fields the PE recorder marked dirty are
 * semantically meaningful. The PE state shadow (default ON via Phase
 * 22) is the source of truth for any field a packet leaves zero;
 * server-side D9CDevice maintains the matching server shadow that the
 * importer mutates as it dispatches each packet's set bits.
 *
 * Run-coalescing (drawPrimitiveRun fast path) keys off "every Valid /
 * Mask / Count is zero" to detect consecutive draws that share state,
 * so the delta encoding is load-bearing for that optimization.
 *
 * The full-snapshot wire mode (DXMT9_PE_DRAW_FULL_SNAPSHOT=1, Phase
 * 16) overrides this contract: every field is forced valid +
 * populated from the PE shadow, making each packet self-contained at
 * the cost of wire bandwidth + disabled run-coalescing. Default OFF —
 * intended for stress testing, debugging out-of-order replay, or
 * environments where importer statelessness matters more than wire
 * efficiency. */
typedef struct D9CDrawPrimitivePacket {
    uint32_t renderStateCount;
    D9CDrawPacketRenderState renderStates[D9C_DRAW_PACKET_MAX_RENDER_STATES];
    uint32_t textureMask;
    D9CWireHandle textures[D9C_DRAW_PACKET_MAX_TEXTURES];
    uint32_t streamSourceMask;
    D9CDrawPacketStreamSource streamSources[D9C_DRAW_PACKET_MAX_STREAMS];
    uint32_t fvfValid;
    uint32_t fvf;
    /* Phase 12: shader handles ride on the draw packet so SetVertexShader /
     * SetPixelShader can be PE-shadow-only — no per-Set unix-call. The
     * vsValid / psValid flags are 1 iff the shader changed since the last
     * draw packet (delta semantics like fvfValid). */
    uint32_t vsValid;
    D9CWireHandle vsHandle;
    uint32_t psValid;
    D9CWireHandle psHandle;
    /* Phase 12: vertex declaration handle delta (alternative to fvf). */
    uint32_t vdeclValid;
    D9CWireHandle vdeclHandle;
    /* Phase 12: render target / depth-stencil handle deltas.
     * rtMask bit i set ⇒ rtHandles[i] is the new RT for slot i.
     * dsValid==1 ⇒ dsHandle is the new depth-stencil surface (may be 0
     * to detach). Each ride as a server-side D9CSurface* wire pointer. */
    uint32_t rtMask;
    D9CWireHandle rtHandles[D9C_DRAW_PACKET_MAX_RENDER_TARGETS];
    uint32_t dsValid;
    D9CWireHandle dsHandle;
    /* Phase 12: viewport / scissor deltas. SetViewport / SetScissorRect
     * become PE-shadow-only; the next packet ships the snapshot. */
    uint32_t viewportValid;
    D9CViewport viewport;
    uint32_t scissorValid;
    D9CRect    scissor;
    /* Phase 12: per-stage TSS + per-sampler scalar setter deltas. */
    uint32_t tssCount;
    D9CDrawPacketTextureStageState tss[D9C_DRAW_PACKET_MAX_TSS];
    uint32_t samplerStateCount;
    D9CDrawPacketSamplerState samplerStates[D9C_DRAW_PACKET_MAX_SAMPLER];
    /* Phase 12: SetMaterial — single fixed struct + valid flag. */
    uint32_t materialValid;
    D9CMaterial material;
    /* Phase 12: SetClipPlane — bit i of clipPlaneMask set ⇒
     * clipPlanes[i*4..i*4+3] is the new plane equation for index i.
     * 6 planes × 4 floats = 96 bytes. */
    uint32_t clipPlaneMask;
    float clipPlanes[6 * 4];
    /* Phase 12: SetTransform — variable-count of (state, matrix). */
    uint32_t transformCount;
    D9CDrawPacketTransform transforms[D9C_DRAW_PACKET_MAX_TRANSFORMS];
    /* Phase 12: SetLight + LightEnable. lightSlotMask bit i ⇒ lights[i]
     * is a fresh D9CLight for index i; lightEnableValidMask bit i ⇒
     * lightEnableMask bit i carries the new enabled state for index i. */
    uint32_t lightSlotMask;
    D9CLight lights[D9C_DRAW_PACKET_MAX_LIGHTS];
    uint32_t lightEnableValidMask;
    uint32_t lightEnableMask;
    uint32_t primitiveType;
    uint32_t startVertex;
    uint32_t primitiveCount;
} D9CDrawPrimitivePacket;

typedef struct D9CDrawIndexedPrimitivePacket {
    D9CDrawPrimitivePacket state;
    int32_t baseVertex;
    uint32_t minVertex;
    uint32_t numVertices;
    uint32_t startIndex;
    uint32_t primitiveCount;
    /* Phase 12: index buffer handle delta. ibValid==1 ⇒ ibHandle is the
     * new index buffer (D9CBuffer* wire); applied via
     * dxmt9c_device_set_indices before drawIndexedPrimitive. */
    uint32_t ibValid;
    D9CWireHandle ibHandle;
} D9CDrawIndexedPrimitivePacket;

typedef struct D9CDrawPrimitiveUPPacket {
    D9CDrawPrimitivePacket state;
    uint32_t primitiveCount;
    uint32_t stride;
    uint32_t vertexDataOffset;
    uint32_t vertexDataSize;
} D9CDrawPrimitiveUPPacket;

typedef struct D9CDrawIndexedPrimitiveUPPacket {
    D9CDrawPrimitivePacket state;
    uint32_t minVertex;
    uint32_t numVertices;
    uint32_t primitiveCount;
    uint32_t indexFormat;
    uint32_t stride;
    uint32_t indexDataOffset;
    uint32_t indexDataSize;
    uint32_t vertexDataOffset;
    uint32_t vertexDataSize;
} D9CDrawIndexedPrimitiveUPPacket;

#define D9C_COMMAND_CHUNK_VERSION 1u

/* Per-chunk resource retention list. PE recorder accumulates the deduped
 * set of resource handles touched by Set{Texture,StreamSource,Indices,
 * RenderTarget,DepthStencil,VertexShader,PixelShader,VertexDeclaration}
 * calls during chunk build, then serializes the entries into
 * D9CCommandChunk.handles[] at commit time. Server-side importer reads
 * the list to mark resource lifetimes against the chunk's seqId in ONE
 * pass — replaces N×per-record markDrawResources walks once the
 * importer is wired to skip per-record marking. */
enum {
    D9C_CHUNK_HANDLE_KIND_TEXTURE = 0,
    D9C_CHUNK_HANDLE_KIND_SURFACE = 1,
    D9C_CHUNK_HANDLE_KIND_BUFFER = 2,
    D9C_CHUNK_HANDLE_KIND_SHADER = 3,
    D9C_CHUNK_HANDLE_KIND_VERTEX_DECL = 4,
};

typedef struct D9CChunkHandleEntry {
    uint32_t kind;
    uint32_t reserved;
    uint64_t handle;
} D9CChunkHandleEntry;

enum {
    D9C_COMMAND_RECORD_DRAW_PRIMITIVE = 1,
    D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE = 2,
    D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP = 3,
    D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP = 4,
    /* Note: IDs 5..13 (per-call Set* setter-replay records) were removed
     * — that shape violated the recorder design (Set* state setters must
     * be PE-shadow-only and embedded into the next DrawRecord, not
     * emitted as separate backend commands). PE side reverted to the
     * legacy unix-call path for those setters until DrawRecord is
     * extended to carry the full snapshot.
     *
     * Variable-size const-array uploads. Each appends `count*kElemSize`
     * bytes after the fixed header — float[count*4] for *F, int32[count*4]
     * for *I, uint32[count] for *B. Currently emitted per Set*Constant*
     * call; should evolve to dirty-range emission at draw/chunk-flush
     * time per the redesign (PE shadow accumulates ranges, single record
     * per (stage,type) at flush). Tracked as Phase 2 follow-up. */
    D9C_COMMAND_RECORD_SET_VS_CONST_F = 14,
    D9C_COMMAND_RECORD_SET_VS_CONST_I = 15,
    D9C_COMMAND_RECORD_SET_VS_CONST_B = 16,
    D9C_COMMAND_RECORD_SET_PS_CONST_F = 17,
    D9C_COMMAND_RECORD_SET_PS_CONST_I = 18,
    D9C_COMMAND_RECORD_SET_PS_CONST_B = 19,
    /* Standalone ordering ops — recorder design's "Clear / Present /
     * Query / Surface" branch. Currently only Clear is wired; the
     * others still bypass the chunk via per-call dxmt9c_device_*
     * unix-calls (follow-up). */
    D9C_COMMAND_RECORD_CLEAR = 20,
    D9C_COMMAND_RECORD_PRESENT = 21,
    D9C_COMMAND_RECORD_STRETCH_RECT = 22,
    D9C_COMMAND_RECORD_COLOR_FILL = 23,
    D9C_COMMAND_RECORD_UPDATE_TEXTURE = 24,
    D9C_COMMAND_RECORD_UPDATE_SURFACE = 25,
    /* Phase 20: Query::Issue (D3DISSUE_BEGIN / D3DISSUE_END). Fire-and-
     * forget at PE level — server records the begin/end into the query
     * object. Query::GetData stays on flush+bridge because it needs to
     * synchronously return the recorded data to the caller. */
    D9C_COMMAND_RECORD_QUERY_ISSUE = 26,
};

typedef struct D9CCommandRecordHeader {
    uint32_t type;
    uint32_t size;
} D9CCommandRecordHeader;

typedef struct D9CCommandRecordDrawPrimitive {
    D9CCommandRecordHeader header;
    D9CDrawPrimitivePacket packet;
} D9CCommandRecordDrawPrimitive;

typedef struct D9CCommandRecordDrawIndexedPrimitive {
    D9CCommandRecordHeader header;
    D9CDrawIndexedPrimitivePacket packet;
} D9CCommandRecordDrawIndexedPrimitive;

typedef struct D9CCommandRecordDrawPrimitiveUP {
    D9CCommandRecordHeader header;
    D9CDrawPrimitiveUPPacket packet;
    /* Vertex bytes follow this fixed header at packet.vertexDataOffset. */
} D9CCommandRecordDrawPrimitiveUP;

typedef struct D9CCommandRecordDrawIndexedPrimitiveUP {
    D9CCommandRecordHeader header;
    D9CDrawIndexedPrimitiveUPPacket packet;
    /* Index and vertex bytes follow this fixed header at the packet offsets. */
} D9CCommandRecordDrawIndexedPrimitiveUP;

/* Variable-size header for const-array uploads. The element payload follows
 * immediately after this struct. `kind` selects which dxmt9c_device_set_*_const_*
 * to call; PE side encodes it as the matching D9C_COMMAND_RECORD_SET_*_CONST_*
 * type so the decoder can validate header.size against count*kElemSize. */
typedef struct D9CCommandRecordSetConst {
    D9CCommandRecordHeader header;
    uint32_t start;
    uint32_t count;
    /* For *_CONST_F: float[count*4]
     * For *_CONST_I: int32[count*4]
     * For *_CONST_B: uint32[count] */
} D9CCommandRecordSetConst;

/* Standalone clear record. Variable-size: rect array (D9CRect[count])
 * follows the fixed header at rectOffset. count==0 → full-target clear. */
typedef struct D9CCommandRecordClear {
    D9CCommandRecordHeader header;
    uint32_t flags;        /* D3DCLEAR_TARGET / DEPTH / STENCIL flags */
    uint32_t colorARGB;
    float    z;
    uint32_t stencil;
    uint32_t rectCount;
    uint32_t rectOffset;   /* byte offset into this record (after header) */
} D9CCommandRecordClear;

/* Standalone present record. The PE Present(...) call drains pending
 * state + const dirty ranges, appends this record, then commits the
 * chunk synchronously — so the chunk's submission boundary IS the
 * Present boundary. dirty-region payload is omitted (rarely consumed
 * by the backend). The optional src/dst rects ride inline; hasX flags
 * select whether the matching D9CRect is meaningful (zeroed otherwise). */
typedef struct D9CCommandRecordPresent {
    D9CCommandRecordHeader header;
    uint64_t hwnd;
    uint32_t flags;
    uint32_t hasSrc;
    uint32_t hasDst;
    uint32_t reserved;
    D9CRect  src;
    D9CRect  dst;
} D9CCommandRecordPresent;

/* Standalone surface ops. Surface pointers are SERVER-SIDE D9CSurface*
 * cast to uint64 (importer decodes via wrapper->obj->handle() the same
 * way as chunk.handles[]). Both rects ride inline; hasX flags select
 * whether the matching D9CRect is meaningful. */
typedef struct D9CCommandRecordStretchRect {
    D9CCommandRecordHeader header;
    uint64_t srcWire;
    uint64_t dstWire;
    uint32_t hasSrcRect;
    uint32_t hasDstRect;
    uint32_t filter;
    uint32_t reserved;
    D9CRect  srcRect;
    D9CRect  dstRect;
} D9CCommandRecordStretchRect;

typedef struct D9CCommandRecordColorFill {
    D9CCommandRecordHeader header;
    uint64_t surfaceWire;
    uint32_t colorARGB;
    uint32_t hasRect;
    D9CRect  rect;
} D9CCommandRecordColorFill;

typedef struct D9CCommandRecordUpdateTexture {
    D9CCommandRecordHeader header;
    uint64_t srcWire;        /* SERVER-SIDE D9CTexture* cast */
    uint64_t dstWire;        /* SERVER-SIDE D9CTexture* cast */
} D9CCommandRecordUpdateTexture;

/* Standalone surface-to-surface region copy. dstPoint encodes only an
 * (x, y) offset on the destination — represented as a D9CRect with
 * left/top set and right/bottom equal (importer reads only left/top).
 * hasSrcRect / hasDstPoint mirror StretchRect's hasX flag pattern. */
typedef struct D9CCommandRecordUpdateSurface {
    D9CCommandRecordHeader header;
    uint64_t srcWire;        /* SERVER-SIDE D9CSurface* cast */
    uint64_t dstWire;        /* SERVER-SIDE D9CSurface* cast */
    uint32_t hasSrcRect;
    uint32_t hasDstPoint;
    D9CRect  srcRect;
    D9CRect  dstPoint;
} D9CCommandRecordUpdateSurface;

/* Standalone Query::Issue record. queryWire is the SERVER-SIDE D9CQuery*
 * cast to uint64; importer round-trips back via reinterpret_cast.
 * flags is the D3DISSUE_* value from the caller. Query objects aren't
 * pool-tracked the same way as textures/buffers, so they don't ride on
 * chunk.handles[]; the PE-side Query wrapper holds its own AddRef on
 * the D9CQuery, keeping it alive across chunk lifetimes. */
typedef struct D9CCommandRecordQueryIssue {
    D9CCommandRecordHeader header;
    uint64_t queryWire;
    uint32_t flags;
    uint32_t reserved;
} D9CCommandRecordQueryIssue;

typedef struct D9CCommandChunk {
    uint32_t version;
    uint32_t recordCount;
    uint32_t recordBytes;
    D9CWireHandle records;
    uint32_t handleCount;
    D9CWireHandle handles;
} D9CCommandChunk;

/* ── factory ─────────────────────────────────────────────────────────────── */

D9CFactory* dxmt9c_factory_create(void);
void        dxmt9c_factory_addref(D9CFactory*);
uint32_t    dxmt9c_factory_release(D9CFactory*);

uint32_t dxmt9c_factory_adapter_count(D9CFactory*);
int32_t  dxmt9c_factory_get_adapter_identifier(D9CFactory*, uint32_t adapter,
                                                D9CAdapterIdentifier* out);
uint32_t dxmt9c_factory_get_adapter_mode_count(D9CFactory*, uint32_t adapter,
                                                uint32_t fmt);
int32_t  dxmt9c_factory_enum_adapter_modes(D9CFactory*, uint32_t adapter,
                                            uint32_t fmt, uint32_t mode,
                                            uint32_t* outW, uint32_t* outH,
                                            uint32_t* outRefresh, uint32_t* outFmt);
int32_t  dxmt9c_factory_get_adapter_display_mode(D9CFactory*, uint32_t adapter,
                                                  uint32_t* outW, uint32_t* outH,
                                                  uint32_t* outRefresh, uint32_t* outFmt);
uint64_t dxmt9c_factory_get_adapter_monitor(D9CFactory*, uint32_t adapter);
int32_t  dxmt9c_factory_check_device_type(D9CFactory*, uint32_t adapter,
                                           uint32_t devType, uint32_t adapterFmt,
                                           uint32_t backFmt, uint32_t windowed);
int32_t  dxmt9c_factory_check_device_format(D9CFactory*, uint32_t adapter,
                                             uint32_t fmt, uint32_t usage);
int32_t  dxmt9c_factory_check_device_multisample(D9CFactory*, uint32_t adapter,
                                                  uint32_t fmt, uint32_t msType,
                                                  uint32_t windowed);
int32_t  dxmt9c_factory_get_caps(D9CFactory*, uint32_t adapter, D9CCaps* out);
int32_t  dxmt9c_factory_get_adapter_luid(D9CFactory*, uint32_t adapter,
                                          uint32_t* lowPart, int32_t* highPart);

D9CDevice* dxmt9c_factory_create_device(D9CFactory*, uint32_t adapter,
                                         const D9CPresentParams*, uint32_t behaviorFlags,
                                         const D9CDisplayModeEx* fullscreenMode);

/* ── device ──────────────────────────────────────────────────────────────── */

void     dxmt9c_device_addref(D9CDevice*);
uint32_t dxmt9c_device_release(D9CDevice*);

int32_t  dxmt9c_device_get_caps(D9CDevice*, D9CCaps* out);
int32_t  dxmt9c_device_test_cooperative_level(D9CDevice*);
int32_t  dxmt9c_device_check_device_state(D9CDevice*, uint64_t destWindow);
int32_t  dxmt9c_device_reset(D9CDevice*, const D9CPresentParams*);
int32_t  dxmt9c_device_reset_ex(D9CDevice*, const D9CPresentParams*,
                                 const D9CDisplayModeEx*);
int32_t  dxmt9c_device_present(D9CDevice*, const D9CRect* src, const D9CRect* dst,
                                uint64_t destWindowOverride, const void* dirtyRegion,
                                uint32_t flags);
int32_t  dxmt9c_device_begin_scene(D9CDevice*);
int32_t  dxmt9c_device_end_scene(D9CDevice*);

int32_t  dxmt9c_device_clear(D9CDevice*, uint32_t count, const D9CRect* rects,
                              uint32_t flags, uint32_t colorARGB, float z,
                              uint32_t stencil);

int32_t  dxmt9c_device_set_viewport(D9CDevice*, const D9CViewport*);
void     dxmt9c_device_get_viewport(D9CDevice*, D9CViewport*);
int32_t  dxmt9c_device_set_scissor_rect(D9CDevice*, const D9CRect*);
void     dxmt9c_device_get_scissor_rect(D9CDevice*, D9CRect*);

int32_t  dxmt9c_device_set_transform(D9CDevice*, uint32_t state, const D9CMatrix*);
int32_t  dxmt9c_device_get_transform(D9CDevice*, uint32_t state, D9CMatrix*);
int32_t  dxmt9c_device_set_material(D9CDevice*, const D9CMaterial*);
int32_t  dxmt9c_device_get_material(D9CDevice*, D9CMaterial*);
int32_t  dxmt9c_device_set_light(D9CDevice*, uint32_t index, const D9CLight*);
int32_t  dxmt9c_device_light_enable(D9CDevice*, uint32_t index, uint32_t enable);

int32_t  dxmt9c_device_set_render_state(D9CDevice*, uint32_t state, uint32_t value);
uint32_t dxmt9c_device_get_render_state(D9CDevice*, uint32_t state);
int32_t  dxmt9c_device_set_texture_stage_state(D9CDevice*, uint32_t stage,
                                                uint32_t type, uint32_t value);
uint32_t dxmt9c_device_get_texture_stage_state(D9CDevice*, uint32_t stage,
                                                uint32_t type);
int32_t  dxmt9c_device_set_sampler_state(D9CDevice*, uint32_t sampler,
                                          uint32_t type, uint32_t value);
uint32_t dxmt9c_device_get_sampler_state(D9CDevice*, uint32_t sampler, uint32_t type);

int32_t  dxmt9c_device_set_clip_plane(D9CDevice*, uint32_t index,
                                       const float plane[4]);
int32_t  dxmt9c_device_get_clip_plane(D9CDevice*, uint32_t index, float plane[4]);

int32_t  dxmt9c_device_set_fvf(D9CDevice*, uint32_t fvf);
uint32_t dxmt9c_device_get_fvf(D9CDevice*);
int32_t  dxmt9c_device_set_vertex_declaration(D9CDevice*, D9CVertexDecl*);
int32_t  dxmt9c_device_set_stream_source(D9CDevice*, uint32_t stream,
                                          D9CBuffer*, uint32_t offset, uint32_t stride);
int32_t  dxmt9c_device_set_stream_source_freq(D9CDevice*, uint32_t stream,
                                               uint32_t freq);
int32_t  dxmt9c_device_set_indices(D9CDevice*, D9CBuffer*);
int32_t  dxmt9c_device_set_texture(D9CDevice*, uint32_t stage, D9CTexture*);

int32_t  dxmt9c_device_set_vertex_shader(D9CDevice*, D9CShader*);
int32_t  dxmt9c_device_set_pixel_shader(D9CDevice*, D9CShader*);
int32_t  dxmt9c_device_set_vs_const_f(D9CDevice*, uint32_t start,
                                       const float* data, uint32_t count);
int32_t  dxmt9c_device_get_vs_const_f(D9CDevice*, uint32_t start,
                                       float* data, uint32_t count);
int32_t  dxmt9c_device_set_ps_const_f(D9CDevice*, uint32_t start,
                                       const float* data, uint32_t count);
int32_t  dxmt9c_device_get_ps_const_f(D9CDevice*, uint32_t start,
                                       float* data, uint32_t count);
int32_t  dxmt9c_device_set_vs_const_i(D9CDevice*, uint32_t start,
                                       const int32_t* data, uint32_t count);
int32_t  dxmt9c_device_set_ps_const_i(D9CDevice*, uint32_t start,
                                       const int32_t* data, uint32_t count);
int32_t  dxmt9c_device_set_vs_const_b(D9CDevice*, uint32_t start,
                                       const uint32_t* data, uint32_t count);
int32_t  dxmt9c_device_set_ps_const_b(D9CDevice*, uint32_t start,
                                       const uint32_t* data, uint32_t count);

int32_t  dxmt9c_device_set_render_target(D9CDevice*, uint32_t index, D9CSurface*);
D9CSurface* dxmt9c_device_get_render_target(D9CDevice*, uint32_t index);
int32_t  dxmt9c_device_set_depth_stencil(D9CDevice*, D9CSurface*);
D9CSurface* dxmt9c_device_get_depth_stencil(D9CDevice*);

int32_t  dxmt9c_device_draw_primitive(D9CDevice*, uint32_t type,
                                       uint32_t startVertex, uint32_t count);
int32_t  dxmt9c_device_commit_chunk(D9CDevice*, const D9CCommandChunk*);
int32_t  dxmt9c_device_draw_primitive_packet(D9CDevice*,
                                              const D9CDrawPrimitivePacket*);
int32_t  dxmt9c_device_draw_primitive_chunk(D9CDevice*,
                                             const D9CDrawPrimitivePacket* packets,
                                             uint32_t packetCount);
int32_t  dxmt9c_device_draw_indexed_primitive(D9CDevice*, uint32_t type,
                                               int32_t baseVertex, uint32_t minVertex,
                                               uint32_t numVertices, uint32_t startIndex,
                                               uint32_t count);
int32_t  dxmt9c_device_draw_primitive_up(D9CDevice*, uint32_t type, uint32_t count,
                                          const void* data, uint32_t stride);
int32_t  dxmt9c_device_draw_indexed_primitive_up(D9CDevice*, uint32_t type,
                                                  uint32_t minVertex, uint32_t numVertices,
                                                  uint32_t count, const void* indexData,
                                                  uint32_t indexFmt,
                                                  const void* vertexData, uint32_t stride);

int32_t  dxmt9c_device_update_surface(D9CDevice*, D9CSurface* src,
                                       const D9CRect* srcRect,
                                       D9CSurface* dst, const D9CRect* dstPt);
int32_t  dxmt9c_device_update_texture(D9CDevice*, D9CTexture* src, D9CTexture* dst);
int32_t  dxmt9c_device_stretch_rect(D9CDevice*, D9CSurface* src, const D9CRect* srcRect,
                                     D9CSurface* dst, const D9CRect* dstRect, uint32_t filter);
int32_t  dxmt9c_device_color_fill(D9CDevice*, D9CSurface*, const D9CRect*, uint32_t colorARGB);
int32_t  dxmt9c_device_get_render_target_data(D9CDevice*, D9CSurface* rt, D9CSurface* dst);

int32_t     dxmt9c_device_set_maximum_frame_latency(D9CDevice*, uint32_t);
uint32_t    dxmt9c_device_get_maximum_frame_latency(D9CDevice*);
int32_t     dxmt9c_device_wait_for_vblank(D9CDevice*, uint32_t swapChainIndex);
int32_t     dxmt9c_device_check_device_multisample(D9CDevice*, uint32_t fmt,
                                                    uint32_t msType, uint32_t windowed);
D9CSwapChain* dxmt9c_device_get_swap_chain(D9CDevice*, uint32_t index);
uint32_t    dxmt9c_device_get_swap_chain_count(D9CDevice*);
D9CSwapChain* dxmt9c_device_create_additional_swap_chain(D9CDevice*,
                                                          const D9CPresentParams*);

/* ── resource creation ───────────────────────────────────────────────────── */

D9CTexture* dxmt9c_device_create_texture(D9CDevice*, uint32_t w, uint32_t h,
                                          uint32_t levels, uint32_t usage,
                                          uint32_t fmt, uint32_t pool);
D9CTexture* dxmt9c_device_create_cube_texture(D9CDevice*, uint32_t size,
                                               uint32_t levels, uint32_t usage,
                                               uint32_t fmt, uint32_t pool);
D9CTexture* dxmt9c_device_create_volume_texture(D9CDevice*, uint32_t w, uint32_t h,
                                                 uint32_t d, uint32_t levels,
                                                 uint32_t usage, uint32_t fmt,
                                                 uint32_t pool);
D9CBuffer*  dxmt9c_device_create_vertex_buffer(D9CDevice*, uint32_t length,
                                                uint32_t usage, uint32_t fvf,
                                                uint32_t pool);
D9CBuffer*  dxmt9c_device_create_index_buffer(D9CDevice*, uint32_t length,
                                               uint32_t usage, uint32_t fmt,
                                               uint32_t pool);
D9CSurface* dxmt9c_device_create_render_target(D9CDevice*, uint32_t w, uint32_t h,
                                                uint32_t fmt, uint32_t msType,
                                                uint32_t msQuality, uint32_t lockable,
                                                uint64_t* sharedHandle);
D9CSurface* dxmt9c_device_create_depth_stencil(D9CDevice*, uint32_t w, uint32_t h,
                                                uint32_t fmt, uint32_t msType,
                                                uint32_t msQuality, uint32_t discard,
                                                uint64_t* sharedHandle);
D9CSurface* dxmt9c_device_create_offscreen_surface(D9CDevice*, uint32_t w, uint32_t h,
                                                    uint32_t fmt, uint32_t pool,
                                                    uint64_t* sharedHandle);

D9CShader*  dxmt9c_device_create_vertex_shader(D9CDevice*, const uint32_t* bytecode);
D9CShader*  dxmt9c_device_create_pixel_shader(D9CDevice*, const uint32_t* bytecode);

D9CVertexDecl* dxmt9c_device_create_vertex_declaration(D9CDevice*,
                                                         const D9CVertexElement*);

D9CQuery*      dxmt9c_device_create_query(D9CDevice*, uint32_t type);
D9CStateBlock* dxmt9c_device_create_state_block(D9CDevice*, uint32_t type);
int32_t        dxmt9c_device_begin_state_block(D9CDevice*);
int32_t        dxmt9c_device_end_state_block(D9CDevice*, D9CStateBlock**);

/* ── swap chain ──────────────────────────────────────────────────────────── */

void     dxmt9c_swapchain_addref(D9CSwapChain*);
uint32_t dxmt9c_swapchain_release(D9CSwapChain*);
int32_t  dxmt9c_swapchain_present(D9CSwapChain*, const D9CRect* src,
                                   const D9CRect* dst, uint64_t destWindow,
                                   const void* dirtyRegion, uint32_t flags);
D9CSurface* dxmt9c_swapchain_get_back_buffer(D9CSwapChain*, uint32_t index,
                                              uint32_t type);
D9CSurface* dxmt9c_swapchain_get_depth_stencil(D9CSwapChain*);
int32_t  dxmt9c_swapchain_get_present_params(D9CSwapChain*, D9CPresentParams*);

/* ── texture ─────────────────────────────────────────────────────────────── */

void     dxmt9c_texture_addref(D9CTexture*);
uint32_t dxmt9c_texture_release(D9CTexture*);
int32_t  dxmt9c_texture_lock_rect(D9CTexture*, uint32_t level, D9CLockedRect* out,
                                   const D9CRect*, uint32_t flags);
int32_t  dxmt9c_texture_unlock_rect(D9CTexture*, uint32_t level);
D9CSurface* dxmt9c_texture_get_surface_level(D9CTexture*, uint32_t level);
uint32_t dxmt9c_texture_get_level_count(D9CTexture*);
int32_t  dxmt9c_texture_get_level_desc(D9CTexture*, uint32_t level, D9CSurfaceDesc*);
int32_t  dxmt9c_texture_generate_mip_sublevels(D9CTexture*);

/* ── buffer ──────────────────────────────────────────────────────────────── */

void     dxmt9c_buffer_addref(D9CBuffer*);
uint32_t dxmt9c_buffer_release(D9CBuffer*);
int32_t  dxmt9c_buffer_lock(D9CBuffer*, uint32_t offset, uint32_t size,
                             void** data, uint32_t flags);
int32_t  dxmt9c_buffer_unlock(D9CBuffer*);
int32_t  dxmt9c_buffer_get_desc(D9CBuffer*, D9CBufferDesc*);

/* ── surface ─────────────────────────────────────────────────────────────── */

void     dxmt9c_surface_addref(D9CSurface*);
uint32_t dxmt9c_surface_release(D9CSurface*);
int32_t  dxmt9c_surface_lock_rect(D9CSurface*, D9CLockedRect*, const D9CRect*, uint32_t flags);
int32_t  dxmt9c_surface_unlock_rect(D9CSurface*);
int32_t  dxmt9c_surface_get_desc(D9CSurface*, D9CSurfaceDesc*);
D9CTexture* dxmt9c_surface_get_container_texture(D9CSurface*);

/* ── shader ──────────────────────────────────────────────────────────────── */

void     dxmt9c_shader_addref(D9CShader*);
uint32_t dxmt9c_shader_release(D9CShader*);
int32_t  dxmt9c_shader_get_bytecode(D9CShader*, void* data, uint32_t* size);

/* ── vertex declaration ──────────────────────────────────────────────────── */

void     dxmt9c_vdecl_addref(D9CVertexDecl*);
uint32_t dxmt9c_vdecl_release(D9CVertexDecl*);

/* ── query ───────────────────────────────────────────────────────────────── */

void     dxmt9c_query_addref(D9CQuery*);
uint32_t dxmt9c_query_release(D9CQuery*);
int32_t  dxmt9c_query_issue(D9CQuery*, uint32_t flags);
int32_t  dxmt9c_query_get_data(D9CQuery*, void* data, uint32_t size, uint32_t flags);
uint32_t dxmt9c_query_get_data_size(D9CQuery*);
uint32_t dxmt9c_query_get_type(D9CQuery*);

/* ── state block ─────────────────────────────────────────────────────────── */

void     dxmt9c_stateblock_addref(D9CStateBlock*);
uint32_t dxmt9c_stateblock_release(D9CStateBlock*);
int32_t  dxmt9c_stateblock_capture(D9CStateBlock*);
int32_t  dxmt9c_stateblock_apply(D9CStateBlock*);

/* ── vertex declaration ──────────────────────────────────────────────────── */

int32_t  dxmt9c_vdecl_get_declaration(D9CVertexDecl*, D9CVertexElement* out,
                                       uint32_t* count);

#ifdef __cplusplus
}
#endif
