/* src/d3d9/d3d9_pe_factory.cpp — PE-side IDirect3D9 / IDirect3D9Ex COM wrapper.
 * All methods delegate to the dxmt9c_factory_* C API. */

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include "d3d9_pe.hpp"

// Pull in only the inline capability-pair helpers from core_format_utils.hpp.
// The header's lower section depends on dxmt9/core.hpp which redefines the
// D3DERR_* HRESULT constants as `constexpr HRESULT` — that clashes with the
// preprocessor macro forms installed by `<d3d9.h>` already pulled in via
// d3d9_pe.hpp above. The DXMT9_FORMAT_UTILS_INLINE_HELPERS_ONLY guard skips
// that legacy section.
#define DXMT9_FORMAT_UTILS_INLINE_HELPERS_ONLY 1
#include "core_format_utils.hpp"
#include "util/log/log.hpp"

/* ── helpers ─────────────────────────────────────────────────────────────── */

static inline HRESULT hr32(int32_t r) { return (HRESULT)r; }

static constexpr uint32_t kD9CDeviceTypeHal = 0u;

static bool isKnownDeviceType(D3DDEVTYPE type) {
    return type == D3DDEVTYPE_HAL ||
           type == D3DDEVTYPE_REF ||
           type == D3DDEVTYPE_SW ||
           type == D3DDEVTYPE_NULLREF;
}

static bool isSupportedDeviceType(D3DDEVTYPE type) {
    return type == D3DDEVTYPE_HAL;
}

static bool hasValidVertexProcessingFlags(DWORD behaviorFlags) {
    const DWORD vertexProcessing =
        behaviorFlags & (D3DCREATE_HARDWARE_VERTEXPROCESSING |
                         D3DCREATE_MIXED_VERTEXPROCESSING |
                         D3DCREATE_SOFTWARE_VERTEXPROCESSING);
    return vertexProcessing == D3DCREATE_HARDWARE_VERTEXPROCESSING ||
           vertexProcessing == D3DCREATE_MIXED_VERTEXPROCESSING ||
           vertexProcessing == D3DCREATE_SOFTWARE_VERTEXPROCESSING;
}

static bool isSupportedFullscreenDisplayFormat(D3DFORMAT fmt) {
    return fmt == D3DFMT_X8R8G8B8 || fmt == D3DFMT_R5G6B5;
}

static bool isSupportedAdapterModeFormat(D3DFORMAT fmt) {
    return fmt == D3DFMT_X8R8G8B8 || fmt == D3DFMT_R5G6B5;
}

static bool isValidCheckDeviceAdapterFormat(D3DFORMAT fmt) {
    return fmt == D3DFMT_X8R8G8B8 ||
           fmt == D3DFMT_R5G6B5 ||
           fmt == D3DFMT_X1R5G5B5;
}

static bool isKnownMultiSampleType(D3DMULTISAMPLE_TYPE type) {
    return type >= D3DMULTISAMPLE_NONE && type <= D3DMULTISAMPLE_16_SAMPLES;
}

static bool isValidPresentationIntervalRaw(UINT interval) {
    return interval == D3DPRESENT_INTERVAL_DEFAULT ||
           interval == D3DPRESENT_INTERVAL_ONE ||
           interval == D3DPRESENT_INTERVAL_TWO ||
           interval == D3DPRESENT_INTERVAL_THREE ||
           interval == D3DPRESENT_INTERVAL_FOUR ||
           interval == D3DPRESENT_INTERVAL_IMMEDIATE;
}

[[nodiscard]] static HRESULT validatePresentParametersD3D(const D3DPRESENT_PARAMETERS& pp,
                                            bool extended) {
    switch (pp.SwapEffect) {
    case D3DSWAPEFFECT_DISCARD:
    case D3DSWAPEFFECT_FLIP:
    case D3DSWAPEFFECT_COPY:
        break;
    case D3DSWAPEFFECT_FLIPEX:
        if (extended) break;
        return D3DERR_INVALIDCALL;
    default:
        return D3DERR_INVALIDCALL;
    }

    const UINT maxBackBufferCount = extended ? 30u : 3u;
    if (pp.BackBufferCount > maxBackBufferCount) {
        return D3DERR_INVALIDCALL;
    }
    if (pp.SwapEffect == D3DSWAPEFFECT_COPY && pp.BackBufferCount > 1u) {
        return D3DERR_INVALIDCALL;
    }
    if (!isValidPresentationIntervalRaw(pp.PresentationInterval)) {
        return D3DERR_INVALIDCALL;
    }
    return D3D_OK;
}

static D3DDEVTYPE fromCDeviceType(uint32_t type) {
    switch (type) {
    case 0: return D3DDEVTYPE_HAL;
    case 1: return D3DDEVTYPE_REF;
    case 2: return D3DDEVTYPE_NULLREF;
    default: return (D3DDEVTYPE)type;
    }
}

static void dxmt9FactoryDebugLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    dxmt9::util::vlogf(dxmt9::util::LogLevel::Debug, "dxmt9-factory", fmt, args);
    va_end(args);
}

/* D9CPresentParams ← D3DPRESENT_PARAMETERS */
static D9CPresentParams toCpp(const D3DPRESENT_PARAMETERS& pp) {
    D9CPresentParams c{};
    c.backBufferWidth         = pp.BackBufferWidth;
    c.backBufferHeight        = pp.BackBufferHeight;
    c.backBufferFormat        = (uint32_t)pp.BackBufferFormat;
    c.backBufferCount         = pp.BackBufferCount;
    c.multiSampleType         = (uint32_t)pp.MultiSampleType;
    c.multiSampleQuality      = pp.MultiSampleQuality;
    c.swapEffect              = (uint32_t)pp.SwapEffect;
    c.deviceWindow            = (uint64_t)(uintptr_t)pp.hDeviceWindow;
    c.windowed                = pp.Windowed ? 1u : 0u;
    c.enableAutoDepthStencil  = pp.EnableAutoDepthStencil ? 1u : 0u;
    c.autoDepthStencilFormat  = (uint32_t)pp.AutoDepthStencilFormat;
    c.flags                   = pp.Flags;
    c.fullScreenRefreshRateHz = pp.FullScreen_RefreshRateInHz;
    c.presentationInterval    = pp.PresentationInterval;
    return c;
}

/* D9CDisplayModeEx ← D3DDISPLAYMODEEX */
static D9CDisplayModeEx toCdme(const D3DDISPLAYMODEEX& m) {
    D9CDisplayModeEx c{};
    c.width          = m.Width;
    c.height         = m.Height;
    c.refreshRate    = m.RefreshRate;
    c.format         = (uint32_t)m.Format;
    c.scanLineOrdering = (uint32_t)m.ScanLineOrdering;
    return c;
}

static bool isValidDisplayModeFilter(const D3DDISPLAYMODEFILTER* filter) {
    return !filter || filter->Size == sizeof(D3DDISPLAYMODEFILTER);
}

static bool filterAllowsProgressiveModes(const D3DDISPLAYMODEFILTER* filter) {
    if (!filter) return true;
    return filter->ScanLineOrdering == D3DSCANLINEORDERING_UNKNOWN ||
           filter->ScanLineOrdering == D3DSCANLINEORDERING_PROGRESSIVE;
}

static bool filterMatchesAllFormats(const D3DDISPLAYMODEFILTER* filter) {
    return !filter || filter->Format == D3DFMT_UNKNOWN;
}

static constexpr D3DFORMAT kDisplayModeFormats[] = {
    D3DFMT_X8R8G8B8,
    D3DFMT_R5G6B5,
};

static D3DFORMAT exposeAdapterDisplayFormat(D3DFORMAT fmt) {
    if (fmt == D3DFMT_A8R8G8B8) return D3DFMT_X8R8G8B8;
    return fmt;
}

static UINT getAdapterModeCountForFilter(D9CFactory* f, UINT adapter,
                                          const D3DDISPLAYMODEFILTER* filter) {
    if (!isValidDisplayModeFilter(filter) || !filterAllowsProgressiveModes(filter)) {
        return 0;
    }

    if (!filterMatchesAllFormats(filter)) {
        if (!isSupportedAdapterModeFormat(filter->Format)) {
            return 0;
        }
        return dxmt9c_factory_get_adapter_mode_count(f, adapter, (uint32_t)filter->Format);
    }

    UINT count = 0;
    for (D3DFORMAT format : kDisplayModeFormats) {
        count += dxmt9c_factory_get_adapter_mode_count(f, adapter, (uint32_t)format);
    }
    return count;
}

[[nodiscard]] static HRESULT enumAdapterModeForFormat(D9CFactory* f, UINT adapter, D3DFORMAT format,
                                         UINT mode, D3DDISPLAYMODEEX* out) {
    if (!isSupportedAdapterModeFormat(format)) {
        return D3DERR_INVALIDCALL;
    }

    uint32_t w, h, refresh, fOut;
    HRESULT hr = hr32(dxmt9c_factory_enum_adapter_modes(
        f, adapter, (uint32_t)format, mode, &w, &h, &refresh, &fOut));
    if (FAILED(hr)) return hr;

    out->Size             = sizeof(D3DDISPLAYMODEEX);
    out->Width            = w;
    out->Height           = h;
    out->RefreshRate      = refresh;
    out->Format           = exposeAdapterDisplayFormat((D3DFORMAT)fOut);
    out->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    return S_OK;
}

[[nodiscard]] static HRESULT enumAdapterModeForFilter(D9CFactory* f, UINT adapter,
                                         const D3DDISPLAYMODEFILTER* filter,
                                         UINT mode, D3DDISPLAYMODEEX* out) {
    if (!isValidDisplayModeFilter(filter) || !filterAllowsProgressiveModes(filter)) {
        return D3DERR_INVALIDCALL;
    }

    if (!filterMatchesAllFormats(filter)) {
        return enumAdapterModeForFormat(f, adapter, filter->Format, mode, out);
    }

    UINT remaining = mode;
    for (D3DFORMAT format : kDisplayModeFormats) {
        const UINT count = dxmt9c_factory_get_adapter_mode_count(f, adapter, (uint32_t)format);
        if (remaining < count) {
            return enumAdapterModeForFormat(f, adapter, format, remaining, out);
        }
        remaining -= count;
    }
    return D3DERR_INVALIDCALL;
}

static bool isValidCheckDeviceResourceType(D3DRESOURCETYPE rtype) {
    switch (rtype) {
    case D3DRTYPE_SURFACE:
    case D3DRTYPE_TEXTURE:
    case D3DRTYPE_CUBETEXTURE:
    case D3DRTYPE_VOLUME:
    case D3DRTYPE_VOLUMETEXTURE:
    case D3DRTYPE_VERTEXBUFFER:
    case D3DRTYPE_INDEXBUFFER:
        return true;
    default:
        return false;
    }
}

/* D3DCAPS9 ← D9CCaps */
static void fillD3DCaps9(const D9CCaps& src, D3DCAPS9* out) {
    ZeroMemory(out, sizeof(*out));
    out->DeviceType             = fromCDeviceType(src.deviceType);
    out->AdapterOrdinal         = src.adapterOrdinal;
    out->Caps                   = src.caps;
    out->Caps2                  = src.caps2;
    out->Caps3                  = src.caps3;
    out->PresentationIntervals  = src.presentationIntervals;
    out->CursorCaps             = src.cursorCaps;
    out->DevCaps                = src.devCaps;
    out->PrimitiveMiscCaps      = src.primitiveMiscCaps;
    out->RasterCaps             = src.rasterCaps;
    out->ZCmpCaps               = src.zCmpCaps;
    out->SrcBlendCaps           = src.srcBlendCaps;
    out->DestBlendCaps          = src.destBlendCaps;
    out->AlphaCmpCaps           = src.alphaBlendCaps;
    out->ShadeCaps              = src.shadeCaps;
    out->TextureCaps            = src.textureCaps;
    out->TextureFilterCaps      = src.textureFilterCaps;
    out->CubeTextureFilterCaps  = src.cubetextureFilterCaps;
    out->VolumeTextureFilterCaps= src.volumeTextureFilterCaps;
    out->TextureAddressCaps     = src.textureAddressCaps;
    out->VolumeTextureAddressCaps = src.volumeTextureAddressCaps;
    out->LineCaps               = src.lineCaps;
    out->MaxTextureWidth        = src.maxTextureWidth;
    out->MaxTextureHeight       = src.maxTextureHeight;
    out->MaxVolumeExtent        = src.maxVolumeExtent;
    out->MaxTextureRepeat       = src.maxTextureRepeat;
    out->MaxTextureAspectRatio  = src.maxTextureAspectRatio;
    out->MaxAnisotropy          = src.maxAnisotropy;
    out->MaxVertexW             = src.maxVertexW;
    out->GuardBandLeft          = src.guardBandLeft;
    out->GuardBandTop           = src.guardBandTop;
    out->GuardBandRight         = src.guardBandRight;
    out->GuardBandBottom        = src.guardBandBottom;
    out->ExtentsAdjust          = src.extentsAdjust;
    out->StencilCaps            = src.stencilCaps;
    out->FVFCaps                = src.fvfCaps;
    out->TextureOpCaps          = src.textureBlendCaps;
    out->MaxTextureBlendStages  = src.maxTextureBlendStages;
    out->MaxSimultaneousTextures= src.maxSimultaneousTextures;
    out->VertexProcessingCaps   = src.vertexProcessingCaps;
    out->MaxActiveLights        = src.maxActiveLights;
    out->MaxUserClipPlanes      = src.maxUserClipPlanes;
    out->MaxVertexBlendMatrices = src.maxVertexBlendMatrices;
    out->MaxVertexBlendMatrixIndex = src.maxVertexBlendMatrixIndex;
    out->MaxPointSize           = src.maxPointSize;
    out->MaxPrimitiveCount      = src.maxPrimitiveCount;
    out->MaxVertexIndex         = src.maxVertexIndex;
    out->MaxStreams              = src.maxStreams;
    out->MaxStreamStride        = src.maxStreamStride;
    out->VertexShaderVersion    = src.vertexShaderVersion;
    out->MaxVertexShaderConst   = src.maxVertexShaderConst;
    out->PixelShaderVersion     = src.pixelShaderVersion;
    out->PixelShader1xMaxValue  = src.pixelShader1xMaxValue;
    out->DevCaps2               = src.devCaps2;
    out->MaxNpatchTessellationLevel = src.maxNpatchTessellationLevel;
    out->Reserved5              = src.reserved5;
    out->MasterAdapterOrdinal   = src.masterAdapterOrdinal;
    out->AdapterOrdinalInGroup  = src.adapterOrdinalInGroup;
    out->NumberOfAdaptersInGroup= src.numberOfAdaptersInGroup;
    out->DeclTypes              = src.declTypes;
    out->NumSimultaneousRTs     = src.numSimultaneousRTs;
    out->StretchRectFilterCaps  = src.stretchRectFilterCaps;
    out->VS20Caps.Caps                  = src.vs20Caps;
    out->VS20Caps.DynamicFlowControlDepth = (INT)src.vs20DynamicFlowControlDepth;
    out->VS20Caps.NumTemps              = (INT)src.vs20NumTemps;
    out->VS20Caps.StaticFlowControlDepth= (INT)src.vs20StaticFlowControlDepth;
    out->PS20Caps.Caps                  = src.ps20Caps;
    out->PS20Caps.DynamicFlowControlDepth = (INT)src.ps20DynamicFlowControlDepth;
    out->PS20Caps.NumTemps              = (INT)src.ps20NumTemps;
    out->PS20Caps.StaticFlowControlDepth= (INT)src.ps20StaticFlowControlDepth;
    out->PS20Caps.NumInstructionSlots   = (INT)src.ps20NumInstructionSlots;
    out->VertexTextureFilterCaps        = src.vertexTextureFilterCaps;
    out->MaxVShaderInstructionsExecuted = src.maxVShaderInstructionsExecuted;
    out->MaxPShaderInstructionsExecuted = src.maxPShaderInstructionsExecuted;
    out->MaxVertexShader30InstructionSlots = src.maxVertexShader30InstructionSlots;
    out->MaxPixelShader30InstructionSlots  = src.maxPixelShader30InstructionSlots;
}

/* ── D3D9FactoryImpl ─────────────────────────────────────────────────────── */

class D3D9FactoryImpl final : public IDirect3D9Ex {
    ULONG       refs_ = 1;
    D9CFactory* f_;
    bool        extended_ = false;

public:
    explicit D3D9FactoryImpl(D9CFactory* f, bool extended) : f_(f), extended_(extended) {
        dxmt9FactoryDebugLog("ctor this=%p factory=%p extended=%u", this, f_, extended_ ? 1u : 0u);
    }
    ~D3D9FactoryImpl() {
        dxmt9FactoryDebugLog("dtor this=%p factory=%p", this, f_);
        dxmt9c_factory_release(f_);
    }

    /* ── IUnknown ── */

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)       ||
            IsEqualGUID(riid, IID_IDirect3D9)) {
            *ppv = static_cast<IDirect3D9*>(this); AddRef(); return S_OK;
        }
        if (IsEqualGUID(riid, IID_IDirect3D9Ex)) {
            if (!extended_) {
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            *ppv = static_cast<IDirect3D9Ex*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    /* ── IDirect3D9 ── */

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void*) noexcept override {
        return D3DERR_INVALIDCALL;
    }

    UINT STDMETHODCALLTYPE GetAdapterCount() noexcept override {
        dxmt9FactoryDebugLog("GetAdapterCount");
        return dxmt9c_factory_adapter_count(f_);
    }

    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD /*flags*/,
                                                    D3DADAPTER_IDENTIFIER9* pId) noexcept override {
        if (!pId) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("GetAdapterIdentifier adapter=%u", adapter);
        D9CAdapterIdentifier ci{};
        HRESULT hr = hr32(dxmt9c_factory_get_adapter_identifier(f_, adapter, &ci));
        if (FAILED(hr)) return hr;
        memcpy(pId->Driver,      ci.driver,      sizeof(pId->Driver));
        memcpy(pId->Description, ci.description, sizeof(pId->Description));
        memcpy(pId->DeviceName,  ci.deviceName,  sizeof(pId->DeviceName));
        pId->DriverVersion.QuadPart = (LONGLONG)ci.driverVersion;
        pId->VendorId    = ci.vendorId;
        pId->DeviceId    = ci.deviceId;
        pId->SubSysId    = ci.subSysId;
        pId->Revision    = ci.revision;
        memcpy(&pId->DeviceIdentifier, ci.deviceIdentifier,
               sizeof(pId->DeviceIdentifier));
        pId->WHQLLevel   = ci.whqlLevel;
        dxmt9FactoryDebugLog("GetAdapterIdentifier -> driver=%s desc=%s device=%s vendor=0x%04x deviceId=0x%04x subsys=0x%08x rev=%u driverVersion=0x%llx",
                             pId->Driver,
                             pId->Description,
                             pId->DeviceName,
                             (unsigned)pId->VendorId,
                             (unsigned)pId->DeviceId,
                             (unsigned)pId->SubSysId,
                             (unsigned)pId->Revision,
                             static_cast<unsigned long long>(pId->DriverVersion.QuadPart));
        return S_OK;
    }

    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter,
                                                D3DFORMAT fmt) noexcept override {
        dxmt9FactoryDebugLog("GetAdapterModeCount adapter=%u fmt=%u", adapter, (unsigned)fmt);
        if (!isSupportedAdapterModeFormat(fmt)) {
            return 0;
        }
        return dxmt9c_factory_get_adapter_mode_count(f_, adapter,
                                                      (uint32_t)fmt);
    }

    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, D3DFORMAT fmt,
                                                UINT mode,
                                                D3DDISPLAYMODE* pMode) noexcept override {
        if (!pMode) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("EnumAdapterModes adapter=%u fmt=%u mode=%u", adapter, (unsigned)fmt, mode);
        if (!isSupportedAdapterModeFormat(fmt)) {
            return D3DERR_INVALIDCALL;
        }
        uint32_t w, h, refresh, f;
        HRESULT hr = hr32(dxmt9c_factory_enum_adapter_modes(
            f_, adapter, (uint32_t)fmt, mode, &w, &h, &refresh, &f));
        if (FAILED(hr)) return hr;
        pMode->Width = w; pMode->Height = h;
        pMode->RefreshRate = refresh; pMode->Format = exposeAdapterDisplayFormat((D3DFORMAT)f);
        dxmt9FactoryDebugLog("EnumAdapterModes -> %ux%u refresh=%u fmt=%u",
                             (unsigned)pMode->Width, (unsigned)pMode->Height,
                             (unsigned)pMode->RefreshRate, (unsigned)pMode->Format);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter,
                                                     D3DDISPLAYMODE* pMode) noexcept override {
        if (!pMode) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("GetAdapterDisplayMode adapter=%u", adapter);
        if (adapter >= dxmt9c_factory_adapter_count(f_)) return D3DERR_INVALIDCALL;
        uint32_t w, h, refresh, f;
        HRESULT hr = hr32(dxmt9c_factory_get_adapter_display_mode(
            f_, adapter, &w, &h, &refresh, &f));
        if (FAILED(hr)) return hr;
        pMode->Width = w; pMode->Height = h;
        pMode->RefreshRate = refresh; pMode->Format = exposeAdapterDisplayFormat((D3DFORMAT)f);
        dxmt9FactoryDebugLog("GetAdapterDisplayMode -> %ux%u refresh=%u fmt=%u",
                             (unsigned)pMode->Width, (unsigned)pMode->Height,
                             (unsigned)pMode->RefreshRate, (unsigned)pMode->Format);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE deviceType,
                                               D3DFORMAT adapterFmt,
                                               D3DFORMAT backFmt,
                                               BOOL windowed) noexcept override {
        dxmt9FactoryDebugLog("CheckDeviceType adapter=%u devType=%u adapterFmt=%u backFmt=%u windowed=%u",
                             adapter, (unsigned)deviceType, (unsigned)adapterFmt, (unsigned)backFmt, windowed ? 1u : 0u);
        if (adapter >= dxmt9c_factory_adapter_count(f_)) {
            dxmt9FactoryDebugLog("CheckDeviceType -> invalid adapter=%u", adapter);
            return D3DERR_INVALIDCALL;
        }
        if (!isKnownDeviceType(deviceType)) {
            dxmt9FactoryDebugLog("CheckDeviceType -> invalid devType=%u", (unsigned)deviceType);
            return D3DERR_INVALIDCALL;
        }
        if (!isSupportedDeviceType(deviceType)) {
            dxmt9FactoryDebugLog("CheckDeviceType -> unsupported devType=%u", (unsigned)deviceType);
            return D3DERR_NOTAVAILABLE;
        }
        if (!windowed && !isSupportedFullscreenDisplayFormat(adapterFmt)) {
            dxmt9FactoryDebugLog("CheckDeviceType -> unsupported fullscreen display fmt=%u", (unsigned)adapterFmt);
            return D3DERR_NOTAVAILABLE;
        }
        const HRESULT hr = hr32(dxmt9c_factory_check_device_type(
            f_, adapter, kD9CDeviceTypeHal,
            (uint32_t)adapterFmt, (uint32_t)backFmt,
            windowed ? 1u : 0u));
        dxmt9FactoryDebugLog("CheckDeviceType -> hr=0x%08x", (unsigned)hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE deviceType,
                                                 D3DFORMAT adapterFmt,
                                                 DWORD usage,
                                                 D3DRESOURCETYPE rtype,
                                                 D3DFORMAT fmt) noexcept override {
        dxmt9FactoryDebugLog("CheckDeviceFormat adapter=%u devType=%u adapterFmt=%u rtype=%u fmt=%u usage=0x%x",
                             adapter, (unsigned)deviceType, (unsigned)adapterFmt,
                             (unsigned)rtype, (unsigned)fmt, (unsigned)usage);
        if (adapter >= dxmt9c_factory_adapter_count(f_)) {
            dxmt9FactoryDebugLog("CheckDeviceFormat -> invalid adapter=%u", adapter);
            return D3DERR_INVALIDCALL;
        }
        if (!isKnownDeviceType(deviceType)) {
            dxmt9FactoryDebugLog("CheckDeviceFormat -> invalid devType=%u", (unsigned)deviceType);
            return D3DERR_INVALIDCALL;
        }
        if (!isSupportedDeviceType(deviceType)) {
            dxmt9FactoryDebugLog("CheckDeviceFormat -> unsupported devType=%u", (unsigned)deviceType);
            return D3DERR_NOTAVAILABLE;
        }
        if (adapterFmt == D3DFMT_UNKNOWN) {
            dxmt9FactoryDebugLog("CheckDeviceFormat -> unknown adapterFmt");
            return D3DERR_INVALIDCALL;
        }
        if (!isValidCheckDeviceAdapterFormat(adapterFmt)) {
            dxmt9FactoryDebugLog("CheckDeviceFormat -> unsupported adapterFmt=%u", (unsigned)adapterFmt);
            return D3DERR_NOTAVAILABLE;
        }
        if (!isValidCheckDeviceResourceType(rtype)) {
            dxmt9FactoryDebugLog("CheckDeviceFormat -> invalid rtype=%u", (unsigned)rtype);
            return D3DERR_INVALIDCALL;
        }
        /* Wine `dlls/d3d9/tests/device.c::test_check_device_format` (~line
         * 12634): VERTEXBUFFER / INDEXBUFFER return D3DERR_INVALIDCALL
         * regardless of usage flags. Wine itself marks these todo_wine, but
         * the expected Windows behaviour is the INVALIDCALL we set here. */
        if (rtype == D3DRTYPE_VERTEXBUFFER || rtype == D3DRTYPE_INDEXBUFFER) {
            dxmt9FactoryDebugLog("CheckDeviceFormat -> VB/IB rtype rejected rtype=%u",
                                 (unsigned)rtype);
            return D3DERR_INVALIDCALL;
        }
        /* vendor_policy_fetch4_caps: sampleable-depth (USAGE_DEPTHSTENCIL on
         * a TEXTURE resource) for D24S8 must report NOTAVAILABLE so apps
         * fall off the FETCH4 fast path. dxmt9 does not implement
         * depth-as-texture sampling. */
        if (rtype == D3DRTYPE_TEXTURE && (usage & D3DUSAGE_DEPTHSTENCIL)
                && (fmt == D3DFMT_D24S8 || fmt == D3DFMT_D24X8
                    || fmt == D3DFMT_D16 || fmt == D3DFMT_D32
                    || fmt == D3DFMT_D15S1 || fmt == D3DFMT_D24X4S4
                    || fmt == D3DFMT_D24FS8 || fmt == D3DFMT_D32F_LOCKABLE
                    || fmt == D3DFMT_D16_LOCKABLE
                    || fmt == D3DFMT_D32_LOCKABLE)) {
            dxmt9FactoryDebugLog("CheckDeviceFormat -> depth-as-texture NOTAVAILABLE fmt=%u",
                                 (unsigned)fmt);
            return D3DERR_NOTAVAILABLE;
        }
        HRESULT hr = hr32(dxmt9c_factory_check_device_format2(
            f_, adapter, (uint32_t)fmt, usage, (uint32_t)rtype));
        /* D3DUSAGE_AUTOGENMIPMAP is informational: when the underlying
         * format is otherwise supported but we cannot drive HW mipmap
         * autogen, return D3DOK_NOAUTOGEN (a SUCCESS code) so the app
         * knows to fall back to manual mipmap generation. dxmt9 currently
         * has no autogen path, so any successful AUTOGENMIPMAP query is
         * downgraded. See Wine test_check_device_format autogen loop. */
        if (SUCCEEDED(hr) && (usage & D3DUSAGE_AUTOGENMIPMAP)) {
            hr = D3DOK_NOAUTOGEN;
        }
        dxmt9FactoryDebugLog("CheckDeviceFormat -> hr=0x%08x", (unsigned)hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter,
                                                          D3DDEVTYPE deviceType,
                                                          D3DFORMAT fmt,
                                                          BOOL windowed,
                                                          D3DMULTISAMPLE_TYPE msType,
                                                          DWORD* pQuality) noexcept override {
        dxmt9FactoryDebugLog("CheckDeviceMultiSampleType adapter=%u devType=%u fmt=%u windowed=%u msType=%u",
                             adapter, (unsigned)deviceType, (unsigned)fmt, windowed ? 1u : 0u, (unsigned)msType);
        if (adapter >= dxmt9c_factory_adapter_count(f_)) {
            dxmt9FactoryDebugLog("CheckDeviceMultiSampleType -> invalid adapter=%u", adapter);
            return D3DERR_INVALIDCALL;
        }
        if (!isKnownDeviceType(deviceType)) {
            dxmt9FactoryDebugLog("CheckDeviceMultiSampleType -> invalid devType=%u", (unsigned)deviceType);
            return D3DERR_INVALIDCALL;
        }
        if (!isSupportedDeviceType(deviceType)) {
            if (pQuality) *pQuality = 0u;
            dxmt9FactoryDebugLog("CheckDeviceMultiSampleType -> unsupported devType=%u", (unsigned)deviceType);
            return D3DERR_NOTAVAILABLE;
        }
        if (fmt == D3DFMT_UNKNOWN || !isKnownMultiSampleType(msType)) {
            dxmt9FactoryDebugLog("CheckDeviceMultiSampleType -> invalid fmt/msType fmt=%u msType=%u",
                                 (unsigned)fmt, (unsigned)msType);
            return D3DERR_INVALIDCALL;
        }
        HRESULT hr = hr32(dxmt9c_factory_check_device_multisample(
            f_, adapter, (uint32_t)fmt, (uint32_t)msType,
            windowed ? 1u : 0u));
        if (pQuality) *pQuality = SUCCEEDED(hr) ? 1u : 0u;
        dxmt9FactoryDebugLog("CheckDeviceMultiSampleType -> hr=0x%08x quality=%u",
                             (unsigned)hr, pQuality ? (unsigned)*pQuality : 0u);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT, D3DDEVTYPE deviceType,
                                                      D3DFORMAT adapterFmt,
                                                      D3DFORMAT rtFmt,
                                                      D3DFORMAT dsFmt) noexcept override {
        if (!isSupportedDeviceType(deviceType)) {
            dxmt9FactoryDebugLog("CheckDepthStencilMatch -> unsupported devType=%u", (unsigned)deviceType);
            return D3DERR_NOTAVAILABLE;
        }
        /* Real bit-depth match (Wine `depth_stencil_match` in
         * `dlls/wined3d/directx.c`). 32-bit RTs pair with D24S8/D24X8/D32,
         * 16-bit RTs pair with D16, anything else is rejected. */
        const bool ok = dxmt9::core::dxmt9FormatPair_isDepthStencilCompatible(
            (uint32_t)adapterFmt, (uint32_t)rtFmt, (uint32_t)dsFmt);
        const HRESULT hr = ok ? S_OK : D3DERR_NOTAVAILABLE;
        dxmt9FactoryDebugLog("CheckDepthStencilMatch adapterFmt=%u rtFmt=%u dsFmt=%u -> hr=0x%08x",
                             (unsigned)adapterFmt, (unsigned)rtFmt,
                             (unsigned)dsFmt, (unsigned)hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT, D3DDEVTYPE deviceType,
                                                           D3DFORMAT srcFmt,
                                                           D3DFORMAT dstFmt) noexcept override {
        if (!isSupportedDeviceType(deviceType)) {
            dxmt9FactoryDebugLog("CheckDeviceFormatConversion -> unsupported devType=%u", (unsigned)deviceType);
            return D3DERR_NOTAVAILABLE;
        }
        const bool ok = dxmt9::core::dxmt9FormatPair_canConvert(
            (uint32_t)srcFmt, (uint32_t)dstFmt);
        const HRESULT hr = ok ? D3D_OK : D3DERR_NOTAVAILABLE;
        dxmt9FactoryDebugLog("CheckDeviceFormatConversion src=%u dst=%u -> hr=0x%08x",
                             (unsigned)srcFmt, (unsigned)dstFmt, (unsigned)hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE deviceType,
                                             D3DCAPS9* pCaps) noexcept override {
        if (!pCaps) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("GetDeviceCaps adapter=%u devType=%u", adapter, (unsigned)deviceType);
        if (!isSupportedDeviceType(deviceType)) {
            dxmt9FactoryDebugLog("GetDeviceCaps -> unsupported devType=%u", (unsigned)deviceType);
            return D3DERR_NOTAVAILABLE;
        }
        D9CCaps cc{};
        HRESULT hr = hr32(dxmt9c_factory_get_caps(f_, adapter, &cc));
        if (SUCCEEDED(hr)) {
            fillD3DCaps9(cc, pCaps);
            pCaps->DeviceType = deviceType;
            dxmt9FactoryDebugLog("GetDeviceCaps -> vs=0x%08x ps=0x%08x maxTex=%ux%u maxRT=%u maxLights=%u maxStreams=%u maxAniso=%u intervals=0x%x devCaps=0x%x rasterCaps=0x%x texCaps=0x%x textureOpCaps=0x%x",
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
        }
        return hr;
    }

    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) noexcept override {
        dxmt9FactoryDebugLog("GetAdapterMonitor adapter=%u", adapter);
        return (HMONITOR)(uintptr_t)dxmt9c_factory_get_adapter_monitor(f_,
                                                                         adapter);
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE deviceType,
                                            HWND hwnd, DWORD behaviorFlags,
                                            D3DPRESENT_PARAMETERS* pPP,
                                            IDirect3DDevice9** ppDevice) noexcept override {
        if (ppDevice) *ppDevice = nullptr;
        if (!pPP || !ppDevice) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("CreateDevice adapter=%u devType=%u hwnd=%p behavior=0x%x windowed=%u size=%ux%u fmt=%u",
                             adapter, (unsigned)deviceType, hwnd, (unsigned)behaviorFlags,
                             pPP->Windowed ? 1u : 0u,
                             (unsigned)pPP->BackBufferWidth, (unsigned)pPP->BackBufferHeight,
                             (unsigned)pPP->BackBufferFormat);
        if (!isKnownDeviceType(deviceType)) return D3DERR_INVALIDCALL;
        if (!hasValidVertexProcessingFlags(behaviorFlags)) return D3DERR_INVALIDCALL;
        if (!isSupportedDeviceType(deviceType)) {
            dxmt9FactoryDebugLog("CreateDevice -> unsupported devType=%u", (unsigned)deviceType);
            return D3DERR_NOTAVAILABLE;
        }
        if (const HRESULT validateHr = validatePresentParametersD3D(*pPP, false); FAILED(validateHr)) {
            dxmt9FactoryDebugLog("CreateDevice -> invalid present parameters hr=0x%08x", (unsigned)validateHr);
            return validateHr;
        }
        D9CPresentParams cpp = toCpp(*pPP);
        if (!cpp.deviceWindow) {
            cpp.deviceWindow = (uint64_t)(uintptr_t)hwnd;
        }
        D9CDevice* dev = nullptr;
        const HRESULT hr = hr32(dxmt9c_factory_create_device2(f_, adapter, &cpp,
                                                              behaviorFlags, nullptr, &dev));
        if (FAILED(hr)) {
            dxmt9FactoryDebugLog("CreateDevice -> failed hr=0x%08x", (unsigned)hr);
            return hr;
        }
        if (!dev) {
            dxmt9FactoryDebugLog("CreateDevice -> succeeded without device");
            return D3DERR_INVALIDCALL;
        }
        *ppDevice = CreateDeviceImpl(dev, this, adapter, deviceType,
                                     behaviorFlags, hwnd, extended_,
                                     pPP->Flags);
        dxmt9FactoryDebugLog("CreateDevice -> device=%p", *ppDevice);
        return S_OK;
    }

    /* ── IDirect3D9Ex ── */

    UINT STDMETHODCALLTYPE GetAdapterModeCountEx(UINT adapter,
                                                  const D3DDISPLAYMODEFILTER* pFilter) noexcept override {
        dxmt9FactoryDebugLog("GetAdapterModeCountEx adapter=%u filter=%p fmt=%u scan=%u",
                             adapter, pFilter,
                             pFilter ? (unsigned)pFilter->Format : (unsigned)D3DFMT_UNKNOWN,
                             pFilter ? (unsigned)pFilter->ScanLineOrdering : (unsigned)D3DSCANLINEORDERING_UNKNOWN);
        if (adapter >= dxmt9c_factory_adapter_count(f_)) {
            return 0;
        }
        const UINT count = getAdapterModeCountForFilter(f_, adapter, pFilter);
        dxmt9FactoryDebugLog("GetAdapterModeCountEx -> count=%u", count);
        return count;
    }

    HRESULT STDMETHODCALLTYPE EnumAdapterModesEx(UINT adapter,
                                                  const D3DDISPLAYMODEFILTER* pFilter,
                                                  UINT mode,
                                                  D3DDISPLAYMODEEX* pMode) noexcept override {
        if (!pMode) return D3DERR_INVALIDCALL;
        if (pMode->Size != sizeof(D3DDISPLAYMODEEX)) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("EnumAdapterModesEx adapter=%u filter=%p fmt=%u scan=%u mode=%u",
                             adapter, pFilter,
                             pFilter ? (unsigned)pFilter->Format : (unsigned)D3DFMT_UNKNOWN,
                             pFilter ? (unsigned)pFilter->ScanLineOrdering : (unsigned)D3DSCANLINEORDERING_UNKNOWN,
                             mode);
        if (adapter >= dxmt9c_factory_adapter_count(f_)) return D3DERR_INVALIDCALL;
        const HRESULT hr = enumAdapterModeForFilter(f_, adapter, pFilter, mode, pMode);
        dxmt9FactoryDebugLog("EnumAdapterModesEx -> hr=0x%08x", (unsigned)hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetAdapterDisplayModeEx(UINT adapter,
                                                       D3DDISPLAYMODEEX* pMode,
                                                       D3DDISPLAYROTATION* pRot) noexcept override {
        if (!pMode) return D3DERR_INVALIDCALL;
        if (pMode->Size != sizeof(D3DDISPLAYMODEEX)) return D3DERR_INVALIDCALL;
        uint32_t w, h, refresh, f;
        dxmt9FactoryDebugLog("GetAdapterDisplayModeEx adapter=%u", adapter);
        if (adapter >= dxmt9c_factory_adapter_count(f_)) return D3DERR_INVALIDCALL;
        HRESULT hr = hr32(dxmt9c_factory_get_adapter_display_mode(
            f_, adapter, &w, &h, &refresh, &f));
        if (FAILED(hr)) return hr;
        pMode->Size             = sizeof(D3DDISPLAYMODEEX);
        pMode->Width            = w;
        pMode->Height           = h;
        pMode->RefreshRate      = refresh;
        pMode->Format           = exposeAdapterDisplayFormat((D3DFORMAT)f);
        pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
        dxmt9FactoryDebugLog("GetAdapterDisplayModeEx -> %ux%u refresh=%u fmt=%u",
                             (unsigned)pMode->Width, (unsigned)pMode->Height,
                             (unsigned)pMode->RefreshRate, (unsigned)pMode->Format);
        if (pRot) *pRot = D3DDISPLAYROTATION_IDENTITY;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateDeviceEx(UINT adapter, D3DDEVTYPE deviceType,
                                              HWND hwnd, DWORD behaviorFlags,
                                              D3DPRESENT_PARAMETERS* pPP,
                                              D3DDISPLAYMODEEX* pFsMode,
                                              IDirect3DDevice9Ex** ppDevice) noexcept override {
        if (ppDevice) *ppDevice = nullptr;
        if (!pPP || !ppDevice) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("CreateDeviceEx adapter=%u devType=%u hwnd=%p behavior=0x%x windowed=%u size=%ux%u fmt=%u fsMode=%d",
                             adapter, (unsigned)deviceType, hwnd, (unsigned)behaviorFlags,
                             pPP->Windowed ? 1u : 0u,
                             (unsigned)pPP->BackBufferWidth, (unsigned)pPP->BackBufferHeight,
                             (unsigned)pPP->BackBufferFormat, pFsMode ? 1 : 0);
        if (!isKnownDeviceType(deviceType)) return D3DERR_INVALIDCALL;
        if (!hasValidVertexProcessingFlags(behaviorFlags)) return D3DERR_INVALIDCALL;
        if (!isSupportedDeviceType(deviceType)) {
            dxmt9FactoryDebugLog("CreateDeviceEx -> unsupported devType=%u", (unsigned)deviceType);
            return D3DERR_NOTAVAILABLE;
        }
        if (const HRESULT validateHr = validatePresentParametersD3D(*pPP, true); FAILED(validateHr)) {
            dxmt9FactoryDebugLog("CreateDeviceEx -> invalid present parameters hr=0x%08x", (unsigned)validateHr);
            return validateHr;
        }
        if (pFsMode && pFsMode->Size != sizeof(D3DDISPLAYMODEEX)) {
            dxmt9FactoryDebugLog("CreateDeviceEx -> invalid fullscreen mode size=%u", (unsigned)pFsMode->Size);
            return D3DERR_INVALIDCALL;
        }
        if (pFsMode && pPP->Windowed) {
            dxmt9FactoryDebugLog("CreateDeviceEx -> fullscreen mode supplied for windowed params");
            return D3DERR_INVALIDCALL;
        }
        if (!pPP->Windowed && !pFsMode) {
            dxmt9FactoryDebugLog("CreateDeviceEx -> fullscreen params require fullscreen mode");
            return D3DERR_INVALIDCALL;
        }
        if (pFsMode &&
            (pFsMode->Width != pPP->BackBufferWidth || pFsMode->Height != pPP->BackBufferHeight)) {
            dxmt9FactoryDebugLog("CreateDeviceEx -> fullscreen mode size %ux%u != backbuffer %ux%u",
                                 (unsigned)pFsMode->Width, (unsigned)pFsMode->Height,
                                 (unsigned)pPP->BackBufferWidth, (unsigned)pPP->BackBufferHeight);
            return D3DERR_INVALIDCALL;
        }
        D9CPresentParams cpp = toCpp(*pPP);
        if (!cpp.deviceWindow) {
            cpp.deviceWindow = (uint64_t)(uintptr_t)hwnd;
        }
        D9CDisplayModeEx cdme{};
        if (pFsMode) cdme = toCdme(*pFsMode);
        D9CDevice* dev = nullptr;
        const HRESULT hr = hr32(dxmt9c_factory_create_device2(f_, adapter, &cpp,
                                                              behaviorFlags,
                                                              pFsMode ? &cdme : nullptr,
                                                              &dev));
        if (FAILED(hr)) {
            dxmt9FactoryDebugLog("CreateDeviceEx -> failed hr=0x%08x", (unsigned)hr);
            return hr;
        }
        if (!dev) {
            dxmt9FactoryDebugLog("CreateDeviceEx -> succeeded without device");
            return D3DERR_INVALIDCALL;
        }
        *ppDevice = CreateDeviceImpl(dev, this, adapter, deviceType,
                                     behaviorFlags, hwnd, extended_,
                                     pPP->Flags);
        dxmt9FactoryDebugLog("CreateDeviceEx -> device=%p", *ppDevice);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT adapter, LUID* pLuid) noexcept override {
        if (!pLuid) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("GetAdapterLUID adapter=%u", adapter);
        uint32_t lo; int32_t hi;
        HRESULT hr = hr32(dxmt9c_factory_get_adapter_luid(f_, adapter, &lo, &hi));
        if (SUCCEEDED(hr)) { pLuid->LowPart = lo; pLuid->HighPart = hi; }
        return hr;
    }
};

/* ── factory constructors (called from entry.cpp) ────────────────────────── */

IDirect3D9* CreateFactoryImpl(D9CFactory* f) {
    auto* impl = new D3D9FactoryImpl(f, false);
    dxmt9FactoryDebugLog("CreateFactoryImpl impl=%p", impl);
    return impl;
}
IDirect3D9Ex* CreateFactoryExImpl(D9CFactory* f) {
    auto* impl = new D3D9FactoryImpl(f, true);
    dxmt9FactoryDebugLog("CreateFactoryExImpl impl=%p", impl);
    return impl;
}

/* ── fillD3DCaps9 exported for device.cpp ────────────────────────────────── */
void FillD3DCaps9(const D9CCaps& src, D3DCAPS9* out) {
    fillD3DCaps9(src, out);
}
