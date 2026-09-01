/* src/d3d9/d3d9_pe_device_recorder.cpp — cold recorder settlement/flush.
 *
 * Commit, cleanup, and constant-drain paths are intentionally out of the
 * device class header. The hot owner calls these semantic cold boundaries
 * normally; the generic append envelope remains header-defined for its
 * recorder/cold-COM instantiations. */

#include "d3d9_pe_device_impl.hpp"

#include <new>

PeProductionSemanticBatchOwner* D3D9DeviceImpl::semanticBatchOwner() noexcept {
    return semanticRecorderState_ ? &semanticRecorderState_->owner : nullptr;
}

const PeProductionSemanticBatchOwner*
D3D9DeviceImpl::semanticBatchOwner() const noexcept {
    return semanticRecorderState_ ? &semanticRecorderState_->owner : nullptr;
}

void D3D9DeviceImpl::armSemanticRecord(
    dxmt9::d3d9::pe::PeSemanticProducerKind producer,
    std::uint32_t recordType,
    const dxmt9::d3d9::pe::PeSemanticRecordInput& input) noexcept {
    if (!semanticRecorderState_) return;
    semanticRecorderState_->input = input;
    semanticRecorderState_->input.producer = producer;
    semanticRecorderState_->input.recordType = recordType;
    semanticRecorderState_->input.sourceOrdinal = 0u;
    semanticRecorderState_->input.recordOrdinal = 0u;
    semanticRecorderState_->inputValid = true;
}

void D3D9DeviceImpl::clearSemanticRecordInput() noexcept {
    if (semanticRecorderState_) semanticRecorderState_->inputValid = false;
}

bool D3D9DeviceImpl::settleSemanticRecord(
    const dxmt9::d3d9::pe::PeSemanticRecordInput& input,
    std::uint64_t recordOrdinal) noexcept {
    const auto standaloneConstantReady =
        [&](const ConstShadow& shadow, std::size_t elementBytes) noexcept {
            const auto expected =
                static_cast<std::size_t>(input.setConst.registerCount) *
                elementBytes;
            return expected == input.constantBytes.size() && shadow.dirty() &&
                   shadow.dirtyStart == input.setConst.startRegister &&
                   shadow.dirtyEnd - shadow.dirtyStart ==
                       input.setConst.registerCount;
        };
    switch (input.producer) {
    case dxmt9::d3d9::pe::PeSemanticProducerKind::VsFloatConstant:
        if (!standaloneConstantReady(recorderState_.peConsts.vsConstF,
                                     sizeof(float) * 4u)) return false;
        break;
    case dxmt9::d3d9::pe::PeSemanticProducerKind::VsIntConstant:
        if (!standaloneConstantReady(recorderState_.peConsts.vsConstI,
                                     sizeof(std::int32_t) * 4u)) return false;
        break;
    case dxmt9::d3d9::pe::PeSemanticProducerKind::VsBoolConstant:
        if (!standaloneConstantReady(recorderState_.peConsts.vsConstB,
                                     sizeof(std::int32_t))) return false;
        break;
    case dxmt9::d3d9::pe::PeSemanticProducerKind::PsFloatConstant:
        if (!standaloneConstantReady(recorderState_.peConsts.psConstF,
                                     sizeof(float) * 4u)) return false;
        break;
    case dxmt9::d3d9::pe::PeSemanticProducerKind::PsIntConstant:
        if (!standaloneConstantReady(recorderState_.peConsts.psConstI,
                                     sizeof(std::int32_t) * 4u)) return false;
        break;
    case dxmt9::d3d9::pe::PeSemanticProducerKind::PsBoolConstant:
        if (!standaloneConstantReady(recorderState_.peConsts.psConstB,
                                     sizeof(std::int32_t))) return false;
        break;
    default:
        break;
    }
    const auto appendPlan = dxmt9::d3d9::pe::settleRecorderAppend({
        .phase = dxmt9::d3d9::pe::AppendSettlement::Prepared,
        .appendSucceeded = true,
    });
    if (!appendPlan.valid() || !appendPlan.consumeRepresentedPending() ||
        !appendPlan.recordDurable() ||
        !dxmt9::d3d9::pe::acceptPreparedSparseState(
            recorderState_.peState, recorderState_.peConsts, input.sparse,
            appendPlan, scalarSemanticObserver(), recordOrdinal,
            recorderState_.chunkTransaction.pendingTicket())) {
        return false;
    }
    // Standalone Set*Constant records are not represented by sparse ranges;
    // acceptPreparedSparseState therefore must not consume an arbitrary dirty
    // range on their behalf.  Consume only the exact range admitted by this
    // record, preserving unrelated dirty constants for their own producer.
    switch (input.producer) {
    case dxmt9::d3d9::pe::PeSemanticProducerKind::VsFloatConstant:
        recorderState_.peConsts.vsConstF.clear();
        break;
    case dxmt9::d3d9::pe::PeSemanticProducerKind::VsIntConstant:
        recorderState_.peConsts.vsConstI.clear();
        break;
    case dxmt9::d3d9::pe::PeSemanticProducerKind::VsBoolConstant:
        recorderState_.peConsts.vsConstB.clear();
        break;
    case dxmt9::d3d9::pe::PeSemanticProducerKind::PsFloatConstant:
        recorderState_.peConsts.psConstF.clear();
        break;
    case dxmt9::d3d9::pe::PeSemanticProducerKind::PsIntConstant:
        recorderState_.peConsts.psConstI.clear();
        break;
    case dxmt9::d3d9::pe::PeSemanticProducerKind::PsBoolConstant:
        recorderState_.peConsts.psConstB.clear();
        break;
    default:
        break;
    }
    return true;
}

bool D3D9DeviceImpl::observeCommittedSemanticOwnerRecord() noexcept {
    auto* const observer = allFamilySemanticObserver();
    if (!observer) return true;
    const auto* const owner = semanticBatchOwner();
    return owner && dxmt9::d3d9::pe::projectLastCommittedSemanticRecord(
        *owner, *observer);
}

HRESULT D3D9DeviceImpl::appendSemanticRecord(
    std::uint32_t type, std::size_t bytes) {
    auto* const semanticOwner = semanticBatchOwner();
    if (!semanticOwner || !semanticRecorderState_) return D3DERR_NOTAVAILABLE;

    const auto maxRecords = maxPendingCommandRecords();
    const auto maxBytes = maxPendingCommandBytes();
    const auto recordCountBefore = semanticOwner->size();
    const auto payloadBytesBefore = semanticRecorderState_->cadenceBytes;
    bool ownerCapacityBoundary = false;
    if (semanticRecorderState_->inputValid) {
        auto capacityInput = semanticRecorderState_->input;
        capacityInput.recordType = type;
        bool capacityInputValid = true;
        if (capacityInput.producer ==
                dxmt9::d3d9::pe::PeSemanticProducerKind::DrawPrimitive ||
            capacityInput.producer ==
                dxmt9::d3d9::pe::PeSemanticProducerKind::DrawIndexedPrimitive) {
            dxmt9::d3d9::pe::PeDrawParams params{};
            params.recordType = type;
            params.primitiveType = capacityInput.draw.primitiveType;
            params.baseVertex = capacityInput.draw.baseVertex;
            params.minVertex = capacityInput.draw.minVertex;
            params.numVertices = capacityInput.draw.numVertices;
            params.startVertex = capacityInput.draw.startVertex;
            params.startIndex = capacityInput.draw.startIndex;
            params.primitiveCount = capacityInput.draw.primitiveCount;
            capacityInputValid = addChunkContextSections(
                currentChunkContext(), recorderState_.peState,
                recorderState_.peBindingView, params,
                recorderState_.peSparseScratch, capacityInput.sparse);
        }
        ownerCapacityBoundary = capacityInputValid &&
            !semanticOwner->canAdmitStorage(capacityInput);
    }
    const bool willFlushBeforeAppend = recordCountBefore != 0u &&
        (recordCountBefore >= maxRecords ||
         payloadBytesBefore + bytes > maxBytes || ownerCapacityBoundary);
    const std::uint32_t appendedRecordCount =
        willFlushBeforeAppend ? 1u : recordCountBefore + 1u;
    const auto appendedPayloadBytes =
        willFlushBeforeAppend ? bytes : payloadBytesBefore + bytes;
    const bool willFlushAfterAppend =
        appendedRecordCount >= maxRecords || appendedPayloadBytes >= maxBytes;

    if (willFlushBeforeAppend) {
        const HRESULT preHr =
            flushPendingCommandChunk(PeRecorderFlushReason::CapacityPre);
        if (FAILED(preHr)) {
            clearSemanticRecordInput();
            return preHr;
        }
    }
    std::size_t priorHandles = 0u;
    std::size_t priorPayload = 0u;
    std::size_t priorWire = 0u;
    if (!semanticOwner->emissionMetrics(priorHandles, priorPayload,
                                        priorWire)) {
        priorHandles = priorPayload = priorWire = 0u;
    }
    if (!recorderState_.prepareChunkRecord(
            type, bytes, willFlushBeforeAppend, semanticOwner->size(),
            priorHandles, priorPayload, semanticOwner->retainedCount())) {
        poisonStateBlockTransaction();
        clearSemanticRecordInput();
        return D3DERR_DEVICELOST;
    }
    if (!semanticRecorderState_->inputValid) {
        (void)recorderState_.settleSemanticEmitter(
            false, semanticOwner->size(), priorHandles, priorPayload,
            semanticOwner->retainedCount());
        poisonStateBlockTransaction();
        return D3DERR_INVALIDCALL;
    }
    auto input = semanticRecorderState_->input;
    if (input.sourceOrdinal == 0u) {
        input.sourceOrdinal = semanticOwner->nextSourceOrdinal();
    }
    if (input.recordOrdinal == 0u) {
        input.recordOrdinal = semanticOwner->nextRecordOrdinal();
    }
    if (input.sourceOrdinal == 0u || input.recordOrdinal == 0u) {
        (void)recorderState_.settleSemanticEmitter(
            false, semanticOwner->size(), priorHandles, priorPayload,
            semanticOwner->retainedCount());
        clearSemanticRecordInput();
        return D3DERR_INVALIDCALL;
    }
    input.recordType = type;
    if (input.producer == dxmt9::d3d9::pe::PeSemanticProducerKind::DrawPrimitive ||
        input.producer ==
            dxmt9::d3d9::pe::PeSemanticProducerKind::DrawIndexedPrimitive) {
        dxmt9::d3d9::pe::PeDrawParams params{};
        params.recordType = type;
        params.primitiveType = input.draw.primitiveType;
        params.baseVertex = input.draw.baseVertex;
        params.minVertex = input.draw.minVertex;
        params.numVertices = input.draw.numVertices;
        params.startVertex = input.draw.startVertex;
        params.startIndex = input.draw.startIndex;
        params.primitiveCount = input.draw.primitiveCount;
        if (!addChunkContextSections(
                currentChunkContext(), recorderState_.peState,
                recorderState_.peBindingView, params,
                recorderState_.peSparseScratch, input.sparse)) {
            if (dxmt9PeRecorderChunkLogEnabled()) {
                dxmt9DeviceInfoLog(
                    "pe_semantic_failure_stage type=%u stage=chunk_context "
                    "records=%zu",
                    type, semanticOwner->size());
            }
            (void)recorderState_.settleSemanticEmitter(
                false, semanticOwner->size(), priorHandles, priorPayload,
                semanticOwner->retainedCount());
            poisonStateBlockTransaction();
            return D3DERR_INVALIDCALL;
        }
    }
    if (!semanticOwner->canAdmitStorage(input)) {
        if (dxmt9PeRecorderChunkLogEnabled()) {
            dxmt9DeviceInfoLog(
                "pe_semantic_failure_stage type=%u stage=storage_preflight "
                "records=%zu retained=%zu",
                type, semanticOwner->size(), semanticOwner->retainedCount());
        }
        (void)recorderState_.settleSemanticEmitter(
            false, semanticOwner->size(), priorHandles, priorPayload,
            semanticOwner->retainedCount());
        clearSemanticRecordInput();
        return D3DERR_INVALIDCALL;
    }
    bool semanticSettlement = true;
    const bool admitted = semanticOwner->appendOwnedRecord(
        input, [&]() noexcept {
            semanticSettlement =
                settleSemanticRecord(input, input.recordOrdinal);
            return semanticSettlement;
        });
    if (!admitted && dxmt9PeRecorderChunkLogEnabled()) {
        dxmt9DeviceInfoLog(
            "pe_semantic_failure_stage type=%u stage=owner_admission "
            "records=%zu retained=%zu settlement=%u admission=%u",
            type, semanticOwner->size(), semanticOwner->retainedCount(),
            semanticSettlement ? 1u : 0u,
            static_cast<unsigned>(semanticOwner->lastAdmissionFailure()));
    }
    clearSemanticRecordInput();
    std::size_t handles = 0u;
    std::size_t payload = 0u;
    std::size_t wire = 0u;
    if (!semanticOwner->emissionMetrics(handles, payload, wire)) {
        // A failed callback admission has already rolled back the owner and its
        // typed pins. Only a successful admission with an invalid projection
        // is an invariant failure requiring fail-stop.
        (void)recorderState_.settleSemanticEmitter(
            false, semanticOwner->size(), priorHandles, priorPayload,
            semanticOwner->retainedCount());
        if (admitted) {
            (void)recorderState_.chunkTransaction.poison();
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        return D3DERR_INVALIDCALL;
    }
    if (!admitted || !recorderState_.settleSemanticEmitter(
                          admitted, semanticOwner->size(), handles, payload,
                          semanticOwner->retainedCount())) {
        (void)recorderState_.settleSemanticEmitter(
            false, semanticOwner->size(), priorHandles, priorPayload,
            semanticOwner->retainedCount());
        if (admitted) {
            (void)recorderState_.chunkTransaction.poison();
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        return D3DERR_INVALIDCALL;
    }
    if (!observeCommittedSemanticOwnerRecord()) {
        (void)recorderState_.chunkTransaction.poison();
        poisonStateBlockTransaction();
        return D3DERR_DEVICELOST;
    }
    // The semantic owner has already applied destination-chunk selection to
    // the copied sparse input. Advance the indexed-draw witness only after the
    // owner transaction is durable.
    if (input.producer ==
            dxmt9::d3d9::pe::PeSemanticProducerKind::DrawIndexedPrimitive &&
        input.sparse.indexBuffers.size() == 1u) {
        const auto& index = input.sparse.indexBuffers.front();
        submittedIndexBufferWireValue_ = d9cWireHandleValue(
            toWireHandle(index.object.object));
        submittedIndexBufferKnown_ = index.object.object != nullptr;
    }
    semanticRecorderState_->cadenceBytes = appendedPayloadBytes;
    return willFlushAfterAppend
        ? flushPendingCommandChunk(PeRecorderFlushReason::CapacityPost)
        : S_OK;
}

HRESULT D3D9DeviceImpl::retryPendingSemanticChunk() {
    auto* const owner = semanticBatchOwner();
    std::size_t handles = 0u;
    std::size_t payload = 0u;
    std::size_t wire = 0u;
    const bool retryBytes = owner != nullptr && owner->size() != 0u &&
        owner->emissionMetrics(handles, payload, wire);
    if (!peRecorderRetryBytesReady(false, retryBytes, true)) {
        (void)recorderState_.chunkTransaction.poison();
        poisonStateBlockTransaction();
        return D3DERR_DEVICELOST;
    }
    return flushPendingCommandChunk(PeRecorderFlushReason::CapacityPre);
}

HRESULT D3D9DeviceImpl::appendSemanticRecordBoundary(
    std::uint32_t type, std::size_t bytes, std::uint64_t sourceOrdinal) {
    auto* const owner = semanticBatchOwner();
    if (!owner) return D3DERR_NOTAVAILABLE;
    std::size_t handles = 0u;
    std::size_t payload = 0u;
    std::size_t wire = 0u;
    if (!dxmt9::d3d9::pe::resolvePeSemanticCadenceMetrics(
            *owner, handles, payload, wire)) {
        return D3DERR_DEVICELOST;
    }
    const std::size_t maxBytes = maxPendingCommandBytes();
    const bool payloadWouldExceed =
        payload > maxBytes || bytes > maxBytes - std::min(payload, maxBytes);
    const bool willFlushBeforeAppend = owner->size() != 0u &&
        (owner->size() >= maxPendingCommandRecords() || payloadWouldExceed);
    std::int32_t injectedFaultHr = 0;
    // This is the semantic lane's pre-effect capacity seam. No owner state
    // has changed, so the caller may retry the same source ordinal verbatim.
    if (dxmt9PeRecorderFaultsEnabled() && willFlushBeforeAppend &&
        dxmt9PeConsumeRecorderFault(PeRecorderFaultPoint::CapacityPreReserve,
                                    injectedFaultHr)) {
        return static_cast<HRESULT>(injectedFaultHr);
    }
    if (sourceOrdinal != 0u) {
        semanticRecorderState_->input.sourceOrdinal = sourceOrdinal;
    }
    return appendSemanticRecord(type, bytes);
}

bool D3D9DeviceImpl::clearPendingCommandChunk(
    dxmt9::d3d9::pe::RecorderCommitEvent discardEvent) {
    auto* semanticOwner = semanticBatchOwner();
    const auto discardPlan = dxmt9::d3d9::pe::settleRecorderCommit({
        .phase = semanticOwner &&
                 recorderState_.chunkTransaction.phase() ==
                     dxmt9::d3d9::pe::RecorderChunkTransactionPhase::Sealed
            ? dxmt9::d3d9::pe::RecorderCommitPhase::Sealed
            : dxmt9::d3d9::pe::RecorderCommitPhase::Unsealed,
        .event = discardEvent,
    });
    const bool discardAdmitted =
        discardPlan.valid() && discardPlan.resetBuilder() &&
        discardPlan.action() ==
            dxmt9::d3d9::pe::RecorderCommitAction::DiscardAll;
    DXMT_ASSERT(discardAdmitted);
    if (!discardAdmitted) {
        if (peCaptureState_) markRenderTapeInvalidOnce("commit_discard_settlement");
        return false;
    }
    // Discarded chunks never acquire a tape ObjectDestroy event. Drain
    // the logical pending refs before raw D9C retainer reset.
    drainPendingRenderTapeChunk(false);
    // Discard path (device teardown, Reset, ResetEx): release the warm
    // retainer pins too, so nothing is still holding a unix object when
    // dxmt9c_device_reset* / dxmt9c_device_release runs.
    if (!semanticOwner) return false;
    semanticOwner->reset();
    if (semanticRecorderState_) {
        semanticRecorderState_->cadenceBytes = 0u;
        semanticRecorderState_->inputValid = false;
    }
    if (auto* tokens = scalarSemanticObserver()) tokens->clear();
    if (auto* tokens = allFamilySemanticObserver()) tokens->discard();
    recorderState_.chunkTransaction.discard();
    return true;
}

HRESULT D3D9DeviceImpl::commitPendingCommandChunk(
    PeRecorderFlushReason commitReason, const D9CCommandChunk& chunk,
    const PeCommandChunkCommitInfo& info,
    dxmt9::d3d9::pe::RecorderCommitPhase* settledPhase,
    const dxmt9::d3d9::pe::SegmentedCommandChunk* segmented) {
    auto& commitTransaction = recorderState_.chunkTransaction;
    // This is the same persistent owner started by appendRecord; commit does
    // not create a second local observer. The builder, retainer, and capture
    // objects remain storage owners.
    if (commitTransaction.phase() !=
        dxmt9::d3d9::pe::RecorderChunkTransactionPhase::Sealed) {
        (void)commitTransaction.poison();
        poisonStateBlockTransaction();
        return D3DERR_DEVICELOST;
    }
    std::size_t semanticHandleCount = 0u;
    std::size_t semanticPayloadBytes = 0u;
    std::size_t semanticWireBytes = 0u;
    auto* const semanticOwner = semanticBatchOwner();
    const bool semanticEvidence = semanticOwner &&
        semanticOwner->emissionMetrics(
            semanticHandleCount, semanticPayloadBytes, semanticWireBytes);
    if (!semanticEvidence || !recorderState_.sealedEvidenceMatchesChunk(
            semanticOwner->size(), semanticHandleCount,
            semanticPayloadBytes, semanticOwner->retainedCount())) {
        (void)commitTransaction.poison();
        poisonStateBlockTransaction();
        return D3DERR_DEVICELOST;
    }
    PeCaptureState *const captureState =
        peCaptureState_ ? &*peCaptureState_ : nullptr;
    std::int32_t injectedFaultHr = 0;
    // This seam is before any capture reservation and before entering the
    // unchanged C ABI call. Keep the sealed projection intact for retry.
    if (dxmt9PeRecorderFaultsEnabled() &&
        dxmt9PeConsumeRecorderFault(PeRecorderFaultPoint::BridgePre,
                                    injectedFaultHr)) {
        if (!commitTransaction.recordBridgePreEffectFailure()) {
            (void)commitTransaction.poison();
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        return static_cast<HRESULT>(injectedFaultHr);
    }
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
            bool captureChunkPrepared = false;
            bool captureDispositionFault = false;
            if (captureWasActive) {
                bool injectedCaptureThrow = false;
                try {
                    if (dxmt9PeConsumeRecorderFault(
                            PeRecorderFaultPoint::CaptureThrow,
                            injectedFaultHr)) {
                        injectedCaptureThrow = true;
                        throw std::bad_alloc();
                    }
                    captureChunkPrepared = prepareRenderTapeChunkCapture(
                        chunk, info);
                } catch (...) {
                    abortRenderTapeCapture(injectedCaptureThrow
                        ? "fault_capture_throw"
                        : "capture_prepare_exception");
                    captureChunkPrepared = false;
                }
            }
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
            if (!commitTransaction.recordCaptureReservation(
                    captureState
                        ? captureState->renderTapeActiveCaptureToken
                        : 0u,
                    submittedChunk.renderTapeEventOrdinal,
                    presentMirrorReserved || captureChunkPrepared)) {
                (void)commitTransaction.poison();
                poisonStateBlockTransaction();
                return D3DERR_DEVICELOST;
            }
            HRESULT hr = hr32(
                segmented
                    ? dxmt9c_device_commit_chunk_segmented(
                          dev_, &segmented->transport)
                    : dxmt9c_device_commit_chunk(dev_, &submittedChunk));
            // Once the C ABI call returned, the unchanged ABI cannot tell us
            // whether the provider published before reporting failure. Turn
            // this injected entered fault into the same fail-stop path as a
            // naturally ambiguous bridge failure.
            if (SUCCEEDED(hr) && dxmt9PeRecorderFaultsEnabled() &&
                dxmt9PeConsumeRecorderFault(PeRecorderFaultPoint::BridgeEntered,
                                            injectedFaultHr)) {
                hr = static_cast<HRESULT>(injectedFaultHr);
            }
            const auto composedBridgePlan =
                dxmt9::d3d9::pe::planRecorderSettlement({
                    .point = dxmt9::d3d9::pe::RecorderSettlementPoint::Bridge,
                    .result = SUCCEEDED(hr)
                        ? dxmt9::d3d9::pe::RecorderSettlementResult::Succeeded
                        : dxmt9::d3d9::pe::RecorderSettlementResult::FailedEffectUnknown,
                });
            const auto bridgePlan = dxmt9::d3d9::pe::settleRecorderCommit({
                .phase = dxmt9::d3d9::pe::RecorderCommitPhase::Sealed,
                .event = SUCCEEDED(hr)
                    ? dxmt9::d3d9::pe::RecorderCommitEvent::BridgeAccepted
                    : dxmt9::d3d9::pe::RecorderCommitEvent::BridgeEffectUnknown,
            });
            if (!composedBridgePlan.valid() || !bridgePlan.valid()) {
                DXMT_ASSERT(false && "invalid bridge settlement");
                if (!commitTransaction.poison()) {
                    poisonStateBlockTransaction();
                    return D3DERR_DEVICELOST;
                }
                if (captureState) {
                    markRenderTapeInvalidOnce("commit_bridge_settlement");
                }
                poisonStateBlockTransaction();
                return D3DERR_DEVICELOST;
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
                // The unchanged C ABI cannot prove whether an entered commit
                // failed before or after unix-side publication/replay. Keep
                // sealed ownership for Reset/teardown cleanup, but poison the
                // PE recorder so ordinary app traffic cannot retry it.
                DXMT_ASSERT(composedBridgePlan.poison() && bridgePlan.poisons());
                if (auto* tokens = allFamilySemanticObserver()) {
                    tokens->bridgeEffectUnknown();
                }
                poisonStateBlockTransaction();
                if (presentMirrorReserved) {
                    dxmt9c_device_cancel_render_tape_present_capture(dev_);
                }
                if (captureState &&
                    captureState->renderTapeCapture.state() ==
                        dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
                    abortRenderTapeCapture("bridge_commit");
                }
                const bool bridgeSettled =
                    commitTransaction.recordBridgeResult(false);
                if (!bridgeSettled || !commitTransaction.poisoned()) {
                    poisonStateBlockTransaction();
                    return D3DERR_DEVICELOST;
                }
                return hr;
            }
            if (captureWasActive && dxmt9PeConsumeRecorderFault(
                    PeRecorderFaultPoint::CaptureDisposition,
                    injectedFaultHr)) {
                // This is an accepted bridge, so the command remains
                // irreversible; only the optional capture is rejected. Keep
                // the capture token alive until the recorder transaction has
                // settled the reserved token/ordinal below. Aborting here
                // would clear that identity and turn an optional capture
                // failure into a spurious device-loss result.
                captureDispositionFault = true;
                captureChunkPrepared = false;
            }
            const bool transactionBridgeSettled =
                commitTransaction.recordBridgeResult(true);
            if (!transactionBridgeSettled ||
                commitTransaction.phase() !=
                    dxmt9::d3d9::pe::RecorderChunkTransactionPhase::BridgeAccepted) {
                (void)commitTransaction.poison();
                poisonStateBlockTransaction();
                return D3DERR_DEVICELOST;
            }
            auto settlementPhase = bridgePlan.next();
            const auto capturePlan = dxmt9::d3d9::pe::settleRecorderCommit({
                .phase = settlementPhase,
                .event = captureChunkPrepared
                    ? dxmt9::d3d9::pe::RecorderCommitEvent::CaptureMaterialized
                    : captureWasActive
                        ? dxmt9::d3d9::pe::RecorderCommitEvent::CaptureRejected
                        : dxmt9::d3d9::pe::RecorderCommitEvent::CaptureSkipped,
            });
            if (!capturePlan.valid()) {
                DXMT_ASSERT(false && "invalid capture settlement");
                if (captureState) {
                    markRenderTapeInvalidOnce("commit_capture_settlement");
                }
                if (!commitTransaction.poison()) {
                    poisonStateBlockTransaction();
                    return D3DERR_DEVICELOST;
                }
                poisonStateBlockTransaction();
                return D3DERR_DEVICELOST;
            } else {
                const auto disposition = captureChunkPrepared
                    ? dxmt9::d3d9::pe::RecorderChunkCaptureDisposition::Materialized
                    : captureWasActive
                            ? dxmt9::d3d9::pe::RecorderChunkCaptureDisposition::Rejected
                            : dxmt9::d3d9::pe::RecorderChunkCaptureDisposition::Skipped;
                if (!commitTransaction.captureReservationMatches(
                        captureState
                            ? captureState->renderTapeActiveCaptureToken
                            : 0u,
                        submittedChunk.renderTapeEventOrdinal,
                        presentMirrorReserved || captureChunkPrepared)) {
                    (void)commitTransaction.poison();
                    poisonStateBlockTransaction();
                    return D3DERR_DEVICELOST;
                }
                const bool transactionCaptureSettled =
                    commitTransaction.recordCaptureResult(disposition);
                if (!transactionCaptureSettled) {
                    (void)commitTransaction.poison();
                    poisonStateBlockTransaction();
                    return D3DERR_DEVICELOST;
                }
                settlementPhase = capturePlan.next();
                const bool captureCannotRetract =
                    !dxmt9::d3d9::pe::recorderCaptureMayRetract(
                        bridgePlan.commandAccepted());
                DXMT_ASSERT(captureCannotRetract);
                if (!captureCannotRetract) {
                    (void)commitTransaction.poison();
                    poisonStateBlockTransaction();
                    return D3DERR_DEVICELOST;
                }
                if (auto* tokens = allFamilySemanticObserver()) {
                    const auto disposition = captureChunkPrepared
                        ? dxmt9::d3d9::pe::PeSemanticCaptureDisposition::Materialized
                        : captureWasActive
                            ? dxmt9::d3d9::pe::PeSemanticCaptureDisposition::Rejected
                            : dxmt9::d3d9::pe::PeSemanticCaptureDisposition::Skipped;
                    if (!tokens->settleCapture(disposition)) {
                        (void)commitTransaction.poison();
                        poisonStateBlockTransaction();
                        return D3DERR_DEVICELOST;
                    }
                }
                if (captureDispositionFault) {
                    abortRenderTapeCapture("fault_capture_disposition");
                }
            }
            if (settledPhase) {
                *settledPhase = settlementPhase;
            }
            if (SUCCEEDED(hr)) {
                ++recorderState_.commandChunkCommits;
                recorderState_.commandChunkRecords += info.recordCount;
                recorderState_.commandChunkBytes += info.wireBytes;
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
            if (commitTransaction.phase() ==
                    dxmt9::d3d9::pe::RecorderChunkTransactionPhase::BridgeAccepted) {
                // A malformed capture settlement is handled by the existing
                // fail-closed diagnostics path above; command publication is
                // still complete and cannot be retracted.
                if (!commitTransaction.recordCaptureResult(
                        dxmt9::d3d9::pe::RecorderChunkCaptureDisposition::Rejected)) {
                    (void)commitTransaction.poison();
                    poisonStateBlockTransaction();
                    return D3DERR_DEVICELOST;
                }
            }
            return hr;
}

HRESULT D3D9DeviceImpl::flushPendingCommandChunk(
    PeRecorderFlushReason reason, PeRecorderFlushDisposition disposition) {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    const auto flushAction = planPeRecorderFlush(
        disposition, recorderState_.stateBlockTransaction.isPoisoned());
    if (flushAction == PeRecorderFlushAction::Discard) {
        if (!clearPendingCommandChunk(
            reason == PeRecorderFlushReason::Reset
                ? dxmt9::d3d9::pe::RecorderCommitEvent::DeviceReset
                : dxmt9::d3d9::pe::RecorderCommitEvent::ExplicitDiscard)) {
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        return S_OK;
    }
    if (flushAction == PeRecorderFlushAction::RejectPoisoned) {
        // An entered bridge failure has unknown effect.  The sealed bytes stay
        // owned until Reset/teardown explicitly discards them; ordinary flush
        // traffic must never submit that same command a second time.
        return D3DERR_DEVICELOST;
    }
    // A generic bridge_pre fault preserves the already-sealed transaction.
    // The bounded owner emission is the retry-byte witness. Re-enter the
    // ordinary seal bookkeeping before retrying the same bytes; no new source
    // ordinal, retain, or record is created.
    std::size_t semanticRetryHandles = 0u;
    std::size_t semanticRetryPayload = 0u;
    std::size_t semanticRetryWire = 0u;
    auto* const semanticOwner = semanticBatchOwner();
    if (!semanticOwner) return D3DERR_NOTAVAILABLE;
    const bool semanticRetryBytes = semanticOwner->size() != 0u &&
        semanticOwner->emissionMetrics(
            semanticRetryHandles, semanticRetryPayload, semanticRetryWire);
    if (peRecorderRetryBytesReady(false, semanticRetryBytes,
                                  recorderState_.chunkTransaction.retryable())) {
        if (!recorderState_.chunkTransaction.reopenBridgePreEffectRetry()) {
            (void)recorderState_.chunkTransaction.poison();
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
    }
    if (!recorderState_.commandChunkNegotiated) {
        return D3DERR_NOTAVAILABLE;
    }
    const bool settlingCapacityPost =
        reason == PeRecorderFlushReason::CapacityPost;
    const auto recordCapacityPost = [&](bool succeeded) noexcept {
        if (!settlingCapacityPost) return true;
        const bool recorded =
            recorderState_.chunkTransaction.recordCapacityPostResult(succeeded);
        if (!recorded) {
            // This is a producer/transaction invariant failure, not an
            // assertion-only diagnostic: do not let a flush proceed with
            // capacity evidence detached from its persistent owner.
            (void)recorderState_.chunkTransaction.poison();
            poisonStateBlockTransaction();
        }
        return recorded;
    };
    std::size_t semanticHandleCount = 0u;
    std::size_t semanticPayloadBytes = 0u;
    std::size_t semanticWireBytes = 0u;
    const bool semanticMetricsValid = semanticOwner->emissionMetrics(
        semanticHandleCount, semanticPayloadBytes, semanticWireBytes);
    if (semanticOwner->size() == 0u) return S_OK;
    // A non-empty owner must always have a valid bounded emission plan. There
    // is no legacy seal fallback after promotion.
    if (!semanticMetricsValid) {
        (void)recorderState_.chunkTransaction.poison();
        poisonStateBlockTransaction();
        return D3DERR_DEVICELOST;
    }
    const auto payloadBytes = semanticPayloadBytes;
    // Render Tape consumes the owner-generated canonical contiguous blob on
    // the PE side. Ordinary production may pass the same owner's immutable
    // segmented regions directly; neither form uses a second builder.
    const bool useSegmented =
        recorderState_.commandChunkTransport ==
            D9C_COMMAND_CHUNK_TRANSPORT_SEGMENTED_V1 &&
        peCaptureState_ == nullptr;
    dxmt9::d3d9::pe::SegmentedCommandChunk segmented{};
    dxmt9::d3d9::pe::PeSemanticSegmentedEmission semanticSegmented{};
    dxmt9::d3d9::pe::PeSemanticExactFixedEmission semanticExact{};
    if (useSegmented) {
        if (!semanticOwner->emitSegmented(semanticSegmented)) {
            const auto sealPlan = dxmt9::d3d9::pe::settleRecorderCommit({
                .phase = dxmt9::d3d9::pe::RecorderCommitPhase::Unsealed,
                .event = dxmt9::d3d9::pe::RecorderCommitEvent::SealFailed,
            });
            (void)sealPlan;
            (void)recorderState_.chunkTransaction.recordSealResult(false);
            if (!recordCapacityPost(false)) return D3DERR_DEVICELOST;
            return D3DERR_INVALIDCALL;
        }
        segmented.transport = semanticSegmented.transport;
        segmented.wireBytes = semanticSegmented.wireBytes;
    } else if (!semanticOwner->emitExactFixed(semanticExact)) {
        const auto sealPlan = dxmt9::d3d9::pe::settleRecorderCommit({
            .phase = dxmt9::d3d9::pe::RecorderCommitPhase::Unsealed,
            .event = dxmt9::d3d9::pe::RecorderCommitEvent::SealFailed,
        });
        (void)sealPlan;
        (void)recorderState_.chunkTransaction.recordSealResult(false);
        if (!recordCapacityPost(false)) return D3DERR_DEVICELOST;
        return D3DERR_INVALIDCALL;
    }
    if ((segmented.valid() && segmented.wireBytes > 0xffffffffull) ||
        (!segmented.valid() && semanticExact.wireBytes > 0xffffffffull)) {
        const auto sealPlan = dxmt9::d3d9::pe::settleRecorderCommit({
            .phase = dxmt9::d3d9::pe::RecorderCommitPhase::Unsealed,
            .event = dxmt9::d3d9::pe::RecorderCommitEvent::SealFailed,
        });
        if (!sealPlan.valid() || !sealPlan.preserveRetryBytes()) {
            if (!recorderState_.chunkTransaction.poison()) {
                poisonStateBlockTransaction();
                return D3DERR_DEVICELOST;
            }
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        if (!recorderState_.chunkTransaction.recordSealResult(false)) {
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        if (!recordCapacityPost(false)) return D3DERR_DEVICELOST;
        return D3DERR_INVALIDCALL;
    }
    const auto sealPlan = dxmt9::d3d9::pe::settleRecorderCommit({
        .phase = dxmt9::d3d9::pe::RecorderCommitPhase::Unsealed,
        .event = dxmt9::d3d9::pe::RecorderCommitEvent::SealAccepted,
    });
    if (!sealPlan.valid() || sealPlan.next() !=
            dxmt9::d3d9::pe::RecorderCommitPhase::Sealed) {
        const bool postRecorded = recordCapacityPost(false);
        const bool transactionPoisoned = recorderState_.chunkTransaction.poison();
        if (!postRecorded || !transactionPoisoned) {
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        poisonStateBlockTransaction();
        return D3DERR_DEVICELOST;
    }
    if (!recorderState_.chunkTransaction.recordSealResult(true)) {
        const bool postRecorded = recordCapacityPost(false);
        const bool transactionPoisoned = recorderState_.chunkTransaction.poison();
        if (!postRecorded || !transactionPoisoned) {
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        poisonStateBlockTransaction();
        return D3DERR_DEVICELOST;
    }
    const bool evidenceRecorded = recorderState_.recordChunkSealedEvidence(
        semanticOwner->size(), semanticHandleCount, semanticPayloadBytes,
        semanticOwner->retainedCount());
    if (!evidenceRecorded) {
        (void)recorderState_.chunkTransaction.poison();
        poisonStateBlockTransaction();
        return D3DERR_DEVICELOST;
    }
    D9CCommandChunk chunk{};
    chunk.version = D9C_COMMAND_CHUNK_VERSION;
    if (!segmented.valid()) {
        chunk.recordCount = semanticExact.transport.header.recordCount;
        chunk.recordBytes = semanticExact.wireBytes;
        chunk.records = toWireHandle(semanticExact.wire.data());
        chunk.handleCount = semanticExact.transport.header.handleCount;
        chunk.producerIdentity = semanticExact.transport.producerIdentity;
    } else if (segmented.valid()) {
        chunk.recordCount = segmented.transport.header.recordCount;
        // This metadata chunk is used only for counters and the cold
        // settlement path; the segmented descriptor is the bridge payload.
        chunk.recordBytes = segmented.wireBytes;
        chunk.records = segmented.transport.records;
        chunk.handleCount = segmented.transport.header.handleCount;
        chunk.producerIdentity = segmented.transport.producerIdentity;
    }
    const PeCommandChunkCommitInfo info{
        .recordCount = chunk.recordCount,
        .payloadBytes = static_cast<std::uint32_t>(std::min<std::size_t>(
            payloadBytes, std::numeric_limits<std::uint32_t>::max())),
        .handleCount = chunk.handleCount,
        .wireBytes = chunk.recordBytes,
    };
    dxmt9::d3d9::pe::RecorderCommitPhase settlementPhase =
        dxmt9::d3d9::pe::RecorderCommitPhase::Sealed;
    const HRESULT hr =
        commitPendingCommandChunk(reason, chunk, info, &settlementPhase,
                                  segmented.valid() ? &segmented : nullptr);
    if (FAILED(hr) && !recordCapacityPost(false)) {
        return D3DERR_DEVICELOST;
    }
    if (SUCCEEDED(hr)) {
        const auto beginDrain = dxmt9::d3d9::pe::settleRecorderCommit({
            .phase = settlementPhase,
            .event = dxmt9::d3d9::pe::RecorderCommitEvent::DrainPending,
        });
        const bool beginDrainAdmitted =
            beginDrain.valid() &&
            beginDrain.action() ==
                dxmt9::d3d9::pe::RecorderCommitAction::BeginDrain;
        if (!beginDrainAdmitted && peCaptureState_) {
            markRenderTapeInvalidOnce("commit_begin_drain_settlement");
        }
        if (!beginDrainAdmitted) {
            (void)recorderState_.chunkTransaction.poison();
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        const auto drainPhase = beginDrainAdmitted
            ? beginDrain.next()
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
            .phase = finishDrain.valid()
                ? finishDrain.next()
                : dxmt9::d3d9::pe::RecorderCommitPhase::Drained,
            .event = dxmt9::d3d9::pe::RecorderCommitEvent::BuilderReset,
        });
        const auto warmPlan = dxmt9::d3d9::pe::settleRecorderCommit({
            .phase = resetPlan.valid()
                ? resetPlan.next()
                : dxmt9::d3d9::pe::RecorderCommitPhase::Reset,
            .event = dxmt9::d3d9::pe::RecorderCommitEvent::WarmEpochAdvance,
        });
        const bool finishDrainAdmitted =
            finishDrain.valid() &&
            finishDrain.action() ==
                dxmt9::d3d9::pe::RecorderCommitAction::FinishDrain;
        const bool resetAdmitted =
            resetPlan.valid() && resetPlan.resetBuilder() &&
            resetPlan.action() ==
                dxmt9::d3d9::pe::RecorderCommitAction::ResetBuilder &&
            dxmt9::d3d9::pe::recorderResetAfterDrainAllowed(
                false, false, false);
        const bool warmAdmitted =
            warmPlan.valid() && warmPlan.advanceWarmEpoch() &&
            warmPlan.action() ==
                dxmt9::d3d9::pe::RecorderCommitAction::AdvanceWarmEpoch &&
            dxmt9::d3d9::pe::recorderWarmAdvanceAllowed(
                resetAdmitted, finishDrainAdmitted);
        const bool cleanupSettled = finishDrainAdmitted && resetAdmitted &&
            warmAdmitted;
        DXMT_ASSERT(cleanupSettled);
        if (!cleanupSettled) {
            (void)recorderState_.chunkTransaction.poison();
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        if ((!finishDrainAdmitted || !resetAdmitted || !warmAdmitted) &&
            peCaptureState_) {
            markRenderTapeInvalidOnce("commit_cleanup_settlement");
        }
        if (!recordCapacityPost(true)) {
            (void)recorderState_.chunkTransaction.poison();
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        const bool transactionCompleted =
            recorderState_.chunkTransaction.complete();
        if (!transactionCompleted) {
            (void)recorderState_.chunkTransaction.poison();
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        (void)semanticOwner->settle();
        if (semanticRecorderState_) semanticRecorderState_->cadenceBytes = 0u;
        recorderState_.chunkTransaction.discard();
    }
    return hr;
}

HRESULT D3D9DeviceImpl::flushPeRecorder(
    PeRecorderFlushReason reason, PeRecorderFlushDisposition disposition) {
    const auto flushAction = planPeRecorderFlush(
        disposition, recorderState_.stateBlockTransaction.isPoisoned());
    if (flushAction != PeRecorderFlushAction::Submit) {
        return flushPendingCommandChunk(reason, disposition);
    }
    const HRESULT barrierHr = chunkBarrierFlush();
    if (FAILED(barrierHr)) return barrierHr;
    return flushPendingCommandChunk(reason, disposition);
}

HRESULT D3D9DeviceImpl::appendSetConstRecord(uint32_t recordType, UINT start, UINT count,
                             const void* data, std::size_t elemSize) {
    const std::uint64_t payload64 = static_cast<std::uint64_t>(count) * elemSize;
    if (payload64 > 0xffffffffull - kStableSetConstCadenceBytes) {
        return D3DERR_INVALIDCALL;
    }
    const std::uint32_t payloadBytes = static_cast<std::uint32_t>(payload64);
    if (payloadBytes != 0 && !data) {
        return D3DERR_INVALIDCALL;
    }
    const auto* policy = dxmt9::d3d9::pe::peSemanticProducerPolicy(recordType);
    if (!policy) return D3DERR_INVALIDCALL;
    armSemanticRecord(
        policy->kind, recordType,
        dxmt9::d3d9::pe::PeSemanticRecordInput{
            .setConst = D9CCommandChunkWireSetConst{
                .startRegister = start, .registerCount = count},
            .constantBytes = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(data), payloadBytes)});

    // The stable header+payload estimate keeps the prior capacity-precheck
    // behavior, so chunk seal cadence is unchanged. Both guards above are
    // untouched, which is why flushConstShadow's DXMT9_SPLIT_SPARSE_CONST_
    // RECORDS diagnostic path and its telemetry need no changes: this is
    // the single emitter behind all six VS/PS constant kinds.
    return appendRecord(
        recordType,
        kStableSetConstCadenceBytes + payloadBytes);
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
            recordType, start, count, data, elemSize);
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
    HRESULT hr = flushConstShadow(recorderState_.peConsts.vsConstF, D9C_COMMAND_RECORD_SET_VS_CONST_F, sizeof(float) * 4);
    if (FAILED(hr)) return hr;
    hr = flushConstShadow(recorderState_.peConsts.vsConstI, D9C_COMMAND_RECORD_SET_VS_CONST_I, sizeof(int32_t) * 4);
    if (FAILED(hr)) return hr;
    hr = flushConstShadow(recorderState_.peConsts.vsConstB, D9C_COMMAND_RECORD_SET_VS_CONST_B, sizeof(uint32_t));
    if (FAILED(hr)) return hr;
    hr = flushConstShadow(recorderState_.peConsts.psConstF, D9C_COMMAND_RECORD_SET_PS_CONST_F, sizeof(float) * 4);
    if (FAILED(hr)) return hr;
    hr = flushConstShadow(recorderState_.peConsts.psConstI, D9C_COMMAND_RECORD_SET_PS_CONST_I, sizeof(int32_t) * 4);
    if (FAILED(hr)) return hr;
    hr = flushConstShadow(recorderState_.peConsts.psConstB, D9C_COMMAND_RECORD_SET_PS_CONST_B, sizeof(uint32_t));
    if (FAILED(hr)) return hr;
    return S_OK;
}

HRESULT D3D9DeviceImpl::chunkBarrierFlush() {
    Dxmt9PeAppendFamilyScope appendFamily(diagnostics_.get(), PeInterAppendCallFamily::Barrier);
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
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
    recorderState_.peSparsePayloads = dxmt9::d3d9::pe::PeDrawPayloads{};
    const bool built = buildSparseStateForRecord(applyParams, false, false);
    if (built) {
        recordPeApplyStateBuildCpu(buildEntryNs);
        armSemanticRecord(
            dxmt9::d3d9::pe::PeSemanticProducerKind::ApplyState,
            D9C_COMMAND_RECORD_APPLY_STATE,
            dxmt9::d3d9::pe::PeSemanticRecordInput{
                .draw = recorderState_.peSparseHeader,
                .sparse = recorderState_.peSparseState});
        // The stable cadence estimate keeps seal boundaries unchanged.
        const HRESULT appendHr = appendRecord(
            D9C_COMMAND_RECORD_APPLY_STATE,
            kStableApplyStateCadenceBytes);
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

template <typename Fill>
HRESULT D3D9DeviceImpl::appendSingleCategoryApplyState(Fill fill) {
    recorderState_.peSparseState = dxmt9::d3d9::pe::SparseStateInput{};
    fill();
    armSemanticRecord(
        dxmt9::d3d9::pe::PeSemanticProducerKind::ApplyState,
        D9C_COMMAND_RECORD_APPLY_STATE,
        dxmt9::d3d9::pe::PeSemanticRecordInput{
            .draw = recorderState_.peSparseHeader,
            .sparse = recorderState_.peSparseState});
    return appendRecord(
        D9C_COMMAND_RECORD_APPLY_STATE,
        kStableApplyStateCadenceBytes);
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
    auto renderStates = recorderState_.peState.pendingRenderStatesTyped();
    while (!renderStates.empty()) {
        const std::size_t n = recorderState_.peState.prepareRenderStateBatch(
            recorderState_.peSparseScratch.renderStates);
        const HRESULT hr = appendSingleCategoryApplyState(
            [&] {
                recorderState_.peSparseState.renderStates =
                    std::span(recorderState_.peSparseScratch.renderStates).first(n);
            });
        if (FAILED(hr)) return hr;
    }
    auto textureStageStates = recorderState_.peState.pendingTssTyped();
    while (!textureStageStates.empty()) {
        const std::size_t n = recorderState_.peState.prepareTextureStageStateBatch(
            recorderState_.peSparseScratch.textureStageStates);
        const HRESULT hr = appendSingleCategoryApplyState(
            [&] {
                recorderState_.peSparseState.textureStageStates =
                    std::span(recorderState_.peSparseScratch.textureStageStates).first(n);
            });
        if (FAILED(hr)) return hr;
    }
    auto samplerStates = recorderState_.peState.pendingSamplerStatesTyped();
    while (!samplerStates.empty()) {
        const std::size_t n = recorderState_.peState.prepareSamplerStateBatch(
            recorderState_.peSparseScratch.samplerStates);
        const HRESULT hr = appendSingleCategoryApplyState(
            [&] {
                recorderState_.peSparseState.samplerStates =
                    std::span(recorderState_.peSparseScratch.samplerStates).first(n);
            });
        if (FAILED(hr)) return hr;
    }
    auto transforms = recorderState_.peState.pendingTransformsTyped();
    while (!transforms.empty()) {
        const std::size_t n = recorderState_.peState.prepareTransformBatch(
            recorderState_.peSparseScratch.transforms);
        const HRESULT hr = appendSingleCategoryApplyState(
            [&] {
                recorderState_.peSparseState.transforms =
                    std::span(recorderState_.peSparseScratch.transforms).first(n);
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
    recorderState_.peSparsePayloads = dxmt9::d3d9::pe::PeDrawPayloads{};
    const bool tailBuilt = buildSparseStateForRecord(tailParams, false);
    if (!tailBuilt) {
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
    armSemanticRecord(
        dxmt9::d3d9::pe::PeSemanticProducerKind::ApplyState,
        D9C_COMMAND_RECORD_APPLY_STATE,
        dxmt9::d3d9::pe::PeSemanticRecordInput{
            .draw = recorderState_.peSparseHeader,
            .sparse = recorderState_.peSparseState});
    const HRESULT hr = appendRecord(
        D9C_COMMAND_RECORD_APPLY_STATE,
        kStableApplyStateCadenceBytes);
    if (FAILED(hr)) return hr;
    return S_OK;
}
