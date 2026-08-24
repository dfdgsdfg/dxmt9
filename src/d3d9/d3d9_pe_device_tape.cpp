/* src/d3d9/d3d9_pe_device_tape.cpp — Render Tape capture/session ownership.
 *
 * This translation unit owns the Present-boundary arm/finish lifecycle,
 * bootstrap production and capture-session state transitions. Registry,
 * identity/mutation, and child lifetime/pending-drain methods live in the
 * adjacent semantic units; all remain one D3D9DeviceImpl implementation. */

#include "d3d9_pe_device_impl.hpp"
#include "d3d9_pe_tape_support.hpp"

void D3D9DeviceImpl::abortRenderTapeCapture(const char *reason) noexcept {
    if (!peCaptureState_ || !peCaptureState_->renderTapeCapture.enabled()) {
        return;
    }
    if (!peCaptureState_->renderTapeAbortReason) {
        peCaptureState_->renderTapeAbortReason = reason;
        dxmt9DeviceInfoLog("render_tape_capture first_abort reason=%s",
                           reason);
    }
    peCaptureState_->renderTapeCapture.abort();
    peCaptureState_->renderTapeArmBoundaryPhase =
        dxmt9::d3d9::RenderTapeArmBoundaryPhase::Disabled;
    peCaptureState_->renderTapeArmSnapshots.clear();
    peCaptureState_->renderTapeExpectedDigest.reset();
    peCaptureState_->renderTapeExpectedPixels.clear();
    peCaptureState_->renderTapeExpectedSourcePixels.clear();
    peCaptureState_->renderTapeOutputDesc.reset();
    peCaptureState_->renderTapeActiveCaptureToken = 0u;
}

bool D3D9DeviceImpl::produceRenderTapeBootstrap(
    dxmt9::d3d9::RenderTapeCaptureBootstrapSeed &seed) noexcept {
    dxmt9DeviceInfoLog("render_tape_capture bootstrap_begin");
    if (!peCaptureState_) {
        dxmt9DeviceInfoLog("render_tape_capture producer aborted reason=registry_missing");
        return false;
    }
    if (peCaptureState_->renderTapeRegistry.invalid) {
        dxmt9DeviceInfoLog(
            "render_tape_capture producer aborted reason=registry_invalid "
            "detail=%s kind=%u generation=%u object_id=%llu subresource=%u "
            "format=%u width=%u height=%u pitch=%d bytes=%llu objects=%zu",
            peCaptureState_->renderTapeRegistry.invalidReason
                ? peCaptureState_->renderTapeRegistry.invalidReason
                : "unknown",
            peCaptureState_->renderTapeRegistry.invalidKind,
            peCaptureState_->renderTapeRegistry.invalidGeneration,
            static_cast<unsigned long long>(peCaptureState_->renderTapeRegistry.invalidObjectId),
            peCaptureState_->renderTapeRegistry.invalidSubresource,
            peCaptureState_->renderTapeRegistry.invalidLayout.format,
            peCaptureState_->renderTapeRegistry.invalidLayout.width,
            peCaptureState_->renderTapeRegistry.invalidLayout.height,
            peCaptureState_->renderTapeRegistry.invalidLayout.pitch,
            static_cast<unsigned long long>(
                peCaptureState_->renderTapeRegistry.invalidLayout.bytes),
            peCaptureState_->renderTapeRegistry.objects.size());
        return false;
    }
    if (!admitRenderTapePresentOutput() || peCaptureState_->renderTapeRegistry.invalid) {
        return false;
    }
    const auto findArmSnapshot = [&](const auto &identity) {
        return std::find_if(
            peCaptureState_->renderTapeArmSnapshots.begin(), peCaptureState_->renderTapeArmSnapshots.end(),
            [&](const auto &snapshot) {
                return renderTapeSameIdentity(snapshot.identity, identity);
            });
    };
    try {
        dxmt9::d3d9::pe::CommandChunkBuilder builder({
            .records = 1u,
            .handles = 256u,
            .payloadBytes = 1024u * 1024u,
            .sealedBytes = 1024u * 1024u,
        });
        // The bootstrap is a checkpoint of the live PE shadow, not of the
        // last draw packet. Rebuild every binding slot immediately before
        // producing it so streams, null unbinds, and the current index
        // binding are authoritative even when no draw made them pending.
        populateBindingView(recorderState_.peBindingView, true, true);
        const bool snapshotBuilt = dxmt9::d3d9::pe::buildFullSnapshotState(
            recorderState_.peState, recorderState_.peConsts, recorderState_.peBindingView, recorderState_.peSparseScratch,
            recorderState_.peSparseHeader, recorderState_.peSparseState);
        const bool snapshotAppended =
            snapshotBuilt && dxmt9::d3d9::pe::appendApplyState(
                                 builder, recorderState_.peSparseHeader.flags,
                                 recorderState_.peSparseState);
        if (!snapshotBuilt || !snapshotAppended) {
            dxmt9DeviceInfoLog(
                "render_tape_capture producer aborted reason=bootstrap_state "
                "snapshot_built=%d snapshot_appended=%d",
                snapshotBuilt ? 1 : 0, snapshotAppended ? 1 : 0);
            return false;
        }
        const auto overlay = builder.seal();
        if (!overlay.valid()) {
            dxmt9DeviceInfoLog(
                "render_tape_capture producer aborted reason=overlay_invalid");
            return false;
        }

        // Keep the bootstrap closure tied to the exact generation-qualified
        // handles emitted by the sealed overlay. The canonical validator is
        // the authority for wire bounds, record/section shape, and handle
        // references; this side table only answers whether a live object's
        // missing seed is actually reachable from the checkpoint.
        dxmt9::d3d9::ImportedChunkView bootstrapChunk{};
        dxmt9::d3d9::CommandChunkValidationScratch bootstrapScratch{};
        const auto bootstrapValidation =
            dxmt9::d3d9::validateCommandChunk(
                overlay.blob,
                dxmt9::d3d9::CommandChunkEnvelope{
                    .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
                    .recordCount = overlay.recordCount,
                    .handleCount = overlay.handleCount,
                },
                &bootstrapChunk, bootstrapScratch);
        if (!bootstrapValidation.valid()) {
            dxmt9DeviceInfoLog(
                "render_tape_capture producer aborted reason=overlay_validation "
                "status=%u record=%u section=%u handle=%u offset=%u",
                static_cast<unsigned>(bootstrapValidation.status),
                bootstrapValidation.failedRecordIndex,
                bootstrapValidation.failedSectionIndex,
                bootstrapValidation.failedHandleIndex,
                bootstrapValidation.byteOffset);
            return false;
        }
        std::vector<D9CWireObjectIdentity> bootstrapHandles;
        bootstrapHandles.reserve(bootstrapChunk.handles.size());
        for (const auto& handle : bootstrapChunk.handles) {
            const D9CWireObjectIdentity identity{
                .kind = handle.kind,
                .generation = handle.generation,
                .objectId = handle.objectId,
            };
            const auto alreadyPresent = std::find_if(
                bootstrapHandles.begin(), bootstrapHandles.end(),
                [&](const auto& candidate) {
                    return renderTapeSameIdentity(candidate, identity);
                });
            if (alreadyPresent == bootstrapHandles.end()) {
                bootstrapHandles.push_back(identity);
            }
        }
        seed.bootstrapOverlay.assign(overlay.blob.begin(), overlay.blob.end());
        dxmt9DeviceInfoLog(
            "render_tape_capture bootstrap_overlay_complete "
            "records=%u handles=%u bytes=%zu",
            overlay.recordCount, overlay.handleCount, overlay.blob.size());

        std::vector<const RenderTapeLiveObject *> objects;
        objects.reserve(peCaptureState_->renderTapeRegistry.objects.size());
        for (const auto &object : peCaptureState_->renderTapeRegistry.objects) {
            objects.push_back(&object);
        }
        std::sort(objects.begin(), objects.end(), [](const auto *a, const auto *b) {
            return std::tie(a->identity.kind, a->identity.generation,
                            a->identity.objectId) <
                   std::tie(b->identity.kind, b->identity.generation,
                            b->identity.objectId);
        });
        std::vector<dxmt9::d3d9::RenderTapeBootstrapClosureObject>
            closureObjects;
        closureObjects.reserve(objects.size());
        D9CWireObjectIdentity presentOutput{};
        std::size_t presentOutputCount = 0u;
        bool staleArmSnapshot = false;
        for (const auto *object : objects) {
            if (object->role == RenderTapeLiveObject::Role::PresentOutput) {
                presentOutput = object->identity;
                ++presentOutputCount;
            }
            const auto armSnapshot = findArmSnapshot(object->identity);
            const auto armOverlay = dxmt9::d3d9::
                renderTapeSelectArmObjectSnapshotOverlay(
                    object->descriptor, object->content,
                    armSnapshot != peCaptureState_->renderTapeArmSnapshots.end()
                        ? std::span<const std::byte>(
                              armSnapshot->descriptor)
                        : std::span<const std::byte>{},
                    armSnapshot != peCaptureState_->renderTapeArmSnapshots.end()
                        ? std::span<const std::vector<std::byte>>(
                              armSnapshot->content)
                        : std::span<const std::vector<std::byte>>{},
                    armSnapshot != peCaptureState_->renderTapeArmSnapshots.end()
                        ? armSnapshot->armOrdinal
                        : 0u,
                    peCaptureState_->renderTapeArmSnapshotOrdinal,
                    object->role == RenderTapeLiveObject::Role::PresentOutput
                        ? dxmt9::d3d9::
                              RenderTapeArmObjectSnapshotOverlayPolicy::
                                  PresentOutput
                        : dxmt9::d3d9::
                              RenderTapeArmObjectSnapshotOverlayPolicy::
                                  Ordinary);
            staleArmSnapshot |= armOverlay.source == dxmt9::d3d9::
                RenderTapeArmSnapshotOverlaySource::StaleArm;
            const auto overlayPolicy =
                object->role == RenderTapeLiveObject::Role::PresentOutput
                    ? dxmt9::d3d9::
                          RenderTapeArmObjectSnapshotOverlayPolicy::
                              PresentOutput
                    : dxmt9::d3d9::
                          RenderTapeArmObjectSnapshotOverlayPolicy::Ordinary;
            const bool complete = dxmt9::d3d9::
                renderTapeArmObjectSnapshotContentComplete(
                    object->contentCount, object->lifetime.textureAlias,
                    overlayPolicy, armOverlay.source, armOverlay.content);
            bool producedByCapturedPassCandidate = false;
            if (!complete &&
                object->identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
                RenderTapeTextureDescriptorV2 texture{};
                if (renderTapeLoadTextureDescriptorV2(object->descriptor,
                                                      texture) &&
                    texture.dimension == static_cast<std::uint32_t>(
                        RenderTapeTextureDimension::Texture2D) &&
                    texture.mipLevelCount == 1u &&
                    texture.subresourceCount == 1u) {
                    D9CSurfaceDesc desc{};
                    producedByCapturedPassCandidate =
                        renderTapeTextureSubresourceDescriptor(
                            object->descriptor, 0u, desc) &&
                        (desc.usage & 1u) != 0u;
                }
            } else if (!complete &&
                       object->identity.kind ==
                           D9C_CHUNK_HANDLE_KIND_SURFACE) {
                dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
                producedByCapturedPassCandidate =
                    renderTapeLoadSurfaceDescriptorV2(object->descriptor,
                                                      surface) &&
                    surface.storage == static_cast<std::uint32_t>(
                        dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone) &&
                    dxmt9::d3d9::renderTapeProducedStandaloneSurfaceSupported(
                        surface.surface);
            }
            closureObjects.push_back({
                .identity = object->identity,
                .complete = complete,
                .producedByCapturedPassCandidate =
                    producedByCapturedPassCandidate,
                .hasDescriptorDependency = object->lifetime.textureAlias,
                .descriptorDependency = object->aliasParentTexture,
            });
        }
        if (staleArmSnapshot) {
            dxmt9DeviceInfoLog(
                "render_tape_capture producer aborted reason=stale_arm_snapshot");
            return false;
        }
        if (presentOutputCount != 1u) {
            dxmt9DeviceInfoLog(
                "render_tape_capture producer aborted reason=present_output_count "
                "count=%zu",
                presentOutputCount);
            return false;
        }
        if (dxmt9::d3d9::renderTapeBootstrapRequiresAllLiveObjects(
                dxmt9PeRenderTapeCaptureProfile())) {
            // Sequence tapes cannot define a pre-arm object after their
            // first PresentComplete. Preserve the complete arm snapshot
            // for that profile; exact JIT closure is a frame-tape policy.
            for (const auto *object : objects)
                bootstrapHandles.push_back(object->identity);
        }
        std::vector<D9CWireObjectIdentity> closure;
        const auto closureResult =
            dxmt9::d3d9::renderTapeBuildBootstrapClosureAttributed(
                bootstrapHandles, presentOutput,
                closureObjects, closure);
        const auto closureStatus = closureResult.status;
        if (closureStatus !=
            dxmt9::d3d9::RenderTapeBootstrapClosureStatus::Accepted) {
            const char *reason =
                closureStatus == dxmt9::d3d9::RenderTapeBootstrapClosureStatus::ReferencedObjectIncomplete
                    ? "bootstrap_referenced_incomplete_seed"
                    : closureStatus == dxmt9::d3d9::RenderTapeBootstrapClosureStatus::DescriptorDependencyIncomplete
                        ? "bootstrap_descriptor_dependency_incomplete_seed"
                        : closureStatus == dxmt9::d3d9::RenderTapeBootstrapClosureStatus::DescriptorDependencyMissing
                            ? "bootstrap_descriptor_dependency_missing"
                            : closureStatus == dxmt9::d3d9::RenderTapeBootstrapClosureStatus::DuplicateObjectIdentity
                                ? "bootstrap_duplicate_object_identity"
                                : closureStatus == dxmt9::d3d9::RenderTapeBootstrapClosureStatus::InvalidDescriptorDependency
                                    ? "bootstrap_invalid_descriptor_dependency"
                                    : "bootstrap_referenced_object_missing";
            const auto &offending = closureResult.offendingIdentity;
            const auto &dependency = closureResult.dependencyIdentity;
            const auto offendingObject = std::find_if(
                objects.begin(), objects.end(), [&](const auto *candidate) {
                    return closureResult.hasOffendingIdentity &&
                        renderTapeSameIdentity(candidate->identity, offending);
                });
            const auto missingSubresource =
                offendingObject != objects.end()
                ? static_cast<std::uint32_t>(std::find_if(
                      (*offendingObject)->content.begin(),
                      (*offendingObject)->content.end(),
                      [](const auto &bytes) { return bytes.empty(); }) -
                  (*offendingObject)->content.begin())
                : 0u;
            const auto missing = offendingObject != objects.end()
                ? dxmt9::d3d9::renderTapeDescribeMissingSeed(
                      offending, (*offendingObject)->descriptor,
                      missingSubresource,
                      {.handleIndex =
                           std::numeric_limits<std::uint32_t>::max(),
                       .recordIndex = 0u,
                       .recordType = D9C_COMMAND_RECORD_APPLY_STATE})
                : dxmt9::d3d9::RenderTapeMissingSeedDescriptor{};
            dxmt9DeviceInfoLog(
                "render_tape_capture producer aborted reason=%s "
                "closure_status=%u offending_present=%d "
                "offending_kind=%u offending_generation=%u "
                "offending_object_id=%llu dependency_present=%d "
                "dependency_kind=%u dependency_generation=%u "
                "dependency_object_id=%llu bootstrap_handles=%zu "
                "live_objects=%zu descriptor_status=%s expected_status=%s "
                "missing_subresource=%u format=%u width=%u height=%u depth=%u "
                "multisample_type=%u usage=%u resource_type=%u pool=%u "
                "expected_tight_bytes=%llu expected_tight_bytes_valid=%d",
                reason, static_cast<unsigned>(closureStatus),
                closureResult.hasOffendingIdentity ? 1 : 0,
                offending.kind, offending.generation,
                static_cast<unsigned long long>(offending.objectId),
                closureResult.hasDependencyIdentity ? 1 : 0,
                dependency.kind, dependency.generation,
                static_cast<unsigned long long>(dependency.objectId),
                bootstrapHandles.size(), objects.size(),
                dxmt9::d3d9::renderTapeMissingSeedDescriptorStatusName(
                    missing.descriptorStatus),
                dxmt9::d3d9::renderTapeExpectedContentStatusName(
                    missing.expectedContentStatus),
                missing.missingSubresource, missing.missingSurface.format,
                missing.missingSurface.width, missing.missingSurface.height,
                missing.missingSurface.depth,
                missing.missingSurface.multiSampleType,
                missing.missingSurface.usage,
                missing.missingSurface.resourceType,
                missing.missingSurface.pool,
                static_cast<unsigned long long>(missing.expectedTightBytes),
                missing.expectedTightBytesValid ? 1 : 0);
            return false;
        }
        dxmt9DeviceInfoLog(
            "render_tape_capture bootstrap_closure_complete "
            "handles=%zu live_objects=%zu closure=%zu",
            bootstrapHandles.size(), objects.size(), closure.size());
        for (const auto *object : objects) {
            if (!dxmt9::d3d9::renderTapeBootstrapClosureContains(
                    closure, object->identity)) {
                continue;
            }
            const auto closureObject = std::find_if(
                closureObjects.begin(), closureObjects.end(),
                [&](const auto &candidate) {
                    return renderTapeSameIdentity(candidate.identity,
                                                  object->identity);
                });
            const auto dependencyObject =
                closureObject != closureObjects.end() &&
                        closureObject->hasDescriptorDependency
                    ? std::find_if(
                          closureObjects.begin(), closureObjects.end(),
                          [&](const auto &candidate) {
                            return renderTapeSameIdentity(
                                candidate.identity,
                                closureObject->descriptorDependency);
                          })
                    : closureObjects.end();
            if (closureObject != closureObjects.end() &&
                ((!closureObject->complete &&
                  closureObject->producedByCapturedPassCandidate) ||
                 (dependencyObject != closureObjects.end() &&
                  !dependencyObject->complete &&
                  dependencyObject->producedByCapturedPassCandidate))) {
                // The generation-qualified storage is defined only after
                // the current command chunk proves its first terminal
                // access is the matching unrestricted attachment Clear.
                continue;
            }
            dxmt9::d3d9::RenderTapeCaptureObjectSeed objectSeed{};
            objectSeed.identity = object->identity;
            objectSeed.descriptorKind = static_cast<std::uint32_t>(
                dxmt9::d3d9::renderTapeDescriptorKindForObject(
                    object->identity.kind));
            const auto armSnapshot = findArmSnapshot(object->identity);
            const auto armOverlay = dxmt9::d3d9::
                renderTapeSelectArmObjectSnapshotOverlay(
                    object->descriptor, object->content,
                    armSnapshot != peCaptureState_->renderTapeArmSnapshots.end()
                        ? std::span<const std::byte>(
                              armSnapshot->descriptor)
                        : std::span<const std::byte>{},
                    armSnapshot != peCaptureState_->renderTapeArmSnapshots.end()
                        ? std::span<const std::vector<std::byte>>(
                              armSnapshot->content)
                        : std::span<const std::vector<std::byte>>{},
                    armSnapshot != peCaptureState_->renderTapeArmSnapshots.end()
                        ? armSnapshot->armOrdinal
                        : 0u,
                    peCaptureState_->renderTapeArmSnapshotOrdinal,
                    object->role == RenderTapeLiveObject::Role::PresentOutput
                        ? dxmt9::d3d9::
                              RenderTapeArmObjectSnapshotOverlayPolicy::
                                  PresentOutput
                        : dxmt9::d3d9::
                              RenderTapeArmObjectSnapshotOverlayPolicy::
                                  Ordinary);
            if (armOverlay.source == dxmt9::d3d9::
                    RenderTapeArmSnapshotOverlaySource::StaleArm) {
                return false;
            }
            const auto effectiveDescriptor = armOverlay.descriptor;
            const auto effectiveContent = armOverlay.content;
            objectSeed.descriptor.assign(effectiveDescriptor.begin(),
                                         effectiveDescriptor.end());
            dxmt9::d3d9::RenderTapeExpectedContentContract contentContract{};
            if (!renderTapeValidateExpectedContent(
                    object->identity, effectiveDescriptor,
                    effectiveContent,
                    contentContract)) {
                dxmt9DeviceInfoLog(
                    "render_tape_capture producer aborted reason=expected_content_contract "
                    "status=%s kind=%u generation=%u object_id=%llu expected_bytes=%llu "
                    "expected_count=%u actual_count=%zu",
                    dxmt9::d3d9::renderTapeExpectedContentStatusName(
                        contentContract.status),
                    object->identity.kind, object->identity.generation,
                    static_cast<unsigned long long>(object->identity.objectId),
                    static_cast<unsigned long long>(contentContract.bytes),
                    contentContract.count, effectiveContent.size());
                RejectRenderTapeCaptureForChild(
                    dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                        ExpectedContentContract,
                    dxmt9::d3d9::pe::PeWireObjectRef{
                        .identity = object->identity},
                    std::numeric_limits<std::uint32_t>::max(), {});
                return false;
            }
            objectSeed.expectedContentBytes = contentContract.bytes;
            objectSeed.expectedContentCount = contentContract.count;
            if (!object->immutablePayload.empty()) {
                objectSeed.immutableBytes = object->immutablePayload.size();
                objectSeed.immutableDigest =
                    dxmt9::d3d9::RenderTapeCaptureSession::sha256(
                        object->immutablePayload);
                seed.blobs.push_back({.bytes = object->immutablePayload});
            }
            if (!effectiveContent.empty()) {
                for (std::uint32_t subresource = 0u;
                     subresource < effectiveContent.size(); ++subresource) {
                    const auto &bytes = effectiveContent[subresource];
                    if (bytes.empty()) {
                        if (object->identity.kind ==
                            D9C_CHUNK_HANDLE_KIND_TEXTURE) {
                            RenderTapeTextureDescriptorV2 texture{};
                            std::uint32_t textureVersion = 0u;
                            std::uint32_t levels = 0u;
                            std::uint32_t count = 0u;
                            if (object->descriptor.size() >=
                                sizeof(texture)) {
                                std::memcpy(&texture, object->descriptor.data(),
                                            sizeof(texture));
                                if (texture.schemaVersion ==
                                    dxmt9::d3d9::
                                        kRenderTapeTextureDescriptorVersion2) {
                                    textureVersion = texture.schemaVersion;
                                    levels = texture.mipLevelCount;
                                    count = texture.subresourceCount;
                                }
                            }
                            D9CSurfaceDesc desc{};
                            const bool hasDesc =
                                renderTapeTextureSubresourceDescriptor(
                                    object->descriptor, subresource, desc);
                            dxmt9DeviceInfoLog(
                                "render_tape_capture missing_seed identity_kind=%u "
                                "generation=%u object_id=%llu subresource=%u "
                                "bootstrap_referenced=%d texture_version=%u "
                                "levels=%u count=%u desc_valid=%d format=%u "
                                "width=%u height=%u depth=%u usage=%u pool=%u "
                                "resource_type=%u",
                                object->identity.kind,
                                object->identity.generation,
                                static_cast<unsigned long long>(
                                    object->identity.objectId),
                                subresource, 1,
                                textureVersion, levels, count, hasDesc ? 1 : 0,
                                desc.format, desc.width, desc.height, desc.depth,
                                desc.usage, desc.pool, desc.resourceType);
                        } else if (object->identity.kind ==
                                   D9C_CHUNK_HANDLE_KIND_SURFACE) {
                            D9CSurfaceDesc desc{};
                            dxmt9::d3d9::RenderTapeSurfaceDescriptorV2
                                surface{};
                            const bool hasDesc = dxmt9::d3d9::
                                renderTapeLoadSurfaceDescriptorV2(
                                    object->descriptor, surface);
                            if (hasDesc)
                                desc = surface.surface;
                            dxmt9DeviceInfoLog(
                                "render_tape_capture missing_seed identity_kind=%u "
                                "generation=%u object_id=%llu subresource=%u "
                                "bootstrap_referenced=%d surface_desc_valid=%d "
                                "format=%u width=%u height=%u depth=%u usage=%u "
                                "pool=%u resource_type=%u",
                                object->identity.kind,
                                object->identity.generation,
                                static_cast<unsigned long long>(
                                    object->identity.objectId),
                                subresource, 1,
                                hasDesc ? 1 : 0, desc.format, desc.width,
                                desc.height, desc.depth, desc.usage, desc.pool,
                                desc.resourceType);
                        } else if (object->identity.kind ==
                                   D9C_CHUNK_HANDLE_KIND_BUFFER) {
                            D9CBufferDesc desc{};
                            const bool hasDesc =
                                object->descriptor.size() == sizeof(desc);
                            if (hasDesc) {
                                std::memcpy(&desc, object->descriptor.data(),
                                            sizeof(desc));
                            }
                            dxmt9DeviceInfoLog(
                                "render_tape_capture missing_seed identity_kind=%u "
                                "generation=%u object_id=%llu subresource=%u "
                                "bootstrap_referenced=%d buffer_desc_valid=%d "
                                "size=%u usage=%u pool=%u format=%u fvf=%u",
                                object->identity.kind,
                                object->identity.generation,
                                static_cast<unsigned long long>(
                                    object->identity.objectId),
                                subresource, 1,
                                hasDesc ? 1 : 0, desc.size, desc.usage,
                                desc.pool, desc.format, desc.fvf);
                        }
                        D9CSurfaceDesc rejectionDesc{};
                        const dxmt9::d3d9::pe::PeWireObjectRef reference{
                            .identity = object->identity,
                        };
                        (void)renderTapeObjectSubresourceDesc(
                            *object, reference, subresource, rejectionDesc);
                        RejectRenderTapeCaptureForChild(
                            dxmt9::d3d9::
                                RenderTapeCaptureRejectionReason::
                                    IncompleteSubresourceSeed,
                            reference, subresource,
                            {.format = rejectionDesc.format,
                             .width = rejectionDesc.width,
                             .height = rejectionDesc.height,
                             .pitch = 0,
                             .bytes = 0u});
                        return false;
                    }
                    const auto digest =
                        dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes);
                    seed.blobs.push_back({.bytes = bytes});
                    seed.mutations.push_back({
                        .identity = object->identity,
                        .kind = dxmt9::d3d9::RenderTapeMutationKind::Upload,
                        .subresource = subresource,
                        .byteOffset = 0u,
                        .byteSize = bytes.size(),
                        .digest = digest,
                    });
                }
            }
            seed.objects.push_back(std::move(objectSeed));
            if (object->role == RenderTapeLiveObject::Role::PresentOutput) {
                seed.oracleAttachments.push_back({
                    .identity = object->identity,
                    .descriptorKind = static_cast<std::uint32_t>(
                        dxmt9::d3d9::RenderTapeDescriptorKind::Surface),
                });
            }
        }
        if (seed.oracleAttachments.size() != 1u) {
            dxmt9DeviceInfoLog(
                "render_tape_capture producer aborted reason=present_output_oracle_count "
                "count=%zu",
                seed.oracleAttachments.size());
            return false;
        }
        dxmt9DeviceInfoLog(
            "render_tape_capture bootstrap_complete objects=%zu "
            "mutations=%zu blobs=%zu oracle_attachments=%zu",
            seed.objects.size(), seed.mutations.size(), seed.blobs.size(),
            seed.oracleAttachments.size());
        return true;
    } catch (...) {
        dxmt9DeviceInfoLog(
            "render_tape_capture producer aborted reason=exception");
        return false;
    }
}

bool D3D9DeviceImpl::advanceRenderTapeArmBoundary(
    dxmt9::d3d9::RenderTapeArmBoundaryPhase requested) noexcept {
    if (!peCaptureState_)
        return false;
    const auto transition = dxmt9::d3d9::renderTapeAdvanceArmBoundary(
        peCaptureState_->renderTapeArmBoundaryPhase, requested);
    if (!transition.accepted) {
        dxmt9DeviceInfoLog(
            "render_tape_capture arm_boundary rejected current=%u requested=%u",
            static_cast<unsigned>(peCaptureState_->renderTapeArmBoundaryPhase),
            static_cast<unsigned>(requested));
        return false;
    }
    peCaptureState_->renderTapeArmBoundaryPhase = transition.next;
    return true;
}

bool D3D9DeviceImpl::snapshotRenderTapeResourcesAtArmBoundary() noexcept {
    if (!peCaptureState_ || peCaptureState_->renderTapeRegistry.invalid)
        return false;
    try {
        peCaptureState_->renderTapeArmSnapshots.clear();
        const auto epoch = dxmt9::d3d9::renderTapeNextArmSnapshotEpoch(
            peCaptureState_->renderTapeArmSnapshotOrdinal);
        if (!epoch.valid) {
            return false;
        }
        peCaptureState_->renderTapeArmSnapshotOrdinal = epoch.ordinal;
        for (std::size_t index = 0u;
             index < peCaptureState_->renderTapeRegistry.objects.size(); ++index) {
            const auto &object = peCaptureState_->renderTapeRegistry.objects[index];
            if (object.lifetime.textureAlias) {
                continue;
            }
            dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
            const bool standaloneD24 =
                object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
                object.contentCount == 1u && object.content.size() == 1u &&
                dxmt9::d3d9::renderTapeLoadSurfaceDescriptorV2(
                    object.descriptor, surface) &&
                surface.storage == static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone) &&
                dxmt9::d3d9::renderTapeSnapshotStandaloneD24X8Supported(
                    surface.surface);
            const bool colorTexture =
                object.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE &&
                dxmt9::d3d9::renderTapeArmColorSnapshotTextureSupported(
                    object.descriptor);
            const bool standaloneColor =
                object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
                object.contentCount == 1u && object.content.size() == 1u &&
                surface.storage == static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone) &&
                dxmt9::d3d9::
                    renderTapeArmColorSnapshotStandaloneSurfaceSupported(
                        surface.surface);
            const bool presentOutputColor =
                object.role == RenderTapeLiveObject::Role::PresentOutput &&
                object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
                object.contentCount == 0u &&
                dxmt9::d3d9::renderTapeLoadSurfaceDescriptorV2(
                    object.descriptor, surface) &&
                surface.storage == static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeSurfaceStorage::SwapchainBackbuffer) &&
                dxmt9::d3d9::renderTapeArmColorSnapshotSwapchainSurfaceSupported(
                    surface.surface);
            if (!standaloneD24 && !standaloneColor && !colorTexture &&
                !presentOutputColor)
                continue;

            RenderTapeArmObjectSnapshot snapshot{
                .objectIndex = index,
                .armOrdinal = epoch.ordinal,
                .identity = object.identity,
                .descriptor = object.descriptor,
                .content = std::vector<std::vector<std::byte>>(
                    standaloneD24 || standaloneColor || presentOutputColor
                        ? 1u
                        : object.contentCount),
            };
            if (standaloneD24) {
                if (surface.surface.width >
                        std::numeric_limits<std::uint32_t>::max() / 4u ||
                    surface.surface.height >
                        std::numeric_limits<std::uint64_t>::max() /
                            (surface.surface.width * 4u)) {
                    return false;
                }
                const std::uint64_t byteCount =
                    static_cast<std::uint64_t>(surface.surface.width) *
                    surface.surface.height * 4u;
                if (byteCount == 0u ||
                    byteCount > std::numeric_limits<std::size_t>::max()) {
                    return false;
                }
                snapshot.content[0].resize(
                    static_cast<std::size_t>(byteCount));
                const D9CRenderTapeD24X8SnapshotRequest request{
                    .identity = object.identity,
                    .surface = surface.surface,
                    .encodingVersion =
                        D9C_RENDER_TAPE_D24X8_ENCODING_FLOAT32_LE_V1,
                    .reserved0 = 0u,
                };
                D9CRenderTapeD24X8SnapshotResult result{};
                const HRESULT hr = hr32(
                    dxmt9c_device_capture_render_tape_d24x8_snapshot(
                        dev_, &request, &result,
                        snapshot.content[0].data(),
                        snapshot.content[0].size()));
                const std::uint32_t expectedPitch =
                    surface.surface.width * 4u;
                if (FAILED(hr) || result.status !=
                        D9C_RENDER_TAPE_D24X8_SNAPSHOT_COMPLETE ||
                    result.encodingVersion !=
                        D9C_RENDER_TAPE_D24X8_ENCODING_FLOAT32_LE_V1 ||
                    result.width != surface.surface.width ||
                    result.height != surface.surface.height ||
                    result.pitch != expectedPitch ||
                    result.byteCount != snapshot.content[0].size() ||
                    result.physicalFormat == 0u) {
                    dxmt9DeviceInfoLog(
                        "render_tape_capture d24x8_snapshot rejected "
                        "reason=provider_result hr=0x%08x status=%u "
                        "generation=%u object_id=%llu",
                        static_cast<unsigned>(hr), result.status,
                        object.identity.generation,
                        static_cast<unsigned long long>(
                            object.identity.objectId));
                    return false;
                }
                surface.initialContentDisposition =
                    static_cast<std::uint32_t>(dxmt9::d3d9::
                        RenderTapeInitialContentDisposition::
                            CompleteDepthFloat32V1);
                snapshot.descriptor.assign(
                    std::as_bytes(std::span(&surface, 1u)).begin(),
                    std::as_bytes(std::span(&surface, 1u)).end());
                dxmt9DeviceInfoLog(
                    "render_tape_capture d24x8_snapshot complete kind=%u "
                    "generation=%u object_id=%llu encoding=1 width=%u "
                    "height=%u pitch=%u bytes=%zu physical_format=%u",
                    snapshot.identity.kind, snapshot.identity.generation,
                    static_cast<unsigned long long>(snapshot.identity.objectId),
                    surface.surface.width, surface.surface.height,
                    result.pitch, snapshot.content[0].size(),
                    result.physicalFormat);
            } else {
                RenderTapeTextureDescriptorV2 texture{};
                std::uint32_t subresourceCount = 1u;
                if (standaloneColor || presentOutputColor) {
                    surface.initialContentDisposition =
                        static_cast<std::uint32_t>(dxmt9::d3d9::
                            RenderTapeInitialContentDisposition::
                                CompleteSeed);
                    snapshot.descriptor.assign(
                        std::as_bytes(std::span(&surface, 1u)).begin(),
                        std::as_bytes(std::span(&surface, 1u)).end());
                } else {
                    if (!renderTapeLoadTextureDescriptorV2(
                            snapshot.descriptor, texture) ||
                        texture.subresourceCount !=
                            snapshot.content.size()) {
                        return false;
                    }
                    subresourceCount = texture.subresourceCount;
                }
                for (std::uint32_t subresource = 0u;
                     subresource < subresourceCount;
                     ++subresource) {
                    D9CSurfaceDesc desc = surface.surface;
                    if (!standaloneColor && !presentOutputColor &&
                        !renderTapeTextureSubresourceDescriptor(
                            snapshot.descriptor, subresource, desc)) {
                        return false;
                    }
                    const auto expected = dxmt9::d3d9::
                        renderTapeDeriveExpectedSurfaceContent(desc);
                    if (expected.status != dxmt9::d3d9::
                            RenderTapeExpectedContentStatus::Accepted ||
                        expected.bytes == 0u ||
                        expected.bytes >
                            std::numeric_limits<std::size_t>::max()) {
                        return false;
                    }
                    snapshot.content[subresource].resize(
                        static_cast<std::size_t>(expected.bytes));
                    const D9CRenderTapeColorSnapshotRequest request{
                        .identity = object.identity,
                        .surface = desc,
                        .subresource = subresource,
                        .encodingVersion =
                            D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1,
                    };
                    D9CRenderTapeColorSnapshotResult result{};
                    const HRESULT hr = hr32(
                        dxmt9c_device_capture_render_tape_color_snapshot(
                            dev_, &request, &result,
                            snapshot.content[subresource].data(),
                            snapshot.content[subresource].size()));
                    const std::uint32_t expectedPitch = desc.width * 4u;
                    if (FAILED(hr) || result.status !=
                            D9C_RENDER_TAPE_COLOR_SNAPSHOT_COMPLETE ||
                        result.encodingVersion !=
                            D9C_RENDER_TAPE_COLOR_ENCODING_TIGHT_V1 ||
                        result.subresource != subresource ||
                        result.width != desc.width ||
                        result.height != desc.height ||
                        result.pitch != expectedPitch ||
                        result.format != desc.format ||
                        result.reserved0 != 0u ||
                        result.byteCount !=
                            snapshot.content[subresource].size()) {
                        dxmt9DeviceInfoLog(
                            "render_tape_capture color_snapshot rejected "
                            "reason=provider_result hr=0x%08x status=%u "
                            "generation=%u object_id=%llu subresource=%u",
                            static_cast<unsigned>(hr), result.status,
                            object.identity.generation,
                            static_cast<unsigned long long>(
                                object.identity.objectId),
                            subresource);
                        return false;
                    }
                    dxmt9DeviceInfoLog(
                        "render_tape_capture color_snapshot complete kind=%u "
                        "generation=%u object_id=%llu subresource=%u "
                        "encoding=1 format=%u width=%u height=%u pitch=%u "
                        "bytes=%zu",
                        snapshot.identity.kind,
                        snapshot.identity.generation,
                        static_cast<unsigned long long>(
                            snapshot.identity.objectId),
                        subresource, desc.format, desc.width, desc.height,
                        result.pitch,
                        snapshot.content[subresource].size());
                }
            }
            peCaptureState_->renderTapeArmSnapshots.push_back(std::move(snapshot));
        }
        return advanceRenderTapeArmBoundary(dxmt9::d3d9::
            RenderTapeArmBoundaryPhase::SnapshotComplete);
    } catch (...) {
        dxmt9DeviceInfoLog(
            "render_tape_capture arm_snapshot rejected reason=exception");
        return false;
    }
}

bool D3D9DeviceImpl::armRenderTapeCaptureAtPresentBoundary() {
    if (armRenderTapeCaptureAtPresentBoundaryInterval()) {
        return true;
    }
    releaseRenderTapePresentOutputRole(nullptr);
    return false;
}

bool D3D9DeviceImpl::armRenderTapeCaptureAtPresentBoundaryInterval() {
    if (!peCaptureState_ ||
        !peCaptureState_->renderTapeCapture.enabled() ||
        (peCaptureState_->renderTapeCapture.state() !=
             dxmt9::d3d9::RenderTapeCaptureState::Disabled &&
         peCaptureState_->renderTapeCapture.state() !=
             dxmt9::d3d9::RenderTapeCaptureState::Aborted)) {
        return false;
    }
    if (peCaptureState_->renderTapeArmPresentSkipRemaining != 0u) {
        --peCaptureState_->renderTapeArmPresentSkipRemaining;
        return false;
    }
    // An interval that aborted after arming still holds the role; release
    // it here so a retry starts from exactly one live present output.
    releaseRenderTapePresentOutputRole(nullptr);
    // Keep the first-abort marker sticky only for this arm/interval
    // lifecycle; a retry must get independent attribution.
    peCaptureState_->renderTapeAbortReason = nullptr;
    peCaptureState_->renderTapeAdmittedIdentities.clear();
    peCaptureState_->renderTapeExpectedDigest.reset();
    peCaptureState_->renderTapeExpectedPixels.clear();
    peCaptureState_->renderTapeExpectedSourcePixels.clear();
    peCaptureState_->renderTapeOutputDesc.reset();
    peCaptureState_->renderTapeFirstAccessLedger = {};
    const auto producer = dxmt9PeRenderTapeBootstrapProducer.load(
        std::memory_order_acquire);
    auto publisher = dxmt9PeRenderTapeArtifactPublisher.load(
        std::memory_order_acquire);
    if (!publisher) {
        publisher = dxmt9PeDefaultRenderTapeArtifactPublisher();
    }
    dxmt9DeviceInfoLog(
        "render_tape_capture arm enabled=1 producer=%d publisher=%d",
        producer != nullptr ? 1 : 0, publisher != nullptr ? 1 : 0);
    if (!dxmt9PeRenderTapeCaptureCallbacksInstalled(
            peCaptureState_->renderTapeCapture.enabled(), producer, publisher)) {
        dxmt9DeviceInfoLog(
            "render_tape_capture requested without artifact publisher; "
            "capture remains off");
        return false;
    }
    // Both callers reach this point only after the Present bridge call or
    // Present-record chunk commit returned success.
    peCaptureState_->renderTapeArmBoundaryPhase =
        dxmt9::d3d9::RenderTapeArmBoundaryPhase::Disabled;
    if (!advanceRenderTapeArmBoundary(dxmt9::d3d9::
            RenderTapeArmBoundaryPhase::PresentFlushed)) {
        return false;
    }
    // Admit the just-presented swap-chain backbuffer before taking the arm
    // snapshot. The backbuffer is the only capture identity whose role is
    // assigned lazily by the bootstrap producer; without this ordering its
    // actual post-arm bytes cannot be captured as starting content.
    if (!admitRenderTapePresentOutput() || peCaptureState_->renderTapeRegistry.invalid) {
        dxmt9DeviceInfoLog(
            "render_tape_capture arm aborted reason=present_output_admission");
        abortRenderTapeCapture("present_output_admission");
        return false;
    }
    if (!snapshotRenderTapeResourcesAtArmBoundary()) {
        dxmt9DeviceInfoLog(
            "render_tape_capture arm aborted reason=arm_snapshot");
        abortRenderTapeCapture("arm_snapshot");
        return false;
    }
    dxmt9::d3d9::RenderTapeCaptureBootstrapSeed seed{};
    bool produced = false;
    try {
        // A non-null injected producer is an explicit test override. The
        // production path always snapshots this device's value-owned PE
        // shadow and live-object store at the arm Present boundary.
        produced = producer ? producer(seed)
                            : produceRenderTapeBootstrap(seed);
        // Gamma is PE-owned persistent state and therefore part of the
        // bootstrap checkpoint, not an implicit host default. Keep the
        // bytes in the seed so injected producers can override the
        // complete snapshot in native tests.
        if (produced && seed.gammaRamp.empty()) {
            seed.gammaRamp.resize(dxmt9::d3d9::kRenderTapeGammaRampBytes);
            std::memcpy(seed.gammaRamp.data(), &gammaRamp_,
                        seed.gammaRamp.size());
        }
    } catch (...) {
        produced = false;
    }
    auto armStatus = dxmt9::d3d9::RenderTapeCaptureStatus::InvalidInput;
    auto intervalStatus = dxmt9::d3d9::RenderTapeCaptureStatus::InvalidState;
    if (produced) {
        armStatus = peCaptureState_->renderTapeCapture.armWithBlobs(
            seed.bootstrapOverlay, seed.blobs, seed.gammaRamp);
        if (armStatus == dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            intervalStatus = peCaptureState_->renderTapeCapture.beginPresentInterval();
        }
    }
    if (!produced ||
        armStatus != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted ||
        intervalStatus != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
        dxmt9DeviceInfoLog(
            "render_tape_capture arm aborted produced=%d arm_status=%u "
            "interval_status=%u objects=%zu mutations=%zu blobs=%zu",
            produced ? 1 : 0, static_cast<unsigned>(armStatus),
            static_cast<unsigned>(intervalStatus), seed.objects.size(),
            seed.mutations.size(), seed.blobs.size());
        abortRenderTapeCapture("arm_validation");
        return false;
    }
    for (const auto& object : seed.objects) {
        dxmt9::d3d9::RenderTapeObjectDefineDisposition disposition{};
        const auto status = peCaptureState_->renderTapeCapture.objectDefine(
                object.identity, object.descriptorKind, object.descriptor,
                object.immutableBytes, object.immutableDigest,
                object.expectedContentBytes,
                object.expectedContentCount, &disposition);
        if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            dxmt9DeviceInfoLog(
                "render_tape_capture seed_object_define status=%u reason=%s kind=%u "
                "generation=%u object_id=%llu descriptor=%zu immutable=%llu "
                "expected_bytes=%llu expected_count=%u",
                static_cast<unsigned>(status),
                dxmt9::d3d9::renderTapeObjectDefineDispositionName(
                    disposition),
                object.identity.kind,
                object.identity.generation,
                static_cast<unsigned long long>(object.identity.objectId),
                object.descriptor.size(),
                static_cast<unsigned long long>(object.immutableBytes),
                static_cast<unsigned long long>(object.expectedContentBytes),
                object.expectedContentCount);
            abortRenderTapeCapture("seed_object_define");
            return false;
        }
        try {
            peCaptureState_->renderTapeAdmittedIdentities.push_back(object.identity);
        } catch (...) {
            abortRenderTapeCapture("seed_identity_allocation");
            return false;
        }
    }
    for (const auto& mutation : seed.mutations) {
        const auto status = peCaptureState_->renderTapeCapture.resourceMutation(
            mutation.identity, mutation.kind, mutation.subresource,
            mutation.byteOffset, mutation.byteSize, mutation.digest,
            mutation.bufferDisposition);
        if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            dxmt9DeviceInfoLog(
                "render_tape_capture seed_resource_mutation status=%u kind=%u "
                "generation=%u object_id=%llu subresource=%u bytes=%llu",
                static_cast<unsigned>(status), mutation.identity.kind,
                mutation.identity.generation,
                static_cast<unsigned long long>(mutation.identity.objectId),
                mutation.subresource,
                static_cast<unsigned long long>(mutation.byteSize));
            abortRenderTapeCapture("seed_resource_mutation");
            return false;
        }
    }
    peCaptureState_->renderTapeCaptureOracle = std::move(seed.oracleAttachments);
    if (!advanceRenderTapeArmBoundary(dxmt9::d3d9::
            RenderTapeArmBoundaryPhase::Armed)) {
        abortRenderTapeCapture("arm_boundary_order");
        return false;
    }
    if (peCaptureState_->renderTapeNextCaptureToken ==
        std::numeric_limits<std::uint64_t>::max()) {
        peCaptureState_->renderTapeNextCaptureToken = 1u;
    } else {
        ++peCaptureState_->renderTapeNextCaptureToken;
    }
    peCaptureState_->renderTapeActiveCaptureToken = peCaptureState_->renderTapeNextCaptureToken;
    dxmt9DeviceInfoLog(
        "render_tape_capture arm_complete token=%llu objects=%zu "
        "mutations=%zu blobs=%zu",
        static_cast<unsigned long long>(peCaptureState_->renderTapeActiveCaptureToken),
        seed.objects.size(), seed.mutations.size(), seed.blobs.size());
    return true;
}

void D3D9DeviceImpl::finishRenderTapeCaptureAtPresentBoundary() noexcept {
    if (!peCaptureState_ ||
        peCaptureState_->renderTapeCapture.state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        return;
    }
    if (!peCaptureState_->renderTapeExpectedDigest) {
        abortRenderTapeCapture("present_output_capture_missing");
        return;
    }
    const std::uint64_t capturedPresentOrdinal =
        peCaptureState_->renderTapeCapture.eventCount();
    const auto status = peCaptureState_->renderTapeCapture.completePresent(
        capturedPresentOrdinal,
        ++peCaptureState_->renderTapeCompletionOrdinal,
        dxmt9::d3d9::RenderTapeDigestValidity::Sha256,
        *peCaptureState_->renderTapeExpectedDigest,
        std::as_bytes(std::span(peCaptureState_->renderTapeCaptureOracle)),
        peCaptureState_->renderTapeExpectedPixels, peCaptureState_->renderTapeExpectedSourcePixels);
    if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted &&
        status != dxmt9::d3d9::RenderTapeCaptureStatus::Complete) {
        dxmt9DeviceInfoLog(
            "render_tape_capture completion aborted status=%u events=%u "
            "chunks=%llu present_chunk_seen=%d oracle_bytes=%zu validation=%u",
            static_cast<unsigned>(status), peCaptureState_->renderTapeCapture.eventCount(),
            static_cast<unsigned long long>(recorderState_.commandChunkCommits),
            peCaptureState_->renderTapeCapture.presentChunkSeen() ? 1 : 0,
            std::as_bytes(std::span(peCaptureState_->renderTapeCaptureOracle)).size(),
            static_cast<unsigned>(peCaptureState_->renderTapeCapture.validationStatus()));
        const auto &validation = peCaptureState_->renderTapeCapture.validationResult();
        dxmt9DeviceInfoLog(
            "render_tape_capture validation_failure status=%u name=%s "
            "failed_event_index=%u failed_event_type=%u chunk_status=%u "
            "incomplete_reason=%s offending_present=%d offending_kind=%u "
            "offending_generation=%u offending_object_id=%llu",
            static_cast<unsigned>(validation.status),
            dxmt9::d3d9::renderTapeValidationStatusName(validation.status),
            validation.failedEventIndex, validation.failedEventType,
            static_cast<unsigned>(validation.chunkStatus),
            dxmt9::d3d9::renderTapeIncompleteFrameReasonName(
                validation.incompleteFrameReason),
            validation.hasOffendingIdentity ? 1 : 0,
            validation.offendingIdentity.kind,
            validation.offendingIdentity.generation,
            static_cast<unsigned long long>(
                validation.offendingIdentity.objectId));
        if (validation.objectDefine.valid()) {
            const auto &detail = validation.objectDefine;
            dxmt9DeviceInfoLog(
                "render_tape_capture object_define_detail subreason=%u "
                "name=%s kind=%u generation=%u object_id=%llu "
                "descriptor_kind=%u descriptor_bytes=%u "
                "descriptor_payload_bytes=%u payload_validity=%u "
                "immutable_bytes=%llu expected_bytes=%llu "
                "expected_count=%u schema=%u dimension=%u mips=%u "
                "subresources=%u storage=%u disposition=%u subresource=%u "
                "descriptor_extent=%llu/%llu parent_kind=%u "
                "parent_generation=%u parent_object_id=%llu",
                static_cast<unsigned>(detail.subreason),
                dxmt9::d3d9::renderTapeObjectDefineValidationSubreasonName(
                    detail.subreason),
                detail.identity.kind, detail.identity.generation,
                static_cast<unsigned long long>(detail.identity.objectId),
                detail.descriptorKind, detail.descriptorBytes,
                detail.descriptorPayloadBytes, detail.payloadValidity,
                static_cast<unsigned long long>(detail.immutablePayloadBytes),
                static_cast<unsigned long long>(detail.expectedContentBytes),
                detail.expectedContentCount, detail.descriptorSchemaVersion,
                detail.descriptorDimension, detail.descriptorMipLevelCount,
                detail.descriptorSubresourceCount, detail.descriptorStorage,
                detail.descriptorDisposition, detail.descriptorSubresource,
                static_cast<unsigned long long>(detail.descriptorExtentBytes),
                static_cast<unsigned long long>(
                    detail.descriptorExpectedExtentBytes),
                detail.parentTexture.kind, detail.parentTexture.generation,
                static_cast<unsigned long long>(
                    detail.parentTexture.objectId));
        }
        abortRenderTapeCapture("completion");
        return;
    }
    // Sequence profile keeps the first interval journaled but unsealed;
    // publication is deliberately deferred until the second Present has
    // passed final validation.
    if (status == dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
        if (dxmt9::d3d9::renderTapeArmSnapshotCompletionAction(false) !=
            dxmt9::d3d9::RenderTapeArmSnapshotCompletionAction::
                RetainForNextInterval) {
            abortRenderTapeCapture("snapshot_completion_policy");
            return;
        }
        peCaptureState_->renderTapeExpectedDigest.reset();
        peCaptureState_->renderTapeExpectedPixels.clear();
        peCaptureState_->renderTapeExpectedSourcePixels.clear();
        return;
    }
    D9CRenderTapeIdentityCaptureResult identityResult{};
    if (peCaptureState_->renderTapeActiveCaptureToken == 0u ||
        FAILED(hr32(dxmt9c_device_finish_render_tape_identity_capture(
            dev_, peCaptureState_->renderTapeActiveCaptureToken, &identityResult,
            nullptr, 0u))) ||
        identityResult.status !=
            D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE ||
        identityResult.captureToken != peCaptureState_->renderTapeActiveCaptureToken ||
        identityResult.sourceCount == 0u ||
        identityResult.rangeCount == 0u ||
        identityResult.reserved0 != 0u ||
        identityResult.reserved1 != 0u ||
        identityResult.reserved2 != 0u ||
        identityResult.settlementCount == 0u ||
        identityResult.eventOrdinal == 0u ||
        identityResult.settlementSourceOrdinal == 0u ||
        identityResult.settlementSeqId == 0u ||
        identityResult.settlementEntrySize !=
            sizeof(D9CRenderTapeIdentitySettlementEntry) ||
        identityResult.settlementTableOffset == 0u ||
        identityResult.byteCount >
            std::numeric_limits<std::size_t>::max()) {
        abortRenderTapeCapture("identity_query");
        return;
    }
    std::vector<std::byte> identityBytes;
    try {
        identityBytes.resize(
            static_cast<std::size_t>(identityResult.byteCount));
    } catch (...) {
        abortRenderTapeCapture("identity_allocation");
        return;
    }
    D9CRenderTapeIdentityCaptureResult copiedIdentity{};
    if (FAILED(hr32(dxmt9c_device_finish_render_tape_identity_capture(
            dev_, peCaptureState_->renderTapeActiveCaptureToken, &copiedIdentity,
            identityBytes.data(), identityBytes.size()))) ||
        copiedIdentity.status !=
            D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE ||
        copiedIdentity.sourceCount != identityResult.sourceCount ||
        copiedIdentity.rangeCount != identityResult.rangeCount ||
        copiedIdentity.captureToken != identityResult.captureToken ||
        copiedIdentity.byteCount != identityResult.byteCount ||
        copiedIdentity.eventOrdinal != identityResult.eventOrdinal ||
        copiedIdentity.settlementSourceOrdinal !=
            identityResult.settlementSourceOrdinal ||
        copiedIdentity.settlementSeqId != identityResult.settlementSeqId ||
        copiedIdentity.settlementCount != identityResult.settlementCount ||
        copiedIdentity.settlementEntrySize != identityResult.settlementEntrySize ||
        copiedIdentity.settlementTableOffset != identityResult.settlementTableOffset ||
        copiedIdentity.reserved1 != 0u || copiedIdentity.reserved2 != 0u) {
        abortRenderTapeCapture("identity_copy");
        return;
    }
    if (static_cast<std::size_t>(copiedIdentity.sourceCount) >
            std::numeric_limits<std::size_t>::max() /
                sizeof(D9CRenderTapeIdentitySourceEntry) ||
        static_cast<std::size_t>(copiedIdentity.rangeCount) >
            std::numeric_limits<std::size_t>::max() /
                sizeof(D9CRenderTapeIdentityRangeEntry) ||
        static_cast<std::size_t>(copiedIdentity.settlementCount) >
            std::numeric_limits<std::size_t>::max() /
                sizeof(D9CRenderTapeIdentitySettlementEntry)) {
        abortRenderTapeCapture("identity_layout");
        return;
    }
    const std::size_t sourceBytes =
        static_cast<std::size_t>(copiedIdentity.sourceCount) *
        sizeof(D9CRenderTapeIdentitySourceEntry);
    const std::size_t rangeBytes =
        static_cast<std::size_t>(copiedIdentity.rangeCount) *
        sizeof(D9CRenderTapeIdentityRangeEntry);
    if (sourceBytes > std::numeric_limits<std::size_t>::max() - rangeBytes) {
        abortRenderTapeCapture("identity_layout");
        return;
    }
    const std::size_t sourceAndRangeBytes = sourceBytes + rangeBytes;
    const std::size_t settlementBytes =
        static_cast<std::size_t>(copiedIdentity.settlementCount) *
        sizeof(D9CRenderTapeIdentitySettlementEntry);
    if (sourceAndRangeBytes > std::numeric_limits<std::size_t>::max() -
                                  settlementBytes ||
        sourceBytes > identityBytes.size() ||
        rangeBytes > identityBytes.size() - sourceBytes ||
        copiedIdentity.settlementTableOffset != sourceAndRangeBytes ||
        settlementBytes != identityBytes.size() - sourceAndRangeBytes) {
        abortRenderTapeCapture("identity_layout");
        return;
    }
    std::vector<dxmt9::d3d9::RenderTapeIdentitySource> identitySources;
    std::vector<dxmt9::d3d9::RenderTapeIdentityRange> identityRanges;
    std::vector<D9CRenderTapeIdentitySettlementEntry> identitySettlements;
    try {
        identitySources.resize(copiedIdentity.sourceCount);
        identityRanges.resize(copiedIdentity.rangeCount);
        identitySettlements.resize(copiedIdentity.settlementCount);
    } catch (...) {
        abortRenderTapeCapture("identity_allocation");
        return;
    }
    static_assert(sizeof(D9CRenderTapeIdentitySourceEntry) ==
                  sizeof(dxmt9::d3d9::RenderTapeIdentitySource));
    static_assert(sizeof(D9CRenderTapeIdentityRangeEntry) ==
                  sizeof(dxmt9::d3d9::RenderTapeIdentityRange));
    std::memcpy(identitySources.data(), identityBytes.data(), sourceBytes);
    std::memcpy(identityRanges.data(), identityBytes.data() + sourceBytes,
                rangeBytes);
    std::memcpy(identitySettlements.data(),
                identityBytes.data() + sourceBytes + rangeBytes,
                settlementBytes);
    if (peCaptureState_->renderTapeCapture.attachCaptureIdentity(
            peCaptureState_->renderTapeActiveCaptureToken, capturedPresentOrdinal,
            identitySources, identityRanges,
            dxmt9::d3d9::RenderTapeIdentityEventSettlement{
                .eventOrdinal = copiedIdentity.eventOrdinal,
                .sourceOrdinal = copiedIdentity.settlementSourceOrdinal,
                .seqId = copiedIdentity.settlementSeqId,
                .count = 1u,
            }, identitySettlements) !=
        dxmt9::d3d9::RenderTapeCaptureStatus::Complete) {
        const auto& identityValidation =
            peCaptureState_->renderTapeCapture.identityValidationResult();
        dxmt9DeviceInfoLog(
            "render_tape_capture identity_attach_failure status=%u name=%s "
            "failed_source=%u failed_range=%u sources=%zu ranges=%zu",
            static_cast<unsigned>(identityValidation.status),
            dxmt9::d3d9::renderTapeIdentityStatusName(
                identityValidation.status),
            identityValidation.failedSource,
            identityValidation.failedRange, identitySources.size(),
            identityRanges.size());
        abortRenderTapeCapture("identity_attach");
        return;
    }
    auto publisher = dxmt9PeRenderTapeArtifactPublisher.load(
        std::memory_order_acquire);
    if (!publisher) {
        publisher = dxmt9PeDefaultRenderTapeArtifactPublisher();
    }
    const bool published =
        publisher && publisher(peCaptureState_->renderTapeCapture.publicationBundle());
    dxmt9DeviceInfoLog("render_tape_capture publication published=%d",
                       published ? 1 : 0);
    if (!published) {
        abortRenderTapeCapture("publication");
    }
    if (dxmt9::d3d9::renderTapeArmSnapshotCompletionAction(true) ==
        dxmt9::d3d9::RenderTapeArmSnapshotCompletionAction::Clear) {
        peCaptureState_->renderTapeArmSnapshots.clear();
    }
    peCaptureState_->renderTapeExpectedDigest.reset();
    peCaptureState_->renderTapeExpectedPixels.clear();
    peCaptureState_->renderTapeExpectedSourcePixels.clear();
    peCaptureState_->renderTapeActiveCaptureToken = 0u;
}
