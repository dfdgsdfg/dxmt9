/* src/d3d9/d3d9_pe_device_recorder.cpp — cold recorder settlement/flush.
 *
 * Commit, cleanup, and constant-drain paths are intentionally out of the
 * device class header. Hot appendRecord/timers/setters/draw/present code keeps
 * its existing placement and calls these semantic cold boundaries normally. */

#include "d3d9_pe_device_impl.hpp"

void D3D9DeviceImpl::clearPendingCommandChunk(
    dxmt9::d3d9::pe::RecorderCommitEvent discardEvent) {
    const auto discardPlan = dxmt9::d3d9::pe::settleRecorderCommit({
        .phase = commandChunk_.sealed()
            ? dxmt9::d3d9::pe::RecorderCommitPhase::Sealed
            : dxmt9::d3d9::pe::RecorderCommitPhase::Unsealed,
        .event = discardEvent,
    });
    const bool discardAdmitted =
        discardPlan.valid && discardPlan.resetBuilder &&
        discardPlan.action ==
            dxmt9::d3d9::pe::RecorderCommitAction::DiscardAll;
    DXMT_ASSERT(discardAdmitted);
    if (!discardAdmitted && peCaptureState_) {
        markRenderTapeInvalidOnce("commit_discard_settlement");
    }
    // Discarded chunks never acquire a tape ObjectDestroy event. Drain
    // the logical pending refs before raw D9C retainer reset.
    drainPendingRenderTapeChunk(false);
    // Discard path (device teardown, Reset, ResetEx): release the warm
    // retainer pins too, so nothing is still holding a unix object when
    // dxmt9c_device_reset* / dxmt9c_device_release runs.
    commandChunk_.resetAndReleaseRetained();
}

HRESULT D3D9DeviceImpl::commitPendingCommandChunk(
    PeRecorderFlushReason commitReason, const D9CCommandChunk& chunk,
    const PeCommandChunkCommitInfo& info,
    dxmt9::d3d9::pe::RecorderCommitPhase* settledPhase) {
            PeCaptureState *const captureState =
                peCaptureState_ ? &*peCaptureState_ : nullptr;
            const bool capturePresent = captureState &&
                captureState->renderTapeCapture.state() ==
                    dxmt9::d3d9::RenderTapeCaptureState::Capturing &&
                chunkHasPresentRecord(chunk);
            bool presentMirrorReserved = false;
            if (capturePresent) {
                dxmt9DeviceInfoLog(
                    "render_tape_capture captured_present_reserve_begin "
                    "token=%llu",
                    static_cast<unsigned long long>(
                        captureState->renderTapeActiveCaptureToken));
                const HRESULT reserveHr = hr32(
                    dxmt9c_device_reserve_render_tape_present_capture(dev_));
                dxmt9DeviceInfoLog(
                    "render_tape_capture captured_present_reserve_end "
                    "token=%llu hr=0x%08x disposition=%s",
                    static_cast<unsigned long long>(
                        captureState->renderTapeActiveCaptureToken),
                    static_cast<unsigned>(reserveHr),
                    SUCCEEDED(reserveHr) ? "reserved" : "failed");
                if (SUCCEEDED(reserveHr)) {
                    presentMirrorReserved = true;
                } else {
                    abortRenderTapeCapture("present_output_reserve");
                }
            }
            PeDiagnosticsState *const chunkDiagnostics =
                dxmt9PeChunkCommitDiagnosticsEnabled()
                    ? diagnostics_.get()
                    : nullptr;
            PePresentCadenceClaim chunkCadence{};
            std::int64_t entryNs = 0;
            std::int64_t priorReturnNs = 0;
            if (chunkDiagnostics) {
                if (chunkDiagnostics->config.recorderStats) {
                    chunkCadence = claimPeFirstChunkAfterPresent();
                }
                entryNs = peDiagnosticsRead(
                    chunkDiagnostics, [](PeDiagnosticsState&) noexcept {
                        return dxmt9SteadyClockNs(
                            std::chrono::steady_clock::now());
                    });
                priorReturnNs =
                    chunkDiagnostics->peRecorderLastChunkReturnNs_;
            }
            const bool captureWasActive = captureState &&
                captureState->renderTapeCapture.state() ==
                    dxmt9::d3d9::RenderTapeCaptureState::Capturing;
            const bool captureChunkPrepared = captureWasActive &&
                prepareRenderTapeChunkCapture(chunk, info);
            D9CCommandChunk submittedChunk = chunk;
            if (captureChunkPrepared && captureState &&
                captureState->renderTapeCapture.state() ==
                    dxmt9::d3d9::RenderTapeCaptureState::Capturing &&
                captureState->renderTapeActiveCaptureToken != 0u) {
                submittedChunk.renderTapeCaptureToken =
                    captureState->renderTapeActiveCaptureToken;
                submittedChunk.renderTapeEventOrdinal =
                    static_cast<std::uint64_t>(
                        captureState->renderTapeCapture.eventCount()) + 1u;
            }
            const HRESULT hr = hr32(
                dxmt9c_device_commit_chunk(dev_, &submittedChunk));
            const auto bridgePlan = dxmt9::d3d9::pe::settleRecorderCommit({
                .phase = dxmt9::d3d9::pe::RecorderCommitPhase::Sealed,
                .event = SUCCEEDED(hr)
                    ? dxmt9::d3d9::pe::RecorderCommitEvent::BridgeAccepted
                    : dxmt9::d3d9::pe::RecorderCommitEvent::BridgeFailed,
            });
            if (!bridgePlan.valid) {
                DXMT_ASSERT(false && "invalid bridge settlement");
                if (captureState) {
                    markRenderTapeInvalidOnce("commit_bridge_settlement");
                }
                return D3DERR_INVALIDCALL;
            }
            const std::int64_t returnNs = peDiagnosticsRead(
                chunkDiagnostics, [](PeDiagnosticsState&) noexcept {
                    return dxmt9SteadyClockNs(
                        std::chrono::steady_clock::now());
                });
            if (chunkDiagnostics) {
                chunkDiagnostics->peRecorderLastChunkReturnNs_ = returnNs;
            }
            const std::uint64_t fillGapNs =
                priorReturnNs > 0 && entryNs > priorReturnNs
                ? static_cast<std::uint64_t>(entryNs - priorReturnNs)
                : 0;
            const std::uint64_t activeFillNs =
                chunkDiagnostics &&
                chunkDiagnostics->peRecorderCurrentChunkFirstAppendNs_ > 0 &&
                entryNs > chunkDiagnostics->peRecorderCurrentChunkFirstAppendNs_
                ? static_cast<std::uint64_t>(
                    entryNs -
                        chunkDiagnostics->peRecorderCurrentChunkFirstAppendNs_)
                : 0;
            const std::uint64_t bridgeNs =
                returnNs > entryNs
                ? static_cast<std::uint64_t>(returnNs - entryNs)
                : 0;
            if (SUCCEEDED(hr) && chunkDiagnostics) {
                chunkDiagnostics->peRecorderCurrentChunkFirstAppendNs_ = 0;
                chunkDiagnostics->peRecorderLastAppendReturnNs_ = 0;
                chunkDiagnostics->peRecorderLastAppendCallEntryNs_ = 0;
                chunkDiagnostics->peRecorderLastAppendCallExitNs_ = 0;
                chunkDiagnostics->peRecorderLastAppendRecordType_ = 0;
                if (chunkDiagnostics->config.recorderStats) {
                    resetPeBetweenCallsWindow();
                }
            }
            if (chunkDiagnostics &&
                chunkDiagnostics->config.recorderStats) {
                logPeFirstChunkAfterPresent(commitReason, chunkCadence, hr,
                                            info);
            }
            if (FAILED(hr)) {
                // A failed bridge leaves the sealed builder projection and
                // all pending refs/handles intact for the next attempt.
                // Capture is a post-acceptance phase and must not be
                // evaluated (or allowed to drain) on this path.
                if (presentMirrorReserved) {
                    dxmt9c_device_cancel_render_tape_present_capture(dev_);
                }
                if (captureState &&
                    captureState->renderTapeCapture.state() ==
                        dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
                    abortRenderTapeCapture("bridge_commit");
                }
                return hr;
            }
            auto settlementPhase = bridgePlan.next;
            const auto capturePlan = dxmt9::d3d9::pe::settleRecorderCommit({
                .phase = settlementPhase,
                .event = captureChunkPrepared
                    ? dxmt9::d3d9::pe::RecorderCommitEvent::CaptureMaterialized
                    : captureWasActive
                        ? dxmt9::d3d9::pe::RecorderCommitEvent::CaptureRejected
                        : dxmt9::d3d9::pe::RecorderCommitEvent::CaptureSkipped,
            });
            if (!capturePlan.valid) {
                DXMT_ASSERT(false && "invalid capture settlement");
                if (captureState) {
                    markRenderTapeInvalidOnce("commit_capture_settlement");
                }
            } else {
                settlementPhase = capturePlan.next;
            }
            if (settledPhase) {
                *settledPhase = settlementPhase;
            }
            if (SUCCEEDED(hr)) {
                ++commandChunkCommits_;
                commandChunkRecords_ += info.recordCount;
                commandChunkBytes_ += info.wireBytes;
                if (chunkDiagnostics) {
                    recordPeChunkCommit(commitReason, info.recordCount,
                                        info.payloadBytes,
                                        info.handleCount, info.wireBytes,
                                        fillGapNs, activeFillNs, bridgeNs);
                }
                // Copy the exact sealed canonical bytes only after the
                // bridge accepted them. The source remains valid until
                // flushPendingCommandChunk resets its builder below.
                if (captureChunkPrepared) {
                    captureCommittedRenderTapeChunk(chunk, info);
                } else if (captureState && !captureWasActive) {
                    observeRenderTapeFirstAccessChunk(chunk, info);
                }
            }
            if (presentMirrorReserved) {
                if (FAILED(hr)) {
                    dxmt9c_device_cancel_render_tape_present_capture(dev_);
                } else if (captureState &&
                           captureState->renderTapeCapture.state() ==
                               dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
                    const auto outputBpp = captureState->renderTapeOutputDesc
                        ? dxmt9::d3d9::renderTapeLinearBytesPerPixel(
                              captureState->renderTapeOutputDesc->format)
                        : 0u;
                    const bool outputExtentFits = captureState->renderTapeOutputDesc &&
                        outputBpp != 0u &&
                        captureState->renderTapeOutputDesc->height != 0u &&
                        captureState->renderTapeOutputDesc->width <=
                            std::numeric_limits<std::uint64_t>::max() /
                                captureState->renderTapeOutputDesc->height / outputBpp;
                    const auto outputBytes = outputExtentFits
                        ? static_cast<std::uint64_t>(
                              captureState->renderTapeOutputDesc->width) *
                              captureState->renderTapeOutputDesc->height * outputBpp
                        : 0u;
                    bool outputBufferReady = outputExtentFits &&
                        outputBytes <=
                            std::numeric_limits<std::size_t>::max();
                    std::vector<std::byte> outputPixels;
                    if (outputBufferReady) {
                        try {
                            outputPixels.resize(
                                static_cast<std::size_t>(outputBytes));
                        } catch (...) {
                            outputBufferReady = false;
                        }
                    }
                    if (!outputBufferReady) {
                        dxmt9c_device_cancel_render_tape_present_capture(dev_);
                        abortRenderTapeCapture("present_output_buffer");
                        return hr;
                    }
                    D9CRenderTapePresentCaptureResult output{};
                    const HRESULT finishHr = hr32(
                        dxmt9c_device_finish_render_tape_present_capture(
                            dev_, &output, outputPixels.data(),
                            outputPixels.size()));
                    std::vector<std::byte> sourcePixels;
                    D9CRenderTapePresentSourceCaptureResult source{};
                    HRESULT sourceFinishHr = D3DERR_NOTAVAILABLE;
                    if (SUCCEEDED(finishHr) && outputBufferReady) {
                        try {
                            sourcePixels.resize(
                                static_cast<std::size_t>(outputBytes));
                            sourceFinishHr = hr32(
                                dxmt9c_device_finish_render_tape_present_source_capture(
                                    dev_, &source, sourcePixels.data(),
                                    sourcePixels.size()));
                        } catch (...) {
                            sourceFinishHr = D3DERR_OUTOFVIDEOMEMORY;
                        }
                    }
                    bool sourceDigestMatches = false;
                    if (SUCCEEDED(sourceFinishHr) &&
                        source.status ==
                            D9C_RENDER_TAPE_PRESENT_SOURCE_CAPTURE_COMPLETE) {
                        dxmt9::d3d9::RenderTapeDigest sourceDigest{};
                        std::memcpy(sourceDigest.data(), source.sha256,
                                    sourceDigest.size());
                        sourceDigestMatches =
                            dxmt9::d3d9::RenderTapeCaptureSession::sha256(
                                sourcePixels) == sourceDigest;
                    }
                    if (SUCCEEDED(finishHr) &&
                        output.status ==
                            D9C_RENDER_TAPE_PRESENT_CAPTURE_COMPLETE &&
                        outputExtentFits &&
                        output.width == captureState->renderTapeOutputDesc->width &&
                        output.height == captureState->renderTapeOutputDesc->height &&
                        output.format == captureState->renderTapeOutputDesc->format &&
                        output.byteCount == outputBytes &&
                        SUCCEEDED(sourceFinishHr) &&
                        source.status ==
                            D9C_RENDER_TAPE_PRESENT_SOURCE_CAPTURE_COMPLETE &&
                        source.width == captureState->renderTapeOutputDesc->width &&
                        source.height == captureState->renderTapeOutputDesc->height &&
                        source.format == captureState->renderTapeOutputDesc->format &&
                        source.byteCount == outputBytes &&
                        sourceDigestMatches) {
                        dxmt9::d3d9::RenderTapeDigest digest{};
                        std::memcpy(digest.data(), output.sha256,
                                    digest.size());
                        captureState->renderTapeExpectedDigest = digest;
                        captureState->renderTapeExpectedPixels = std::move(outputPixels);
                        captureState->renderTapeExpectedSourcePixels =
                            std::move(sourcePixels);
                    } else {
                        dxmt9c_device_cancel_render_tape_present_capture(dev_);
                        abortRenderTapeCapture("present_output_or_source_finish");
                    }
                } else {
                    dxmt9c_device_cancel_render_tape_present_capture(dev_);
                }
            }
            return hr;
}

HRESULT D3D9DeviceImpl::flushPendingCommandChunk(PeRecorderFlushReason reason) {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
    if (!commandChunkNegotiated_) {
        return D3DERR_NOTAVAILABLE;
    }
    if (commandChunk_.recordCount() == 0u) {
        return S_OK;
    }
    const auto payloadBytes = commandChunk_.payloadBytes();
    const auto sealed = commandChunk_.seal();
    if (!sealed.valid() || sealed.blob.size() > 0xffffffffull) {
        const auto sealPlan = dxmt9::d3d9::pe::settleRecorderCommit({
            .phase = dxmt9::d3d9::pe::RecorderCommitPhase::Unsealed,
            .event = dxmt9::d3d9::pe::RecorderCommitEvent::SealFailed,
        });
        if (!sealPlan.valid || !sealPlan.preserveRetryBytes) {
            return D3DERR_INVALIDCALL;
        }
        return D3DERR_INVALIDCALL;
    }
    const auto sealPlan = dxmt9::d3d9::pe::settleRecorderCommit({
        .phase = dxmt9::d3d9::pe::RecorderCommitPhase::Unsealed,
        .event = dxmt9::d3d9::pe::RecorderCommitEvent::SealAccepted,
    });
    if (!sealPlan.valid || sealPlan.next !=
            dxmt9::d3d9::pe::RecorderCommitPhase::Sealed) {
        return D3DERR_INVALIDCALL;
    }
    D9CCommandChunk chunk{};
    chunk.version = D9C_COMMAND_CHUNK_VERSION;
    chunk.recordCount = sealed.recordCount;
    chunk.recordBytes = static_cast<std::uint32_t>(sealed.blob.size());
    chunk.records = toWireHandle(sealed.blob.data());
    chunk.handleCount = sealed.handleCount;
    const PeCommandChunkCommitInfo info{
        .recordCount = sealed.recordCount,
        .payloadBytes = static_cast<std::uint32_t>(std::min<std::size_t>(
            payloadBytes, std::numeric_limits<std::uint32_t>::max())),
        .handleCount = sealed.handleCount,
        .wireBytes = chunk.recordBytes,
    };
    dxmt9::d3d9::pe::RecorderCommitPhase settlementPhase =
        dxmt9::d3d9::pe::RecorderCommitPhase::Sealed;
    const HRESULT hr =
        commitPendingCommandChunk(reason, chunk, info, &settlementPhase);
    if (SUCCEEDED(hr)) {
        const auto beginDrain = dxmt9::d3d9::pe::settleRecorderCommit({
            .phase = settlementPhase,
            .event = dxmt9::d3d9::pe::RecorderCommitEvent::DrainPending,
        });
        const bool beginDrainAdmitted =
            beginDrain.valid &&
            beginDrain.action ==
                dxmt9::d3d9::pe::RecorderCommitAction::BeginDrain;
        DXMT_ASSERT(beginDrainAdmitted);
        if (!beginDrainAdmitted && peCaptureState_) {
            markRenderTapeInvalidOnce("commit_begin_drain_settlement");
        }
        const auto drainPhase = beginDrainAdmitted
            ? beginDrain.next
            : dxmt9::d3d9::pe::RecorderCommitPhase::Draining;
        if (peCaptureState_) {
            const bool recordDestroy =
                peCaptureState_->renderTapeCapture.state() ==
                    dxmt9::d3d9::RenderTapeCaptureState::Capturing;
            // Capture materialization/rejection has settled before this
            // call. Drain exactly once, then reset the builder.
            drainPendingRenderTapeChunk(recordDestroy);
        }
        const auto finishDrain = dxmt9::d3d9::pe::settleRecorderCommit({
            .phase = drainPhase,
            .event = dxmt9::d3d9::pe::RecorderCommitEvent::DrainComplete,
        });
        const auto resetPlan = dxmt9::d3d9::pe::settleRecorderCommit({
            .phase = finishDrain.valid
                ? finishDrain.next
                : dxmt9::d3d9::pe::RecorderCommitPhase::Drained,
            .event = dxmt9::d3d9::pe::RecorderCommitEvent::BuilderReset,
        });
        const auto warmPlan = dxmt9::d3d9::pe::settleRecorderCommit({
            .phase = resetPlan.valid
                ? resetPlan.next
                : dxmt9::d3d9::pe::RecorderCommitPhase::Reset,
            .event = dxmt9::d3d9::pe::RecorderCommitEvent::WarmEpochAdvance,
        });
        const bool finishDrainAdmitted =
            finishDrain.valid &&
            finishDrain.action ==
                dxmt9::d3d9::pe::RecorderCommitAction::FinishDrain;
        const bool resetAdmitted =
            resetPlan.valid && resetPlan.resetBuilder &&
            resetPlan.action ==
                dxmt9::d3d9::pe::RecorderCommitAction::ResetBuilder;
        const bool warmAdmitted =
            warmPlan.valid && warmPlan.advanceWarmEpoch &&
            warmPlan.action ==
                dxmt9::d3d9::pe::RecorderCommitAction::AdvanceWarmEpoch;
        DXMT_ASSERT(finishDrainAdmitted && resetAdmitted && warmAdmitted);
        if ((!finishDrainAdmitted || !resetAdmitted || !warmAdmitted) &&
            peCaptureState_) {
            markRenderTapeInvalidOnce("commit_cleanup_settlement");
        }
        commandChunk_.reset();
    }
    return hr;
}

HRESULT D3D9DeviceImpl::flushPeRecorder(
    PeRecorderFlushReason reason) {
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return barrierHr;
    return flushPendingCommandChunk(reason);
}

HRESULT D3D9DeviceImpl::appendSetConstRecord(uint32_t recordType, UINT start, UINT count,
                             const void* data, std::size_t elemSize,
                             ConstShadow& settlementOwner) {
    const std::uint64_t payload64 = static_cast<std::uint64_t>(count) * elemSize;
    if (payload64 > 0xffffffffull - kLegacySetConstSizeHint) {
        return D3DERR_INVALIDCALL;
    }
    const std::uint32_t payloadBytes = static_cast<std::uint32_t>(payload64);
    if (payloadBytes != 0 && !data) {
        return D3DERR_INVALIDCALL;
    }

    // sizeHint keeps the legacy header+payload size the capacity precheck
    // saw before, so chunk seal cadence is unchanged. Both guards above are
    // untouched, which is why flushConstShadow's DXMT9_SPLIT_SPARSE_CONST_
    // RECORDS diagnostic path and its telemetry need no changes: this is
    // the single emitter behind all six VS/PS constant kinds.
    return appendRecord(
        recordType,
        kLegacySetConstSizeHint + payloadBytes,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = phase.begin();
            const bool ok = dxmt9::d3d9::pe::appendSetConstants(
                builder, recordType, start, count,
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(data), payloadBytes));
            const auto settlement =
                dxmt9::d3d9::pe::settleRecorderAppend({
                    .phase = dxmt9::d3d9::pe::AppendSettlement::Prepared,
                    .appendSucceeded = ok,
                });
            if (settlement.consumeRepresentedPending &&
                settlement.recordDurable) {
                settlementOwner.clear();
            }
            phase.recordEncode(t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
}

HRESULT D3D9DeviceImpl::flushConstShadow(ConstShadow& shadow, uint32_t recordType, std::size_t elemSize) {
    // Decimated timing (const_flush scope): sample only every Nth call
    // so the CPU cost of measuring is itself negligible. Independent of
    // DXMT9_PE_RECORDER_STATS. Guard covers every exit path (including
    // the early "not dirty" return below) via RAII.
    DxmtPeDecimatedScopeGuard decimatedScope;
    const std::uint32_t decimationN = dxmt9PeStatsDecimationN();
    if (decimationN != 0 &&
        PeDecimatedScopeTimer::shouldSample(diagnostics_->peConstFlushDecimatedStats_, decimationN)) {
        decimatedScope.stats = &diagnostics_->peConstFlushDecimatedStats_;
        {
            const auto n0 = std::chrono::steady_clock::now();
            const auto n1 = std::chrono::steady_clock::now();
            PeDecimatedScopeTimer::recordSample(
                peDecimatedNullScopeStats(),
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(n1 - n0).count()));
        }
        decimatedScope.t0 = std::chrono::steady_clock::now();
    }
    if (!shadow.dirty()) return S_OK;
    const std::int64_t flushEntryNs = dxmt9PeRecorderStatsEnabled()
        ? dxmt9SteadyClockNs(std::chrono::steady_clock::now())
        : 0;
    HRESULT hr = S_OK;
    std::uint32_t flushedRecords = 0u;
    std::uint32_t flushedRegs = 0u;
    // Emits one D9C_COMMAND_RECORD_SET_*_CONST_* record for [start, end)
    // and updates the flush counters above.
    const auto emitRun = [&](uint32_t start, uint32_t end) {
        if (end <= start) return;
        const uint32_t count = end - start;
        const auto* data =
            shadow.values.data() + static_cast<std::size_t>(start) * elemSize;
        const std::uint32_t dirtyRegs =
            recordType == D9C_COMMAND_RECORD_SET_VS_CONST_F
                ? countDirtyConstRegs(shadow, start, end)
                : 0u;
        hr = appendSetConstRecord(
            recordType, start, count, data, elemSize, shadow);
        if (SUCCEEDED(hr)) {
            ++flushedRecords;
            flushedRegs += count;
        }
        if (SUCCEEDED(hr) && recordType == D9C_COMMAND_RECORD_SET_VS_CONST_F) {
            recordVsConstSetterRange(VsConstSetterRangePhase::Flush,
                                     currentVertexShaderHash(),
                                     currentPixelShaderHash(),
                                     start, count, dirtyRegs, count);
        }
    };
    emitRun(shadow.dirtyStart, shadow.dirtyEnd);
    recordPeConstFlushCpu(recordType, flushEntryNs, flushedRecords,
                          flushedRegs);
    return hr;
}

HRESULT D3D9DeviceImpl::flushPendingConsts() {
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

HRESULT D3D9DeviceImpl::chunkBarrierFlush() {
    Dxmt9PeAppendFamilyScope appendFamily(diagnostics_.get(), PeInterAppendCallFamily::Barrier);
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderMutex_, recorderLockRequired_);
    const std::int64_t constEntryNs = dxmt9PeRecorderStatsEnabled()
        ? dxmt9SteadyClockNs(std::chrono::steady_clock::now())
        : 0;
    const HRESULT constHr = flushPendingConsts();
    recordPeChunkBarrierConstCpu(constEntryNs);
    if (FAILED(constHr)) return constHr;
    if (!hasPendingHotState()) {
        return S_OK;
    }
    // Fast path: single APPLY_STATE record covers all pending
    // state. After Phase 31 cap-checks at every Set* fast path,
    // this is the only path that runs in practice.
    const std::int64_t buildEntryNs = dxmt9PeRecorderStatsEnabled()
        ? dxmt9SteadyClockNs(std::chrono::steady_clock::now())
        : 0;
    dxmt9::d3d9::pe::PeDrawParams applyParams{};
    applyParams.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
    if (buildSparseStateForRecord(applyParams)) {
        recordPeApplyStateBuildCpu(buildEntryNs);
        // sizeHint stays kLegacyApplyStateSizeHint: it is what the
        // capacity precheck saw before, so seal cadence is unchanged.
        const HRESULT appendHr = appendRecord(
            D9C_COMMAND_RECORD_APPLY_STATE,
            kLegacyApplyStateSizeHint,
            [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
                const AppendPhaseTimer& phase) -> HRESULT {
                const auto t0 = phase.begin();
                const bool ok = dxmt9::d3d9::pe::appendApplyState(
                    builder, peSparseHeader_.flags, peSparseState_);
                const auto settlement =
                    dxmt9::d3d9::pe::settleRecorderAppend({
                        .phase =
                            dxmt9::d3d9::pe::AppendSettlement::Prepared,
                        .appendSucceeded = ok,
                    });
                const bool settled =
                    dxmt9::d3d9::pe::acceptPreparedSparseState(
                        peState_, peConsts_, peSparseState_, settlement);
                DXMT_ASSERT(!ok || settled);
                (void)settled;
                phase.recordEncode(t0);
                return ok ? S_OK : D3DERR_INVALIDCALL;
            });
        if (FAILED(appendHr)) return appendHr;
        return S_OK;
    }
    recordPeApplyStateBuildCpu(buildEntryNs);
    // Over-cap slow path: a Set* somewhere bypassed the cap check
    // (regression). Drain pending oversized collections in batches
    // of cap-size records. Critical safety property: every pending
    // state bit MUST be represented in the chunk before the caller
    // appends a barrier record. Sealing-and-deferring (the prior
    // behavior) lets the barrier observe stale server state.
    return drainOversizedPendingStateAsApplyStateRecords();
}

template <typename Fill, typename Accept>
HRESULT D3D9DeviceImpl::appendSingleCategoryApplyState(Fill fill, Accept accept) {
    peSparseState_ = dxmt9::d3d9::pe::SparseStateInput{};
    fill();
    return appendRecord(
        D9C_COMMAND_RECORD_APPLY_STATE,
        kLegacyApplyStateSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = phase.begin();
            const bool ok = dxmt9::d3d9::pe::appendApplyState(
                builder, /*flags=*/0u, peSparseState_);
            const auto settlement =
                dxmt9::d3d9::pe::settleRecorderAppend({
                    .phase =
                        dxmt9::d3d9::pe::AppendSettlement::Prepared,
                    .appendSucceeded = ok,
                });
            accept(settlement);
            phase.recordEncode(t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
}

HRESULT D3D9DeviceImpl::drainOversizedPendingStateAsApplyStateRecords() {
    // Drain the four cappable collections (renderStates, tss,
    // samplerStates, transforms) in batches of cap-size. Each batch becomes
    // one APPLY_STATE record carrying ONLY that collection's batch; the
    // server applies unset categories idempotently, so an otherwise-empty
    // sparse record is safe.
    //
    // Section order is ascending by construction: every typed prepare walk
    // visits its bitmap from the lowest set bit and its rows in order.
    // appendPlainSection does not enforce ordering for these four categories,
    // but emitting them out of order would still change the wire shape.
    auto renderStates = peState_.pendingRenderStatesTyped();
    while (!renderStates.empty()) {
        const std::size_t n = peState_.prepareRenderStateBatch(
            peSparseScratch_.renderStates);
        const HRESULT hr = appendSingleCategoryApplyState(
            [&] {
                peSparseState_.renderStates =
                    std::span(peSparseScratch_.renderStates).first(n);
            },
            [&](const dxmt9::d3d9::pe::AppendPlan& settlement) {
                peState_.acceptRenderStateBatch(
                    std::span(peSparseScratch_.renderStates).first(n),
                    settlement);
            });
        if (FAILED(hr)) return hr;
    }
    auto textureStageStates = peState_.pendingTssTyped();
    while (!textureStageStates.empty()) {
        const std::size_t n = peState_.prepareTextureStageStateBatch(
            peSparseScratch_.textureStageStates);
        const HRESULT hr = appendSingleCategoryApplyState(
            [&] {
                peSparseState_.textureStageStates =
                    std::span(peSparseScratch_.textureStageStates).first(n);
            },
            [&](const dxmt9::d3d9::pe::AppendPlan& settlement) {
                peState_.acceptTextureStageStateBatch(
                    std::span(peSparseScratch_.textureStageStates).first(n),
                    settlement);
            });
        if (FAILED(hr)) return hr;
    }
    auto samplerStates = peState_.pendingSamplerStatesTyped();
    while (!samplerStates.empty()) {
        const std::size_t n = peState_.prepareSamplerStateBatch(
            peSparseScratch_.samplerStates);
        const HRESULT hr = appendSingleCategoryApplyState(
            [&] {
                peSparseState_.samplerStates =
                    std::span(peSparseScratch_.samplerStates).first(n);
            },
            [&](const dxmt9::d3d9::pe::AppendPlan& settlement) {
                peState_.acceptSamplerStateBatch(
                    std::span(peSparseScratch_.samplerStates).first(n),
                    settlement);
            });
        if (FAILED(hr)) return hr;
    }
    auto transforms = peState_.pendingTransformsTyped();
    while (!transforms.empty()) {
        const std::size_t n = peState_.prepareTransformBatch(
            peSparseScratch_.transforms);
        const HRESULT hr = appendSingleCategoryApplyState(
            [&] {
                peSparseState_.transforms =
                    std::span(peSparseScratch_.transforms).first(n);
            },
            [&](const dxmt9::d3d9::pe::AppendPlan& settlement) {
                peState_.acceptTransformBatch(
                    std::span(peSparseScratch_.transforms).first(n),
                    settlement);
            });
        if (FAILED(hr)) return hr;
    }
    // Remaining scalar pending bits (texture / stream / vs / ps / vdecl / RT
    // / DS / viewport / scissor / fvf / material / clip / lights /
    // lightEnable) all fit in one record. After draining the four cappable
    // collections above, the sparse build succeeds.
    if (!hasPendingHotState()) {
        return S_OK;
    }
    dxmt9::d3d9::pe::PeDrawParams tailParams{};
    tailParams.recordType = D9C_COMMAND_RECORD_APPLY_STATE;
    if (!buildSparseStateForRecord(tailParams)) {
        // Truly should never happen -- the four cappable collections are now
        // empty. Defensive: log + return failure rather than silently leaving
        // pending state dirty (which would let the upcoming barrier observe
        // stale server state).
        dxmt9DeviceDebugLog(
            "ERR: drainOversizedPendingStateAsApplyStateRecords could "
            "not build tail APPLY_STATE - pending state lost. Caller "
            "should treat as recorder failure.");
        return D3DERR_INVALIDCALL;
    }
    const HRESULT hr = appendRecord(
        D9C_COMMAND_RECORD_APPLY_STATE,
        kLegacyApplyStateSizeHint,
        [&](dxmt9::d3d9::pe::CommandChunkBuilder& builder,
            const AppendPhaseTimer& phase) -> HRESULT {
            const auto t0 = phase.begin();
            const bool ok = dxmt9::d3d9::pe::appendApplyState(
                builder, peSparseHeader_.flags, peSparseState_);
            const auto settlement =
                dxmt9::d3d9::pe::settleRecorderAppend({
                    .phase =
                        dxmt9::d3d9::pe::AppendSettlement::Prepared,
                    .appendSucceeded = ok,
                });
            const bool settled =
                dxmt9::d3d9::pe::acceptPreparedSparseState(
                    peState_, peConsts_, peSparseState_, settlement);
            DXMT_ASSERT(!ok || settled);
            (void)settled;
            phase.recordEncode(t0);
            return ok ? S_OK : D3DERR_INVALIDCALL;
        });
    if (FAILED(hr)) return hr;
    return S_OK;
}
