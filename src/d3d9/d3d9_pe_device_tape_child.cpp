/* src/d3d9/d3d9_pe_device_tape_child.cpp — Render Tape child/lifetime drain.
 *
 * Child callbacks, alias/parent lifetime retirement, and pending-chunk drain
 * are kept together because they form the wrapper-to-tape ownership path. */

#include "d3d9_pe_device_impl.hpp"

void D3D9DeviceImpl::NotifyRenderTapeObjectDefineForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::span<const std::byte> descriptor,
    std::span<const std::byte> immutablePayload) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!peCaptureState_ ||
        peCaptureState_->renderTapeCapture.state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        return;
    }
    try {
        dxmt9::d3d9::RenderTapeDigest digest{};
        std::uint64_t bytes = 0u;
        if (!immutablePayload.empty()) {
            const auto status = peCaptureState_->renderTapeCapture.registerBlobBytes(
                immutablePayload, &digest);
            if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
                abortRenderTapeCapture("object_define_blob");
                return;
            }
            bytes = immutablePayload.size();
        }
        const auto descriptorKind =
            dxmt9::d3d9::renderTapeDescriptorKindForObject(
                object.identity.kind);
        if (descriptorKind ==
            dxmt9::d3d9::RenderTapeDescriptorKind::Invalid) {
            markRenderTapeInvalidOnce("descriptor_kind_invalid", &object);
            abortRenderTapeCapture("object_define_descriptor_kind");
            return;
        }
        dxmt9::d3d9::RenderTapeObjectDefineDisposition disposition{};
        const auto status = peCaptureState_->renderTapeCapture.objectDefine(
            object.identity, static_cast<std::uint32_t>(descriptorKind),
            descriptor, bytes, digest, 0u, 0u, &disposition);
        if (status == dxmt9::d3d9::RenderTapeCaptureStatus::Accepted &&
            disposition == dxmt9::d3d9::
                               RenderTapeObjectDefineDisposition::
                                   IdempotentSurfaceAlias) {
            dxmt9DeviceInfoLog(
                "render_tape_capture object_define reason=%s kind=%u "
                "generation=%u object_id=%llu descriptor=%zu",
                dxmt9::d3d9::renderTapeObjectDefineDispositionName(
                    disposition),
                object.identity.kind, object.identity.generation,
                static_cast<unsigned long long>(object.identity.objectId),
                descriptor.size());
        }
        if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            dxmt9DeviceInfoLog(
                "render_tape_capture object_define rejected status=%u reason=%s "
                "kind=%u generation=%u object_id=%llu descriptor=%zu "
                "immutable=%llu",
                static_cast<unsigned>(status),
                dxmt9::d3d9::renderTapeObjectDefineDispositionName(
                    disposition),
                object.identity.kind,
                object.identity.generation,
                static_cast<unsigned long long>(object.identity.objectId),
                descriptor.size(),
                static_cast<unsigned long long>(bytes));
            abortRenderTapeCapture("object_define");
        }
    } catch (...) {
        abortRenderTapeCapture("object_define_exception");
    }
}

bool D3D9DeviceImpl::IsRenderTapeCaptureActiveForChild() const noexcept {
    return peCaptureState_ &&
           peCaptureState_->renderTapeCapture.state() ==
               dxmt9::d3d9::RenderTapeCaptureState::Capturing;
}

bool D3D9DeviceImpl::IsRenderTapeCaptureTrackingEnabledForChild() const noexcept {
    return peCaptureState_ != nullptr;
}

void D3D9DeviceImpl::AbortRenderTapeCaptureForChild() noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    markRenderTapeInvalidOnce("child_abort");
    if (IsRenderTapeCaptureActiveForChild())
        abortRenderTapeCapture("child_abort");
}

void D3D9DeviceImpl::RejectRenderTapeCaptureForChild(
    dxmt9::d3d9::RenderTapeCaptureRejectionReason reason,
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::uint32_t subresource,
    const dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic &diagnostic)
    noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    const char *name =
        dxmt9::d3d9::renderTapeCaptureRejectionReasonName(reason);
    const bool first = peCaptureState_ && !peCaptureState_->renderTapeRegistry.invalid;
    markRenderTapeInvalidOnce(name, &object, subresource, diagnostic);
    if (first) {
        dxmt9DeviceInfoLog(
            "render_tape_capture first_rejection reason=%s kind=%u "
            "generation=%u object_id=%llu subresource=%u format=%u "
            "width=%u height=%u pitch=%d bytes=%llu",
            name, object.identity.kind, object.identity.generation,
            static_cast<unsigned long long>(object.identity.objectId),
            subresource, diagnostic.format, diagnostic.width,
            diagnostic.height, diagnostic.pitch,
            static_cast<unsigned long long>(diagnostic.bytes));
    }
    if (IsRenderTapeCaptureActiveForChild())
        abortRenderTapeCapture(name);
}

dxmt9::d3d9::RenderTapeFullSnapshotStatus
D3D9DeviceImpl::RenderTapeFullSnapshotStatusForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::uint32_t subresource, std::uint32_t fullRowBytes,
    std::uint32_t fullRows, std::uint64_t fullBytes) const noexcept {
    using Status = dxmt9::d3d9::RenderTapeFullSnapshotStatus;
    if (!peCaptureState_ ||
        (object.identity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE &&
         object.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER)) {
        return Status::NotRequired;
    }
    const auto *entry = findRenderTapeObject(object);
    if (!entry || subresource >= entry->content.size()) {
        return Status::InvalidIdentity;
    }
    if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        if (entry->descriptor.size() != sizeof(D9CBufferDesc) ||
            fullRowBytes == 0u || fullRows != 1u ||
            fullBytes != fullRowBytes) {
            return Status::InvalidExtent;
        }
        D9CBufferDesc desc{};
        std::memcpy(&desc, entry->descriptor.data(), sizeof(desc));
        if (fullRowBytes != desc.size || fullBytes != desc.size) {
            return Status::InvalidExtent;
        }
        return dxmt9::d3d9::renderTapeClassifyBufferSnapshot(
            true, true, true, true, entry->content[subresource].size(),
            desc.size);
    }
    D9CSurfaceDesc desc{};
    if (!renderTapeObjectSubresourceDesc(*entry, object, subresource, desc))
        return Status::InvalidIdentity;
    if (fullRowBytes == 0u || fullRows == 0u || fullBytes == 0u)
        return Status::InvalidExtent;
    std::uint64_t expectedBytes = 0u;
    if (renderTapeFormatIsBlockCompressed(desc.format)) {
        dxmt9::d3d9::RenderTapeBlockLockLayout expected{};
        if (dxmt9::d3d9::renderTapeBlockLockLayout(
                desc, static_cast<std::int32_t>(fullRowBytes), nullptr,
                expected) !=
                dxmt9::d3d9::RenderTapeBlockLayoutStatus::Accepted ||
            !expected.fullSubresource ||
            expected.fullRowBytes != fullRowBytes ||
            expected.fullRows != fullRows) {
            return Status::InvalidExtent;
        }
        expectedBytes = expected.tightBytes;
    } else {
        dxmt9::d3d9::RenderTapeLinearLockLayout expected{};
        if (dxmt9::d3d9::renderTapeLinearLockLayout(
                desc, static_cast<std::int32_t>(fullRowBytes), nullptr,
                expected) !=
                dxmt9::d3d9::RenderTapeLinearLayoutStatus::Accepted ||
            !expected.fullSubresource ||
            expected.fullRowBytes != fullRowBytes ||
            expected.fullRows != fullRows) {
            return Status::InvalidExtent;
        }
        expectedBytes = expected.tightBytes;
    }
    if (expectedBytes != fullBytes)
        return Status::InvalidExtent;
    const auto &content = entry->content[subresource];
    return dxmt9::d3d9::renderTapeClassifySnapshot(
        true, true, true, true, content.size(), expectedBytes);
}

void D3D9DeviceImpl::NotifyRenderTapeBlockMutationForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::uint32_t subresource,
    const dxmt9::d3d9::RenderTapeBlockLockLayout &layout,
    std::span<const std::byte> bytes) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    const auto status =
        recordRenderTapeBlockBytes(object, subresource, layout, bytes);
    if (status !=
        dxmt9::d3d9::RenderTapeBlockMutationStatus::Accepted) {
        if (status == dxmt9::d3d9::RenderTapeBlockMutationStatus::IncompleteSeed &&
            !renderTapeObjectAdmitted(object.identity)) {
            return;
        }
        const auto reason =
            status == dxmt9::d3d9::
                          RenderTapeBlockMutationStatus::IncompleteSeed
                ? dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                      IncompleteSubresourceSeed
                : dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                      DescriptorMismatch;
        D9CSurfaceDesc desc{};
        const auto *entry = findRenderTapeObject(object);
        if (entry && subresource < entry->content.size()) {
            (void)renderTapeObjectSubresourceDesc(
                *entry, object, subresource, desc);
        }
        logRenderTapeMutationFailure(
            "block", status, object, subresource, layout.fullRowBytes,
            layout.fullRows, layout.rowBytes, layout.rows, layout.pitch,
            bytes);
        RejectRenderTapeCaptureForChild(
            reason, object, subresource,
            {.format = desc.format,
             .width = desc.width,
             .height = desc.height,
             .pitch = static_cast<std::int32_t>(layout.pitch),
             .bytes = bytes.size()});
        return;
    }
    if (!IsRenderTapeCaptureActiveForChild())
        return;
    if (!renderTapeObjectAdmitted(object.identity))
        return;
    if (!appendRenderTapeUnlockMutation(object, subresource,
                                        "block_resource_mutation")) {
        abortRenderTapeCapture("block_resource_mutation");
    }
}

void D3D9DeviceImpl::NotifyRenderTapeLinearMutationForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::uint32_t subresource,
    const dxmt9::d3d9::RenderTapeLinearLockLayout &layout,
    std::span<const std::byte> bytes) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    const auto status =
        recordRenderTapeLinearBytes(object, subresource, layout, bytes);
    if (status != dxmt9::d3d9::RenderTapeBlockMutationStatus::Accepted) {
        if (status == dxmt9::d3d9::RenderTapeBlockMutationStatus::IncompleteSeed &&
            !renderTapeObjectAdmitted(object.identity)) {
            return;
        }
        const auto reason =
            status == dxmt9::d3d9::RenderTapeBlockMutationStatus::IncompleteSeed
                ? dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                      IncompleteSubresourceSeed
                : dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                      DescriptorMismatch;
        D9CSurfaceDesc desc{};
        const auto *entry = findRenderTapeObject(object);
        if (entry && subresource < entry->content.size()) {
            (void)renderTapeObjectSubresourceDesc(
                *entry, object, subresource, desc);
        }
        logRenderTapeMutationFailure(
            "linear", status, object, subresource, layout.fullRowBytes,
            layout.fullRows, layout.rowBytes, layout.rows, layout.pitch,
            bytes);
        RejectRenderTapeCaptureForChild(
            reason, object, subresource,
            {.format = desc.format,
             .width = desc.width,
             .height = desc.height,
             .pitch = static_cast<std::int32_t>(layout.pitch),
             .bytes = bytes.size()});
        return;
    }
    if (!IsRenderTapeCaptureActiveForChild())
        return;
    if (!renderTapeObjectAdmitted(object.identity))
        return;
    if (!appendRenderTapeUnlockMutation(object, subresource,
                                        "linear_resource_mutation")) {
        abortRenderTapeCapture("linear_resource_mutation");
    }
}

void D3D9DeviceImpl::NotifyRenderTapeSurfaceAliasForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &surface,
    const dxmt9::d3d9::pe::PeWireObjectRef &parentTexture,
    std::uint32_t subresource,
    const D9CSurfaceDesc &descriptor) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!IsRenderTapeCaptureTrackingEnabledForChild())
        return;
    const dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 alias{
        .schemaVersion =
            dxmt9::d3d9::kRenderTapeSurfaceDescriptorVersion2,
        .storage = static_cast<std::uint32_t>(
            dxmt9::d3d9::RenderTapeSurfaceStorage::TextureSubresource),
        .initialContentDisposition = static_cast<std::uint32_t>(
            dxmt9::d3d9::RenderTapeInitialContentDisposition::Unavailable),
        .subresource = subresource,
        .parentTexture = parentTexture.identity,
        .surface = descriptor,
    };
    notifyRenderTapeCreatedObject(
        surface,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(&alias), sizeof(alias)));
}

void D3D9DeviceImpl::NotifyRenderTapeStandaloneSurfaceForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &surface,
    const D9CSurfaceDesc &descriptor) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!IsRenderTapeCaptureTrackingEnabledForChild())
        return;
    const dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 standalone{
        .schemaVersion =
            dxmt9::d3d9::kRenderTapeSurfaceDescriptorVersion2,
        .storage = static_cast<std::uint32_t>(
            dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone),
        .initialContentDisposition = static_cast<std::uint32_t>(
            dxmt9::d3d9::RenderTapeInitialContentDisposition::CompleteSeed),
        .surface = descriptor,
    };
    notifyRenderTapeCreatedObject(
        surface,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(&standalone),
            sizeof(standalone)));
}

bool D3D9DeviceImpl::retireRenderTapeObject(
    const D9CWireObjectIdentity &identity, bool recordDestroy,
    const char *failureReason) noexcept {
    if (!peCaptureState_)
        return false;
    const auto it = std::find_if(
        peCaptureState_->renderTapeRegistry.objects.begin(),
        peCaptureState_->renderTapeRegistry.objects.end(), [&](const auto &entry) {
            return renderTapeSameIdentity(entry.identity, identity);
        });
    if (it == peCaptureState_->renderTapeRegistry.objects.end())
        return false;
    const bool admitted = renderTapeObjectAdmitted(identity);
    if (recordDestroy && admitted && peCaptureState_ &&
        peCaptureState_->renderTapeCapture.state() ==
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        const auto status = peCaptureState_->renderTapeCapture.objectDestroy(identity);
        dxmt9DeviceInfoLog(
            "render_tape_capture object_destroy status=%u kind=%u "
            "generation=%u object_id=%llu",
            static_cast<unsigned>(status), identity.kind,
            identity.generation,
            static_cast<unsigned long long>(identity.objectId));
        if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            abortRenderTapeCapture(failureReason);
        } else {
            removeRenderTapeObjectAdmitted(identity);
        }
    }
    try {
        peCaptureState_->renderTapeRegistry.knownDead.push_back(identity);
    } catch (...) {
        const dxmt9::d3d9::pe::PeWireObjectRef object{.identity = identity};
        markRenderTapeInvalidOnce("object_destroy_tombstone_allocation",
                                 &object);
        return false;
    }
    peCaptureState_->renderTapeRegistry.objects.erase(it);
    return true;
}

void D3D9DeviceImpl::retireRenderTapeAliasesForParent(
    const D9CWireObjectIdentity &parent, bool recordDestroy) noexcept {
    if (!peCaptureState_) {
        return;
    }
    for (auto it = peCaptureState_->renderTapeRegistry.objects.begin();
         it != peCaptureState_->renderTapeRegistry.objects.end();) {
        if (!it->lifetime.textureAlias ||
            !renderTapeSameIdentity(it->aliasParentTexture, parent)) {
            ++it;
            continue;
        }
        if (it->lifetime.wrapperRefs != 0u) {
            const dxmt9::d3d9::pe::PeWireObjectRef aliasObject{
                .identity = it->identity};
            markRenderTapeInvalidOnce("alias_parent_destroy_live_wrapper",
                                     &aliasObject);
            if (IsRenderTapeCaptureActiveForChild()) {
                abortRenderTapeCapture("alias_parent_destroy_live_wrapper");
            }
            ++it;
            continue;
        }
        const auto identity = it->identity;
        if (!it->lifetime.retireParent()) {
            ++it;
            continue;
        }
        const auto aliasPlan = dxmt9::d3d9::pe::settleRecorderCommit({
            .phase = dxmt9::d3d9::pe::RecorderCommitPhase::Draining,
            .event = dxmt9::d3d9::pe::RecorderCommitEvent::DrainAlias,
            .aliasesRemain = true,
        });
        const bool aliasAdmitted =
            aliasPlan.valid() &&
            aliasPlan.action() ==
                dxmt9::d3d9::pe::RecorderCommitAction::DestroyAlias;
        DXMT_ASSERT(aliasAdmitted);
        if (!aliasAdmitted) {
            markRenderTapeInvalidOnce("commit_alias_settlement");
        }
        const bool retired = retireRenderTapeObject(
            identity, recordDestroy, "alias_object_destroy");
        DXMT_ASSERT(retired);
        if (!retired) {
            markRenderTapeInvalidOnce("commit_alias_destroy");
        }
        // The helper erases the identity. Restarting from a value lookup
        // keeps iterator invalidation out of this bounded registry walk.
        it = peCaptureState_->renderTapeRegistry.objects.begin();
    }
}

void D3D9DeviceImpl::drainPendingRenderTapeChunk(bool recordDestroy) noexcept {
    if (!peCaptureState_)
        return;
    // Handles are intentionally walked after the command has been
    // materialized. Duplicate handles across records are harmless because
    // the bounded lifetime ref reaches zero on the first visit.
    for (const auto &handle : recorderState_.commandChunk.handles()) {
        const D9CWireObjectIdentity identity{
            .kind = handle.kind,
            .generation = handle.generation,
            .objectId = handle.objectId,
        };
        auto *entry = findRenderTapeObject(
            dxmt9::d3d9::pe::PeWireObjectRef{.identity = identity});
        if (!entry || entry->lifetime.pendingChunkRefs == 0u)
            continue;
        if (!entry->lifetime.releasePendingChunk())
            continue;
        const bool isTexture = identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE;
        const bool isAlias = entry->lifetime.textureAlias;
        if (isTexture)
            retireRenderTapeAliasesForParent(identity, recordDestroy);
        if (isAlias) {
            const auto aliasPlan = dxmt9::d3d9::pe::settleRecorderCommit({
                .phase = dxmt9::d3d9::pe::RecorderCommitPhase::Draining,
                .event = dxmt9::d3d9::pe::RecorderCommitEvent::DrainAlias,
                .aliasesRemain = true,
            });
            const bool aliasAdmitted =
                aliasPlan.valid() &&
                aliasPlan.action() ==
                    dxmt9::d3d9::pe::RecorderCommitAction::DestroyAlias;
            DXMT_ASSERT(aliasAdmitted);
            if (!aliasAdmitted) {
                markRenderTapeInvalidOnce("commit_alias_settlement");
            }
            const bool retired = retireRenderTapeObject(
                identity, recordDestroy, "alias_object_destroy");
            DXMT_ASSERT(retired);
            if (!retired) {
                markRenderTapeInvalidOnce("commit_alias_destroy");
            }
            continue;
        }
        // Preserve the established alias-before-parent event order. The
        // parent entry remains in the registry until the alias scan has
        // completed, so the scan is safe for both immediate and pending
        // retirement.
        const bool aliasesRemain = std::any_of(
            peCaptureState_->renderTapeRegistry.objects.begin(),
            peCaptureState_->renderTapeRegistry.objects.end(),
            [&](const auto &candidate) {
                return candidate.lifetime.textureAlias &&
                    renderTapeSameIdentity(candidate.aliasParentTexture,
                                           identity);
            });
        const auto parentPlan = dxmt9::d3d9::pe::settleRecorderCommit({
            .phase = dxmt9::d3d9::pe::RecorderCommitPhase::Draining,
            .event = dxmt9::d3d9::pe::RecorderCommitEvent::DrainParent,
            .aliasesRemain = aliasesRemain,
            .parentPending = true,
        });
        const bool parentAdmitted =
            parentPlan.valid() &&
            parentPlan.action() ==
                dxmt9::d3d9::pe::RecorderCommitAction::DestroyParent;
        DXMT_ASSERT(parentAdmitted == !aliasesRemain);
        if (!parentAdmitted && !aliasesRemain) {
            markRenderTapeInvalidOnce("commit_parent_settlement");
        }
        if (parentAdmitted) {
            const bool retired = retireRenderTapeObject(
                identity, recordDestroy, "object_destroy");
            DXMT_ASSERT(retired);
            if (!retired) {
                markRenderTapeInvalidOnce("commit_parent_destroy");
            }
        }
    }
}

void D3D9DeviceImpl::NotifyRenderTapeObjectDestroyForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (peCaptureState_) {
        auto *entry = findRenderTapeObject(object);
        // The PE wrapper destructor has already delivered this callback.
        // Transfer the logical lifetime to the bounded pending chunk ref;
        // drain it after command materialization and before raw D9C
        // retainer reset.
        if (entry && entry->lifetime.wrapperRefs == 1u &&
            entry->lifetime.pendingChunkRefs == 0u &&
            recorderState_.commandChunk.referencesObject(
                dxmt9::d3d9::pe::PeLocalObjectIdentity{
                    .kind = object.identity.kind, .object = object.object}) &&
            entry->lifetime.retainPendingChunk()) {
            (void)entry->lifetime.releaseWrapper();
            dxmt9DeviceInfoLog(
                "render_tape_capture object_destroy deferred kind=%u "
                "generation=%u object_id=%llu pending=%u",
                object.identity.kind, object.identity.generation,
                static_cast<unsigned long long>(object.identity.objectId),
                entry->lifetime.pendingChunkRefs);
            return;
        }
    }
    const bool retired = unregisterRenderTapeObject(object);
    if (!retired) {
        return;
    }
    if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        retireRenderTapeAliasesForParent(object.identity);
    }
    retireRenderTapeObject(object.identity, true, "object_destroy");
}

void D3D9DeviceImpl::NotifyRenderTapeResourceMutationForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    dxmt9::d3d9::RenderTapeMutationKind kind, std::uint32_t subresource,
    std::uint64_t byteOffset,
    std::span<const std::byte> bytes,
    dxmt9::d3d9::RenderTapeBufferMutationDisposition bufferDisposition)
    noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    const bool registryAccepted =
        recordRenderTapeCpuBytes(object, subresource, byteOffset, bytes);
    if (!registryAccepted) {
        if (IsRenderTapeCaptureActiveForChild())
            abortRenderTapeCapture("resource_mutation_registry");
        return;
    }
    if (!peCaptureState_ ||
        peCaptureState_->renderTapeCapture.state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        return;
    }
    if (!renderTapeObjectAdmitted(object.identity))
        return;
    try {
        if (peCaptureState_->renderTapeCapture.resourceMutationBytes(
                object.identity, kind, subresource, byteOffset, bytes,
                bufferDisposition) !=
            dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            abortRenderTapeCapture("resource_mutation");
        }
    } catch (...) {
        abortRenderTapeCapture("resource_mutation_exception");
    }
}

void D3D9DeviceImpl::NotifyRenderTapeOrderedControlForChild(
    const dxmt9::d3d9::RenderTapeOrderedControlHeader &fixed,
    std::span<const std::byte> payload) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!peCaptureState_ ||
        peCaptureState_->renderTapeCapture.state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        return;
    }
    if (fixed.identity.objectId != 0u &&
        !materializeRenderTapeObjectForReference(fixed.identity)) {
        return;
    }
    auto recorded = fixed;
    recorded.completionOrdinal = ++peCaptureState_->renderTapeCompletionOrdinal;
    if (peCaptureState_->renderTapeCapture.orderedControl(recorded, payload) !=
        dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
        abortRenderTapeCapture("ordered_control");
    }
}

void D3D9DeviceImpl::notifyRenderTapeCreatedObject(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::span<const std::byte> descriptor,
    std::span<const std::byte> immutablePayload) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!IsRenderTapeCaptureTrackingEnabledForChild())
        return;
    if (!object.valid(object.identity.kind) || descriptor.empty()) {
        AbortRenderTapeCaptureForChild();
        return;
    }
    const auto registration =
        registerRenderTapeObject(object, descriptor, immutablePayload);
    if (registration != RenderTapeObjectRegistration::New) {
        // A repeated COM wrapper for the same underlying identity is a
        // lifetime alias, not a second tape ObjectDefine. Conflicting
        // descriptors are already recorded as a registry rejection.
        return;
    }
    // A newly created frame-tape identity is materialized immediately
    // before its first command/control reference. That cold JIT point can
    // require complete seeds and emit the exact descriptor plus mutations
    // transactionally; creation alone cannot claim initialized bytes.
    // Sequence-tape deliberately rejects such post-arm identities.
}

void D3D9DeviceImpl::notifyRenderTapeCreatedBuffer(
    D9CBuffer *buffer,
    const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!IsRenderTapeCaptureTrackingEnabledForChild())
        return;
    D9CBufferDesc descriptor{};
    if (!buffer || FAILED(hr32(dxmt9c_buffer_get_desc(buffer, &descriptor)))) {
        AbortRenderTapeCaptureForChild();
        return;
    }
    notifyRenderTapeCreatedObject(
        object,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(&descriptor),
            sizeof(descriptor)));
}

void D3D9DeviceImpl::notifyRenderTapeCreatedTexture(
    D9CTexture *texture,
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    dxmt9::d3d9::RenderTapeTextureDimension dimension) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!IsRenderTapeCaptureTrackingEnabledForChild())
        return;
    if (!texture) {
        AbortRenderTapeCaptureForChild();
        return;
    }
    const std::uint32_t mipLevelCount =
        dxmt9c_texture_get_level_count(texture);
    if (mipLevelCount == 0u) {
        AbortRenderTapeCaptureForChild();
        return;
    }
    std::uint32_t subresourceCount = mipLevelCount;
    if (dimension == dxmt9::d3d9::RenderTapeTextureDimension::Cube) {
        if (mipLevelCount > std::numeric_limits<std::uint32_t>::max() / 6u) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        subresourceCount = mipLevelCount * 6u;
    }
    if (!renderTapeDescriptorSubresourceCountFits(
            subresourceCount, sizeof(RenderTapeTextureDescriptorV2))) {
        AbortRenderTapeCaptureForChild();
        return;
    }
    const RenderTapeTextureDescriptorV2 descriptor{
        .schemaVersion =
            dxmt9::d3d9::kRenderTapeTextureDescriptorVersion2,
        .dimension = static_cast<std::uint32_t>(dimension),
        .mipLevelCount = mipLevelCount,
        .subresourceCount = subresourceCount,
        .initialContentDisposition = static_cast<std::uint32_t>(
            dxmt9::d3d9::RenderTapeInitialContentDisposition::CompleteSeed),
    };
    std::vector<std::byte> descriptorBytes;
    try {
        descriptorBytes.resize(
            sizeof(descriptor) +
            static_cast<std::size_t>(subresourceCount) *
                sizeof(D9CSurfaceDesc));
    } catch (...) {
        AbortRenderTapeCaptureForChild();
        return;
    }
    std::memcpy(descriptorBytes.data(), &descriptor, sizeof(descriptor));
    for (std::uint32_t subresource = 0u;
         subresource < subresourceCount; ++subresource) {
        D9CSurfaceDesc subresourceDesc{};
        const auto mipLevel =
            dxmt9::d3d9::renderTapeTextureDescriptorMipLevel(
                dimension, mipLevelCount, subresource);
        if (FAILED(hr32(dxmt9c_texture_get_level_desc(
                texture, mipLevel, &subresourceDesc)))) {
            AbortRenderTapeCaptureForChild();
            return;
        }
        std::memcpy(descriptorBytes.data() + sizeof(descriptor) +
                        static_cast<std::size_t>(subresource) *
                            sizeof(D9CSurfaceDesc),
                    &subresourceDesc, sizeof(subresourceDesc));
    }
    notifyRenderTapeCreatedObject(
        object,
        descriptorBytes);
}

void D3D9DeviceImpl::notifyRenderTapeCreatedVertexDecl(
    D9CVertexDecl *decl,
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::span<const std::byte> elements, std::size_t elementCount) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!IsRenderTapeCaptureTrackingEnabledForChild())
        return;
    if (!decl || elements.empty() ||
        elementCount > std::numeric_limits<uint32_t>::max() ||
        elements.size() > std::numeric_limits<uint32_t>::max()) {
        AbortRenderTapeCaptureForChild();
        return;
    }
    const RenderTapeVertexDeclDescriptor descriptor{
        static_cast<uint32_t>(elementCount),
        static_cast<uint32_t>(elements.size())};
    notifyRenderTapeCreatedObject(
        object,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(&descriptor),
            sizeof(descriptor)),
        elements);
}

void D3D9DeviceImpl::notifyRenderTapeCreatedQuery(
    D9CQuery *query,
    const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!IsRenderTapeCaptureTrackingEnabledForChild())
        return;
    if (!query) {
        AbortRenderTapeCaptureForChild();
        return;
    }
    const RenderTapeQueryDescriptor descriptor{
        dxmt9c_query_get_type(query), dxmt9c_query_get_data_size(query)};
    if (descriptor.dataBytes == 0u) {
        AbortRenderTapeCaptureForChild();
        return;
    }
    notifyRenderTapeCreatedObject(
        object,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(&descriptor),
            sizeof(descriptor)));
}

void D3D9DeviceImpl::notifyRenderTapeCreatedShader(
    D9CShader *shader,
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    uint32_t stage) noexcept {
    assertRecorderThreadConfined();
    PeRecorderGuard recorderLock(recorderState_.recorderMutex, recorderState_.recorderLockRequired);
    if (!IsRenderTapeCaptureTrackingEnabledForChild())
        return;
    if (!shader || !object.valid(object.identity.kind)) {
        AbortRenderTapeCaptureForChild();
        return;
    }
    uint32_t bytecodeBytes = 0u;
    if (FAILED(hr32(dxmt9c_shader_get_bytecode(shader, nullptr,
                                               &bytecodeBytes))) ||
        bytecodeBytes == 0u || bytecodeBytes % sizeof(uint32_t) != 0u) {
        AbortRenderTapeCaptureForChild();
        return;
    }
    std::vector<std::byte> bytecode;
    try {
        bytecode.resize(bytecodeBytes);
    } catch (...) {
        AbortRenderTapeCaptureForChild();
        return;
    }
    uint32_t availableBytes = bytecodeBytes;
    if (FAILED(hr32(dxmt9c_shader_get_bytecode(
            shader, bytecode.data(), &availableBytes))) ||
        availableBytes != bytecodeBytes) {
        AbortRenderTapeCaptureForChild();
        return;
    }
    const RenderTapeShaderDescriptor descriptor{stage, bytecodeBytes};
    notifyRenderTapeCreatedObject(
        object,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(&descriptor),
            sizeof(descriptor)),
        bytecode);
}
