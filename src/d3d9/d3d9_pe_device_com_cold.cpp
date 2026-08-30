/* src/d3d9/d3d9_pe_device_com_cold.cpp — D3D9DeviceImpl cold COM surface.
 *
 * The IDirect3DDevice9Ex entry points an app calls at load, on a device
 * reset, or once per resource — not per draw and not per frame: every
 * Get* accessor, every Create* factory, the resource copy/fill operations
 * (UpdateSurface, UpdateTexture, StretchRect, ColorFill, ProcessVertices),
 * Reset/ResetEx and the device-lost queries, cursor/gamma/palette, the caps
 * and display-mode queries, and the state-block plumbing.
 *
 * All 96 are virtual overrides. Out-lining them here is only safe because
 * d3d9_pe_device.cpp pins an explicit key function (out-of-line
 * QueryInterface, the first non-pure non-inline virtual in declaration order); without it the
 * first cold TU to out-line a virtual claims the vtable and the whole COM
 * surface with it. See the QueryInterface key-function comment in the class
 * header and agents/rules/codebase_conventions.rules.md.
 *
 * The two validator regions below are reached only from these definitions, so
 * they move out of the class header and become this TU's anonymous namespace.
 * The wrapper is what gives them internal linkage; the file bodies are
 * unchanged. */

#include "d3d9_pe_device_impl.hpp"
#include "d3d9_pe_shader_bytecode_scan.hpp"
#include "d3d9_pe_stateblock_fault.hpp"

#include <new>

namespace {

#include "d3d9_pe_device_com_cold_helpers.inc.hpp"

#include "d3d9_pe_device_com_shader_validators.inc.hpp"

}  // namespace

namespace {
inline uint32_t userMemoryBytesPerPixel(D3DFORMAT fmt) {
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
        case D3DFMT_A8P8:
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
}  // namespace

D3D9DeviceImpl::D3D9DeviceImpl(D9CDevice* dev, IDirect3D9Ex* factory,
               UINT adapter, D3DDEVTYPE deviceType, DWORD behaviorFlags,
               HWND window, bool extended,
               DWORD implicitSwapchainFlags)
    : dev_(dev), factory_(factory)
    , adapter_(adapter), deviceType_(deviceType), behaviorFlags_(behaviorFlags)
    , recorderState_(dxmt9PeRecorderLockRequired(
          behaviorFlags, dxmt9PeForceRecorderLockEnabled()))
    // recorderState_.recorderOwnership binds to this thread through its default member
    // initializer, so it needs no entry here.
    , softwareVertexProcessing_((behaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) ? TRUE : FALSE)
    , extended_(extended)
    , peCaptureState_(makePeCaptureState(
          dxmt9PeRenderTapeCaptureEnabled(),
          dxmt9PeRenderTapeCaptureLimits(
              dxmt9PeRenderTapeCaptureEnabled()),
          dxmt9PeRenderTapeCaptureProfile(),
          dxmt9PeRenderTapeCaptureSkipPresents()))
    , diagnostics_(makePeDiagnosticsState(
          this, dxmt9PeResolvedDiagnosticsConfig()))
    , creationWindow_(window)
    , implicitSwapchainFlagsShadow_(implicitSwapchainFlags) {
    stateBlockContext_.device = this;
    bufferContext_.device = this;
    surfaceTextureContext_.device = this;
    queryContext_.device = this;
    presentationContext_.device = this;
    shaderDeclarationContext_.device = this;
    for (UINT& freq : streamFreq_) {
        freq = 1;
    }
    if (dev_) {
        D9CCommandChunkNegotiation negotiation{};
        negotiation.peSupportedVersions = D9C_COMMAND_CHUNK_CAP_CURRENT;
        negotiation.pePreferredVersion = D9C_COMMAND_CHUNK_VERSION;
        const HRESULT negotiationHr =
            hr32(dxmt9c_device_negotiate_command_chunk(
                dev_, &negotiation));
        recorderState_.commandChunkNegotiated = SUCCEEDED(negotiationHr) &&
            negotiation.selectedVersion == D9C_COMMAND_CHUNK_VERSION;
        if (recorderState_.commandChunkNegotiated) {
            dxmt9DeviceInfoLog(
                "command chunk negotiation selected canonical pe_caps=0x%x unix_caps=0x%x",
                negotiation.peSupportedVersions,
                negotiation.unixSupportedVersions);
        } else {
            dxmt9DeviceInfoLog(
                "command chunk negotiation failed hr=0x%08x preferred=canonical selected=v%u unix_caps=0x%x",
                static_cast<unsigned>(negotiationHr),
                negotiation.selectedVersion,
                negotiation.unixSupportedVersions);
        }
    }
    // T2: Initialize viewport/scissor PE shadow from the implicit
    // swapchain's back-buffer rect so GetViewport/GetScissorRect
    // round-trip correctly before any Set call (Wine conformance:
    // test_viewport_scissor_state_getters). Mirrors the Reset()
    // / ResetEx() block at lines ~1601 / ~3154.
    if (dev_) {
        D9CSwapChain* chain = dxmt9c_device_get_swap_chain(dev_, 0);
        if (chain) {
            D9CPresentParams cpp{};
            if (SUCCEEDED(hr32(dxmt9c_swapchain_get_present_params(chain, &cpp)))) {
                const uint32_t w = std::max<uint32_t>(1u, cpp.backBufferWidth);
                const uint32_t h = std::max<uint32_t>(1u, cpp.backBufferHeight);
                recorderState_.peState.maintenance().viewportShadow() = D9CViewport{0, 0, w, h, 0.0f, 1.0f};
                recorderState_.peState.maintenance().scissorShadow()  = D9CRect{0, 0, (int32_t)w, (int32_t)h};
            }
            dxmt9c_swapchain_release(chain);
        }
    }
    initGammaRampIdentity();
    // DXMT9_PE_MODULE_MAP: dump the loaded-module map after all modules
    // (game exe, our PE d3d9.dll/winemetal_dxmt9.dll, Wine DLLs) are loaded.
    // Device creation happens well after process/module init, so this is
    // a safe, one-time-per-device site for the Tier 1 PE symbolication
    // diagnostic.
    dxmt9PeDumpModuleMap();
    // DXMT9_PE_THREAD_SAMPLER: same site, same reasoning — every module is
    // loaded by now, and the creating thread is the game thread.
    startPeThreadSamplerIfRequested();
    dxmt9DeviceDebugLog("device_ctor this=%p dev=%p factory=%p adapter=%u devType=%u behavior=0x%x window=%p extended=%u",
                        this, static_cast<void*>(dev_), static_cast<void*>(factory_),
                        adapter_, (unsigned)deviceType_, (unsigned)behaviorFlags_, window, extended_ ? 1u : 0u);
    // Acquire the raw COM member only after all constructor work that can
    // fail. A throwing constructor does not run ~D3D9DeviceImpl(), so taking
    // this reference at entry would leak it on an allocation failure.
    if (factory_) factory_->AddRef();
}

D3D9DeviceImpl::~D3D9DeviceImpl() {
    (void)flushPeRecorder(
        PeRecorderFlushReason::Destructor,
        PeRecorderFlushDisposition::DiscardForRecovery);
    dxmt9DeviceInfoLog(
        "command chunk totals selected=canonical chunks=%llu records=%llu bytes=%llu identity_getter_calls=%llu",
        static_cast<unsigned long long>(recorderState_.commandChunkCommits),
        static_cast<unsigned long long>(recorderState_.commandChunkRecords),
        static_cast<unsigned long long>(recorderState_.commandChunkBytes),
        static_cast<unsigned long long>(
            dxmt9::d3d9::pe::wireIdentityGetterCallCount()));
    logVsConstSetterRangePerf("destructor");
    logPeRecorderStats("destructor", true);
    logPeStatsDecimation();
    if (diagnostics_ && diagnostics_->peCopyMaterializationReportPresents_ != 0u &&
        diagnostics_->peCopyMaterializationReportPresents_ % 60u != 0u) {
        logPeCopyMaterializationLedger();
    }
    // Emit the last partial interval before the sampler stops, then stop
    // it: the sampler thread must not outlive the state it reads.
    logPeThreadSampler();
    stopPeThreadSampler();
    recorderState_.stateBlockTransaction.discardAll(
        d3d9PeReleaseStateBlockRef);
    dxmt9PeFinalizeDeviceAndReleaseWsiBinding(
        dev_, implicitWsiBinding_);
    releaseAllBound();
    dxmt9c_device_release(dev_);
    if (factory_) factory_->Release();
}

HRESULT D3D9DeviceImpl::CaptureStateBlockShadowForChild(
    D3D9StateBlockShadow& out,
    StateBlockCaptureDisposition disposition) noexcept {
  try {
    auto outWriter = out.writer();
    const auto outSnapshot = out.snapshot();
    const bool initialSnapshot = !outSnapshot.initialized();
    const StateBlockCaptureDisposition effectiveDisposition =
        recorderState_.stateBlockTransaction.isInsideEnd()
            ? StateBlockCaptureDisposition::Explicit
            : disposition;
    if (initialSnapshot) {
        outWriter.renderStates().clear();
        outWriter.textureStageStates().clear();
        outWriter.samplerStates().clear();
        outWriter.transforms().clear();
        const auto renderSource = recorderState_.stateBlockTransaction.isInsideEnd()
            ? recorderState_.stateBlockTransaction.recordedSnapshot().renderStates()
            : std::as_const(recorderState_.peState).renderStateShadowTyped();
        renderSource.forEach([&](RenderStateSlot state, std::uint32_t value) {
            if (stateBlockRenderStateSelected(effectiveDisposition,
                                               rawSlot(state))) {
                outWriter.renderStates().set(state, value);
            }
        });
        const auto tssSource = recorderState_.stateBlockTransaction.isInsideEnd()
            ? recorderState_.stateBlockTransaction.recordedSnapshot().textureStageStates()
            : std::as_const(recorderState_.peState).tssShadowTyped();
        tssSource.forEach(
            [&](TextureStageIndex stage, TextureStageStateType type,
                std::uint32_t value) {
                if (stateBlockTextureStageStateSelected(
                        effectiveDisposition, rawSlot(type))) {
                    outWriter.textureStageStates().set(stage, type, value);
                }
            });
        const auto samplerSource = recorderState_.stateBlockTransaction.isInsideEnd()
            ? recorderState_.stateBlockTransaction.recordedSnapshot().samplerStates()
            : std::as_const(recorderState_.peState).samplerStateShadowTyped();
        samplerSource.forEach(
            [&](SamplerIndex sampler, SamplerStateType type,
                std::uint32_t value) {
                if (stateBlockSamplerStateSelected(effectiveDisposition,
                                                   rawSlot(type))) {
                    outWriter.samplerStates().set(sampler, type, value);
                }
            });
        const auto transformSource = recorderState_.stateBlockTransaction.isInsideEnd()
            ? recorderState_.stateBlockTransaction.recordedSnapshot().transforms()
            : std::as_const(recorderState_.peState).transformShadowTyped();
        if (stateBlockCaptureCategorySelected(
                effectiveDisposition, StateBlockCaptureCategory::Transform)) {
            transformSource.forEach(
                [&](TransformState state, const D9CMatrix& value) {
                    outWriter.transforms().set(state, value);
                });
        }

        StateBlockRecorded categorySource{};
        if (recorderState_.stateBlockTransaction.isInsideEnd()) {
            categorySource = recorderState_.stateBlockTransaction.recordedSnapshot();
        } else {
            auto categoryWriter = categorySource.writer();
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition, StateBlockCaptureCategory::Texture)) {
                for (std::uint32_t slot = 0; slot < kPeTextureSlots; ++slot)
                    categoryWriter.textures().set(
                        stateBlockFixedSlotKey<
                            StateBlockApplyPhysicalStore::textures>(slot),
                        stateBlockTextureRef(textures_[slot]));
            }
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition,
                    StateBlockCaptureCategory::StreamSource)) {
                for (std::uint32_t slot = 0;
                     slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot) {
                    categoryWriter.streamSources().set(
                        stateBlockFixedSlotKey<
                            StateBlockApplyPhysicalStore::streamSources>(slot),
                        StateBlockStreamSourceValue{
                                  .buffer = stateBlockBufferRef(streamSrc_[slot]),
                                  .offset = streamOff_[slot],
                                  .stride = streamStr_[slot],
                              });
                }
            }
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition,
                    StateBlockCaptureCategory::StreamFrequency)) {
                for (std::uint32_t slot = 0;
                     slot < D9C_DRAW_PACKET_MAX_STREAMS; ++slot)
                    categoryWriter.streamFrequencies().set(
                        stateBlockFixedSlotKey<
                            StateBlockApplyPhysicalStore::streamFrequencies>(slot),
                        streamFreq_[slot]);
            }
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition, StateBlockCaptureCategory::VertexShader))
                categoryWriter.vertexShader().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::vertexShader>(0u),
                    stateBlockVertexShaderRef(vs_));
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition, StateBlockCaptureCategory::PixelShader))
                categoryWriter.pixelShader().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::pixelShader>(0u),
                    stateBlockPixelShaderRef(ps_));
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition, StateBlockCaptureCategory::Fvf))
                categoryWriter.fvf().set(
                    stateBlockFixedSlotKey<StateBlockApplyPhysicalStore::fvf>(0u),
                    fvf_);
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition,
                    StateBlockCaptureCategory::VertexDeclaration))
                categoryWriter.vertexDeclaration().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::vertexDeclaration>(0u),
                    stateBlockVertexDeclarationRef(vdecl_));
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition, StateBlockCaptureCategory::IndexBuffer))
                categoryWriter.indexBuffer().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::indexBuffer>(0u),
                    stateBlockIndexBufferRef(indexBuf_));
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition, StateBlockCaptureCategory::RenderTarget)) {
                for (std::uint32_t slot = 0;
                     slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot)
                    categoryWriter.renderTargets().set(
                        stateBlockFixedSlotKey<
                            StateBlockApplyPhysicalStore::renderTargets>(slot),
                        stateBlockSurfaceRef(rtSlots_[slot]));
            }
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition, StateBlockCaptureCategory::DepthStencil))
                categoryWriter.depthStencil().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::depthStencil>(0u),
                    stateBlockDepthStencilRef(dsSurface_));
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition, StateBlockCaptureCategory::Viewport))
                categoryWriter.viewport().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::viewport>(0u),
                    recorderState_.peState.viewportShadow());
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition, StateBlockCaptureCategory::Scissor))
                categoryWriter.scissor().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::scissor>(0u),
                    recorderState_.peState.scissorShadow());
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition, StateBlockCaptureCategory::Material))
                categoryWriter.material().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::material>(0u),
                    recorderState_.peState.materialShadow());
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition, StateBlockCaptureCategory::ClipPlane)) {
                for (std::uint32_t idx = 0; idx < 6u; ++idx) {
                    std::array<float, 4> plane{};
                    std::memcpy(plane.data(), recorderState_.peState.clipPlaneShadow() + idx * 4u,
                                sizeof(plane));
                    categoryWriter.clipPlanes().set(
                        stateBlockFixedSlotKey<
                            StateBlockApplyPhysicalStore::clipPlanes>(idx),
                        plane);
                }
            }
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition, StateBlockCaptureCategory::Light)) {
                for (std::uint32_t idx = 0;
                     idx < D9C_DRAW_PACKET_MAX_LIGHTS; ++idx)
                    categoryWriter.lights().set(
                        stateBlockFixedSlotKey<
                            StateBlockApplyPhysicalStore::lights>(idx),
                        recorderState_.peState.lightShadow()[idx]);
            }
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition, StateBlockCaptureCategory::LightEnable)) {
                for (std::uint32_t idx = 0;
                     idx < D9C_DRAW_PACKET_MAX_LIGHTS; ++idx)
                    categoryWriter.lightEnables().set(
                        stateBlockFixedSlotKey<
                            StateBlockApplyPhysicalStore::lightEnables>(idx),
                        (recorderState_.peState.lightEnableShadow() & (1u << idx)) != 0u);
            }
        }
        out.copyCategoriesFrom(categorySource);

        outWriter.constants().clearForBegin();
        if (recorderState_.stateBlockTransaction.isInsideEnd()) {
            outWriter.constants() = recorderState_.stateBlockTransaction.recordedSnapshot().constantSnapshot();
        } else {
            const auto copyLive = [](const ConstShadow& live,
                                     StateBlockConstShadow& destination,
                                     std::size_t elemSize) {
                const auto count = static_cast<std::uint32_t>(
                    live.values.size() / elemSize);
                if (count != 0u) {
                    destination.record(0u, count, live.values.data(), elemSize);
                }
            };
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition,
                    StateBlockCaptureCategory::VertexConstants)) {
                copyLive(recorderState_.peConsts.vsConstF, outWriter.constants().vsConstF,
                         sizeof(float) * 4u);
                copyLive(recorderState_.peConsts.vsConstI, outWriter.constants().vsConstI,
                         sizeof(std::int32_t) * 4u);
                copyLive(recorderState_.peConsts.vsConstB, outWriter.constants().vsConstB,
                         sizeof(std::uint32_t));
            }
            if (stateBlockCaptureCategorySelected(
                    effectiveDisposition,
                    StateBlockCaptureCategory::PixelConstants)) {
                copyLive(recorderState_.peConsts.psConstF, outWriter.constants().psConstF,
                         sizeof(float) * 4u);
                copyLive(recorderState_.peConsts.psConstI, outWriter.constants().psConstI,
                         sizeof(std::int32_t) * 4u);
                copyLive(recorderState_.peConsts.psConstB, outWriter.constants().psConstB,
                         sizeof(std::uint32_t));
            }
        }
        outWriter.categories().constants() = outWriter.constants();
    } else {
        StateBlockRecorded refreshed = outSnapshot.categories();
        auto refreshedWriter = refreshed.writer();
        outWriter.renderStates().forEach(
            [&](RenderStateSlot state, std::uint32_t /*prior*/) {
                std::uint32_t latest = 0u;
                if (!recorderState_.peState.renderStateShadowTyped().get(state, latest)) {
                    latest = dxmt9c_device_get_render_state(dev_, rawSlot(state));
                }
                outWriter.renderStates().set(state, latest);
            });
        outWriter.textureStageStates().forEach(
            [&](TextureStageIndex stage, TextureStageStateType type,
                std::uint32_t /*prior*/) {
                std::uint32_t latest = 0u;
                if (!recorderState_.peState.tssShadowTyped().get(stage, type, latest)) {
                    latest = dxmt9c_device_get_texture_stage_state(
                        dev_, rawSlot(stage), rawSlot(type));
                }
                outWriter.textureStageStates().set(stage, type, latest);
            });
        outWriter.samplerStates().forEach(
            [&](SamplerIndex sampler, SamplerStateType type,
                std::uint32_t /*prior*/) {
                std::uint32_t latest = 0u;
                if (!recorderState_.peState.samplerStateShadowTyped().get(
                        sampler, type, latest)) {
                    latest = dxmt9c_device_get_sampler_state(
                        dev_, rawSlot(sampler), rawSlot(type));
                }
                outWriter.samplerStates().set(sampler, type, latest);
            });
        outWriter.transforms().forEach(
            [&](TransformState state, const D9CMatrix& /*prior*/) {
            D9CMatrix latest{};
            if (!recorderState_.peState.transformShadowTyped().get(state, latest)) {
                latest = identityTransformMatrix();
                (void)dxmt9c_device_get_transform(dev_, rawSlot(state), &latest);
            }
            outWriter.transforms().set(state, latest);
        });
        outWriter.constants().vsConstF.refreshFrom(recorderState_.peConsts.vsConstF,
                                           sizeof(float) * 4u);
        outWriter.constants().vsConstI.refreshFrom(recorderState_.peConsts.vsConstI,
                                           sizeof(std::int32_t) * 4u);
        outWriter.constants().vsConstB.refreshFrom(recorderState_.peConsts.vsConstB,
                                           sizeof(std::uint32_t));
        outWriter.constants().psConstF.refreshFrom(recorderState_.peConsts.psConstF,
                                           sizeof(float) * 4u);
        outWriter.constants().psConstI.refreshFrom(recorderState_.peConsts.psConstI,
                                           sizeof(std::int32_t) * 4u);
        outWriter.constants().psConstB.refreshFrom(recorderState_.peConsts.psConstB,
                                           sizeof(std::uint32_t));
        refreshed.textures().forEach(
            [&](std::size_t slot, const StateBlockTextureRef& /*prior*/) {
                refreshedWriter.textures().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::textures>(slot),
                    stateBlockTextureRef(textures_[slot]));
            });
        refreshed.streamSources().forEach(
            [&](std::size_t slot, const StateBlockStreamSourceValue& prior) {
                StateBlockStreamSourceValue latest = prior;
                latest.buffer = stateBlockBufferRef(streamSrc_[slot]);
                latest.offset = streamOff_[slot];
                latest.stride = streamStr_[slot];
                refreshedWriter.streamSources().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::streamSources>(slot),
                    latest);
            });
        refreshed.streamFrequencies().forEach(
            [&](std::size_t slot, std::uint32_t) {
                refreshedWriter.streamFrequencies().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::streamFrequencies>(slot),
                    streamFreq_[slot]);
            });
        if (refreshed.vertexShader().contains(0u))
            refreshedWriter.vertexShader().set(
                stateBlockFixedSlotKey<
                    StateBlockApplyPhysicalStore::vertexShader>(0u),
                stateBlockVertexShaderRef(vs_));
        if (refreshed.pixelShader().contains(0u))
            refreshedWriter.pixelShader().set(
                stateBlockFixedSlotKey<
                    StateBlockApplyPhysicalStore::pixelShader>(0u),
                stateBlockPixelShaderRef(ps_));
        if (refreshed.fvf().contains(0u))
            refreshedWriter.fvf().set(
                stateBlockFixedSlotKey<StateBlockApplyPhysicalStore::fvf>(0u),
                fvf_);
        if (refreshed.vertexDeclaration().contains(0u))
            refreshedWriter.vertexDeclaration().set(
                stateBlockFixedSlotKey<
                    StateBlockApplyPhysicalStore::vertexDeclaration>(0u),
                stateBlockVertexDeclarationRef(vdecl_));
        if (refreshed.indexBuffer().contains(0u))
            refreshedWriter.indexBuffer().set(
                stateBlockFixedSlotKey<
                    StateBlockApplyPhysicalStore::indexBuffer>(0u),
                stateBlockIndexBufferRef(indexBuf_));
        refreshed.renderTargets().forEach(
            [&](std::size_t slot, const StateBlockRenderTargetRef& /*prior*/) {
                refreshedWriter.renderTargets().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::renderTargets>(slot),
                    stateBlockSurfaceRef(rtSlots_[slot]));
            });
        if (refreshed.depthStencil().contains(0u))
            refreshedWriter.depthStencil().set(
                stateBlockFixedSlotKey<
                    StateBlockApplyPhysicalStore::depthStencil>(0u),
                stateBlockDepthStencilRef(dsSurface_));
        if (refreshed.viewport().contains(0u))
            refreshedWriter.viewport().set(
                stateBlockFixedSlotKey<
                    StateBlockApplyPhysicalStore::viewport>(0u),
                recorderState_.peState.viewportShadow());
        if (refreshed.scissor().contains(0u))
            refreshedWriter.scissor().set(
                stateBlockFixedSlotKey<
                    StateBlockApplyPhysicalStore::scissor>(0u),
                recorderState_.peState.scissorShadow());
        if (refreshed.material().contains(0u))
            refreshedWriter.material().set(
                stateBlockFixedSlotKey<
                    StateBlockApplyPhysicalStore::material>(0u),
                recorderState_.peState.materialShadow());
        refreshed.clipPlanes().forEach(
            [&](std::size_t idx, const std::array<float, 4>&) {
                std::array<float, 4> plane{};
                std::memcpy(plane.data(), recorderState_.peState.clipPlaneShadow() + idx * 4u,
                            sizeof(plane));
                refreshedWriter.clipPlanes().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::clipPlanes>(idx), plane);
            });
        refreshed.lights().forEach(
            [&](std::size_t idx, const D9CLight&) {
                refreshedWriter.lights().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::lights>(idx),
                    recorderState_.peState.lightShadow()[idx]);
            });
        refreshed.lightEnables().forEach(
            [&](std::size_t idx, std::uint32_t) {
                refreshedWriter.lightEnables().set(
                    stateBlockFixedSlotKey<
                        StateBlockApplyPhysicalStore::lightEnables>(idx),
                    (recorderState_.peState.lightEnableShadow() & (1u << idx)) != 0u);
            });
        refreshedWriter.constants() = outWriter.constants();
        out.copyCategoriesFrom(refreshed);
    }
    // vdecl tracking:
    //   - Initial snapshot, Begin/End path → track only if
    //     SetVertexDeclaration was called during recording.
    //   - Initial snapshot, CreateStateBlock path → always track.
    //   - Mid-game refresh → preserve the initial-time decision
    //     (the snapshot capability preserves the ctor-time decision).
    bool shouldTrackVdecl;
    if (initialSnapshot) {
        shouldTrackVdecl =
            recorderState_.stateBlockTransaction.isInsideEnd()
                ? recorderState_.stateBlockTransaction.recordedSnapshot().vertexDeclarationWasRecorded()
                : stateBlockCaptureCategorySelected(
                      effectiveDisposition,
                      StateBlockCaptureCategory::VertexDeclaration);
    } else {
        shouldTrackVdecl = outSnapshot.hasVdecl();
    }
    IDirect3DVertexDeclaration9* sourceVdecl = vdecl_;
    if (initialSnapshot && recorderState_.stateBlockTransaction.isInsideEnd()) {
        StateBlockVertexDeclarationRef candidateVdecl{};
        if (recorderState_.stateBlockTransaction.recordedSnapshot().vertexDeclaration().get(0u, candidateVdecl)) {
            sourceVdecl = reinterpret_cast<IDirect3DVertexDeclaration9*>(candidateVdecl.raw());
        }
    }
    outWriter.replaceVdecl(shouldTrackVdecl, sourceVdecl);
    outWriter.setInitialized(true);
    return S_OK;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  }
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::TestCooperativeLevel() noexcept {
    dxmt9DeviceDebugLog("device_test_cooperative_level device=%p", this);
    if (deviceNotReset_) {
        dxmt9DeviceDebugLog("device_test_cooperative_level -> device not reset");
        return D3DERR_DEVICENOTRESET;
    }
    const HRESULT hr = hr32(dxmt9c_device_test_cooperative_level(dev_));
    dxmt9DeviceDebugLog("device_test_cooperative_level -> hr=0x%08x", (unsigned)hr);
    return hr;
}

UINT STDMETHODCALLTYPE D3D9DeviceImpl::GetAvailableTextureMem() noexcept {
    dxmt9DeviceDebugLog("device_get_available_texture_mem device=%p", this);
    // Wine base_vidmem_accounting_policy: report a pseudo-budget that
    // decreases with each large allocation. The actual GPU has its own
    // budget machinery; this PE-side accounting only needs to expose
    // the strictly-decreasing property the conformance test asserts.
    constexpr uint64_t kBudget = 0x80000000ull;  // 2 GiB sentinel
    const uint64_t used = vidmemBytesUsedShadow_;
    const uint64_t remaining = used >= kBudget ? 0ull : (kBudget - used);
    const UINT value = static_cast<UINT>(remaining);
    dxmt9DeviceDebugLog("device_get_available_texture_mem -> %u (0x%x) used=%llu",
                        value, (unsigned)value, (unsigned long long)used);
    return value;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::EvictManagedResources() noexcept {
    // stub: Wine returns S_OK; Apple GPUs have unified memory, manual eviction is not exposed.
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetDirect3D(IDirect3D9** ppD3D) noexcept {
    if (!ppD3D) return D3DERR_INVALIDCALL;
    factory_->AddRef();
    *ppD3D = static_cast<IDirect3D9*>(factory_);
    dxmt9DeviceDebugLog("device_get_direct3d this=%p -> factory=%p", this, static_cast<void*>(*ppD3D));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetDeviceCaps(D3DCAPS9* pCaps) noexcept {
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

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetDisplayMode(UINT sc, D3DDISPLAYMODE* pMode) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex,
                                 recorderState_.recorderLockRequired);
    if (!pMode) return D3DERR_INVALIDCALL;
    dxmt9DeviceDebugLog("device_get_display_mode device=%p sc=%u", this, sc);
    D9CSwapChain* chain = borrowSwapChainHandle(sc);
    if (!chain) {
        return D3DERR_INVALIDCALL;
    }
    D9CPresentParams cpp{};
    const HRESULT hr = hr32(dxmt9c_swapchain_get_present_params(chain, &cpp));
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

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetCreationParameters(
        D3DDEVICE_CREATION_PARAMETERS* pParams) noexcept {
    if (!pParams) return D3DERR_INVALIDCALL;
    pParams->AdapterOrdinal  = adapter_;
    pParams->DeviceType      = deviceType_;
    pParams->hFocusWindow    = creationWindow_;
    pParams->BehaviorFlags   = behaviorFlags_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* surface) noexcept {
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

void    STDMETHODCALLTYPE D3D9DeviceImpl::SetCursorPosition(int x, int y, DWORD flags) noexcept {
    // stub: Wine returns S_OK; cursor positioning belongs to the WindowServer / window manager,
    // the app's hint is informational.
    dxmt9DeviceDebugLog("device_set_cursor_position device=%p x=%d y=%d flags=0x%x",
                        this, x, y, (unsigned)flags);
}

BOOL    STDMETHODCALLTYPE D3D9DeviceImpl::ShowCursor(BOOL show) noexcept {
    dxmt9DeviceDebugLog("device_show_cursor device=%p show=%u", this, (unsigned)show);
    if (!cursorSurfaceSet_) {
        return FALSE;
    }
    const BOOL previous = cursorVisible_ ? TRUE : FALSE;
    cursorVisible_ = show ? true : false;
    return previous;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateAdditionalSwapChain(
        D3DPRESENT_PARAMETERS* pPP, IDirect3DSwapChain9** ppSC) noexcept {
    if (!pPP || !ppSC) return D3DERR_INVALIDCALL;
    *ppSC = nullptr;
    // Present-parameter validation (same rule as CreateDevice / Reset):
    // invalid swap effect, BackBufferCount over the cap, COPY with > 1
    // back buffer, and undocumented presentation intervals are rejected
    // with D3DERR_INVALIDCALL before any swap chain is created.
    if (const HRESULT vhr = pePresentParamsHResult(pPP->SwapEffect,
            pPP->BackBufferCount, pPP->PresentationInterval,
            pPP->MultiSampleType, pPP->MultiSampleQuality, extended_);
        FAILED(vhr)) {
        return vhr;
    }
    D9CPresentParams cpp{};
    // minimal fill
    cpp.backBufferWidth  = pPP->BackBufferWidth;
    cpp.backBufferHeight = pPP->BackBufferHeight;
    cpp.backBufferFormat = (uint32_t)pPP->BackBufferFormat;
    cpp.backBufferCount  = pPP->BackBufferCount;
    cpp.swapEffect       = (uint32_t)pPP->SwapEffect;
    // Wine d3d9: when the caller leaves hDeviceWindow NULL the
    // swap-chain inherits the device's focus window (the one that
    // owns the device). IDirect3DSwapChain9::GetPresentParameters
    // then reports the resolved window — see
    // test_additional_swapchain_backbuffer_bounds line 475.
    HWND effectiveDeviceWindow = pPP->hDeviceWindow ? pPP->hDeviceWindow
                                                   : creationWindow_;
    cpp.deviceWindow     = (uint64_t)(uintptr_t)effectiveDeviceWindow;
    cpp.windowed         = pPP->Windowed ? 1u : 0u;
    cpp.presentationInterval = pPP->PresentationInterval;
    D3D9PeWsiBinding wsiBinding =
        dxmt9PeAcquireWsiBinding(effectiveDeviceWindow);
    if (!dxmt9::wsi::validAdoptionCandidate(wsiBinding)) {
        return D3DERR_NOTAVAILABLE;
    }
    D9CSwapChain* sc = dxmt9c_device_create_additional_swap_chain(dev_, &cpp);
    if (!sc) {
        dxmt9PeReleaseWsiBindingAfterQuiescence(wsiBinding);
        return D3DERR_INVALIDCALL;
    }
    if (FAILED(dxmt9PeAdoptWsiBinding(sc, wsiBinding))) {
        dxmt9PeReleaseWsiBindingAfterQuiescence(wsiBinding);
        dxmt9c_swapchain_release(sc);
        return D3DERR_NOTAVAILABLE;
    }
    *ppSC = CreatePeSwapChain(sc, this, presentationContext(), diagnosticObserverForChild(),
                              extended_, pPP->Flags,
                              std::move(wsiBinding));
    if (!*ppSC) {
        dxmt9c_swapchain_release(sc);
        return E_OUTOFMEMORY;
    }
    // Wine d3d9 post-create mutation of pPP (additional swapchain only):
    //   * BackBufferCount=0 normalises to 1 (clamped to a documented
    //     minimum) and is written back to the caller's struct.
    //   * hDeviceWindow is cleared on the additional swapchain's view
    //     of the present parameters — the swapchain's internal record
    //     still stores the device's focus window (which
    //     IDirect3DSwapChain9::GetPresentParameters reports), but the
    //     mutated pPP struct visible to the caller has hDeviceWindow
    //     zeroed.
    // Tests: test_additional_swapchain_backbuffer_bounds at
    // tests/conformance/d3d9/d3d9_conformance_swapchain.c:443-444.
    pPP->BackBufferCount = peNormalizeBackBufferCount(pPP->BackBufferCount);
    pPP->hDeviceWindow = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetSwapChain(UINT index,
                                                    IDirect3DSwapChain9** ppSC) noexcept {
    notePeDeviceCallAfterPresent("GetSwapChain");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex,
                                 recorderState_.recorderLockRequired);
    if (!ppSC) return D3DERR_INVALIDCALL;
    *ppSC = nullptr;
    if (auto it = swapchainWrappers_.find(index);
        it != swapchainWrappers_.end()) {
        it->second->AddRef();
        *ppSC = it->second;
        return S_OK;
    }
    D9CSwapChain* sc = dxmt9c_device_get_swap_chain(dev_, index);
    if (!sc) return D3DERR_INVALIDCALL;
    // index 0 = implicit swap chain; additional swap chains are
    // created with their own flags through CreateAdditionalSwapChain
    // and that path already sets the shadow. For lazy-created index-0
    // wrappers fall back to the device's captured implicit flags.
    const DWORD wrapperFlags = (index == 0) ? implicitSwapchainFlagsShadow_ : 0;
    auto* wrapper = CreatePeSwapChain(sc, this, presentationContext(),
                                      diagnosticObserverForChild(), extended_,
                                      wrapperFlags);
    if (!wrapper) {
        dxmt9c_swapchain_release(sc);
        return E_OUTOFMEMORY;
    }
    IDirect3DSwapChain9* canonical = nullptr;
    const auto insertStatus = D3D9PeCanonicalizeComCacheInsertion(
        swapchainWrappers_, index,
        static_cast<IDirect3DSwapChain9*>(wrapper), &canonical);
    if (insertStatus == D3D9PeComCacheInsertStatus::OutOfMemory) {
        return E_OUTOFMEMORY;
    }
    if (insertStatus == D3D9PeComCacheInsertStatus::Failed) {
        return D3DERR_INVALIDCALL;
    }
    *ppSC = canonical;
    return S_OK;
}

UINT STDMETHODCALLTYPE D3D9DeviceImpl::GetNumberOfSwapChains() noexcept {
    return dxmt9c_device_get_swap_chain_count(dev_);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::Reset(D3DPRESENT_PARAMETERS* pPP) noexcept {
    dxmt9PeSetCurrentCallName("Reset");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!pPP) return D3DERR_INVALIDCALL;
    // Present-parameter validation (same rule as CreateDevice): invalid
    // swap effect, BackBufferCount over the cap, COPY with > 1 back
    // buffer, and undocumented presentation intervals are rejected with
    // D3DERR_INVALIDCALL before any device state is torn down.
    if (const HRESULT vhr = pePresentParamsHResult(pPP->SwapEffect,
            pPP->BackBufferCount, pPP->PresentationInterval,
            pPP->MultiSampleType, pPP->MultiSampleQuality, extended_);
        FAILED(vhr)) {
        return vhr;
    }
    D9CPresentParams cpp{};
    cpp.backBufferWidth  = pPP->BackBufferWidth;
    cpp.backBufferHeight = pPP->BackBufferHeight;
    cpp.backBufferFormat = (uint32_t)pPP->BackBufferFormat;
    cpp.backBufferCount  = pPP->BackBufferCount;
    cpp.multiSampleType  = (uint32_t)pPP->MultiSampleType;
    cpp.multiSampleQuality = pPP->MultiSampleQuality;
    cpp.swapEffect       = (uint32_t)pPP->SwapEffect;
    HWND effectiveDeviceWindow = pPP->hDeviceWindow ? pPP->hDeviceWindow
                                                    : creationWindow_;
    cpp.deviceWindow     = (uint64_t)(uintptr_t)effectiveDeviceWindow;
    cpp.windowed         = pPP->Windowed ? 1u : 0u;
    cpp.enableAutoDepthStencil = pPP->EnableAutoDepthStencil ? 1u : 0u;
    cpp.autoDepthStencilFormat = (uint32_t)pPP->AutoDepthStencilFormat;
    cpp.flags            = pPP->Flags;
    cpp.fullScreenRefreshRateHz = pPP->FullScreen_RefreshRateInHz;
    cpp.presentationInterval = pPP->PresentationInterval;
    const HRESULT flushHr = flushPeRecorder(
        PeRecorderFlushReason::Reset,
        peRecorderResetDisposition(
            recorderState_.stateBlockTransaction.isPoisoned()));
    if (FAILED(flushHr)) return flushHr;
    releaseAllBound();
    if (defaultPoolResourceRefs_ != 0) {
        clearPeStateTracking();
        releaseRecordedStateBlockRefs();
        recorderState_.stateBlockTransaction.resetFailed();
        deviceNotReset_ = true;
        return D3DERR_INVALIDCALL;
    }
    D3D9PeWsiBinding candidate =
        dxmt9PeAcquireWsiBinding(effectiveDeviceWindow);
    if (!dxmt9::wsi::validAdoptionCandidate(candidate) ||
        FAILED(dxmt9PeAdoptDeviceWsiBinding(dev_, candidate))) {
        dxmt9PeReleaseWsiBindingAfterQuiescence(candidate);
        recorderState_.stateBlockTransaction.resetFailed();
        deviceNotReset_ = true;
        return D3DERR_NOTAVAILABLE;
    }
    D3D9PeWsiBinding oldBinding = std::move(implicitWsiBinding_);
    clearPeStateTracking();
    releaseRecordedStateBlockRefs();
    const HRESULT hr = hr32(dxmt9c_device_reset(dev_, &cpp));
    if (SUCCEEDED(hr)) {
        dxmt9PeReleaseWsiBindingAfterQuiescence(oldBinding);
        implicitWsiBinding_ = std::move(candidate);
        recorderState_.stateBlockTransaction.resetSucceeded(
            d3d9PeReleaseStateBlockRef);
        deviceNotReset_ = false;
        // reset_lockable_backbuffer_policy: capture the new
        // PresentParameters.Flags so future GetSwapChain wrapper
        // creations see the updated value.
        implicitSwapchainFlagsShadow_ = pPP->Flags;
        // T2: per Wine d3d9_device_Reset, viewport and scissor must
        // be set to {0, 0, BackBufferWidth, BackBufferHeight, 0, 1}
        // after a successful Reset. The core::Device already sets
        // its server-side viewport in resetValidated() — mirror it
        // into the PE shadow so the next draw packet carries fresh
        // viewport/scissor instead of stale pre-reset values.
        const uint32_t w = std::max<uint32_t>(1u, cpp.backBufferWidth);
        const uint32_t h = std::max<uint32_t>(1u, cpp.backBufferHeight);
        recorderState_.peState.maintenance().viewportShadow() = D9CViewport{0, 0, w, h, 0.0f, 1.0f};
        recorderState_.peState.maintenance().scissorShadow()  = D9CRect{0, 0, (int32_t)w, (int32_t)h};
        recorderState_.peState.maintenance().pendingViewport() = false;
        recorderState_.peState.maintenance().pendingScissor()  = false;
    } else {
        if (SUCCEEDED(dxmt9PeAdoptDeviceWsiBinding(dev_, oldBinding))) {
            dxmt9PeReleaseWsiBindingAfterQuiescence(candidate);
            implicitWsiBinding_ = std::move(oldBinding);
        } else {
            dxmt9PeFinalizeDeviceAndReleaseWsiBinding(dev_, candidate);
            dxmt9PeReleaseWsiBindingAfterQuiescence(oldBinding);
            implicitWsiBinding_ = {};
        }
        recorderState_.stateBlockTransaction.resetFailed();
    }
    if (peCaptureState_ &&
        peCaptureState_->renderTapeCapture.state() ==
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        const dxmt9::d3d9::RenderTapeResetControl payload{
            .reclaimedGeneration = 0u, .terminal = 1u};
        NotifyRenderTapeOrderedControlForChild(
            dxmt9::d3d9::RenderTapeOrderedControlHeader{
                .kind = static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeControlKind::Reset),
                .disposition = static_cast<std::uint32_t>(
                    SUCCEEDED(hr)
                        ? dxmt9::d3d9::RenderTapeControlDisposition::Terminal
                        : dxmt9::d3d9::RenderTapeControlDisposition::Failed),
                .resultCode = static_cast<std::int32_t>(hr),
                .controlBytes = sizeof(payload)},
            std::as_bytes(std::span(&payload, 1u)));
        abortRenderTapeCapture("reset");
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetBackBuffer(UINT sc, UINT idx,
                                                     D3DBACKBUFFER_TYPE type,
                                                     IDirect3DSurface9** ppS) noexcept {
    notePeDeviceCallAfterPresent("GetBackBuffer", DXMT9_PE_CALLSITE_PC());
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex,
                                 recorderState_.recorderLockRequired);
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    dxmt9DeviceDebugLog("device_get_back_buffer device=%p sc=%u idx=%u", this, sc, idx);
    D9CSwapChain* chain = borrowSwapChainHandle(sc);
    if (!chain) return D3DERR_INVALIDCALL;
    // Wine d3d9 test_swapchain_parameters: GetBackBuffer with an
    // index meeting or exceeding the swapchain's BackBufferCount
    // returns D3DERR_INVALIDCALL — *ppS must remain NULL on
    // failure, but cppcheck'd test asserts the pointer is left
    // untouched at the deadbeef sentinel only on the swapchain
    // path; the device path's spec resets it to NULL above and
    // expects NULL back.
    D9CPresentParams cppGuard{};
    if (SUCCEEDED(hr32(dxmt9c_swapchain_get_present_params(chain, &cppGuard)))) {
        if (idx >= cppGuard.backBufferCount) {
            return D3DERR_INVALIDCALL;
        }
    }
    // The former dxmt9c_swapchain_get_back_buffer + surface_release probe
    // that stood here is gone: it allocated and immediately dropped a unix
    // surface handle purely to map "no such back buffer" to
    // D3DERR_INVALIDCALL, which the swap-chain wrapper's own
    // GetBackBuffer below does with the identical null check and the
    // identical HRESULT, without disturbing *ppS (already NULL).
    // Wine d3d9 contract: device-level GetBackBuffer must return
    // the same COM wrapper as the matching swap-chain GetBackBuffer
    // for any (sc, idx). Route through the cached swap-chain
    // wrapper so its per-idx back-buffer cache is the single source
    // of truth. See test_swapchain_backbuffer_getter_policy lines
    // 371/379 — the assertion typed == backbuffer requires this
    // identity across the device-level and swap-chain-level calls.
    IDirect3DSwapChain9* swapchain = nullptr;
    const HRESULT swapHr = GetSwapChain(sc, &swapchain);
    if (FAILED(swapHr) || !swapchain) return swapHr;
    const HRESULT bbHr = swapchain->GetBackBuffer(idx, type, ppS);
    swapchain->Release();
    if (sc == 0 && idx == 0 && SUCCEEDED(bbHr) && *ppS) {
        // Preserve the pre-existing cachedBackBuffer0_ alias used by
        // the rest of the device implementation (e.g. resetState
        // explicit-clear paths) without holding an extra reference
        // beyond the swap-chain cache.
        setRef(cachedBackBuffer0_, *ppS);
    }
    return bbHr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetRasterStatus(UINT swapChain, D3DRASTER_STATUS* p) noexcept {
    notePeDeviceCallAfterPresent("GetRasterStatus");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex,
                                 recorderState_.recorderLockRequired);
    if (!p || swapChain != 0) {
        return D3DERR_INVALIDCALL;
    }
    // Synthesize a monotonically-advancing ScanLine so apps that VBlank-poll do
    // not spin forever. dxmt9 has no real per-line vblank signal from Metal;
    // the helper takes a per-call counter and the current backbuffer height.
    static std::atomic<uint64_t> rasterTick{0};
    uint32_t displayHeight = 0;
    D9CSwapChain* chain = borrowSwapChainHandle(swapChain);
    if (chain) {
        D9CPresentParams cpp{};
        if (SUCCEEDED(hr32(dxmt9c_swapchain_get_present_params(chain, &cpp)))) {
            displayHeight = cpp.backBufferHeight;
        }
    }
    const auto tick = rasterTick.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto est = ::dxmt9::d3d9::computeRasterStatusEstimate(tick, displayHeight);
    memset(p, 0, sizeof(*p));
    p->ScanLine = est.scanLine;
    p->InVBlank = est.inVBlank ? TRUE : FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetDialogBoxMode(BOOL enableDialogs) noexcept {
    dxmt9DeviceDebugLog("device_set_dialog_box_mode device=%p enable=%u", this, (unsigned)enableDialogs);
    // stub: Wine `wined3d_device_set_dialog_box_mode` returns WINED3D_OK
    // unconditionally — dialog-box mode requires Win32 user32/dwm primitives
    // that don't exist on macOS, but matching the Wine S_OK contract keeps
    // the conformance manifest aligned. Toggling has no observable effect.
    return S_OK;
}

void    STDMETHODCALLTYPE D3D9DeviceImpl::SetGammaRamp(UINT swapChain, DWORD flags, const D3DGAMMARAMP* ramp) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    dxmt9DeviceDebugLog("device_set_gamma_ramp device=%p swapChain=%u flags=0x%x ramp=%p",
                        this, swapChain, (unsigned)flags,
                        static_cast<const void*>(ramp));
    if (!ramp) return;
    const bool captureGamma =
        peCaptureState_ &&
        peCaptureState_->renderTapeCapture.state() ==
            dxmt9::d3d9::RenderTapeCaptureState::Capturing;
    if (captureGamma) {
        // A direct state mutation is an ordering boundary. Seal any
        // pending draw/state chunk before journaling GammaRampSet so the
        // provider cannot apply the new LUT ahead of older work.
        const HRESULT flushHr =
            flushPeRecorder(PeRecorderFlushReason::Barrier);
        if (FAILED(flushHr)) {
            abortRenderTapeCapture("gamma_ramp_barrier");
        }
    }
    // Byte-copy: D3DGAMMARAMP is a POD (3 * 256 * WORD). sizeof
    // is the safe shape regardless of any future struct growth.
    std::memcpy(&gammaRamp_, ramp, sizeof(D3DGAMMARAMP));
    if (dev_) {
        dxmt9c_device_set_gamma_ramp(dev_, reinterpret_cast<const uint16_t*>(ramp));
    }
    if (captureGamma && peCaptureState_ &&
        peCaptureState_->renderTapeCapture.state() ==
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        const auto *bytes = reinterpret_cast<const std::byte *>(ramp);
        NotifyRenderTapeOrderedControlForChild(
            dxmt9::d3d9::RenderTapeOrderedControlHeader{
                .identity = {},
                .kind = static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeControlKind::GammaRampSet),
                .disposition = static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeControlDisposition::Completed),
                .resultCode = 0,
                .controlBytes = dxmt9::d3d9::kRenderTapeGammaRampBytes,
            },
            std::span<const std::byte>(
                bytes, dxmt9::d3d9::kRenderTapeGammaRampBytes));
    }
}

void    STDMETHODCALLTYPE D3D9DeviceImpl::GetGammaRamp(UINT swapChain, D3DGAMMARAMP* p) noexcept {
    dxmt9DeviceDebugLog("device_get_gamma_ramp device=%p swapChain=%u out=%p",
                        this, swapChain, static_cast<void*>(p));
    if (!p) return;
    std::memcpy(p, &gammaRamp_, sizeof(D3DGAMMARAMP));
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateTexture(UINT w, UINT h, UINT levels,
                                                     DWORD usage, D3DFORMAT fmt,
                                                     D3DPOOL pool,
                                                     IDirect3DTexture9** ppTex,
                                                     HANDLE* psh) noexcept {
    if (!ppTex) return D3DERR_INVALIDCALL;
    *ppTex = nullptr;
    if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
    const HRESULT autogenHr =
        peAutogenTextureCreationHResult(usage, pool, levels, false);
    if (FAILED(autogenHr)) return autogenHr;
    const HRESULT levelHr = peTextureLevelCountHResult(std::min(w, h), std::max(w, h), levels);
    if (FAILED(levelHr)) return levelHr;
    // Wine D3D9Ex contract from dlls/d3d9/tests/d3d9ex.c
    // test_resource_access: D3DPOOL_MANAGED is rejected outright on
    // Ex devices (every MANAGED row in the matrix is marked
    // valid=FALSE, regardless of usage / format). Scaffold:
    // tests/conformance/d3d9/d3d9_conformance_device.c
    // test_resource_access_ex_pool_policy.
    if (extended_ && pool == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
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
    uint64_t sharedValue = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared =
        extended_ && psh && pool == D3DPOOL_DEFAULT ? &sharedValue : nullptr;
    D9CTexture* t = dxmt9c_device_create_texture_shared(dev_, w, h, levels,
                                                  usage, (uint32_t)fmt,
                                                  (uint32_t)pool,
                                                  providerShared);
    if (!t) return D3DERR_INVALIDCALL;
    *ppTex = CreatePeTexture(t, this, surfaceTextureContext(), diagnosticObserverForChild(),
                             userPtr, userPitch);
    if (!*ppTex) {
        dxmt9c_texture_release(t);
        return E_OUTOFMEMORY;
    }
    if (providerShared) *psh = (HANDLE)(uintptr_t)sharedValue;
    D3D9PeValidatedTexture validatedTexture{};
    if (SUCCEEDED(D3D9PeValidateTexture(
            *ppTex, static_cast<IDirect3DDevice9*>(this),
            &validatedTexture))) {
        notifyRenderTapeCreatedTexture(
            t, validatedTexture.wire(),
            dxmt9::d3d9::RenderTapeTextureDimension::Texture2D);
    }
    // Wine base_vidmem_accounting_policy expects a strictly-decreasing
    // budget on non-Ex devices; ex_vidmem_accounting_policy expects
    // the value to stay roughly constant for D3D9Ex devices (the spec
    // reports an "unlimited" budget for Ex). Only charge non-Ex.
    if (!extended_) {
        vidmemBytesUsedShadow_ += static_cast<uint64_t>(w) * h
            * std::max<uint32_t>(1u, levels) * 4u;
    }
    dxmt9DeviceDebugLog("device_create_texture -> texture=%p", *ppTex);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateVolumeTexture(UINT w, UINT h, UINT d,
                                                           UINT levels, DWORD usage,
                                                           D3DFORMAT fmt, D3DPOOL pool,
                                                           IDirect3DVolumeTexture9** ppTex,
                                                           HANDLE* psh) noexcept {
    if (!ppTex) return D3DERR_INVALIDCALL;
    *ppTex = nullptr;
    if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
    const HRESULT autogenHr =
        peAutogenTextureCreationHResult(usage, pool, levels, true);
    if (FAILED(autogenHr)) return autogenHr;
    const HRESULT levelHr =
        peTextureLevelCountHResult(std::min({w, h, d}), std::max({w, h, d}), levels);
    if (FAILED(levelHr)) return levelHr;
    // Wine D3D9Ex contract: see CreateTexture above — MANAGED pool
    // is rejected outright on Ex devices.
    if (extended_ && pool == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
    const HRESULT sharedHr = validateSharedHandleForTexture(extended_, psh, pool, levels, false);
    if (FAILED(sharedHr)) return sharedHr;
    dxmt9DeviceDebugLog("device_create_volume_texture device=%p size=%ux%ux%u levels=%u usage=0x%x fmt=%u pool=%u",
                        this, w, h, d, levels, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
    uint64_t sharedValue = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared =
        extended_ && psh && pool == D3DPOOL_DEFAULT ? &sharedValue : nullptr;
    D9CTexture* t = dxmt9c_device_create_volume_texture_shared(dev_, w, h, d, levels,
                                                         usage, (uint32_t)fmt,
                                                         (uint32_t)pool,
                                                         providerShared);
    if (!t) return D3DERR_INVALIDCALL;
    *ppTex = CreatePeVolumeTexture(t, this, surfaceTextureContext(),
                                   diagnosticObserverForChild());
    if (!*ppTex) {
        dxmt9c_texture_release(t);
        return E_OUTOFMEMORY;
    }
    if (providerShared) *psh = (HANDLE)(uintptr_t)sharedValue;
    D3D9PeValidatedTexture validatedTexture{};
    if (SUCCEEDED(D3D9PeValidateTexture(
            *ppTex, static_cast<IDirect3DDevice9*>(this),
            &validatedTexture))) {
        notifyRenderTapeCreatedTexture(
            t, validatedTexture.wire(),
            dxmt9::d3d9::RenderTapeTextureDimension::Volume);
    }
    dxmt9DeviceDebugLog("device_create_volume_texture -> texture=%p", *ppTex);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateCubeTexture(UINT size, UINT levels,
                                                         DWORD usage, D3DFORMAT fmt,
                                                         D3DPOOL pool,
                                                         IDirect3DCubeTexture9** ppTex,
                                                         HANDLE* psh) noexcept {
    if (!ppTex) return D3DERR_INVALIDCALL;
    *ppTex = nullptr;
    if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
    const HRESULT autogenHr =
        peAutogenTextureCreationHResult(usage, pool, levels, false);
    if (FAILED(autogenHr)) return autogenHr;
    const HRESULT levelHr = peTextureLevelCountHResult(size, size, levels);
    if (FAILED(levelHr)) return levelHr;
    // Wine D3D9Ex contract: see CreateTexture above — MANAGED pool
    // is rejected outright on Ex devices.
    if (extended_ && pool == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
    const HRESULT sharedHr = validateSharedHandleForTexture(extended_, psh, pool, levels, false);
    if (FAILED(sharedHr)) return sharedHr;
    dxmt9DeviceDebugLog("device_create_cube_texture device=%p size=%u levels=%u usage=0x%x fmt=%u pool=%u",
                        this, size, levels, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
    uint64_t sharedValue = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared =
        extended_ && psh && pool == D3DPOOL_DEFAULT ? &sharedValue : nullptr;
    D9CTexture* t = dxmt9c_device_create_cube_texture_shared(dev_, size, levels,
                                                       usage, (uint32_t)fmt,
                                                       (uint32_t)pool,
                                                       providerShared);
    if (!t) return D3DERR_INVALIDCALL;
    *ppTex = CreatePeCubeTexture(t, this, surfaceTextureContext(),
                                 diagnosticObserverForChild());
    if (!*ppTex) {
        dxmt9c_texture_release(t);
        return E_OUTOFMEMORY;
    }
    if (providerShared) *psh = (HANDLE)(uintptr_t)sharedValue;
    D3D9PeValidatedTexture validatedTexture{};
    if (SUCCEEDED(D3D9PeValidateTexture(
            *ppTex, static_cast<IDirect3DDevice9*>(this),
            &validatedTexture))) {
        notifyRenderTapeCreatedTexture(
            t, validatedTexture.wire(),
            dxmt9::d3d9::RenderTapeTextureDimension::Cube);
    }
    dxmt9DeviceDebugLog("device_create_cube_texture -> texture=%p", *ppTex);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateVertexBuffer(UINT len, DWORD usage,
                                                          DWORD fvf, D3DPOOL pool,
                                                          IDirect3DVertexBuffer9** ppBuf,
                                                          HANDLE* psh) noexcept {
    if (!ppBuf) return D3DERR_INVALIDCALL;
    *ppBuf = nullptr;
    // Wine D3D9 (test_vertex_buffer_desc_binding_policy line 2887):
    // SCRATCH pool is invalid for vertex buffers; Wine returns
    // D3DERR_INVALIDCALL.
    if (pool == D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
    const HRESULT sharedHr = validateSharedHandleForBuffer(extended_, psh, pool);
    if (FAILED(sharedHr)) return sharedHr;
    dxmt9DeviceDebugLog("device_create_vertex_buffer device=%p len=%u usage=0x%x fvf=0x%x pool=%u",
                        this, len, (unsigned)usage, (unsigned)fvf, (unsigned)pool);
    uint64_t sharedValue = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared =
        extended_ && psh && pool == D3DPOOL_DEFAULT ? &sharedValue : nullptr;
    D9CBuffer* b = dxmt9c_device_create_vertex_buffer_shared(dev_, len, usage,
                                                       fvf, (uint32_t)pool,
                                                       providerShared);
    if (!b) return D3DERR_INVALIDCALL;
    *ppBuf = CreatePeVertexBuffer(b, this, bufferContext(),
                                  diagnosticObserverForChild());
    if (!*ppBuf) {
        dxmt9c_buffer_release(b);
        return E_OUTOFMEMORY;
    }
    if (providerShared) *psh = (HANDLE)(uintptr_t)sharedValue;
    D3D9PeValidatedVertexBuffer validatedBuffer{};
    if (SUCCEEDED(D3D9PeValidateVertexBuffer(
            *ppBuf, static_cast<IDirect3DDevice9*>(this),
            &validatedBuffer))) {
        notifyRenderTapeCreatedBuffer(b, validatedBuffer.wire());
    }
    dxmt9DeviceDebugLog("device_create_vertex_buffer -> buffer=%p", *ppBuf);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateIndexBuffer(UINT len, DWORD usage,
                                                         D3DFORMAT fmt, D3DPOOL pool,
                                                         IDirect3DIndexBuffer9** ppBuf,
                                                         HANDLE* psh) noexcept {
    if (!ppBuf) return D3DERR_INVALIDCALL;
    *ppBuf = nullptr;
    // Wine D3D9 (test_index_buffer_desc_binding_policy line 2768):
    // SCRATCH pool is invalid for index buffers; Wine returns
    // D3DERR_INVALIDCALL.
    if (pool == D3DPOOL_SCRATCH) return D3DERR_INVALIDCALL;
    const HRESULT sharedHr = validateSharedHandleForBuffer(extended_, psh, pool);
    if (FAILED(sharedHr)) return sharedHr;
    dxmt9DeviceDebugLog("device_create_index_buffer device=%p len=%u usage=0x%x fmt=%u pool=%u",
                        this, len, (unsigned)usage, (unsigned)fmt, (unsigned)pool);
    uint64_t sharedValue = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared =
        extended_ && psh && pool == D3DPOOL_DEFAULT ? &sharedValue : nullptr;
    D9CBuffer* b = dxmt9c_device_create_index_buffer_shared(dev_, len, usage,
                                                      (uint32_t)fmt,
                                                      (uint32_t)pool,
                                                      providerShared);
    if (!b) return D3DERR_INVALIDCALL;
    *ppBuf = CreatePeIndexBuffer(b, this, bufferContext(),
                                 diagnosticObserverForChild());
    if (!*ppBuf) {
        dxmt9c_buffer_release(b);
        return E_OUTOFMEMORY;
    }
    if (providerShared) *psh = (HANDLE)(uintptr_t)sharedValue;
    D3D9PeValidatedIndexBuffer validatedBuffer{};
    if (SUCCEEDED(D3D9PeValidateIndexBuffer(
            *ppBuf, static_cast<IDirect3DDevice9*>(this),
            &validatedBuffer))) {
        notifyRenderTapeCreatedBuffer(b, validatedBuffer.wire());
    }
    dxmt9DeviceDebugLog("device_create_index_buffer -> buffer=%p", *ppBuf);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt,
                                                          D3DMULTISAMPLE_TYPE ms,
                                                          DWORD msQual, BOOL lockable,
                                                          IDirect3DSurface9** ppS,
                                                          HANDLE* psh) noexcept {
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
    // create_rt_ds_failure_policy: depth-stencil formats cannot be RTs.
    if (fmt == D3DFMT_D16 || fmt == D3DFMT_D24X8 || fmt == D3DFMT_D24S8
            || fmt == D3DFMT_D32 || fmt == D3DFMT_D15S1
            || fmt == D3DFMT_D24X4S4 || fmt == D3DFMT_D24FS8
            || fmt == D3DFMT_D32F_LOCKABLE || fmt == D3DFMT_D16_LOCKABLE
            || fmt == D3DFMT_D32_LOCKABLE) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT sharedHr = validateSharedHandleForDefaultSurface(extended_, psh);
    if (FAILED(sharedHr)) return sharedHr;
    dxmt9DeviceDebugLog("device_create_render_target device=%p size=%ux%u fmt=%u ms=%u msQual=%u lockable=%u",
                        this, w, h, (unsigned)fmt, (unsigned)ms, (unsigned)msQual, (unsigned)lockable);
    // Wine d3d9 test_invalid_multisample: CreateRenderTarget probes
    // CheckDeviceMultiSampleType to validate the (sampleCount,
    // quality) pair before dispatching the allocation. Mapping:
    //   - D3DMULTISAMPLE_NONE + quality>=1 → INVALIDCALL
    //   - CheckDeviceMultiSampleType returns NOTAVAILABLE → INVALIDCALL
    //   - quality >= reportedLevels → INVALIDCALL
    // The backend allocator silently dropped quality, so the
    // quality-vs-reported check has to live at the PE boundary.
    if (ms == D3DMULTISAMPLE_NONE && msQual != 0) {
        return D3DERR_INVALIDCALL;
    }
    if (ms != D3DMULTISAMPLE_NONE && factory_) {
        DWORD reportedQuality = 0;
        const HRESULT msHr = factory_->CheckDeviceMultiSampleType(
            adapter_, deviceType_, fmt, /*windowed=*/FALSE, ms,
            &reportedQuality);
        if (FAILED(msHr)) {
            return D3DERR_INVALIDCALL;
        }
        if (msQual >= reportedQuality) {
            return D3DERR_INVALIDCALL;
        }
    }
    uint64_t sh = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared = extended_ && psh ? &sh : nullptr;
    D9CSurface* s = dxmt9c_device_create_render_target(dev_, w, h,
                                                        (uint32_t)fmt,
                                                        (uint32_t)ms, msQual,
                                                        lockable ? 1u : 0u,
                                                        providerShared);
    if (!s) return D3DERR_INVALIDCALL;
    *ppS = CreatePeSurface(s, this, nullptr, surfaceTextureContext(),
                           diagnosticObserverForChild());
    if (!*ppS) {
        dxmt9c_surface_release(s);
        return E_OUTOFMEMORY;
    }
    if (providerShared) *psh = (HANDLE)(uintptr_t)sh;
    dxmt9DeviceDebugLog("device_create_render_target -> surface=%p", *ppS);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateDepthStencilSurface(UINT w, UINT h,
                                                                 D3DFORMAT fmt,
                                                                 D3DMULTISAMPLE_TYPE ms,
                                                                 DWORD msQual,
                                                                 BOOL discard,
                                                                 IDirect3DSurface9** ppS,
                                                                 HANDLE* psh) noexcept {
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
    // create_rt_ds_failure_policy: only depth-stencil formats are valid
    // as a DS surface. Colour formats must reject with INVALIDCALL.
    const bool isDepthFormat =
        fmt == D3DFMT_D16 || fmt == D3DFMT_D24X8 || fmt == D3DFMT_D24S8
        || fmt == D3DFMT_D32 || fmt == D3DFMT_D15S1
        || fmt == D3DFMT_D24X4S4 || fmt == D3DFMT_D24FS8
        || fmt == D3DFMT_D32F_LOCKABLE || fmt == D3DFMT_D16_LOCKABLE
        || fmt == D3DFMT_D32_LOCKABLE;
    if (!isDepthFormat) return D3DERR_INVALIDCALL;
    const HRESULT sharedHr = validateSharedHandleForDefaultSurface(extended_, psh);
    if (FAILED(sharedHr)) return sharedHr;
    dxmt9DeviceDebugLog("device_create_depth_stencil_surface device=%p size=%ux%u fmt=%u ms=%u msQual=%u discard=%u",
                        this, w, h, (unsigned)fmt, (unsigned)ms, (unsigned)msQual, (unsigned)discard);
    uint64_t sh = psh ? (uint64_t)(uintptr_t)*psh : 0;
    uint64_t* providerShared = extended_ && psh ? &sh : nullptr;
    D9CSurface* s = dxmt9c_device_create_depth_stencil(dev_, w, h,
                                                        (uint32_t)fmt,
                                                        (uint32_t)ms, msQual,
                                                        discard ? 1u : 0u,
                                                        providerShared);
    if (!s) return D3DERR_INVALIDCALL;
    *ppS = CreatePeSurface(s, this, nullptr, surfaceTextureContext(),
                           diagnosticObserverForChild());
    if (!*ppS) {
        dxmt9c_surface_release(s);
        return E_OUTOFMEMORY;
    }
    if (providerShared) *psh = (HANDLE)(uintptr_t)sh;
    dxmt9DeviceDebugLog("device_create_depth_stencil_surface -> surface=%p", *ppS);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::UpdateSurface(IDirect3DSurface9* src,
                                                     const RECT* srcRect,
                                                     IDirect3DSurface9* dst,
                                                     const POINT* dstPt) noexcept {
    dxmt9PeSetCurrentCallName("UpdateSurface");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    dxmt9DeviceDebugLog("device_update_surface device=%p src=%p dst=%p srcRect=%s dstPt=%s",
                        this, src, dst,
                        srcRect ? "<custom>" : "<full>",
                        dstPt ? "<custom>" : "<origin>");
    // Wine d3d9 conformance (test_mipmap_surface_update_lock_policy):
    // UpdateSurface requires the source to be SYSTEMMEM, the
    // destination to be DEFAULT, matching formats, and the source
    // not be currently locked. wined3d's
    // device_update_surface validates all four invariants before
    // initiating the copy; mirroring them here keeps a malformed
    // call from generating an upload record.
    if (!src || !dst) {
        return D3DERR_INVALIDCALL;
    }
    D3D9PeValidatedSurface source{};
    D3D9PeValidatedSurface destination{};
    if (FAILED(D3D9PeValidateSurface(
            src, static_cast<IDirect3DDevice9*>(this), &source)) ||
        FAILED(D3D9PeValidateSurface(
            dst, static_cast<IDirect3DDevice9*>(this), &destination))) {
        return D3DERR_INVALIDCALL;
    }
    D3DSURFACE_DESC srcDesc{};
    D3DSURFACE_DESC dstDesc{};
    if (FAILED(src->GetDesc(&srcDesc)) || FAILED(dst->GetDesc(&dstDesc))) {
        return D3DERR_INVALIDCALL;
    }
    if (srcDesc.Pool != D3DPOOL_SYSTEMMEM ||
        dstDesc.Pool != D3DPOOL_DEFAULT) {
        return D3DERR_INVALIDCALL;
    }
    if (srcDesc.Format != dstDesc.Format) {
        return D3DERR_INVALIDCALL;
    }
    if (source.localMetadata() != 0u || destination.localMetadata() != 0u) {
        return D3DERR_INVALIDCALL;
    }
    if (srcRect) {
        if (srcRect->left < 0 || srcRect->top < 0 ||
            srcRect->right <= srcRect->left ||
            srcRect->bottom <= srcRect->top ||
            static_cast<UINT>(srcRect->right) > srcDesc.Width ||
            static_cast<UINT>(srcRect->bottom) > srcDesc.Height) {
            return D3DERR_INVALIDCALL;
        }
    }
    {
        const UINT copyW = srcRect ? static_cast<UINT>(srcRect->right - srcRect->left)
                                   : srcDesc.Width;
        const UINT copyH = srcRect ? static_cast<UINT>(srcRect->bottom - srcRect->top)
                                   : srcDesc.Height;
        if (dstPt) {
            if (dstPt->x < 0 || dstPt->y < 0 ||
                static_cast<UINT>(dstPt->x) + copyW > dstDesc.Width ||
                static_cast<UINT>(dstPt->y) + copyH > dstDesc.Height) {
                return D3DERR_INVALIDCALL;
            }
        } else if (copyW > dstDesc.Width || copyH > dstDesc.Height) {
            return D3DERR_INVALIDCALL;
        }
    }
    D9CRect cs{}, cd{};
    if (srcRect) cs = toR(*srcRect);
    if (dstPt) { cd.left = dstPt->x; cd.top = dstPt->y;
                 cd.right = dstPt->x; cd.bottom = dstPt->y; }
    // Fire-and-forget copy records stay queued until the normal chunk
    // boundary. The raw D9C wrappers are AddRef'd by the pending chunk
    // so callers may release their D3D9 wrappers immediately.
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return barrierHr;
    // Handle indices are assigned by appendUpdateSurface as it appends
    // the refs, so they stay zero here.
    const D9CCommandChunkWireUpdateSurface wire{
        .srcHandleIndex = 0u,
        .dstHandleIndex = 0u,
        .hasSrcRect = srcRect ? 1u : 0u,
        .hasDstPoint = dstPt ? 1u : 0u,
        .srcRect = cs,
        .dstPoint = cd,
    };
    const HRESULT appendHr = appendRecord(
        D9C_COMMAND_RECORD_UPDATE_SURFACE,
        kLegacyUpdateSurfaceSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = phase.begin();
            const bool ok = dxmt9::d3d9::pe::appendUpdateSurface(
                builder, wire, source.wire(), destination.wire());
            phase.recordEncode(t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
    if (SUCCEEDED(appendHr)) {
        D3D9PeMarkSurfaceAutogenDirty(dst);
    }
    return appendHr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::UpdateTexture(IDirect3DBaseTexture9* src,
                                                     IDirect3DBaseTexture9* dst) noexcept {
    dxmt9PeSetCurrentCallName("UpdateTexture");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    // Wine d3d9 IDirect3DDevice9::UpdateTexture: both args non-NULL;
    // src must be SYSTEMMEM; dst must NOT be SYSTEMMEM/SCRATCH. See
    // test_update_texture_pool_copy_2d in d3d9_conformance_resource.c.
    if (!src || !dst) return D3DERR_INVALIDCALL;
    D3D9PeValidatedTexture source{};
    D3D9PeValidatedTexture destination{};
    if (FAILED(D3D9PeValidateTexture(
            src, static_cast<IDirect3DDevice9*>(this), &source)) ||
        FAILED(D3D9PeValidateTexture(
            dst, static_cast<IDirect3DDevice9*>(this), &destination))) {
        return D3DERR_INVALIDCALL;
    }
    bool palettizedUpdate = false;
    if (src->GetType() == D3DRTYPE_TEXTURE && dst->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC sd{}, dd{};
        ((IDirect3DTexture9*)src)->GetLevelDesc(0, &sd);
        ((IDirect3DTexture9*)dst)->GetLevelDesc(0, &dd);
        if (sd.Pool != D3DPOOL_SYSTEMMEM) return D3DERR_INVALIDCALL;
        if (dd.Pool == D3DPOOL_SYSTEMMEM || dd.Pool == D3DPOOL_SCRATCH)
            return D3DERR_INVALIDCALL;
        palettizedUpdate =
            (sd.Format == D3DFMT_P8 || sd.Format == D3DFMT_A8P8) &&
            sd.Format == dd.Format;
    }
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return barrierHr;
    const auto sourceWire = source.wire();
    const auto destinationWire = destination.wire();
    // Wine d3d9 UpdateTexture: both args non-NULL; src in SYSTEMMEM;
    // dst not SYSTEMMEM/SCRATCH. test_update_texture_pool_copy_2d.
    const HRESULT appendHr = appendRecord(
        D9C_COMMAND_RECORD_UPDATE_TEXTURE,
        kLegacyUpdateTextureSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = phase.begin();
            const bool ok = dxmt9::d3d9::pe::appendUpdateTexture(
                builder, sourceWire, destinationWire);
            phase.recordEncode(t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
    // The registry shadow is committed only after appendRecord accepts the
    // record. If the append's capacity flush or emitter fails, the
    // destination remains unchanged; the normal command bytes are also
    // left on their existing failure path.
    if (SUCCEEDED(appendHr)) {
        applyRenderTapeUpdateTextureClosure(sourceWire, destinationWire);
    }
    if (FAILED(appendHr) || !palettizedUpdate) {
        return appendHr;
    }
    // P8/A8P8 resources keep CPU-visible index/alpha shadow state while
    // sampling through an expanded A8R8G8B8 backing. Make the shadow copy
    // visible immediately so CPU ProcessVertices vertex-texture TEXLDL can
    // observe a preceding UpdateTexture without waiting for a later draw.
    const HRESULT flushHr = flushPendingCommandChunk(PeRecorderFlushReason::Readback);
    if (FAILED(flushHr)) return flushHr;
    // The immediate commit may run before a later SetTexture applies the
    // device-current palette to this destination. Re-expand now as well,
    // so fixed-function/programmable draws and CPU samplers see the same
    // palette if they sample the destination right after UpdateTexture.
    applyCurrentPaletteToTexture(dst);
    return appendHr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetRenderTargetData(IDirect3DSurface9* rt,
                                                           IDirect3DSurface9* dst) noexcept {
    dxmt9PeSetCurrentCallName("GetRenderTargetData");
    auto peCadence = claimPeFirstCallAfterPresent();
    const void* callerPc = DXMT9_PE_CALLSITE_PC();
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    // Decomposed on purpose: the cadence claim above must be taken before
    // the recorder lock and the milestone log after it, so this site cannot
    // use notePeDeviceCallAfterPresent. Gating the pair keeps the sample
    // inside the branch, so the disabled path constructs nothing. When
    // tracking is off both calls were already no-ops -- the claim is
    // unclaimed and the milestone sample untracked -- so skipping them
    // changes no emission.
    if (dxmt9PeCallTrackingEnabled()) {
        const auto peCall =
            logPeCallMilestoneAfterPresent("GetRenderTargetData", callerPc);
        logPeFirstCallAfterPresent("GetRenderTargetData", peCadence,
                                   peCall);
    }
    dxmt9DeviceDebugLog("device_get_render_target_data device=%p rt=%p dst=%p",
                        this, rt, dst);
    // get_render_target_data_policy: both args must be non-NULL. Wine
    // rejects NULL with INVALIDCALL before any backend work.
    if (!rt || !dst) return D3DERR_INVALIDCALL;
    D3D9PeValidatedSurface source{};
    D3D9PeValidatedSurface destination{};
    if (FAILED(D3D9PeValidateSurface(
            rt, static_cast<IDirect3DDevice9*>(this), &source)) ||
        FAILED(D3D9PeValidateSurface(
            dst, static_cast<IDirect3DDevice9*>(this), &destination))) {
        return D3DERR_INVALIDCALL;
    }
    // Wine d3d9 multisample_get_rtdata_test (visual.c:17106) contract:
    // GetRenderTargetData cannot copy from a multisampled render target
    // into a SYSTEMMEM offscreen surface — there is no way to express
    // per-sample data in a single sysmem surface. The legal readback
    // path is StretchRect (MSAA -> non-MSAA resolve) followed by
    // GetRenderTargetData on the resolved RT.
    // get_render_target_data_msaa_policy.
    {
        D3DSURFACE_DESC sdSrc{};
        if (SUCCEEDED(rt->GetDesc(&sdSrc))) {
            if (sdSrc.MultiSampleType != D3DMULTISAMPLE_NONE) {
                return D3DERR_INVALIDCALL;
            }
        }
    }
    // Phase 24: chunk-recorder path. The PE caller is synchronous —
    // the call doesn't return until the data is in dst — but
    // routing through the chunk record stream keeps ordering atomic
    // with surrounding draws/clears in the SAME chunk. We append a
    // READBACK record then commit the chunk synchronously (Present
    // pattern); commit_chunk's per-record short-circuit propagates
    // the actual readback HRESULT back to PE.
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return barrierHr;
    const dxmt9::d3d9::pe::PeReadbackBatch readbackBatch{
        .source = source.wire(), .destination = destination.wire()};
    const HRESULT appendHr = appendRecord(
        D9C_COMMAND_RECORD_READBACK, kLegacyReadbackSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = phase.begin();
            const bool ok = dxmt9::d3d9::pe::appendPeExactReadbackSingleton(
                builder, readbackBatch);
            phase.recordEncode(t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
    if (FAILED(appendHr)) return appendHr;
    // Sync semantics: commit the chunk now and wait for completion.
    // flushPendingCommandChunk routes through commit_chunk -> server's
    // record dispatcher -> readback record handler.
    const HRESULT flushHr =
        flushPendingCommandChunk(PeRecorderFlushReason::Readback);
    if (SUCCEEDED(flushHr)) {
        (void)recorderState_.commandChunk.returnToLegacyFinalLayout();
    }
    return flushHr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetFrontBufferData(UINT sc, IDirect3DSurface9* surface) noexcept {
    dxmt9DeviceDebugLog("device_get_front_buffer_data device=%p sc=%u surface=%p",
                        this, sc, surface);
    if (!surface) return D3DERR_INVALIDCALL;
    IDirect3DSwapChain9* swapchain = nullptr;
    const HRESULT swapHr = GetSwapChain(sc, &swapchain);
    if (FAILED(swapHr) || !swapchain) return swapHr;
    const HRESULT hr = swapchain->GetFrontBufferData(surface);
    swapchain->Release();
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::StretchRect(IDirect3DSurface9* src,
                                                   const RECT* srcRect,
                                                   IDirect3DSurface9* dst,
                                                   const RECT* dstRect,
                                                   D3DTEXTUREFILTERTYPE filter) noexcept {
    dxmt9PeSetCurrentCallName("StretchRect");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    dxmt9DeviceDebugLog("device_stretch_rect device=%p src=%p dst=%p filter=%u srcRect=%s dstRect=%s",
                        this, src, dst, (unsigned)filter,
                        srcRect ? "<custom>" : "<full>",
                        dstRect ? "<custom>" : "<full>");
    // Wine d3d9 test_stretch_rect contract: NULL src or NULL dst, and
    // degenerate / negative-extent src or dst rects, are rejected with
    // D3DERR_INVALIDCALL before any wire-record dispatch
    // (stretch_rect_null_and_degenerate_policy).
    if (!src || !dst) return D3DERR_INVALIDCALL;
    D3D9PeValidatedSurface source{};
    D3D9PeValidatedSurface destination{};
    if (FAILED(D3D9PeValidateSurface(
            src, static_cast<IDirect3DDevice9*>(this), &source)) ||
        FAILED(D3D9PeValidateSurface(
            dst, static_cast<IDirect3DDevice9*>(this), &destination))) {
        return D3DERR_INVALIDCALL;
    }
    if (srcRect && (srcRect->right <= srcRect->left ||
                    srcRect->bottom <= srcRect->top)) {
        return D3DERR_INVALIDCALL;
    }
    if (dstRect && (dstRect->right <= dstRect->left ||
                    dstRect->bottom <= dstRect->top)) {
        return D3DERR_INVALIDCALL;
    }
    // Wine d3d9 depth_blit_test (visual.c:14713) contract: when either
    // surface is a depth-stencil format, the other must be the same
    // depth-stencil format. Cross-format depth blits (e.g. D24S8 -> D16)
    // and depth -> color blits are rejected with INVALIDCALL.
    // stretch_rect_depth_stencil_policy.
    {
        D3DSURFACE_DESC sdSrc{};
        D3DSURFACE_DESC sdDst{};
        const bool gotSrc = SUCCEEDED(src->GetDesc(&sdSrc));
        const bool gotDst = SUCCEEDED(dst->GetDesc(&sdDst));
        auto isDepth = [](D3DFORMAT f) {
            return f == D3DFMT_D16 || f == D3DFMT_D24X8 ||
                   f == D3DFMT_D24S8 || f == D3DFMT_D32 ||
                   f == D3DFMT_D15S1 || f == D3DFMT_D24X4S4 ||
                   f == D3DFMT_D24FS8 || f == D3DFMT_D32F_LOCKABLE ||
                   f == D3DFMT_D16_LOCKABLE || f == D3DFMT_D32_LOCKABLE;
        };
        if (gotSrc && gotDst) {
            const bool srcIsDepth = isDepth(sdSrc.Format);
            const bool dstIsDepth = isDepth(sdDst.Format);
            if ((srcIsDepth || dstIsDepth) &&
                sdSrc.Format != sdDst.Format) {
                return D3DERR_INVALIDCALL;
            }
        }
        // Wine d3d9 test_format_conversion (visual.c:27960) contract:
        // StretchRect does not perform block-compressed <-> linear
        // conversion. If either surface is a DXT* compressed format,
        // the other surface must have the same compressed format.
        // (DXT1 -> A8R8G8B8 must fail with D3DERR_INVALIDCALL.)
        // stretch_rect_format_conversion_policy.
        if (gotSrc && gotDst) {
            auto isCompressed = [](D3DFORMAT f) {
                return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 ||
                       f == D3DFMT_DXT3 || f == D3DFMT_DXT4 ||
                       f == D3DFMT_DXT5;
            };
            const bool srcIsCompressed = isCompressed(sdSrc.Format);
            const bool dstIsCompressed = isCompressed(sdDst.Format);
            if ((srcIsCompressed || dstIsCompressed) &&
                sdSrc.Format != sdDst.Format) {
                return D3DERR_INVALIDCALL;
            }
        }
        // Wine d3d9 test_multisample_stretch_rect (visual.c:4494)
        // contract: the destination of a StretchRect must not be
        // multisampled. MSAA -> non-MSAA is the D3D9 resolve idiom
        // and is allowed; non-MSAA -> MSAA and MSAA -> MSAA blits
        // are rejected with D3DERR_INVALIDCALL.
        // stretch_rect_multisample_resolve_policy.
        if (gotSrc && gotDst) {
            if (sdDst.MultiSampleType != D3DMULTISAMPLE_NONE) {
                return D3DERR_INVALIDCALL;
            }
        }
    }
    D9CRect cs{}, cd{};
    if (srcRect) cs = toR(*srcRect); if (dstRect) cd = toR(*dstRect);
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return barrierHr;
    const D9CCommandChunkWireStretchRect wire{
        .srcHandleIndex = 0u,
        .dstHandleIndex = 0u,
        .hasSrcRect = srcRect ? 1u : 0u,
        .hasDstRect = dstRect ? 1u : 0u,
        .filter = (uint32_t)filter,
        .reserved0 = 0u,
        .srcRect = srcRect ? cs : D9CRect{},
        .dstRect = dstRect ? cd : D9CRect{},
    };
    const HRESULT appendHr = appendRecord(
        D9C_COMMAND_RECORD_STRETCH_RECT,
        kLegacyStretchRectSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = phase.begin();
            const bool ok = dxmt9::d3d9::pe::appendStretchRect(
                builder, wire, source.wire(), destination.wire());
            phase.recordEncode(t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
    if (SUCCEEDED(appendHr)) {
        D3D9PeMarkSurfaceAutogenDirty(dst);
    }
    return appendHr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::ColorFill(IDirect3DSurface9* pSurf,
                                                 const RECT* pRect,
                                                 D3DCOLOR color) noexcept {
    dxmt9PeSetCurrentCallName("ColorFill");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    // Wine d3d9 ColorFill: DXT-compressed and SYSTEMMEM surfaces are
    // rejected. visual_colorfill_format_policy.
    if (!pSurf) return D3DERR_INVALIDCALL;
    D3D9PeValidatedSurface surface{};
    if (FAILED(D3D9PeValidateSurface(
            pSurf, static_cast<IDirect3DDevice9*>(this), &surface))) {
        return D3DERR_INVALIDCALL;
    }
    {
        D3DSURFACE_DESC sd{};
        if (SUCCEEDED(pSurf->GetDesc(&sd))) {
            if (sd.Pool == D3DPOOL_SYSTEMMEM) return D3DERR_INVALIDCALL;
            const D3DFORMAT f = sd.Format;
            if (f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 ||
                f == D3DFMT_DXT4 || f == D3DFMT_DXT5)
                return D3DERR_INVALIDCALL;
        }
    }
    dxmt9DeviceDebugLog("device_color_fill device=%p surf=%p rect=%s color=0x%08x",
                        this, pSurf, pRect ? "<custom>" : "<full>", (unsigned)color);
    D9CRect cr{}; if (pRect) cr = toR(*pRect);
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return barrierHr;
    const D9CCommandChunkWireColorFill wire{
        .surfaceHandleIndex = 0u,
        .colorARGB = (uint32_t)color,
        .hasRect = pRect ? 1u : 0u,
        .reserved0 = 0u,
        .rect = pRect ? cr : D9CRect{},
    };
    const HRESULT appendHr = appendRecord(
        D9C_COMMAND_RECORD_COLOR_FILL, kLegacyColorFillSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = phase.begin();
            const bool ok = dxmt9::d3d9::pe::appendColorFill(
                builder, wire, surface.wire());
            phase.recordEncode(t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
    if (SUCCEEDED(appendHr)) {
        D3D9PeMarkSurfaceAutogenDirty(pSurf);
    }
    return appendHr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateOffscreenPlainSurface(UINT w, UINT h,
                                                                   D3DFORMAT fmt,
                                                                   D3DPOOL pool,
                                                                   IDirect3DSurface9** ppS,
                                                                   HANDLE* psh) noexcept {
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    // Wine D3D9 contract from test_surface_dimensions: width or
    // height of zero is rejected with D3DERR_INVALIDCALL.
    if (w == 0 || h == 0) return D3DERR_INVALIDCALL;
    if (isUnknownFormat(fmt)) return D3DERR_INVALIDCALL;
    // visual_offscreen_surface_creation_policy: only DEFAULT, SYSTEMMEM
    // and SCRATCH are valid pools for offscreen-plain surfaces. MANAGED
    // is rejected with INVALIDCALL.
    if (pool == D3DPOOL_MANAGED) return D3DERR_INVALIDCALL;
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
    uint64_t* providerShared =
        extended_ && psh && pool == D3DPOOL_DEFAULT ? &sh : nullptr;
    D9CSurface* s = dxmt9c_device_create_offscreen_surface(dev_, w, h,
                                                            (uint32_t)fmt,
                                                            (uint32_t)pool,
                                                            providerShared);
    if (!s) return D3DERR_INVALIDCALL;
    *ppS = CreatePeSurface(s, this, nullptr, surfaceTextureContext(),
                           diagnosticObserverForChild(), true, userPtr,
                           userPitch);
    if (!*ppS) {
        dxmt9c_surface_release(s);
        return E_OUTOFMEMORY;
    }
    if (providerShared) *psh = (HANDLE)(uintptr_t)sh;
    dxmt9DeviceDebugLog("device_create_offscreen_surface -> surface=%p", *ppS);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetRenderTarget(DWORD idx,
                                                       IDirect3DSurface9** ppS) noexcept {
    return withPeCallScope(
        "GetRenderTarget", DXMT9_PE_CALLSITE_PC(), nullptr,
        [&](auto& peCall) __attribute__((always_inline)) noexcept -> HRESULT {
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("GetRenderTarget", hr);
    };
    if (!ppS) return finishPeCall(D3DERR_INVALIDCALL);
    dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u",
                        this, (unsigned)idx);
    if (idx < 4 && rtSlotExplicit_[idx]) {
        if (!rtSlots_[idx]) {
            *ppS = nullptr;
            dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> explicit null",
                                this, (unsigned)idx);
            return finishPeCall(D3DERR_NOTFOUND);
        }
        rtSlots_[idx]->AddRef();
        *ppS = rtSlots_[idx];
        dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> cached rt=%p",
                            this, (unsigned)idx, static_cast<void*>(*ppS));
        return finishPeCall(S_OK);
    }
    if (idx == 0 && cachedBackBuffer0_) {
        cachedBackBuffer0_->AddRef();
        *ppS = cachedBackBuffer0_;
        dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> cached backbuffer=%p",
                            this, (unsigned)idx, static_cast<void*>(*ppS));
        return finishPeCall(S_OK);
    }
    D9CSurface* s = dxmt9c_device_get_render_target(dev_, idx);
    *ppS = s ? CreatePeSurface(s, this, nullptr, surfaceTextureContext(),
                               diagnosticObserverForChild(), false)
             : nullptr;
    if (s && !*ppS) {
        dxmt9c_surface_release(s);
        return finishPeCall(E_OUTOFMEMORY);
    }
    dxmt9DeviceDebugLog("device_get_render_target device=%p idx=%u -> surface=%p",
                        this, (unsigned)idx, ppS ? static_cast<void*>(*ppS) : nullptr);
    return finishPeCall(s ? S_OK : D3DERR_NOTFOUND);
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetDepthStencilSurface(IDirect3DSurface9** ppS) noexcept {
    notePeDeviceCallAfterPresent("GetDepthStencilSurface");
    if (!ppS) return D3DERR_INVALIDCALL;
    // Wine d3d9 contract: when there is no depth-stencil surface bound,
    // the return code is D3DERR_NOTFOUND (not S_FALSE) and *ppS is NULL.
    // visual_depth_buffer_reset_policy + visual_depth_stencil_init_policy.
    if (dsSurfaceExplicit_) {
        if (!dsSurface_) {
            *ppS = nullptr;
            return D3DERR_NOTFOUND;
        }
        dsSurface_->AddRef();
        *ppS = dsSurface_;
        dxmt9DeviceDebugLog("device_get_depth_stencil_surface device=%p -> cached surface=%p",
                            this, static_cast<void*>(*ppS));
        return S_OK;
    }
    D9CSurface* s = dxmt9c_device_get_depth_stencil(dev_);
    *ppS = s ? CreatePeSurface(s, this, nullptr, surfaceTextureContext(),
                               diagnosticObserverForChild())
             : nullptr;
    if (s && !*ppS) {
        dxmt9c_surface_release(s);
        return E_OUTOFMEMORY;
    }
    dxmt9DeviceDebugLog("device_get_depth_stencil_surface device=%p -> surface=%p",
                        this, ppS ? static_cast<void*>(*ppS) : nullptr);
    return s ? S_OK : D3DERR_NOTFOUND;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetTransform(D3DTRANSFORMSTATETYPE state,
                                                    D3DMATRIX* pM) noexcept {
    notePeDeviceCallAfterPresent("GetTransform");
    if (!pM) return D3DERR_INVALIDCALL;
    dxmt9DeviceDebugLog("device_get_transform device=%p state=%u", this, (unsigned)state);
    const uint32_t stateKey = static_cast<uint32_t>(state);
    D9CMatrix wireM{};
    if (recorderState_.peState.transformShadowTyped().get(transformStateKey(stateKey), wireM)) {
        std::memcpy(pM, &wireM, sizeof(wireM));
        return S_OK;
    }
    return hr32(dxmt9c_device_get_transform(dev_, stateKey,
                reinterpret_cast<D9CMatrix*>(pM)));
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetViewport(D3DVIEWPORT9* pVP) noexcept {
    return withPeCallScope(
        "GetViewport", DXMT9_PE_CALLSITE_PC(), nullptr,
        [&](auto& peCall) __attribute__((always_inline)) noexcept -> HRESULT {
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("GetViewport", hr);
    };
    if (!pVP) return finishPeCall(D3DERR_INVALIDCALL);
    // Phase 12: PE shadow is the source of truth. SetViewport writes
    // only into recorderState_.peState.viewportShadow (recorder-active path);
    // round-trip the same value.
    const D9CViewport& vp = recorderState_.peState.viewportShadow();
    pVP->X = vp.x; pVP->Y = vp.y;
    pVP->Width = vp.width; pVP->Height = vp.height;
    pVP->MinZ = vp.minZ;   pVP->MaxZ   = vp.maxZ;
    dxmt9DeviceDebugLog("device_get_viewport device=%p -> x=%u y=%u w=%u h=%u minZ=%f maxZ=%f",
                        this, pVP->X, pVP->Y, pVP->Width, pVP->Height, pVP->MinZ, pVP->MaxZ);
    return finishPeCall(S_OK);
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetScissorRect(RECT* pR) noexcept {
    return withPeCallScope(
        "GetScissorRect", DXMT9_PE_CALLSITE_PC(), nullptr,
        [&](auto& peCall) __attribute__((always_inline)) noexcept -> HRESULT {
    const auto finishPeCall = [&](HRESULT hr) noexcept {
        return peCall.finish("GetScissorRect", hr);
    };
    if (!pR) return finishPeCall(D3DERR_INVALIDCALL);
    // Phase 12: PE shadow is the source of truth (see GetViewport).
    const D9CRect& cr = recorderState_.peState.scissorShadow();
    pR->left = cr.left; pR->top = cr.top;
    pR->right = cr.right; pR->bottom = cr.bottom;
    return finishPeCall(S_OK);
        });
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetMaterial(D3DMATERIAL9* pM) noexcept {
    notePeDeviceCallAfterPresent("GetMaterial");
    if (!pM) return D3DERR_INVALIDCALL;
    dxmt9DeviceDebugLog("device_get_material device=%p", this);
    // PE-shadow is the source of truth: SetMaterial only writes the
    // shadow, never the C-side state. Reading from C would return the
    // default-constructed value instead of the last Set value.
    std::memcpy(pM, &recorderState_.peState.materialShadow(), sizeof(D3DMATERIAL9));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetLight(DWORD idx, D3DLIGHT9* pL) noexcept {
    notePeDeviceCallAfterPresent("GetLight");
    dxmt9DeviceDebugLog("device_get_light device=%p idx=%u", this, (unsigned)idx);
    if (!pL) return D3DERR_INVALIDCALL;
    if (idx >= D9C_DRAW_PACKET_MAX_LIGHTS) {
        // Unset slot — Wine returns INVALIDCALL for never-Set indices.
        return D3DERR_INVALIDCALL;
    }
    const D9CLight& cl = recorderState_.peState.lightShadow()[idx];
    pL->Type = (D3DLIGHTTYPE)cl.type;
    std::memcpy(&pL->Diffuse,  &cl.diffuse,  sizeof(D3DCOLORVALUE));
    std::memcpy(&pL->Specular, &cl.specular, sizeof(D3DCOLORVALUE));
    std::memcpy(&pL->Ambient,  &cl.ambient,  sizeof(D3DCOLORVALUE));
    pL->Position.x = cl.position[0];
    pL->Position.y = cl.position[1];
    pL->Position.z = cl.position[2];
    pL->Direction.x = cl.direction[0];
    pL->Direction.y = cl.direction[1];
    pL->Direction.z = cl.direction[2];
    pL->Range = cl.range;
    pL->Falloff = cl.falloff;
    pL->Attenuation0 = cl.attenuation0;
    pL->Attenuation1 = cl.attenuation1;
    pL->Attenuation2 = cl.attenuation2;
    pL->Theta = cl.theta;
    pL->Phi = cl.phi;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetLightEnable(DWORD idx, BOOL* pEn) noexcept {
    notePeDeviceCallAfterPresent("GetLightEnable");
    dxmt9DeviceDebugLog("device_get_light_enable device=%p idx=%u", this, (unsigned)idx);
    if (!pEn) return D3DERR_INVALIDCALL;
    // Phase 12: LightEnable shadow is the source of truth for
    // idx < D9C_DRAW_PACKET_MAX_LIGHTS (the setter writes into
    // recorderState_.peState.lightEnableShadow exclusively in the recorder-active
    // path). High indices fall through to the legacy unix-call —
    // mirror the boundary so idx out of shadow range stays FALSE
    // by default (no easy bridge read here without an extra ABI).
    if (idx < D9C_DRAW_PACKET_MAX_LIGHTS) {
        const DWORD bit = 1u << idx;
        *pEn = (recorderState_.peState.lightEnableShadow() & bit) ? TRUE : FALSE;
    } else {
        *pEn = FALSE;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetClipPlane(DWORD idx, float* pPlane) noexcept {
    notePeDeviceCallAfterPresent("GetClipPlane");
    dxmt9DeviceDebugLog("device_get_clip_plane device=%p idx=%u", this, (unsigned)idx);
    if (!pPlane) return D3DERR_INVALIDCALL;
    if (idx >= 6) return D3DERR_INVALIDCALL;
    // Phase 12: PE shadow is the source of truth — SetClipPlane
    // writes into recorderState_.peState.clipPlaneShadow exclusively (the array
    // is zero-initialized by PeHotStateShadow default ctor, which
    // matches the D3D9 post-CreateDevice all-zero contract).
    const std::size_t off = static_cast<std::size_t>(idx) * 4u;
    std::memcpy(pPlane, &recorderState_.peState.clipPlaneShadow()[off], sizeof(float) * 4);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetClipStatus(const D3DCLIPSTATUS9* p) noexcept {
    notePeDeviceCallAfterPresent("SetClipStatus");
    dxmt9DeviceDebugLog("device_set_clip_status device=%p", this);
    // gap_d3d9 B.8: dxmt9 does not track per-primitive clip status — no hardware
    // path exposes per-vertex clip-flag accumulation, exactly like wined3d's
    // storage-free stub. Reject null (the one real wined3d contract) and
    // otherwise accept without storing; echoing the seed back would be a
    // meaningless fake value, so GetClipStatus returns a defined default instead.
    if (!p) return D3DERR_INVALIDCALL;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetClipStatus(D3DCLIPSTATUS9* p) noexcept {
    notePeDeviceCallAfterPresent("GetClipStatus");
    dxmt9DeviceDebugLog("device_get_clip_status device=%p", this);
    if (!p) return D3DERR_INVALIDCALL;
    // Defined "everything visible / nothing clipped" default rather than echoing
    // a meaningless seed (no real clip accumulation exists on the HW path) or
    // leaving the caller's buffer untouched.
    p->ClipUnion = 0u;
    p->ClipIntersection = 0xFFFFFFFFu;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetRenderState(D3DRENDERSTATETYPE state,
                                                      DWORD* pValue) noexcept {
    notePeDeviceCallAfterPresent("GetRenderState");
    if (!pValue) return D3DERR_INVALIDCALL;
    uint32_t shadowValue = 0;
    if (recorderState_.peState.renderStateShadowTyped().get(
            renderStateSlotKey(static_cast<DWORD>(state)), shadowValue)) {
        *pValue = shadowValue;
        return S_OK;
    }
    *pValue = dxmt9c_device_get_render_state(dev_, (uint32_t)state);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateStateBlock(D3DSTATEBLOCKTYPE type,
                                                        IDirect3DStateBlock9** ppSB) noexcept {
    notePeDeviceCallAfterPresent("CreateStateBlock");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!ppSB) return D3DERR_INVALIDCALL;
    if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
    // D3D9 creation contract: a failed create must leave the out-pointer
    // NULL before the error HRESULT is returned.
    *ppSB = nullptr;
    if (!isValidD3DStateBlockType(type) ||
        recorderState_.stateBlockTransaction.isRecording()) {
        return D3DERR_INVALIDCALL;
    }
    std::int32_t injectedHr = 0;
    if (dxmt9PeConsumeStateBlockFault(
            PeStateBlockFaultPoint::AllocPre, injectedHr)) {
        if (diagnosticObserverForChild())
            diagnosticObserverForChild()->notifyStateBlockFault(false,
                                                                injectedHr);
        return hr32(injectedHr);
    }
    // State-block creation needs current server state.
    // flushPeRecorder() routes pending PE state through chunk records.
    const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::StateBlock);
    if (FAILED(flushHr)) return flushHr;
    dxmt9DeviceDebugLog("device_create_state_block device=%p type=%u", this, (unsigned)type);
    D9CStateBlock* sb = dxmt9c_device_create_state_block(dev_, (uint32_t)type);
    if (!sb) return D3DERR_INVALIDCALL;
    *ppSB = CreatePeStateBlock(
        sb, this, stateBlockContext(), diagnosticObserverForChild(),
        stateBlockCaptureDispositionFromType(static_cast<std::uint32_t>(type)));
    if (!*ppSB) return E_OUTOFMEMORY;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::BeginStateBlock() noexcept {
    notePeDeviceCallAfterPresent("BeginStateBlock");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
    if (recorderState_.stateBlockTransaction.isRecording()) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::StateBlock);
    if (FAILED(flushHr)) {
        recorderState_.stateBlockTransaction.beginFailed();
        return flushHr;
    }
    dxmt9DeviceDebugLog("device_begin_state_block device=%p", this);
    const HRESULT hr = hr32(dxmt9c_device_begin_state_block(dev_));
    if (SUCCEEDED(hr)) {
        if (!recorderState_.stateBlockTransaction.beginAccepted(
                d3d9PeReleaseStateBlockRef)) {
            // The recording epoch is monotonic and intentionally fails closed
            // on exhaustion; do not expose a backend-accepted Begin without a
            // fresh capability witness.
            return D3DERR_DEVICELOST;
        }
    } else {
        recorderState_.stateBlockTransaction.beginFailed();
    }
    dxmt9DeviceDebugLog("device_begin_state_block -> hr=0x%08x", (unsigned)hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::EndStateBlock(IDirect3DStateBlock9** ppSB) noexcept {
    notePeDeviceCallAfterPresent("EndStateBlock");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!ppSB) return D3DERR_INVALIDCALL;
    if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
    // EndStateBlock without a matching BeginStateBlock returns INVALIDCALL
    // and MUST leave the out-pointer UNTOUCHED — the Wine d3d9 oracle checks
    // `stateblock == sentinel` here (begin_end_state_block_policy /
    // stateblock_invalid_type_recording_invalid_calls). So null the
    // out-pointer only once we are actually attempting the create.
    if (!recorderState_.stateBlockTransaction.isRecording()) {
        return D3DERR_INVALIDCALL;
    }
    *ppSB = nullptr;
    std::int32_t injectedHr = 0;
    if (dxmt9PeConsumeStateBlockFault(
            PeStateBlockFaultPoint::EndPre, injectedHr)) {
        if (diagnosticObserverForChild())
            diagnosticObserverForChild()->notifyStateBlockFault(false,
                                                                injectedHr);
        recorderState_.stateBlockTransaction.endPreEffectFailed();
        return hr32(injectedHr);
    }
    if (dxmt9PeConsumeStateBlockFault(
            PeStateBlockFaultPoint::AllocPre, injectedHr)) {
        if (diagnosticObserverForChild())
            diagnosticObserverForChild()->notifyStateBlockFault(false,
                                                                injectedHr);
        recorderState_.stateBlockTransaction.endPreEffectFailed();
        return hr32(injectedHr);
    }
    const HRESULT flushHr = flushPeRecorder(PeRecorderFlushReason::StateBlock);
    if (FAILED(flushHr)) {
        recorderState_.stateBlockTransaction.endPreEffectFailed();
        return flushHr;
    }
    dxmt9DeviceDebugLog("device_end_state_block device=%p", this);
    D9CStateBlock* sb = nullptr;
    if (dxmt9PeConsumeStateBlockFault(
            PeStateBlockFaultPoint::BridgePre, injectedHr)) {
        if (diagnosticObserverForChild())
            diagnosticObserverForChild()->notifyStateBlockFault(false,
                                                                injectedHr);
        recorderState_.stateBlockTransaction.endPreEffectFailed();
        return hr32(injectedHr);
    }
    HRESULT hr = hr32(dxmt9c_device_end_state_block(dev_, &sb));
    if (SUCCEEDED(hr) &&
        dxmt9PeConsumeStateBlockEnteredFault(
            PeStateBlockFaultPoint::EndEntered, injectedHr)) {
        if (diagnosticObserverForChild())
            diagnosticObserverForChild()->notifyStateBlockFault(true,
                                                                injectedHr);
        if (sb) {
            dxmt9c_stateblock_release(sb);
            sb = nullptr;
        }
        recorderState_.stateBlockTransaction.failEnd(
            d3d9PeReleaseStateBlockRef);
        return hr32(injectedHr);
    }
    if (FAILED(hr)) {
        DXMT_ASSERT(peStateBlockEndTransition(
            PeStateBlockEndPhase::Backend, false) ==
            PeStateBlockEndAction::Poison);
        // The unix End implementation consumes its recording flag before its
        // remaining fallible work. A failed bridge call therefore cannot be
        // retried safely: leave Recording, discard the unpublished PE
        // candidate, and fail-stop all later recorder writes until Reset.
        recorderState_.stateBlockTransaction.failEnd(
            d3d9PeReleaseStateBlockRef);
        dxmt9DeviceDebugLog("device_end_state_block -> hr=0x%08x sb=%p out=%p",
                            (unsigned)hr, static_cast<void*>(sb), *ppSB);
        return hr;
    }

    recorderState_.stateBlockTransaction.enterEndPublication();
    if (sb) {
        // Mark Begin/End context so the new stateblock's ctor
        // takes its tracked-keys set from
        // stateBlockTransformRecorded (which may be empty if all
        // recording was MultiplyTransform) instead of falling
        // back to a full transformShadow capture.
        *ppSB = CreatePeStateBlock(sb, this, stateBlockContext(),
                                   diagnosticObserverForChild(),
                                   StateBlockCaptureDisposition::Explicit);
    }
    if (!*ppSB) {
        DXMT_ASSERT(peStateBlockEndTransition(
            PeStateBlockEndPhase::Wrapper, false) ==
            PeStateBlockEndAction::Poison);
        // Backend End already accepted, so wrapper publication failure is
        // also fail-stop rather than a retryable End.
        recorderState_.stateBlockTransaction.finishEndPublication(
            false, d3d9PeReleaseStateBlockRef);
        return E_OUTOFMEMORY;
    }
    // Clear only after the immutable child snapshot has been published.
    recorderState_.stateBlockTransaction.finishEndPublication(
        true, d3d9PeReleaseStateBlockRef);
    dxmt9DeviceDebugLog("device_end_state_block -> hr=0x%08x sb=%p out=%p",
                        (unsigned)hr, static_cast<void*>(sb), *ppSB);
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetTextureStageState(DWORD stage,
                                                            D3DTEXTURESTAGESTATETYPE type,
                                                            DWORD* pValue) noexcept {
    notePeDeviceCallAfterPresent("GetTextureStageState");
    if (!pValue) return D3DERR_INVALIDCALL;
    if (stage >= kFragmentBlendStageCount) return D3DERR_INVALIDCALL;
    if (!isValidTextureStageStateType(type)) return D3DERR_INVALIDCALL;
    const TextureStageIndex stageKey = textureStageIndexKey(stage);
    const TextureStageStateType typeKey =
        textureStageStateTypeKey(static_cast<uint32_t>(type));
    uint32_t shadowValue = 0;
    if (recorderState_.peState.tssShadowTyped().get(stageKey, typeKey, shadowValue)) {
        *pValue = shadowValue;
        return S_OK;
    }
    *pValue = dxmt9c_device_get_texture_stage_state(
        dev_, rawSlot(stageKey), rawSlot(typeKey));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetSamplerState(DWORD sampler,
                                                       D3DSAMPLERSTATETYPE type,
                                                       DWORD* pValue) noexcept {
    notePeDeviceCallAfterPresent("GetSamplerState");
    if (!pValue) return D3DERR_INVALIDCALL;
    SamplerIndex samplerIndexKeyVal{};
    if (!samplerIndexKey(sampler, samplerIndexKeyVal)) {
        *pValue = 0;
        return S_OK;
    }
    // Phase 34: serve from the PE-side shadow so a Set/Get round-trip
    // is observable without forcing a recorder flush. Mirrors the
    // GetTextureStageState pattern (see above): shadow first, fall
    // back to the core-side read for slots the app has never written
    // (those return the resetState() defaults).
    SamplerStateType stateTypeKeyVal{};
    if (samplerStateTypeKey(static_cast<uint32_t>(type), stateTypeKeyVal)) {
        uint32_t shadowValue = 0;
        if (recorderState_.peState.samplerStateShadowTyped().get(
                samplerIndexKeyVal, stateTypeKeyVal, shadowValue)) {
            *pValue = shadowValue;
            return S_OK;
        }
    }
    *pValue = dxmt9c_device_get_sampler_state(
        dev_, rawSlot(samplerIndexKeyVal), (uint32_t)type);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::ValidateDevice(DWORD* pPasses) noexcept {
    notePeDeviceCallAfterPresent("ValidateDevice");
    dxmt9DeviceDebugLog("device_validate_device device=%p", this);
    if (pPasses) *pPasses = 1; return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) noexcept {
    notePeDeviceCallAfterPresent("SetPaletteEntries");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    dxmt9DeviceDebugLog("device_set_palette_entries device=%p palette=%u entries=%p",
                        this, palette, static_cast<const void*>(entries));
    if (!entries) return D3DERR_INVALIDCALL;
    // D3DPTEXTURECAPS_ALPHAPALETTE policy: when the cap is NOT set,
    // any entry with non-trivial alpha (peFlags != 0xff) is rejected
    // and the previous palette must be preserved.
    D9CCaps cc{};
    bool alphaPaletteCap = false;
    if (SUCCEEDED(hr32(dxmt9c_device_get_caps(dev_, &cc)))) {
        D3DCAPS9 dcaps{};
        FillD3DCaps9(cc, &dcaps);
        alphaPaletteCap = (dcaps.TextureCaps & D3DPTEXTURECAPS_ALPHAPALETTE) != 0;
    }
    if (!alphaPaletteCap) {
        for (UINT i = 0; i < 256; ++i) {
            if (entries[i].peFlags != 0xff) {
                return D3DERR_INVALIDCALL;
            }
        }
    }
    std::array<PALETTEENTRY, 256> candidate{};
    std::memcpy(candidate.data(), entries,
                sizeof(PALETTEENTRY) * candidate.size());
    const auto insertion = dxmt9::d3d9::pe::replacePalette(
        palettes_, palette, candidate);
    if (insertion !=
        dxmt9::d3d9::pe::PublicAllocationResult::Completed) {
        return E_OUTOFMEMORY;
    }
    if (currentPaletteSet_ && currentPaletteIndex_ == palette) {
        applyCurrentPaletteToBoundTextures();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetPaletteEntries(UINT palette, PALETTEENTRY* out) noexcept {
    notePeDeviceCallAfterPresent("GetPaletteEntries");
    dxmt9DeviceDebugLog("device_get_palette_entries device=%p palette=%u out=%p",
                        this, palette, static_cast<void*>(out));
    if (!out) return D3DERR_INVALIDCALL;
    const auto it = palettes_.find(palette);
    if (it == palettes_.end()) return D3DERR_INVALIDCALL;
    std::memcpy(out, it->second.data(), sizeof(PALETTEENTRY) * 256);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetCurrentTexturePalette(UINT palette) noexcept {
    notePeDeviceCallAfterPresent("SetCurrentTexturePalette");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    dxmt9DeviceDebugLog("device_set_current_texture_palette device=%p palette=%u", this, palette);
    if (palettes_.find(palette) == palettes_.end()) {
        return D3DERR_INVALIDCALL;
    }
    currentPaletteIndex_ = palette;
    currentPaletteSet_ = true;
    applyCurrentPaletteToBoundTextures();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetCurrentTexturePalette(UINT* p) noexcept {
    notePeDeviceCallAfterPresent("GetCurrentTexturePalette");
    dxmt9DeviceDebugLog("device_get_current_texture_palette device=%p out=%p",
                        this, static_cast<void*>(p));
    if (!p) return D3DERR_INVALIDCALL;
    if (!currentPaletteSet_) return D3DERR_INVALIDCALL;
    *p = currentPaletteIndex_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetSoftwareVertexProcessing(BOOL enable) noexcept {
    notePeDeviceCallAfterPresent("SetSoftwareVertexProcessing");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    dxmt9DeviceDebugLog("device_set_software_vertex_processing device=%p enable=%u", this, (unsigned)enable);
    softwareVertexProcessing_ = enable ? TRUE : FALSE;
    return S_OK;
}

BOOL    STDMETHODCALLTYPE D3D9DeviceImpl::GetSoftwareVertexProcessing() noexcept {
    notePeDeviceCallAfterPresent("GetSoftwareVertexProcessing");
    dxmt9DeviceDebugLog("device_get_software_vertex_processing device=%p", this);
    return softwareVertexProcessing_;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetNPatchMode(float segments) noexcept {
    notePeDeviceCallAfterPresent("SetNPatchMode");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    dxmt9DeviceDebugLog("device_set_npatch_mode device=%p segments=%f", this, segments);
    // stub: Wine returns S_OK; N-Patch tessellation was removed in D3D10, legacy
    // apps tolerate a no-op.
    return S_OK;
}

float   STDMETHODCALLTYPE D3D9DeviceImpl::GetNPatchMode() noexcept {
    notePeDeviceCallAfterPresent("GetNPatchMode");
    // stub: Wine returns 0.0f; N-Patch tessellation removed in D3D10, legacy apps
    // tolerate a no-op.
    return 0.0f;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetTexture(DWORD stage,
                                                  IDirect3DBaseTexture9** ppTex) noexcept {
    notePeDeviceCallAfterPresent("GetTexture");
    if (!ppTex) return D3DERR_INVALIDCALL;
    uint32_t textureSlot = 0;
    if (!fragmentTextureStageSlot(stage, textureSlot)) {
        // Wine leaves the caller's out-pointer untouched on
        // INVALIDCALL -- test_get_set_texture asserts the sentinel
        // value (0xdeadbeef) survives the failed call.
        return D3DERR_INVALIDCALL;
    }
    IDirect3DBaseTexture9* t = textures_[textureSlot];
    if (t) t->AddRef();
    *ppTex = t;
    dxmt9DeviceDebugLog("device_get_texture device=%p stage=%u -> tex=%p",
                        this, (unsigned)stage, static_cast<void*>(t));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetFVF(DWORD* pFVF) noexcept {
    notePeDeviceCallAfterPresent("GetFVF");
    if (!pFVF) return D3DERR_INVALIDCALL;
    *pFVF = fvf_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateVertexDeclaration(
        const D3DVERTEXELEMENT9* pElems,
        IDirect3DVertexDeclaration9** ppVD) noexcept {
    notePeDeviceCallAfterPresent("CreateVertexDeclaration");
    if (!ppVD) return D3DERR_INVALIDCALL;
    *ppVD = nullptr;
    /* Wine returns INVALIDCALL with *ppVD == NULL on bad input. */
    const HRESULT validationHr = validateVertexElements(pElems);
    if (FAILED(validationHr)) {
        *ppVD = nullptr;
        return validationHr;
    }
    /* count elements until D3DDECL_END() */
    size_t n = 0;
    while (pElems[n].Stream != 0xFF) ++n;
    ++n; /* include D3DDECL_END */
    if (n > MAXD3DDECLLENGTH + 1) {
        *ppVD = nullptr;
        return D3DERR_INVALIDCALL;
    }
    D9CVertexElement tmp[MAXD3DDECLLENGTH + 1]{};
    for (size_t i = 0; i < n; ++i) {
        tmp[i].stream = pElems[i].Stream; tmp[i].offset = pElems[i].Offset;
        tmp[i].type   = pElems[i].Type;   tmp[i].method = pElems[i].Method;
        tmp[i].usage  = pElems[i].Usage;  tmp[i].usageIndex = pElems[i].UsageIndex;
    }
    D9CVertexDecl* d = dxmt9c_device_create_vertex_declaration(dev_, tmp);
    if (!d) {
        *ppVD = nullptr;
        return D3DERR_INVALIDCALL;
    }
    *ppVD = CreatePeVertexDecl(d, this, shaderDeclarationContext());
    if (!*ppVD) {
        dxmt9c_vdecl_release(d);
        return E_OUTOFMEMORY;
    }
    D3D9PeValidatedDeclaration validatedDeclaration{};
    if (SUCCEEDED(D3D9PeValidateVertexDecl(
            *ppVD, static_cast<IDirect3DDevice9*>(this),
            &validatedDeclaration))) {
        notifyRenderTapeCreatedVertexDecl(
            d, validatedDeclaration.wire(),
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(tmp),
                n * sizeof(tmp[0])), n);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetVertexDeclaration(
        IDirect3DVertexDeclaration9** ppVD) noexcept {
    notePeDeviceCallAfterPresent("GetVertexDeclaration");
    if (!ppVD) return D3DERR_INVALIDCALL;
    if (vdecl_) vdecl_->AddRef();
    *ppVD = vdecl_; return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateVertexShader(const DWORD* pFn,
                                                          IDirect3DVertexShader9** ppVS) noexcept {
    notePeDeviceCallAfterPresent("CreateVertexShader");
    if (!ppVS) return D3DERR_INVALIDCALL;
    *ppVS = nullptr;
    /* Wine semantics: leave *ppVS as NULL on validation failure. */
    const HRESULT validationHr = validateShaderBytecodeForStage(pFn,
                                                                /*vertexStage=*/true);
    if (FAILED(validationHr)) {
        dxmt9DeviceDebugLog("device_create_vertex_shader rejected token0=0x%08x hr=0x%08x",
                            pFn ? static_cast<uint32_t>(pFn[0]) : 0u,
                            static_cast<uint32_t>(validationHr));
        *ppVS = nullptr;
        return validationHr;
    }
    dxmt9DeviceDebugLog("device_create_vertex_shader device=%p code=%p", this, pFn);
    D9CShader* s = dxmt9c_device_create_vertex_shader(dev_, reinterpret_cast<const uint32_t*>(pFn));
    if (!s) {
        *ppVS = nullptr;
        return D3DERR_INVALIDCALL;
    }
    *ppVS = CreatePeVertexShader(s, this, hashValidatedShaderBytecode(pFn), shaderDeclarationContext());
    if (!*ppVS) {
        dxmt9c_shader_release(s);
        return E_OUTOFMEMORY;
    }
    D3D9PeValidatedVertexShader validatedShader{};
    if (SUCCEEDED(D3D9PeValidateVertexShader(
            *ppVS, static_cast<IDirect3DDevice9*>(this),
            &validatedShader))) {
        notifyRenderTapeCreatedShader(s, validatedShader.wire(), 0u);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetVertexShader(IDirect3DVertexShader9** ppVS) noexcept {
    notePeDeviceCallAfterPresent("GetVertexShader");
    if (!ppVS) return D3DERR_INVALIDCALL;
    if (vs_) vs_->AddRef(); *ppVS = vs_; return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetVertexShaderConstantF(UINT start, float* pData,
                                                                UINT count) noexcept {
    notePeDeviceCallAfterPresent("GetVertexShaderConstantF");
    const HRESULT hr = validateConstRange(start, count, pData, kVsConstFMax);
    if (FAILED(hr)) return hr;
    readConstShadow(recorderState_.peConsts.vsConstF, start, pData, count, sizeof(float) * 4);
    return S_OK;    }

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetVertexShaderConstantI(UINT start, INT* pData,
                                                                UINT count) noexcept {
    notePeDeviceCallAfterPresent("GetVertexShaderConstantI");
    const HRESULT hr = validateConstRange(start, count, pData, kVsConstIMax);
    if (FAILED(hr)) return hr;
    readConstShadow(recorderState_.peConsts.vsConstI, start, pData, count, sizeof(int32_t) * 4);        return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetVertexShaderConstantB(UINT start, BOOL* pData,
                                                                UINT count) noexcept {
    notePeDeviceCallAfterPresent("GetVertexShaderConstantB");
    const HRESULT hr = validateConstRange(start, count, pData, kVsConstBMax);
    if (FAILED(hr)) return hr;
    readConstShadow(recorderState_.peConsts.vsConstB, start, pData, count, sizeof(uint32_t));        return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetStreamSource(UINT stream,
                                                       IDirect3DVertexBuffer9** ppBuf,
                                                       UINT* pOffset, UINT* pStride) noexcept {
    notePeDeviceCallAfterPresent("GetStreamSource");
    if (!ppBuf) return D3DERR_INVALIDCALL;
    if (stream >= 16) return D3DERR_INVALIDCALL;
    IDirect3DVertexBuffer9* b = streamSrc_[stream];
    if (b) b->AddRef();
    *ppBuf = b;
    if (pOffset) *pOffset = streamOff_[stream];
    if (pStride) *pStride = streamStr_[stream];
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetStreamSourceFreq(UINT stream, UINT* pFreq) noexcept {
    notePeDeviceCallAfterPresent("GetStreamSourceFreq");
    if (!pFreq) return D3DERR_INVALIDCALL;
    if (stream >= 16) return D3DERR_INVALIDCALL;
    const UINT freq = streamFreq_[stream];
    *pFreq = freq;
    dxmt9DeviceDebugLog("device_get_stream_source_freq device=%p stream=%u -> freq=0x%x",
                        this, stream, (unsigned)freq);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetIndices(IDirect3DIndexBuffer9** ppIBuf) noexcept {
    notePeDeviceCallAfterPresent("GetIndices");
    if (!ppIBuf) return D3DERR_INVALIDCALL;
    if (indexBuf_) indexBuf_->AddRef(); *ppIBuf = indexBuf_; return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreatePixelShader(const DWORD* pFn,
                                                         IDirect3DPixelShader9** ppPS) noexcept {
    notePeDeviceCallAfterPresent("CreatePixelShader");
    if (!ppPS) return D3DERR_INVALIDCALL;
    *ppPS = nullptr;
    const HRESULT validationHr = validateShaderBytecodeForStage(pFn,
                                                                /*vertexStage=*/false);
    if (FAILED(validationHr)) {
        dxmt9DeviceDebugLog("device_create_pixel_shader rejected token0=0x%08x hr=0x%08x",
                            pFn ? static_cast<uint32_t>(pFn[0]) : 0u,
                            static_cast<uint32_t>(validationHr));
        *ppPS = nullptr;
        return validationHr;
    }
    dxmt9DeviceDebugLog("device_create_pixel_shader device=%p code=%p", this, pFn);
    D9CShader* s = dxmt9c_device_create_pixel_shader(dev_, reinterpret_cast<const uint32_t*>(pFn));
    if (!s) {
        *ppPS = nullptr;
        return D3DERR_INVALIDCALL;
    }
    *ppPS = CreatePePixelShader(s, this, hashValidatedShaderBytecode(pFn), shaderDeclarationContext());
    if (!*ppPS) {
        dxmt9c_shader_release(s);
        return E_OUTOFMEMORY;
    }
    D3D9PeValidatedPixelShader validatedShader{};
    if (SUCCEEDED(D3D9PeValidatePixelShader(
            *ppPS, static_cast<IDirect3DDevice9*>(this),
            &validatedShader))) {
        notifyRenderTapeCreatedShader(s, validatedShader.wire(), 1u);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetPixelShader(IDirect3DPixelShader9** ppPS) noexcept {
    notePeDeviceCallAfterPresent("GetPixelShader");
    if (!ppPS) return D3DERR_INVALIDCALL;
    if (ps_) ps_->AddRef(); *ppPS = ps_; return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetPixelShaderConstantF(UINT start, float* pData,
                                                               UINT count) noexcept {
    notePeDeviceCallAfterPresent("GetPixelShaderConstantF");
    const HRESULT hr = validateConstRange(start, count, pData, kPsConstFMax);
    if (FAILED(hr)) return hr;
    readConstShadow(recorderState_.peConsts.psConstF, start, pData, count, sizeof(float) * 4);
    return S_OK;    }

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetPixelShaderConstantI(UINT start, INT* pData,
                                                               UINT count) noexcept {
    notePeDeviceCallAfterPresent("GetPixelShaderConstantI");
    const HRESULT hr = validateConstRange(start, count, pData, kPsConstIMax);
    if (FAILED(hr)) return hr;
    readConstShadow(recorderState_.peConsts.psConstI, start, pData, count, sizeof(int32_t) * 4);        return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetPixelShaderConstantB(UINT start, BOOL* pData,
                                                               UINT count) noexcept {
    notePeDeviceCallAfterPresent("GetPixelShaderConstantB");
    const HRESULT hr = validateConstRange(start, count, pData, kPsConstBMax);
    if (FAILED(hr)) return hr;
    readConstShadow(recorderState_.peConsts.psConstB, start, pData, count, sizeof(uint32_t));        return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::ProcessVertices(UINT srcStart, UINT dstIndex,
                                                       UINT vertexCount,
                                                       IDirect3DVertexBuffer9* dstBuffer,
                                                       IDirect3DVertexDeclaration9* declaration,
                                                       DWORD flags) noexcept {
    notePeDeviceCallAfterPresent("ProcessVertices");
    // T2 device-lost gate: lost devices must report DEVICELOST before
    // any ProcessVertices validation or unsupported-path rejection.
    if (deviceNotReset_) return D3DERR_DEVICELOST;
    const Context context{
        .device = dev_,
        .deviceIdentity = static_cast<IDirect3DDevice9*>(this),
        .fvf = fvf_,
        .vertexDeclaration = vdecl_,
        .vertexShader = vs_,
        .streamSources = std::span<
            IDirect3DVertexBuffer9* const,
            D9C_DRAW_PACKET_MAX_STREAMS>{streamSrc_},
        .streamOffsets = std::span<
            const UINT, D9C_DRAW_PACKET_MAX_STREAMS>{streamOff_},
        .streamStrides = std::span<
            const UINT, D9C_DRAW_PACKET_MAX_STREAMS>{streamStr_},
        .streamFrequencies = std::span<
            const UINT, D9C_DRAW_PACKET_MAX_STREAMS>{streamFreq_},
        .textures = std::span<
            IDirect3DBaseTexture9* const,
            D9C_DRAW_PACKET_MAX_TEXTURES>{textures_},
        .state = recorderState_.peState,
        .constants = recorderState_.peConsts,
    };
    return processVertices(
        context, srcStart, dstIndex, vertexCount,
        dstBuffer, declaration, flags);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) noexcept { return D3DERR_INVALIDCALL; }

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) noexcept { return D3DERR_INVALIDCALL; }

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::DeletePatch(UINT) noexcept {
    // stub: Wine returns S_OK; patch primitives unused on Metal.
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateQuery(D3DQUERYTYPE type,
                                                   IDirect3DQuery9** ppQ) noexcept {
    notePeDeviceCallAfterPresent("CreateQuery");
    // Query-type support gate. Unsupported / out-of-range types return
    // D3DERR_NOTAVAILABLE and must leave *ppQ untouched (the oracle probes
    // with a 0xdeadbeef sentinel); the support-probe form
    // CreateQuery(type, NULL) likewise must not mutate *ppQ. No early
    // nulling here — the success path assigns, failure paths leave the
    // caller's pointer as-is.
    // peQueryDataSizeForType reports a non-zero size for exactly the
    // supported set {EVENT, OCCLUSION, TIMESTAMP, TIMESTAMPDISJOINT,
    // TIMESTAMPFREQ}; everything else (e.g. 0xdeadbeef) reports 0.
    // Oracle: test_query_get_data_size_policy, query_support_probe.
    if (peQueryDataSizeForType(type) == 0u) return D3DERR_NOTAVAILABLE;
    D9CQuery* q = dxmt9c_device_create_query(dev_, (uint32_t)type);
    if (!q) return D3DERR_NOTAVAILABLE;
    if (!ppQ) {
        // Support-probe form: the type is supported, report S_OK but
        // create no object (caller passed a NULL out pointer).
        dxmt9c_query_release(q);
        return S_OK;
    }
    *ppQ = CreatePeQuery(q, this, queryContext(), diagnosticObserverForChild());
    if (!*ppQ) {
        dxmt9c_query_release(q);
        return E_OUTOFMEMORY;
    }
    D3D9PeValidatedQuery validatedQuery{};
    if (SUCCEEDED(D3D9PeValidateQuery(
            *ppQ, static_cast<IDirect3DDevice9*>(this),
            &validatedQuery))) {
        notifyRenderTapeCreatedQuery(q, validatedQuery.wire());
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetConvolutionMonoKernel(UINT,UINT,float*,float*) noexcept { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::ComposeRects(IDirect3DSurface9*,IDirect3DSurface9*,
                                                    IDirect3DVertexBuffer9*,UINT,
                                                    IDirect3DVertexBuffer9*,
                                                    D3DCOMPOSERECTSOP,int,int) noexcept { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetGPUThreadPriority(INT* p) noexcept {
    notePeDeviceCallAfterPresent("GetGPUThreadPriority");
    // stub: Wine returns S_OK; GPU thread priority is not exposed by Metal.
    if (p) *p = 0; return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetGPUThreadPriority(INT) noexcept {
    // stub: Wine returns S_OK; GPU thread priority is not exposed by Metal.
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::WaitForVBlank(UINT sc) noexcept {
    notePeDeviceCallAfterPresent("WaitForVBlank");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    const HRESULT hr = hr32(dxmt9c_device_wait_for_vblank(dev_, sc));
    const dxmt9::d3d9::RenderTapeFlushWaitControl payload{.waitedSeqId = 0u};
    NotifyRenderTapeOrderedControlForChild(
        dxmt9::d3d9::RenderTapeOrderedControlHeader{
            .kind = static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeControlKind::FlushWait),
            .disposition = static_cast<std::uint32_t>(
                SUCCEEDED(hr)
                    ? dxmt9::d3d9::RenderTapeControlDisposition::Completed
                    : dxmt9::d3d9::RenderTapeControlDisposition::Failed),
            .resultCode = static_cast<std::int32_t>(hr),
            .controlBytes = sizeof(payload)},
        std::as_bytes(std::span(&payload, 1u)));
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CheckResourceResidency(IDirect3DResource9**,
                                                              UINT32) noexcept {
    // stub: Wine returns S_OK; unified memory on Apple Silicon — all resources
    // are resident.
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::SetMaximumFrameLatency(UINT maxLatency) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    // Wine d3d9ex test_frame_latency contract: valid range is 1..30.
    // 0 or >= 31 must return D3DERR_INVALIDCALL.
    if (maxLatency == 0 || maxLatency >= 31)
        return D3DERR_INVALIDCALL;
    maxFrameLatencyShadow_ = maxLatency;
    return hr32(dxmt9c_device_set_maximum_frame_latency(dev_, maxLatency));
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetMaximumFrameLatency(UINT* p) noexcept {
    notePeDeviceCallAfterPresent("GetMaximumFrameLatency");
    if (!p) return D3DERR_INVALIDCALL;
    // PE-shadow: return value previously set or the default of 3.
    *p = maxFrameLatencyShadow_;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CheckDeviceState(HWND wnd) noexcept {
    notePeDeviceCallAfterPresent("CheckDeviceState");
    return hr32(dxmt9c_device_check_device_state(dev_,
                (uint64_t)(uintptr_t)wnd));
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateRenderTargetEx(UINT w, UINT h,
                                                            D3DFORMAT fmt,
                                                            D3DMULTISAMPLE_TYPE ms,
                                                            DWORD msQual, BOOL lockable,
                                                            IDirect3DSurface9** ppS,
                                                            HANDLE* psh,
                                                            DWORD usage) noexcept {
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
    return CreateRenderTarget(w, h, fmt, ms, msQual, lockable, ppS, psh);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateOffscreenPlainSurfaceEx(UINT w, UINT h,
                                                                     D3DFORMAT fmt,
                                                                     D3DPOOL pool,
                                                                     IDirect3DSurface9** ppS,
                                                                     HANDLE* psh,
                                                                     DWORD usage) noexcept {
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
    return CreateOffscreenPlainSurface(w, h, fmt, pool, ppS, psh);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::CreateDepthStencilSurfaceEx(UINT w, UINT h,
                                                                   D3DFORMAT fmt,
                                                                   D3DMULTISAMPLE_TYPE ms,
                                                                   DWORD msQual,
                                                                   BOOL discard,
                                                                   IDirect3DSurface9** ppS,
                                                                   HANDLE* psh,
                                                                   DWORD usage) noexcept {
    if (!ppS) return D3DERR_INVALIDCALL;
    *ppS = nullptr;
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
    return CreateDepthStencilSurface(w, h, fmt, ms, msQual, discard, ppS, psh);
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::ResetEx(D3DPRESENT_PARAMETERS* pPP,
                                               D3DDISPLAYMODEEX* pFsMode) noexcept {
    dxmt9PeSetCurrentCallName("ResetEx");
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!pPP) return D3DERR_INVALIDCALL;
    // ResetEx windowed/fullscreen mode rules: wrong mode Size, a mode
    // supplied for a windowed reset (or missing for a fullscreen reset),
    // and a fullscreen mode whose dimensions do not match the requested
    // back-buffer size are all rejected with D3DERR_INVALIDCALL.
    if (const HRESULT mhr = peResetExModeHResult(pPP->Windowed != FALSE,
            pFsMode != nullptr, pFsMode ? pFsMode->Size : 0u,
            pFsMode ? pFsMode->Width : 0u, pFsMode ? pFsMode->Height : 0u,
            pPP->BackBufferWidth, pPP->BackBufferHeight);
        FAILED(mhr)) {
        return mhr;
    }
    // Present-parameter validation (same rule as Reset / CreateDevice),
    // evaluated on the extended lane (FLIPEX allowed, cap 30).
    if (const HRESULT vhr = pePresentParamsHResult(pPP->SwapEffect,
            pPP->BackBufferCount, pPP->PresentationInterval,
            pPP->MultiSampleType, pPP->MultiSampleQuality, extended_);
        FAILED(vhr)) {
        return vhr;
    }
    D9CPresentParams cpp{};
    cpp.backBufferWidth  = pPP->BackBufferWidth;
    cpp.backBufferHeight = pPP->BackBufferHeight;
    cpp.backBufferFormat = (uint32_t)pPP->BackBufferFormat;
    cpp.backBufferCount  = pPP->BackBufferCount;
    cpp.multiSampleType  = (uint32_t)pPP->MultiSampleType;
    cpp.multiSampleQuality = pPP->MultiSampleQuality;
    cpp.swapEffect       = (uint32_t)pPP->SwapEffect;
    HWND effectiveDeviceWindow = pPP->hDeviceWindow ? pPP->hDeviceWindow
                                                    : creationWindow_;
    cpp.deviceWindow     = (uint64_t)(uintptr_t)effectiveDeviceWindow;
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
    const HRESULT flushHr = flushPeRecorder(
        PeRecorderFlushReason::Reset,
        peRecorderResetDisposition(
            recorderState_.stateBlockTransaction.isPoisoned()));
    if (FAILED(flushHr)) return flushHr;
    releaseAllBound();
    D3D9PeWsiBinding candidate =
        dxmt9PeAcquireWsiBinding(effectiveDeviceWindow);
    if (!dxmt9::wsi::validAdoptionCandidate(candidate) ||
        FAILED(dxmt9PeAdoptDeviceWsiBinding(dev_, candidate))) {
        dxmt9PeReleaseWsiBindingAfterQuiescence(candidate);
        recorderState_.stateBlockTransaction.resetFailed();
        return D3DERR_NOTAVAILABLE;
    }
    D3D9PeWsiBinding oldBinding = std::move(implicitWsiBinding_);
    clearPeStateTracking();
    releaseRecordedStateBlockRefs();
    const HRESULT hr = hr32(dxmt9c_device_reset_ex(dev_, &cpp,
        pFsMode ? &cdme : nullptr));
    if (SUCCEEDED(hr)) {
        dxmt9PeReleaseWsiBindingAfterQuiescence(oldBinding);
        implicitWsiBinding_ = std::move(candidate);
        recorderState_.stateBlockTransaction.resetSucceeded(
            d3d9PeReleaseStateBlockRef);
        deviceNotReset_ = false;
        // Same flags-capture as Reset().
        implicitSwapchainFlagsShadow_ = pPP->Flags;
        // T2: same viewport/scissor reset semantics as Reset().
        const uint32_t w = std::max<uint32_t>(1u, cpp.backBufferWidth);
        const uint32_t h = std::max<uint32_t>(1u, cpp.backBufferHeight);
        recorderState_.peState.maintenance().viewportShadow() = D9CViewport{0, 0, w, h, 0.0f, 1.0f};
        recorderState_.peState.maintenance().scissorShadow()  = D9CRect{0, 0, (int32_t)w, (int32_t)h};
        recorderState_.peState.maintenance().pendingViewport() = false;
        recorderState_.peState.maintenance().pendingScissor()  = false;
    } else {
        if (SUCCEEDED(dxmt9PeAdoptDeviceWsiBinding(dev_, oldBinding))) {
            dxmt9PeReleaseWsiBindingAfterQuiescence(candidate);
            implicitWsiBinding_ = std::move(oldBinding);
        } else {
            dxmt9PeFinalizeDeviceAndReleaseWsiBinding(dev_, candidate);
            dxmt9PeReleaseWsiBindingAfterQuiescence(oldBinding);
            implicitWsiBinding_ = {};
        }
        recorderState_.stateBlockTransaction.resetFailed();
    }
    if (peCaptureState_ &&
        peCaptureState_->renderTapeCapture.state() ==
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        const dxmt9::d3d9::RenderTapeResetControl payload{
            .reclaimedGeneration = 0u, .terminal = 1u};
        NotifyRenderTapeOrderedControlForChild(
            dxmt9::d3d9::RenderTapeOrderedControlHeader{
                .kind = static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeControlKind::Reset),
                .disposition = static_cast<std::uint32_t>(
                    SUCCEEDED(hr)
                        ? dxmt9::d3d9::RenderTapeControlDisposition::Terminal
                        : dxmt9::d3d9::RenderTapeControlDisposition::Failed),
                .resultCode = static_cast<std::int32_t>(hr),
                .controlBytes = sizeof(payload)},
            std::as_bytes(std::span(&payload, 1u)));
        abortRenderTapeCapture("reset_ex");
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::GetDisplayModeEx(UINT sc,
                                                        D3DDISPLAYMODEEX* pMode,
                                                        D3DDISPLAYROTATION* pRot) noexcept {
    notePeDeviceCallAfterPresent("GetDisplayModeEx");
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

HRESULT D3D9DeviceImpl::resolveImplicitDeclForFvf(
        DWORD fvf, IDirect3DVertexDeclaration9** out) noexcept {
        if (!out) return D3DERR_INVALIDCALL;
        *out = nullptr;
        if (fvf == 0) return S_OK;
        if (auto it = fvfDeclCache_.find(fvf); it != fvfDeclCache_.end()) {
            *out = it->second;
            return S_OK;
        }
        try {
            std::vector<D3DVERTEXELEMENT9> elements;
            fvfToVertexElements(fvf, elements);
            D9CVertexElement tmp[MAXD3DDECLLENGTH + 1]{};
            const size_t n = elements.size();
            if (n > MAXD3DDECLLENGTH + 1) return D3DERR_INVALIDCALL;
            for (size_t i = 0; i < n; ++i) {
                tmp[i].stream     = elements[i].Stream;
                tmp[i].offset     = elements[i].Offset;
                tmp[i].type       = elements[i].Type;
                tmp[i].method     = elements[i].Method;
                tmp[i].usage      = elements[i].Usage;
                tmp[i].usageIndex = elements[i].UsageIndex;
            }
            const auto result =
                dxmt9::d3d9::pe::createImplicitFvfDeclTransaction<
                    D9CVertexDecl*, IDirect3DVertexDeclaration9*>(
                    [&]() {
                        return dxmt9c_device_create_vertex_declaration(dev_, tmp);
                    },
                    [](D9CVertexDecl* decl) noexcept {
                        dxmt9c_vdecl_release(decl);
                    },
                    [&](D9CVertexDecl* decl) {
                        IDirect3DVertexDeclaration9* wrapper =
                            CreatePeVertexDecl(decl, this, shaderDeclarationContext());
                        if (wrapper) {
                            // Create must precede any failure cleanup because
                            // the wrapper destructor emits ObjectDestroy.
                            D3D9PeValidatedDeclaration validated{};
                            if (SUCCEEDED(D3D9PeValidateVertexDecl(
                                    wrapper,
                                    static_cast<IDirect3DDevice9*>(this),
                                    &validated))) {
                                notifyRenderTapeCreatedVertexDecl(
                                    decl, validated.wire(),
                                    std::span<const std::byte>(
                                        reinterpret_cast<const std::byte *>(tmp),
                                        n * sizeof(tmp[0])), n);
                            }
                        }
                        return wrapper;
                    },
                    [](IDirect3DVertexDeclaration9* decl) noexcept {
                        decl->Release();
                    },
                    [&](IDirect3DVertexDeclaration9* decl) {
                        return fvfDeclCache_.emplace(fvf, decl).second;
                    });
            if (!result) {
                return result.failure ==
                        dxmt9::d3d9::pe::ImplicitFvfDeclFailure::Backend
                    ? D3DERR_INVALIDCALL
                    : E_OUTOFMEMORY;
            }
            *out = result.wrapper;
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

HRESULT D3D9DeviceImpl::PrepareStateBlockApplyForChild(
        const D3D9StateBlockShadow& shadow) noexcept {
        if (recorderState_.stateBlockTransaction.isPoisoned()) return D3DERR_DEVICELOST;
        const auto snapshot = shadow.snapshot();
        discardPreparedStateBlockApply();
        const auto prepareConst = [&](ConstShadow& live,
                                      const StateBlockConstShadow& recorded,
                                      std::size_t elemSize,
                                      std::size_t maxElems) -> HRESULT {
            if (recorded.valueSize > StateBlockConstShadow::kMaxBytes ||
                recorded.trackedSize > StateBlockConstShadow::kMaxElems ||
                recorded.trackedSize > maxElems ||
                recorded.valueSize <
                    static_cast<std::size_t>(recorded.trackedSize) * elemSize) {
                return D3DERR_INVALIDCALL;
            }
            for (std::size_t i = 0; i < recorded.trackedSize; ++i) {
                if (recorded.trackedElems[i] != 0u &&
                    (i + 1u) * elemSize > recorded.valueSize) {
                    return D3DERR_INVALIDCALL;
                }
            }
            try {
                // Reserve only: preserving live values/dirty bits makes this
                // phase invisible if backend Apply subsequently fails.
                live.reserveCapacity(recorded.valueSize,
                                      recorded.trackedSize);
            } catch (const std::bad_alloc&) {
                return E_OUTOFMEMORY;
            }
            return S_OK;
        };
        HRESULT hr = prepareConst(recorderState_.peConsts.vsConstF, snapshot.constants().vsConstF,
                                  sizeof(float) * 4u, kVsConstFMax);
        if (FAILED(hr)) return hr;
        hr = prepareConst(recorderState_.peConsts.vsConstI, snapshot.constants().vsConstI,
                          sizeof(std::int32_t) * 4u, kVsConstIMax);
        if (FAILED(hr)) return hr;
        hr = prepareConst(recorderState_.peConsts.vsConstB, snapshot.constants().vsConstB,
                          sizeof(std::uint32_t), kVsConstBMax);
        if (FAILED(hr)) return hr;
        hr = prepareConst(recorderState_.peConsts.psConstF, snapshot.constants().psConstF,
                          sizeof(float) * 4u, kPsConstFMax);
        if (FAILED(hr)) return hr;
        hr = prepareConst(recorderState_.peConsts.psConstI, snapshot.constants().psConstI,
                          sizeof(std::int32_t) * 4u, kPsConstIMax);
        if (FAILED(hr)) return hr;
        hr = prepareConst(recorderState_.peConsts.psConstB, snapshot.constants().psConstB,
                          sizeof(std::uint32_t), kPsConstBMax);
        if (FAILED(hr)) return hr;

        // Resolve/cache and retain by walking the authoritative 26-store
        // APPLY PHYSICAL inventory. Resolution stays before every retain so a failing implicit
        // FVF lookup leaves no staged ownership. Commit can then stay
        // allocation-free after backend mutation; a tracked null remains
        // represented by its occupancy mask.
        HRESULT categoryHr = S_OK;
        snapshot.categories().forEachApplyPhysical(
            [&]<StateBlockApplyPhysicalStore store>(const auto& values) {
                constexpr auto descriptor =
                    stateBlockApplyPhysicalDescriptor<store>();
                constexpr auto role = descriptor.role;
                if constexpr (role ==
                              StateBlockApplyCategoryRole::ImplicitFvf) {
                    values.forEach([&](std::size_t, DWORD fvf) {
                        IDirect3DVertexDeclaration9* decl = nullptr;
                        if (SUCCEEDED(categoryHr)) {
                            categoryHr = resolveImplicitDeclForFvf(fvf, &decl);
                        }
                    });
                } else if constexpr (
                    descriptor.kind == StateBlockApplyPhysicalKind::Fixed) {
                    static_assert(
                        role == StateBlockApplyCategoryRole::Value ||
                        role == StateBlockApplyCategoryRole::
                                    CandidateOwnedVertexDeclaration ||
                        role >= StateBlockApplyCategoryRole::StagedTexture,
                        "StateBlock resolution role is not handled");
                } else if constexpr (
                    descriptor.kind == StateBlockApplyPhysicalKind::Keyed) {
                    static_assert(
                        store == StateBlockApplyPhysicalStore::renderStates ||
                        store == StateBlockApplyPhysicalStore::textureStageStates ||
                        store == StateBlockApplyPhysicalStore::samplerStates ||
                        store == StateBlockApplyPhysicalStore::transforms,
                        "StateBlock Prepare keyed store is not bound");
                } else {
                    static_assert(
                        store == StateBlockApplyPhysicalStore::vsConstF ||
                        store == StateBlockApplyPhysicalStore::vsConstI ||
                        store == StateBlockApplyPhysicalStore::vsConstB ||
                        store == StateBlockApplyPhysicalStore::psConstF ||
                        store == StateBlockApplyPhysicalStore::psConstI ||
                        store == StateBlockApplyPhysicalStore::psConstB,
                        "StateBlock Prepare constant store is not bound");
                }
            });
        if (FAILED(categoryHr)) return categoryHr;
        snapshot.categories().forEachApplyPhysical(
            [&]<StateBlockApplyPhysicalStore store>(const auto& values) {
                constexpr auto descriptor =
                    stateBlockApplyPhysicalDescriptor<store>();
                constexpr auto role = descriptor.role;
                if constexpr (
                    role == StateBlockApplyCategoryRole::StagedTexture) {
                    values.forEach([&](std::size_t slot, auto value) {
                        if (!recorderState_.stateBlockTransaction.stageTexture(
                                stateBlockTextureSlotKey(slot), value,
                                d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else if constexpr (
                    role == StateBlockApplyCategoryRole::StagedStreamSource) {
                    values.forEach([&](std::size_t slot, const auto& value) {
                        if (!recorderState_.stateBlockTransaction.stageStream(
                                stateBlockStreamSlotKey(slot), value,
                                d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else if constexpr (
                    role == StateBlockApplyCategoryRole::StagedVertexShader) {
                    values.forEach([&](std::size_t, auto value) {
                        if (!recorderState_.stateBlockTransaction.stageVertexShader(
                                value, d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else if constexpr (
                    role == StateBlockApplyCategoryRole::StagedPixelShader) {
                    values.forEach([&](std::size_t, auto value) {
                        if (!recorderState_.stateBlockTransaction.stagePixelShader(
                                value, d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else if constexpr (
                    role == StateBlockApplyCategoryRole::StagedIndexBuffer) {
                    values.forEach([&](std::size_t, auto value) {
                        if (!recorderState_.stateBlockTransaction.stageIndexBuffer(
                                value, d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else if constexpr (
                    role == StateBlockApplyCategoryRole::StagedRenderTarget) {
                    values.forEach([&](std::size_t slot, auto value) {
                        if (!recorderState_.stateBlockTransaction.stageRenderTarget(
                                stateBlockRenderTargetSlotKey(slot), value,
                                d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else if constexpr (
                    role == StateBlockApplyCategoryRole::StagedDepthStencil) {
                    values.forEach([&](std::size_t, auto value) {
                        if (!recorderState_.stateBlockTransaction.stageDepthStencil(
                                value, d3d9PeRetainStateBlockRef)) {
                            categoryHr = D3DERR_INVALIDCALL;
                        }
                    });
                } else {
                    if constexpr (descriptor.kind ==
                                  StateBlockApplyPhysicalKind::Keyed) {
                        static_assert(
                            store == StateBlockApplyPhysicalStore::renderStates ||
                            store == StateBlockApplyPhysicalStore::textureStageStates ||
                            store == StateBlockApplyPhysicalStore::samplerStates ||
                            store == StateBlockApplyPhysicalStore::transforms,
                            "StateBlock Prepare keyed store is not bound");
                    } else if constexpr (descriptor.kind ==
                                         StateBlockApplyPhysicalKind::Constant) {
                        static_assert(
                            store == StateBlockApplyPhysicalStore::vsConstF ||
                            store == StateBlockApplyPhysicalStore::vsConstI ||
                            store == StateBlockApplyPhysicalStore::vsConstB ||
                            store == StateBlockApplyPhysicalStore::psConstF ||
                            store == StateBlockApplyPhysicalStore::psConstI ||
                            store == StateBlockApplyPhysicalStore::psConstB,
                            "StateBlock Prepare constant store is not bound");
                    } else {
                        static_assert(
                            role == StateBlockApplyCategoryRole::Value ||
                            role == StateBlockApplyCategoryRole::ImplicitFvf ||
                            role == StateBlockApplyCategoryRole::
                                        CandidateOwnedVertexDeclaration,
                            "StateBlock Prepare role is not handled");
                    }
                }
            });
        if (FAILED(categoryHr)) {
            recorderState_.stateBlockTransaction.discardPrepared(
                d3d9PeReleaseStateBlockRef);
            return categoryHr;
        }
        recorderState_.stateBlockTransaction.markApplyPrepared();
        return categoryHr;
    }

void D3D9DeviceImpl::CommitStateBlockApplyForChild(
        const D3D9StateBlockShadow& shadow,
        StateBlockCaptureDisposition disposition) noexcept {
        if (recorderState_.stateBlockTransaction.isPoisoned()) return;
        const auto snapshot = shadow.snapshot();
        const bool replayThroughPe = stateBlockApplyPublication(disposition) ==
            StateBlockApplyPublication::PeReplayRequired;
        auto copyConst = [](ConstShadow& live,
                            const StateBlockConstShadow& recorded,
                            std::size_t elemSize) noexcept {
            (void)recorded.forEachRange(
                elemSize, [&](std::uint32_t start, std::uint32_t count,
                              const std::uint8_t* bytes) {
                    const std::size_t begin =
                        static_cast<std::size_t>(start) * elemSize;
                    const std::size_t end =
                        begin + static_cast<std::size_t>(count) * elemSize;
                    // PrepareStateBlockApplyForChild reserved this capacity;
                    // resize therefore cannot allocate in the commit phase.
                    if (live.values.size() < end) live.values.resize(end);
                    if (live.dirtyElems.size() <
                        static_cast<std::size_t>(start) + count) {
                        live.dirtyElems.resize(
                            static_cast<std::size_t>(start) + count);
                    }
                    std::memcpy(live.values.data() + begin, bytes, end - begin);
                    return true;
                });
        };

        auto publishConst = [replayThroughPe, &copyConst](
                                ConstShadow& live,
                                const StateBlockConstShadow& recorded,
                                std::size_t elemSize) noexcept {
            copyConst(live, recorded, elemSize);
            if (!replayThroughPe) {
                live.clear();
                return;
            }
            (void)recorded.forEachRange(
                elemSize, [&](std::uint32_t start, std::uint32_t count,
                              const std::uint8_t*) {
                    const std::uint32_t end = start + count;
                    std::fill(live.dirtyElems.begin() + start,
                              live.dirtyElems.begin() + end,
                              std::uint8_t{1});
                    if (!live.dirty()) {
                        live.dirtyStart = start;
                        live.dirtyEnd = end;
                    } else {
                        live.dirtyStart = std::min(live.dirtyStart, start);
                        live.dirtyEnd = std::max(live.dirtyEnd, end);
                    }
                    return true;
                });
        };

        auto* const semanticTokens = scalarSemanticObserver();
        snapshot.renderStates().forEach([&](RenderStateSlot key, DWORD value) {
            recorderState_.peState.maintenance().renderStateShadowTyped().set(key, value);
            if (replayThroughPe) {
                recorderState_.peState.maintenance().pendingRenderStatesTyped().set(key, value);
                if (semanticTokens) {
                    const bool tokenRecorded = semanticTokens->record(
                        dxmt9::d3d9::pe::ScalarSemanticCategory::RenderState,
                        rawSlot(key));
                    DXMT_ASSERT(tokenRecorded);
                    (void)tokenRecorded;
                }
            } else {
                recorderState_.peState.maintenance().pendingRenderStatesTyped().erase(key);
            }
            if (!replayThroughPe && semanticTokens) {
                (void)semanticTokens->eraseSuperseded(
                    dxmt9::d3d9::pe::ScalarSemanticCategory::RenderState,
                    rawSlot(key));
            }
        });
        snapshot.textureStageStates().forEach(
            [&](TextureStageIndex stage, TextureStageStateType type,
                DWORD value) {
                recorderState_.peState.maintenance().tssShadowTyped().set(stage, type, value);
                if (replayThroughPe) {
                    recorderState_.peState.maintenance().pendingTssTyped().set(stage, type, value);
                    if (semanticTokens) {
                        const bool tokenRecorded = semanticTokens->record(
                            dxmt9::d3d9::pe::ScalarSemanticCategory::
                                TextureStageState,
                            rawSlot(stage), rawSlot(type));
                        DXMT_ASSERT(tokenRecorded);
                        (void)tokenRecorded;
                    }
                } else {
                    recorderState_.peState.maintenance().pendingTssTyped().erase(stage, type);
                }
                if (!replayThroughPe && semanticTokens) {
                    (void)semanticTokens->eraseSuperseded(
                        dxmt9::d3d9::pe::ScalarSemanticCategory::
                            TextureStageState,
                        rawSlot(stage), rawSlot(type));
                }
            });
        snapshot.samplerStates().forEach(
            [&](SamplerIndex sampler, SamplerStateType type, DWORD value) {
                recorderState_.peState.maintenance().samplerStateShadowTyped().set(sampler, type, value);
                if (replayThroughPe) {
                    recorderState_.peState.maintenance().pendingSamplerStatesTyped().set(sampler, type, value);
                    if (semanticTokens) {
                        const bool tokenRecorded = semanticTokens->record(
                            dxmt9::d3d9::pe::ScalarSemanticCategory::SamplerState,
                            rawSlot(sampler), rawSlot(type));
                        DXMT_ASSERT(tokenRecorded);
                        (void)tokenRecorded;
                    }
                } else {
                    recorderState_.peState.maintenance().pendingSamplerStatesTyped().erase(sampler, type);
                }
                if (!replayThroughPe && semanticTokens) {
                    (void)semanticTokens->eraseSuperseded(
                        dxmt9::d3d9::pe::ScalarSemanticCategory::SamplerState,
                        rawSlot(sampler), rawSlot(type));
                }
            });
        snapshot.transforms().forEach([&](TransformState key, const D9CMatrix& value) {
            recorderState_.peState.maintenance().transformShadowTyped().set(key, value);
            if (replayThroughPe) {
                recorderState_.peState.maintenance().pendingTransformsTyped().set(key, value);
            } else {
                recorderState_.peState.maintenance().pendingTransformsTyped().erase(key);
            }
        });

        // This visitor is deliberately compile-time-only binding for the
        // manually specialized keyed/constant commit code surrounding the
        // generated fixed-category visitor below. A new physical row must be
        // classified here before this PE COM TU can compile.
        snapshot.categories().forEachApplyPhysical(
            []<StateBlockApplyPhysicalStore store>(const auto&) {
                constexpr auto descriptor =
                    stateBlockApplyPhysicalDescriptor<store>();
                if constexpr (descriptor.kind ==
                              StateBlockApplyPhysicalKind::Keyed) {
                    static_assert(
                        store == StateBlockApplyPhysicalStore::renderStates ||
                        store == StateBlockApplyPhysicalStore::textureStageStates ||
                        store == StateBlockApplyPhysicalStore::samplerStates ||
                        store == StateBlockApplyPhysicalStore::transforms,
                        "StateBlock Commit keyed store is not bound");
                } else if constexpr (descriptor.kind ==
                                     StateBlockApplyPhysicalKind::Constant) {
                    static_assert(
                        store == StateBlockApplyPhysicalStore::vsConstF ||
                        store == StateBlockApplyPhysicalStore::vsConstI ||
                        store == StateBlockApplyPhysicalStore::vsConstB ||
                        store == StateBlockApplyPhysicalStore::psConstF ||
                        store == StateBlockApplyPhysicalStore::psConstI ||
                        store == StateBlockApplyPhysicalStore::psConstB,
                        "StateBlock Commit constant store is not bound");
                } else {
                    static_assert(
                        descriptor.role == StateBlockApplyCategoryRole::Value ||
                        descriptor.role == StateBlockApplyCategoryRole::ImplicitFvf ||
                        descriptor.role == StateBlockApplyCategoryRole::
                                    CandidateOwnedVertexDeclaration ||
                        descriptor.role >= StateBlockApplyCategoryRole::
                                    StagedTexture,
                        "StateBlock Commit fixed store is not bound");
                }
            });

        using Category = StateBlockRecorded::Category;
        bool commitPublicationValid = true;
        const auto validateForCommit =
            [&commitPublicationValid](auto&& validate) noexcept {
                const HRESULT hr = validate();
                DXMT_ASSERT(SUCCEEDED(hr));
                if (FAILED(hr)) commitPublicationValid = false;
                return SUCCEEDED(hr);
            };
        snapshot.categories().forEachTypedCategory(
            [&]<Category category>(const auto& values) {
                if constexpr (category == Category::textures) {
                    values.forEach([&](std::size_t slot, auto) {
                        if (textures_[slot]) textures_[slot]->Release();
                        textures_[slot] =
                            recorderState_.stateBlockTransaction.takeTexture(
                                stateBlockTextureSlotKey(slot)).raw();
                        D3D9PeValidatedTexture validated{};
                        if (!validateForCommit([&] {
                                return D3D9PeValidateTexture(
                                    textures_[slot],
                                    static_cast<IDirect3DDevice9*>(this),
                                    &validated);
                            })) return;
                        recorderState_.peBindingView.textures[slot] =
                            validated.wire();
                        if (replayThroughPe) {
                            recorderState_.peState.maintenance().pendingTextureMask() |=
                                1u << slot;
                        } else {
                            recorderState_.peState.maintenance().pendingTextureMask() &=
                                ~(1u << slot);
                        }
                    });
                } else if constexpr (category == Category::streamSources) {
                    values.forEach([&](std::size_t slot, const auto&) {
                        if (streamSrc_[slot]) streamSrc_[slot]->Release();
                        const auto staged =
                            recorderState_.stateBlockTransaction.takeStream(
                                stateBlockStreamSlotKey(slot));
                        streamSrc_[slot] = staged.buffer.raw();
                        streamOff_[slot] = staged.offset;
                        streamStr_[slot] = staged.stride;
                        D3D9PeValidatedVertexBuffer validated{};
                        if (!validateForCommit([&] {
                                return D3D9PeValidateVertexBuffer(
                                    streamSrc_[slot],
                                    static_cast<IDirect3DDevice9*>(this),
                                    &validated);
                            })) return;
                        recorderState_.peBindingView.streams[slot] = {
                            .buffer = validated.wire(),
                            .offset = staged.offset,
                            .stride = staged.stride,
                        };
                        if (replayThroughPe) {
                            recorderState_.peState.maintenance().pendingStreamMask() |=
                                1u << slot;
                        } else {
                            recorderState_.peState.maintenance().pendingStreamMask() &=
                                ~(1u << slot);
                        }
                    });
                } else if constexpr (category ==
                                     Category::streamFrequencies) {
                    values.forEach([&](std::size_t slot, std::uint32_t value) {
                        streamFreq_[slot] = value;
                        if (replayThroughPe) {
                            recorderState_.peState.maintenance().pendingStreamMask() |=
                                1u << slot;
                        }
                    });
                } else if constexpr (category == Category::vertexShader) {
                    values.forEach([&](std::size_t, auto) {
                        if (vs_) vs_->Release();
                        vs_ = recorderState_.stateBlockTransaction
                                  .takeVertexShader().raw();
                        D3D9PeValidatedVertexShader validated{};
                        if (!validateForCommit([&] {
                                return D3D9PeValidateVertexShader(
                                    vs_, static_cast<IDirect3DDevice9*>(this),
                                    &validated);
                            })) return;
                        recorderState_.peBindingView.vs = validated.wire();
                        recorderState_.peState.maintenance().pendingVs() =
                            replayThroughPe;
                    });
                } else if constexpr (category == Category::pixelShader) {
                    values.forEach([&](std::size_t, auto) {
                        if (ps_) ps_->Release();
                        ps_ = recorderState_.stateBlockTransaction
                                  .takePixelShader().raw();
                        D3D9PeValidatedPixelShader validated{};
                        if (!validateForCommit([&] {
                                return D3D9PeValidatePixelShader(
                                    ps_, static_cast<IDirect3DDevice9*>(this),
                                    &validated);
                            })) return;
                        recorderState_.peBindingView.ps = validated.wire();
                        recorderState_.peState.maintenance().pendingPs() =
                            replayThroughPe;
                    });
                } else if constexpr (category == Category::fvf) {
                    values.forEach([&](std::size_t, DWORD value) {
                        fvf_ = value;
                        recorderState_.peState.maintenance().pendingFvf() =
                            replayThroughPe;
                        recorderState_.peState.maintenance().pendingVdecl() = false;
                        if (value == 0u) {
                            vdecl_ = nullptr;
                        } else {
                            const auto it = fvfDeclCache_.find(value);
                            DXMT_ASSERT(it != fvfDeclCache_.end());
                            if (it == fvfDeclCache_.end()) {
                                commitPublicationValid = false;
                            }
                            vdecl_ = it == fvfDeclCache_.end()
                                ? nullptr : it->second;
                        }
                        D3D9PeValidatedDeclaration validated{};
                        if (!validateForCommit([&] {
                                return D3D9PeValidateVertexDecl(
                                    vdecl_, static_cast<IDirect3DDevice9*>(this),
                                    &validated);
                            })) return;
                        recorderState_.peBindingView.fvf = value;
                        recorderState_.peBindingView.vdecl = validated.wire();
                    });
                } else if constexpr (category ==
                                     Category::vertexDeclaration) {
                    values.forEach([&](std::size_t, auto value) {
                        vdecl_ = value.raw();
                        D3D9PeValidatedDeclaration validated{};
                        if (!validateForCommit([&] {
                                return D3D9PeValidateVertexDecl(
                                    vdecl_, static_cast<IDirect3DDevice9*>(this),
                                    &validated);
                            })) return;
                        recorderState_.peBindingView.vdecl = validated.wire();
                        recorderState_.peState.maintenance().pendingVdecl() =
                            replayThroughPe;
                        recorderState_.peState.maintenance().pendingFvf() = false;
                    });
                } else if constexpr (category == Category::indexBuffer) {
                    values.forEach([&](std::size_t, auto) {
                        if (indexBuf_) indexBuf_->Release();
                        indexBuf_ = recorderState_.stateBlockTransaction
                                        .takeIndexBuffer().raw();
                        D3D9PeValidatedIndexBuffer validated{};
                        if (!validateForCommit([&] {
                                return D3D9PeValidateIndexBuffer(
                                    indexBuf_,
                                    static_cast<IDirect3DDevice9*>(this),
                                    &validated);
                            })) return;
                        recorderState_.peBindingView.indexBuffer =
                            validated.wire();
                        recorderState_.peState.maintenance().pendingIb() =
                            replayThroughPe;
                    });
                } else if constexpr (category == Category::renderTargets) {
                    values.forEach([&](std::size_t slot, auto) {
                        if (rtSlots_[slot]) rtSlots_[slot]->Release();
                        rtSlots_[slot] = recorderState_.stateBlockTransaction
                            .takeRenderTarget(
                                stateBlockRenderTargetSlotKey(slot)).raw();
                        D3D9PeValidatedSurface validated{};
                        if (!validateForCommit([&] {
                                return D3D9PeValidateSurface(
                                    rtSlots_[slot],
                                    static_cast<IDirect3DDevice9*>(this),
                                    &validated);
                            })) return;
                        recorderState_.peBindingView.renderTargets[slot] =
                            validated.wire();
                        rtSlotExplicit_[slot] = true;
                        recorderState_.peBindingView.rtExplicitMask =
                            currentRtExplicitMask();
                        if (replayThroughPe) {
                            recorderState_.peState.maintenance().pendingRtMask() |=
                                1u << slot;
                        } else {
                            recorderState_.peState.maintenance().pendingRtMask() &=
                                ~(1u << slot);
                        }
                    });
                } else if constexpr (category == Category::depthStencil) {
                    values.forEach([&](std::size_t, auto) {
                        if (dsSurface_) dsSurface_->Release();
                        dsSurface_ = recorderState_.stateBlockTransaction
                                         .takeDepthStencil().raw();
                        D3D9PeValidatedSurface validated{};
                        if (!validateForCommit([&] {
                                return D3D9PeValidateSurface(
                                    dsSurface_,
                                    static_cast<IDirect3DDevice9*>(this),
                                    &validated);
                            })) return;
                        recorderState_.peBindingView.depthStencil =
                            validated.wire();
                        dsSurfaceExplicit_ = true;
                        recorderState_.peState.maintenance().pendingDs() =
                            replayThroughPe;
                    });
                } else if constexpr (category == Category::viewport) {
                    values.forEach([&](std::size_t, const D9CViewport& value) {
                        recorderState_.peState.maintenance().viewportShadow() = value;
                        recorderState_.peState.maintenance().pendingViewport() =
                            replayThroughPe;
                    });
                } else if constexpr (category == Category::scissor) {
                    values.forEach([&](std::size_t, const D9CRect& value) {
                        recorderState_.peState.maintenance().scissorShadow() = value;
                        recorderState_.peState.maintenance().pendingScissor() =
                            replayThroughPe;
                    });
                } else if constexpr (category == Category::material) {
                    values.forEach([&](std::size_t, const D9CMaterial& value) {
                        recorderState_.peState.maintenance().materialShadow() = value;
                        recorderState_.peState.maintenance().pendingMaterial() =
                            replayThroughPe;
                    });
                } else if constexpr (category == Category::clipPlanes) {
                    values.forEach([&](std::size_t idx, const auto& value) {
                        std::memcpy(recorderState_.peState.maintenance().clipPlaneShadow() + idx * 4u,
                                    value.data(), sizeof(value));
                        if (replayThroughPe) {
                            recorderState_.peState.maintenance().pendingClipPlaneMask() |=
                                1u << idx;
                        } else {
                            recorderState_.peState.maintenance().pendingClipPlaneMask() &=
                                ~(1u << idx);
                        }
                    });
                } else if constexpr (category == Category::lights) {
                    values.forEach([&](std::size_t idx,
                                       const D9CLight& value) {
                        recorderState_.peState.maintenance().lightShadow()[idx] = value;
                        if (replayThroughPe) {
                            recorderState_.peState.maintenance().pendingLightSlotMask() |=
                                1u << idx;
                        } else {
                            recorderState_.peState.maintenance().pendingLightSlotMask() &=
                                ~(1u << idx);
                        }
                    });
                } else if constexpr (category == Category::lightEnables) {
                    values.forEach([&](std::size_t idx,
                                       std::uint32_t value) {
                        const std::uint32_t bit = 1u << idx;
                        if (value) recorderState_.peState.maintenance().lightEnableShadow() |= bit;
                        else recorderState_.peState.maintenance().lightEnableShadow() &= ~bit;
                        if (replayThroughPe) {
                            recorderState_.peState.maintenance().pendingLightEnableValidMask() |= bit;
                            if (value) {
                                recorderState_.peState.maintenance().pendingLightEnableMask() |= bit;
                            } else {
                                recorderState_.peState.maintenance().pendingLightEnableMask() &= ~bit;
                            }
                        } else {
                            recorderState_.peState.maintenance().pendingLightEnableValidMask() &= ~bit;
                            recorderState_.peState.maintenance().pendingLightEnableMask() &= ~bit;
                        }
                    });
                } else {
                    []<bool handled = false>() {
                        static_assert(handled,
                                      "StateBlock Commit category omitted");
                    }();
                }
            });
        if (!commitPublicationValid) {
            poisonStateBlockTransaction();
            return;
        }
        if (snapshot.hasVdecl() && !snapshot.categories().vertexDeclaration().contains(0u)) {
            vdecl_ = snapshot.vdecl();
            D3D9PeValidatedDeclaration validated{};
            if (!validateForCommit([&] {
                    return D3D9PeValidateVertexDecl(
                        vdecl_, static_cast<IDirect3DDevice9*>(this),
                        &validated);
                })) {
                poisonStateBlockTransaction();
                return;
            }
            recorderState_.peBindingView.vdecl = validated.wire();
            recorderState_.peState.maintenance().pendingVdecl() =
                replayThroughPe;
            recorderState_.peState.maintenance().pendingFvf() = false;
        }
        publishConst(recorderState_.peConsts.vsConstF,
                     snapshot.constants().vsConstF, sizeof(float) * 4u);
        publishConst(recorderState_.peConsts.vsConstI,
                     snapshot.constants().vsConstI,
                     sizeof(std::int32_t) * 4u);
        publishConst(recorderState_.peConsts.vsConstB,
                     snapshot.constants().vsConstB, sizeof(std::uint32_t));
        publishConst(recorderState_.peConsts.psConstF,
                     snapshot.constants().psConstF, sizeof(float) * 4u);
        publishConst(recorderState_.peConsts.psConstI,
                     snapshot.constants().psConstI,
                     sizeof(std::int32_t) * 4u);
        publishConst(recorderState_.peConsts.psConstB,
                     snapshot.constants().psConstB, sizeof(std::uint32_t));
        DXMT_ASSERT(!recorderState_.stateBlockTransaction.hasPreparedApply());
        recorderState_.stateBlockTransaction.finishPreparedApply();
    }

[[nodiscard]] HRESULT D3D9DeviceImpl::validateConstRange(UINT start, UINT count,
                                                    const void* pData,
                                                    UINT maxRegisters) {
        if (count == 0) return S_OK;
        if (!pData) return D3DERR_INVALIDCALL;
        const uint64_t end = static_cast<uint64_t>(start) + count;
        if (end > maxRegisters) return D3DERR_INVALIDCALL;
        return S_OK;
    }

void D3D9DeviceImpl::readConstShadow(const ConstShadow& shadow,
                                UINT start, void* pData, UINT count,
                                std::size_t elemSize) {
        if (count == 0 || !pData) return;
        std::memset(pData, 0, count * elemSize);
        const std::size_t base = static_cast<std::size_t>(start) * elemSize;
        const std::size_t want = static_cast<std::size_t>(count) * elemSize;
        if (shadow.values.size() <= base) return;
        const std::size_t avail =
            std::min<std::size_t>(want, shadow.values.size() - base);
        std::memcpy(pData, shadow.values.data() + base, avail);
    }

HRESULT __attribute__((noinline))
    D3D9DeviceImpl::SetVertexShaderConstantFSlow(UINT start, const float* pData,
                                  UINT count) noexcept {
        if (diagnostics_->gates.callScope) {
            PeCallScope peCall(*diagnostics_, "SetVertexShaderConstantF",
                               DXMT9_PE_CALLSITE_PC());
            return SetVertexShaderConstantFSlowBody(start, pData, count,
                                                    peCall);
        }
        return SetVertexShaderConstantFSlowBody(start, pData, count,
                                                peNullCallScope_);
    }

HRESULT __attribute__((noinline))
    D3D9DeviceImpl::SetVertexShaderConstantISlow(UINT start, const INT* pData,
                                  UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? &diagnostics_->peEntryConstDecimatedStats_ : nullptr);
        notePeDeviceCallAfterPresent("SetVertexShaderConstantI");
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstIMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            recorderState_.peConsts.vsConstI, &PeStateBlockConstRecorded::vsConstI,
            start, count, pData, sizeof(int32_t) * 4);
    }

HRESULT __attribute__((noinline))
    D3D9DeviceImpl::SetVertexShaderConstantBSlow(UINT start, const BOOL* pData,
                                  UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? &diagnostics_->peEntryConstDecimatedStats_ : nullptr);
        notePeDeviceCallAfterPresent("SetVertexShaderConstantB");
        dxmt9DeviceDebugLog("device_set_vertex_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kVsConstBMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            recorderState_.peConsts.vsConstB, &PeStateBlockConstRecorded::vsConstB,
            start, count, pData, sizeof(uint32_t));
    }

HRESULT __attribute__((noinline))
    D3D9DeviceImpl::SetPixelShaderConstantFSlow(UINT start, const float* pData,
                                 UINT count) noexcept {
        if (diagnostics_->gates.callScope) {
            PeCallScope peCall(*diagnostics_, "SetPixelShaderConstantF",
                               DXMT9_PE_CALLSITE_PC());
            return SetPixelShaderConstantFSlowBody(start, pData, count,
                                                   peCall);
        }
        return SetPixelShaderConstantFSlowBody(start, pData, count,
                                               peNullCallScope_);
    }

HRESULT __attribute__((noinline))
    D3D9DeviceImpl::SetPixelShaderConstantISlow(UINT start, const INT* pData,
                                 UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? &diagnostics_->peEntryConstDecimatedStats_ : nullptr);
        notePeDeviceCallAfterPresent("SetPixelShaderConstantI");
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_i device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstIMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            recorderState_.peConsts.psConstI, &PeStateBlockConstRecorded::psConstI,
            start, count, pData, sizeof(int32_t) * 4);
    }

HRESULT __attribute__((noinline))
    D3D9DeviceImpl::SetPixelShaderConstantBSlow(UINT start, const BOOL* pData,
                                 UINT count) noexcept {
        DxmtPeDecimatedScopeGuard peEntryScope;
        dxmt9PeArmDecimatedScope(peEntryScope, diagnostics_ ? &diagnostics_->peEntryConstDecimatedStats_ : nullptr);
        notePeDeviceCallAfterPresent("SetPixelShaderConstantB");
        dxmt9DeviceDebugLog("device_set_pixel_shader_constant_b device=%p start=%u count=%u data=%p",
                            this, start, count, pData);
        const HRESULT hr = validateConstRange(start, count, pData, kPsConstBMax);
        if (FAILED(hr)) return hr;
        return applyConstStateWrite(
            recorderState_.peConsts.psConstB, &PeStateBlockConstRecorded::psConstB,
            start, count, pData, sizeof(uint32_t));
    }
