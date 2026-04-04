/* dxmt9/device_c.h — C ABI bridge between the PE dxmt9.dll bridge and
 * the unix-side dxmt9.so module. All types use stdint / plain C so this
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
