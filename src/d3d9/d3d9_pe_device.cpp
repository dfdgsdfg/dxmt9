/* src/d3d9/d3d9_pe_device.cpp — PE-side IDirect3DDevice9Ex and recorder glue.
 * All methods delegate to the dxmt9c_* C API from dxmt9/device_c.h. */

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include "d3d9_pe.hpp"
#include "d3d9_pe_device_child.hpp"
#include "d3d9_pe_recorder.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

static inline HRESULT hr32(int32_t r) { return (HRESULT)r; }

static D3DFORMAT exposeAdapterDisplayFormat(D3DFORMAT fmt) {
    if (fmt == D3DFMT_A8R8G8B8) return D3DFMT_X8R8G8B8;
    return fmt;
}

static void dxmt9DeviceDebugLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "dxmt9-device", fmt, args);
    va_end(args);
}

static void dxmt9DeviceInfoLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dxmt9::util::vlogf(dxmt9::util::LogLevel::Info, "dxmt9-device", fmt, args);
    va_end(args);
}

static bool dxmt9PeRecorderStatsEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_RECORDER_STATS");
    return enabled;
}

static bool dxmt9PeRecorderChunkLogEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_RECORDER_CHUNK_LOG");
    return enabled;
}

// Structural invariant: the chunk recorder and PE state shadow are the
// production path. Draw / Set* hot paths have no runtime env opt-out to
// per-call bridge mode for dxmt9c_device_set_render_state /
// set_texture / set_stream_source / set_fvf / set_vs / set_ps /
// set_vertex_declaration / set_render_target / set_depth_stencil_surface
// / set_viewport / set_scissor_rect / set_texture_stage_state /
// set_sampler_state / set_material / set_clip_plane / set_transform /
// set_light / light_enable / set_indices unix-calls. New Set*-style code
// should update PE shadow state and encode it into command records.
//
// Phase 16: full-snapshot mode. When set, every draw packet emitted in
// chunk-recorder mode carries the COMPLETE BaseDrawState snapshot (every
// field marked valid + populated from the PE shadow), not just the
// delta-since-last-packet. Wire size grows (typical packet jumps from
// ~100B to ~1KB) but the importer becomes idempotent — every packet is
// self-contained and can be replayed independently of prior packets.
// Off (default) keeps the delta optimization that makes run-coalescing
// detection cheap (packetHasNoStateDelta == "all valid bits zero").
static bool dxmt9PeFullSnapshotEnabled() {
    static const bool enabled = dxmt9::util::getenvFlag("DXMT9_PE_DRAW_FULL_SNAPSHOT");
    return enabled;
}

static bool isValidD3DStateBlockType(D3DSTATEBLOCKTYPE type) {
    return type == D3DSBT_ALL || type == D3DSBT_PIXELSTATE || type == D3DSBT_VERTEXSTATE;
}

static bool isUnknownFormat(D3DFORMAT fmt) {
    return fmt == D3DFMT_UNKNOWN;
}

// T4 (D3D9Ex shared-handle, SYSTEMMEM partial): for SYSTEMMEM textures
// the test_user_memory oracle (Wine d3d9ex tests) requires that
//   - 0 levels (auto-mip)            -> D3DERR_INVALIDCALL
//   - levels > 1                     -> D3DERR_INVALIDCALL
//   - SCRATCH pool                   -> D3DERR_INVALIDCALL
//   - SYSTEMMEM, levels == 1         -> S_OK; user pointer aliased
// allowSystemMemUserMemory is false for cube/volume textures since the
// partial scope only covers 2D textures and offscreen plain surfaces.
// The width/height == 1x1 narrowing for 2D textures is enforced at the
// call site (validate* doesn't see W/H). DEFAULT-pool sharing remains
// E_NOTIMPL until the IOSurface / MTLSharedTexture bridge lands.
[[nodiscard]] static HRESULT validateSharedHandleForTexture(bool extended,
                                              HANDLE* sharedHandle,
                                              D3DPOOL pool,
                                              UINT levels,
                                              bool allowSystemMemUserMemory) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    if (pool == D3DPOOL_SYSTEMMEM) {
        if (!allowSystemMemUserMemory) return D3DERR_INVALIDCALL;
        if (levels != 1) return D3DERR_INVALIDCALL;
        return S_OK;
    }
    if (pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
    return E_NOTIMPL;
}

// T4: per Wine test_user_memory (~line 793-798), VB/IB with pSharedHandle
// and SYSTEMMEM (or any non-DEFAULT pool) must return D3DERR_NOTAVAILABLE.
[[nodiscard]] static HRESULT validateSharedHandleForBuffer(bool extended,
                                             HANDLE* sharedHandle,
                                             D3DPOOL pool) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    if (pool != D3DPOOL_DEFAULT) return D3DERR_NOTAVAILABLE;
    return E_NOTIMPL;
}

// T4: per Wine test_user_memory (~line 800-830), offscreen plain surface
// with pSharedHandle:
//   - SYSTEMMEM           -> S_OK; user pointer aliased
//   - SCRATCH             -> D3DERR_INVALIDCALL
//   - DEFAULT (E_NOTIMPL) -> partial scope, see validateSharedHandleForDefaultSurface
[[nodiscard]] static HRESULT validateSharedHandleForSurface(bool extended,
                                              HANDLE* sharedHandle,
                                              D3DPOOL pool,
                                              bool allowSystemMemUserMemory) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    if (pool == D3DPOOL_SYSTEMMEM) {
        if (!allowSystemMemUserMemory) return D3DERR_INVALIDCALL;
        return S_OK;
    }
    if (pool == D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
    if (pool != D3DPOOL_DEFAULT) return D3DERR_INVALIDCALL;
    return E_NOTIMPL;
}

[[nodiscard]] static HRESULT validateSharedHandleForDefaultSurface(bool extended,
                                                     HANDLE* sharedHandle) {
    if (!sharedHandle) return S_OK;
    if (!extended) return E_NOTIMPL;
    return E_NOTIMPL;
}

static D9CRect toR(const RECT& r) {
    D9CRect c; c.left = r.left; c.top = r.top;
    c.right = r.right; c.bottom = r.bottom;
    return c;
}

// T4 (D3D9Ex shared-handle, SYSTEMMEM partial): format byte size for the
// formats the SYSTEMMEM user-memory paths actually exercise. The PE side
// is intentionally walled off from dxmt9::core helpers — keeping a tiny
// table here avoids dragging core_format into the PE TU. Returns 0 for
// unknown/unsupported formats; the caller must fall through to the
// normal create path on 0.
static uint32_t userMemoryBytesPerPixel(D3DFORMAT fmt) {
    switch (fmt) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8:
        case D3DFMT_A2R10G10B10:
        case D3DFMT_A2B10G10R10:
        case D3DFMT_G16R16:
        case D3DFMT_D32:
        case D3DFMT_D24S8:
        case D3DFMT_D24X8:
            return 4;
        case D3DFMT_R8G8B8:
            return 3;
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
        case D3DFMT_A8L8:
        case D3DFMT_L16:
        case D3DFMT_D16:
        case D3DFMT_D15S1:
            return 2;
        case D3DFMT_A8:
        case D3DFMT_L8:
        case D3DFMT_R3G3B2:
        case D3DFMT_A4L4:
        case D3DFMT_P8:
            return 1;
        default:
            return 0;
    }
}

/* =========================================================================
 * Raw-handle extractors — safe because only our device creates these objects.
 * ========================================================================= */

static D9CSurface*   rawSurf(IDirect3DSurface9* p)          { return D3D9PeRawSurface(p); }
static D9CBuffer*    rawVBuf(IDirect3DVertexBuffer9* p)     { return D3D9PeRawVertexBuffer(p); }
static D9CBuffer*    rawIBuf(IDirect3DIndexBuffer9* p)      { return D3D9PeRawIndexBuffer(p); }
static D9CShader*    rawVS(IDirect3DVertexShader9* p)       { return D3D9PeRawVertexShader(p); }
static D9CShader*    rawPS(IDirect3DPixelShader9* p)        { return D3D9PeRawPixelShader(p); }
static D9CVertexDecl* rawVD(IDirect3DVertexDeclaration9* p) { return D3D9PeRawVertexDecl(p); }
static D9CTexture*   rawTex(IDirect3DBaseTexture9* p)       { return D3D9PeRawTexture(p); }

/* =========================================================================
 * D3D9DeviceImpl — IDirect3DDevice9Ex
 * ========================================================================= */

class D3D9DeviceImpl final : public IDirect3DDevice9Ex, public D3D9PeRecorderFlush {
    static_assert(sizeof(D9CCommandChunkWireHeader) ==
                  D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE);
    static_assert(sizeof(D9CCommandChunkWireRecordHeader) ==
                  D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE);
    static_assert(sizeof(D9CCommandChunkWireHandleEntry) ==
                  D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE);

    // Phase 21: chunk-flush thresholds. Defaults match what the PE
    // recorder has been tuned around since Phase 5 (64 records = a few
    // dozen draws + their state setters; 256 KB ≈ one full vertex
    // upload for a complex draw + headers). Both are env-overridable
    // via DXMT9_PE_CHUNK_MAX_RECORDS / DXMT9_PE_CHUNK_MAX_BYTES; the
    // helpers below cap the env values to prevent pathological inputs
    // from blowing chunk-side allocations.
    static constexpr UINT kDefaultMaxPendingCommandRecords = 64;
    static constexpr size_t kDefaultMaxPendingCommandBytes = 256 * 1024;
    static constexpr UINT kAbsoluteMaxPendingCommandRecords = 4096;
    static constexpr size_t kAbsoluteMaxPendingCommandBytes = 16 * 1024 * 1024;
    static UINT maxPendingCommandRecords() {
        static const UINT cached = []() -> UINT {
            const auto envValue = dxmt9::util::getenvU32("DXMT9_PE_CHUNK_MAX_RECORDS");
            if (!envValue || *envValue == 0) return kDefaultMaxPendingCommandRecords;
            return std::min<UINT>(*envValue, kAbsoluteMaxPendingCommandRecords);
        }();
        return cached;
    }
    static size_t maxPendingCommandBytes() {
        static const size_t cached = []() -> size_t {
            const auto envValue = dxmt9::util::getenvU64("DXMT9_PE_CHUNK_MAX_BYTES");
            if (!envValue || *envValue == 0) return kDefaultMaxPendingCommandBytes;
            return std::min<size_t>(*envValue, kAbsoluteMaxPendingCommandBytes);
        }();
        return cached;
    }

    using WireHandleEntryList = D3D9PeWireHandleEntryList;

    ULONG        refs_    = 1;
    D9CDevice*   dev_;
    IDirect3D9Ex* factory_;
    UINT         adapter_ = 0;
    D3DDEVTYPE   deviceType_ = D3DDEVTYPE_HAL;
    DWORD        behaviorFlags_ = 0;
    bool         extended_ = false;
    bool         cursorSurfaceSet_ = false;
    bool         cursorVisible_ = false;
    bool         deviceNotReset_ = false;
    uint32_t     defaultPoolResourceRefs_ = 0;
    bool         stateBlockRecording_ = false;
    std::recursive_mutex recorderMutex_{};

    /* bound resource tracking (AddRef'd) */
    IDirect3DBaseTexture9*     textures_[16]    = {};
    IDirect3DVertexShader9*    vs_              = nullptr;
    IDirect3DPixelShader9*     ps_              = nullptr;
    IDirect3DVertexBuffer9*    streamSrc_[16]   = {};
    UINT                       streamOff_[16]   = {};
    UINT                       streamStr_[16]   = {};
    UINT                       streamFreq_[16]  = {};
    IDirect3DIndexBuffer9*     indexBuf_        = nullptr;
    IDirect3DVertexDeclaration9* vdecl_         = nullptr;
    DWORD                      fvf_             = 0;
    IDirect3DSurface9*         cachedBackBuffer0_ = nullptr;

    PeHotStateShadow peState_{};
    PeConstShadowBlock peConsts_{};
    IDirect3DSurface9* rtSlots_[4]{};
    IDirect3DSurface9* dsSurface_ = nullptr;
    PeCommandChunkBuilder commandChunk_{};
    PeRecorderStats peRecorderStats_{};
    std::uint64_t peRecorderStatsLastLoggedCommitCount_ = 0;

    /* present params copy for GetCreationParameters */
    HWND creationWindow_ = nullptr;

    template<typename T>
    static void setRef(T*& slot, T* newVal) {
        if (newVal) newVal->AddRef();
        if (slot)   slot->Release();
        slot = newVal;
    }

    void releaseAllBound() {
        for (auto& t : textures_)   setRef(t, (IDirect3DBaseTexture9*)nullptr);
        setRef(vs_, (IDirect3DVertexShader9*)nullptr);
        setRef(ps_, (IDirect3DPixelShader9*)nullptr);
        for (auto& s : streamSrc_)  setRef(s, (IDirect3DVertexBuffer9*)nullptr);
        setRef(indexBuf_, (IDirect3DIndexBuffer9*)nullptr);
        setRef(vdecl_, (IDirect3DVertexDeclaration9*)nullptr);
        for (auto& rt : rtSlots_)   setRef(rt, (IDirect3DSurface9*)nullptr);
        setRef(dsSurface_, (IDirect3DSurface9*)nullptr);
        setRef(cachedBackBuffer0_, (IDirect3DSurface9*)nullptr);
        // T2 device-lost: explicitly nullify the device's primary RT slot
        // and depth-stencil on the C side so no stale Metal surface handle
        // survives a Reset(). The PE shadow's pendingRtMask/pendingDs is
        // cleared via clearPendingHotState() in clearPeStateTracking, but
        // the server-side core::Device state must also lose the prior
        // attachment references — invalidateDefaultPoolResources() clears
        // the resource itself but not the bound-slot pointer.
        if (dev_) {
            dxmt9c_device_set_render_target(dev_, 0, nullptr);
            dxmt9c_device_set_depth_stencil(dev_, nullptr);
        }
    }

    void clearPendingCommandChunk() {
        commandChunk_.clear();
    }

    void clearPeStateTracking() {
        peState_.clearServerShadowTables();
        peState_.clearPendingHotState();
        peConsts_.reset();
        clearPendingCommandChunk();
        fvf_ = 0;
        std::memset(streamOff_, 0, sizeof(streamOff_));
        std::memset(streamStr_, 0, sizeof(streamStr_));
        std::memset(streamFreq_, 0, sizeof(streamFreq_));
    }

    bool hasPendingHotState() const {
        return peState_.hasPendingHotState();
    }

    void clearPendingHotState() {
        peState_.clearPendingHotState();
    }

    bool shadowedRenderStateEquals(DWORD state, DWORD value) const {
        return peState_.renderStateEquals(state, value);
    }

    bool shadowedTextureEquals(DWORD stage, IDirect3DBaseTexture9* texture) const {
        return stage < 16 && textures_[stage] == texture;
    }

    bool shadowedStreamSourceEquals(UINT stream,
                                    IDirect3DVertexBuffer9* buffer,
                                    UINT offset,
                                    UINT stride) const {
        return stream < 16 && streamSrc_[stream] == buffer &&
               streamOff_[stream] == offset && streamStr_[stream] == stride;
    }

    bool buildDrawPrimitivePacket(D3DPRIMITIVETYPE type,
                                  UINT startVertex,
                                  UINT count,
                                  D9CDrawPrimitivePacket& packet) const {
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            return false;
        }

        packet = D9CDrawPrimitivePacket{};
        peState_.pendingRenderStates.forEach([&](uint32_t state, uint32_t value) {
            auto& entry = packet.renderStates[packet.renderStateCount++];
            entry.state = state;
            entry.value = value;
        });

        packet.textureMask = peState_.pendingTextureMask;
        for (DWORD stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
            if ((peState_.pendingTextureMask & (1u << stage)) != 0) {
                packet.textures[stage] = toWireHandle(rawTex(textures_[stage]));
            }
        }

        packet.streamSourceMask = peState_.pendingStreamMask;
        for (DWORD stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
            if ((peState_.pendingStreamMask & (1u << stream)) == 0) {
                continue;
            }
            auto& source = packet.streamSources[stream];
            source.buffer = toWireHandle(rawVBuf(streamSrc_[stream]));
            source.offset = streamOff_[stream];
            source.stride = streamStr_[stream];
        }

        packet.fvfValid = peState_.pendingFvf ? 1u : 0u;
        packet.fvf = fvf_;
        // Phase 12: shader-handle delta. Server-side applyDrawPacketState
        // dispatches dxmt9c_device_set_vertex_shader / set_pixel_shader
        // when valid=1, mirroring the renderState/texture/stream pattern.
        packet.vsValid = peState_.pendingVs ? 1u : 0u;
        packet.vsHandle = toWireHandle(rawVS(vs_));
        packet.psValid = peState_.pendingPs ? 1u : 0u;
        packet.psHandle = toWireHandle(rawPS(ps_));
        packet.vdeclValid = peState_.pendingVdecl ? 1u : 0u;
        packet.vdeclHandle = toWireHandle(rawVD(vdecl_));
        // RT delta — emit handle for every set bit. Slot 0 is rt0_ if
        // ever populated; slots 1..3 are rtSlots_[i]. The legacy SetRT
        // path doesn't populate rt0_ separately, so always use rtSlots_.
        packet.rtMask = peState_.pendingRtMask;
        for (DWORD slot = 0; slot < 4; ++slot) {
            packet.rtHandles[slot] = (peState_.pendingRtMask & (1u << slot))
                                          ? toWireHandle(rawSurf(rtSlots_[slot]))
                                          : D9CWireHandle{};
        }
        packet.dsValid = peState_.pendingDs ? 1u : 0u;
        packet.dsHandle = toWireHandle(rawSurf(dsSurface_));
        packet.viewportValid = peState_.pendingViewport ? 1u : 0u;
        packet.viewport = peState_.viewportShadow;
        packet.scissorValid = peState_.pendingScissor ? 1u : 0u;
        packet.scissor = peState_.scissorShadow;
        // Phase 12: drain TSS / SamplerState pending tables into packet
        // delta arrays. The cap check inside Set* already flushes the
        // chunk if a single Set would push beyond the per-packet limit;
        // here we just emit what's pending.
        if (peState_.pendingTss.size() > D9C_DRAW_PACKET_MAX_TSS ||
            peState_.pendingSamplerStates.size() > D9C_DRAW_PACKET_MAX_SAMPLER) {
            return false;
        }
        packet.tssCount = static_cast<uint32_t>(peState_.pendingTss.size());
        uint32_t tssIdx = 0;
        peState_.pendingTss.forEach([&](uint32_t stage, uint32_t state, uint32_t value) {
            packet.tss[tssIdx].stage = stage;
            packet.tss[tssIdx].type = state;
            packet.tss[tssIdx].value = value;
            ++tssIdx;
        });
        packet.samplerStateCount = static_cast<uint32_t>(peState_.pendingSamplerStates.size());
        uint32_t ssIdx = 0;
        peState_.pendingSamplerStates.forEach([&](uint32_t sampler, uint32_t state, uint32_t value) {
            packet.samplerStates[ssIdx].sampler = sampler;
            packet.samplerStates[ssIdx].type = state;
            packet.samplerStates[ssIdx].value = value;
            ++ssIdx;
        });
        // Phase 12: material + clip-plane deltas. Material rides as a
        // single struct + valid flag; clip planes ride as a 6-bit mask
        // + flat 6×4 float array (only set bits' slots are
        // semantically meaningful, but the array is fixed-size so the
        // packet layout stays simple).
        packet.materialValid = peState_.pendingMaterial ? 1u : 0u;
        packet.material = peState_.materialShadow;
        packet.clipPlaneMask = peState_.pendingClipPlaneMask;
        std::memcpy(packet.clipPlanes, peState_.clipPlaneShadow, sizeof(packet.clipPlanes));
        // Phase 12: Transform delta — drain pending transform table
        // (per-frame typically a handful: View, Projection, a few
        // World/Texture transforms). Cap check: > MAX_TRANSFORMS forces
        // chunk seal upstream.
        if (peState_.pendingTransforms.size() > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
            return false;
        }
        packet.transformCount = static_cast<uint32_t>(peState_.pendingTransforms.size());
        uint32_t txIdx = 0;
        peState_.pendingTransforms.forEach([&](uint32_t state, const D9CMatrix& matrix) {
            packet.transforms[txIdx].state = state;
            packet.transforms[txIdx].reserved = 0;
            packet.transforms[txIdx].matrix = matrix;
            ++txIdx;
        });
        // Phase 12: Light + LightEnable deltas. Light slot mask carries
        // the per-slot full D9CLight payload (set bit ⇒ lights[slot] is
        // semantically meaningful). LightEnable delta is two parallel
        // masks: ValidMask says "this slot has a fresh enable" and
        // LightEnableMask carries the new value.
        packet.lightSlotMask = peState_.pendingLightSlotMask;
        for (uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_LIGHTS; ++slot) {
            if ((peState_.pendingLightSlotMask & (1u << slot)) != 0) {
                packet.lights[slot] = peState_.lightShadow[slot];
            }
        }
        packet.lightEnableValidMask = peState_.pendingLightEnableValidMask;
        packet.lightEnableMask = peState_.pendingLightEnableMask;
        // Phase 16: full-snapshot mode — override every delta field with
        // the complete shadow snapshot. The importer applies whatever
        // valid bits are set, so flipping every bit + populating from
        // the existing PE shadow gives a self-contained packet without
        // requiring any importer changes. We respect the per-array caps;
        // a shadow that overflows (e.g. > 64 distinct render states)
        // returns false to force the chunk to seal.
        if (dxmt9PeFullSnapshotEnabled()) {
            // Render states: drain the entire shadow table.
            if (peState_.renderStateShadow.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
                return false;
            }
            packet.renderStateCount = 0;
            peState_.renderStateShadow.forEach([&](uint32_t state, uint32_t value) {
                auto& entry = packet.renderStates[packet.renderStateCount++];
                entry.state = state;
                entry.value = value;
            });
            // Texture / RT / Stream — set mask bits for every populated slot.
            packet.textureMask = 0;
            for (DWORD stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
                if (textures_[stage] != nullptr) {
                    packet.textureMask |= 1u << stage;
                    packet.textures[stage] = toWireHandle(rawTex(textures_[stage]));
                }
            }
            packet.streamSourceMask = 0;
            for (DWORD stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
                if (streamSrc_[stream] != nullptr) {
                    packet.streamSourceMask |= 1u << stream;
                    auto& s = packet.streamSources[stream];
                    s.buffer = toWireHandle(rawVBuf(streamSrc_[stream]));
                    s.offset = streamOff_[stream];
                    s.stride = streamStr_[stream];
                }
            }
            packet.rtMask = 0;
            for (DWORD slot = 0; slot < 4; ++slot) {
                if (rtSlots_[slot] != nullptr) {
                    packet.rtMask |= 1u << slot;
                    packet.rtHandles[slot] = toWireHandle(rawSurf(rtSlots_[slot]));
                }
            }
            // Scalar valid bits: emit shadow contents unconditionally.
            packet.fvfValid = 1u;
            packet.fvf = fvf_;
            packet.vsValid = 1u;
            packet.vsHandle = toWireHandle(rawVS(vs_));
            packet.psValid = 1u;
            packet.psHandle = toWireHandle(rawPS(ps_));
            packet.vdeclValid = 1u;
            packet.vdeclHandle = toWireHandle(rawVD(vdecl_));
            packet.dsValid = 1u;
            packet.dsHandle = toWireHandle(rawSurf(dsSurface_));
            packet.viewportValid = 1u;
            packet.viewport = peState_.viewportShadow;
            packet.scissorValid = 1u;
            packet.scissor = peState_.scissorShadow;
            // TSS / SamplerState — drain shadow tables fully.
            if (peState_.tssShadow.size() > D9C_DRAW_PACKET_MAX_TSS ||
                peState_.samplerStateShadow.size() > D9C_DRAW_PACKET_MAX_SAMPLER ||
                peState_.transformShadow.size() > D9C_DRAW_PACKET_MAX_TRANSFORMS) {
                return false;
            }
            packet.tssCount = 0;
            peState_.tssShadow.forEach([&](uint32_t stage, uint32_t state, uint32_t value) {
                auto& e = packet.tss[packet.tssCount++];
                e.stage = stage;
                e.type = state;
                e.value = value;
            });
            packet.samplerStateCount = 0;
            peState_.samplerStateShadow.forEach([&](uint32_t sampler, uint32_t state, uint32_t value) {
                auto& e = packet.samplerStates[packet.samplerStateCount++];
                e.sampler = sampler;
                e.type = state;
                e.value = value;
            });
            packet.materialValid = 1u;
            packet.material = peState_.materialShadow;
            // Clip planes: emit every slot with mask = 0x3F (all 6).
            packet.clipPlaneMask = 0x3Fu;
            std::memcpy(packet.clipPlanes, peState_.clipPlaneShadow,
                        sizeof(packet.clipPlanes));
            // Transforms: drain shadow.
            packet.transformCount = 0;
            peState_.transformShadow.forEach([&](uint32_t state, const D9CMatrix& matrix) {
                auto& t = packet.transforms[packet.transformCount++];
                t.state = state;
                t.reserved = 0;
                t.matrix = matrix;
            });
            // Lights: emit every slot.
            packet.lightSlotMask = (1u << D9C_DRAW_PACKET_MAX_LIGHTS) - 1u;
            for (uint32_t i = 0; i < D9C_DRAW_PACKET_MAX_LIGHTS; ++i) {
                packet.lights[i] = peState_.lightShadow[i];
            }
            packet.lightEnableValidMask = (1u << D9C_DRAW_PACKET_MAX_LIGHTS) - 1u;
            packet.lightEnableMask = peState_.lightEnableShadow;
        }
        packet.primitiveType = static_cast<uint32_t>(type);
        packet.startVertex = startVertex;
        packet.primitiveCount = count;
        return true;
    }

    static UINT primitiveVertexCount(D3DPRIMITIVETYPE type, UINT primitiveCount) {
        switch (type) {
        case D3DPT_POINTLIST: return primitiveCount;
        case D3DPT_LINELIST: return primitiveCount * 2u;
        case D3DPT_LINESTRIP: return primitiveCount + 1u;
        case D3DPT_TRIANGLELIST: return primitiveCount * 3u;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN: return primitiveCount + 2u;
        default: return 0;
        }
    }

    static bool checkedByteCount(UINT count, UINT stride, std::uint32_t& bytes) {
        const auto value = static_cast<std::uint64_t>(count) * stride;
        if (value > 0xffffffffull) {
            return false;
        }
        bytes = static_cast<std::uint32_t>(value);
        return true;
    }

    // Draw records consume the effective server state, not only the handles
    // present in their delta packet. Capture these at append time so coarser
    // chunks can survive later Set* mutations and wrapper releases.
    bool appendCurrentlyBoundDrawHandles(WireHandleEntryList& handles,
                                         std::size_t firstHandle) {
        for (auto* tex : textures_) {
            if (auto* raw = rawTex(tex); raw != nullptr) {
                if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                        handles, firstHandle, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                        reinterpret_cast<uint64_t>(raw))) {
                    return false;
                }
            }
        }
        for (auto* vb : streamSrc_) {
            if (auto* raw = rawVBuf(vb); raw != nullptr) {
                if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                        handles, firstHandle, D9C_CHUNK_HANDLE_KIND_BUFFER,
                        reinterpret_cast<uint64_t>(raw))) {
                    return false;
                }
            }
        }
        if (auto* raw = rawIBuf(indexBuf_); raw != nullptr) {
            if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                    handles, firstHandle, D9C_CHUNK_HANDLE_KIND_BUFFER,
                    reinterpret_cast<uint64_t>(raw))) {
                return false;
            }
        }
        for (auto* surf : rtSlots_) {
            if (auto* raw = rawSurf(surf); raw != nullptr) {
                if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                        handles, firstHandle, D9C_CHUNK_HANDLE_KIND_SURFACE,
                        reinterpret_cast<uint64_t>(raw))) {
                    return false;
                }
            }
        }
        if (auto* raw = rawSurf(dsSurface_); raw != nullptr) {
            if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                    handles, firstHandle, D9C_CHUNK_HANDLE_KIND_SURFACE,
                    reinterpret_cast<uint64_t>(raw))) {
                return false;
            }
        }
        // VS/PS/Vdecl have no pool retention table on the server side
        // (importer's markChunkResources skips SHADER / VERTEX_DECL
        // kinds), so emitting them here would be inert. Leaving them
        // out keeps the wire payload tight.
        return true;
    }

    bool appendCurrentlyBoundClearHandles(WireHandleEntryList& handles,
                                          std::size_t firstHandle,
                                          uint32_t flags) {
        if ((flags & D3DCLEAR_TARGET) != 0) {
            for (auto* surf : rtSlots_) {
                if (auto* raw = rawSurf(surf); raw != nullptr) {
                    if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                            handles, firstHandle, D9C_CHUNK_HANDLE_KIND_SURFACE,
                            reinterpret_cast<uint64_t>(raw))) {
                        return false;
                    }
                }
            }
        }
        if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) != 0) {
            if (auto* raw = rawSurf(dsSurface_); raw != nullptr) {
                if (!PeCommandChunkBuilder::appendRecordWireHandleFrom(
                        handles, firstHandle, D9C_CHUNK_HANDLE_KIND_SURFACE,
                        reinterpret_cast<uint64_t>(raw))) {
                    return false;
                }
            }
        }
        return true;
    }

    bool collectAppendTimeExtraWireHandles(
        const D9CCommandChunkWireRecordHeader& wireRecord,
        const std::uint8_t* payload,
        WireHandleEntryList& handles,
        std::size_t firstHandle) {
        switch (wireRecord.type) {
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
        case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
        case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
            return appendCurrentlyBoundDrawHandles(handles, firstHandle);
        case D9C_COMMAND_RECORD_CLEAR: {
            if (!payload || wireRecord.payloadSize < sizeof(D9CCommandRecordClear)) {
                return false;
            }
            D9CCommandRecordClear decoded{};
            std::memcpy(&decoded, payload, sizeof(decoded));
            return appendCurrentlyBoundClearHandles(
                handles, firstHandle, decoded.flags);
        }
        default:
            return true;
        }
    }

    void recordPeChunkCommit(PeRecorderFlushReason reason,
                             std::uint32_t recordCount,
                             std::uint32_t payloadBytes,
                             std::uint32_t handleCount,
                             std::uint32_t wireBytes) {
        ++peRecorderStats_.commitCount;
        peRecorderStats_.recordCountTotal += recordCount;
        peRecorderStats_.recordCountMax =
            std::max<std::uint64_t>(peRecorderStats_.recordCountMax, recordCount);
        peRecorderStats_.payloadBytesTotal += payloadBytes;
        peRecorderStats_.payloadBytesMax =
            std::max<std::uint64_t>(peRecorderStats_.payloadBytesMax, payloadBytes);
        peRecorderStats_.handleCountTotal += handleCount;
        peRecorderStats_.handleCountMax =
            std::max<std::uint64_t>(peRecorderStats_.handleCountMax, handleCount);
        const auto reasonIndex = static_cast<std::size_t>(reason);
        if (reasonIndex < peRecorderStats_.flushReasons.size()) {
            ++peRecorderStats_.flushReasons[reasonIndex];
        }
        if (dxmt9PeRecorderChunkLogEnabled()) {
            dxmt9DeviceInfoLog(
                "pe_recorder_chunk device=%p reason=%s commitCount=%llu "
                "recordCount=%u payloadBytes=%u handleCount=%u wireBytes=%u",
                this, peRecorderFlushReasonName(reason),
                static_cast<unsigned long long>(peRecorderStats_.commitCount),
                recordCount, payloadBytes, handleCount, wireBytes);
        }
    }

    void logPeRecorderStats(const char* event, bool force = false) {
        if (!dxmt9PeRecorderStatsEnabled()) {
            return;
        }
        if (!force &&
            peRecorderStatsLastLoggedCommitCount_ == peRecorderStats_.commitCount) {
            return;
        }
        peRecorderStatsLastLoggedCommitCount_ = peRecorderStats_.commitCount;
        dxmt9DeviceInfoLog(
            "pe_recorder_stats event=%s device=%p commitCount=%llu "
            "recordCountTotal=%llu recordCountMax=%llu "
            "payloadBytesTotal=%llu payloadBytesMax=%llu "
            "handleCountTotal=%llu handleCountMax=%llu "
            "flushReasons{explicit=%llu capacityPre=%llu capacityPost=%llu "
            "barrier=%llu present=%llu readback=%llu reset=%llu "
            "stateblock=%llu child=%llu destructor=%llu stateMutation=%llu} "
            "up{drawPrimitiveUPCalls=%llu drawIndexedPrimitiveUPCalls=%llu "
            "vertexBytes=%llu indexBytes=%llu}",
            event ? event : "unknown", this,
            static_cast<unsigned long long>(peRecorderStats_.commitCount),
            static_cast<unsigned long long>(peRecorderStats_.recordCountTotal),
            static_cast<unsigned long long>(peRecorderStats_.recordCountMax),
            static_cast<unsigned long long>(peRecorderStats_.payloadBytesTotal),
            static_cast<unsigned long long>(peRecorderStats_.payloadBytesMax),
            static_cast<unsigned long long>(peRecorderStats_.handleCountTotal),
            static_cast<unsigned long long>(peRecorderStats_.handleCountMax),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Explicit)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::CapacityPre)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::CapacityPost)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Barrier)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Present)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Readback)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Reset)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::StateBlock)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Child)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::Destructor)]),
            static_cast<unsigned long long>(
                peRecorderStats_.flushReasons[
                    static_cast<std::size_t>(PeRecorderFlushReason::StateMutation)]),
            static_cast<unsigned long long>(peRecorderStats_.drawPrimitiveUPCalls),
            static_cast<unsigned long long>(peRecorderStats_.drawIndexedPrimitiveUPCalls),
            static_cast<unsigned long long>(peRecorderStats_.upVertexBytes),
            static_cast<unsigned long long>(peRecorderStats_.upIndexBytes));
    }

    void recordDrawPrimitiveUPCopy(std::uint32_t vertexBytes) {
        ++peRecorderStats_.drawPrimitiveUPCalls;
        peRecorderStats_.upVertexBytes += vertexBytes;
    }

    void recordDrawIndexedPrimitiveUPCopy(std::uint32_t vertexBytes,
                                          std::uint32_t indexBytes) {
        ++peRecorderStats_.drawIndexedPrimitiveUPCalls;
        peRecorderStats_.upVertexBytes += vertexBytes;
        peRecorderStats_.upIndexBytes += indexBytes;
    }

    HRESULT flushPendingCommandChunk(
        PeRecorderFlushReason reason = PeRecorderFlushReason::Explicit) {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        return commandChunk_.flush(
            reason,
            [this](PeRecorderFlushReason commitReason,
                   const D9CCommandChunk& chunk,
                   const PeCommandChunkCommitInfo& info) {
                const HRESULT hr = hr32(dxmt9c_device_commit_chunk(dev_, &chunk));
                if (SUCCEEDED(hr)) {
                    recordPeChunkCommit(commitReason, info.recordCount,
                                        info.payloadBytes, info.handleCount,
                                        info.wireBytes);
                }
                return hr;
            });
    }

    template<typename WriteFn>
    HRESULT appendCommandRecordDirect(uint32_t type, size_t bytes, WriteFn write) {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        return commandChunk_.appendRecordDirect(
            type, bytes, maxPendingCommandRecords(), maxPendingCommandBytes(),
            std::forward<WriteFn>(write),
            [this](const D9CCommandChunkWireRecordHeader& wireRecord,
                   const std::uint8_t* payload,
                   WireHandleEntryList& extraHandles,
                   std::size_t firstHandle) {
                return collectAppendTimeExtraWireHandles(
                    wireRecord, payload, extraHandles, firstHandle);
            },
            [this](PeRecorderFlushReason flushReason) {
                return flushPendingCommandChunk(flushReason);
            });
    }

    HRESULT appendCommandRecord(const void* data, size_t bytes) {
        if (!data) {
            return D3DERR_INVALIDCALL;
        }
        uint32_t type = 0;
        if (bytes >= sizeof(D9CCommandRecordHeader)) {
            D9CCommandRecordHeader legacyHeader{};
            std::memcpy(&legacyHeader, data, sizeof(legacyHeader));
            type = legacyHeader.type;
        }
        return appendCommandRecordDirect(
            type, bytes, [data, bytes](std::uint8_t* dst) {
                std::memcpy(dst, data, bytes);
            });
    }

    HRESULT appendCommandRecordRetained(const void* data,
                                        size_t bytes,
                                        D9CSurface* surface0 = nullptr,
                                        D9CSurface* surface1 = nullptr,
                                        D9CTexture* texture0 = nullptr,
                                        D9CTexture* texture1 = nullptr) {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        if (!data || bytes == 0 || bytes > 0xffffffffull) {
            return D3DERR_INVALIDCALL;
        }
        if (commandChunk_.shouldFlushBeforeAppend(
                bytes, maxPendingCommandRecords(), maxPendingCommandBytes())) {
            const HRESULT flushHr =
                flushPendingCommandChunk(PeRecorderFlushReason::CapacityPre);
            if (FAILED(flushHr)) return flushHr;
        }

        D3D9PePendingCommandRetainer::Acquired acquired{};
        auto& retainer = commandChunk_.retainer();
        retainer.retainSurface(surface0, acquired);
        retainer.retainSurface(surface1, acquired);
        retainer.retainTexture(texture0, acquired);
        retainer.retainTexture(texture1, acquired);

        const auto recordCountBefore = commandChunk_.recordCount();
        const HRESULT hr = appendCommandRecord(data, bytes);
        if (FAILED(hr) && commandChunk_.recordCount() == recordCountBefore) {
            retainer.rollback(acquired);
        }
        return hr;
    }

    HRESULT appendDrawPrimitiveRecord(D3DPRIMITIVETYPE type, UINT startVertex, UINT count) {
        // Drain any accumulated const dirty ranges into chunk records FIRST,
        // so the chunk replays "consts → draw" in API order.
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawPrimitive record{};
        record.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
        record.header.size = sizeof(record);
        if (!buildDrawPrimitivePacket(type, startVertex, count, record.packet)) {
            return D3DERR_INVALIDCALL;
        }
        return appendCommandRecord(&record, sizeof(record));
    }

    HRESULT appendDrawIndexedPrimitiveRecord(D3DPRIMITIVETYPE type,
                                             INT baseVertex,
                                             UINT minVertex,
                                             UINT numVertices,
                                             UINT startIndex,
                                             UINT count) {
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawIndexedPrimitive record{};
        record.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
        record.header.size = sizeof(record);
        if (!buildDrawPrimitivePacket(type, 0, count, record.packet.state)) {
            return D3DERR_INVALIDCALL;
        }
        record.packet.baseVertex = baseVertex;
        record.packet.minVertex = minVertex;
        record.packet.numVertices = numVertices;
        record.packet.startIndex = startIndex;
        record.packet.primitiveCount = count;
        // Phase 12: index buffer delta. Server applies before
        // dxmt9c_device_draw_indexed_primitive.
        record.packet.ibValid = peState_.pendingIb ? 1u : 0u;
        record.packet.ibHandle = toWireHandle(rawIBuf(indexBuf_));
        peState_.pendingIb = false;
        return appendCommandRecord(&record, sizeof(record));
    }

    HRESULT appendDrawPrimitiveUPRecord(D3DPRIMITIVETYPE type,
                                        UINT count,
                                        const void* data,
                                        UINT stride) {
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawPrimitiveUP header{};
        header.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP;
        if (!buildDrawPrimitivePacket(type, 0, count, header.packet.state)) {
            return D3DERR_INVALIDCALL;
        }

        std::uint32_t vertexBytes = 0;
        if (!checkedByteCount(primitiveVertexCount(type, count), stride, vertexBytes) ||
            (vertexBytes != 0 && !data)) {
            return D3DERR_INVALIDCALL;
        }
        header.packet.primitiveCount = count;
        header.packet.stride = stride;
        header.packet.vertexDataOffset = sizeof(D9CCommandRecordDrawPrimitiveUP);
        header.packet.vertexDataSize = vertexBytes;
        header.header.size = sizeof(D9CCommandRecordDrawPrimitiveUP) + vertexBytes;

        const HRESULT hr = appendCommandRecordDirect(
            header.header.type, header.header.size,
            [&header, data, vertexBytes](std::uint8_t* record) {
                std::memcpy(record, &header, sizeof(header));
                if (vertexBytes != 0) {
                    std::memcpy(record + header.packet.vertexDataOffset, data, vertexBytes);
                }
            });
        if (SUCCEEDED(hr)) {
            recordDrawPrimitiveUPCopy(vertexBytes);
        }
        return hr;
    }

    HRESULT appendDrawIndexedPrimitiveUPRecord(D3DPRIMITIVETYPE type,
                                               UINT minVertex,
                                               UINT numVertices,
                                               UINT count,
                                               const void* indexData,
                                               D3DFORMAT indexFormat,
                                               const void* vertexData,
                                               UINT stride) {
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        D9CCommandRecordDrawIndexedPrimitiveUP header{};
        header.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
        if (!buildDrawPrimitivePacket(type, 0, count, header.packet.state)) {
            return D3DERR_INVALIDCALL;
        }

        const UINT indexSize = indexFormat == D3DFMT_INDEX32 ? 4u : 2u;
        std::uint32_t indexBytes = 0;
        std::uint32_t vertexBytes = 0;
        if (minVertex > 0xffffffffu - numVertices) {
            return D3DERR_INVALIDCALL;
        }
        if (!checkedByteCount(primitiveVertexCount(type, count), indexSize, indexBytes) ||
            !checkedByteCount(minVertex + numVertices, stride, vertexBytes) ||
            (indexBytes != 0 && !indexData) ||
            (vertexBytes != 0 && !vertexData)) {
            return D3DERR_INVALIDCALL;
        }

        header.packet.minVertex = minVertex;
        header.packet.numVertices = numVertices;
        header.packet.primitiveCount = count;
        header.packet.indexFormat = static_cast<std::uint32_t>(indexFormat);
        header.packet.stride = stride;
        header.packet.indexDataOffset = sizeof(D9CCommandRecordDrawIndexedPrimitiveUP);
        header.packet.indexDataSize = indexBytes;
        header.packet.vertexDataOffset = header.packet.indexDataOffset + indexBytes;
        header.packet.vertexDataSize = vertexBytes;
        header.header.size = sizeof(D9CCommandRecordDrawIndexedPrimitiveUP) +
                             indexBytes + vertexBytes;

        const HRESULT hr = appendCommandRecordDirect(
            header.header.type, header.header.size,
            [&header, indexData, indexBytes, vertexData, vertexBytes](std::uint8_t* record) {
                std::memcpy(record, &header, sizeof(header));
                if (indexBytes != 0) {
                    std::memcpy(record + header.packet.indexDataOffset, indexData, indexBytes);
                }
                if (vertexBytes != 0) {
                    std::memcpy(record + header.packet.vertexDataOffset, vertexData, vertexBytes);
                }
            });
        if (SUCCEEDED(hr)) {
            recordDrawIndexedPrimitiveUPCopy(vertexBytes, indexBytes);
        }
        return hr;
    }

    HRESULT flushPeRecorder(
        PeRecorderFlushReason reason = PeRecorderFlushReason::Barrier) {
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;
        return flushPendingCommandChunk(reason);
    }

    // Variable-size const-array record append. The record is
    // header + (start, count) + count * elemSize bytes of payload.
    // Caller must supply the matching D9C_COMMAND_RECORD_SET_*_CONST_*
    // type tag; decoder will validate header.size against count*elemSize.
    HRESULT appendSetConstRecord(uint32_t recordType, UINT start, UINT count,
                                 const void* data, std::size_t elemSize) {
        const std::uint64_t payload64 = static_cast<std::uint64_t>(count) * elemSize;
        if (payload64 > 0xffffffffull - sizeof(D9CCommandRecordSetConst)) {
            return D3DERR_INVALIDCALL;
        }
        const std::uint32_t payloadBytes = static_cast<std::uint32_t>(payload64);
        if (payloadBytes != 0 && !data) {
            return D3DERR_INVALIDCALL;
        }

        D9CCommandRecordSetConst header{};
        header.header.type = recordType;
        header.header.size = static_cast<std::uint32_t>(sizeof(header) + payloadBytes);
        header.start = start;
        header.count = count;

        return appendCommandRecordDirect(
            header.header.type, header.header.size,
            [&header, data, payloadBytes](std::uint8_t* record) {
                std::memcpy(record, &header, sizeof(header));
                if (payloadBytes != 0) {
                    std::memcpy(record + sizeof(header), data, payloadBytes);
                }
            });
    }

    // Emit one record covering the merged dirty range, then clear it.
    HRESULT flushConstShadow(ConstShadow& shadow, uint32_t recordType, std::size_t elemSize) {
        if (!shadow.dirty()) return S_OK;
        const uint32_t start = shadow.dirtyStart;
        const uint32_t count = shadow.dirtyEnd - shadow.dirtyStart;
        const auto* data = shadow.values.data() + static_cast<std::size_t>(start) * elemSize;
        const HRESULT hr = appendSetConstRecord(recordType, start, count, data, elemSize);
        if (SUCCEEDED(hr)) {
            shadow.clear();
        }
        return hr;
    }

    // Drain all 6 const shadows. Called before each appended Draw record
    // and at chunk flush so the chunk's record stream replays
    // constants → draw in API order.
    HRESULT flushPendingConsts() {
        HRESULT hr = flushConstShadow(peConsts_.vsConstF, D9C_COMMAND_RECORD_SET_VS_CONST_F, sizeof(float) * 4);
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(peConsts_.vsConstI, D9C_COMMAND_RECORD_SET_VS_CONST_I, sizeof(int32_t) * 4);
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(peConsts_.vsConstB, D9C_COMMAND_RECORD_SET_VS_CONST_B, sizeof(uint32_t));
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(peConsts_.psConstF, D9C_COMMAND_RECORD_SET_PS_CONST_F, sizeof(float) * 4);
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(peConsts_.psConstI, D9C_COMMAND_RECORD_SET_PS_CONST_I, sizeof(int32_t) * 4);
        if (FAILED(hr)) return hr;
        hr = flushConstShadow(peConsts_.psConstB, D9C_COMMAND_RECORD_SET_PS_CONST_B, sizeof(uint32_t));
        if (FAILED(hr)) return hr;
        return S_OK;
    }

    // Phase 28: chunk-mode barrier flush. Replaces flushPendingHotState's
    // bridge-emit path with a chunk-record path that preserves the
    // "Set* never crosses PE/unix in default chunk mode" invariant.
    //
    // Drains pending consts (existing per-record stream) THEN, if hot
    // state is pending, packages the delta into a D9C_COMMAND_RECORD_
    // APPLY_STATE record + appends to the chunk + clears the pending
    // bits. Server importer dispatches APPLY_STATE via the same
    // applyDrawPacketState() that draw records use, so the server
    // shadow is updated before the upcoming barrier record runs.
    //
    // Caller still appends the actual barrier record afterwards;
    // chunk-commit flushes everything in the recorded order.
    HRESULT chunkBarrierFlush() {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        const HRESULT constHr = flushPendingConsts();
        if (FAILED(constHr)) return constHr;
        if (!hasPendingHotState()) {
            return S_OK;
        }
        D9CCommandRecordApplyState record{};
        record.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
        record.header.size = sizeof(record);
        // Fast path: single APPLY_STATE record covers all pending
        // state. After Phase 31 cap-checks at every Set* fast path,
        // this is the only path that runs in practice.
        if (buildDrawPrimitivePacket(D3DPT_POINTLIST, 0, 0, record.packet)) {
            const HRESULT appendHr = appendCommandRecord(&record, sizeof(record));
            if (FAILED(appendHr)) return appendHr;
            clearPendingHotState();
            return S_OK;
        }
        // Over-cap slow path: a Set* somewhere bypassed the cap check
        // (regression). Drain pending oversized collections in batches
        // of cap-size records. Critical safety property: every pending
        // state bit MUST be represented in the chunk before the caller
        // appends a barrier record. Sealing-and-deferring (the prior
        // behavior) lets the barrier observe stale server state.
        return drainOversizedPendingStateAsApplyStateRecords();
    }

    HRESULT drainOversizedPendingStateAsApplyStateRecords() {
        // Drain the four cappable collections (renderStates, tss,
        // samplerStates, transforms) in batches of cap-size. Each batch
        // becomes one APPLY_STATE record carrying ONLY that collection's
        // batch (other fields zero / unset). Server's applyDrawPacketState
        // is idempotent for unset fields so empty validX/maskX are safe.
        auto drainTable = [&](auto& pendingTable, auto cap, auto fillEntry,
                              auto packetCountField) -> HRESULT {
            while (!pendingTable.empty()) {
                D9CCommandRecordApplyState rec{};
                rec.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
                rec.header.size = sizeof(rec);
                std::uint32_t n = 0;
                uint32_t key = 0;
                uint32_t value = 0;
                while (n < cap && pendingTable.popFirst(key, value)) {
                    fillEntry(rec.packet, n, key, value);
                    ++n;
                }
                packetCountField(rec.packet) = n;
                const HRESULT hr = appendCommandRecord(&rec, sizeof(rec));
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        };
        auto drainTransformTable = [&](auto& pendingTable, auto cap, auto fillEntry,
                                       auto packetCountField) -> HRESULT {
            while (!pendingTable.empty()) {
                D9CCommandRecordApplyState rec{};
                rec.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
                rec.header.size = sizeof(rec);
                std::uint32_t n = 0;
                uint32_t key = 0;
                D9CMatrix value{};
                while (n < cap && pendingTable.popFirst(key, value)) {
                    fillEntry(rec.packet, n, key, value);
                    ++n;
                }
                packetCountField(rec.packet) = n;
                const HRESULT hr = appendCommandRecord(&rec, sizeof(rec));
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        };
        auto drainMatrix = [&](auto& pendingMatrix, auto cap, auto fillEntry,
                               auto packetCountField) -> HRESULT {
            while (!pendingMatrix.empty()) {
                D9CCommandRecordApplyState rec{};
                rec.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
                rec.header.size = sizeof(rec);
                std::uint32_t n = 0;
                uint32_t row = 0;
                uint32_t key = 0;
                uint32_t value = 0;
                while (n < cap && pendingMatrix.popFirst(row, key, value)) {
                    fillEntry(rec.packet, n, row, key, value);
                    ++n;
                }
                packetCountField(rec.packet) = n;
                const HRESULT hr = appendCommandRecord(&rec, sizeof(rec));
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        };
        if (auto hr = drainTable(peState_.pendingRenderStates,
                                 (uint32_t)D9C_DRAW_PACKET_MAX_RENDER_STATES,
                                 [](D9CDrawPrimitivePacket& p, std::uint32_t i,
                                    uint32_t k, uint32_t v) {
                                     p.renderStates[i].state = k;
                                     p.renderStates[i].value = v;
                                 },
                                 [](D9CDrawPrimitivePacket& p) -> std::uint32_t& {
                                     return p.renderStateCount;
                                 });
            FAILED(hr)) return hr;
        if (auto hr = drainMatrix(peState_.pendingTss,
                                  (uint32_t)D9C_DRAW_PACKET_MAX_TSS,
                                  [](D9CDrawPrimitivePacket& p, std::uint32_t i,
                                     uint32_t row, uint32_t k, uint32_t v) {
                                      p.tss[i].stage = row;
                                      p.tss[i].type = k;
                                      p.tss[i].value = v;
                                  },
                                  [](D9CDrawPrimitivePacket& p) -> std::uint32_t& {
                                      return p.tssCount;
                                  });
            FAILED(hr)) return hr;
        if (auto hr = drainMatrix(peState_.pendingSamplerStates,
                                  (uint32_t)D9C_DRAW_PACKET_MAX_SAMPLER,
                                  [](D9CDrawPrimitivePacket& p, std::uint32_t i,
                                     uint32_t row, uint32_t k, uint32_t v) {
                                      p.samplerStates[i].sampler = row;
                                      p.samplerStates[i].type = k;
                                      p.samplerStates[i].value = v;
                                  },
                                  [](D9CDrawPrimitivePacket& p) -> std::uint32_t& {
                                      return p.samplerStateCount;
                                  });
            FAILED(hr)) return hr;
        if (auto hr = drainTransformTable(peState_.pendingTransforms,
                                          (uint32_t)D9C_DRAW_PACKET_MAX_TRANSFORMS,
                                          [](D9CDrawPrimitivePacket& p, std::uint32_t i,
                                             uint32_t k, const D9CMatrix& v) {
                                              p.transforms[i].state = k;
                                              p.transforms[i].reserved = 0;
                                              p.transforms[i].matrix = v;
                                          },
                                          [](D9CDrawPrimitivePacket& p) -> std::uint32_t& {
                                              return p.transformCount;
                                          });
            FAILED(hr)) return hr;
        // Remaining scalar pending bits (texture / stream / vs / ps /
        // vdecl / RT / DS / viewport / scissor / fvf / material / clip
        // / lights / lightEnable) all fit in one packet. After draining
        // the four cappable collections above, buildDrawPrimitivePacket
        // succeeds.
        if (!hasPendingHotState()) {
            return S_OK;
        }
        D9CCommandRecordApplyState tail{};
        tail.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
        tail.header.size = sizeof(tail);
        if (!buildDrawPrimitivePacket(D3DPT_POINTLIST, 0, 0, tail.packet)) {
            // Truly should never happen — the four cappable collections
            // are now empty. Defensive: log + return failure rather than
            // silently leaving pending state dirty (which would let the
            // upcoming barrier observe stale server state).
            dxmt9DeviceDebugLog(
                "ERR: drainOversizedPendingStateAsApplyStateRecords could "
                "not build tail APPLY_STATE — pending state lost. Caller "
                "should treat as recorder failure.");
            return D3DERR_INVALIDCALL;
        }
        const HRESULT hr = appendCommandRecord(&tail, sizeof(tail));
        if (FAILED(hr)) return hr;
        clearPendingHotState();
        return S_OK;
    }

public:
    HRESULT FlushPeRecorderForChild() noexcept override {
        return flushPeRecorder(PeRecorderFlushReason::Child);
    }
    bool IsStateBlockRecordingForChild() const noexcept override {
        return stateBlockRecording_;
    }
    void InvalidateStateBlockShadowForChild() noexcept override {
        peState_.renderStateShadow.clear();
        peState_.transformShadow.clear();
        clearPendingHotState();
    }
    void AddDefaultPoolResourceRefForChild() noexcept override {
        ++defaultPoolResourceRefs_;
    }
    void ReleaseDefaultPoolResourceRefForChild() noexcept override {
        if (defaultPoolResourceRefs_ != 0) {
            --defaultPoolResourceRefs_;
        }
    }
    bool IsChunkRecorderEnabledForChild() const override {
        return true;
    }
    HRESULT AppendRecordForChild(const void* data, size_t bytes) noexcept override {
        return appendCommandRecord(data, bytes);
    }

    D3D9DeviceImpl(D9CDevice* dev, IDirect3D9Ex* factory,
                   UINT adapter, D3DDEVTYPE deviceType, DWORD behaviorFlags,
                   HWND window, bool extended)
        : dev_(dev), factory_(factory)
        , adapter_(adapter), deviceType_(deviceType), behaviorFlags_(behaviorFlags)
        , extended_(extended)
        , creationWindow_(window) {
        if (factory_) factory_->AddRef();
        for (UINT& freq : streamFreq_) {
            freq = 1;
        }
        dxmt9DeviceDebugLog("device_ctor this=%p dev=%p factory=%p adapter=%u devType=%u behavior=0x%x window=%p extended=%u",
                            this, static_cast<void*>(dev_), static_cast<void*>(factory_),
                            adapter_, (unsigned)deviceType_, (unsigned)behaviorFlags_, window, extended_ ? 1u : 0u);
    }

    ~D3D9DeviceImpl() {
        (void)flushPeRecorder(PeRecorderFlushReason::Destructor);
        logPeRecorderStats("destructor", true);
        clearPendingCommandChunk();
        releaseAllBound();
        dxmt9c_device_release(dev_);
        if (factory_) factory_->Release();
    }

    /* ── IUnknown ── */

    ULONG STDMETHODCALLTYPE AddRef()  noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_; if (!r) delete this; return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)          ||
            IsEqualGUID(riid, IID_IDirect3DDevice9)) {
            *ppv = static_cast<IDirect3DDevice9*>(this);
            dxmt9DeviceDebugLog("device_query_interface this=%p -> out=%p", this, *ppv);
            AddRef();
            return S_OK;
        }
        if (IsEqualGUID(riid, IID_IDirect3DDevice9Ex)) {
            if (!extended_) {
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            *ppv = static_cast<IDirect3DDevice9Ex*>(this);
            dxmt9DeviceDebugLog("device_query_interface_ex this=%p -> out=%p", this, *ppv);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    /* ── device info ── */

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() noexcept override {
        dxmt9DeviceDebugLog("device_test_cooperative_level device=%p", this);
        if (deviceNotReset_) {
            dxmt9DeviceDebugLog("device_test_cooperative_level -> device not reset");
            return D3DERR_DEVICENOTRESET;
        }
        const HRESULT hr = hr32(dxmt9c_device_test_cooperative_level(dev_));
        dxmt9DeviceDebugLog("device_test_cooperative_level -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() noexcept override {
        dxmt9DeviceDebugLog("device_get_available_texture_mem device=%p", this);
        const UINT value = 0x80000000u;
        dxmt9DeviceDebugLog("device_get_available_texture_mem -> %u (0x%x)",
                            value, (unsigned)value);
        return value;
    }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() noexcept override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D) noexcept override {
        if (!ppD3D) return D3DERR_INVALIDCALL;
        factory_->AddRef();
        *ppD3D = static_cast<IDirect3D9*>(factory_);
        dxmt9DeviceDebugLog("device_get_direct3d this=%p -> factory=%p", this, static_cast<void*>(*ppD3D));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) noexcept override {
        if (!pCaps) return D3DERR_INVALIDCALL;
        D9CCaps cc{};
        HRESULT hr = hr32(dxmt9c_device_get_caps(dev_, &cc));
        if (SUCCEEDED(hr)) {
            FillD3DCaps9(cc, pCaps);
            pCaps->DeviceType = deviceType_;
            dxmt9DeviceDebugLog("device_get_caps -> vs=0x%08x ps=0x%08x maxTex=%ux%u maxRT=%u maxLights=%u maxStreams=%u maxAniso=%u intervals=0x%x devCaps=0x%x rasterCaps=0x%x texCaps=0x%x textureOpCaps=0x%x",
                                (unsigned)pCaps->VertexShaderVersion,
                                (unsigned)pCaps->PixelShaderVersion,
                                (unsigned)pCaps->MaxTextureWidth,
                                (unsigned)pCaps->MaxTextureHeight,
                                (unsigned)pCaps->NumSimultaneousRTs,
                                (unsigned)pCaps->MaxActiveLights,
                                (unsigned)pCaps->MaxStreams,
                                (unsigned)pCaps->MaxAnisotropy,
                                (unsigned)pCaps->PresentationIntervals,
                                (unsigned)pCaps->DevCaps,
                                (unsigned)pCaps->RasterCaps,
                                (unsigned)pCaps->TextureCaps,
                                (unsigned)pCaps->TextureOpCaps);
        } else {
            dxmt9DeviceDebugLog("device_get_caps -> hr=0x%08x", (unsigned)hr);
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT sc, D3DDISPLAYMODE* pMode) noexcept override {
        if (!pMode) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_display_mode device=%p sc=%u", this, sc);
        D9CSwapChain* chain = dxmt9c_device_get_swap_chain(dev_, sc);
        if (!chain) {
            return D3DERR_INVALIDCALL;
        }
        D9CPresentParams cpp{};
        const HRESULT hr = hr32(dxmt9c_swapchain_get_present_params(chain, &cpp));
        dxmt9c_swapchain_release(chain);
        if (FAILED(hr)) {
            dxmt9DeviceDebugLog("device_get_display_mode -> hr=0x%08x", (unsigned)hr);
            return hr;
        }
        pMode->Width = cpp.backBufferWidth;
        pMode->Height = cpp.backBufferHeight;
        pMode->RefreshRate = cpp.fullScreenRefreshRateHz;
        pMode->Format = exposeAdapterDisplayFormat(static_cast<D3DFORMAT>(cpp.backBufferFormat));
        dxmt9DeviceDebugLog("device_get_display_mode -> %ux%u fmt=%u hz=%u",
                            pMode->Width, pMode->Height, (unsigned)pMode->Format, pMode->RefreshRate);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCreationParameters(
            D3DDEVICE_CREATION_PARAMETERS* pParams) noexcept override {
        if (!pParams) return D3DERR_INVALIDCALL;
        pParams->AdapterOrdinal  = adapter_;
        pParams->DeviceType      = deviceType_;
        pParams->hFocusWindow    = creationWindow_;
        pParams->BehaviorFlags   = behaviorFlags_;
        return S_OK;
    }

    /* ── cursor (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* surface) noexcept override {
        dxmt9DeviceDebugLog("device_set_cursor_properties device=%p x=%u y=%u surface=%p",
                            this, x, y, surface);
        if (!surface) {
            return D3DERR_INVALIDCALL;
        }
        D3DSURFACE_DESC desc{};
        const HRESULT hr = surface->GetDesc(&desc);
        if (FAILED(hr)) {
            return hr;
        }
        const auto isPowerOfTwo = [](UINT value) noexcept -> bool {
            return value != 0 && (value & (value - 1u)) == 0;
        };
        if (desc.Format != D3DFMT_A8R8G8B8 ||
            !isPowerOfTwo(desc.Width) ||
            !isPowerOfTwo(desc.Height)) {
            return D3DERR_INVALIDCALL;
        }
        cursorSurfaceSet_ = true;
        return S_OK;
    }
    void    STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD flags) noexcept override {
        dxmt9DeviceDebugLog("device_set_cursor_position device=%p x=%d y=%d flags=0x%x",
                            this, x, y, (unsigned)flags);
    }
    BOOL    STDMETHODCALLTYPE ShowCursor(BOOL show) noexcept override {
        dxmt9DeviceDebugLog("device_show_cursor device=%p show=%u", this, (unsigned)show);
        if (!cursorSurfaceSet_) {
            return FALSE;
        }
        const BOOL previous = cursorVisible_ ? TRUE : FALSE;
        cursorVisible_ = show ? true : false;
        return previous;
    }

    /* ── swap chains ── */

    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(
            D3DPRESENT_PARAMETERS* pPP, IDirect3DSwapChain9** ppSC) noexcept override {
        if (!pPP || !ppSC) return D3DERR_INVALIDCALL;
        *ppSC = nullptr;
        D9CPresentParams cpp{};
        // minimal fill
        cpp.backBufferWidth  = pPP->BackBufferWidth;
        cpp.backBufferHeight = pPP->BackBufferHeight;
        cpp.backBufferFormat = (uint32_t)pPP->BackBufferFormat;
        cpp.backBufferCount  = pPP->BackBufferCount;
        cpp.swapEffect       = (uint32_t)pPP->SwapEffect;
        cpp.deviceWindow     = (uint64_t)(uintptr_t)pPP->hDeviceWindow;
        cpp.windowed         = pPP->Windowed ? 1u : 0u;
        cpp.presentationInterval = pPP->PresentationInterval;
        D9CSwapChain* sc = dxmt9c_device_create_additional_swap_chain(dev_, &cpp);
        if (!sc) return D3DERR_INVALIDCALL;
        *ppSC = CreatePeSwapChain(sc, this, this, extended_);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT index,
                                            IDirect3DSwapChain9** ppSC) noexcept override {
        if (!ppSC) return D3DERR_INVALIDCALL;
        *ppSC = nullptr;
        D9CSwapChain* sc = dxmt9c_device_get_swap_chain(dev_, index);
        if (!sc) return D3DERR_INVALIDCALL;
        *ppSC = CreatePeSwapChain(sc, this, this, extended_);
        return S_OK;
    }

    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() noexcept override {
        return dxmt9c_device_get_swap_chain_count(dev_);
    }

    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPP) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        if (!pPP) return D3DERR_INVALIDCALL;
        D9CPresentParams cpp{};
        cpp.backBufferWidth  = pPP->BackBufferWidth;
        cpp.backBufferHeight = pPP->BackBufferHeight;
        cpp.backBufferFormat = (uint32_t)pPP->BackBufferFormat;
        cpp.backBufferCount  = pPP->BackBufferCount;
        cpp.multiSampleType  = (uint32_t)pPP->MultiSampleType;
        cpp.multiSampleQuality = pPP->MultiSampleQuality;
        cpp.swapEffect       = (uint32_t)pPP->SwapEffect;
        cpp.deviceWindow     = (uint64_t)(uintptr_t)pPP->hDeviceWindow;
        cpp.windowed         = pPP->Windowed ? 1u : 0u;
        cpp.enableAutoDepthStencil = pPP->EnableAutoDepthStencil ? 1u : 0u;
        cpp.autoDepthStencilFormat = (uint32_t)pPP->AutoDepthStencilFormat;
        cpp.flags            = pPP->Flags;
        cpp.fullScreenRefreshRateHz = pPP->FullScreen_RefreshRateInHz;
        cpp.presentationInterval = pPP->PresentationInterval;
        const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::Reset);
        if (FAILED(flushHr)) return flushHr;
        releaseAllBound();
        if (defaultPoolResourceRefs_ != 0) {
            clearPeStateTracking();
            stateBlockRecording_ = false;
            peState_.stateBlockRenderStateRestore.clear();
            peState_.stateBlockTransformRestore.clear();
            deviceNotReset_ = true;
            return D3DERR_INVALIDCALL;
        }
        clearPeStateTracking();
        stateBlockRecording_ = false;
        peState_.stateBlockRenderStateRestore.clear();
        peState_.stateBlockTransformRestore.clear();
        const HRESULT hr = hr32(dxmt9c_device_reset(dev_, &cpp));
        if (SUCCEEDED(hr)) {
            deviceNotReset_ = false;
            // T2: per Wine d3d9_device_Reset, viewport and scissor must
            // be set to {0, 0, BackBufferWidth, BackBufferHeight, 0, 1}
            // after a successful Reset. The core::Device already sets
            // its server-side viewport in resetValidated() — mirror it
            // into the PE shadow so the next draw packet carries fresh
            // viewport/scissor instead of stale pre-reset values.
            const uint32_t w = std::max<uint32_t>(1u, cpp.backBufferWidth);
            const uint32_t h = std::max<uint32_t>(1u, cpp.backBufferHeight);
            peState_.viewportShadow = D9CViewport{0, 0, w, h, 0.0f, 1.0f};
            peState_.scissorShadow  = D9CRect{0, 0, (int32_t)w, (int32_t)h};
            peState_.pendingViewport = false;
            peState_.pendingScissor  = false;
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst,
                                       HWND wnd, const RGNDATA* dirty) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        // T2 device-lost gate: render-path methods must early-return
        // D3DERR_DEVICELOST while the device awaits Reset.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_present device=%p wnd=%p src=%s dst=%s dirty=%p",
                            this, wnd,
                            src ? "<custom>" : "<full>",
                            dst ? "<custom>" : "<full>",
                            dirty);
        D9CRect cs{}, cd{};
        if (src) cs = toR(*src); if (dst) cd = toR(*dst);
        // Recorder-design Present: append a PRESENT record to the
        // current chunk after draining hot state + const dirty ranges,
        // then commit the chunk synchronously. The server-side
        // importer dispatches dxmt9c_device_present after replaying
        // every preceding draw / clear / state in the chunk — so
        // ordering is preserved and Present serves as the natural
        // chunk boundary. Dirty-region payload is dropped (the
        // backend present path doesn't consume it).
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;

        D9CCommandRecordPresent record{};
        record.header.type = D9C_COMMAND_RECORD_PRESENT;
        record.header.size = sizeof(record);
        record.hwnd = (uint64_t)(uintptr_t)wnd;
        record.flags = 0;
        record.hasSrc = src ? 1u : 0u;
        record.hasDst = dst ? 1u : 0u;
        if (src) record.src = cs;
        if (dst) record.dst = cd;
        const HRESULT appendHr = appendCommandRecord(&record, sizeof(record));
        if (FAILED(appendHr)) return appendHr;
        // Force-commit so Present runs at the bridge boundary even
        // if the chunk is below the byte/record threshold.
        const HRESULT flushHr = flushPendingCommandChunk(PeRecorderFlushReason::Present);
        if (SUCCEEDED(flushHr)) {
            logPeRecorderStats("present");
        }
        return flushHr;
    }

    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT sc, UINT idx,
                                             D3DBACKBUFFER_TYPE,
                                             IDirect3DSurface9** ppS) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        dxmt9DeviceDebugLog("device_get_back_buffer device=%p sc=%u idx=%u", this, sc, idx);
        D9CSwapChain* chain = dxmt9c_device_get_swap_chain(dev_, sc);
        if (!chain) return D3DERR_INVALIDCALL;
        D9CSurface* s = dxmt9c_swapchain_get_back_buffer(chain, idx, 0);
        if (!s) {
            dxmt9c_swapchain_release(chain);
            return D3DERR_INVALIDCALL;
        }
        if (sc == 0 && idx == 0 && cachedBackBuffer0_) {
            dxmt9c_surface_release(s);
            dxmt9c_swapchain_release(chain);
            cachedBackBuffer0_->AddRef();
            *ppS = cachedBackBuffer0_;
            return S_OK;
        }
        auto* swapchain = CreatePeSwapChain(chain, this, this, extended_);
        auto* surface = CreatePeSurface(s, this, static_cast<IDirect3DSwapChain9*>(swapchain), this, false);
        if (sc == 0 && idx == 0) {
            setRef(cachedBackBuffer0_, static_cast<IDirect3DSurface9*>(surface));
        }
        *ppS = surface;
        swapchain->Release();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT swapChain, D3DRASTER_STATUS* p) noexcept override {
        if (!p || swapChain != 0) {
            return D3DERR_INVALIDCALL;
        }
        memset(p, 0, sizeof(*p));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL enableDialogs) noexcept override {
        dxmt9DeviceDebugLog("device_set_dialog_box_mode device=%p enable=%u", this, (unsigned)enableDialogs);
        return S_OK;
    }
    void    STDMETHODCALLTYPE SetGammaRamp(UINT swapChain, DWORD flags, const D3DGAMMARAMP*) noexcept override {
        dxmt9DeviceDebugLog("device_set_gamma_ramp device=%p swapChain=%u flags=0x%x",
                            this, swapChain, (unsigned)flags);
    }
    void    STDMETHODCALLTYPE GetGammaRamp(UINT, D3DGAMMARAMP* p) noexcept override {
        if (p) memset(p, 0, sizeof(*p));
    }

    /* ── resource creation ── */

    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels,
                                             DWORD usage, D3DFORMAT fmt,
                                             D3DPOOL pool,
                                             IDirect3DTexture9** ppTex,
                                             HANDLE* psh) noexcept override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        *ppTex = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForTexture(extended_, psh, pool, levels, true);
        if (FAILED(sharedHr)) return sharedHr;
        // T4 (D3D9Ex shared-handle, SYSTEMMEM partial): SYSTEMMEM 1-mip
        // 2D texture with pSharedHandle aliases caller-supplied memory.
        // Wine d3d9ex test_user_memory line 769-778 accepts arbitrary
        // widths/heights for this path; the only constraint is single
        // mip level (validateSharedHandleForTexture already enforced).
        // bytesPerPixel == 0 means "format we cannot alias" — reject.
        const bool useUserMemory =
            extended_ && psh && pool == D3DPOOL_SYSTEMMEM && levels == 1;
        void* userPtr = nullptr;
        int32_t userPitch = 0;
        if (useUserMemory) {
            const uint32_t bpp = userMemoryBytesPerPixel(fmt);
            if (bpp == 0) return D3DERR_INVALIDCALL;
            userPtr = *psh;
            userPitch = static_cast<int32_t>(bpp * w);
        }
        dxmt9DeviceDebugLog("device_create_texture device=%p size=%ux%u levels=%u usage=0x%x fmt=%u pool=%u user=%p",
                            this, w, h, levels, (unsigned)usage, (unsigned)fmt, (unsigned)pool,
                            userPtr);
        D9CTexture* t = dxmt9c_device_create_texture(dev_, w, h, levels,
                                                      usage, (uint32_t)fmt,
                                                      (uint32_t)pool);
        if (!t) return D3DERR_INVALIDCALL;
        *ppTex = CreatePeTexture(t, this, this, userPtr, userPitch);
        dxmt9DeviceDebugLog("device_create_texture -> texture=%p", *ppTex);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d,
                                                   UINT levels, DWORD usage,
                                                   D3DFORMAT fmt, D3DPOOL pool,
                                                   IDirect3DVolumeTexture9** ppTex,
                                                   HANDLE* psh) noexcept override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        *ppTex = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForTexture(extended_, psh, pool, levels, false);
        if (FAILED(sharedHr)) return sharedHr;
        dxmt9DeviceDebugLog("device_create_volume_texture device=%p size=%ux%ux%u levels=%u usage=0x%x fmt=%u pool=%u",
                            this, w, h, d, levels, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
        D9CTexture* t = dxmt9c_device_create_volume_texture(dev_, w, h, d, levels,
                                                             usage, (uint32_t)fmt,
                                                             (uint32_t)pool);
        if (!t) return D3DERR_INVALIDCALL;
        *ppTex = CreatePeVolumeTexture(t, this, this);
        dxmt9DeviceDebugLog("device_create_volume_texture -> texture=%p", *ppTex);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT size, UINT levels,
                                                 DWORD usage, D3DFORMAT fmt,
                                                 D3DPOOL pool,
                                                 IDirect3DCubeTexture9** ppTex,
                                                 HANDLE* psh) noexcept override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        *ppTex = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForTexture(extended_, psh, pool, levels, false);
        if (FAILED(sharedHr)) return sharedHr;
        dxmt9DeviceDebugLog("device_create_cube_texture device=%p size=%u levels=%u usage=0x%x fmt=%u pool=%u",
                            this, size, levels, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
        D9CTexture* t = dxmt9c_device_create_cube_texture(dev_, size, levels,
                                                           usage, (uint32_t)fmt,
                                                           (uint32_t)pool);
        if (!t) return D3DERR_INVALIDCALL;
        *ppTex = CreatePeCubeTexture(t, this, this);
        dxmt9DeviceDebugLog("device_create_cube_texture -> texture=%p", *ppTex);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD usage,
                                                  DWORD fvf, D3DPOOL pool,
                                                  IDirect3DVertexBuffer9** ppBuf,
                                                  HANDLE* psh) noexcept override {
        if (!ppBuf) return D3DERR_INVALIDCALL;
        *ppBuf = nullptr;
        const HRESULT sharedHr = validateSharedHandleForBuffer(extended_, psh, pool);
        if (FAILED(sharedHr)) return sharedHr;
        dxmt9DeviceDebugLog("device_create_vertex_buffer device=%p len=%u usage=0x%x fvf=0x%x pool=%u",
                            this, len, (unsigned)usage, (unsigned)fvf, (unsigned)pool);
        D9CBuffer* b = dxmt9c_device_create_vertex_buffer(dev_, len, usage,
                                                           fvf, (uint32_t)pool);
        if (!b) return D3DERR_INVALIDCALL;
        *ppBuf = CreatePeVertexBuffer(b, this, this);
        dxmt9DeviceDebugLog("device_create_vertex_buffer -> buffer=%p", *ppBuf);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD usage,
                                                 D3DFORMAT fmt, D3DPOOL pool,
                                                 IDirect3DIndexBuffer9** ppBuf,
                                                 HANDLE* psh) noexcept override {
        if (!ppBuf) return D3DERR_INVALIDCALL;
        *ppBuf = nullptr;
        const HRESULT sharedHr = validateSharedHandleForBuffer(extended_, psh, pool);
        if (FAILED(sharedHr)) return sharedHr;
        dxmt9DeviceDebugLog("device_create_index_buffer device=%p len=%u usage=0x%x fmt=%u pool=%u",
                            this, len, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
        D9CBuffer* b = dxmt9c_device_create_index_buffer(dev_, len, usage,
                                                          (uint32_t)fmt,
                                                          (uint32_t)pool);
        if (!b) return D3DERR_INVALIDCALL;
        *ppBuf = CreatePeIndexBuffer(b, this, this);
        dxmt9DeviceDebugLog("device_create_index_buffer -> buffer=%p", *ppBuf);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt,
                                                  D3DMULTISAMPLE_TYPE ms,
                                                  DWORD msQual, BOOL lockable,
                                                  IDirect3DSurface9** ppS,
                                                  HANDLE* psh) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForDefaultSurface(extended_, psh);
        if (FAILED(sharedHr)) return sharedHr;
        dxmt9DeviceDebugLog("device_create_render_target device=%p size=%ux%u fmt=%u ms=%u msQual=%u lockable=%u",
                            this, w, h, (unsigned)fmt, (unsigned)ms, (unsigned)msQual, (unsigned)lockable);
        uint64_t sh = psh ? (uint64_t)(uintptr_t)*psh : 0;
        D9CSurface* s = dxmt9c_device_create_render_target(dev_, w, h,
                                                            (uint32_t)fmt,
                                                            (uint32_t)ms, msQual,
                                                            lockable ? 1u : 0u, &sh);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = CreatePeSurface(s, this, nullptr, this);
        dxmt9DeviceDebugLog("device_create_render_target -> surface=%p", *ppS);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h,
                                                         D3DFORMAT fmt,
                                                         D3DMULTISAMPLE_TYPE ms,
                                                         DWORD msQual,
                                                         BOOL discard,
                                                         IDirect3DSurface9** ppS,
                                                         HANDLE* psh) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForDefaultSurface(extended_, psh);
        if (FAILED(sharedHr)) return sharedHr;
        dxmt9DeviceDebugLog("device_create_depth_stencil_surface device=%p size=%ux%u fmt=%u ms=%u msQual=%u discard=%u",
                            this, w, h, (unsigned)fmt, (unsigned)ms, (unsigned)msQual, (unsigned)discard);
        uint64_t sh = 0;
        D9CSurface* s = dxmt9c_device_create_depth_stencil(dev_, w, h,
                                                            (uint32_t)fmt,
                                                            (uint32_t)ms, msQual,
                                                            discard ? 1u : 0u, &sh);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = CreatePeSurface(s, this, nullptr, this);
        dxmt9DeviceDebugLog("device_create_depth_stencil_surface -> surface=%p", *ppS);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* src,
                                             const RECT* srcRect,
                                             IDirect3DSurface9* dst,
                                             const POINT* dstPt) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        dxmt9DeviceDebugLog("device_update_surface device=%p src=%p dst=%p srcRect=%s dstPt=%s",
                            this, src, dst,
                            srcRect ? "<custom>" : "<full>",
                            dstPt ? "<custom>" : "<origin>");
        D9CRect cs{}, cd{};
        if (srcRect) cs = toR(*srcRect);
        if (dstPt) { cd.left = dstPt->x; cd.top = dstPt->y;
                     cd.right = dstPt->x; cd.bottom = dstPt->y; }
        // Fire-and-forget copy records stay queued until the normal chunk
        // boundary. The raw D9C wrappers are AddRef'd by the pending chunk
        // so callers may release their D3D9 wrappers immediately.
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;
        auto* const srcRaw = rawSurf(src);
        auto* const dstRaw = rawSurf(dst);
        D9CCommandRecordUpdateSurface record{};
        record.header.type = D9C_COMMAND_RECORD_UPDATE_SURFACE;
        record.header.size = sizeof(record);
        record.srcWire = reinterpret_cast<uint64_t>(srcRaw);
        record.dstWire = reinterpret_cast<uint64_t>(dstRaw);
        record.hasSrcRect = srcRect ? 1u : 0u;
        record.hasDstPoint = dstPt ? 1u : 0u;
        record.srcRect = cs;
        record.dstPoint = cd;
        return appendCommandRecordRetained(&record, sizeof(record),
                                           srcRaw, dstRaw);
    }

    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src,
                                             IDirect3DBaseTexture9* dst) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;
        auto* const srcRaw = rawTex(src);
        auto* const dstRaw = rawTex(dst);
        D9CCommandRecordUpdateTexture record{};
        record.header.type = D9C_COMMAND_RECORD_UPDATE_TEXTURE;
        record.header.size = sizeof(record);
        record.srcWire = reinterpret_cast<uint64_t>(srcRaw);
        record.dstWire = reinterpret_cast<uint64_t>(dstRaw);
        return appendCommandRecordRetained(&record, sizeof(record),
                                           nullptr, nullptr, srcRaw, dstRaw);
    }

    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* rt,
                                                   IDirect3DSurface9* dst) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        dxmt9DeviceDebugLog("device_get_render_target_data device=%p rt=%p dst=%p",
                            this, rt, dst);
        // Phase 24: chunk-recorder path. The PE caller is synchronous —
        // the call doesn't return until the data is in dst — but
        // routing through the chunk record stream keeps ordering atomic
        // with surrounding draws/clears in the SAME chunk. We append a
        // READBACK record then commit the chunk synchronously (Present
        // pattern); commit_chunk's per-record short-circuit propagates
        // the actual readback HRESULT back to PE.
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;
        auto* const rtRaw = rawSurf(rt);
        auto* const dstRaw = rawSurf(dst);
        D9CCommandRecordReadback record{};
        record.header.type = D9C_COMMAND_RECORD_READBACK;
        record.header.size = sizeof(record);
        record.srcWire = reinterpret_cast<uint64_t>(rtRaw);
        record.dstWire = reinterpret_cast<uint64_t>(dstRaw);
        const HRESULT appendHr = appendCommandRecordRetained(&record, sizeof(record),
                                                             rtRaw, dstRaw);
        if (FAILED(appendHr)) return appendHr;
        // Sync semantics: commit the chunk now and wait for completion.
        // flushPendingCommandChunk routes through commit_chunk -> server's
        // record dispatcher -> readback record handler.
        return flushPendingCommandChunk(PeRecorderFlushReason::Readback);
    }

    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT sc, IDirect3DSurface9* surface) noexcept override {
        dxmt9DeviceDebugLog("device_get_front_buffer_data device=%p sc=%u surface=%p",
                            this, sc, surface);
        return D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* src,
                                           const RECT* srcRect,
                                           IDirect3DSurface9* dst,
                                           const RECT* dstRect,
                                           D3DTEXTUREFILTERTYPE filter) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        dxmt9DeviceDebugLog("device_stretch_rect device=%p src=%p dst=%p filter=%u srcRect=%s dstRect=%s",
                            this, src, dst, (unsigned)filter,
                            srcRect ? "<custom>" : "<full>",
                            dstRect ? "<custom>" : "<full>");
        D9CRect cs{}, cd{};
        if (srcRect) cs = toR(*srcRect); if (dstRect) cd = toR(*dstRect);
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;
        auto* const srcRaw = rawSurf(src);
        auto* const dstRaw = rawSurf(dst);
        D9CCommandRecordStretchRect record{};
        record.header.type = D9C_COMMAND_RECORD_STRETCH_RECT;
        record.header.size = sizeof(record);
        record.srcWire = reinterpret_cast<uint64_t>(srcRaw);
        record.dstWire = reinterpret_cast<uint64_t>(dstRaw);
        record.hasSrcRect = srcRect ? 1u : 0u;
        record.hasDstRect = dstRect ? 1u : 0u;
        record.filter = (uint32_t)filter;
        if (srcRect) record.srcRect = cs;
        if (dstRect) record.dstRect = cd;
        return appendCommandRecordRetained(&record, sizeof(record),
                                           srcRaw, dstRaw);
    }

    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurf,
                                         const RECT* pRect,
                                         D3DCOLOR color) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        dxmt9DeviceDebugLog("device_color_fill device=%p surf=%p rect=%s color=0x%08x",
                            this, pSurf, pRect ? "<custom>" : "<full>", (unsigned)color);
        D9CRect cr{}; if (pRect) cr = toR(*pRect);
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;
        auto* const surfRaw = rawSurf(pSurf);
        D9CCommandRecordColorFill record{};
        record.header.type = D9C_COMMAND_RECORD_COLOR_FILL;
        record.header.size = sizeof(record);
        record.surfaceWire = reinterpret_cast<uint64_t>(surfRaw);
        record.colorARGB = (uint32_t)color;
        record.hasRect = pRect ? 1u : 0u;
        if (pRect) record.rect = cr;
        return appendCommandRecordRetained(&record, sizeof(record), surfRaw);
    }

    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT h,
                                                           D3DFORMAT fmt,
                                                           D3DPOOL pool,
                                                           IDirect3DSurface9** ppS,
                                                           HANDLE* psh) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
        const HRESULT sharedHr = validateSharedHandleForSurface(extended_, psh, pool, true);
        if (FAILED(sharedHr)) return sharedHr;
        // T4 (D3D9Ex shared-handle, SYSTEMMEM partial): SYSTEMMEM
        // offscreen surfaces accept arbitrary W/H per Wine's
        // test_user_memory (~line 800). The user pointer becomes the
        // entire surface storage; pitch == bpp * width.
        const bool useUserMemory =
            extended_ && psh && pool == D3DPOOL_SYSTEMMEM;
        void* userPtr = nullptr;
        int32_t userPitch = 0;
        if (useUserMemory) {
            const uint32_t bpp = userMemoryBytesPerPixel(fmt);
            if (bpp == 0) return D3DERR_INVALIDCALL;
            userPtr = *psh;
            userPitch = static_cast<int32_t>(bpp * w);
        }
        dxmt9DeviceDebugLog("device_create_offscreen_surface device=%p size=%ux%u fmt=%u pool=%u user=%p",
                            this, w, h, (unsigned)fmt, (unsigned)pool, userPtr);
        uint64_t sh = psh ? (uint64_t)(uintptr_t)*psh : 0;
        D9CSurface* s = dxmt9c_device_create_offscreen_surface(dev_, w, h,
                                                                (uint32_t)fmt,
                                                                (uint32_t)pool, &sh);
        if (!s) return D3DERR_INVALIDCALL;
        *ppS = CreatePeSurface(s, this, nullptr, this, true, userPtr, userPitch);
        dxmt9DeviceDebugLog("device_create_offscreen_surface -> surface=%p", *ppS);
        return S_OK;
    }

    /* ── render targets ── */

    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD idx,
                                               IDirect3DSurface9* pSurf) noexcept override {
        dxmt9DeviceDebugLog("device_set_render_target device=%p idx=%u surf=%p",
                            this, (unsigned)idx, pSurf);
        if (idx >= 4) return D3DERR_INVALIDCALL;
        if (rtSlots_[idx] == pSurf) return S_OK;
        if (idx == 0) {
            setRef(cachedBackBuffer0_, (IDirect3DSurface9*)nullptr);
        }
        setRef(rtSlots_[idx], pSurf);
        peState_.pendingRtMask |= 1u << idx;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD idx,
                                               IDirect3DSurface9** ppS) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u",
                            this, (unsigned)idx);
        if (idx == 0 && cachedBackBuffer0_) {
            cachedBackBuffer0_->AddRef();
            *ppS = cachedBackBuffer0_;
            dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> cached backbuffer=%p",
                                this, (unsigned)idx, static_cast<void*>(*ppS));
            return S_OK;
        }
        D9CSurface* s = dxmt9c_device_get_render_target(dev_, idx);
        *ppS = s ? CreatePeSurface(s, this, nullptr, this, false) : nullptr;
        dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> surface=%p",
                            this, (unsigned)idx, ppS ? static_cast<void*>(*ppS) : nullptr);
        return s ? S_OK : D3DERR_NOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pSurf) noexcept override {
        dxmt9DeviceDebugLog("device_set_depth_stencil device=%p surf=%p", this, pSurf);
        if (dsSurface_ == pSurf) return S_OK;
        setRef(dsSurface_, pSurf);
        peState_.pendingDs = true;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppS) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        D9CSurface* s = dxmt9c_device_get_depth_stencil(dev_);
        *ppS = s ? CreatePeSurface(s, this, nullptr, this) : nullptr;
        dxmt9DeviceDebugLog("device_get_depth_stencil_surface device=%p -> surface=%p",
                            this, ppS ? static_cast<void*>(*ppS) : nullptr);
        return s ? S_OK : S_FALSE;
    }

    /* ── scene ── */
    HRESULT STDMETHODCALLTYPE BeginScene() noexcept override {
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_begin_scene device=%p", this);
        const HRESULT hr = hr32(dxmt9c_device_begin_scene(dev_));
        dxmt9DeviceDebugLog("device_begin_scene -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EndScene()   noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_end_scene device=%p", this);
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        const HRESULT hr = hr32(dxmt9c_device_end_scene(dev_));
        dxmt9DeviceDebugLog("device_end_scene -> hr=0x%08x", (unsigned)hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* pRects,
                                     DWORD flags, D3DCOLOR color,
                                     float z, DWORD stencil) noexcept override {
        std::lock_guard<std::recursive_mutex> recorderLock(recorderMutex_);
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_clear device=%p count=%u flags=0x%x color=0x%08x z=%f stencil=%u",
                            this, (unsigned)count, (unsigned)flags, (unsigned)color, z,
                            (unsigned)stencil);
        // Per recorder design: Clear is a standalone ordering record
        // inside the chunk — drains pending hot state + const dirty
        // ranges first so the chunk replays in API order, then
        // appends a CLEAR record carrying flags + color + z + stencil
        // + the optional rect array as a tail payload.
        const HRESULT barrierHr = chunkBarrierFlush();
        if (FAILED(barrierHr)) return barrierHr;

        const std::uint32_t rectBytes = static_cast<std::uint32_t>(count) * sizeof(D9CRect);
        D9CCommandRecordClear header{};
        header.header.type = D9C_COMMAND_RECORD_CLEAR;
        header.header.size = static_cast<std::uint32_t>(sizeof(header) + rectBytes);
        header.flags = (uint32_t)flags;
        header.colorARGB = (uint32_t)color;
        header.z = z;
        header.stencil = (uint32_t)stencil;
        header.rectCount = (uint32_t)count;
        header.rectOffset = sizeof(header);

        return appendCommandRecordDirect(
            header.header.type, header.header.size,
            [&header, pRects, rectBytes](std::uint8_t* record) {
                std::memcpy(record, &header, sizeof(header));
                if (rectBytes != 0 && pRects) {
                    std::memcpy(record + header.rectOffset, pRects, rectBytes);
                }
            });
    }

    /* ── transforms ── */
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state,
                                            const D3DMATRIX* pM) noexcept override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog(
            "device_set_transform device=%p state=%u "
            "m=[[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g],[%g,%g,%g,%g]]",
            this, (unsigned)state,
            pM->m[0][0], pM->m[0][1], pM->m[0][2], pM->m[0][3],
            pM->m[1][0], pM->m[1][1], pM->m[1][2], pM->m[1][3],
            pM->m[2][0], pM->m[2][1], pM->m[2][2], pM->m[2][3],
            pM->m[3][0], pM->m[3][1], pM->m[3][2], pM->m[3][3]);
        // Phase 12: PE-shadow-only when chunk recorder is active. Pending
        // transforms ride on the next draw packet's transforms[] array;
        // server-side applyDrawPacketState dispatches set_transform per
        // entry before the draw runs.
        const D9CMatrix& wireM = *reinterpret_cast<const D9CMatrix*>(pM);
        const uint32_t stateKey = static_cast<uint32_t>(state);
        if (stateBlockRecording_) {
            if (!peState_.stateBlockTransformRestore.contains(stateKey)) {
                D9CMatrix previous = identityTransformMatrix();
                (void)peState_.transformShadow.get(stateKey, previous);
                peState_.stateBlockTransformRestore.set(stateKey, previous);
            }
            return hr32(dxmt9c_device_set_transform(dev_, stateKey, &wireM));
        }
        uint32_t transformSlotIndex = 0;
        if (!FixedTransformTable::slotForState(stateKey, transformSlotIndex)) {
            const HRESULT flushHr = flushPeRecorder();
            if (FAILED(flushHr)) return flushHr;
            return hr32(dxmt9c_device_set_transform(dev_, stateKey, &wireM));
        }
        D9CMatrix shadowMatrix{};
        const bool shadowMatches = peState_.transformShadow.get(stateKey, shadowMatrix) &&
                                   matrixEquals(shadowMatrix, wireM);
        const bool alreadyPending = peState_.pendingTransforms.contains(stateKey);
        if (!alreadyPending && shadowMatches) {
            return S_OK;
        }
        // Phase 34: cap-check uses chunkBarrierFlush, not bare
        // flushPendingCommandChunk. The latter only seals existing
        // records — pending hot state would remain DIRTY across
        // the seal, leaving the next Draw* / barrier observing
        // stale server state. chunkBarrierFlush encodes pending
        // state as APPLY_STATE record(s) + clears the pending
        // maps, so the new entry below starts with a fresh delta
        // budget AND the server has already received the prior
        // delta when the next chunk-record runs.
        if (!alreadyPending &&
            peState_.pendingTransforms.size() >= D9C_DRAW_PACKET_MAX_TRANSFORMS) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        peState_.pendingTransforms.set(stateKey, wireM);
        peState_.transformShadow.set(stateKey, wireM);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state,
                                            D3DMATRIX* pM) noexcept override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_transform device=%p state=%u", this, (unsigned)state);
        const uint32_t stateKey = static_cast<uint32_t>(state);
        D9CMatrix wireM{};
        if (peState_.transformShadow.get(stateKey, wireM)) {
            std::memcpy(pM, &wireM, sizeof(wireM));
            return S_OK;
        }
        return hr32(dxmt9c_device_get_transform(dev_, stateKey,
                    reinterpret_cast<D9CMatrix*>(pM)));
    }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state,
                                                 const D3DMATRIX* pM) noexcept override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_multiply_transform device=%p state=%u", this, (unsigned)state);
        D3DMATRIX cur{};
        GetTransform(state, &cur);
        /* multiply 4x4 */
        D3DMATRIX result{};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                float s = 0;
                for (int k = 0; k < 4; ++k)
                    s += cur.m[r][k] * pM->m[k][c];
                result.m[r][c] = s;
            }
        return SetTransform(state, &result);
    }

    /* ── viewport / scissor ── */
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* pVP) noexcept override {
        if (!pVP) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_viewport device=%p x=%u y=%u w=%u h=%u minZ=%f maxZ=%f",
                            this, pVP->X, pVP->Y, pVP->Width, pVP->Height, pVP->MinZ, pVP->MaxZ);
        D9CViewport vp{ pVP->X, pVP->Y, pVP->Width, pVP->Height,
                        pVP->MinZ, pVP->MaxZ };
        // Phase 12: PE-shadow-only when chunk recorder is active. The
        // packet built for the next draw carries viewportValid=1 + the
        // shadow snapshot; server-side applyDrawPacketState dispatches
        // dxmt9c_device_set_viewport before the draw runs.
        if (std::memcmp(&peState_.viewportShadow, &vp, sizeof(vp)) == 0) {
            return S_OK;
        }
        peState_.viewportShadow = vp;
        peState_.pendingViewport = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pVP) noexcept override {
        if (!pVP) return D3DERR_INVALIDCALL;
        D9CViewport vp{};
        dxmt9c_device_get_viewport(dev_, &vp);
        pVP->X = vp.x; pVP->Y = vp.y;
        pVP->Width = vp.width; pVP->Height = vp.height;
        pVP->MinZ = vp.minZ;   pVP->MaxZ   = vp.maxZ;
        dxmt9DeviceDebugLog("device_get_viewport device=%p -> x=%u y=%u w=%u h=%u minZ=%f maxZ=%f",
                            this, pVP->X, pVP->Y, pVP->Width, pVP->Height, pVP->MinZ, pVP->MaxZ);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* pR) noexcept override {
        if (!pR) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_scissor_rect device=%p rect=%ld,%ld-%ld,%ld",
                            this, (long)pR->left, (long)pR->top, (long)pR->right, (long)pR->bottom);
        D9CRect cr = toR(*pR);
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (std::memcmp(&peState_.scissorShadow, &cr, sizeof(cr)) == 0) {
            return S_OK;
        }
        peState_.scissorShadow = cr;
        peState_.pendingScissor = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pR) noexcept override {
        if (!pR) return D3DERR_INVALIDCALL;
        D9CRect cr{};
        dxmt9c_device_get_scissor_rect(dev_, &cr);
        pR->left = cr.left; pR->top = cr.top;
        pR->right = cr.right; pR->bottom = cr.bottom;
        return S_OK;
    }

    /* ── material / lights ── */
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* pM) noexcept override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_material device=%p", this);
        if (std::memcmp(&peState_.materialShadow, pM, sizeof(D9CMaterial)) == 0) {
            return S_OK;
        }
        std::memcpy(&peState_.materialShadow, pM, sizeof(D9CMaterial));
        peState_.pendingMaterial = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pM) noexcept override {
        if (!pM) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_get_material device=%p", this);
        return hr32(dxmt9c_device_get_material(dev_,
                    reinterpret_cast<D9CMaterial*>(pM)));
    }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD idx, const D3DLIGHT9* pL) noexcept override {
        if (!pL) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_set_light device=%p idx=%u type=%u", this, (unsigned)idx, (unsigned)pL->Type);
        D9CLight cl{};
        cl.type = (uint32_t)pL->Type;
        memcpy(&cl.diffuse,  &pL->Diffuse,  sizeof(D9CColorRGBA));
        memcpy(&cl.specular, &pL->Specular, sizeof(D9CColorRGBA));
        memcpy(&cl.ambient,  &pL->Ambient,  sizeof(D9CColorRGBA));
        cl.position[0] = pL->Position.x;
        cl.position[1] = pL->Position.y;
        cl.position[2] = pL->Position.z;
        cl.direction[0] = pL->Direction.x;
        cl.direction[1] = pL->Direction.y;
        cl.direction[2] = pL->Direction.z;
        cl.range  = pL->Range;  cl.falloff = pL->Falloff;
        cl.attenuation0 = pL->Attenuation0;
        cl.attenuation1 = pL->Attenuation1;
        cl.attenuation2 = pL->Attenuation2;
        cl.theta = pL->Theta; cl.phi = pL->Phi;
        // Phase 12: PE-shadow-only when chunk recorder is active. Up to
        // D9C_DRAW_PACKET_MAX_LIGHTS (8) light slots ride on a single
        // packet via lightSlotMask + lights[8]. Out-of-range idx falls
        // back to legacy unix-call (rare, and the backend may also
        // refuse).
        if (idx < D9C_DRAW_PACKET_MAX_LIGHTS) {
            if ((peState_.pendingLightSlotMask & (1u << idx)) == 0 &&
                std::memcmp(&peState_.lightShadow[idx], &cl, sizeof(D9CLight)) == 0) {
                return S_OK;
            }
            peState_.lightShadow[idx] = cl;
            peState_.pendingLightSlotMask |= 1u << idx;
            return S_OK;
        }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_set_light(dev_, idx, &cl));
    }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD, D3DLIGHT9* pL) noexcept override {
        dxmt9DeviceDebugLog("device_get_light device=%p", this);
        if (pL) memset(pL, 0, sizeof(*pL)); return S_OK; /* stub */
    }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD idx, BOOL en) noexcept override {
        dxmt9DeviceDebugLog("device_light_enable device=%p idx=%u enable=%u", this, (unsigned)idx, (unsigned)en);
        // Phase 12: PE-shadow-only when chunk recorder is active.
        if (idx < D9C_DRAW_PACKET_MAX_LIGHTS) {
            const DWORD bit = 1u << idx;
            const bool wantEnabled = en != 0;
            const bool shadowEnabled = (peState_.lightEnableShadow & bit) != 0;
            if ((peState_.pendingLightEnableValidMask & bit) == 0 &&
                wantEnabled == shadowEnabled) {
                return S_OK;
            }
            peState_.pendingLightEnableValidMask |= bit;
            if (wantEnabled) {
                peState_.pendingLightEnableMask |= bit;
                peState_.lightEnableShadow |= bit;
            } else {
                peState_.pendingLightEnableMask &= ~bit;
                peState_.lightEnableShadow &= ~bit;
            }
            return S_OK;
        }
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        return hr32(dxmt9c_device_light_enable(dev_, idx, en ? 1u : 0u));
    }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD, BOOL* pEn) noexcept override {
        dxmt9DeviceDebugLog("device_get_light_enable device=%p", this);
        if (pEn) *pEn = FALSE; return S_OK; /* stub */
    }

    /* ── clip planes ── */
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD idx, const float* pPlane) noexcept override {
        dxmt9DeviceDebugLog("device_set_clip_plane device=%p idx=%u plane=%p", this, (unsigned)idx, pPlane);
        if (!pPlane) return D3DERR_INVALIDCALL;
        if (idx >= 6) return D3DERR_INVALIDCALL;
        const std::size_t off = static_cast<std::size_t>(idx) * 4u;
        if ((peState_.pendingClipPlaneMask & (1u << idx)) == 0 &&
            std::memcmp(&peState_.clipPlaneShadow[off], pPlane, sizeof(float) * 4) == 0) {
            return S_OK;
        }
        std::memcpy(&peState_.clipPlaneShadow[off], pPlane, sizeof(float) * 4);
        peState_.pendingClipPlaneMask |= 1u << idx;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD idx, float* pPlane) noexcept override {
        dxmt9DeviceDebugLog("device_get_clip_plane device=%p idx=%u", this, (unsigned)idx);
        return hr32(dxmt9c_device_get_clip_plane(dev_, idx, pPlane));
    }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9*) noexcept override {
        dxmt9DeviceDebugLog("device_set_clip_status device=%p", this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* p) noexcept override {
        dxmt9DeviceDebugLog("device_get_clip_status device=%p", this);
        if (p) memset(p, 0, sizeof(*p)); return S_OK;
    }

    /* ── render states ── */
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD value) noexcept override {
        dxmt9DeviceDebugLog("device_set_render_state device=%p state=%u value=0x%x",
                            this, (unsigned)state, (unsigned)value);
        if (stateBlockRecording_) {
            const DWORD stateKey = static_cast<DWORD>(state);
            if (!peState_.stateBlockRenderStateRestore.contains(stateKey)) {
                DWORD previous = dxmt9c_device_get_render_state(dev_, stateKey);
                uint32_t shadowValue = 0;
                if (peState_.renderStateShadow.get(stateKey, shadowValue)) {
                    previous = shadowValue;
                }
                peState_.stateBlockRenderStateRestore.set(stateKey, previous);
            }
            return hr32(dxmt9c_device_set_render_state(dev_, (uint32_t)state, value));
        }
        const DWORD stateKey = static_cast<DWORD>(state);
        if (shadowedRenderStateEquals(stateKey, value)) {
            return S_OK;
        }
        // Phase 31: cap check — if a NEW state would push the pending
        // table past the per-packet cap, drain pending state into the chunk
        // via chunkBarrierFlush() so the next packet starts fresh.
        if (!peState_.pendingRenderStates.contains(stateKey) &&
            peState_.pendingRenderStates.size() >= D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        peState_.renderStateShadow.set(stateKey, value);
        peState_.pendingRenderStates.set(stateKey, value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state,
                                              DWORD* pValue) noexcept override {
        if (!pValue) return D3DERR_INVALIDCALL;
        uint32_t shadowValue = 0;
        if (peState_.renderStateShadow.get(static_cast<DWORD>(state), shadowValue)) {
            *pValue = shadowValue;
            return S_OK;
        }
        *pValue = dxmt9c_device_get_render_state(dev_, (uint32_t)state);
        return S_OK;
    }

    /* ── state blocks ── */
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE type,
                                                IDirect3DStateBlock9** ppSB) noexcept override {
        if (!ppSB) return D3DERR_INVALIDCALL;
        if (!isValidD3DStateBlockType(type) || stateBlockRecording_) {
            return D3DERR_INVALIDCALL;
        }
        // State-block creation needs current server state.
        // flushPeRecorder() routes pending PE state through chunk records.
        const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::StateBlock);
        if (FAILED(flushHr)) return flushHr;
        dxmt9DeviceDebugLog("device_create_state_block device=%p type=%u", this, (unsigned)type);
        D9CStateBlock* sb = dxmt9c_device_create_state_block(dev_, (uint32_t)type);
        if (!sb) return D3DERR_INVALIDCALL;
        *ppSB = CreatePeStateBlock(sb, this, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() noexcept override {
        if (stateBlockRecording_) {
            return D3DERR_INVALIDCALL;
        }
        const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::StateBlock);
        if (FAILED(flushHr)) return flushHr;
        dxmt9DeviceDebugLog("device_begin_state_block device=%p", this);
        const HRESULT hr = hr32(dxmt9c_device_begin_state_block(dev_));
        if (SUCCEEDED(hr)) {
            stateBlockRecording_ = true;
            peState_.stateBlockRenderStateRestore.clear();
            peState_.stateBlockTransformRestore.clear();
        }
        dxmt9DeviceDebugLog("device_begin_state_block -> hr=0x%08x", (unsigned)hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) noexcept override {
        if (!ppSB) return D3DERR_INVALIDCALL;
        if (!stateBlockRecording_) {
            return D3DERR_INVALIDCALL;
        }
        const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::StateBlock);
        if (FAILED(flushHr)) return flushHr;
        dxmt9DeviceDebugLog("device_end_state_block device=%p", this);
        D9CStateBlock* sb = nullptr;
        HRESULT hr = hr32(dxmt9c_device_end_state_block(dev_, &sb));
        if (SUCCEEDED(hr)) {
            stateBlockRecording_ = false;
            peState_.stateBlockRenderStateRestore.forEach([&](uint32_t state, uint32_t value) {
                (void)dxmt9c_device_set_render_state(dev_, state, value);
                peState_.renderStateShadow.set(state, value);
                peState_.pendingRenderStates.erase(state);
            });
            peState_.stateBlockRenderStateRestore.clear();
            peState_.stateBlockTransformRestore.forEach([&](uint32_t state, const D9CMatrix& value) {
                (void)dxmt9c_device_set_transform(dev_, state, &value);
                peState_.transformShadow.set(state, value);
                peState_.pendingTransforms.erase(state);
            });
            peState_.stateBlockTransformRestore.clear();
            if (sb) {
                *ppSB = CreatePeStateBlock(sb, this, this);
            }
        }
        dxmt9DeviceDebugLog("device_end_state_block -> hr=0x%08x sb=%p out=%p",
                            (unsigned)hr, static_cast<void*>(sb), *ppSB);
        return hr;
    }

    /* ── texture stage / sampler states ── */
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD value) noexcept override {
        dxmt9DeviceDebugLog("device_set_texture_stage_state device=%p stage=%u type=%u value=0x%x",
                            this, (unsigned)stage, (unsigned)type, (unsigned)value);
        const uint32_t stageSlot = textureStageSlot(stage);
        const uint32_t stateSlot = textureStageStateSlot(type);
        uint32_t shadowValue = 0;
        if (peState_.tssShadow.get(stageSlot, stateSlot, shadowValue) &&
            shadowValue == value) {
            return S_OK;
        }
        // Phase 34: cap-check uses chunkBarrierFlush so pending state is
        // encoded as APPLY_STATE record(s) + cleared before the new entry.
        if (!peState_.pendingTss.contains(stageSlot, stateSlot) &&
            peState_.pendingTss.size() >= D9C_DRAW_PACKET_MAX_TSS) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        peState_.tssShadow.set(stageSlot, stateSlot, value);
        peState_.pendingTss.set(stageSlot, stateSlot, value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage,
                                                    D3DTEXTURESTAGESTATETYPE type,
                                                    DWORD* pValue) noexcept override {
        if (!pValue) return D3DERR_INVALIDCALL;
        const uint32_t stageSlot = textureStageSlot(stage);
        const uint32_t stateSlot = textureStageStateSlot(type);
        uint32_t shadowValue = 0;
        if (peState_.tssShadow.get(stageSlot, stateSlot, shadowValue)) {
            *pValue = shadowValue;
            return S_OK;
        }
        *pValue = dxmt9c_device_get_texture_stage_state(dev_, stageSlot, stateSlot);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD value) noexcept override {
        dxmt9DeviceDebugLog("device_set_sampler_state device=%p sampler=%u type=%u value=0x%x",
                            this, (unsigned)sampler, (unsigned)type, (unsigned)value);
        uint32_t samplerIndex = 0;
        if (!samplerSlot(sampler, samplerIndex)) {
            return D3DERR_INVALIDCALL;
        }
        uint32_t stateSlot = 0;
        if (!samplerStateSlot(type, stateSlot)) {
            return S_OK;
        }
        uint32_t shadowValue = 0;
        if (peState_.samplerStateShadow.get(samplerIndex, stateSlot, shadowValue) &&
            shadowValue == value) {
            return S_OK;
        }
        // Phase 34: cap-check uses chunkBarrierFlush.
        if (!peState_.pendingSamplerStates.contains(samplerIndex, stateSlot) &&
            peState_.pendingSamplerStates.size() >= D9C_DRAW_PACKET_MAX_SAMPLER) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        peState_.samplerStateShadow.set(samplerIndex, stateSlot, value);
        peState_.pendingSamplerStates.set(samplerIndex, stateSlot, value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD sampler,
                                               D3DSAMPLERSTATETYPE type,
                                               DWORD* pValue) noexcept override {
        if (!pValue) return D3DERR_INVALIDCALL;
        *pValue = dxmt9c_device_get_sampler_state(dev_, sampler, (uint32_t)type);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pPasses) noexcept override {
        dxmt9DeviceDebugLog("device_validate_device device=%p", this);
        if (pPasses) *pPasses = 1; return S_OK;
    }

    /* ── palette (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette, const PALETTEENTRY*) noexcept override {
        dxmt9DeviceDebugLog("device_set_palette_entries device=%p palette=%u", this, palette);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT palette, PALETTEENTRY*) noexcept override {
        dxmt9DeviceDebugLog("device_get_palette_entries device=%p palette=%u", this, palette);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT palette) noexcept override {
        dxmt9DeviceDebugLog("device_set_current_texture_palette device=%p palette=%u", this, palette);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* p) noexcept override {
        dxmt9DeviceDebugLog("device_get_current_texture_palette device=%p", this);
        if (p) *p = 0; return S_OK;
    }

    /* ── soft VP / NPatches (stubs) ── */
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL enable) noexcept override {
        dxmt9DeviceDebugLog("device_set_software_vertex_processing device=%p enable=%u", this, (unsigned)enable);
        return S_OK;
    }
    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() noexcept override {
        dxmt9DeviceDebugLog("device_get_software_vertex_processing device=%p", this);
        return FALSE;
    }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float segments) noexcept override {
        dxmt9DeviceDebugLog("device_set_npatch_mode device=%p segments=%f", this, segments);
        return S_OK;
    }
    float   STDMETHODCALLTYPE GetNPatchMode() noexcept override { return 0.0f; }

    /* ── textures ── */
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage,
                                          IDirect3DBaseTexture9* pTex) noexcept override {
        dxmt9DeviceDebugLog("device_set_texture device=%p stage=%u tex=%p",
                            this, (unsigned)stage, pTex);
        if (stage >= 16) return D3DERR_INVALIDCALL;
        if (shadowedTextureEquals(stage, pTex)) {
            return S_OK;
        }
        setRef(textures_[stage], pTex);
        peState_.pendingTextureMask |= 1u << stage;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage,
                                          IDirect3DBaseTexture9** ppTex) noexcept override {
        if (!ppTex) return D3DERR_INVALIDCALL;
        IDirect3DBaseTexture9* t = textures_[stage < 16 ? stage : 0];
        if (t) t->AddRef();
        *ppTex = t;
        dxmt9DeviceDebugLog("device_get_texture device=%p stage=%u -> tex=%p",
                            this, (unsigned)stage, static_cast<void*>(t));
        return S_OK;
    }

    /* ── FVF / vertex declaration ── */
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) noexcept override {
        dxmt9DeviceDebugLog("device_set_fvf device=%p fvf=0x%x", this, (unsigned)fvf);
        if (fvf_ == fvf) {
            return S_OK;
        }
        fvf_ = fvf;
        peState_.pendingFvf = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) noexcept override {
        if (!pFVF) return D3DERR_INVALIDCALL;
        *pFVF = fvf_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(
            const D3DVERTEXELEMENT9* pElems,
            IDirect3DVertexDeclaration9** ppVD) noexcept override {
        if (!pElems || !ppVD) return D3DERR_INVALIDCALL;
        /* count elements until D3DDECL_END() */
        int n = 0;
        while (pElems[n].Stream != 0xFF) ++n;
        ++n; /* include D3DDECL_END */
        D9CVertexElement tmp[64]{};
        if (n > 64) return D3DERR_INVALIDCALL;
        for (int i = 0; i < n; ++i) {
            tmp[i].stream = pElems[i].Stream; tmp[i].offset = pElems[i].Offset;
            tmp[i].type   = pElems[i].Type;   tmp[i].method = pElems[i].Method;
            tmp[i].usage  = pElems[i].Usage;  tmp[i].usageIndex = pElems[i].UsageIndex;
        }
        D9CVertexDecl* d = dxmt9c_device_create_vertex_declaration(dev_, tmp);
        if (!d) return D3DERR_INVALIDCALL;
        *ppVD = CreatePeVertexDecl(d, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(
            IDirect3DVertexDeclaration9* pVD) noexcept override {
        dxmt9DeviceDebugLog("device_set_vertex_declaration device=%p decl=%p", this, pVD);
        if (vdecl_ == pVD) return S_OK;
        setRef(vdecl_, pVD);
        peState_.pendingVdecl = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(
            IDirect3DVertexDeclaration9** ppVD) noexcept override {
        if (!ppVD) return D3DERR_INVALIDCALL;
        if (vdecl_) vdecl_->AddRef();
        *ppVD = vdecl_; return S_OK;
    }

    /* ── vertex shaders ── */
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* pFn,
                                                  IDirect3DVertexShader9** ppVS) noexcept override {
        if (!pFn || !ppVS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_vertex_shader device=%p code=%p", this, pFn);
        D9CShader* s = dxmt9c_device_create_vertex_shader(dev_, reinterpret_cast<const uint32_t*>(pFn));
        if (!s) return D3DERR_INVALIDCALL;
        *ppVS = CreatePeVertexShader(s, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pVS) noexcept override {
        dxmt9DeviceDebugLog("device_set_vertex_shader device=%p shader=%p", this, pVS);
        // Phase 12: PE-shadow-only when chunk recorder is active. The
        // packet built for the next draw carries vsValid=1 + the vs_
        // wire handle; server-side applyDrawPacketState dispatches the
        // dxmt9c_device_set_vertex_shader call before the draw runs.
        if (vs_ == pVS) return S_OK;
        setRef(vs_, pVS);
        peState_.pendingVs = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppVS) noexcept override {
        if (!ppVS) return D3DERR_INVALIDCALL;
        if (vs_) vs_->AddRef(); *ppVS = vs_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT start, const float* pData,
                                                        UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_f device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        // Shadow-only: defer the record until the next flushPendingConsts()
        // (called before each draw record + at chunk commit).
        touchConstShadow(peConsts_.vsConstF, start, count, pData, sizeof(float) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT start, float* pData,
                                                        UINT count) noexcept override {
        return hr32(dxmt9c_device_get_vs_const_f(dev_, start, pData, count));
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT start, const INT* pData,
                                                        UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        touchConstShadow(peConsts_.vsConstI, start, count, pData, sizeof(int32_t) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT start, INT* pData,
                                                        UINT count) noexcept override {
        (void)start; (void)pData; (void)count; return S_OK; /* stub */
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT start, const BOOL* pData,
                                                        UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        touchConstShadow(peConsts_.vsConstB, start, count, pData, sizeof(uint32_t));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT start, BOOL* pData,
                                                        UINT count) noexcept override {
        (void)start; (void)pData; (void)count; return S_OK; /* stub */
    }

    /* ── stream sources ── */
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream,
                                               IDirect3DVertexBuffer9* pBuf,
                                               UINT offset, UINT stride) noexcept override {
        dxmt9DeviceDebugLog("device_set_stream_source device=%p stream=%u buf=%p offset=%u stride=%u",
                            this, stream, pBuf, offset, stride);
        if (stream >= 16) return D3DERR_INVALIDCALL;
        if (shadowedStreamSourceEquals(stream, pBuf, offset, stride)) {
            return S_OK;
        }
        setRef(streamSrc_[stream], pBuf);
        streamOff_[stream] = offset;
        streamStr_[stream] = stride;
        peState_.pendingStreamMask |= 1u << stream;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream,
                                               IDirect3DVertexBuffer9** ppBuf,
                                               UINT* pOffset, UINT* pStride) noexcept override {
        if (!ppBuf) return D3DERR_INVALIDCALL;
        IDirect3DVertexBuffer9* b = streamSrc_[stream < 16 ? stream : 0];
        if (b) b->AddRef();
        *ppBuf = b;
        if (pOffset) *pOffset = streamOff_[stream < 16 ? stream : 0];
        if (pStride) *pStride = streamStr_[stream < 16 ? stream : 0];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT stream, UINT freq) noexcept override {
        dxmt9DeviceDebugLog("device_set_stream_source_freq device=%p stream=%u freq=0x%x",
                            this, stream, (unsigned)freq);
        const HRESULT flushHr = flushPeRecorder();
        if (FAILED(flushHr)) return flushHr;
        streamFreq_[stream < 16 ? stream : 0] = freq;
        return hr32(dxmt9c_device_set_stream_source_freq(dev_, stream, freq));
    }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT stream, UINT* pFreq) noexcept override {
        const UINT freq = streamFreq_[stream < 16 ? stream : 0];
        if (pFreq) *pFreq = freq;
        dxmt9DeviceDebugLog("device_get_stream_source_freq device=%p stream=%u -> freq=0x%x",
                            this, stream, (unsigned)freq);
        return S_OK;
    }

    /* ── indices ── */
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIBuf) noexcept override {
        dxmt9DeviceDebugLog("device_set_indices device=%p ib=%p", this, pIBuf);
        if (indexBuf_ == pIBuf) return S_OK;
        setRef(indexBuf_, pIBuf);
        peState_.pendingIb = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIBuf) noexcept override {
        if (!ppIBuf) return D3DERR_INVALIDCALL;
        if (indexBuf_) indexBuf_->AddRef(); *ppIBuf = indexBuf_; return S_OK;
    }

    /* ── pixel shaders ── */
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* pFn,
                                                 IDirect3DPixelShader9** ppPS) noexcept override {
        if (!pFn || !ppPS) return D3DERR_INVALIDCALL;
        dxmt9DeviceDebugLog("device_create_pixel_shader device=%p code=%p", this, pFn);
        D9CShader* s = dxmt9c_device_create_pixel_shader(dev_, reinterpret_cast<const uint32_t*>(pFn));
        if (!s) return D3DERR_INVALIDCALL;
        *ppPS = CreatePePixelShader(s, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pPS) noexcept override {
        dxmt9DeviceDebugLog("device_set_pixel_shader device=%p shader=%p", this, pPS);
        if (ps_ == pPS) return S_OK;
        setRef(ps_, pPS);
        peState_.pendingPs = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppPS) noexcept override {
        if (!ppPS) return D3DERR_INVALIDCALL;
        if (ps_) ps_->AddRef(); *ppPS = ps_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT start, const float* pData,
                                                       UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_f device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        touchConstShadow(peConsts_.psConstF, start, count, pData, sizeof(float) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT start, float* pData,
                                                       UINT count) noexcept override {
        return hr32(dxmt9c_device_get_ps_const_f(dev_, start, pData, count));
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT start, const INT* pData,
                                                       UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        touchConstShadow(peConsts_.psConstI, start, count, pData, sizeof(int32_t) * 4);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT start, INT* pData,
                                                       UINT count) noexcept override {
        (void)start; (void)pData; (void)count; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT start, const BOOL* pData,
                                                       UINT count) noexcept override {
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        touchConstShadow(peConsts_.psConstB, start, count, pData, sizeof(uint32_t));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT start, BOOL* pData,
                                                       UINT count) noexcept override {
        (void)start; (void)pData; (void)count; return S_OK;
    }

    /* ── draw calls ── */
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type,
                                             UINT startVertex,
                                             UINT count) noexcept override {
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_draw_primitive device=%p type=%u startVertex=%u count=%u",
                            this, (unsigned)type, startVertex, count);
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        const HRESULT hr = appendDrawPrimitiveRecord(type, startVertex, count);
        if (SUCCEEDED(hr)) {
            clearPendingHotState();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type,
                                                    INT baseVertex,
                                                    UINT minVertex, UINT numVertices,
                                                    UINT startIndex,
                                                    UINT count) noexcept override {
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_draw_indexed_primitive device=%p type=%u base=%d min=%u num=%u startIndex=%u count=%u",
                            this, (unsigned)type, baseVertex, minVertex, numVertices,
                            startIndex, count);
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        const HRESULT hr = appendDrawIndexedPrimitiveRecord(type, baseVertex, minVertex,
                                                            numVertices, startIndex, count);
        if (SUCCEEDED(hr)) {
            clearPendingHotState();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                               UINT count,
                                               const void* pData,
                                               UINT stride) noexcept override {
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_draw_primitive_up device=%p type=%u count=%u data=%p stride=%u",
                            this, (unsigned)type, count, pData, stride);
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        const HRESULT hr = appendDrawPrimitiveUPRecord(type, count, pData, stride);
        if (SUCCEEDED(hr)) {
            clearPendingHotState();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type,
                                                      UINT minVertex,
                                                      UINT numVertices,
                                                      UINT count,
                                                      const void* pIdxData,
                                                      D3DFORMAT idxFmt,
                                                      const void* pVtxData,
                                                      UINT stride) noexcept override {
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_draw_indexed_primitive_up device=%p type=%u min=%u num=%u count=%u idx=%p idxFmt=%u vtx=%p stride=%u",
                            this, (unsigned)type, minVertex, numVertices, count,
                            pIdxData, (unsigned)idxFmt, pVtxData, stride);
        if (peState_.pendingRenderStates.size() > D9C_DRAW_PACKET_MAX_RENDER_STATES) {
            const HRESULT barrierHr = chunkBarrierFlush();
            if (FAILED(barrierHr)) return barrierHr;
        }
        const HRESULT hr = appendDrawIndexedPrimitiveUPRecord(type, minVertex, numVertices,
                                                              count, pIdxData, idxFmt,
                                                              pVtxData, stride);
        if (SUCCEEDED(hr)) {
            clearPendingHotState();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT, UINT, UINT,
                                               IDirect3DVertexBuffer9*,
                                               IDirect3DVertexDeclaration9*,
                                               DWORD) noexcept override {
        // T2 device-lost gate. ProcessVertices isn't implemented yet, but
        // when the device is lost it must return D3DERR_DEVICELOST before
        // the unimplemented INVALIDCALL fallback.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        dxmt9DeviceDebugLog("device_process_vertices device=%p", this);
        return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) noexcept override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) noexcept override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT) noexcept override { return S_OK; }

    /* ── query ── */
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE type,
                                           IDirect3DQuery9** ppQ) noexcept override {
        D9CQuery* q = dxmt9c_device_create_query(dev_, (uint32_t)type);
        if (!q) return D3DERR_NOTAVAILABLE;
        if (!ppQ) {
            dxmt9c_query_release(q);
            return S_OK;
        }
        *ppQ = CreatePeQuery(q, this, this);
        return S_OK;
    }

    /* ── IDirect3DDevice9Ex ── */

    HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT,UINT,float*,float*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9*,IDirect3DSurface9*,
                                            IDirect3DVertexBuffer9*,UINT,
                                            IDirect3DVertexBuffer9*,
                                            D3DCOMPOSERECTSOP,int,int) noexcept override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE PresentEx(const RECT* src, const RECT* dst,
                                         HWND wnd, const RGNDATA* dirty,
                                         DWORD flags) noexcept override {
        // T2 device-lost gate.
        if (deviceNotReset_) return D3DERR_DEVICELOST;
        D9CRect cs{}, cd{};
        if (src) cs = toR(*src); if (dst) cd = toR(*dst);
        const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::Present);
        if (FAILED(flushHr)) return flushHr;
        const HRESULT hr = hr32(dxmt9c_device_present(dev_,
            src ? &cs : nullptr, dst ? &cd : nullptr,
            (uint64_t)(uintptr_t)wnd, dirty, flags));
        if (SUCCEEDED(hr)) {
            logPeRecorderStats("present_ex");
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* p) noexcept override { if (p) *p = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) noexcept override { return S_OK; }

    HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT sc) noexcept override {
        return hr32(dxmt9c_device_wait_for_vblank(dev_, sc));
    }

    HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9**,
                                                      UINT32) noexcept override { return S_OK; }

    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT maxLatency) noexcept override {
        return hr32(dxmt9c_device_set_maximum_frame_latency(dev_, maxLatency));
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* p) noexcept override {
        if (!p) return D3DERR_INVALIDCALL;
        *p = dxmt9c_device_get_maximum_frame_latency(dev_); return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND wnd) noexcept override {
        return hr32(dxmt9c_device_check_device_state(dev_,
                    (uint64_t)(uintptr_t)wnd));
    }

    HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(UINT w, UINT h,
                                                    D3DFORMAT fmt,
                                                    D3DMULTISAMPLE_TYPE ms,
                                                    DWORD msQual, BOOL lockable,
                                                    IDirect3DSurface9** ppS,
                                                    HANDLE* psh,
                                                    DWORD usage) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
        return CreateRenderTarget(w, h, fmt, ms, msQual, lockable, ppS, psh);
    }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(UINT w, UINT h,
                                                             D3DFORMAT fmt,
                                                             D3DPOOL pool,
                                                             IDirect3DSurface9** ppS,
                                                             HANDLE* psh,
                                                             DWORD usage) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
        return CreateOffscreenPlainSurface(w, h, fmt, pool, ppS, psh);
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(UINT w, UINT h,
                                                           D3DFORMAT fmt,
                                                           D3DMULTISAMPLE_TYPE ms,
                                                           DWORD msQual,
                                                           BOOL discard,
                                                           IDirect3DSurface9** ppS,
                                                           HANDLE* psh,
                                                           DWORD usage) noexcept override {
        if (!ppS) return D3DERR_INVALIDCALL;
        *ppS = nullptr;
        if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
        return CreateDepthStencilSurface(w, h, fmt, ms, msQual, discard, ppS, psh);
    }

    HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS* pPP,
                                       D3DDISPLAYMODEEX* pFsMode) noexcept override {
        if (!pPP) return D3DERR_INVALIDCALL;
        if (pFsMode && pFsMode->Size != sizeof(D3DDISPLAYMODEEX)) return D3DERR_INVALIDCALL;
        if (pPP->Windowed ? pFsMode != nullptr : pFsMode == nullptr) return D3DERR_INVALIDCALL;
        if (pFsMode && (pFsMode->Width != pPP->BackBufferWidth
                || pFsMode->Height != pPP->BackBufferHeight)) {
            return D3DERR_INVALIDCALL;
        }
        D9CPresentParams cpp{};
        cpp.backBufferWidth  = pPP->BackBufferWidth;
        cpp.backBufferHeight = pPP->BackBufferHeight;
        cpp.backBufferFormat = (uint32_t)pPP->BackBufferFormat;
        cpp.backBufferCount  = pPP->BackBufferCount;
        cpp.multiSampleType  = (uint32_t)pPP->MultiSampleType;
        cpp.multiSampleQuality = pPP->MultiSampleQuality;
        cpp.swapEffect       = (uint32_t)pPP->SwapEffect;
        cpp.deviceWindow     = (uint64_t)(uintptr_t)pPP->hDeviceWindow;
        cpp.windowed         = pPP->Windowed ? 1u : 0u;
        cpp.enableAutoDepthStencil = pPP->EnableAutoDepthStencil ? 1u : 0u;
        cpp.autoDepthStencilFormat = (uint32_t)pPP->AutoDepthStencilFormat;
        cpp.flags            = pPP->Flags;
        cpp.fullScreenRefreshRateHz = pPP->FullScreen_RefreshRateInHz;
        cpp.presentationInterval = pPP->PresentationInterval;
        D9CDisplayModeEx cdme{};
        if (pFsMode) {
            cdme.width  = pFsMode->Width; cdme.height = pFsMode->Height;
            cdme.refreshRate = pFsMode->RefreshRate;
            cdme.format = (uint32_t)pFsMode->Format;
            cdme.scanLineOrdering = (uint32_t)pFsMode->ScanLineOrdering;
        }
        const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::Reset);
        if (FAILED(flushHr)) return flushHr;
        releaseAllBound();
        clearPeStateTracking();
        stateBlockRecording_ = false;
        peState_.stateBlockRenderStateRestore.clear();
        peState_.stateBlockTransformRestore.clear();
        const HRESULT hr = hr32(dxmt9c_device_reset_ex(dev_, &cpp,
            pFsMode ? &cdme : nullptr));
        if (SUCCEEDED(hr)) {
            deviceNotReset_ = false;
            // T2: same viewport/scissor reset semantics as Reset().
            const uint32_t w = std::max<uint32_t>(1u, cpp.backBufferWidth);
            const uint32_t h = std::max<uint32_t>(1u, cpp.backBufferHeight);
            peState_.viewportShadow = D9CViewport{0, 0, w, h, 0.0f, 1.0f};
            peState_.scissorShadow  = D9CRect{0, 0, (int32_t)w, (int32_t)h};
            peState_.pendingViewport = false;
            peState_.pendingScissor  = false;
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT sc,
                                                D3DDISPLAYMODEEX* pMode,
                                                D3DDISPLAYROTATION* pRot) noexcept override {
        if (!pMode) return D3DERR_INVALIDCALL;
        if (pMode->Size != sizeof(D3DDISPLAYMODEEX)) return D3DERR_INVALIDCALL;
        D3DDISPLAYMODE mode{};
        const HRESULT hr = GetDisplayMode(sc, &mode);
        if (FAILED(hr)) return hr;
        pMode->Size = sizeof(D3DDISPLAYMODEEX);
        pMode->Width = mode.Width;
        pMode->Height = mode.Height;
        pMode->RefreshRate = mode.RefreshRate;
        pMode->Format = mode.Format;
        pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
        if (pRot)  *pRot = D3DDISPLAYROTATION_IDENTITY;
        return S_OK;
    }
};

/* =========================================================================
 * Factory function (called from factory.cpp)
 * ========================================================================= */

IDirect3DDevice9Ex* CreateDeviceImpl(D9CDevice* dev, IDirect3D9Ex* pFactory,
                                     UINT adapter, D3DDEVTYPE deviceType,
                                     DWORD behaviorFlags,
                                     HWND window, bool extended) {
    return new D3D9DeviceImpl(dev, pFactory, adapter, deviceType,
                              behaviorFlags, window, extended);
}
