/* src/d3d9/d3d9_pe_factory.cpp — PE-side IDirect3D9 / IDirect3D9Ex COM wrapper.
 * All methods delegate to the dxmt9c_factory_* C API. */

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include "d3d9_pe.hpp"
#include "util/log/log.hpp"

/* ── helpers ─────────────────────────────────────────────────────────────── */

static inline HRESULT hr32(int32_t r) { return (HRESULT)r; }

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

/* D3DCAPS9 ← D9CCaps */
static void fillD3DCaps9(const D9CCaps& src, D3DCAPS9* out) {
    ZeroMemory(out, sizeof(*out));
    out->DeviceType             = (D3DDEVTYPE)src.deviceType;
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
        return dxmt9c_factory_get_adapter_mode_count(f_, adapter,
                                                      (uint32_t)fmt);
    }

    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, D3DFORMAT fmt,
                                                UINT mode,
                                                D3DDISPLAYMODE* pMode) noexcept override {
        if (!pMode) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("EnumAdapterModes adapter=%u fmt=%u mode=%u", adapter, (unsigned)fmt, mode);
        uint32_t w, h, refresh, f;
        HRESULT hr = hr32(dxmt9c_factory_enum_adapter_modes(
            f_, adapter, (uint32_t)fmt, mode, &w, &h, &refresh, &f));
        if (FAILED(hr)) return hr;
        pMode->Width = w; pMode->Height = h;
        pMode->RefreshRate = refresh; pMode->Format = (D3DFORMAT)f;
        dxmt9FactoryDebugLog("EnumAdapterModes -> %ux%u refresh=%u fmt=%u",
                             (unsigned)pMode->Width, (unsigned)pMode->Height,
                             (unsigned)pMode->RefreshRate, (unsigned)pMode->Format);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter,
                                                     D3DDISPLAYMODE* pMode) noexcept override {
        if (!pMode) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("GetAdapterDisplayMode adapter=%u", adapter);
        uint32_t w, h, refresh, f;
        HRESULT hr = hr32(dxmt9c_factory_get_adapter_display_mode(
            f_, adapter, &w, &h, &refresh, &f));
        if (FAILED(hr)) return hr;
        pMode->Width = w; pMode->Height = h;
        pMode->RefreshRate = refresh; pMode->Format = (D3DFORMAT)f;
        dxmt9FactoryDebugLog("GetAdapterDisplayMode -> %ux%u refresh=%u fmt=%u",
                             (unsigned)pMode->Width, (unsigned)pMode->Height,
                             (unsigned)pMode->RefreshRate, (unsigned)pMode->Format);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE,
                                               D3DFORMAT adapterFmt,
                                               D3DFORMAT backFmt,
                                               BOOL windowed) noexcept override {
        dxmt9FactoryDebugLog("CheckDeviceType adapter=%u adapterFmt=%u backFmt=%u windowed=%u",
                             adapter, (unsigned)adapterFmt, (unsigned)backFmt, windowed ? 1u : 0u);
        const HRESULT hr = hr32(dxmt9c_factory_check_device_type(
            f_, adapter, (uint32_t)D3DDEVTYPE_HAL,
            (uint32_t)adapterFmt, (uint32_t)backFmt,
            windowed ? 1u : 0u));
        dxmt9FactoryDebugLog("CheckDeviceType -> hr=0x%08x", (unsigned)hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE,
                                                 D3DFORMAT /*adapterFmt*/,
                                                 DWORD usage,
                                                 D3DRESOURCETYPE /*rtype*/,
                                                 D3DFORMAT fmt) noexcept override {
        dxmt9FactoryDebugLog("CheckDeviceFormat adapter=%u fmt=%u usage=0x%x",
                             adapter, (unsigned)fmt, (unsigned)usage);
        const HRESULT hr = hr32(dxmt9c_factory_check_device_format(
            f_, adapter, (uint32_t)fmt, usage));
        dxmt9FactoryDebugLog("CheckDeviceFormat -> hr=0x%08x", (unsigned)hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter,
                                                          D3DDEVTYPE,
                                                          D3DFORMAT fmt,
                                                          BOOL windowed,
                                                          D3DMULTISAMPLE_TYPE msType,
                                                          DWORD* pQuality) noexcept override {
        dxmt9FactoryDebugLog("CheckDeviceMultiSampleType adapter=%u fmt=%u windowed=%u msType=%u",
                             adapter, (unsigned)fmt, windowed ? 1u : 0u, (unsigned)msType);
        HRESULT hr = hr32(dxmt9c_factory_check_device_multisample(
            f_, adapter, (uint32_t)fmt, (uint32_t)msType,
            windowed ? 1u : 0u));
        if (pQuality) *pQuality = SUCCEEDED(hr) ? 1u : 0u;
        dxmt9FactoryDebugLog("CheckDeviceMultiSampleType -> hr=0x%08x quality=%u",
                             (unsigned)hr, pQuality ? (unsigned)*pQuality : 0u);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT, D3DDEVTYPE,
                                                      D3DFORMAT, D3DFORMAT,
                                                      D3DFORMAT) noexcept override {
        dxmt9FactoryDebugLog("CheckDepthStencilMatch -> hr=0x%08x", (unsigned)S_OK);
        return S_OK; /* all depth-stencil combinations accepted */
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT, D3DDEVTYPE,
                                                           D3DFORMAT,
                                                           D3DFORMAT) noexcept override {
        dxmt9FactoryDebugLog("CheckDeviceFormatConversion -> hr=0x%08x",
                             (unsigned)D3DERR_NOTAVAILABLE);
        return D3DERR_NOTAVAILABLE;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE,
                                             D3DCAPS9* pCaps) noexcept override {
        if (!pCaps) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("GetDeviceCaps adapter=%u", adapter);
        D9CCaps cc{};
        HRESULT hr = hr32(dxmt9c_factory_get_caps(f_, adapter, &cc));
        if (SUCCEEDED(hr)) {
            fillD3DCaps9(cc, pCaps);
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

    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE,
                                            HWND hwnd, DWORD behaviorFlags,
                                            D3DPRESENT_PARAMETERS* pPP,
                                            IDirect3DDevice9** ppDevice) noexcept override {
        if (!pPP || !ppDevice) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("CreateDevice adapter=%u hwnd=%p behavior=0x%x windowed=%u size=%ux%u fmt=%u",
                             adapter, hwnd, (unsigned)behaviorFlags,
                             pPP->Windowed ? 1u : 0u,
                             (unsigned)pPP->BackBufferWidth, (unsigned)pPP->BackBufferHeight,
                             (unsigned)pPP->BackBufferFormat);
        D9CPresentParams cpp = toCpp(*pPP);
        cpp.deviceWindow = (uint64_t)(uintptr_t)hwnd;
        D9CDevice* dev = dxmt9c_factory_create_device(f_, adapter, &cpp,
                                                       behaviorFlags, nullptr);
        if (!dev) {
            dxmt9FactoryDebugLog("CreateDevice -> failed");
            return D3DERR_INVALIDCALL;
        }
        *ppDevice = CreateDeviceImpl(dev, this, adapter, behaviorFlags, hwnd, extended_);
        dxmt9FactoryDebugLog("CreateDevice -> device=%p", *ppDevice);
        return S_OK;
    }

    /* ── IDirect3D9Ex ── */

    UINT STDMETHODCALLTYPE GetAdapterModeCountEx(UINT adapter,
                                                  const D3DDISPLAYMODEFILTER*) noexcept override {
        dxmt9FactoryDebugLog("GetAdapterModeCountEx adapter=%u", adapter);
        return dxmt9c_factory_get_adapter_mode_count(f_, adapter,
                                                      (uint32_t)D3DFMT_X8R8G8B8);
    }

    HRESULT STDMETHODCALLTYPE EnumAdapterModesEx(UINT adapter,
                                                  const D3DDISPLAYMODEFILTER* pFilter,
                                                  UINT mode,
                                                  D3DDISPLAYMODEEX* pMode) noexcept override {
        if (!pMode) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("EnumAdapterModesEx adapter=%u mode=%u", adapter, mode);
        uint32_t fmt = pFilter ? (uint32_t)pFilter->Format : (uint32_t)D3DFMT_X8R8G8B8;
        uint32_t w, h, refresh, f;
        HRESULT hr = hr32(dxmt9c_factory_enum_adapter_modes(
            f_, adapter, fmt, mode, &w, &h, &refresh, &f));
        if (FAILED(hr)) return hr;
        pMode->Size            = sizeof(D3DDISPLAYMODEEX);
        pMode->Width           = w;
        pMode->Height          = h;
        pMode->RefreshRate     = refresh;
        pMode->Format          = (D3DFORMAT)f;
        pMode->ScanLineOrdering= D3DSCANLINEORDERING_PROGRESSIVE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAdapterDisplayModeEx(UINT adapter,
                                                       D3DDISPLAYMODEEX* pMode,
                                                       D3DDISPLAYROTATION* pRot) noexcept override {
        uint32_t w, h, refresh, f;
        dxmt9FactoryDebugLog("GetAdapterDisplayModeEx adapter=%u", adapter);
        HRESULT hr = hr32(dxmt9c_factory_get_adapter_display_mode(
            f_, adapter, &w, &h, &refresh, &f));
        if (FAILED(hr)) return hr;
        if (pMode) {
            pMode->Size             = sizeof(D3DDISPLAYMODEEX);
            pMode->Width            = w;
            pMode->Height           = h;
            pMode->RefreshRate      = refresh;
            pMode->Format           = (D3DFORMAT)f;
            pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
            dxmt9FactoryDebugLog("GetAdapterDisplayModeEx -> %ux%u refresh=%u fmt=%u",
                                 (unsigned)pMode->Width, (unsigned)pMode->Height,
                                 (unsigned)pMode->RefreshRate, (unsigned)pMode->Format);
        }
        if (pRot) *pRot = D3DDISPLAYROTATION_IDENTITY;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateDeviceEx(UINT adapter, D3DDEVTYPE,
                                              HWND hwnd, DWORD behaviorFlags,
                                              D3DPRESENT_PARAMETERS* pPP,
                                              D3DDISPLAYMODEEX* pFsMode,
                                              IDirect3DDevice9Ex** ppDevice) noexcept override {
        if (!pPP || !ppDevice) return D3DERR_INVALIDCALL;
        dxmt9FactoryDebugLog("CreateDeviceEx adapter=%u hwnd=%p behavior=0x%x windowed=%u size=%ux%u fmt=%u fsMode=%d",
                             adapter, hwnd, (unsigned)behaviorFlags,
                             pPP->Windowed ? 1u : 0u,
                             (unsigned)pPP->BackBufferWidth, (unsigned)pPP->BackBufferHeight,
                             (unsigned)pPP->BackBufferFormat, pFsMode ? 1 : 0);
        D9CPresentParams cpp = toCpp(*pPP);
        cpp.deviceWindow = (uint64_t)(uintptr_t)hwnd;
        D9CDisplayModeEx cdme{};
        if (pFsMode) cdme = toCdme(*pFsMode);
        D9CDevice* dev = dxmt9c_factory_create_device(f_, adapter, &cpp,
                                                       behaviorFlags,
                                                       pFsMode ? &cdme : nullptr);
        if (!dev) {
            dxmt9FactoryDebugLog("CreateDeviceEx -> failed");
            return D3DERR_INVALIDCALL;
        }
        *ppDevice = CreateDeviceImpl(dev, this, adapter, behaviorFlags, hwnd, extended_);
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
