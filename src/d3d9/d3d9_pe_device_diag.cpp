/* src/d3d9/d3d9_pe_device_diag.cpp — D3D9DeviceImpl PE diagnostics.
 *
 * The PE recorder statistics, decimated-scope sampling, inter-append
 * attribution, call-scope tracking, and the in-process PE thread sampler:
 * everything behind DXMT9_PE_RECORDER_STATS, DXMT9_PE_STATS_DECIMATION,
 * DXMT9_PE_THREAD_SAMPLER, DXMT9_PE_MODULE_MAP and
 * DXMT9_PERF_VS_CONST_SETTER_RANGE. All default off.
 *
 * Split out of the class header because none of it runs in a production or
 * perf configuration -- every entry point sits behind a cached-bool gate that
 * is false. The gates themselves, and the ten diagnostic helpers the compiler
 * currently inlines into callers that stay behind (notePeDeviceCallAfterPresent
 * on every COM entry, peAppendTypeBucket in appendRecord, the two Present-
 * boundary notes, ...), deliberately remain in the header: moving those would
 * put a real call on a hot path. Which ten was decided by measuring the object,
 * not by reading the source. */

#include "d3d9_pe_device_impl.hpp"

const char* D3D9DeviceImpl::vsConstSetterRangePhaseName(
    VsConstSetterRangePhase phase) noexcept {
    switch (phase) {
    case VsConstSetterRangePhase::Call:
        return "call";
    case VsConstSetterRangePhase::Flush:
        return "flush";
    }
    return "unknown";
}

std::size_t D3D9DeviceImpl::vsConstSetterRangePhaseIndex(
    VsConstSetterRangePhase phase) noexcept {
    return phase == VsConstSetterRangePhase::Flush ? 2u : 1u;
}

void D3D9DeviceImpl::recordVsConstSetterRange(VsConstSetterRangePhase phase,
                              std::uint64_t vsHash,
                              std::uint64_t psHash,
                              std::uint32_t start,
                              std::uint32_t count,
                              std::uint32_t changedRegs,
                              std::uint32_t changedSpanRegs) noexcept {
    if (!dxmt9PerfVsConstSetterRangeEnabled() || count == 0u) {
        return;
    }
    VsConstSetterRangeBucket* bucket = nullptr;
    for (auto& candidate : vsConstSetterRangePerf_.buckets) {
        if (candidate.used) {
            if (candidate.phase == phase &&
                candidate.vsHash == vsHash &&
                candidate.psHash == psHash &&
                candidate.start == start &&
                candidate.count == count) {
                bucket = &candidate;
                break;
            }
            continue;
        }
        candidate.used = true;
        candidate.phase = phase;
        candidate.vsHash = vsHash;
        candidate.psHash = psHash;
        candidate.start = start;
        candidate.count = count;
        bucket = &candidate;
        break;
    }

    const bool fullRange = start == 0u && count >= kVsConstFMax;
    const bool fullChanged = start == 0u && changedSpanRegs >= kVsConstFMax;
    if (!bucket) {
        auto& overflow = vsConstSetterRangePerf_.overflow[
            vsConstSetterRangePhaseIndex(phase)];
        ++overflow.events;
        overflow.rangeRegs += count;
        overflow.changedRegs += changedRegs;
        overflow.changedSpanRegs += changedSpanRegs;
        overflow.fullRangeEvents += fullRange ? 1u : 0u;
        overflow.fullChangedEvents += fullChanged ? 1u : 0u;
        return;
    }

    ++bucket->events;
    bucket->rangeRegs += count;
    bucket->changedRegs += changedRegs;
    bucket->changedSpanRegs += changedSpanRegs;
    bucket->fullRangeEvents += fullRange ? 1u : 0u;
    bucket->fullChangedEvents += fullChanged ? 1u : 0u;
}

void D3D9DeviceImpl::logVsConstSetterRangePerf(const char* event) {
    if (!dxmt9PerfVsConstSetterRangeEnabled()) {
        return;
    }
    for (const auto& bucket : vsConstSetterRangePerf_.buckets) {
        if (!bucket.used || bucket.events == 0u) {
            continue;
        }
        dxmt9PerfLogStderrAtomic(
            "[dxmt9-perf-vs-const-setter-range event=%s overflow=0 "
            "phase=%s vs_hash=0x%llx ps_hash=0x%llx "
            "start=%u count=%u events=%llu range_regs=%llu "
            "changed_regs=%llu changed_span_regs=%llu "
            "full_range_events=%llu full_changed_events=%llu]\n",
            event ? event : "unknown",
            vsConstSetterRangePhaseName(bucket.phase),
            static_cast<unsigned long long>(bucket.vsHash),
            static_cast<unsigned long long>(bucket.psHash),
            bucket.start, bucket.count,
            static_cast<unsigned long long>(bucket.events),
            static_cast<unsigned long long>(bucket.rangeRegs),
            static_cast<unsigned long long>(bucket.changedRegs),
            static_cast<unsigned long long>(bucket.changedSpanRegs),
            static_cast<unsigned long long>(bucket.fullRangeEvents),
            static_cast<unsigned long long>(bucket.fullChangedEvents));
    }
    for (std::size_t phaseIndex = 1u;
         phaseIndex < vsConstSetterRangePerf_.overflow.size();
         ++phaseIndex) {
        const auto& overflow = vsConstSetterRangePerf_.overflow[phaseIndex];
        if (overflow.events == 0u) {
            continue;
        }
        const auto phase = phaseIndex == 2u
            ? VsConstSetterRangePhase::Flush
            : VsConstSetterRangePhase::Call;
        dxmt9PerfLogStderrAtomic(
            "[dxmt9-perf-vs-const-setter-range event=%s overflow=1 "
            "phase=%s events=%llu range_regs=%llu changed_regs=%llu "
            "changed_span_regs=%llu full_range_events=%llu "
            "full_changed_events=%llu]\n",
            event ? event : "unknown",
            vsConstSetterRangePhaseName(phase),
            static_cast<unsigned long long>(overflow.events),
            static_cast<unsigned long long>(overflow.rangeRegs),
            static_cast<unsigned long long>(overflow.changedRegs),
            static_cast<unsigned long long>(overflow.changedSpanRegs),
            static_cast<unsigned long long>(overflow.fullRangeEvents),
            static_cast<unsigned long long>(overflow.fullChangedEvents));
    }
    std::fflush(stderr);
    vsConstSetterRangePerf_ = VsConstSetterRangePerf{};
}

void D3D9DeviceImpl::logPeFirstCallAfterPresent(const char* callName,
                                const PePresentCadenceClaim& claim,
                                const PePresentCallSample& sample) {
    if (!claim.claimed) {
        return;
    }
    const std::int64_t observedNs =
        dxmt9SteadyClockNs(std::chrono::steady_clock::now());
    const auto callerInfo = dxmt9PeResolveCallerModule(sample.callerPc);
    const auto callerStack = dxmt9PeFormatCallerStack(sample);
    dxmt9DeviceInfoLog(
        "pe_present_next_call device=%p ordinal=%llu call=%s "
        "thread_id=0x%lx "
        "entry_delta_ms=%.3f observed_delta_ms=%.3f "
        "observed_wait_ms=%.3f caller_pc=%p caller_module=%s "
        "caller_base=%p caller_rva=0x%llx caller_stack=%s",
        this, static_cast<unsigned long long>(claim.ordinal),
        callName ? callName : "unknown",
        static_cast<unsigned long>(sample.threadId),
        static_cast<double>(claim.entryNs - claim.returnNs) / 1000000.0,
        static_cast<double>(observedNs - claim.returnNs) / 1000000.0,
        static_cast<double>(observedNs - claim.entryNs) / 1000000.0,
        sample.callerPc, dxmt9PeCallerModuleLeaf(callerInfo),
        callerInfo.base, static_cast<unsigned long long>(callerInfo.rva),
        callerStack.data());
}

void D3D9DeviceImpl::notePeDeviceCallAfterPresentTracked(const char* callName,
                                         const void* callerPc,
                                         PePresentCallSample& out) {
    dxmt9PeSetCurrentCallName(callName);
    dxmt9PeCurrentCallEntryNs =
        dxmt9SteadyClockNs(std::chrono::steady_clock::now());
    recordPeBetweenCallsEntry(callName, dxmt9PeCurrentCallEntryNs,
                              callerPc);
    out = logPeCallMilestoneAfterPresent(callName, callerPc);
    if (!out.tracked) {
        out = PePresentCallSample{};
        out.entryNs = dxmt9PeCurrentCallEntryNs;
        out.callerPc = callerPc;
        out.threadId = dxmt9PeCurrentThreadId();
        dxmt9PeCaptureCallStack(out);
    }
    logPeFirstCallAfterPresent(callName, claimPeFirstCallAfterPresent(),
                               out);
}

bool D3D9DeviceImpl::peCallMilestoneBit(std::uint32_t callCount,
                               std::uint32_t& bit) noexcept {
    switch (callCount) {
    case 1: bit = 1u << 0; return true;
    case 2: bit = 1u << 1; return true;
    case 3: bit = 1u << 2; return true;
    case 4: bit = 1u << 3; return true;
    case 5: bit = 1u << 4; return true;
    case 6: bit = 1u << 5; return true;
    case 7: bit = 1u << 6; return true;
    case 8: bit = 1u << 7; return true;
    case 16: bit = 1u << 8; return true;
    case 32: bit = 1u << 9; return true;
    case 64: bit = 1u << 10; return true;
    default:
        bit = 0;
        return false;
    }
}

D3D9DeviceImpl::PePresentCallSample D3D9DeviceImpl::logPeCallMilestoneAfterPresent(const char* callName,
                                                   const void* callerPc) {
    if (!dxmt9PeRecorderStatsEnabled()) {
        return {};
    }
    const std::uint64_t ordinal =
        pePresentCallMilestonePendingOrdinal_.load(std::memory_order_acquire);
    if (ordinal == 0) {
        return {};
    }
    const auto entry = std::chrono::steady_clock::now();
    const std::int64_t entryNs = dxmt9SteadyClockNs(entry);
    const std::uint32_t callCount =
        pePresentCallCount_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const std::int64_t returnNs =
        pePresentCadenceReturnNs_.load(std::memory_order_acquire);
    std::uint32_t milestoneBit = 0;
    const bool milestone = peCallMilestoneBit(callCount, milestoneBit);
    PePresentCallSample sample{
        true, ordinal, callCount, returnNs, entryNs, callerPc};
    sample.threadId = dxmt9PeCurrentThreadId();
    if (callCount <= 8 || milestone) {
        dxmt9PeCaptureCallStack(sample);
    }
    if (!milestone) {
        return sample;
    }
    std::uint32_t mask =
        pePresentCallMilestoneMask_.load(std::memory_order_acquire);
    while ((mask & milestoneBit) == 0) {
        if (pePresentCallMilestoneMask_.compare_exchange_weak(
                mask, mask | milestoneBit, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            const auto callerInfo = dxmt9PeResolveCallerModule(callerPc);
            const auto callerStack = dxmt9PeFormatCallerStack(sample);
            dxmt9DeviceInfoLog(
                "pe_present_call_milestone device=%p ordinal=%llu "
                "milestone=%u call=%s thread_id=0x%lx "
                "entry_delta_ms=%.3f caller_pc=%p "
                "caller_module=%s caller_base=%p caller_rva=0x%llx "
                "caller_stack=%s",
                this, static_cast<unsigned long long>(ordinal), callCount,
                callName ? callName : "unknown",
                static_cast<unsigned long>(sample.threadId),
                static_cast<double>(entryNs - returnNs) / 1000000.0,
                callerPc, dxmt9PeCallerModuleLeaf(callerInfo),
                callerInfo.base,
                static_cast<unsigned long long>(callerInfo.rva),
                callerStack.data());
            return sample;
        }
    }
    return sample;
}

void D3D9DeviceImpl::logPeCallReturnAfterPresent(const PePresentCallSample& sample,
                                 const char* callName,
                                 HRESULT hr) {
    if (!dxmt9PeRecorderStatsEnabled()) {
        return;
    }
    const std::int64_t exitNs =
        dxmt9SteadyClockNs(std::chrono::steady_clock::now());
    if (!sample.tracked) {
        if (hr != D3DERR_INVALIDCALL) {
            return;
        }
        if (sample.entryNs != 0) {
            recordPeBetweenCallsReturn(callName, sample.entryNs, exitNs);
        }
        const auto callerInfo = dxmt9PeResolveCallerModule(sample.callerPc);
        const auto callerStack = dxmt9PeFormatCallerStack(sample);
        dxmt9DeviceInfoLog(
            "pe_call_return_untracked_failure device=%p call=%s "
            "thread_id=0x%lx hr=0x%08x duration_ms=%.3f "
            "caller_pc=%p caller_module=%s caller_base=%p "
            "caller_rva=0x%llx caller_stack=%s",
            this, callName ? callName : "unknown",
            static_cast<unsigned long>(sample.threadId),
            static_cast<unsigned>(hr),
            sample.entryNs != 0
                ? static_cast<double>(exitNs - sample.entryNs) / 1000000.0
                : 0.0,
            sample.callerPc, dxmt9PeCallerModuleLeaf(callerInfo),
            callerInfo.base,
            static_cast<unsigned long long>(callerInfo.rva),
            callerStack.data());
        return;
    }
    recordPeBetweenCallsReturn(callName, sample.entryNs, exitNs);
    if (sample.callCount > 8 && SUCCEEDED(hr)) {
        return;
    }
    const auto callerInfo = dxmt9PeResolveCallerModule(sample.callerPc);
    const auto callerStack = dxmt9PeFormatCallerStack(sample);
    dxmt9DeviceInfoLog(
        "pe_present_call_return device=%p ordinal=%llu milestone=%u "
        "call=%s thread_id=0x%lx hr=0x%08x "
        "return_delta_ms=%.3f duration_ms=%.3f "
        "caller_pc=%p caller_module=%s caller_base=%p caller_rva=0x%llx "
        "caller_stack=%s",
        this, static_cast<unsigned long long>(sample.ordinal),
        sample.callCount, callName ? callName : "unknown",
        static_cast<unsigned long>(sample.threadId),
        static_cast<unsigned>(hr),
        static_cast<double>(exitNs - sample.returnNs) / 1000000.0,
        static_cast<double>(exitNs - sample.entryNs) / 1000000.0,
        sample.callerPc, dxmt9PeCallerModuleLeaf(callerInfo),
        callerInfo.base,
        static_cast<unsigned long long>(callerInfo.rva),
        callerStack.data());
}

const char* D3D9DeviceImpl::peCommandRecordTypeName(std::uint32_t type) noexcept {
    switch (type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
        return "draw";
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
        return "draw_indexed";
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
        return "draw_up";
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
        return "draw_indexed_up";
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
        return "set_vs_const_f";
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
        return "set_vs_const_i";
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
        return "set_vs_const_b";
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
        return "set_ps_const_f";
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
        return "set_ps_const_i";
    case D9C_COMMAND_RECORD_SET_PS_CONST_B:
        return "set_ps_const_b";
    case D9C_COMMAND_RECORD_CLEAR:
        return "clear";
    case D9C_COMMAND_RECORD_PRESENT:
        return "present";
    case D9C_COMMAND_RECORD_STRETCH_RECT:
        return "stretch_rect";
    case D9C_COMMAND_RECORD_COLOR_FILL:
        return "color_fill";
    case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
        return "update_texture";
    case D9C_COMMAND_RECORD_UPDATE_SURFACE:
        return "update_surface";
    case D9C_COMMAND_RECORD_QUERY_ISSUE:
        return "query_issue";
    case D9C_COMMAND_RECORD_READBACK:
        return "readback";
    case D9C_COMMAND_RECORD_APPLY_STATE:
        return "apply_state";
    case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE:
        return "resz_depth_resolve";
    default:
        return "unknown";
    }
}

std::uint32_t D3D9DeviceImpl::peCommandRecordTypeBucket(std::uint32_t type) noexcept {
    return type < kPeCommandRecordTypeBucketCount ? type : 0u;
}

const char*
D3D9DeviceImpl::peInterAppendCallFamilyName(std::uint32_t family) noexcept {
    switch (static_cast<PeInterAppendCallFamily>(family)) {
    case PeInterAppendCallFamily::Unknown: return "unknown";
    case PeInterAppendCallFamily::RenderTarget: return "render_target";
    case PeInterAppendCallFamily::DepthStencil: return "depth_stencil";
    case PeInterAppendCallFamily::ViewportScissor:
        return "viewport_scissor";
    case PeInterAppendCallFamily::Transform: return "transform";
    case PeInterAppendCallFamily::MaterialLightClip:
        return "material_light_clip";
    case PeInterAppendCallFamily::RenderState: return "render_state";
    case PeInterAppendCallFamily::TextureStageSampler:
        return "tss_sampler";
    case PeInterAppendCallFamily::Texture: return "texture";
    case PeInterAppendCallFamily::VertexInput: return "vertex_input";
    case PeInterAppendCallFamily::Shader: return "shader";
    case PeInterAppendCallFamily::VsConst: return "vs_const";
    case PeInterAppendCallFamily::PsConst: return "ps_const";
    case PeInterAppendCallFamily::Draw: return "draw";
    case PeInterAppendCallFamily::Barrier: return "barrier";
    case PeInterAppendCallFamily::ScenePresent: return "scene_present";
    case PeInterAppendCallFamily::Resource: return "resource";
    case PeInterAppendCallFamily::Count: break;
    }
    return "unknown";
}

PeInterAppendCallFamily
D3D9DeviceImpl::peInterAppendCallFamilyFromName(const char* callName) noexcept {
    if (!callName) {
        return PeInterAppendCallFamily::Unknown;
    }
    if (std::strcmp(callName, "SetRenderTarget") == 0 ||
        std::strcmp(callName, "GetRenderTarget") == 0) {
        return PeInterAppendCallFamily::RenderTarget;
    }
    if (std::strcmp(callName, "SetDepthStencilSurface") == 0 ||
        std::strcmp(callName, "GetDepthStencilSurface") == 0) {
        return PeInterAppendCallFamily::DepthStencil;
    }
    if (std::strcmp(callName, "SetViewport") == 0 ||
        std::strcmp(callName, "GetViewport") == 0 ||
        std::strcmp(callName, "SetScissorRect") == 0 ||
        std::strcmp(callName, "GetScissorRect") == 0) {
        return PeInterAppendCallFamily::ViewportScissor;
    }
    if (std::strcmp(callName, "SetTransform") == 0 ||
        std::strcmp(callName, "GetTransform") == 0 ||
        std::strcmp(callName, "MultiplyTransform") == 0) {
        return PeInterAppendCallFamily::Transform;
    }
    if (std::strcmp(callName, "SetMaterial") == 0 ||
        std::strcmp(callName, "GetMaterial") == 0 ||
        std::strcmp(callName, "SetLight") == 0 ||
        std::strcmp(callName, "GetLight") == 0 ||
        std::strcmp(callName, "LightEnable") == 0 ||
        std::strcmp(callName, "GetLightEnable") == 0 ||
        std::strcmp(callName, "SetClipPlane") == 0 ||
        std::strcmp(callName, "GetClipPlane") == 0 ||
        std::strcmp(callName, "SetClipStatus") == 0 ||
        std::strcmp(callName, "GetClipStatus") == 0) {
        return PeInterAppendCallFamily::MaterialLightClip;
    }
    if (std::strcmp(callName, "SetRenderState") == 0 ||
        std::strcmp(callName, "GetRenderState") == 0) {
        return PeInterAppendCallFamily::RenderState;
    }
    if (std::strcmp(callName, "SetTextureStageState") == 0 ||
        std::strcmp(callName, "GetTextureStageState") == 0 ||
        std::strcmp(callName, "SetSamplerState") == 0 ||
        std::strcmp(callName, "GetSamplerState") == 0) {
        return PeInterAppendCallFamily::TextureStageSampler;
    }
    if (std::strcmp(callName, "SetTexture") == 0 ||
        std::strcmp(callName, "GetTexture") == 0) {
        return PeInterAppendCallFamily::Texture;
    }
    if (std::strcmp(callName, "SetFVF") == 0 ||
        std::strcmp(callName, "GetFVF") == 0 ||
        std::strcmp(callName, "SetVertexDeclaration") == 0 ||
        std::strcmp(callName, "GetVertexDeclaration") == 0 ||
        std::strcmp(callName, "SetStreamSource") == 0 ||
        std::strcmp(callName, "GetStreamSource") == 0 ||
        std::strcmp(callName, "SetStreamSourceFreq") == 0 ||
        std::strcmp(callName, "GetStreamSourceFreq") == 0 ||
        std::strcmp(callName, "SetIndices") == 0 ||
        std::strcmp(callName, "GetIndices") == 0) {
        return PeInterAppendCallFamily::VertexInput;
    }
    if (std::strcmp(callName, "SetVertexShader") == 0 ||
        std::strcmp(callName, "GetVertexShader") == 0 ||
        std::strcmp(callName, "SetPixelShader") == 0 ||
        std::strcmp(callName, "GetPixelShader") == 0) {
        return PeInterAppendCallFamily::Shader;
    }
    if (std::strcmp(callName, "SetVertexShaderConstantF") == 0 ||
        std::strcmp(callName, "GetVertexShaderConstantF") == 0 ||
        std::strcmp(callName, "SetVertexShaderConstantI") == 0 ||
        std::strcmp(callName, "GetVertexShaderConstantI") == 0 ||
        std::strcmp(callName, "SetVertexShaderConstantB") == 0 ||
        std::strcmp(callName, "GetVertexShaderConstantB") == 0) {
        return PeInterAppendCallFamily::VsConst;
    }
    if (std::strcmp(callName, "SetPixelShaderConstantF") == 0 ||
        std::strcmp(callName, "GetPixelShaderConstantF") == 0 ||
        std::strcmp(callName, "SetPixelShaderConstantI") == 0 ||
        std::strcmp(callName, "GetPixelShaderConstantI") == 0 ||
        std::strcmp(callName, "SetPixelShaderConstantB") == 0 ||
        std::strcmp(callName, "GetPixelShaderConstantB") == 0) {
        return PeInterAppendCallFamily::PsConst;
    }
    if (std::strcmp(callName, "DrawPrimitive") == 0 ||
        std::strcmp(callName, "DrawIndexedPrimitive") == 0 ||
        std::strcmp(callName, "DrawPrimitiveUP") == 0 ||
        std::strcmp(callName, "DrawIndexedPrimitiveUP") == 0) {
        return PeInterAppendCallFamily::Draw;
    }
    if (std::strcmp(callName, "Clear") == 0 ||
        std::strcmp(callName, "StretchRect") == 0 ||
        std::strcmp(callName, "ColorFill") == 0 ||
        std::strcmp(callName, "UpdateTexture") == 0 ||
        std::strcmp(callName, "UpdateSurface") == 0 ||
        std::strcmp(callName, "ProcessVertices") == 0) {
        return PeInterAppendCallFamily::Barrier;
    }
    if (std::strcmp(callName, "BeginScene") == 0 ||
        std::strcmp(callName, "EndScene") == 0 ||
        std::strcmp(callName, "Present") == 0 ||
        std::strcmp(callName, "PresentEx") == 0 ||
        std::strcmp(callName, "Reset") == 0 ||
        std::strcmp(callName, "ResetEx") == 0) {
        return PeInterAppendCallFamily::ScenePresent;
    }
    if (std::strncmp(callName, "Create", 6) == 0 ||
        std::strncmp(callName, "Get", 3) == 0 ||
        std::strcmp(callName, "ValidateDevice") == 0 ||
        std::strcmp(callName, "SetPaletteEntries") == 0 ||
        std::strcmp(callName, "GetPaletteEntries") == 0 ||
        std::strcmp(callName, "SetCurrentTexturePalette") == 0 ||
        std::strcmp(callName, "GetCurrentTexturePalette") == 0) {
        return PeInterAppendCallFamily::Resource;
    }
    return PeInterAppendCallFamily::Unknown;
}

const char*
D3D9DeviceImpl::peInterAppendCallNameName(std::uint32_t callName) noexcept {
    switch (static_cast<PeInterAppendCallName>(callName)) {
    case PeInterAppendCallName::Unknown: return "unknown";
    case PeInterAppendCallName::BeginScene: return "BeginScene";
    case PeInterAppendCallName::EndScene: return "EndScene";
    case PeInterAppendCallName::Clear: return "Clear";
    case PeInterAppendCallName::SetRenderTarget: return "SetRenderTarget";
    case PeInterAppendCallName::GetRenderTarget: return "GetRenderTarget";
    case PeInterAppendCallName::SetDepthStencilSurface:
        return "SetDepthStencilSurface";
    case PeInterAppendCallName::GetDepthStencilSurface:
        return "GetDepthStencilSurface";
    case PeInterAppendCallName::SetViewport: return "SetViewport";
    case PeInterAppendCallName::GetViewport: return "GetViewport";
    case PeInterAppendCallName::SetScissorRect: return "SetScissorRect";
    case PeInterAppendCallName::GetScissorRect: return "GetScissorRect";
    case PeInterAppendCallName::SetRenderState: return "SetRenderState";
    case PeInterAppendCallName::SetTextureStageState:
        return "SetTextureStageState";
    case PeInterAppendCallName::SetSamplerState: return "SetSamplerState";
    case PeInterAppendCallName::SetTexture: return "SetTexture";
    case PeInterAppendCallName::SetFVF: return "SetFVF";
    case PeInterAppendCallName::SetVertexDeclaration:
        return "SetVertexDeclaration";
    case PeInterAppendCallName::SetStreamSource:
        return "SetStreamSource";
    case PeInterAppendCallName::SetStreamSourceFreq:
        return "SetStreamSourceFreq";
    case PeInterAppendCallName::SetIndices: return "SetIndices";
    case PeInterAppendCallName::SetVertexShader:
        return "SetVertexShader";
    case PeInterAppendCallName::SetPixelShader: return "SetPixelShader";
    case PeInterAppendCallName::SetVertexShaderConstantF:
        return "SetVertexShaderConstantF";
    case PeInterAppendCallName::SetVertexShaderConstantI:
        return "SetVertexShaderConstantI";
    case PeInterAppendCallName::SetVertexShaderConstantB:
        return "SetVertexShaderConstantB";
    case PeInterAppendCallName::SetPixelShaderConstantF:
        return "SetPixelShaderConstantF";
    case PeInterAppendCallName::SetPixelShaderConstantI:
        return "SetPixelShaderConstantI";
    case PeInterAppendCallName::SetPixelShaderConstantB:
        return "SetPixelShaderConstantB";
    case PeInterAppendCallName::DrawPrimitive: return "DrawPrimitive";
    case PeInterAppendCallName::DrawIndexedPrimitive:
        return "DrawIndexedPrimitive";
    case PeInterAppendCallName::DrawPrimitiveUP:
        return "DrawPrimitiveUP";
    case PeInterAppendCallName::DrawIndexedPrimitiveUP:
        return "DrawIndexedPrimitiveUP";
    case PeInterAppendCallName::ProcessVertices: return "ProcessVertices";
    case PeInterAppendCallName::GetBackBuffer: return "GetBackBuffer";
    case PeInterAppendCallName::GetSwapChain: return "GetSwapChain";
    case PeInterAppendCallName::GetRasterStatus: return "GetRasterStatus";
    case PeInterAppendCallName::ValidateDevice: return "ValidateDevice";
    case PeInterAppendCallName::SetSoftwareVertexProcessing:
        return "SetSoftwareVertexProcessing";
    case PeInterAppendCallName::SetNPatchMode: return "SetNPatchMode";
    case PeInterAppendCallName::SurfaceGetDesc:
        return "Surface::GetDesc";
    case PeInterAppendCallName::SurfaceLockRect:
        return "Surface::LockRect";
    case PeInterAppendCallName::TextureGetSurfaceLevel:
        return "Texture::GetSurfaceLevel";
    case PeInterAppendCallName::CubeTextureGetCubeMapSurface:
        return "CubeTexture::GetCubeMapSurface";
    case PeInterAppendCallName::VertexBufferLock:
        return "VertexBuffer::Lock";
    case PeInterAppendCallName::VertexBufferGetDesc:
        return "VertexBuffer::GetDesc";
    case PeInterAppendCallName::IndexBufferLock:
        return "IndexBuffer::Lock";
    case PeInterAppendCallName::IndexBufferGetDesc:
        return "IndexBuffer::GetDesc";
    case PeInterAppendCallName::QueryIssue: return "Query::Issue";
    case PeInterAppendCallName::QueryGetData: return "Query::GetData";
    case PeInterAppendCallName::StateBlockCapture:
        return "StateBlock::Capture";
    case PeInterAppendCallName::StateBlockApply:
        return "StateBlock::Apply";
    case PeInterAppendCallName::OtherChild: return "other_child";
    case PeInterAppendCallName::OtherGet: return "other_get";
    case PeInterAppendCallName::OtherSet: return "other_set";
    case PeInterAppendCallName::OtherCreate: return "other_create";
    case PeInterAppendCallName::Other: return "other";
    case PeInterAppendCallName::Count: break;
    }
    return "unknown";
}

PeInterAppendCallName
D3D9DeviceImpl::peInterAppendCallNameFromName(const char* callName) noexcept {
    if (!callName) {
        return PeInterAppendCallName::Unknown;
    }
    if (std::strcmp(callName, "BeginScene") == 0) {
        return PeInterAppendCallName::BeginScene;
    }
    if (std::strcmp(callName, "EndScene") == 0) {
        return PeInterAppendCallName::EndScene;
    }
    if (std::strcmp(callName, "Clear") == 0) {
        return PeInterAppendCallName::Clear;
    }
    if (std::strcmp(callName, "SetRenderTarget") == 0) {
        return PeInterAppendCallName::SetRenderTarget;
    }
    if (std::strcmp(callName, "GetRenderTarget") == 0) {
        return PeInterAppendCallName::GetRenderTarget;
    }
    if (std::strcmp(callName, "SetDepthStencilSurface") == 0) {
        return PeInterAppendCallName::SetDepthStencilSurface;
    }
    if (std::strcmp(callName, "GetDepthStencilSurface") == 0) {
        return PeInterAppendCallName::GetDepthStencilSurface;
    }
    if (std::strcmp(callName, "SetViewport") == 0) {
        return PeInterAppendCallName::SetViewport;
    }
    if (std::strcmp(callName, "GetViewport") == 0) {
        return PeInterAppendCallName::GetViewport;
    }
    if (std::strcmp(callName, "SetScissorRect") == 0) {
        return PeInterAppendCallName::SetScissorRect;
    }
    if (std::strcmp(callName, "GetScissorRect") == 0) {
        return PeInterAppendCallName::GetScissorRect;
    }
    if (std::strcmp(callName, "SetRenderState") == 0) {
        return PeInterAppendCallName::SetRenderState;
    }
    if (std::strcmp(callName, "SetTextureStageState") == 0) {
        return PeInterAppendCallName::SetTextureStageState;
    }
    if (std::strcmp(callName, "SetSamplerState") == 0) {
        return PeInterAppendCallName::SetSamplerState;
    }
    if (std::strcmp(callName, "SetTexture") == 0) {
        return PeInterAppendCallName::SetTexture;
    }
    if (std::strcmp(callName, "SetFVF") == 0) {
        return PeInterAppendCallName::SetFVF;
    }
    if (std::strcmp(callName, "SetVertexDeclaration") == 0) {
        return PeInterAppendCallName::SetVertexDeclaration;
    }
    if (std::strcmp(callName, "SetStreamSource") == 0) {
        return PeInterAppendCallName::SetStreamSource;
    }
    if (std::strcmp(callName, "SetStreamSourceFreq") == 0) {
        return PeInterAppendCallName::SetStreamSourceFreq;
    }
    if (std::strcmp(callName, "SetIndices") == 0) {
        return PeInterAppendCallName::SetIndices;
    }
    if (std::strcmp(callName, "SetVertexShader") == 0) {
        return PeInterAppendCallName::SetVertexShader;
    }
    if (std::strcmp(callName, "SetPixelShader") == 0) {
        return PeInterAppendCallName::SetPixelShader;
    }
    if (std::strcmp(callName, "SetVertexShaderConstantF") == 0) {
        return PeInterAppendCallName::SetVertexShaderConstantF;
    }
    if (std::strcmp(callName, "SetVertexShaderConstantI") == 0) {
        return PeInterAppendCallName::SetVertexShaderConstantI;
    }
    if (std::strcmp(callName, "SetVertexShaderConstantB") == 0) {
        return PeInterAppendCallName::SetVertexShaderConstantB;
    }
    if (std::strcmp(callName, "SetPixelShaderConstantF") == 0) {
        return PeInterAppendCallName::SetPixelShaderConstantF;
    }
    if (std::strcmp(callName, "SetPixelShaderConstantI") == 0) {
        return PeInterAppendCallName::SetPixelShaderConstantI;
    }
    if (std::strcmp(callName, "SetPixelShaderConstantB") == 0) {
        return PeInterAppendCallName::SetPixelShaderConstantB;
    }
    if (std::strcmp(callName, "DrawPrimitive") == 0) {
        return PeInterAppendCallName::DrawPrimitive;
    }
    if (std::strcmp(callName, "DrawIndexedPrimitive") == 0) {
        return PeInterAppendCallName::DrawIndexedPrimitive;
    }
    if (std::strcmp(callName, "DrawPrimitiveUP") == 0) {
        return PeInterAppendCallName::DrawPrimitiveUP;
    }
    if (std::strcmp(callName, "DrawIndexedPrimitiveUP") == 0) {
        return PeInterAppendCallName::DrawIndexedPrimitiveUP;
    }
    if (std::strcmp(callName, "ProcessVertices") == 0) {
        return PeInterAppendCallName::ProcessVertices;
    }
    if (std::strcmp(callName, "GetBackBuffer") == 0) {
        return PeInterAppendCallName::GetBackBuffer;
    }
    if (std::strcmp(callName, "GetSwapChain") == 0) {
        return PeInterAppendCallName::GetSwapChain;
    }
    if (std::strcmp(callName, "GetRasterStatus") == 0) {
        return PeInterAppendCallName::GetRasterStatus;
    }
    if (std::strcmp(callName, "ValidateDevice") == 0) {
        return PeInterAppendCallName::ValidateDevice;
    }
    if (std::strcmp(callName, "SetSoftwareVertexProcessing") == 0) {
        return PeInterAppendCallName::SetSoftwareVertexProcessing;
    }
    if (std::strcmp(callName, "SetNPatchMode") == 0) {
        return PeInterAppendCallName::SetNPatchMode;
    }
    if (std::strcmp(callName, "Surface::GetDesc") == 0) {
        return PeInterAppendCallName::SurfaceGetDesc;
    }
    if (std::strcmp(callName, "Surface::LockRect") == 0) {
        return PeInterAppendCallName::SurfaceLockRect;
    }
    if (std::strcmp(callName, "Texture::GetSurfaceLevel") == 0) {
        return PeInterAppendCallName::TextureGetSurfaceLevel;
    }
    if (std::strcmp(callName, "CubeTexture::GetCubeMapSurface") == 0) {
        return PeInterAppendCallName::CubeTextureGetCubeMapSurface;
    }
    if (std::strcmp(callName, "VertexBuffer::Lock") == 0) {
        return PeInterAppendCallName::VertexBufferLock;
    }
    if (std::strcmp(callName, "VertexBuffer::GetDesc") == 0) {
        return PeInterAppendCallName::VertexBufferGetDesc;
    }
    if (std::strcmp(callName, "IndexBuffer::Lock") == 0) {
        return PeInterAppendCallName::IndexBufferLock;
    }
    if (std::strcmp(callName, "IndexBuffer::GetDesc") == 0) {
        return PeInterAppendCallName::IndexBufferGetDesc;
    }
    if (std::strcmp(callName, "Query::Issue") == 0) {
        return PeInterAppendCallName::QueryIssue;
    }
    if (std::strcmp(callName, "Query::GetData") == 0) {
        return PeInterAppendCallName::QueryGetData;
    }
    if (std::strcmp(callName, "StateBlock::Capture") == 0) {
        return PeInterAppendCallName::StateBlockCapture;
    }
    if (std::strcmp(callName, "StateBlock::Apply") == 0) {
        return PeInterAppendCallName::StateBlockApply;
    }
    if (std::strstr(callName, "::") != nullptr) {
        return PeInterAppendCallName::OtherChild;
    }
    if (std::strncmp(callName, "Get", 3) == 0) {
        return PeInterAppendCallName::OtherGet;
    }
    if (std::strncmp(callName, "Set", 3) == 0) {
        return PeInterAppendCallName::OtherSet;
    }
    if (std::strncmp(callName, "Create", 6) == 0) {
        return PeInterAppendCallName::OtherCreate;
    }
    return PeInterAppendCallName::Other;
}

std::size_t D3D9DeviceImpl::peInterAppendFocusPairIndex(std::uint32_t prevType,
                                               std::uint32_t nextType) noexcept {
    if (prevType != D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
        return kPeInterAppendFocusPairCount;
    }
    switch (nextType) {
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
        return static_cast<std::size_t>(
            PeInterAppendFocusPair::DrawIndexedToVsConstF);
    case D9C_COMMAND_RECORD_APPLY_STATE:
        return static_cast<std::size_t>(
            PeInterAppendFocusPair::DrawIndexedToApplyState);
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
        return static_cast<std::size_t>(
            PeInterAppendFocusPair::DrawIndexedToDrawIndexed);
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
        return static_cast<std::size_t>(
            PeInterAppendFocusPair::DrawIndexedToPsConstF);
    default:
        return kPeInterAppendFocusPairCount;
    }
}

std::size_t D3D9DeviceImpl::peInterAppendFocusCallFamilyIndex(
    std::size_t focusPair,
    PeInterAppendCallFamily family) noexcept {
    return focusPair * kPeInterAppendCallFamilyCount +
           static_cast<std::size_t>(family);
}

std::size_t D3D9DeviceImpl::peInterAppendFocusCallTransitionIndex(
    std::size_t focusPair,
    PeInterAppendCallFamily prevFamily,
    PeInterAppendCallFamily nextFamily) noexcept {
    return (focusPair * kPeInterAppendCallFamilyCount +
            static_cast<std::size_t>(prevFamily)) *
               kPeInterAppendCallFamilyCount +
           static_cast<std::size_t>(nextFamily);
}

std::size_t D3D9DeviceImpl::peInterAppendFocusCallNameTransitionIndex(
    std::size_t focusPair,
    PeInterAppendCallName prevCallName,
    PeInterAppendCallName nextCallName) noexcept {
    return (focusPair * kPeInterAppendCallNameCount +
            static_cast<std::size_t>(prevCallName)) *
               kPeInterAppendCallNameCount +
           static_cast<std::size_t>(nextCallName);
}

std::size_t D3D9DeviceImpl::peInterAppendCallTransitionIndex(
    PeInterAppendCallFamily prevFamily,
    PeInterAppendCallFamily nextFamily) noexcept {
    return static_cast<std::size_t>(prevFamily) *
               kPeInterAppendCallFamilyCount +
           static_cast<std::size_t>(nextFamily);
}

std::size_t D3D9DeviceImpl::peInterAppendCallNameTransitionIndex(
    PeInterAppendCallName prevCallName,
    PeInterAppendCallName nextCallName) noexcept {
    return static_cast<std::size_t>(prevCallName) *
               kPeInterAppendCallNameCount +
           static_cast<std::size_t>(nextCallName);
}

std::size_t D3D9DeviceImpl::peInterAppendPairIndex(std::uint32_t prevType,
                                          std::uint32_t nextType) noexcept {
    return static_cast<std::size_t>(
               peCommandRecordTypeBucket(prevType)) *
               kPeCommandRecordTypeBucketCount +
           peCommandRecordTypeBucket(nextType);
}

std::array<D3D9DeviceImpl::PeInterAppendPairSummary, kPeRecorderInterAppendTopPairCount>
D3D9DeviceImpl::topPeInterAppendPairs() const noexcept {
    std::array<PeInterAppendPairSummary, kPeRecorderInterAppendTopPairCount>
        top{};
    for (std::size_t prev = 0; prev < kPeCommandRecordTypeBucketCount;
         ++prev) {
        for (std::size_t next = 0; next < kPeCommandRecordTypeBucketCount;
             ++next) {
            const std::size_t index =
                prev * kPeCommandRecordTypeBucketCount + next;
            const std::uint64_t totalNs =
                peRecorderStats_.chunkInterAppendPairNsTotal[index];
            if (totalNs == 0) {
                continue;
            }
            PeInterAppendPairSummary candidate{
                static_cast<std::uint32_t>(prev),
                static_cast<std::uint32_t>(next),
                peRecorderStats_.chunkInterAppendPairSamples[index],
                totalNs,
                peRecorderStats_.chunkInterAppendPairNsMax[index]};
            for (auto& slot : top) {
                if (candidate.totalNs <= slot.totalNs) {
                    continue;
                }
                std::swap(candidate, slot);
            }
        }
    }
    return top;
}

std::array<D3D9DeviceImpl::PeInterAppendCallFamilySummary,
           kPeRecorderInterAppendTopCallFamilyCount>
D3D9DeviceImpl::topPeInterAppendFocusCallFamilies(std::size_t focusPair) const noexcept {
    std::array<PeInterAppendCallFamilySummary,
               kPeRecorderInterAppendTopCallFamilyCount>
        top{};
    if (focusPair >= kPeInterAppendFocusPairCount) {
        return top;
    }
    for (std::size_t family = 0; family < kPeInterAppendCallFamilyCount;
         ++family) {
        const std::size_t index =
            focusPair * kPeInterAppendCallFamilyCount + family;
        const std::uint64_t totalNs =
            peRecorderStats_
                .chunkInterAppendFocusCallFamilyNsTotal[index];
        if (totalNs == 0) {
            continue;
        }
        PeInterAppendCallFamilySummary candidate{
            static_cast<std::uint32_t>(family),
            peRecorderStats_
                .chunkInterAppendFocusCallFamilySamples[index],
            totalNs,
            peRecorderStats_
                .chunkInterAppendFocusCallFamilyNsMax[index]};
        for (auto& slot : top) {
            if (candidate.totalNs <= slot.totalNs) {
                continue;
            }
            std::swap(candidate, slot);
        }
    }
    return top;
}

std::array<D3D9DeviceImpl::PeInterAppendCallFamilySummary,
           kPeRecorderInterAppendTopCallFamilyCount>
D3D9DeviceImpl::topPeInterAppendFocusBetweenCallFamilies(std::size_t focusPair) const noexcept {
    std::array<PeInterAppendCallFamilySummary,
               kPeRecorderInterAppendTopCallFamilyCount>
        top{};
    if (focusPair >= kPeInterAppendFocusPairCount) {
        return top;
    }
    for (std::size_t family = 0; family < kPeInterAppendCallFamilyCount;
         ++family) {
        const std::size_t index =
            focusPair * kPeInterAppendCallFamilyCount + family;
        const std::uint64_t samples =
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallFamilySamples[index];
        if (samples == 0) {
            continue;
        }
        PeInterAppendCallFamilySummary candidate{
            static_cast<std::uint32_t>(family), samples, samples, 0};
        for (auto& slot : top) {
            if (candidate.samples <= slot.samples) {
                continue;
            }
            std::swap(candidate, slot);
        }
    }
    return top;
}

std::array<D3D9DeviceImpl::PeInterAppendCallNameSummary,
           kPeRecorderInterAppendTopCallNameCount>
D3D9DeviceImpl::topPeInterAppendFocusBetweenCallNames(std::size_t focusPair) const noexcept {
    std::array<PeInterAppendCallNameSummary,
               kPeRecorderInterAppendTopCallNameCount>
        top{};
    if (focusPair >= kPeInterAppendFocusPairCount) {
        return top;
    }
    for (std::size_t callName = 0; callName < kPeInterAppendCallNameCount;
         ++callName) {
        const std::size_t index =
            focusPair * kPeInterAppendCallNameCount + callName;
        const std::uint64_t samples =
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallNameSamples[index];
        if (samples == 0) {
            continue;
        }
        const auto cpuNs =
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallNameCpuNsTotal[index];
        PeInterAppendCallNameSummary candidate{
            static_cast<std::uint32_t>(callName), samples, cpuNs,
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallNameCpuNsMax[index]};
        for (auto& slot : top) {
            if (candidate.samples <= slot.samples) {
                continue;
            }
            std::swap(candidate, slot);
        }
    }
    return top;
}

std::array<D3D9DeviceImpl::PeInterAppendCallTransitionSummary,
           kPeRecorderInterAppendTopCallTransitionCount>
D3D9DeviceImpl::topPeInterAppendFocusBetweenCallTransitions(
    std::size_t focusPair) const noexcept {
    std::array<PeInterAppendCallTransitionSummary,
               kPeRecorderInterAppendTopCallTransitionCount>
        top{};
    if (focusPair >= kPeInterAppendFocusPairCount) {
        return top;
    }
    for (std::size_t prevFamily = 0;
         prevFamily < kPeInterAppendCallFamilyCount; ++prevFamily) {
        for (std::size_t nextFamily = 0;
             nextFamily < kPeInterAppendCallFamilyCount; ++nextFamily) {
            const std::size_t index =
                peInterAppendFocusCallTransitionIndex(
                    focusPair,
                    static_cast<PeInterAppendCallFamily>(prevFamily),
                    static_cast<PeInterAppendCallFamily>(nextFamily));
            const std::uint64_t totalNs =
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallTransitionNsTotal[
                        index];
            if (totalNs == 0) {
                continue;
            }
            PeInterAppendCallTransitionSummary candidate{
                static_cast<std::uint32_t>(prevFamily),
                static_cast<std::uint32_t>(nextFamily),
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallTransitionSamples[
                        index],
                totalNs,
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallTransitionNsMax[
                        index]};
            for (auto& slot : top) {
                if (candidate.totalNs <= slot.totalNs) {
                    continue;
                }
                std::swap(candidate, slot);
            }
        }
    }
    return top;
}

std::array<D3D9DeviceImpl::PeInterAppendCallNameTransitionSummary,
           kPeRecorderInterAppendTopCallTransitionCount>
D3D9DeviceImpl::topPeInterAppendFocusBetweenCallNameTransitions(
    std::size_t focusPair) const noexcept {
    std::array<PeInterAppendCallNameTransitionSummary,
               kPeRecorderInterAppendTopCallTransitionCount>
        top{};
    if (focusPair >= kPeInterAppendFocusPairCount) {
        return top;
    }
    for (std::size_t prevCallName = 0;
         prevCallName < kPeInterAppendCallNameCount; ++prevCallName) {
        for (std::size_t nextCallName = 0;
             nextCallName < kPeInterAppendCallNameCount; ++nextCallName) {
            const std::size_t index =
                peInterAppendFocusCallNameTransitionIndex(
                    focusPair,
                    static_cast<PeInterAppendCallName>(prevCallName),
                    static_cast<PeInterAppendCallName>(nextCallName));
            const std::uint64_t totalNs =
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallNameTransitionNsTotal[
                        index];
            if (totalNs == 0) {
                continue;
            }
            PeInterAppendCallNameTransitionSummary candidate{
                static_cast<std::uint32_t>(prevCallName),
                static_cast<std::uint32_t>(nextCallName),
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallNameTransitionSamples[
                        index],
                totalNs,
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallNameTransitionNsMax[
                        index]};
            for (auto& slot : top) {
                if (candidate.totalNs <= slot.totalNs) {
                    continue;
                }
                std::swap(candidate, slot);
            }
        }
    }
    return top;
}

std::array<PeInterAppendCallSiteSummary,
           kPeRecorderInterAppendTopCallTransitionCount>
D3D9DeviceImpl::topPeInterAppendFocusBetweenCallNameTransitionSites(
    std::size_t focusPair) const {
    std::array<PeInterAppendCallSiteSummary,
               kPeRecorderInterAppendTopCallTransitionCount>
        top{};
    if (focusPair >= kPeInterAppendFocusPairCount) {
        return top;
    }
    const auto wantedFocusPair = static_cast<std::uint32_t>(focusPair);
    for (const auto& entry :
         peRecorderFocusBetweenCallNameTransitionSites_) {
        const auto& key = entry.first;
        const auto& stats = entry.second;
        if (key.focusPair != wantedFocusPair || stats.totalNs == 0) {
            continue;
        }
        PeInterAppendCallSiteSummary candidate{
            key.prevCallName, key.nextCallName, key.callerPc,
            stats.samples, stats.totalNs, stats.maxNs};
        for (auto& slot : top) {
            if (candidate.totalNs <= slot.totalNs) {
                continue;
            }
            std::swap(candidate, slot);
        }
    }
    return top;
}

bool D3D9DeviceImpl::peRecordMilestoneBit(std::uint32_t recordCount,
                                 std::uint32_t& bit) noexcept {
    switch (recordCount) {
    case 1: bit = 1u << 0; return true;
    case 4: bit = 1u << 1; return true;
    case 8: bit = 1u << 2; return true;
    case 16: bit = 1u << 3; return true;
    case 32: bit = 1u << 4; return true;
    case 64: bit = 1u << 5; return true;
    default:
        bit = 0;
        return false;
    }
}

void D3D9DeviceImpl::logPeRecordMilestoneAfterPresent(std::uint32_t type,
                                      std::uint32_t recordCount,
                                      std::uint32_t payloadBytes,
                                      std::int64_t entryNs) {
    if (!dxmt9PeRecorderStatsEnabled()) {
        return;
    }
    std::uint32_t milestoneBit = 0;
    if (!peRecordMilestoneBit(recordCount, milestoneBit)) {
        return;
    }
    const std::uint64_t ordinal =
        pePresentRecordPendingOrdinal_.load(std::memory_order_acquire);
    if (ordinal == 0) {
        return;
    }
    std::uint32_t mask =
        pePresentRecordMilestoneMask_.load(std::memory_order_acquire);
    while ((mask & milestoneBit) == 0) {
        if (pePresentRecordMilestoneMask_.compare_exchange_weak(
                mask, mask | milestoneBit, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            const std::int64_t returnNs =
                pePresentCadenceReturnNs_.load(std::memory_order_acquire);
            dxmt9DeviceInfoLog(
                "pe_present_record_milestone device=%p ordinal=%llu "
                "milestone=%u type=%s typeId=%u call=%s "
                "thread_id=0x%lx "
                "entry_delta_ms=%.3f recordCount=%u payloadBytes=%u",
                this, static_cast<unsigned long long>(ordinal), recordCount,
                peCommandRecordTypeName(type), type,
                dxmt9PeCurrentCallName ? dxmt9PeCurrentCallName : "unknown",
                static_cast<unsigned long>(dxmt9PeCurrentThreadId()),
                static_cast<double>(entryNs - returnNs) / 1000000.0,
                recordCount, payloadBytes);
            return;
        }
    }
}

void D3D9DeviceImpl::logPeFirstChunkAfterPresent(PeRecorderFlushReason reason,
                                 const PePresentCadenceClaim& claim,
                                 HRESULT hr,
                                 const PeCommandChunkCommitInfo& info) {
    if (!claim.claimed) {
        return;
    }
    const std::int64_t observedNs =
        dxmt9SteadyClockNs(std::chrono::steady_clock::now());
    dxmt9DeviceInfoLog(
        "pe_present_next_chunk device=%p ordinal=%llu reason=%s "
        "thread_id=0x%lx hr=0x%08x "
        "entry_delta_ms=%.3f return_delta_ms=%.3f "
        "bridge_ms=%.3f recordCount=%u payloadBytes=%u "
        "handleCount=%u wireBytes=%u",
        this, static_cast<unsigned long long>(claim.ordinal),
        peRecorderFlushReasonName(reason),
        static_cast<unsigned long>(dxmt9PeCurrentThreadId()),
        static_cast<unsigned>(hr),
        static_cast<double>(claim.entryNs - claim.returnNs) / 1000000.0,
        static_cast<double>(observedNs - claim.returnNs) / 1000000.0,
        static_cast<double>(observedNs - claim.entryNs) / 1000000.0,
        info.recordCount, info.payloadBytes, info.handleCount,
        info.wireBytes);
}

void D3D9DeviceImpl::recordPeChunkCommit(PeRecorderFlushReason reason,
                         std::uint32_t recordCount,
                         std::uint32_t payloadBytes,
                         std::uint32_t handleCount,
                         std::uint32_t wireBytes,
                         std::uint64_t fillGapNs,
                         std::uint64_t activeFillNs,
                         std::uint64_t bridgeNs) {
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
    if (fillGapNs > 0) {
        ++peRecorderStats_.chunkFillGapSamples;
        peRecorderStats_.chunkFillGapNsTotal += fillGapNs;
        peRecorderStats_.chunkFillGapNsMax =
            std::max(peRecorderStats_.chunkFillGapNsMax, fillGapNs);
    }
    if (activeFillNs > 0) {
        ++peRecorderStats_.chunkActiveFillSamples;
        peRecorderStats_.chunkActiveFillNsTotal += activeFillNs;
        peRecorderStats_.chunkActiveFillNsMax =
            std::max(peRecorderStats_.chunkActiveFillNsMax, activeFillNs);
    }
    ++peRecorderStats_.chunkBridgeSamples;
    peRecorderStats_.chunkBridgeNsTotal += bridgeNs;
    peRecorderStats_.chunkBridgeNsMax =
        std::max(peRecorderStats_.chunkBridgeNsMax, bridgeNs);
    const auto reasonIndex = static_cast<std::size_t>(reason);
    if (reasonIndex < peRecorderStats_.flushReasons.size()) {
        ++peRecorderStats_.flushReasons[reasonIndex];
    }
    if (dxmt9PeRecorderChunkLogEnabled()) {
        dxmt9DeviceInfoLog(
            "pe_recorder_chunk device=%p reason=%s commitCount=%llu "
            "recordCount=%u payloadBytes=%u handleCount=%u wireBytes=%u "
            "fillGapMs=%.3f activeFillMs=%.3f bridgeMs=%.3f",
            this, peRecorderFlushReasonName(reason),
            static_cast<unsigned long long>(peRecorderStats_.commitCount),
            recordCount, payloadBytes, handleCount, wireBytes,
            static_cast<double>(fillGapNs) / 1000000.0,
            static_cast<double>(activeFillNs) / 1000000.0,
            static_cast<double>(bridgeNs) / 1000000.0);
    }
}

void D3D9DeviceImpl::recordPeChunkInterAppendGap(std::int64_t appendEntryNs,
                                 std::uint32_t recordCountBefore,
                                 std::uint32_t nextType) {
    if (!dxmt9PeRecorderStatsEnabled() ||
        recordCountBefore == 0 ||
        peRecorderLastAppendReturnNs_ <= 0 ||
        appendEntryNs <= peRecorderLastAppendReturnNs_) {
        return;
    }
    const auto interAppendGapNs =
        static_cast<std::uint64_t>(
            appendEntryNs - peRecorderLastAppendReturnNs_);
    ++peRecorderStats_.chunkInterAppendGapSamples;
    peRecorderStats_.chunkInterAppendGapNsTotal += interAppendGapNs;
    peRecorderStats_.chunkInterAppendGapNsMax =
        std::max(peRecorderStats_.chunkInterAppendGapNsMax,
                 interAppendGapNs);
    const std::size_t pairIndex =
        peInterAppendPairIndex(peRecorderLastAppendRecordType_, nextType);
    ++peRecorderStats_.chunkInterAppendPairSamples[pairIndex];
    peRecorderStats_.chunkInterAppendPairNsTotal[pairIndex] +=
        interAppendGapNs;
    peRecorderStats_.chunkInterAppendPairNsMax[pairIndex] =
        std::max(peRecorderStats_.chunkInterAppendPairNsMax[pairIndex],
                 interAppendGapNs);
    const std::uint32_t prevType = peRecorderLastAppendRecordType_;
    const std::uint32_t nextBucket = peCommandRecordTypeBucket(nextType);
    const std::size_t focusPair =
        peInterAppendFocusPairIndex(prevType, nextBucket);
    if (focusPair < kPeInterAppendFocusPairCount) {
        PeInterAppendCallFamily callFamily = dxmt9PeCurrentAppendFamily;
        if (callFamily == PeInterAppendCallFamily::Unknown) {
            callFamily =
                peInterAppendCallFamilyFromName(dxmt9PeCurrentCallName);
        }
        const std::size_t focusIndex =
            peInterAppendFocusCallFamilyIndex(focusPair, callFamily);
        ++peRecorderStats_
            .chunkInterAppendFocusCallFamilySamples[focusIndex];
        peRecorderStats_
            .chunkInterAppendFocusCallFamilyNsTotal[focusIndex] +=
            interAppendGapNs;
        peRecorderStats_
            .chunkInterAppendFocusCallFamilyNsMax[focusIndex] =
            std::max(
                peRecorderStats_
                    .chunkInterAppendFocusCallFamilyNsMax[focusIndex],
                interAppendGapNs);
        recordPeChunkInterAppendFocusPhaseSplit(focusPair, appendEntryNs);
    }
    if (peRecorderBetweenCallsActive_) {
        resetPeBetweenCallsWindow();
    }
}

void D3D9DeviceImpl::recordPeChunkInterAppendFocusPhaseSplit(std::size_t focusPair,
                                             std::int64_t appendEntryNs) {
    if (focusPair >= kPeInterAppendFocusPairCount ||
        peRecorderLastAppendReturnNs_ <= 0 ||
        dxmt9PeCurrentCallEntryNs <= peRecorderLastAppendReturnNs_ ||
        dxmt9PeCurrentCallEntryNs > appendEntryNs) {
        return;
    }
    const auto preCallNs = static_cast<std::uint64_t>(
        dxmt9PeCurrentCallEntryNs - peRecorderLastAppendReturnNs_);
    const auto insideCallNs = static_cast<std::uint64_t>(
        appendEntryNs - dxmt9PeCurrentCallEntryNs);
    ++peRecorderStats_.chunkInterAppendFocusPhaseSamples[focusPair];
    peRecorderStats_.chunkInterAppendFocusPreCallNsTotal[focusPair] +=
        preCallNs;
    peRecorderStats_.chunkInterAppendFocusPreCallNsMax[focusPair] =
        std::max(
            peRecorderStats_.chunkInterAppendFocusPreCallNsMax[focusPair],
            preCallNs);
    peRecorderStats_.chunkInterAppendFocusInsideCallNsTotal[focusPair] +=
        insideCallNs;
    peRecorderStats_.chunkInterAppendFocusInsideCallNsMax[focusPair] =
        std::max(
            peRecorderStats_.chunkInterAppendFocusInsideCallNsMax[focusPair],
            insideCallNs);
    recordPeChunkInterAppendFocusTailSplit(focusPair);
}

void D3D9DeviceImpl::recordPeChunkInterAppendFocusTailSplit(std::size_t focusPair) {
    if (focusPair >= kPeInterAppendFocusPairCount ||
        peRecorderLastAppendReturnNs_ <= 0 ||
        peRecorderLastAppendCallExitNs_ <= peRecorderLastAppendReturnNs_ ||
        dxmt9PeCurrentCallEntryNs < peRecorderLastAppendCallExitNs_) {
        return;
    }
    const auto prevCallTailNs = static_cast<std::uint64_t>(
        peRecorderLastAppendCallExitNs_ - peRecorderLastAppendReturnNs_);
    const auto betweenCallsNs = static_cast<std::uint64_t>(
        dxmt9PeCurrentCallEntryNs - peRecorderLastAppendCallExitNs_);
    ++peRecorderStats_.chunkInterAppendFocusTailSplitSamples[focusPair];
    peRecorderStats_
        .chunkInterAppendFocusPrevCallTailNsTotal[focusPair] +=
        prevCallTailNs;
    peRecorderStats_.chunkInterAppendFocusPrevCallTailNsMax[focusPair] =
        std::max(
            peRecorderStats_
                .chunkInterAppendFocusPrevCallTailNsMax[focusPair],
            prevCallTailNs);
    peRecorderStats_.chunkInterAppendFocusBetweenCallsNsTotal[focusPair] +=
        betweenCallsNs;
    peRecorderStats_.chunkInterAppendFocusBetweenCallsNsMax[focusPair] =
        std::max(
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallsNsMax[focusPair],
            betweenCallsNs);
    recordPeChunkInterAppendFocusBetweenCallFamilies(focusPair);
}

void D3D9DeviceImpl::recordPeBetweenCallsEntry(const char* callName,
                               std::int64_t entryNs,
                               const void* callerPc) {
    if (!dxmt9PeRecorderStatsEnabled() ||
        !peRecorderBetweenCallsActive_ ||
        entryNs <= peRecorderBetweenCallsStartNs_) {
        return;
    }
    const auto family = peInterAppendCallFamilyFromName(callName);
    const auto callNameBucket = peInterAppendCallNameFromName(callName);
    if (peRecorderBetweenLastCallExitNs_ >=
            peRecorderBetweenCallsStartNs_ &&
        entryNs > peRecorderBetweenLastCallExitNs_) {
        const std::size_t transitionIndex =
            peInterAppendCallTransitionIndex(
                peRecorderBetweenLastCallFamily_, family);
        const auto gapNs = static_cast<std::uint64_t>(
            entryNs - peRecorderBetweenLastCallExitNs_);
        ++peRecorderBetweenCallTransitionSamples_[transitionIndex];
        peRecorderBetweenCallTransitionNsTotal_[transitionIndex] += gapNs;
        peRecorderBetweenCallTransitionNsMax_[transitionIndex] =
            std::max(
                peRecorderBetweenCallTransitionNsMax_[transitionIndex],
                gapNs);
        const std::size_t nameTransitionIndex =
            peInterAppendCallNameTransitionIndex(
                peRecorderBetweenLastCallName_, callNameBucket);
        ++peRecorderBetweenCallNameTransitionSamples_[nameTransitionIndex];
        peRecorderBetweenCallNameTransitionNsTotal_[nameTransitionIndex] +=
            gapNs;
        peRecorderBetweenCallNameTransitionNsMax_[nameTransitionIndex] =
            std::max(
                peRecorderBetweenCallNameTransitionNsMax_[
                    nameTransitionIndex],
                gapNs);
        PeInterAppendCallSiteLocalKey siteKey{
            static_cast<std::uint32_t>(peRecorderBetweenLastCallName_),
            static_cast<std::uint32_t>(callNameBucket),
            callerPc};
        auto& siteStats =
            peRecorderBetweenCallNameTransitionSites_[siteKey];
        ++siteStats.samples;
        siteStats.totalNs += gapNs;
        siteStats.maxNs = std::max(siteStats.maxNs, gapNs);
    }
    peRecorderBetweenLastCallExitNs_ = 0;
    peRecorderBetweenLastCallFamily_ = family;
    peRecorderBetweenLastCallName_ = callNameBucket;
    ++peRecorderBetweenCallFamilySamples_[
        static_cast<std::size_t>(family)];
    ++peRecorderBetweenCallNameSamples_[
        static_cast<std::size_t>(callNameBucket)];
}

void D3D9DeviceImpl::recordPeBetweenCallsReturn(const char* callName,
                                std::int64_t entryNs,
                                std::int64_t exitNs) {
    if (!dxmt9PeRecorderStatsEnabled() ||
        !peRecorderBetweenCallsActive_ ||
        entryNs <= peRecorderBetweenCallsStartNs_ ||
        exitNs <= entryNs) {
        return;
    }
    const auto callNameBucket = peInterAppendCallNameFromName(callName);
    const auto index = static_cast<std::size_t>(callNameBucket);
    const auto cpuNs = static_cast<std::uint64_t>(exitNs - entryNs);
    peRecorderBetweenLastCallFamily_ =
        peInterAppendCallFamilyFromName(callName);
    peRecorderBetweenLastCallName_ = callNameBucket;
    peRecorderBetweenLastCallExitNs_ = exitNs;
    ++peRecorderBetweenCallBodyCalls_;
    peRecorderBetweenCallBodyCpuNsTotal_ += cpuNs;
    peRecorderBetweenCallBodyCpuNsMax_ =
        std::max(peRecorderBetweenCallBodyCpuNsMax_, cpuNs);
    peRecorderBetweenCallNameCpuNsTotal_[index] += cpuNs;
    peRecorderBetweenCallNameCpuNsMax_[index] =
        std::max(peRecorderBetweenCallNameCpuNsMax_[index], cpuNs);
}

void D3D9DeviceImpl::recordPeChunkInterAppendFocusBetweenCallFamilies(std::size_t focusPair) {
    if (focusPair >= kPeInterAppendFocusPairCount) {
        resetPeBetweenCallsWindow();
        return;
    }
    auto samples = peRecorderBetweenCallFamilySamples_;
    const auto terminalFamily =
        peRecorderBetweenCallsActive_
        ? peInterAppendCallFamilyFromName(dxmt9PeCurrentCallName)
        : PeInterAppendCallFamily::Unknown;
    auto& terminalSamples =
        samples[static_cast<std::size_t>(terminalFamily)];
    if (terminalSamples != 0) {
        --terminalSamples;
    }
    auto callNameSamples = peRecorderBetweenCallNameSamples_;
    auto callNameCpuNsTotal = peRecorderBetweenCallNameCpuNsTotal_;
    auto callNameCpuNsMax = peRecorderBetweenCallNameCpuNsMax_;
    auto transitionSamples = peRecorderBetweenCallTransitionSamples_;
    auto transitionNsTotal = peRecorderBetweenCallTransitionNsTotal_;
    auto transitionNsMax = peRecorderBetweenCallTransitionNsMax_;
    auto nameTransitionSamples =
        peRecorderBetweenCallNameTransitionSamples_;
    auto nameTransitionNsTotal =
        peRecorderBetweenCallNameTransitionNsTotal_;
    auto nameTransitionNsMax = peRecorderBetweenCallNameTransitionNsMax_;
    const std::uint64_t bodyCalls = peRecorderBetweenCallBodyCalls_;
    const std::uint64_t bodyCpuNsTotal =
        peRecorderBetweenCallBodyCpuNsTotal_;
    const std::uint64_t bodyCpuNsMax = peRecorderBetweenCallBodyCpuNsMax_;
    const auto terminalCallName =
        peRecorderBetweenCallsActive_
        ? peInterAppendCallNameFromName(dxmt9PeCurrentCallName)
        : PeInterAppendCallName::Unknown;
    auto& terminalCallNameSamples =
        callNameSamples[static_cast<std::size_t>(terminalCallName)];
    if (terminalCallNameSamples != 0) {
        --terminalCallNameSamples;
    }
    for (std::size_t family = 0; family < kPeInterAppendCallFamilyCount;
         ++family) {
        const std::uint64_t count = samples[family];
        if (count == 0) {
            continue;
        }
        const std::size_t index =
            focusPair * kPeInterAppendCallFamilyCount + family;
        peRecorderStats_
            .chunkInterAppendFocusBetweenCallFamilySamples[index] += count;
    }
    for (std::size_t callName = 0; callName < kPeInterAppendCallNameCount;
         ++callName) {
        const std::uint64_t count = callNameSamples[callName];
        if (count == 0) {
            continue;
        }
        const std::size_t index =
            focusPair * kPeInterAppendCallNameCount + callName;
        peRecorderStats_
            .chunkInterAppendFocusBetweenCallNameSamples[index] += count;
        peRecorderStats_
            .chunkInterAppendFocusBetweenCallNameCpuNsTotal[index] +=
            callNameCpuNsTotal[callName];
        peRecorderStats_.chunkInterAppendFocusBetweenCallNameCpuNsMax[index] =
            std::max(
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallNameCpuNsMax[index],
            callNameCpuNsMax[callName]);
    }
    for (std::size_t prevFamily = 0;
         prevFamily < kPeInterAppendCallFamilyCount; ++prevFamily) {
        for (std::size_t nextFamily = 0;
             nextFamily < kPeInterAppendCallFamilyCount; ++nextFamily) {
            const std::size_t localIndex =
                prevFamily * kPeInterAppendCallFamilyCount + nextFamily;
            const std::uint64_t count = transitionSamples[localIndex];
            if (count == 0) {
                continue;
            }
            const std::size_t index =
                peInterAppendFocusCallTransitionIndex(
                    focusPair,
                    static_cast<PeInterAppendCallFamily>(prevFamily),
                    static_cast<PeInterAppendCallFamily>(nextFamily));
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallTransitionSamples[
                    index] += count;
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallTransitionNsTotal[
                    index] += transitionNsTotal[localIndex];
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallTransitionNsMax[index] =
                std::max(
                    peRecorderStats_
                        .chunkInterAppendFocusBetweenCallTransitionNsMax[
                            index],
                    transitionNsMax[localIndex]);
        }
    }
    for (std::size_t prevCallName = 0;
         prevCallName < kPeInterAppendCallNameCount; ++prevCallName) {
        for (std::size_t nextCallName = 0;
             nextCallName < kPeInterAppendCallNameCount; ++nextCallName) {
            const std::size_t localIndex =
                prevCallName * kPeInterAppendCallNameCount + nextCallName;
            const std::uint64_t count =
                nameTransitionSamples[localIndex];
            if (count == 0) {
                continue;
            }
            const std::size_t index =
                peInterAppendFocusCallNameTransitionIndex(
                    focusPair,
                    static_cast<PeInterAppendCallName>(prevCallName),
                    static_cast<PeInterAppendCallName>(nextCallName));
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallNameTransitionSamples[
                    index] += count;
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallNameTransitionNsTotal[
                    index] += nameTransitionNsTotal[localIndex];
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallNameTransitionNsMax[
                    index] =
                std::max(
                    peRecorderStats_
                        .chunkInterAppendFocusBetweenCallNameTransitionNsMax[
                            index],
                    nameTransitionNsMax[localIndex]);
        }
    }
    const auto focusPairValue = static_cast<std::uint32_t>(focusPair);
    for (const auto& entry : peRecorderBetweenCallNameTransitionSites_) {
        const auto& key = entry.first;
        const auto& stats = entry.second;
        PeInterAppendCallSiteKey siteKey{
            focusPairValue, key.prevCallName, key.nextCallName,
            key.callerPc};
        auto& siteStats =
            peRecorderFocusBetweenCallNameTransitionSites_[siteKey];
        siteStats.samples += stats.samples;
        siteStats.totalNs += stats.totalNs;
        siteStats.maxNs = std::max(siteStats.maxNs, stats.maxNs);
    }
    peRecorderStats_.chunkInterAppendFocusBetweenCallBodyCalls[focusPair] +=
        bodyCalls;
    peRecorderStats_
        .chunkInterAppendFocusBetweenCallBodyCpuNsTotal[focusPair] +=
        bodyCpuNsTotal;
    peRecorderStats_.chunkInterAppendFocusBetweenCallBodyCpuNsMax[focusPair] =
        std::max(
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallBodyCpuNsMax[focusPair],
            bodyCpuNsMax);
    resetPeBetweenCallsWindow();
}

void D3D9DeviceImpl::notePeCurrentCallReturnForInterAppendSplit() {
    if (!dxmt9PeRecorderStatsEnabled() ||
        peRecorderLastAppendCallEntryNs_ <= 0 ||
        peRecorderLastAppendCallEntryNs_ != dxmt9PeCurrentCallEntryNs ||
        peRecorderLastAppendRecordType_ !=
            D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
        return;
    }
    peRecorderLastAppendCallExitNs_ =
        dxmt9SteadyClockNs(std::chrono::steady_clock::now());
    peRecorderBetweenCallsActive_ = true;
    peRecorderBetweenCallsStartNs_ = peRecorderLastAppendCallExitNs_;
    peRecorderBetweenCallFamilySamples_.fill(0);
    peRecorderBetweenCallNameSamples_.fill(0);
    peRecorderBetweenCallNameCpuNsTotal_.fill(0);
    peRecorderBetweenCallNameCpuNsMax_.fill(0);
    peRecorderBetweenLastCallFamily_ = PeInterAppendCallFamily::Draw;
    peRecorderBetweenLastCallName_ =
        PeInterAppendCallName::DrawIndexedPrimitive;
    peRecorderBetweenLastCallExitNs_ = peRecorderBetweenCallsStartNs_;
    peRecorderBetweenCallTransitionSamples_.fill(0);
    peRecorderBetweenCallTransitionNsTotal_.fill(0);
    peRecorderBetweenCallTransitionNsMax_.fill(0);
    peRecorderBetweenCallNameTransitionSamples_.fill(0);
    peRecorderBetweenCallNameTransitionNsTotal_.fill(0);
    peRecorderBetweenCallNameTransitionNsMax_.fill(0);
    peRecorderBetweenCallNameTransitionSites_.clear();
    peRecorderBetweenCallBodyCalls_ = 0;
    peRecorderBetweenCallBodyCpuNsTotal_ = 0;
    peRecorderBetweenCallBodyCpuNsMax_ = 0;
}

void D3D9DeviceImpl::recordPeAppendCpu(std::uint64_t appendCpuNs, bool noFlushAppend) {
    if (!dxmt9PeRecorderStatsEnabled() || appendCpuNs == 0) {
        return;
    }
    ++peRecorderStats_.recordAppendCalls;
    peRecorderStats_.recordAppendCpuNsTotal += appendCpuNs;
    peRecorderStats_.recordAppendCpuNsMax =
        std::max(peRecorderStats_.recordAppendCpuNsMax, appendCpuNs);
    if (!noFlushAppend) {
        return;
    }
    ++peRecorderStats_.recordAppendNoFlushCalls;
    peRecorderStats_.recordAppendNoFlushCpuNsTotal += appendCpuNs;
    peRecorderStats_.recordAppendNoFlushCpuNsMax =
        std::max(peRecorderStats_.recordAppendNoFlushCpuNsMax,
                 appendCpuNs);
}

void D3D9DeviceImpl::recordPeConstSetterCpu(std::uint32_t recordType,
                            std::int64_t entryNs,
                            std::uint32_t regs) {
    if (!dxmt9PeRecorderStatsEnabled() || entryNs <= 0) {
        return;
    }
    const std::int64_t returnNs =
        dxmt9SteadyClockNs(std::chrono::steady_clock::now());
    if (returnNs <= entryNs) {
        return;
    }
    const auto cpuNs = static_cast<std::uint64_t>(returnNs - entryNs);
    if (recordType == D9C_COMMAND_RECORD_SET_VS_CONST_F) {
        ++peRecorderStats_.vsConstFSetterCalls;
        peRecorderStats_.vsConstFSetterRegs += regs;
        peRecorderStats_.vsConstFSetterCpuNsTotal += cpuNs;
        peRecorderStats_.vsConstFSetterCpuNsMax =
            std::max(peRecorderStats_.vsConstFSetterCpuNsMax, cpuNs);
    } else if (recordType == D9C_COMMAND_RECORD_SET_PS_CONST_F) {
        ++peRecorderStats_.psConstFSetterCalls;
        peRecorderStats_.psConstFSetterRegs += regs;
        peRecorderStats_.psConstFSetterCpuNsTotal += cpuNs;
        peRecorderStats_.psConstFSetterCpuNsMax =
            std::max(peRecorderStats_.psConstFSetterCpuNsMax, cpuNs);
    }
}

void D3D9DeviceImpl::recordPeConstFlushCpu(std::uint32_t recordType,
                           std::int64_t entryNs,
                           std::uint32_t records,
                           std::uint32_t regs) {
    if (!dxmt9PeRecorderStatsEnabled() || entryNs <= 0 || records == 0u) {
        return;
    }
    const std::int64_t returnNs =
        dxmt9SteadyClockNs(std::chrono::steady_clock::now());
    if (returnNs <= entryNs) {
        return;
    }
    const auto cpuNs = static_cast<std::uint64_t>(returnNs - entryNs);
    ++peRecorderStats_.constFlushCalls;
    peRecorderStats_.constFlushRecords += records;
    peRecorderStats_.constFlushRegs += regs;
    peRecorderStats_.constFlushCpuNsTotal += cpuNs;
    peRecorderStats_.constFlushCpuNsMax =
        std::max(peRecorderStats_.constFlushCpuNsMax, cpuNs);
    if (recordType == D9C_COMMAND_RECORD_SET_VS_CONST_F) {
        peRecorderStats_.vsConstFFlushRecords += records;
        peRecorderStats_.vsConstFFlushRegs += regs;
        peRecorderStats_.vsConstFFlushCpuNsTotal += cpuNs;
    } else if (recordType == D9C_COMMAND_RECORD_SET_PS_CONST_F) {
        peRecorderStats_.psConstFFlushRecords += records;
        peRecorderStats_.psConstFFlushRegs += regs;
        peRecorderStats_.psConstFFlushCpuNsTotal += cpuNs;
    }
}

void D3D9DeviceImpl::recordPeChunkBarrierConstCpu(std::int64_t entryNs) {
    if (!dxmt9PeRecorderStatsEnabled() || entryNs <= 0) {
        return;
    }
    const std::int64_t returnNs =
        dxmt9SteadyClockNs(std::chrono::steady_clock::now());
    if (returnNs <= entryNs) {
        return;
    }
    const auto cpuNs = static_cast<std::uint64_t>(returnNs - entryNs);
    ++peRecorderStats_.chunkBarrierFlushCalls;
    peRecorderStats_.chunkBarrierConstCpuNsTotal += cpuNs;
    peRecorderStats_.chunkBarrierConstCpuNsMax =
        std::max(peRecorderStats_.chunkBarrierConstCpuNsMax, cpuNs);
}

void D3D9DeviceImpl::recordPeApplyStateBuildCpu(std::int64_t entryNs) {
    if (!dxmt9PeRecorderStatsEnabled() || entryNs <= 0) {
        return;
    }
    const std::int64_t returnNs =
        dxmt9SteadyClockNs(std::chrono::steady_clock::now());
    if (returnNs <= entryNs) {
        return;
    }
    const auto cpuNs = static_cast<std::uint64_t>(returnNs - entryNs);
    ++peRecorderStats_.applyStateBuildCalls;
    peRecorderStats_.applyStateBuildCpuNsTotal += cpuNs;
    peRecorderStats_.applyStateBuildCpuNsMax =
        std::max(peRecorderStats_.applyStateBuildCpuNsMax, cpuNs);
}

void D3D9DeviceImpl::recordPeHotStateSetterCpu(PeHotStateSetterFamily family,
                               std::int64_t entryNs,
                               bool dirty) {
    if (!dxmt9PeRecorderStatsEnabled() || entryNs <= 0) {
        return;
    }
    const auto index = static_cast<std::size_t>(family);
    if (index >= kPeHotStateSetterFamilyCount) {
        return;
    }
    ++peRecorderStats_.hotStateSetterCalls[index];
    if (dirty) {
        ++peRecorderStats_.hotStateSetterDirty[index];
    }
    const std::int64_t returnNs =
        dxmt9SteadyClockNs(std::chrono::steady_clock::now());
    if (returnNs <= entryNs) {
        return;
    }
    const auto cpuNs = static_cast<std::uint64_t>(returnNs - entryNs);
    peRecorderStats_.hotStateSetterCpuNsTotal[index] += cpuNs;
    peRecorderStats_.hotStateSetterCpuNsMax[index] =
        std::max(peRecorderStats_.hotStateSetterCpuNsMax[index], cpuNs);
}

void D3D9DeviceImpl::notePeChunkAppendBoundary(std::int64_t appendReturnNs,
                               std::uint32_t type) {
    if (!dxmt9PeRecorderStatsEnabled() ||
        commandChunk_.recordCount() == 0) {
        return;
    }
    if (peRecorderCurrentChunkFirstAppendNs_ == 0) {
        peRecorderCurrentChunkFirstAppendNs_ = appendReturnNs;
        const std::int64_t priorReturnNs = peRecorderLastChunkReturnNs_;
        if (priorReturnNs > 0 && appendReturnNs > priorReturnNs) {
            const auto firstRecordGapNs =
                static_cast<std::uint64_t>(
                    appendReturnNs - priorReturnNs);
            ++peRecorderStats_.chunkFirstRecordGapSamples;
            peRecorderStats_.chunkFirstRecordGapNsTotal += firstRecordGapNs;
            peRecorderStats_.chunkFirstRecordGapNsMax =
                std::max(peRecorderStats_.chunkFirstRecordGapNsMax,
                         firstRecordGapNs);
        }
    }
    peRecorderLastAppendReturnNs_ = appendReturnNs;
    peRecorderLastAppendCallEntryNs_ = dxmt9PeCurrentCallEntryNs;
    peRecorderLastAppendCallExitNs_ = 0;
    peRecorderLastAppendRecordType_ = peCommandRecordTypeBucket(type);
}

void D3D9DeviceImpl::logPeRecorderStats(const char* event, bool force) {
    if (!dxmt9PeRecorderStatsEnabled()) {
        return;
    }
    if (!force &&
        peRecorderStatsLastLoggedCommitCount_ == peRecorderStats_.commitCount) {
        return;
    }
    peRecorderStatsLastLoggedCommitCount_ = peRecorderStats_.commitCount;
    const auto topInterAppendPairs = topPeInterAppendPairs();
    const auto hotRt =
        static_cast<std::size_t>(PeHotStateSetterFamily::RenderTarget);
    const auto hotDs =
        static_cast<std::size_t>(PeHotStateSetterFamily::DepthStencil);
    const auto hotViewportScissor =
        static_cast<std::size_t>(PeHotStateSetterFamily::ViewportScissor);
    const auto hotTransform =
        static_cast<std::size_t>(PeHotStateSetterFamily::Transform);
    const auto hotMaterialLightClip =
        static_cast<std::size_t>(PeHotStateSetterFamily::MaterialLightClip);
    const auto hotRenderState =
        static_cast<std::size_t>(PeHotStateSetterFamily::RenderState);
    const auto hotTssSampler =
        static_cast<std::size_t>(
            PeHotStateSetterFamily::TextureStageSampler);
    const auto hotTexture =
        static_cast<std::size_t>(PeHotStateSetterFamily::Texture);
    const auto hotVertexInput =
        static_cast<std::size_t>(PeHotStateSetterFamily::VertexInput);
    const auto hotShader =
        static_cast<std::size_t>(PeHotStateSetterFamily::Shader);
    const auto gapVsConstFocus = static_cast<std::size_t>(
        PeInterAppendFocusPair::DrawIndexedToVsConstF);
    const auto gapApplyFocus = static_cast<std::size_t>(
        PeInterAppendFocusPair::DrawIndexedToApplyState);
    const auto gapDrawFocus = static_cast<std::size_t>(
        PeInterAppendFocusPair::DrawIndexedToDrawIndexed);
    const auto gapPsConstFocus = static_cast<std::size_t>(
        PeInterAppendFocusPair::DrawIndexedToPsConstF);
    const auto gapVsConstFamilies =
        topPeInterAppendFocusCallFamilies(gapVsConstFocus);
    const auto gapApplyFamilies =
        topPeInterAppendFocusCallFamilies(gapApplyFocus);
    const auto gapDrawFamilies =
        topPeInterAppendFocusCallFamilies(gapDrawFocus);
    const auto gapPsConstFamilies =
        topPeInterAppendFocusCallFamilies(gapPsConstFocus);
    const auto gapVsConstBetweenFamilies =
        topPeInterAppendFocusBetweenCallFamilies(gapVsConstFocus);
    const auto gapApplyBetweenFamilies =
        topPeInterAppendFocusBetweenCallFamilies(gapApplyFocus);
    const auto gapDrawBetweenFamilies =
        topPeInterAppendFocusBetweenCallFamilies(gapDrawFocus);
    const auto gapPsConstBetweenFamilies =
        topPeInterAppendFocusBetweenCallFamilies(gapPsConstFocus);
    const auto gapVsConstBetweenCallNames =
        topPeInterAppendFocusBetweenCallNames(gapVsConstFocus);
    const auto gapApplyBetweenCallNames =
        topPeInterAppendFocusBetweenCallNames(gapApplyFocus);
    const auto gapDrawBetweenCallNames =
        topPeInterAppendFocusBetweenCallNames(gapDrawFocus);
    const auto gapPsConstBetweenCallNames =
        topPeInterAppendFocusBetweenCallNames(gapPsConstFocus);
    const auto gapVsConstBetweenTransitions =
        topPeInterAppendFocusBetweenCallTransitions(gapVsConstFocus);
    const auto gapApplyBetweenTransitions =
        topPeInterAppendFocusBetweenCallTransitions(gapApplyFocus);
    const auto gapDrawBetweenTransitions =
        topPeInterAppendFocusBetweenCallTransitions(gapDrawFocus);
    const auto gapPsConstBetweenTransitions =
        topPeInterAppendFocusBetweenCallTransitions(gapPsConstFocus);
    const auto gapVsConstBetweenNameTransitions =
        topPeInterAppendFocusBetweenCallNameTransitions(gapVsConstFocus);
    const auto gapApplyBetweenNameTransitions =
        topPeInterAppendFocusBetweenCallNameTransitions(gapApplyFocus);
    const auto gapDrawBetweenNameTransitions =
        topPeInterAppendFocusBetweenCallNameTransitions(gapDrawFocus);
    const auto gapPsConstBetweenNameTransitions =
        topPeInterAppendFocusBetweenCallNameTransitions(gapPsConstFocus);
    const auto gapVsConstBetweenCallSites =
        topPeInterAppendFocusBetweenCallNameTransitionSites(gapVsConstFocus);
    const auto gapApplyBetweenCallSites =
        topPeInterAppendFocusBetweenCallNameTransitionSites(gapApplyFocus);
    const auto gapDrawBetweenCallSites =
        topPeInterAppendFocusBetweenCallNameTransitionSites(gapDrawFocus);
    const auto gapPsConstBetweenCallSites =
        topPeInterAppendFocusBetweenCallNameTransitionSites(gapPsConstFocus);
    const auto phaseSamples = [this](std::size_t focus) noexcept {
        return peRecorderStats_.chunkInterAppendFocusPhaseSamples[focus];
    };
    const auto preCallMs = [this](std::size_t focus) noexcept {
        return static_cast<double>(
            peRecorderStats_
                .chunkInterAppendFocusPreCallNsTotal[focus]) /
            1000000.0;
    };
    const auto preCallMaxMs = [this](std::size_t focus) noexcept {
        return static_cast<double>(
            peRecorderStats_
                .chunkInterAppendFocusPreCallNsMax[focus]) /
            1000000.0;
    };
    const auto insideCallMs = [this](std::size_t focus) noexcept {
        return static_cast<double>(
            peRecorderStats_
                .chunkInterAppendFocusInsideCallNsTotal[focus]) /
            1000000.0;
    };
    const auto insideCallMaxMs = [this](std::size_t focus) noexcept {
        return static_cast<double>(
            peRecorderStats_
                .chunkInterAppendFocusInsideCallNsMax[focus]) /
            1000000.0;
    };
    const auto tailSplitSamples = [this](std::size_t focus) noexcept {
        return peRecorderStats_
            .chunkInterAppendFocusTailSplitSamples[focus];
    };
    const auto prevCallTailMs = [this](std::size_t focus) noexcept {
        return static_cast<double>(
            peRecorderStats_
                .chunkInterAppendFocusPrevCallTailNsTotal[focus]) /
            1000000.0;
    };
    const auto prevCallTailMaxMs = [this](std::size_t focus) noexcept {
        return static_cast<double>(
            peRecorderStats_
                .chunkInterAppendFocusPrevCallTailNsMax[focus]) /
            1000000.0;
    };
    const auto betweenCallsMs = [this](std::size_t focus) noexcept {
        return static_cast<double>(
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallsNsTotal[focus]) /
            1000000.0;
    };
    const auto betweenCallsMaxMs = [this](std::size_t focus) noexcept {
        return static_cast<double>(
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallsNsMax[focus]) /
            1000000.0;
    };
    const auto betweenCallBodyCalls = [this](std::size_t focus) noexcept {
        return peRecorderStats_
            .chunkInterAppendFocusBetweenCallBodyCalls[focus];
    };
    const auto betweenCallBodyCpuMs = [this](std::size_t focus) noexcept {
        return static_cast<double>(
            peRecorderStats_
                .chunkInterAppendFocusBetweenCallBodyCpuNsTotal[focus]) /
            1000000.0;
    };
    const auto betweenCallBodyCpuMaxMs =
        [this](std::size_t focus) noexcept {
            return static_cast<double>(
                peRecorderStats_
                    .chunkInterAppendFocusBetweenCallBodyCpuNsMax[focus]) /
                1000000.0;
        };
    dxmt9DeviceInfoLog(
        "pe_recorder_stats event=%s device=%p commitCount=%llu "
        "recordCountTotal=%llu recordCountMax=%llu "
        "payloadBytesTotal=%llu payloadBytesMax=%llu "
        "handleCountTotal=%llu handleCountMax=%llu "
        "chunkFillGapSamples=%llu chunkFillGapMs=%.3f "
        "chunkFillGapMaxMs=%.3f "
        "chunkFirstRecordGapSamples=%llu chunkFirstRecordGapMs=%.3f "
        "chunkFirstRecordGapMaxMs=%.3f "
        "chunkActiveFillSamples=%llu chunkActiveFillMs=%.3f "
        "chunkActiveFillMaxMs=%.3f "
        "chunkInterAppendGapSamples=%llu chunkInterAppendGapMs=%.3f "
        "chunkInterAppendGapMaxMs=%.3f "
        "chunkBridgeSamples=%llu chunkBridgeMs=%.3f "
        "chunkBridgeMaxMs=%.3f "
        "recordAppendCalls=%llu recordAppendCpuMs=%.3f "
        "recordAppendCpuMaxMs=%.3f "
        "recordAppendNoFlushCalls=%llu recordAppendNoFlushCpuMs=%.3f "
        "recordAppendNoFlushCpuMaxMs=%.3f "
        "interAppendTop1PrevType=%u interAppendTop1Prev=%s "
        "interAppendTop1NextType=%u interAppendTop1Next=%s "
        "interAppendTop1Samples=%llu interAppendTop1Ms=%.3f "
        "interAppendTop1MaxMs=%.3f "
        "interAppendTop2PrevType=%u interAppendTop2Prev=%s "
        "interAppendTop2NextType=%u interAppendTop2Next=%s "
        "interAppendTop2Samples=%llu interAppendTop2Ms=%.3f "
        "interAppendTop2MaxMs=%.3f "
        "interAppendTop3PrevType=%u interAppendTop3Prev=%s "
        "interAppendTop3NextType=%u interAppendTop3Next=%s "
        "interAppendTop3Samples=%llu interAppendTop3Ms=%.3f "
        "interAppendTop3MaxMs=%.3f "
        "interAppendTop4PrevType=%u interAppendTop4Prev=%s "
        "interAppendTop4NextType=%u interAppendTop4Next=%s "
        "interAppendTop4Samples=%llu interAppendTop4Ms=%.3f "
        "interAppendTop4MaxMs=%.3f "
        "vsConstFSetterCalls=%llu vsConstFSetterRegs=%llu "
        "vsConstFSetterCpuMs=%.3f vsConstFSetterCpuMaxMs=%.3f "
        "psConstFSetterCalls=%llu psConstFSetterRegs=%llu "
        "psConstFSetterCpuMs=%.3f psConstFSetterCpuMaxMs=%.3f "
        "constFlushCalls=%llu constFlushRecords=%llu "
        "constFlushRegs=%llu constFlushCpuMs=%.3f "
        "constFlushCpuMaxMs=%.3f "
        "vsConstFFlushRecords=%llu vsConstFFlushRegs=%llu "
        "vsConstFFlushCpuMs=%.3f "
        "psConstFFlushRecords=%llu psConstFFlushRegs=%llu "
        "psConstFFlushCpuMs=%.3f "
        "chunkBarrierFlushCalls=%llu chunkBarrierConstCpuMs=%.3f "
        "chunkBarrierConstCpuMaxMs=%.3f "
        "applyStateBuildCalls=%llu applyStateBuildCpuMs=%.3f "
        "applyStateBuildCpuMaxMs=%.3f "
        "hotSetterRtCalls=%llu hotSetterRtDirty=%llu "
        "hotSetterRtCpuMs=%.3f hotSetterRtCpuMaxMs=%.3f "
        "hotSetterDsCalls=%llu hotSetterDsDirty=%llu "
        "hotSetterDsCpuMs=%.3f hotSetterDsCpuMaxMs=%.3f "
        "hotSetterViewportScissorCalls=%llu "
        "hotSetterViewportScissorDirty=%llu "
        "hotSetterViewportScissorCpuMs=%.3f "
        "hotSetterViewportScissorCpuMaxMs=%.3f "
        "hotSetterTransformCalls=%llu hotSetterTransformDirty=%llu "
        "hotSetterTransformCpuMs=%.3f "
        "hotSetterTransformCpuMaxMs=%.3f "
        "hotSetterMaterialLightClipCalls=%llu "
        "hotSetterMaterialLightClipDirty=%llu "
        "hotSetterMaterialLightClipCpuMs=%.3f "
        "hotSetterMaterialLightClipCpuMaxMs=%.3f "
        "hotSetterRenderStateCalls=%llu "
        "hotSetterRenderStateDirty=%llu "
        "hotSetterRenderStateCpuMs=%.3f "
        "hotSetterRenderStateCpuMaxMs=%.3f "
        "hotSetterTssSamplerCalls=%llu "
        "hotSetterTssSamplerDirty=%llu "
        "hotSetterTssSamplerCpuMs=%.3f "
        "hotSetterTssSamplerCpuMaxMs=%.3f "
        "hotSetterTextureCalls=%llu hotSetterTextureDirty=%llu "
        "hotSetterTextureCpuMs=%.3f "
        "hotSetterTextureCpuMaxMs=%.3f "
        "hotSetterVertexInputCalls=%llu "
        "hotSetterVertexInputDirty=%llu "
        "hotSetterVertexInputCpuMs=%.3f "
        "hotSetterVertexInputCpuMaxMs=%.3f "
        "hotSetterShaderCalls=%llu hotSetterShaderDirty=%llu "
        "hotSetterShaderCpuMs=%.3f "
        "hotSetterShaderCpuMaxMs=%.3f "
        "gapDrawIndexedVsConstFTop1CallFamily=%s "
        "gapDrawIndexedVsConstFTop1Samples=%llu "
        "gapDrawIndexedVsConstFTop1Ms=%.3f "
        "gapDrawIndexedVsConstFTop1MaxMs=%.3f "
        "gapDrawIndexedVsConstFTop2CallFamily=%s "
        "gapDrawIndexedVsConstFTop2Samples=%llu "
        "gapDrawIndexedVsConstFTop2Ms=%.3f "
        "gapDrawIndexedVsConstFTop2MaxMs=%.3f "
        "gapDrawIndexedApplyStateTop1CallFamily=%s "
        "gapDrawIndexedApplyStateTop1Samples=%llu "
        "gapDrawIndexedApplyStateTop1Ms=%.3f "
        "gapDrawIndexedApplyStateTop1MaxMs=%.3f "
        "gapDrawIndexedApplyStateTop2CallFamily=%s "
        "gapDrawIndexedApplyStateTop2Samples=%llu "
        "gapDrawIndexedApplyStateTop2Ms=%.3f "
        "gapDrawIndexedApplyStateTop2MaxMs=%.3f "
        "gapDrawIndexedDrawIndexedTop1CallFamily=%s "
        "gapDrawIndexedDrawIndexedTop1Samples=%llu "
        "gapDrawIndexedDrawIndexedTop1Ms=%.3f "
        "gapDrawIndexedDrawIndexedTop1MaxMs=%.3f "
        "gapDrawIndexedDrawIndexedTop2CallFamily=%s "
        "gapDrawIndexedDrawIndexedTop2Samples=%llu "
        "gapDrawIndexedDrawIndexedTop2Ms=%.3f "
        "gapDrawIndexedDrawIndexedTop2MaxMs=%.3f "
        "gapDrawIndexedPsConstFTop1CallFamily=%s "
        "gapDrawIndexedPsConstFTop1Samples=%llu "
        "gapDrawIndexedPsConstFTop1Ms=%.3f "
        "gapDrawIndexedPsConstFTop1MaxMs=%.3f "
        "gapDrawIndexedPsConstFTop2CallFamily=%s "
        "gapDrawIndexedPsConstFTop2Samples=%llu "
        "gapDrawIndexedPsConstFTop2Ms=%.3f "
        "gapDrawIndexedPsConstFTop2MaxMs=%.3f "
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
            peRecorderStats_.chunkFillGapSamples),
        static_cast<double>(peRecorderStats_.chunkFillGapNsTotal) /
            1000000.0,
        static_cast<double>(peRecorderStats_.chunkFillGapNsMax) /
            1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.chunkFirstRecordGapSamples),
        static_cast<double>(
            peRecorderStats_.chunkFirstRecordGapNsTotal) / 1000000.0,
        static_cast<double>(
            peRecorderStats_.chunkFirstRecordGapNsMax) / 1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.chunkActiveFillSamples),
        static_cast<double>(peRecorderStats_.chunkActiveFillNsTotal) /
            1000000.0,
        static_cast<double>(peRecorderStats_.chunkActiveFillNsMax) /
            1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.chunkInterAppendGapSamples),
        static_cast<double>(
            peRecorderStats_.chunkInterAppendGapNsTotal) / 1000000.0,
        static_cast<double>(
            peRecorderStats_.chunkInterAppendGapNsMax) / 1000000.0,
        static_cast<unsigned long long>(peRecorderStats_.chunkBridgeSamples),
        static_cast<double>(peRecorderStats_.chunkBridgeNsTotal) /
            1000000.0,
        static_cast<double>(peRecorderStats_.chunkBridgeNsMax) /
            1000000.0,
        static_cast<unsigned long long>(peRecorderStats_.recordAppendCalls),
        static_cast<double>(peRecorderStats_.recordAppendCpuNsTotal) /
            1000000.0,
        static_cast<double>(peRecorderStats_.recordAppendCpuNsMax) /
            1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.recordAppendNoFlushCalls),
        static_cast<double>(
            peRecorderStats_.recordAppendNoFlushCpuNsTotal) / 1000000.0,
        static_cast<double>(
            peRecorderStats_.recordAppendNoFlushCpuNsMax) / 1000000.0,
        topInterAppendPairs[0].prevType,
        peCommandRecordTypeName(topInterAppendPairs[0].prevType),
        topInterAppendPairs[0].nextType,
        peCommandRecordTypeName(topInterAppendPairs[0].nextType),
        static_cast<unsigned long long>(topInterAppendPairs[0].samples),
        static_cast<double>(topInterAppendPairs[0].totalNs) / 1000000.0,
        static_cast<double>(topInterAppendPairs[0].maxNs) / 1000000.0,
        topInterAppendPairs[1].prevType,
        peCommandRecordTypeName(topInterAppendPairs[1].prevType),
        topInterAppendPairs[1].nextType,
        peCommandRecordTypeName(topInterAppendPairs[1].nextType),
        static_cast<unsigned long long>(topInterAppendPairs[1].samples),
        static_cast<double>(topInterAppendPairs[1].totalNs) / 1000000.0,
        static_cast<double>(topInterAppendPairs[1].maxNs) / 1000000.0,
        topInterAppendPairs[2].prevType,
        peCommandRecordTypeName(topInterAppendPairs[2].prevType),
        topInterAppendPairs[2].nextType,
        peCommandRecordTypeName(topInterAppendPairs[2].nextType),
        static_cast<unsigned long long>(topInterAppendPairs[2].samples),
        static_cast<double>(topInterAppendPairs[2].totalNs) / 1000000.0,
        static_cast<double>(topInterAppendPairs[2].maxNs) / 1000000.0,
        topInterAppendPairs[3].prevType,
        peCommandRecordTypeName(topInterAppendPairs[3].prevType),
        topInterAppendPairs[3].nextType,
        peCommandRecordTypeName(topInterAppendPairs[3].nextType),
        static_cast<unsigned long long>(topInterAppendPairs[3].samples),
        static_cast<double>(topInterAppendPairs[3].totalNs) / 1000000.0,
        static_cast<double>(topInterAppendPairs[3].maxNs) / 1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.vsConstFSetterCalls),
        static_cast<unsigned long long>(
            peRecorderStats_.vsConstFSetterRegs),
        static_cast<double>(
            peRecorderStats_.vsConstFSetterCpuNsTotal) / 1000000.0,
        static_cast<double>(
            peRecorderStats_.vsConstFSetterCpuNsMax) / 1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.psConstFSetterCalls),
        static_cast<unsigned long long>(
            peRecorderStats_.psConstFSetterRegs),
        static_cast<double>(
            peRecorderStats_.psConstFSetterCpuNsTotal) / 1000000.0,
        static_cast<double>(
            peRecorderStats_.psConstFSetterCpuNsMax) / 1000000.0,
        static_cast<unsigned long long>(peRecorderStats_.constFlushCalls),
        static_cast<unsigned long long>(peRecorderStats_.constFlushRecords),
        static_cast<unsigned long long>(peRecorderStats_.constFlushRegs),
        static_cast<double>(peRecorderStats_.constFlushCpuNsTotal) /
            1000000.0,
        static_cast<double>(peRecorderStats_.constFlushCpuNsMax) /
            1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.vsConstFFlushRecords),
        static_cast<unsigned long long>(peRecorderStats_.vsConstFFlushRegs),
        static_cast<double>(
            peRecorderStats_.vsConstFFlushCpuNsTotal) / 1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.psConstFFlushRecords),
        static_cast<unsigned long long>(peRecorderStats_.psConstFFlushRegs),
        static_cast<double>(
            peRecorderStats_.psConstFFlushCpuNsTotal) / 1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.chunkBarrierFlushCalls),
        static_cast<double>(
            peRecorderStats_.chunkBarrierConstCpuNsTotal) / 1000000.0,
        static_cast<double>(
            peRecorderStats_.chunkBarrierConstCpuNsMax) / 1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.applyStateBuildCalls),
        static_cast<double>(
            peRecorderStats_.applyStateBuildCpuNsTotal) / 1000000.0,
        static_cast<double>(
            peRecorderStats_.applyStateBuildCpuNsMax) / 1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterCalls[hotRt]),
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterDirty[hotRt]),
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsTotal[hotRt]) /
            1000000.0,
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsMax[hotRt]) / 1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterCalls[hotDs]),
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterDirty[hotDs]),
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsTotal[hotDs]) /
            1000000.0,
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsMax[hotDs]) / 1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterCalls[hotViewportScissor]),
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterDirty[hotViewportScissor]),
        static_cast<double>(
            peRecorderStats_
                .hotStateSetterCpuNsTotal[hotViewportScissor]) /
            1000000.0,
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsMax[hotViewportScissor]) /
            1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterCalls[hotTransform]),
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterDirty[hotTransform]),
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsTotal[hotTransform]) /
            1000000.0,
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsMax[hotTransform]) /
            1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterCalls[hotMaterialLightClip]),
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterDirty[hotMaterialLightClip]),
        static_cast<double>(
            peRecorderStats_
                .hotStateSetterCpuNsTotal[hotMaterialLightClip]) /
            1000000.0,
        static_cast<double>(
            peRecorderStats_
                .hotStateSetterCpuNsMax[hotMaterialLightClip]) /
            1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterCalls[hotRenderState]),
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterDirty[hotRenderState]),
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsTotal[hotRenderState]) /
            1000000.0,
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsMax[hotRenderState]) /
            1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterCalls[hotTssSampler]),
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterDirty[hotTssSampler]),
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsTotal[hotTssSampler]) /
            1000000.0,
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsMax[hotTssSampler]) /
            1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterCalls[hotTexture]),
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterDirty[hotTexture]),
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsTotal[hotTexture]) /
            1000000.0,
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsMax[hotTexture]) /
            1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterCalls[hotVertexInput]),
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterDirty[hotVertexInput]),
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsTotal[hotVertexInput]) /
            1000000.0,
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsMax[hotVertexInput]) /
            1000000.0,
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterCalls[hotShader]),
        static_cast<unsigned long long>(
            peRecorderStats_.hotStateSetterDirty[hotShader]),
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsTotal[hotShader]) /
            1000000.0,
        static_cast<double>(
            peRecorderStats_.hotStateSetterCpuNsMax[hotShader]) /
            1000000.0,
        peInterAppendCallFamilyName(gapVsConstFamilies[0].family),
        static_cast<unsigned long long>(gapVsConstFamilies[0].samples),
        static_cast<double>(gapVsConstFamilies[0].totalNs) / 1000000.0,
        static_cast<double>(gapVsConstFamilies[0].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapVsConstFamilies[1].family),
        static_cast<unsigned long long>(gapVsConstFamilies[1].samples),
        static_cast<double>(gapVsConstFamilies[1].totalNs) / 1000000.0,
        static_cast<double>(gapVsConstFamilies[1].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapApplyFamilies[0].family),
        static_cast<unsigned long long>(gapApplyFamilies[0].samples),
        static_cast<double>(gapApplyFamilies[0].totalNs) / 1000000.0,
        static_cast<double>(gapApplyFamilies[0].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapApplyFamilies[1].family),
        static_cast<unsigned long long>(gapApplyFamilies[1].samples),
        static_cast<double>(gapApplyFamilies[1].totalNs) / 1000000.0,
        static_cast<double>(gapApplyFamilies[1].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapDrawFamilies[0].family),
        static_cast<unsigned long long>(gapDrawFamilies[0].samples),
        static_cast<double>(gapDrawFamilies[0].totalNs) / 1000000.0,
        static_cast<double>(gapDrawFamilies[0].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapDrawFamilies[1].family),
        static_cast<unsigned long long>(gapDrawFamilies[1].samples),
        static_cast<double>(gapDrawFamilies[1].totalNs) / 1000000.0,
        static_cast<double>(gapDrawFamilies[1].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapPsConstFamilies[0].family),
        static_cast<unsigned long long>(gapPsConstFamilies[0].samples),
        static_cast<double>(gapPsConstFamilies[0].totalNs) / 1000000.0,
        static_cast<double>(gapPsConstFamilies[0].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapPsConstFamilies[1].family),
        static_cast<unsigned long long>(gapPsConstFamilies[1].samples),
        static_cast<double>(gapPsConstFamilies[1].totalNs) / 1000000.0,
        static_cast<double>(gapPsConstFamilies[1].maxNs) / 1000000.0,
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
    dxmt9DeviceInfoLog(
        "pe_recorder_gap_call_stats event=%s device=%p "
        "gapDrawIndexedVsConstFTop1CallFamily=%s "
        "gapDrawIndexedVsConstFTop1Samples=%llu "
        "gapDrawIndexedVsConstFTop1Ms=%.3f "
        "gapDrawIndexedVsConstFTop1MaxMs=%.3f "
        "gapDrawIndexedVsConstFTop2CallFamily=%s "
        "gapDrawIndexedVsConstFTop2Samples=%llu "
        "gapDrawIndexedVsConstFTop2Ms=%.3f "
        "gapDrawIndexedVsConstFTop2MaxMs=%.3f "
        "gapDrawIndexedApplyStateTop1CallFamily=%s "
        "gapDrawIndexedApplyStateTop1Samples=%llu "
        "gapDrawIndexedApplyStateTop1Ms=%.3f "
        "gapDrawIndexedApplyStateTop1MaxMs=%.3f "
        "gapDrawIndexedApplyStateTop2CallFamily=%s "
        "gapDrawIndexedApplyStateTop2Samples=%llu "
        "gapDrawIndexedApplyStateTop2Ms=%.3f "
        "gapDrawIndexedApplyStateTop2MaxMs=%.3f "
        "gapDrawIndexedDrawIndexedTop1CallFamily=%s "
        "gapDrawIndexedDrawIndexedTop1Samples=%llu "
        "gapDrawIndexedDrawIndexedTop1Ms=%.3f "
        "gapDrawIndexedDrawIndexedTop1MaxMs=%.3f "
        "gapDrawIndexedDrawIndexedTop2CallFamily=%s "
        "gapDrawIndexedDrawIndexedTop2Samples=%llu "
        "gapDrawIndexedDrawIndexedTop2Ms=%.3f "
        "gapDrawIndexedDrawIndexedTop2MaxMs=%.3f "
        "gapDrawIndexedPsConstFTop1CallFamily=%s "
        "gapDrawIndexedPsConstFTop1Samples=%llu "
        "gapDrawIndexedPsConstFTop1Ms=%.3f "
        "gapDrawIndexedPsConstFTop1MaxMs=%.3f "
        "gapDrawIndexedPsConstFTop2CallFamily=%s "
        "gapDrawIndexedPsConstFTop2Samples=%llu "
        "gapDrawIndexedPsConstFTop2Ms=%.3f "
        "gapDrawIndexedPsConstFTop2MaxMs=%.3f "
        "gapDrawIndexedVsConstFPhaseSamples=%llu "
        "gapDrawIndexedVsConstFPreCallMs=%.3f "
        "gapDrawIndexedVsConstFPreCallMaxMs=%.3f "
        "gapDrawIndexedVsConstFInsideCallMs=%.3f "
        "gapDrawIndexedVsConstFInsideCallMaxMs=%.3f "
        "gapDrawIndexedApplyStatePhaseSamples=%llu "
        "gapDrawIndexedApplyStatePreCallMs=%.3f "
        "gapDrawIndexedApplyStatePreCallMaxMs=%.3f "
        "gapDrawIndexedApplyStateInsideCallMs=%.3f "
        "gapDrawIndexedApplyStateInsideCallMaxMs=%.3f "
        "gapDrawIndexedDrawIndexedPhaseSamples=%llu "
        "gapDrawIndexedDrawIndexedPreCallMs=%.3f "
        "gapDrawIndexedDrawIndexedPreCallMaxMs=%.3f "
        "gapDrawIndexedDrawIndexedInsideCallMs=%.3f "
        "gapDrawIndexedDrawIndexedInsideCallMaxMs=%.3f "
        "gapDrawIndexedPsConstFPhaseSamples=%llu "
        "gapDrawIndexedPsConstFPreCallMs=%.3f "
        "gapDrawIndexedPsConstFPreCallMaxMs=%.3f "
        "gapDrawIndexedPsConstFInsideCallMs=%.3f "
        "gapDrawIndexedPsConstFInsideCallMaxMs=%.3f",
        event ? event : "unknown", this,
        peInterAppendCallFamilyName(gapVsConstFamilies[0].family),
        static_cast<unsigned long long>(gapVsConstFamilies[0].samples),
        static_cast<double>(gapVsConstFamilies[0].totalNs) / 1000000.0,
        static_cast<double>(gapVsConstFamilies[0].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapVsConstFamilies[1].family),
        static_cast<unsigned long long>(gapVsConstFamilies[1].samples),
        static_cast<double>(gapVsConstFamilies[1].totalNs) / 1000000.0,
        static_cast<double>(gapVsConstFamilies[1].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapApplyFamilies[0].family),
        static_cast<unsigned long long>(gapApplyFamilies[0].samples),
        static_cast<double>(gapApplyFamilies[0].totalNs) / 1000000.0,
        static_cast<double>(gapApplyFamilies[0].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapApplyFamilies[1].family),
        static_cast<unsigned long long>(gapApplyFamilies[1].samples),
        static_cast<double>(gapApplyFamilies[1].totalNs) / 1000000.0,
        static_cast<double>(gapApplyFamilies[1].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapDrawFamilies[0].family),
        static_cast<unsigned long long>(gapDrawFamilies[0].samples),
        static_cast<double>(gapDrawFamilies[0].totalNs) / 1000000.0,
        static_cast<double>(gapDrawFamilies[0].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapDrawFamilies[1].family),
        static_cast<unsigned long long>(gapDrawFamilies[1].samples),
        static_cast<double>(gapDrawFamilies[1].totalNs) / 1000000.0,
        static_cast<double>(gapDrawFamilies[1].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapPsConstFamilies[0].family),
        static_cast<unsigned long long>(gapPsConstFamilies[0].samples),
        static_cast<double>(gapPsConstFamilies[0].totalNs) / 1000000.0,
        static_cast<double>(gapPsConstFamilies[0].maxNs) / 1000000.0,
        peInterAppendCallFamilyName(gapPsConstFamilies[1].family),
        static_cast<unsigned long long>(gapPsConstFamilies[1].samples),
        static_cast<double>(gapPsConstFamilies[1].totalNs) / 1000000.0,
        static_cast<double>(gapPsConstFamilies[1].maxNs) / 1000000.0,
        static_cast<unsigned long long>(phaseSamples(gapVsConstFocus)),
        preCallMs(gapVsConstFocus),
        preCallMaxMs(gapVsConstFocus),
        insideCallMs(gapVsConstFocus),
        insideCallMaxMs(gapVsConstFocus),
        static_cast<unsigned long long>(phaseSamples(gapApplyFocus)),
        preCallMs(gapApplyFocus),
        preCallMaxMs(gapApplyFocus),
        insideCallMs(gapApplyFocus),
        insideCallMaxMs(gapApplyFocus),
        static_cast<unsigned long long>(phaseSamples(gapDrawFocus)),
        preCallMs(gapDrawFocus),
        preCallMaxMs(gapDrawFocus),
        insideCallMs(gapDrawFocus),
        insideCallMaxMs(gapDrawFocus),
        static_cast<unsigned long long>(phaseSamples(gapPsConstFocus)),
        preCallMs(gapPsConstFocus),
        preCallMaxMs(gapPsConstFocus),
        insideCallMs(gapPsConstFocus),
        insideCallMaxMs(gapPsConstFocus));
    dxmt9DeviceInfoLog(
        "pe_recorder_gap_body_stats event=%s device=%p "
        "gapDrawIndexedVsConstFBetweenCallBodyCalls=%llu "
        "gapDrawIndexedVsConstFBetweenCallBodyCpuMs=%.3f "
        "gapDrawIndexedVsConstFBetweenCallBodyCpuMaxMs=%.3f "
        "gapDrawIndexedApplyStateBetweenCallBodyCalls=%llu "
        "gapDrawIndexedApplyStateBetweenCallBodyCpuMs=%.3f "
        "gapDrawIndexedApplyStateBetweenCallBodyCpuMaxMs=%.3f "
        "gapDrawIndexedDrawIndexedBetweenCallBodyCalls=%llu "
        "gapDrawIndexedDrawIndexedBetweenCallBodyCpuMs=%.3f "
        "gapDrawIndexedDrawIndexedBetweenCallBodyCpuMaxMs=%.3f "
        "gapDrawIndexedPsConstFBetweenCallBodyCalls=%llu "
        "gapDrawIndexedPsConstFBetweenCallBodyCpuMs=%.3f "
        "gapDrawIndexedPsConstFBetweenCallBodyCpuMaxMs=%.3f",
        event ? event : "unknown", this,
        static_cast<unsigned long long>(
            betweenCallBodyCalls(gapVsConstFocus)),
        betweenCallBodyCpuMs(gapVsConstFocus),
        betweenCallBodyCpuMaxMs(gapVsConstFocus),
        static_cast<unsigned long long>(
            betweenCallBodyCalls(gapApplyFocus)),
        betweenCallBodyCpuMs(gapApplyFocus),
        betweenCallBodyCpuMaxMs(gapApplyFocus),
        static_cast<unsigned long long>(
            betweenCallBodyCalls(gapDrawFocus)),
        betweenCallBodyCpuMs(gapDrawFocus),
        betweenCallBodyCpuMaxMs(gapDrawFocus),
        static_cast<unsigned long long>(
            betweenCallBodyCalls(gapPsConstFocus)),
        betweenCallBodyCpuMs(gapPsConstFocus),
        betweenCallBodyCpuMaxMs(gapPsConstFocus));
    dxmt9DeviceInfoLog(
        "pe_recorder_gap_tail_stats event=%s device=%p "
        "gapDrawIndexedVsConstFTailSplitSamples=%llu "
        "gapDrawIndexedVsConstFPrevCallTailMs=%.3f "
        "gapDrawIndexedVsConstFPrevCallTailMaxMs=%.3f "
        "gapDrawIndexedVsConstFBetweenCallsMs=%.3f "
        "gapDrawIndexedVsConstFBetweenCallsMaxMs=%.3f "
        "gapDrawIndexedApplyStateTailSplitSamples=%llu "
        "gapDrawIndexedApplyStatePrevCallTailMs=%.3f "
        "gapDrawIndexedApplyStatePrevCallTailMaxMs=%.3f "
        "gapDrawIndexedApplyStateBetweenCallsMs=%.3f "
        "gapDrawIndexedApplyStateBetweenCallsMaxMs=%.3f "
        "gapDrawIndexedDrawIndexedTailSplitSamples=%llu "
        "gapDrawIndexedDrawIndexedPrevCallTailMs=%.3f "
        "gapDrawIndexedDrawIndexedPrevCallTailMaxMs=%.3f "
        "gapDrawIndexedDrawIndexedBetweenCallsMs=%.3f "
        "gapDrawIndexedDrawIndexedBetweenCallsMaxMs=%.3f "
        "gapDrawIndexedPsConstFTailSplitSamples=%llu "
        "gapDrawIndexedPsConstFPrevCallTailMs=%.3f "
        "gapDrawIndexedPsConstFPrevCallTailMaxMs=%.3f "
        "gapDrawIndexedPsConstFBetweenCallsMs=%.3f "
        "gapDrawIndexedPsConstFBetweenCallsMaxMs=%.3f",
        event ? event : "unknown", this,
        static_cast<unsigned long long>(
            tailSplitSamples(gapVsConstFocus)),
        prevCallTailMs(gapVsConstFocus),
        prevCallTailMaxMs(gapVsConstFocus),
        betweenCallsMs(gapVsConstFocus),
        betweenCallsMaxMs(gapVsConstFocus),
        static_cast<unsigned long long>(tailSplitSamples(gapApplyFocus)),
        prevCallTailMs(gapApplyFocus),
        prevCallTailMaxMs(gapApplyFocus),
        betweenCallsMs(gapApplyFocus),
        betweenCallsMaxMs(gapApplyFocus),
        static_cast<unsigned long long>(tailSplitSamples(gapDrawFocus)),
        prevCallTailMs(gapDrawFocus),
        prevCallTailMaxMs(gapDrawFocus),
        betweenCallsMs(gapDrawFocus),
        betweenCallsMaxMs(gapDrawFocus),
        static_cast<unsigned long long>(tailSplitSamples(gapPsConstFocus)),
        prevCallTailMs(gapPsConstFocus),
        prevCallTailMaxMs(gapPsConstFocus),
        betweenCallsMs(gapPsConstFocus),
        betweenCallsMaxMs(gapPsConstFocus));
    dxmt9DeviceInfoLog(
        "pe_recorder_gap_between_call_stats event=%s device=%p "
        "gapDrawIndexedVsConstFBetweenTop1CallFamily=%s "
        "gapDrawIndexedVsConstFBetweenTop1Samples=%llu "
        "gapDrawIndexedVsConstFBetweenTop2CallFamily=%s "
        "gapDrawIndexedVsConstFBetweenTop2Samples=%llu "
        "gapDrawIndexedApplyStateBetweenTop1CallFamily=%s "
        "gapDrawIndexedApplyStateBetweenTop1Samples=%llu "
        "gapDrawIndexedApplyStateBetweenTop2CallFamily=%s "
        "gapDrawIndexedApplyStateBetweenTop2Samples=%llu "
        "gapDrawIndexedDrawIndexedBetweenTop1CallFamily=%s "
        "gapDrawIndexedDrawIndexedBetweenTop1Samples=%llu "
        "gapDrawIndexedDrawIndexedBetweenTop2CallFamily=%s "
        "gapDrawIndexedDrawIndexedBetweenTop2Samples=%llu "
        "gapDrawIndexedPsConstFBetweenTop1CallFamily=%s "
        "gapDrawIndexedPsConstFBetweenTop1Samples=%llu "
        "gapDrawIndexedPsConstFBetweenTop2CallFamily=%s "
        "gapDrawIndexedPsConstFBetweenTop2Samples=%llu "
        "gapDrawIndexedVsConstFBetweenTop1CallName=%s "
        "gapDrawIndexedVsConstFBetweenTop1CallNameSamples=%llu "
        "gapDrawIndexedVsConstFBetweenTop1CallNameCpuMs=%.3f "
        "gapDrawIndexedVsConstFBetweenTop1CallNameCpuMaxMs=%.3f "
        "gapDrawIndexedVsConstFBetweenTop2CallName=%s "
        "gapDrawIndexedVsConstFBetweenTop2CallNameSamples=%llu "
        "gapDrawIndexedVsConstFBetweenTop2CallNameCpuMs=%.3f "
        "gapDrawIndexedVsConstFBetweenTop2CallNameCpuMaxMs=%.3f "
        "gapDrawIndexedApplyStateBetweenTop1CallName=%s "
        "gapDrawIndexedApplyStateBetweenTop1CallNameSamples=%llu "
        "gapDrawIndexedApplyStateBetweenTop1CallNameCpuMs=%.3f "
        "gapDrawIndexedApplyStateBetweenTop1CallNameCpuMaxMs=%.3f "
        "gapDrawIndexedApplyStateBetweenTop2CallName=%s "
        "gapDrawIndexedApplyStateBetweenTop2CallNameSamples=%llu "
        "gapDrawIndexedApplyStateBetweenTop2CallNameCpuMs=%.3f "
        "gapDrawIndexedApplyStateBetweenTop2CallNameCpuMaxMs=%.3f "
        "gapDrawIndexedDrawIndexedBetweenTop1CallName=%s "
        "gapDrawIndexedDrawIndexedBetweenTop1CallNameSamples=%llu "
        "gapDrawIndexedDrawIndexedBetweenTop1CallNameCpuMs=%.3f "
        "gapDrawIndexedDrawIndexedBetweenTop1CallNameCpuMaxMs=%.3f "
        "gapDrawIndexedDrawIndexedBetweenTop2CallName=%s "
        "gapDrawIndexedDrawIndexedBetweenTop2CallNameSamples=%llu "
        "gapDrawIndexedDrawIndexedBetweenTop2CallNameCpuMs=%.3f "
        "gapDrawIndexedDrawIndexedBetweenTop2CallNameCpuMaxMs=%.3f "
        "gapDrawIndexedPsConstFBetweenTop1CallName=%s "
        "gapDrawIndexedPsConstFBetweenTop1CallNameSamples=%llu "
        "gapDrawIndexedPsConstFBetweenTop1CallNameCpuMs=%.3f "
        "gapDrawIndexedPsConstFBetweenTop1CallNameCpuMaxMs=%.3f "
        "gapDrawIndexedPsConstFBetweenTop2CallName=%s "
        "gapDrawIndexedPsConstFBetweenTop2CallNameSamples=%llu "
        "gapDrawIndexedPsConstFBetweenTop2CallNameCpuMs=%.3f "
        "gapDrawIndexedPsConstFBetweenTop2CallNameCpuMaxMs=%.3f",
        event ? event : "unknown", this,
        peInterAppendCallFamilyName(
            gapVsConstBetweenFamilies[0].family),
        static_cast<unsigned long long>(
            gapVsConstBetweenFamilies[0].samples),
        peInterAppendCallFamilyName(
            gapVsConstBetweenFamilies[1].family),
        static_cast<unsigned long long>(
            gapVsConstBetweenFamilies[1].samples),
        peInterAppendCallFamilyName(gapApplyBetweenFamilies[0].family),
        static_cast<unsigned long long>(
            gapApplyBetweenFamilies[0].samples),
        peInterAppendCallFamilyName(gapApplyBetweenFamilies[1].family),
        static_cast<unsigned long long>(
            gapApplyBetweenFamilies[1].samples),
        peInterAppendCallFamilyName(gapDrawBetweenFamilies[0].family),
        static_cast<unsigned long long>(
            gapDrawBetweenFamilies[0].samples),
        peInterAppendCallFamilyName(gapDrawBetweenFamilies[1].family),
        static_cast<unsigned long long>(
            gapDrawBetweenFamilies[1].samples),
        peInterAppendCallFamilyName(gapPsConstBetweenFamilies[0].family),
        static_cast<unsigned long long>(
            gapPsConstBetweenFamilies[0].samples),
        peInterAppendCallFamilyName(gapPsConstBetweenFamilies[1].family),
        static_cast<unsigned long long>(
            gapPsConstBetweenFamilies[1].samples),
        peInterAppendCallNameName(
            gapVsConstBetweenCallNames[0].callName),
        static_cast<unsigned long long>(
            gapVsConstBetweenCallNames[0].samples),
        static_cast<double>(gapVsConstBetweenCallNames[0].totalNs) /
            1000000.0,
        static_cast<double>(gapVsConstBetweenCallNames[0].maxNs) /
            1000000.0,
        peInterAppendCallNameName(
            gapVsConstBetweenCallNames[1].callName),
        static_cast<unsigned long long>(
            gapVsConstBetweenCallNames[1].samples),
        static_cast<double>(gapVsConstBetweenCallNames[1].totalNs) /
            1000000.0,
        static_cast<double>(gapVsConstBetweenCallNames[1].maxNs) /
            1000000.0,
        peInterAppendCallNameName(gapApplyBetweenCallNames[0].callName),
        static_cast<unsigned long long>(
            gapApplyBetweenCallNames[0].samples),
        static_cast<double>(gapApplyBetweenCallNames[0].totalNs) /
            1000000.0,
        static_cast<double>(gapApplyBetweenCallNames[0].maxNs) /
            1000000.0,
        peInterAppendCallNameName(gapApplyBetweenCallNames[1].callName),
        static_cast<unsigned long long>(
            gapApplyBetweenCallNames[1].samples),
        static_cast<double>(gapApplyBetweenCallNames[1].totalNs) /
            1000000.0,
        static_cast<double>(gapApplyBetweenCallNames[1].maxNs) /
            1000000.0,
        peInterAppendCallNameName(gapDrawBetweenCallNames[0].callName),
        static_cast<unsigned long long>(
            gapDrawBetweenCallNames[0].samples),
        static_cast<double>(gapDrawBetweenCallNames[0].totalNs) /
            1000000.0,
        static_cast<double>(gapDrawBetweenCallNames[0].maxNs) /
            1000000.0,
        peInterAppendCallNameName(gapDrawBetweenCallNames[1].callName),
        static_cast<unsigned long long>(
            gapDrawBetweenCallNames[1].samples),
        static_cast<double>(gapDrawBetweenCallNames[1].totalNs) /
            1000000.0,
        static_cast<double>(gapDrawBetweenCallNames[1].maxNs) /
            1000000.0,
        peInterAppendCallNameName(gapPsConstBetweenCallNames[0].callName),
        static_cast<unsigned long long>(
            gapPsConstBetweenCallNames[0].samples),
        static_cast<double>(gapPsConstBetweenCallNames[0].totalNs) /
            1000000.0,
        static_cast<double>(gapPsConstBetweenCallNames[0].maxNs) /
            1000000.0,
        peInterAppendCallNameName(gapPsConstBetweenCallNames[1].callName),
        static_cast<unsigned long long>(
            gapPsConstBetweenCallNames[1].samples),
        static_cast<double>(gapPsConstBetweenCallNames[1].totalNs) /
            1000000.0,
        static_cast<double>(gapPsConstBetweenCallNames[1].maxNs) /
            1000000.0);
    const auto logTransitionStats =
        [this, event](const char* prefix, const auto& transitions) {
            dxmt9DeviceInfoLog(
                "pe_recorder_gap_transition_stats event=%s device=%p "
                "%sBetweenGapTop1PrevFamily=%s "
                "%sBetweenGapTop1NextFamily=%s "
                "%sBetweenGapTop1Samples=%llu "
                "%sBetweenGapTop1Ms=%.3f "
                "%sBetweenGapTop1MaxMs=%.3f "
                "%sBetweenGapTop2PrevFamily=%s "
                "%sBetweenGapTop2NextFamily=%s "
                "%sBetweenGapTop2Samples=%llu "
                "%sBetweenGapTop2Ms=%.3f "
                "%sBetweenGapTop2MaxMs=%.3f",
                event ? event : "unknown", this,
                prefix,
                peInterAppendCallFamilyName(
                    transitions[0].prevFamily),
                prefix,
                peInterAppendCallFamilyName(
                    transitions[0].nextFamily),
                prefix,
                static_cast<unsigned long long>(
                    transitions[0].samples),
                prefix,
                static_cast<double>(transitions[0].totalNs) / 1000000.0,
                prefix,
                static_cast<double>(transitions[0].maxNs) / 1000000.0,
                prefix,
                peInterAppendCallFamilyName(
                    transitions[1].prevFamily),
                prefix,
                peInterAppendCallFamilyName(
                    transitions[1].nextFamily),
                prefix,
                static_cast<unsigned long long>(
                    transitions[1].samples),
                prefix,
                static_cast<double>(transitions[1].totalNs) / 1000000.0,
                prefix,
                static_cast<double>(transitions[1].maxNs) / 1000000.0);
        };
    logTransitionStats("gapDrawIndexedVsConstF",
                       gapVsConstBetweenTransitions);
    logTransitionStats("gapDrawIndexedApplyState",
                       gapApplyBetweenTransitions);
    logTransitionStats("gapDrawIndexedDrawIndexed",
                       gapDrawBetweenTransitions);
    logTransitionStats("gapDrawIndexedPsConstF",
                       gapPsConstBetweenTransitions);
    const auto logNameTransitionStats =
        [this, event](const char* prefix, const auto& transitions) {
            dxmt9DeviceInfoLog(
                "pe_recorder_gap_name_transition_stats event=%s device=%p "
                "%sBetweenGapTop1PrevCallName=%s "
                "%sBetweenGapTop1NextCallName=%s "
                "%sBetweenGapTop1NameSamples=%llu "
                "%sBetweenGapTop1NameMs=%.3f "
                "%sBetweenGapTop1NameMaxMs=%.3f "
                "%sBetweenGapTop2PrevCallName=%s "
                "%sBetweenGapTop2NextCallName=%s "
                "%sBetweenGapTop2NameSamples=%llu "
                "%sBetweenGapTop2NameMs=%.3f "
                "%sBetweenGapTop2NameMaxMs=%.3f",
                event ? event : "unknown", this,
                prefix,
                peInterAppendCallNameName(
                    transitions[0].prevCallName),
                prefix,
                peInterAppendCallNameName(
                    transitions[0].nextCallName),
                prefix,
                static_cast<unsigned long long>(
                    transitions[0].samples),
                prefix,
                static_cast<double>(transitions[0].totalNs) / 1000000.0,
                prefix,
                static_cast<double>(transitions[0].maxNs) / 1000000.0,
                prefix,
                peInterAppendCallNameName(
                    transitions[1].prevCallName),
                prefix,
                peInterAppendCallNameName(
                    transitions[1].nextCallName),
                prefix,
                static_cast<unsigned long long>(
                    transitions[1].samples),
                prefix,
                static_cast<double>(transitions[1].totalNs) / 1000000.0,
                prefix,
                static_cast<double>(transitions[1].maxNs) / 1000000.0);
        };
    logNameTransitionStats("gapDrawIndexedVsConstF",
                           gapVsConstBetweenNameTransitions);
    logNameTransitionStats("gapDrawIndexedApplyState",
                           gapApplyBetweenNameTransitions);
    logNameTransitionStats("gapDrawIndexedDrawIndexed",
                           gapDrawBetweenNameTransitions);
    logNameTransitionStats("gapDrawIndexedPsConstF",
                           gapPsConstBetweenNameTransitions);
    const auto logCallSiteStats =
        [this, event](const char* prefix, const auto& sites) {
            const auto callerInfo0 =
                dxmt9PeResolveCallerModule(sites[0].callerPc);
            const auto callerInfo1 =
                dxmt9PeResolveCallerModule(sites[1].callerPc);
            dxmt9DeviceInfoLog(
                "pe_recorder_gap_callsite_stats event=%s device=%p "
                "%sBetweenGapSiteTop1PrevCallName=%s "
                "%sBetweenGapSiteTop1NextCallName=%s "
                "%sBetweenGapSiteTop1CallerModule=%s "
                "%sBetweenGapSiteTop1CallerRva=0x%llx "
                "%sBetweenGapSiteTop1Samples=%llu "
                "%sBetweenGapSiteTop1Ms=%.3f "
                "%sBetweenGapSiteTop1MaxMs=%.3f "
                "%sBetweenGapSiteTop2PrevCallName=%s "
                "%sBetweenGapSiteTop2NextCallName=%s "
                "%sBetweenGapSiteTop2CallerModule=%s "
                "%sBetweenGapSiteTop2CallerRva=0x%llx "
                "%sBetweenGapSiteTop2Samples=%llu "
                "%sBetweenGapSiteTop2Ms=%.3f "
                "%sBetweenGapSiteTop2MaxMs=%.3f",
                event ? event : "unknown", this,
                prefix,
                peInterAppendCallNameName(sites[0].prevCallName),
                prefix,
                peInterAppendCallNameName(sites[0].nextCallName),
                prefix,
                dxmt9PeCallerModuleLeaf(callerInfo0),
                prefix,
                static_cast<unsigned long long>(callerInfo0.rva),
                prefix,
                static_cast<unsigned long long>(sites[0].samples),
                prefix,
                static_cast<double>(sites[0].totalNs) / 1000000.0,
                prefix,
                static_cast<double>(sites[0].maxNs) / 1000000.0,
                prefix,
                peInterAppendCallNameName(sites[1].prevCallName),
                prefix,
                peInterAppendCallNameName(sites[1].nextCallName),
                prefix,
                dxmt9PeCallerModuleLeaf(callerInfo1),
                prefix,
                static_cast<unsigned long long>(callerInfo1.rva),
                prefix,
                static_cast<unsigned long long>(sites[1].samples),
                prefix,
                static_cast<double>(sites[1].totalNs) / 1000000.0,
                prefix,
                static_cast<double>(sites[1].maxNs) / 1000000.0);
        };
    logCallSiteStats("gapDrawIndexedVsConstF",
                     gapVsConstBetweenCallSites);
    logCallSiteStats("gapDrawIndexedApplyState",
                     gapApplyBetweenCallSites);
    logCallSiteStats("gapDrawIndexedDrawIndexed",
                     gapDrawBetweenCallSites);
    logCallSiteStats("gapDrawIndexedPsConstF",
                     gapPsConstBetweenCallSites);
}

void D3D9DeviceImpl::logPeStatsDecimation() {
    const std::uint32_t decimationN = dxmt9PeStatsDecimationN();
    if (decimationN == 0) {
        return;
    }
    const auto& appendStats = peChunkAppendDecimatedStats_;
    const auto& constSetterStats = peConstSetterDecimatedStats();
    // Per-call register-count split of the const_setter scope. A flat
    // ns/sample across buckets means the fixed per-call entry cost
    // dominates; a slope means the per-element compare loop does.
    std::string constSetterBucketText;
    {
        const auto& buckets = peConstSetterDecimatedBuckets();
        static const char* const kNames[PeDecimatedBucketStats::kBuckets] = {
            "1", "2", "3_4", "5_8", "9_16", "gt16"};
        for (int i = 0; i < PeDecimatedBucketStats::kBuckets; ++i) {
            const auto& b = buckets.bucket[i];
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          " const_setter_n%s_events=%llu"
                          " const_setter_n%s_sampled=%llu"
                          " const_setter_n%s_sampled_ms=%.3f",
                          kNames[i], static_cast<unsigned long long>(b.events),
                          kNames[i], static_cast<unsigned long long>(b.sampled),
                          kNames[i], static_cast<double>(b.sampledNs) / 1.0e6);
            constSetterBucketText += buf;
        }
    }
    std::string appendTypeText;
    {
        static const char* const kTypeNames[kPeAppendTypeBuckets] = {
            "draw", "drawidx", "drawup", "applystate",
            "vsconst", "psconst", "clear", "other"};
        for (std::size_t i = 0; i < kPeAppendTypeBuckets; ++i) {
            if (peAppendTypeCounts_[i] == 0) continue;
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          " append_%s_calls=%llu append_%s_bytes=%llu",
                          kTypeNames[i],
                          static_cast<unsigned long long>(peAppendTypeCounts_[i]),
                          kTypeNames[i],
                          static_cast<unsigned long long>(peAppendTypeBytes_[i]));
            appendTypeText += buf;
        }
    }
    dxmt9DeviceInfoLog(
        "[dxmt9-pe-decimated] presents=%llu decimation=%u "
        "append_events=%llu append_sampled=%llu append_sampled_ms=%.3f "
        "const_setter_events=%llu const_setter_sampled=%llu const_setter_sampled_ms=%.3f "
        "const_flush_events=%llu const_flush_sampled=%llu const_flush_sampled_ms=%.3f "
        "draw_packet_events=%llu draw_packet_sampled=%llu draw_packet_sampled_ms=%.3f "
        "identity_getter_calls=%llu null_scope_sampled=%llu null_scope_ms=%.3f "
        "append_encode_sampled=%llu append_encode_ms=%.3f "
        "append_flush_sampled=%llu append_flush_ms=%.3f "
        "entry_const_events=%llu entry_const_sampled=%llu entry_const_ms=%.3f "
        "entry_draw_events=%llu entry_draw_sampled=%llu entry_draw_ms=%.3f "
        "entry_state_events=%llu entry_state_sampled=%llu entry_state_ms=%.3f "
        "draw_swvp_sampled=%llu draw_swvp_ms=%.3f "
        "draw_record_sampled=%llu draw_record_ms=%.3f "
        "%s%s",
        static_cast<unsigned long long>(peStatsDecimationPresents_),
        decimationN,
        static_cast<unsigned long long>(appendStats.events),
        static_cast<unsigned long long>(appendStats.sampled),
        static_cast<double>(appendStats.sampledNs) / 1.0e6,
        static_cast<unsigned long long>(constSetterStats.events),
        static_cast<unsigned long long>(constSetterStats.sampled),
        static_cast<double>(constSetterStats.sampledNs) / 1.0e6,
        static_cast<unsigned long long>(peConstFlushDecimatedStats_.events),
        static_cast<unsigned long long>(peConstFlushDecimatedStats_.sampled),
        static_cast<double>(peConstFlushDecimatedStats_.sampledNs) / 1.0e6,
        static_cast<unsigned long long>(peDrawPacketDecimatedStats_.events),
        static_cast<unsigned long long>(peDrawPacketDecimatedStats_.sampled),
        static_cast<double>(peDrawPacketDecimatedStats_.sampledNs) / 1.0e6,
        static_cast<unsigned long long>(
            dxmt9::d3d9::pe::wireIdentityGetterCallCount()),
        static_cast<unsigned long long>(peDecimatedNullScopeStats().sampled),
        static_cast<double>(peDecimatedNullScopeStats().sampledNs) / 1.0e6,
        static_cast<unsigned long long>(peAppendPhaseEncode_.sampled),
        static_cast<double>(peAppendPhaseEncode_.sampledNs) / 1.0e6,
        static_cast<unsigned long long>(peAppendPhaseFlush_.sampled),
        static_cast<double>(peAppendPhaseFlush_.sampledNs) / 1.0e6,
        static_cast<unsigned long long>(peEntryConstDecimatedStats_.events),
        static_cast<unsigned long long>(peEntryConstDecimatedStats_.sampled),
        static_cast<double>(peEntryConstDecimatedStats_.sampledNs) / 1.0e6,
        static_cast<unsigned long long>(peEntryDrawDecimatedStats_.events),
        static_cast<unsigned long long>(peEntryDrawDecimatedStats_.sampled),
        static_cast<double>(peEntryDrawDecimatedStats_.sampledNs) / 1.0e6,
        static_cast<unsigned long long>(peEntryStateDecimatedStats_.events),
        static_cast<unsigned long long>(peEntryStateDecimatedStats_.sampled),
        static_cast<double>(peEntryStateDecimatedStats_.sampledNs) / 1.0e6,
        static_cast<unsigned long long>(peDrawPhaseSwvpDecimatedStats_.sampled),
        static_cast<double>(peDrawPhaseSwvpDecimatedStats_.sampledNs) / 1.0e6,
        static_cast<unsigned long long>(peDrawPhaseRecordDecimatedStats_.sampled),
        static_cast<double>(peDrawPhaseRecordDecimatedStats_.sampledNs) / 1.0e6,
        constSetterBucketText.c_str(),
        appendTypeText.c_str());
}

void D3D9DeviceImpl::startPeThreadSamplerIfRequested() {
    if (!dxmt9PeThreadSamplerEnabled()) {
        return;
    }
    const std::uint32_t hz = dxmt9PeThreadSamplerHz();
    // A second device must not add a second thread stopping the same
    // target, so the sampler refuses; distinguish that from an OS failure
    // rather than reporting a stale GetLastError() for it.
    const bool alreadyLive =
        dxmt9::d3d9::pe::PeThreadSampler::processSamplerIsLive();
    peThreadSampler_ =
        dxmt9::d3d9::pe::PeThreadSampler::startForThread(GetCurrentThreadId(), hz);
    if (!peThreadSampler_) {
        if (alreadyLive) {
            dxmt9PeThreadSamplerInfoLog(
                "not_started reason=already_running_for_process thread_id=0x%lx",
                static_cast<unsigned long>(GetCurrentThreadId()));
        } else {
            dxmt9PeThreadSamplerInfoLog(
                "not_started reason=start_failed thread_id=0x%lx hz=%u "
                "error=0x%08lx",
                static_cast<unsigned long>(GetCurrentThreadId()), hz,
                static_cast<unsigned long>(GetLastError()));
        }
        return;
    }
    dxmt9PeThreadSamplerInfoLog(
        "started thread_id=0x%lx hz=%u interval_ms=%u",
        static_cast<unsigned long>(peThreadSampler_->targetThreadId()), hz,
        peThreadSampler_->intervalMs());
}

void D3D9DeviceImpl::logPeThreadSampler() {
    if (!peThreadSampler_) {
        return;
    }
    dxmt9::d3d9::pe::PeSamplerSnapshot snap;
    peThreadSampler_->snapshot(snap);
    dxmt9PeThreadSamplerInfoLog(
        "presents=%llu samples=%llu suspend_failures=%llu "
        "ctx_failures=%llu resume_failures=%llu hz=%u module_table=%u",
        static_cast<unsigned long long>(peThreadSamplerPresents_),
        static_cast<unsigned long long>(snap.samples),
        static_cast<unsigned long long>(snap.suspendFailures),
        static_cast<unsigned long long>(snap.contextFailures),
        static_cast<unsigned long long>(snap.resumeFailures),
        snap.hz,
        snap.moduleTableReady ? 1u : 0u);
    for (std::size_t i = 0; i < snap.moduleRows; ++i) {
        dxmt9PeThreadSamplerInfoLog(
            "module=%s samples=%llu",
            snap.moduleNames[i],
            static_cast<unsigned long long>(snap.topModules[i].samples));
    }
    if (snap.selfModuleName[0] != '\0') {
        // Names the module the buckets below are relative to, so the join
        // tool subtracts the right [dxmt9-pe-module-map] base for the RVA.
        dxmt9PeThreadSamplerInfoLog("selfpc_module=%s", snap.selfModuleName);
    }
    for (std::size_t i = 0; i < snap.selfPcRows; ++i) {
        dxmt9PeThreadSamplerInfoLog(
            "selfpc bucket=0x%llx samples=%llu",
            static_cast<unsigned long long>(snap.topSelfPc[i].bucket),
            static_cast<unsigned long long>(snap.topSelfPc[i].samples));
    }
    dxmt9PeThreadSamplerInfoLog(
        "selfpc_overflow=%llu",
        static_cast<unsigned long long>(snap.selfPcOverflow));
}

void D3D9DeviceImpl::logRenderTapeMutationFailure(
    const char *route,
    dxmt9::d3d9::RenderTapeBlockMutationStatus status,
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::uint32_t subresource, std::uint32_t fullRowBytes,
    std::uint32_t fullRows, std::uint32_t rowBytes,
    std::uint32_t rows, std::uint32_t pitch,
    std::span<const std::byte> bytes) const noexcept {
    if (!renderTapeRegistry_ || renderTapeRegistry_->invalid)
        return;
    const auto *entry = findRenderTapeObject(object);
    D9CSurfaceDesc desc{};
    const bool descValid =
        entry && renderTapeObjectSubresourceDesc(
                     *entry, object, subresource, desc);
    dxmt9::d3d9::RenderTapeTextureDescriptorV2 texture{};
    const bool textureValid =
        entry && object.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE &&
        dxmt9::d3d9::renderTapeLoadTextureDescriptorV2(
            entry->descriptor, texture);
    const std::size_t contentBytes =
        entry && subresource < entry->content.size()
            ? entry->content[subresource].size()
            : 0u;
    dxmt9DeviceInfoLog(
        "render_tape_capture mutation_rejected route=%s status=%u "
        "kind=%u generation=%u object_id=%llu subresource=%u admitted=%d "
        "entry=%d descriptor_bytes=%zu desc_valid=%d format=%u width=%u "
        "height=%u depth=%u texture_v2=%d dimension=%u mips=%u "
        "subresources=%u disposition=%u content_slots=%zu content_bytes=%zu "
        "full_row_bytes=%u full_rows=%u row_bytes=%u rows=%u pitch=%u "
        "mutation_bytes=%zu",
        route, static_cast<unsigned>(status), object.identity.kind,
        object.identity.generation,
        static_cast<unsigned long long>(object.identity.objectId),
        subresource, renderTapeObjectAdmitted(object.identity) ? 1 : 0,
        entry ? 1 : 0, entry ? entry->descriptor.size() : 0u,
        descValid ? 1 : 0, desc.format, desc.width, desc.height, desc.depth,
        textureValid ? 1 : 0, texture.dimension, texture.mipLevelCount,
        texture.subresourceCount, texture.initialContentDisposition,
        entry ? entry->content.size() : 0u, contentBytes, fullRowBytes,
        fullRows, rowBytes, rows, pitch, bytes.size());
}

bool D3D9DeviceImpl::constShadowElemEquals(const ConstShadow& shadow,
                                  std::uint32_t reg,
                                  const std::uint8_t* src,
                                  std::size_t elemSize) {
    const std::size_t offset = static_cast<std::size_t>(reg) * elemSize;
    if (shadow.values.size() >= offset + elemSize) {
        return std::memcmp(shadow.values.data() + offset, src, elemSize) == 0;
    }
    for (std::size_t i = 0; i < elemSize; ++i) {
        if (src[i] != 0u) {
            return false;
        }
    }
    return true;
}
