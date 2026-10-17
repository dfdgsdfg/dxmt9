/* src/d3d9/d3d9_pe_device_recorder.cpp — cold recorder settlement/flush.
 *
 * Commit, cleanup, and constant-drain paths are intentionally out of the
 * device class header. The hot owner calls these semantic cold boundaries
 * normally; the generic append envelope remains header-defined for its
 * recorder/cold-COM instantiations. */

#include "d3d9_pe_device_impl.hpp"
#include "d3d9_pe_append_preparation.hpp"
#include "d3d9_pe_trace.hpp"

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
    semanticRecorderState_->inputValid = false;
    if (semanticRecorderState_->stagedSparsePlan.prepared())
        semanticRecorderState_->stagedSparsePlan.invalidate();
    auto& staged = semanticRecorderState_->stagedInput;
    staged = input;
    staged.producer = producer;
    staged.recordType = recordType;
    staged.sourceOrdinal = 0u;
    staged.recordOrdinal = 0u;
    semanticRecorderState_->stagedFamily = PeSemanticStagingFamily::Cold;
    semanticRecorderState_->sourceOrdinalHint = 0u;
    semanticRecorderState_->inputValid = true;
}

void D3D9DeviceImpl::armSemanticRecord(
    dxmt9::d3d9::pe::PeSemanticProducerKind producer,
    std::uint32_t recordType,
    const D9CCommandChunkWireDrawHeader& draw,
    const dxmt9::d3d9::pe::SparseStateInput& sparse) noexcept {
    if (!semanticRecorderState_) return;
    if (semanticRecorderState_->stagedSparsePlan.prepared())
        semanticRecorderState_->stagedSparsePlan.invalidate();
    stagePeSemanticDrawInput(
        semanticRecorderState_->stagedInput,
        semanticRecorderState_->stagedFamily,
        semanticRecorderState_->inputValid, producer, recordType, draw, sparse);
    semanticRecorderState_->sourceOrdinalHint = 0u;
}

void D3D9DeviceImpl::armSemanticRecord(
    dxmt9::d3d9::pe::PeSemanticProducerKind producer,
    std::uint32_t recordType,
    const D9CCommandChunkWireSetConst& setConst,
    std::span<const std::byte> constantBytes) noexcept {
    if (!semanticRecorderState_) return;
    if (semanticRecorderState_->stagedSparsePlan.prepared())
        semanticRecorderState_->stagedSparsePlan.invalidate();
    stagePeSemanticConstantInput(
        semanticRecorderState_->stagedConstantInput,
        semanticRecorderState_->stagedFamily,
        semanticRecorderState_->inputValid, producer, recordType, setConst,
        constantBytes);
    semanticRecorderState_->sourceOrdinalHint = 0u;
}

void D3D9DeviceImpl::armSemanticSparsePlan(
    dxmt9::d3d9::pe::PeSemanticProducerKind producer,
    std::uint32_t recordType) noexcept {
    if (!semanticRecorderState_) return;
    semanticRecorderState_->inputValid = false;
    auto& plan = semanticRecorderState_->stagedSparsePlan;
    if (!plan.prepared() || plan.recordType() != recordType) {
        plan.invalidate();
        return;
    }
    semanticRecorderState_->stagedSparsePlanProducer = producer;
    semanticRecorderState_->stagedFamily = PeSemanticStagingFamily::SparsePlan;
    semanticRecorderState_->sourceOrdinalHint = 0u;
    semanticRecorderState_->inputValid = true;
}

void D3D9DeviceImpl::clearSemanticRecordInput() noexcept {
    if (!semanticRecorderState_) return;
    // Keep the family frontier for reuse: the next same-family arm rewrites
    // every active field, while inputValid gates the unpublished staging.
    semanticRecorderState_->inputValid = false;
    semanticRecorderState_->sourceOrdinalHint = 0u;
    if (semanticRecorderState_->stagedSparsePlan.prepared())
        semanticRecorderState_->stagedSparsePlan.invalidate();
}

bool D3D9DeviceImpl::settleSemanticRecord(
    const dxmt9::d3d9::pe::PeSemanticRecordInputView& input,
    std::uint64_t recordOrdinal) noexcept {
    if (!input.valid()) return false;
    const auto& staged = *input.staged;
    const auto& sparse = *input.destination.sparse;
    const auto standaloneConstantReady =
        [&](const ConstShadow& shadow, std::size_t elementBytes) noexcept {
            const auto expected =
                static_cast<std::size_t>(staged.setConst.registerCount) *
                elementBytes;
            return expected == staged.constantBytes.size() && shadow.dirty() &&
                   shadow.dirtyStart == staged.setConst.startRegister &&
                   shadow.dirtyEnd - shadow.dirtyStart ==
                       staged.setConst.registerCount;
        };
    switch (staged.producer) {
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
    if (!appendPlan.valid() ||
        !dxmt9::d3d9::pe::acceptPreparedSparseState(
            recorderState_.peState, recorderState_.peConsts, sparse,
            appendPlan, scalarSemanticObserver(), recordOrdinal,
            recorderState_.chunkTransaction.pendingTicket())) {
        return false;
    }
    // Standalone Set*Constant records are not represented by sparse ranges;
    // acceptPreparedSparseState therefore must not consume an arbitrary dirty
    // range on their behalf.  Consume only the exact range admitted by this
    // record, preserving unrelated dirty constants for their own producer.
    switch (staged.producer) {
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

bool D3D9DeviceImpl::settleSemanticSparsePlan(
    const dxmt9::d3d9::pe::RecorderLockCapability& access,
    const dxmt9::d3d9::pe::SparseStatePlan& plan,
    std::uint64_t recordOrdinal) noexcept {
    const auto appendPlan = dxmt9::d3d9::pe::settleRecorderAppend({
        .phase = dxmt9::d3d9::pe::AppendSettlement::Prepared,
        .appendSucceeded = true,
    });
    const auto transactionTicket =
        recorderState_.chunkTransaction.pendingTicket();
    const auto pendingTicket = plan.pendingTicket();
    if (!pendingTicket.valid() || !transactionTicket.valid() ||
        transactionTicket.generation != pendingTicket.generation) {
        return false;
    }
    return dxmt9::d3d9::pe::acceptSparseStatePlan(
        access, recorderState_.peState, recorderState_.peConsts, plan,
        appendPlan, scalarSemanticObserver(), recordOrdinal,
        transactionTicket);
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

    return dxmt9::d3d9::pe::withPeSemanticAppendPreparation(
        semanticRecorderState_->stagedFamily, semanticRecorderState_->stagedInput,
        semanticRecorderState_->stagedConstantInput,
        semanticRecorderState_->stagedSparsePlan,
        semanticRecorderState_->stagedSparsePlanProducer,
        [&](auto& preparation) {
          return appendSemanticRecordPrepared(type, bytes, semanticOwner,
                                              preparation);
        });
}

template <typename Preparation>
HRESULT D3D9DeviceImpl::appendSemanticRecordPrepared(
    std::uint32_t type, std::size_t bytes,
    PeProductionSemanticBatchOwner* semanticOwner,
    Preparation& preparation) {
    constexpr bool constantFamily =
        dxmt9::d3d9::pe::isPeSemanticConstantAppendPreparation<Preparation>;
    constexpr bool sparsePlanFamily =
        dxmt9::d3d9::pe::isPeSemanticSparseAppendPreparation<Preparation>;

    auto& stagedInput = semanticRecorderState_->stagedInput;
    const auto maxRecords = maxPendingCommandRecords();
    const auto maxBytes = maxPendingCommandBytes();
    const auto recordCountBefore = semanticOwner->size();
    const auto payloadBytesBefore = semanticRecorderState_->cadenceBytes;
    // This is the only destination-context rebuild. The normal path borrows
    // the staged descriptor directly; only draw stream/index spans are copied
    // into the recorder-owned overlay. CapacityPre calls this again after the
    // destination changes, without copying the full staged input.
    const auto rebase = [&]() __attribute__((always_inline)) noexcept {
        const auto source = semanticRecorderState_->sourceOrdinalHint != 0u
            ? semanticRecorderState_->sourceOrdinalHint
            : semanticOwner->nextSourceOrdinal();
        const auto record = semanticOwner->nextRecordOrdinal();
        if constexpr (constantFamily) {
            preparation.setDestination(type, source, record);
            return true;
        } else if constexpr (sparsePlanFamily) {
            preparation.setDestination(type, source, record);
            PeRecorderGuard planGuard(recorderState_.recorderMutex,
                                      recorderState_.recorderLockRequired);
            auto& plan = *preparation.plan;
            return planGuard.withCapability(
                recorderState_.recorderOwnership,
                recorderState_.stateBlockTransaction.capabilityEpoch(),
                [&](const auto& access) noexcept {
                  const auto transactionTicket =
                      recorderState_.chunkTransaction.pendingTicket();
                  const bool destinationDependent =
                      type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
                      type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
                  if (!destinationDependent) {
                    const auto* const sourceFacts = plan.source(access);
                    if (!sourceFacts || !transactionTicket.valid() ||
                        transactionTicket.generation != sourceFacts->pendingTicket.generation ||
                        !recorderState_.peState.pendingTicketMatches(
                            sourceFacts->pendingTicket)) return false;
                    return plan.destination().chunkContextFinalized;
                  }
                  // Compact plans resolve only destination-dependent facts here.
                  // The generic currentChunkContext() keeps its full retention walk
                  // for the compatibility/full-snapshot path.
                  const auto resolve = [](const auto& access, auto& plan,
                                          const auto* semanticOwner,
                                          const auto transactionTicket,
                                          const auto currentTicket,
                                          bool indexKnown,
                                          std::uint64_t indexWire)
                      __attribute__((always_inline)) noexcept {
                    dxmt9::d3d9::pe::PeChunkContext chunk{};
                    chunk.indexBufferKnown = indexKnown;
                    chunk.submittedIndexBufferWire = indexWire;
                    const auto retained =
                        [semanticOwner](
                            std::uint32_t,
                            const dxmt9::d3d9::pe::BufferRef& buffer) noexcept {
                          return semanticOwner->referencesBuffer(buffer);
                        };
                    return dxmt9::d3d9::pe::finalizeSparseStatePlanChunkContext(
                        access, chunk, plan, retained,
                        transactionTicket, currentTicket);
                  };
                  if (diagnostics_ && diagnostics_->config.statsDecimationN != 0u) {
                    return dxmt9PeObserveCompactScope(
                        diagnostics_->peDrawPacketContextDecimatedStats_,
                        diagnostics_->peCompactSampleNesting_, [&]() noexcept {
                          return resolve(access, plan, semanticOwner,
                                         transactionTicket,
                                         recorderState_.peState.pendingTicket(),
                                         submittedIndexBufferKnown_,
                                         submittedIndexBufferWireValue_);
                        });
                  }
                  return resolve(access, plan, semanticOwner,
                                 transactionTicket,
                                 recorderState_.peState.pendingTicket(),
                                 submittedIndexBufferKnown_,
                                 submittedIndexBufferWireValue_);
                });
        } else {
            preparation.setDestination(type, source, record);
            if (stagedInput.producer !=
                    dxmt9::d3d9::pe::PeSemanticProducerKind::DrawPrimitive &&
                stagedInput.producer !=
                    dxmt9::d3d9::pe::PeSemanticProducerKind::DrawIndexedPrimitive) {
              return true;
            }
            auto& destinationSparse = semanticRecorderState_->destinationSparse;
            destinationSparse = stagedInput.sparse;
            preparation.inputView.destination.sparse = &destinationSparse;
            dxmt9::d3d9::pe::PeDrawParams params{};
            params.recordType = type;
            params.primitiveType = stagedInput.draw.primitiveType;
            params.baseVertex = stagedInput.draw.baseVertex;
            params.minVertex = stagedInput.draw.minVertex;
            params.numVertices = stagedInput.draw.numVertices;
            params.startVertex = stagedInput.draw.startVertex;
            params.startIndex = stagedInput.draw.startIndex;
            params.primitiveCount = stagedInput.draw.primitiveCount;
            return addChunkContextSections(
                currentChunkContext(), recorderState_.peState,
                recorderState_.peBindingView, params,
                recorderState_.peSparseScratch, destinationSparse);
        }
    };
    const auto rebaseInputView = [&]() noexcept {
        if (diagnostics_ && diagnostics_->config.statsDecimationN != 0u) {
            return dxmt9PeObserveCompactScope(
                diagnostics_->peDrawPacketDestinationDecimatedStats_,
                diagnostics_->peCompactSampleNesting_, rebase);
        }
        return rebase();
    };
    const auto sourceOrdinal = [&]() noexcept {
        if constexpr (constantFamily)
            return preparation.input->sourceOrdinal;
        else if constexpr (sparsePlanFamily)
            return preparation.sourceOrdinal;
        else
            return preparation.inputView.destination.sourceOrdinal;
    };
    const auto recordOrdinal = [&]() noexcept {
        if constexpr (constantFamily)
            return preparation.input->recordOrdinal;
        else if constexpr (sparsePlanFamily)
            return preparation.recordOrdinal;
        else
            return preparation.inputView.destination.recordOrdinal;
    };
    const bool willFlushBeforeAppend = recordCountBefore != 0u &&
        (recordCountBefore >= maxRecords ||
         payloadBytesBefore + bytes > maxBytes);
    std::uint32_t appendedRecordCount =
        willFlushBeforeAppend ? 1u : recordCountBefore + 1u;
    std::size_t appendedPayloadBytes =
        willFlushBeforeAppend ? bytes : payloadBytesBefore + bytes;
    bool willFlushAfterAppend =
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
    if (!dxmt9::d3d9::pe::resolvePeSemanticCadenceFrontier(
            *semanticOwner, priorHandles, priorPayload)) {
        priorHandles = priorPayload = 0u;
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
        clearSemanticRecordInput();
        return D3DERR_INVALIDCALL;
    }
    if (!rebaseInputView()) {
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
        clearSemanticRecordInput();
        return D3DERR_INVALIDCALL;
    }
    if (sourceOrdinal() == 0u || recordOrdinal() == 0u) {
        (void)recorderState_.settleSemanticEmitter(
            false, semanticOwner->size(), priorHandles, priorPayload,
            semanticOwner->retainedCount());
        clearSemanticRecordInput();
        return D3DERR_INVALIDCALL;
    }
    bool semanticSettlement = true;
    bool hasIndexedReceipt = false;
    std::uint64_t indexedReceiptWire = 0u;
    bool indexedReceiptBound = false;
    auto commitSemanticRecord =
        [&](const dxmt9::d3d9::pe::RecorderLockCapability* access)
        noexcept {
        bool indexedReceiptCandidate = false;
        if constexpr (sparsePlanFamily) {
            indexedReceiptCandidate =
                access != nullptr &&
                preparation.producer ==
                    dxmt9::d3d9::pe::PeSemanticProducerKind::DrawIndexedPrimitive &&
                preparation.plan->destination().indexBuffer;
        }
        bool indexedReceiptReady = !indexedReceiptCandidate;
        if (indexedReceiptCandidate) {
            // Receipt extraction is a source-borrow operation. It must finish
            // before settlement consumes the pending source and invalidates
            // the plan's dependency witness.
            if constexpr (sparsePlanFamily) {
              indexedReceiptReady = preparation.plan->withBindingSource(
                    *access,
                    [&](const dxmt9::d3d9::pe::PeBindingView& bindings) noexcept {
                      indexedReceiptWire = d9cWireHandleValue(
                          toWireHandle(bindings.indexBuffer.object));
                      indexedReceiptBound = bindings.indexBuffer.object != nullptr;
                    });
            }
        }
        if constexpr (sparsePlanFamily) {
            semanticSettlement = indexedReceiptReady && access != nullptr &&
                settleSemanticSparsePlan(*access, *preparation.plan,
                                         recordOrdinal());
        } else if constexpr (
            dxmt9::d3d9::pe::isPeSemanticGenericAppendPreparation<Preparation>) {
            semanticSettlement = settleSemanticRecord(preparation.inputView,
                                                      recordOrdinal());
        } else {
            semanticSettlement = false;
        }
        hasIndexedReceipt = indexedReceiptCandidate && indexedReceiptReady &&
                            semanticSettlement;
        return semanticSettlement;
    };
    const auto admitSemanticRecord = [&]() noexcept {
        if constexpr (constantFamily) {
            dxmt9::d3d9::pe::PeSemanticConstantSettlement settlement(
                recorderState_.peConsts, recorderState_.peState,
                recorderState_.chunkTransaction.pendingTicket());
            const bool accepted = semanticOwner->tryAppendConstantRecord(
                *preparation.input, settlement);
            semanticSettlement = semanticOwner->lastAdmissionFailure() !=
                PeProductionSemanticBatchOwner::AdmissionFailure::Settlement;
            return accepted;
        } else if constexpr (sparsePlanFamily) {
            PeRecorderGuard planGuard(recorderState_.recorderMutex,
                                      recorderState_.recorderLockRequired);
            return planGuard.withCapability(
                recorderState_.recorderOwnership,
                recorderState_.stateBlockTransaction.capabilityEpoch(),
                [&](const auto& access) noexcept {
                  return semanticOwner->tryAppendSparseStatePlan(
                      access, *preparation.plan, preparation.producer, type,
                      D9C_COMMAND_CHUNK_RECORD_FLAG_NONE, sourceOrdinal(),
                      recordOrdinal(),
                      [&]() noexcept { return commitSemanticRecord(&access); });
                });
        } else {
            return semanticOwner->tryAppendOwnedRecord(
                preparation.inputView,
                [&]() noexcept { return commitSemanticRecord(nullptr); });
        }
    };
    bool admitted = admitSemanticRecord();
    if (!admitted &&
        semanticOwner->lastAdmissionFailure() ==
            PeProductionSemanticBatchOwner::AdmissionFailure::Capacity &&
        !willFlushBeforeAppend && semanticOwner->size() != 0u) {
        // The owner has not mutated on Capacity. Roll back only the recorder's
        // active intent, flush the existing destination, rebuild its
        // destination-dependent sparse projection, and retry once.
        if (!recorderState_.settleSemanticEmitter(
                false, semanticOwner->size(), priorHandles, priorPayload,
                semanticOwner->retainedCount())) {
            (void)recorderState_.chunkTransaction.poison();
            poisonStateBlockTransaction();
            clearSemanticRecordInput();
            return D3DERR_DEVICELOST;
        }
        if (!recorderState_.recordCapacityPreEvidence(true)) {
            (void)recorderState_.chunkTransaction.poison();
            poisonStateBlockTransaction();
            clearSemanticRecordInput();
            return D3DERR_DEVICELOST;
        }
        const HRESULT preHr =
            flushPendingCommandChunk(PeRecorderFlushReason::CapacityPre);
        if (FAILED(preHr)) {
            clearSemanticRecordInput();
            return preHr;
        }
        if (!dxmt9::d3d9::pe::resolvePeSemanticCadenceFrontier(
                *semanticOwner, priorHandles, priorPayload)) {
            priorHandles = priorPayload = 0u;
        }
        willFlushAfterAppend =
            semanticOwner->size() + 1u >= maxRecords || bytes >= maxBytes;
        appendedRecordCount = 1u;
        appendedPayloadBytes = bytes;
        if (!recorderState_.prepareChunkRecord(
                type, bytes, true, semanticOwner->size(), priorHandles,
                priorPayload, semanticOwner->retainedCount()) ||
            !semanticRecorderState_->inputValid) {
            (void)recorderState_.chunkTransaction.poison();
            poisonStateBlockTransaction();
            clearSemanticRecordInput();
            return D3DERR_DEVICELOST;
        }
        if (!rebaseInputView() || sourceOrdinal() == 0u ||
            recordOrdinal() == 0u) {
            clearSemanticRecordInput();
            poisonStateBlockTransaction();
            return D3DERR_INVALIDCALL;
        }
        semanticSettlement = true;
        admitted = admitSemanticRecord();
    }
    if (!admitted && dxmt9PeRecorderChunkLogEnabled()) {
        dxmt9DeviceInfoLog(
            "pe_semantic_failure_stage type=%u stage=owner_admission "
            "records=%zu retained=%zu settlement=%u admission=%u",
            type, semanticOwner->size(), semanticOwner->retainedCount(),
            semanticSettlement ? 1u : 0u,
            static_cast<unsigned>(semanticOwner->lastAdmissionFailure()));
    }
    if constexpr (dxmt9::d3d9::pe::isPeSemanticGenericAppendPreparation<Preparation>) {
        if (admitted &&
            stagedInput.producer ==
                dxmt9::d3d9::pe::PeSemanticProducerKind::DrawIndexedPrimitive &&
            preparation.inputView.destination.sparse->indexBuffers.size() == 1u) {
          const auto& index =
              preparation.inputView.destination.sparse->indexBuffers.front();
          hasIndexedReceipt = true;
          indexedReceiptWire = d9cWireHandleValue(
              toWireHandle(index.object.object));
          indexedReceiptBound = index.object.object != nullptr;
        }
    }
    clearSemanticRecordInput();
    std::size_t handles = 0u;
    std::size_t payload = 0u;
    dxmt9::d3d9::pe::PeSemanticEmissionFrontier frontier{};
    if (!semanticOwner->emissionFrontier(frontier)) {
        // A failed callback admission has already rolled back the owner and its
        // typed pins. A successful admission with an invalid projection or
        // an unsuccessful transaction rollback requires fail-stop.
        const bool rolledBack = recorderState_.settleSemanticEmitter(
            false, semanticOwner->size(), priorHandles, priorPayload,
            semanticOwner->retainedCount());
        const HRESULT rejectedHr =
            dxmt9PeRecorderAdmissionFailureHresult(D3DERR_INVALIDCALL);
        if (admitted || !rolledBack) {
            (void)recorderState_.chunkTransaction.poison();
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        // An empty rolled-back owner has no emission frontier. A nonempty
        // invalid frontier must not be masked by the consumed fault receipt.
        return semanticOwner->size() == 0u ? rejectedHr : D3DERR_INVALIDCALL;
    }
    handles = frontier.handleCount;
    payload = frontier.payloadBytes;
    if (!admitted || !recorderState_.settleSemanticEmitter(
                          admitted, semanticOwner->size(), handles, payload,
                          semanticOwner->retainedCount())) {
        const bool rolledBack = recorderState_.settleSemanticEmitter(
            false, semanticOwner->size(), priorHandles, priorPayload,
            semanticOwner->retainedCount());
        const HRESULT rejectedHr =
            dxmt9PeRecorderAdmissionFailureHresult(D3DERR_INVALIDCALL);
        if (admitted || !rolledBack) {
            (void)recorderState_.chunkTransaction.poison();
            poisonStateBlockTransaction();
            return D3DERR_DEVICELOST;
        }
        return rejectedHr;
    }
    if (!observeCommittedSemanticOwnerRecord()) {
        (void)recorderState_.chunkTransaction.poison();
        poisonStateBlockTransaction();
        return D3DERR_DEVICELOST;
    }
    // The semantic owner has already applied destination-chunk selection to
    // the sparse overlay. Advance the indexed-draw witness only after the
    // owner transaction is durable.
    if (admitted && hasIndexedReceipt) {
        submittedIndexBufferWireValue_ = indexedReceiptWire;
        submittedIndexBufferKnown_ = indexedReceiptBound;
    }
    semanticRecorderState_->cadenceBytes = appendedPayloadBytes;
    return willFlushAfterAppend
        ? flushPendingCommandChunk(PeRecorderFlushReason::CapacityPost)
        : S_OK;
}

HRESULT D3D9DeviceImpl::retryPendingSemanticChunk() {
    auto* const owner = semanticBatchOwner();
    dxmt9::d3d9::pe::PeSemanticEmissionFrontier frontier{};
    const bool retryBytes = owner != nullptr && owner->size() != 0u &&
        owner->emissionFrontier(frontier);
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
    if (!dxmt9::d3d9::pe::resolvePeSemanticCadenceFrontier(
            *owner, handles, payload)) {
        return D3DERR_DEVICELOST;
    }
    std::int32_t injectedFaultHr = 0;
    // This is the semantic lane's pre-effect capacity seam. No owner state
    // has changed, so the caller may retry the same source ordinal verbatim.
    const bool willFlushBeforeAppend =
        dxmt9::d3d9::pe::peSemanticFaultWouldFlush(
            dxmt9PeRecorderFaultsEnabled(), owner->size(), payload, bytes,
            [&]() noexcept {
              return dxmt9::d3d9::pe::PeSemanticAppendLimits{
                  .maxRecords = maxPendingCommandRecords(),
                  .maxBytes = maxPendingCommandBytes()};
            });
    if (willFlushBeforeAppend &&
        dxmt9PeConsumeRecorderFault(PeRecorderFaultPoint::CapacityPreReserve,
                                    injectedFaultHr)) {
        return static_cast<HRESULT>(injectedFaultHr);
    }
    if (sourceOrdinal != 0u) {
        semanticRecorderState_->sourceOrdinalHint = sourceOrdinal;
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
        clearSemanticRecordInput();
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
    auto* const semanticOwner = semanticBatchOwner();
    const bool semanticEvidence = semanticOwner &&
        [&]() noexcept {
          dxmt9::d3d9::pe::PeSemanticEmissionFrontier frontier{};
          if (!semanticOwner->emissionFrontier(frontier)) return false;
          semanticHandleCount = frontier.handleCount;
          semanticPayloadBytes = frontier.payloadBytes;
          return true;
        }();
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
            const bool peTraceRequested = dxmt9PeTraceEnabled();
            const bool peTraceActive =
                peTraceRequested && dxmt9PeTraceChunkBegin();
            const HRESULT bridgeHr = hr32(
                segmented
                    ? dxmt9c_device_commit_chunk_segmented(
                          dev_, &segmented->transport)
                    : dxmt9c_device_commit_chunk(dev_, &submittedChunk));
            if (peTraceActive) dxmt9PeTraceChunkEnd(dev_);
            // This is the only authoritative PE-side acceptance point for a
            // capture event. Stamp before any later capture settlement or
            // diagnostics can fail; callers may then wait on this exact
            // token/ordinal without predicting the next event.
            if (captureState &&
                captureState->renderTapeCommitCollector &&
                SUCCEEDED(bridgeHr)) {
                PeRenderTapeCommitReceipt accepted{};
                accepted.bridgeAccepted = true;
                accepted.captureToken = submittedChunk.renderTapeCaptureToken;
                accepted.eventOrdinal = submittedChunk.renderTapeEventOrdinal;
                captureState->renderTapeCommitCollector->observe(accepted);
            }
            HRESULT hr = bridgeHr;
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
    auto* const semanticOwner = semanticBatchOwner();
    dxmt9::d3d9::pe::PeSemanticEmissionFrontier semanticRetryFrontier{};
    if (!semanticOwner) return D3DERR_NOTAVAILABLE;
    const bool semanticRetryBytes = semanticOwner->size() != 0u &&
        semanticOwner->emissionFrontier(semanticRetryFrontier);
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
    dxmt9::d3d9::pe::PeSemanticEmissionFrontier semanticFrontier{};
    const bool semanticMetricsValid =
        semanticOwner->emissionFrontier(semanticFrontier);
    semanticHandleCount = semanticFrontier.handleCount;
    semanticPayloadBytes = semanticFrontier.payloadBytes;
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
    const bool emitted = useSegmented
        ? semanticOwner->emitSegmented(semanticSegmented)
        : semanticOwner->emitExactFixed(semanticExact);
    if (useSegmented && emitted) {
        segmented.transport = semanticSegmented.transport;
        segmented.wireBytes = semanticSegmented.wireBytes;
    }
    // A rejected segmented descriptor is a failed seal, not permission to
    // submit the never-emitted ExactFixed alternative with zero record bytes.
    if (!emitted || (useSegmented && !segmented.valid())) {
        const auto sealPlan = dxmt9::d3d9::pe::settleRecorderCommit({
            .phase = dxmt9::d3d9::pe::RecorderCommitPhase::Unsealed,
            .event = dxmt9::d3d9::pe::RecorderCommitEvent::SealFailed,
        });
        (void)sealPlan;
        (void)recorderState_.chunkTransaction.recordSealResult(false);
        if (!recordCapacityPost(false)) return D3DERR_DEVICELOST;
        return D3DERR_INVALIDCALL;
    }
    if ((useSegmented && segmented.wireBytes > 0xffffffffull) ||
        (!useSegmented && semanticExact.wireBytes > 0xffffffffull)) {
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
    if (!useSegmented) {
        chunk.recordCount = semanticExact.transport.header.recordCount;
        chunk.recordBytes = semanticExact.wireBytes;
        chunk.records = toWireHandle(semanticExact.wire.data());
        chunk.handleCount = semanticExact.transport.header.handleCount;
        chunk.producerIdentity = semanticExact.transport.producerIdentity;
    } else {
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
                                  useSegmented ? &segmented : nullptr);
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
        D9CCommandChunkWireSetConst{
            .startRegister = start, .registerCount = count},
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data),
                                   payloadBytes));

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
    const bool observeVsRange =
        recordType == D9C_COMMAND_RECORD_SET_VS_CONST_F &&
        dxmt9PerfVsConstSetterRangeEnabled();
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
        // This count is diagnostic-only. Keep it before appendSetConstRecord
        // so the callback observes the pre-append dirty state, but do not pay
        // for the scan when VS range observation is disabled.
        const std::uint32_t dirtyRegs = observeVsRange
            ? countDirtyConstRegs(shadow, start, end)
            : 0u;
        hr = appendSetConstRecord(
            recordType, start, count, data, elemSize);
        if (SUCCEEDED(hr)) {
            ++flushedRecords;
            flushedRegs += count;
        }
        if (SUCCEEDED(hr) && observeVsRange) {
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
    const bool observeEmptyFlushes = dxmt9PeStatsDecimationN() != 0u;
    return dispatchPeConstFlushes(
        recorderState_.peConsts, observeEmptyFlushes,
        [this](ConstShadow& shadow, std::size_t family) -> HRESULT {
            switch (family) {
            case 0u:
                return flushConstShadow(
                    shadow, D9C_COMMAND_RECORD_SET_VS_CONST_F,
                    sizeof(float) * 4u);
            case 1u:
                return flushConstShadow(
                    shadow, D9C_COMMAND_RECORD_SET_VS_CONST_I,
                    sizeof(std::int32_t) * 4u);
            case 2u:
                return flushConstShadow(
                    shadow, D9C_COMMAND_RECORD_SET_VS_CONST_B,
                    sizeof(std::uint32_t));
            case 3u:
                return flushConstShadow(
                    shadow, D9C_COMMAND_RECORD_SET_PS_CONST_F,
                    sizeof(float) * 4u);
            case 4u:
                return flushConstShadow(
                    shadow, D9C_COMMAND_RECORD_SET_PS_CONST_I,
                    sizeof(std::int32_t) * 4u);
            case 5u:
                return flushConstShadow(
                    shadow, D9C_COMMAND_RECORD_SET_PS_CONST_B,
                    sizeof(std::uint32_t));
            }
            return D3DERR_INVALIDCALL;
        },
        [](HRESULT hr) noexcept { return FAILED(hr); });
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
    const bool explicitSnapshot =
        dxmt9::d3d9::pe::dxmt9PeFullSnapshotEnabled();
    bool builtPlan = false;
    bool builtExplicit = false;
    if (explicitSnapshot) {
        builtExplicit = buildSparseStateForRecord(applyParams, false, false);
    } else {
        builtPlan = recorderLock.withCapability(
            recorderState_.recorderOwnership,
            recorderState_.stateBlockTransaction.capabilityEpoch(),
            [&](const auto& access) noexcept {
              return buildSparseStatePlanForRecord(access, applyParams, false);
            });
    }
    if (builtPlan || builtExplicit) {
        recordPeApplyStateBuildCpu(buildEntryNs);
        if (builtPlan) {
            armSemanticSparsePlan(
                dxmt9::d3d9::pe::PeSemanticProducerKind::ApplyState,
                D9C_COMMAND_RECORD_APPLY_STATE);
        } else {
            armSemanticRecord(
                dxmt9::d3d9::pe::PeSemanticProducerKind::ApplyState,
                D9C_COMMAND_RECORD_APPLY_STATE,
                recorderState_.peSparseHeader, recorderState_.peSparseState);
        }
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
        recorderState_.peSparseHeader, recorderState_.peSparseState);
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
        recorderState_.peSparseHeader, recorderState_.peSparseState);
    const HRESULT hr = appendRecord(
        D9C_COMMAND_RECORD_APPLY_STATE,
        kStableApplyStateCadenceBytes);
    if (FAILED(hr)) return hr;
    return S_OK;
}
