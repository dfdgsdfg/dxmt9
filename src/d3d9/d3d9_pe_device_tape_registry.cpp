/* src/d3d9/d3d9_pe_device_tape_registry.cpp — Render Tape registry/identity.
 *
 * Registry admission, identity/tombstone validation, object materialization,
 * and byte/mutation bookkeeping are kept together so ownership decisions are
 * visible in one cold translation unit. */

#include "d3d9_pe_device_impl.hpp"
#include "d3d9_pe_tape_support.hpp"

RenderTapeLiveObject *D3D9DeviceImpl::findRenderTapeObject(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
    if (!peCaptureState_) {
        return nullptr;
    }
    const auto it = std::find_if(
        peCaptureState_->renderTapeRegistry.objects.begin(),
        peCaptureState_->renderTapeRegistry.objects.end(),
        [&](const auto &entry) {
            return renderTapeSameIdentity(entry.identity, object.identity);
        });
    return it == peCaptureState_->renderTapeRegistry.objects.end() ? nullptr : &*it;
}
const RenderTapeLiveObject *D3D9DeviceImpl::findRenderTapeObject(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) const noexcept {
    if (!peCaptureState_) {
        return nullptr;
    }
    const auto it = std::find_if(
        peCaptureState_->renderTapeRegistry.objects.begin(),
        peCaptureState_->renderTapeRegistry.objects.end(),
        [&](const auto &entry) {
            return renderTapeSameIdentity(entry.identity, object.identity);
        });
    return it == peCaptureState_->renderTapeRegistry.objects.end() ? nullptr : &*it;
}

bool D3D9DeviceImpl::hasRenderTapeDeadObject(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) const noexcept {
    return peCaptureState_ &&
           std::any_of(peCaptureState_->renderTapeRegistry.knownDead.begin(),
                       peCaptureState_->renderTapeRegistry.knownDead.end(),
                       [&](const auto &identity) {
                           return renderTapeSameIdentity(identity,
                                                         object.identity);
                       });
}

void D3D9DeviceImpl::markRenderTapeInvalidOnce(
    const char *reason,
    const dxmt9::d3d9::pe::PeWireObjectRef *object,
    std::uint32_t subresource,
    const dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic &diagnostic) noexcept {
    if (!peCaptureState_ || peCaptureState_->renderTapeRegistry.invalid) {
        return;
    }
    peCaptureState_->renderTapeRegistry.invalid = true;
    peCaptureState_->renderTapeRegistry.invalidReason = reason;
    peCaptureState_->renderTapeRegistry.invalidSubresource = subresource;
    peCaptureState_->renderTapeRegistry.invalidLayout = diagnostic;
    if (object) {
        peCaptureState_->renderTapeRegistry.invalidKind = object->identity.kind;
        peCaptureState_->renderTapeRegistry.invalidGeneration = object->identity.generation;
        peCaptureState_->renderTapeRegistry.invalidObjectId = object->identity.objectId;
    }
}

RenderTapeObjectRegistration D3D9DeviceImpl::registerRenderTapeObject(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::span<const std::byte> descriptor,
    std::span<const std::byte> immutablePayload,
    RenderTapeLiveObject::Role role,
    std::uint32_t replacementRestart) noexcept {
    if (!peCaptureState_) {
        return RenderTapeObjectRegistration::Rejected;
    }
    bool replacingRetainedAlias = false;
    try {
        if (!object.valid(object.identity.kind)) {
            markRenderTapeInvalidOnce("invalid_identity", &object);
            return RenderTapeObjectRegistration::Rejected;
        }
        if (descriptor.empty()) {
            markRenderTapeInvalidOnce("empty_descriptor", &object);
            return RenderTapeObjectRegistration::Rejected;
        }
        if (auto *existing = findRenderTapeObject(object)) {
            const bool descriptorMatches =
                (existing->descriptor.size() == descriptor.size() &&
                 std::equal(existing->descriptor.begin(),
                            existing->descriptor.end(),
                            descriptor.begin())) ||
                (object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE &&
                 dxmt9::d3d9::renderTapeSurfaceAliasDescriptorsEqual(
                     existing->descriptor, descriptor));
            if (!descriptorMatches ||
                existing->immutablePayload.size() !=
                    immutablePayload.size() ||
                !std::equal(existing->immutablePayload.begin(),
                            existing->immutablePayload.end(),
                            immutablePayload.begin())) {
                markRenderTapeInvalidOnce("duplicate_object_identity_conflict",
                                         &object);
                return RenderTapeObjectRegistration::Rejected;
            }
            if (!existing->lifetime.acquire()) {
                markRenderTapeInvalidOnce("duplicate_object_ref_overflow",
                                         &object);
                return RenderTapeObjectRegistration::Rejected;
            }
            return RenderTapeObjectRegistration::Existing;
        }
        if (hasRenderTapeDeadObject(object)) {
            markRenderTapeInvalidOnce(
                "duplicate_retired_object_identity", &object);
            if (IsRenderTapeCaptureActiveForChild())
                abortRenderTapeCapture(
                    "duplicate_retired_object_identity");
            return RenderTapeObjectRegistration::Rejected;
        }
        const auto logicalSlot = dxmt9::d3d9::
            renderTapeLogicalObjectSlot(object.identity, descriptor);
        if (logicalSlot.malformedSurfaceDescriptor) {
            markRenderTapeInvalidOnce("surface_descriptor_v2", &object);
            if (IsRenderTapeCaptureActiveForChild())
                abortRenderTapeCapture("surface_descriptor_v2");
            return RenderTapeObjectRegistration::Rejected;
        }
        const auto generationDoesNotAdvance =
            [&](const D9CWireObjectIdentity &prior) {
                return dxmt9::d3d9::renderTapeSameWireObject(
                           prior, object.identity) &&
                       !dxmt9::d3d9::renderTapeWireGenerationAdvances(
                           prior, object.identity);
            };
        if (std::any_of(peCaptureState_->renderTapeRegistry.objects.begin(),
                        peCaptureState_->renderTapeRegistry.objects.end(),
                        [&](const auto &candidate) {
                            return generationDoesNotAdvance(
                                candidate.identity);
                        }) ||
            std::any_of(peCaptureState_->renderTapeRegistry.knownDead.begin(),
                        peCaptureState_->renderTapeRegistry.knownDead.end(),
                        generationDoesNotAdvance)) {
            markRenderTapeInvalidOnce("non_monotone_generation", &object);
            if (IsRenderTapeCaptureActiveForChild())
                abortRenderTapeCapture("non_monotone_generation");
            return RenderTapeObjectRegistration::Rejected;
        }
        const auto replacement = std::find_if(
            peCaptureState_->renderTapeRegistry.objects.begin(),
            peCaptureState_->renderTapeRegistry.objects.end(),
            [&](const auto &candidate) {
                const auto candidateSlot = dxmt9::d3d9::
                    renderTapeLogicalObjectSlot(candidate.identity,
                                                candidate.descriptor);
                return dxmt9::d3d9::renderTapeLogicalSlotRelation(
                           candidateSlot, logicalSlot) !=
                       dxmt9::d3d9::
                           RenderTapeLogicalSlotRelation::Different;
            });
        std::size_t replacementIndex =
            std::numeric_limits<std::size_t>::max();
        if (replacement != peCaptureState_->renderTapeRegistry.objects.end()) {
            const auto transition = dxmt9::d3d9::
                renderTapeSurfaceAliasReplacementStatus(
                    replacement->identity, replacement->lifetime,
                    replacement->descriptor, object.identity, descriptor);
            if (transition == dxmt9::d3d9::
                                  RenderTapeSurfaceAliasReplacementStatus::
                                      PendingChunkRequiresFlush) {
                const auto priorIdentity = replacement->identity;
                const auto priorPending =
                    replacement->lifetime.pendingChunkRefs;
                dxmt9DeviceInfoLog(
                    "render_tape_capture alias_pending_flush profile=%u "
                    "device=%p registry=%p restart=%u "
                    "old_kind=%u old_generation=%u old_object_id=%llu "
                    "new_kind=%u new_generation=%u new_object_id=%llu "
                    "pending=%u admitted=%d known_dead=%d",
                    dxmt9PeRenderTapeCaptureProfile(), this,
                    static_cast<void *>(&peCaptureState_->renderTapeRegistry),
                    replacementRestart, priorIdentity.kind,
                    priorIdentity.generation,
                    static_cast<unsigned long long>(priorIdentity.objectId),
                    object.identity.kind, object.identity.generation,
                    static_cast<unsigned long long>(object.identity.objectId),
                    priorPending,
                    renderTapeObjectAdmitted(priorIdentity) ? 1 : 0,
                    hasRenderTapeDeadObject(dxmt9::d3d9::pe::PeWireObjectRef{
                        .identity = priorIdentity}) ? 1 : 0);
                if (replacementRestart != 0u) {
                    markRenderTapeInvalidOnce(
                        "alias_pending_flush_restart_exhausted", &object);
                    if (IsRenderTapeCaptureActiveForChild())
                        abortRenderTapeCapture(
                            "alias_pending_flush_restart_exhausted");
                    return RenderTapeObjectRegistration::Rejected;
                }
                const HRESULT flushHr = flushPendingCommandChunk(
                    PeRecorderFlushReason::Child);
                dxmt9DeviceInfoLog(
                    "render_tape_capture alias_pending_flush_end "
                    "hr=0x%08x disposition=%s",
                    static_cast<unsigned>(flushHr),
                    SUCCEEDED(flushHr) ? "completed" : "failed");
                if (FAILED(flushHr)) {
                    markRenderTapeInvalidOnce(
                        "alias_pending_flush_failed", &object);
                    if (IsRenderTapeCaptureActiveForChild())
                        abortRenderTapeCapture(
                            "alias_pending_flush_failed");
                    return RenderTapeObjectRegistration::Rejected;
                }
                return registerRenderTapeObject(
                    object, descriptor, immutablePayload, role,
                    replacementRestart + 1u);
            }
            if (transition != dxmt9::d3d9::
                                  RenderTapeSurfaceAliasReplacementStatus::
                                      Accepted) {
                const char *reason = dxmt9::d3d9::
                    renderTapeSurfaceAliasReplacementStatusName(transition);
                markRenderTapeInvalidOnce(reason, &object);
                dxmt9DeviceInfoLog(
                    "render_tape_capture alias_generation rejected reason=%s "
                    "old_generation=%u new_generation=%u "
                    "old_object_id=%llu new_object_id=%llu",
                    reason, replacement->identity.generation,
                    object.identity.generation,
                    static_cast<unsigned long long>(
                        replacement->identity.objectId),
                    static_cast<unsigned long long>(
                        object.identity.objectId));
                if (IsRenderTapeCaptureActiveForChild())
                    abortRenderTapeCapture(reason);
                return RenderTapeObjectRegistration::Rejected;
            }
            replacingRetainedAlias = true;
            replacementIndex = static_cast<std::size_t>(
                replacement - peCaptureState_->renderTapeRegistry.objects.begin());
        }
        RenderTapeLiveObject entry{};
        entry.identity = object.identity;
        entry.descriptor.assign(descriptor.begin(), descriptor.end());
        entry.immutablePayload.assign(immutablePayload.begin(),
                                      immutablePayload.end());
        if (!entry.lifetime.acquire()) {
            markRenderTapeInvalidOnce("object_ref_overflow", &object);
            return RenderTapeObjectRegistration::Rejected;
        }
        entry.role = role;
        switch (object.identity.kind) {
        case D9C_CHUNK_HANDLE_KIND_BUFFER:
            if (entry.descriptor.size() != sizeof(D9CBufferDesc)) {
                markRenderTapeInvalidOnce("buffer_descriptor_size", &object);
                return RenderTapeObjectRegistration::Rejected;
            }
            entry.contentCount = 1u;
            break;
        case D9C_CHUNK_HANDLE_KIND_SURFACE: {
            dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
            if (!dxmt9::d3d9::renderTapeLoadSurfaceDescriptorV2(
                    entry.descriptor, surface)) {
                markRenderTapeInvalidOnce("surface_descriptor_v2",
                                          &object);
                return RenderTapeObjectRegistration::Rejected;
            }
            if (surface.storage == static_cast<std::uint32_t>(
                                       dxmt9::d3d9::RenderTapeSurfaceStorage::
                                           TextureSubresource)) {
                if (role == RenderTapeLiveObject::Role::PresentOutput) {
                    markRenderTapeInvalidOnce("surface_descriptor_role",
                                              &object);
                    return RenderTapeObjectRegistration::Rejected;
                }
                entry.lifetime.textureAlias = true;
                entry.aliasParentTexture = surface.parentTexture;
                entry.contentCount = 0u;
                break;
            }
            if ((role == RenderTapeLiveObject::Role::PresentOutput) !=
                (surface.storage == static_cast<std::uint32_t>(
                     dxmt9::d3d9::RenderTapeSurfaceStorage::
                         SwapchainBackbuffer))) {
                markRenderTapeInvalidOnce("surface_descriptor_role", &object);
                return RenderTapeObjectRegistration::Rejected;
            }
            entry.contentCount =
                role == RenderTapeLiveObject::Role::PresentOutput ? 0u
                                                                    : 1u;
            break;
        }
        case D9C_CHUNK_HANDLE_KIND_TEXTURE: {
            RenderTapeTextureDescriptorV2 texture{};
            if (!dxmt9::d3d9::renderTapeLoadTextureDescriptorV2(
                    entry.descriptor, texture) ||
                texture.initialContentDisposition !=
                    static_cast<std::uint32_t>(dxmt9::d3d9::
                        RenderTapeInitialContentDisposition::CompleteSeed)) {
                markRenderTapeInvalidOnce("texture_descriptor_v2", &object);
                return RenderTapeObjectRegistration::Rejected;
            }
            entry.contentCount = texture.subresourceCount;
            break;
        }
        default:
            break;
        }
        entry.content.resize(entry.contentCount);
        if (replacingRetainedAlias) {
            auto &prior = peCaptureState_->renderTapeRegistry.objects[replacementIndex];
            const bool priorAdmitted = renderTapeObjectAdmitted(prior.identity);
            peCaptureState_->renderTapeRegistry.knownDead.reserve(
                peCaptureState_->renderTapeRegistry.knownDead.size() + 1u);
            if (priorAdmitted && IsRenderTapeCaptureActiveForChild()) {
                const auto destroyStatus =
                    peCaptureState_->renderTapeCapture.objectDestroy(prior.identity);
                if (destroyStatus != dxmt9::d3d9::
                                         RenderTapeCaptureStatus::Accepted) {
                    markRenderTapeInvalidOnce(
                        "alias_generation_destroy_failed", &object);
                    abortRenderTapeCapture(
                        "alias_generation_destroy_failed");
                    return RenderTapeObjectRegistration::Rejected;
                }
                removeRenderTapeObjectAdmitted(prior.identity);
            }
            dxmt9DeviceInfoLog(
                "render_tape_capture alias_generation replaced "
                "old_generation=%u new_generation=%u object_id=%llu",
                prior.identity.generation, object.identity.generation,
                static_cast<unsigned long long>(object.identity.objectId));
            peCaptureState_->renderTapeRegistry.knownDead.push_back(prior.identity);
            prior = std::move(entry);
            return RenderTapeObjectRegistration::New;
        }
        peCaptureState_->renderTapeRegistry.objects.push_back(std::move(entry));
        return RenderTapeObjectRegistration::New;
    } catch (...) {
        markRenderTapeInvalidOnce("object_registration_exception", &object);
        if (replacingRetainedAlias && IsRenderTapeCaptureActiveForChild())
            abortRenderTapeCapture("object_registration_exception");
        return RenderTapeObjectRegistration::Rejected;
    }
}

void D3D9DeviceImpl::releaseRenderTapePresentOutputRole(
    const D9CWireObjectIdentity *next) noexcept {
    if (!peCaptureState_) {
        return;
    }
    // Only a pre-arm or aborted lifecycle may move the role. An armed or
    // capturing interval owns its holder until it terminates.
    if (peCaptureState_ &&
        peCaptureState_->renderTapeCapture.state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Disabled &&
        peCaptureState_->renderTapeCapture.state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Aborted) {
        return;
    }
    auto &role = peCaptureState_->renderTapeRegistry.presentOutputRole;
    const dxmt9::d3d9::pe::PeWireObjectRef priorObject{
        .identity = role.identity,
    };
    auto *prior = role.held ? findRenderTapeObject(priorObject) : nullptr;
    const auto transition = dxmt9::d3d9::renderTapePresentOutputRoleTransition(
        role, next, prior != nullptr,
        prior ? prior->lifetime.wrapperRefs : 0u);
    if (transition == dxmt9::d3d9::RenderTapePresentOutputRoleTransition::
                          Retained) {
        return;
    }
    const auto identity = role.identity;
    // Only a capture-owned holder carries a wrapper reference this device
    // took; an app-owned entry was merely re-roled in place.
    const bool releaseAdmissionRef = role.captureOwned;
    const auto priorContentCount =
        peCaptureState_->renderTapeRegistry.presentOutputPriorContentCount;
    auto priorDescriptor =
        std::move(peCaptureState_->renderTapeRegistry.presentOutputPriorDescriptor);
    auto priorContent =
        std::move(peCaptureState_->renderTapeRegistry.presentOutputPriorContent);
    role = {};
    peCaptureState_->renderTapeRegistry.presentOutputPriorDescriptor.clear();
    peCaptureState_->renderTapeRegistry.presentOutputPriorContentCount = 0u;
    peCaptureState_->renderTapeRegistry.presentOutputPriorContent.clear();
    if (transition ==
        dxmt9::d3d9::RenderTapePresentOutputRoleTransition::None) {
        return;
    }
    if (prior->role != RenderTapeLiveObject::Role::PresentOutput) {
        // Something else already reclaimed the entry; leave it alone.
        return;
    }
    if (transition ==
        dxmt9::d3d9::RenderTapePresentOutputRoleTransition::Demote) {
        prior->role = RenderTapeLiveObject::Role::Ordinary;
        prior->descriptor = std::move(priorDescriptor);
        prior->contentCount = priorContentCount;
        prior->content = std::move(priorContent);
        if (releaseAdmissionRef)
            (void)prior->lifetime.releaseWrapper();
        dxmt9DeviceInfoLog(
            "render_tape_capture present_output released transition=%s "
            "kind=%u generation=%u object_id=%llu",
            dxmt9::d3d9::renderTapePresentOutputRoleTransitionName(
                transition),
            identity.kind, identity.generation,
            static_cast<unsigned long long>(identity.objectId));
        return;
    }
    try {
        peCaptureState_->renderTapeRegistry.knownDead.push_back(identity);
    } catch (...) {
        markRenderTapeInvalidOnce("present_output_tombstone_allocation",
                                  &priorObject);
        return;
    }
    if (releaseAdmissionRef)
        (void)prior->lifetime.releaseWrapper();
    peCaptureState_->renderTapeRegistry.objects.erase(
        peCaptureState_->renderTapeRegistry.objects.begin() +
        (prior - peCaptureState_->renderTapeRegistry.objects.data()));
    dxmt9DeviceInfoLog(
        "render_tape_capture present_output released transition=%s kind=%u "
        "generation=%u object_id=%llu",
        dxmt9::d3d9::renderTapePresentOutputRoleTransitionName(transition),
        identity.kind, identity.generation,
        static_cast<unsigned long long>(identity.objectId));
}

bool D3D9DeviceImpl::admitRenderTapePresentOutput() noexcept {
    if (!peCaptureState_) {
        return false;
    }
    // Use the stable cached PE backbuffer wrapper. Calling the C getter
    // directly would allocate a fresh D9CSurface and therefore a second
    // generation-qualified identity, while command chunks use the
    // wrapper-owned raw surface returned by the swap-chain cache.
    IDirect3DSwapChain9 *swapchain = nullptr;
    if (FAILED(GetSwapChain(0u, &swapchain)) || !swapchain) {
        dxmt9DeviceInfoLog(
            "render_tape_capture producer aborted reason=present_output_swapchain_missing");
        return false;
    }
    IDirect3DSurface9 *backBuffer = nullptr;
    const HRESULT backBufferHr = swapchain->GetBackBuffer(
        0u, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    swapchain->Release();
    // These getters only observe the post-Present call-cadence counters;
    // neither path calls Present or arms capture. The swap-chain cache
    // keeps its own backbuffer-wrapper reference, so releasing this
    // temporary COM reference cannot release the identity used by the
    // command chunks or the PresentOutput role.
    if (FAILED(backBufferHr) || !backBuffer) {
        dxmt9DeviceInfoLog(
            "render_tape_capture producer aborted reason=present_output_surface_missing");
        return false;
    }
    D3D9PeValidatedSurface validatedSurface{};
    if (FAILED(D3D9PeValidateSurface(
            backBuffer, static_cast<IDirect3DDevice9*>(this),
            &validatedSurface))) {
        backBuffer->Release();
        dxmt9DeviceInfoLog(
            "render_tape_capture producer aborted reason=present_output_surface_foreign");
        return false;
    }
    auto *surface = validatedSurface.raw();
    D9CWireObjectIdentity identity{};
    D9CWireObjectIdentity rawIdentity{};
    D9CSurfaceDesc descriptor{};
    const auto &cachedWireObject = validatedSurface.wire();
    const bool rawIdentityOk =
        dxmt9c_surface_get_wire_identity(surface, &rawIdentity) >= 0;
    identity = rawIdentity;
    const bool identityOk = cachedWireObject.valid() &&
        rawIdentityOk &&
        dxmt9::d3d9::renderTapePresentOutputIdentityMatchesCommand(
            cachedWireObject.identity, rawIdentity);
    const bool descriptorOk = dxmt9c_surface_get_desc(surface, &descriptor) >= 0;
    if (!identityOk || !descriptorOk ||
        identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
        identity.generation == 0u || identity.objectId == 0u) {
        backBuffer->Release();
        dxmt9DeviceInfoLog(
            "render_tape_capture producer aborted reason=present_output_identity_or_descriptor_invalid");
        return false;
    }
    peCaptureState_->renderTapeOutputDesc = descriptor;
    const dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 outputDescriptor{
        .schemaVersion =
            dxmt9::d3d9::kRenderTapeSurfaceDescriptorVersion2,
        .storage = static_cast<std::uint32_t>(
            dxmt9::d3d9::RenderTapeSurfaceStorage::SwapchainBackbuffer),
        .initialContentDisposition = static_cast<std::uint32_t>(
            dxmt9::d3d9::RenderTapeInitialContentDisposition::
                ProducedPresentOutput),
        .surface = descriptor,
    };
    const dxmt9::d3d9::pe::PeWireObjectRef object{
        .identity = identity,
        .object = surface,
    };
    // The role must leave its previous holder before this identity is
    // registered: a recycled wire object id would otherwise meet the stale
    // entry in the logical-slot replacement scan, and a standalone surface
    // is deliberately not an alias replacement candidate there.
    releaseRenderTapePresentOutputRole(&identity);
    // A retained role already stashed this holder's displaced content on an
    // earlier admission; re-stashing would capture the cleared state.
    const bool roleRetained = peCaptureState_->renderTapeRegistry.presentOutputRole.held;
    auto *existing = findRenderTapeObject(object);
    const bool captureOwned = existing == nullptr;
    if (existing) {
        if (!roleRetained) {
            peCaptureState_->renderTapeRegistry.presentOutputPriorDescriptor =
                existing->descriptor;
            peCaptureState_->renderTapeRegistry.presentOutputPriorContentCount =
                existing->contentCount;
            peCaptureState_->renderTapeRegistry.presentOutputPriorContent =
                std::move(existing->content);
        }
        existing->role = RenderTapeLiveObject::Role::PresentOutput;
        existing->descriptor.assign(
            reinterpret_cast<const std::byte *>(&outputDescriptor),
            reinterpret_cast<const std::byte *>(&outputDescriptor + 1u));
        existing->contentCount = 0u;
        existing->content.clear();
    } else {
        peCaptureState_->renderTapeRegistry.presentOutputPriorDescriptor.clear();
        peCaptureState_->renderTapeRegistry.presentOutputPriorContentCount = 0u;
        peCaptureState_->renderTapeRegistry.presentOutputPriorContent.clear();
        registerRenderTapeObject(
            object,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(&outputDescriptor),
                sizeof(outputDescriptor)),
            {}, RenderTapeLiveObject::Role::PresentOutput);
    }
    backBuffer->Release();
    const auto *admitted = findRenderTapeObject(object);
    if (!admitted || admitted->role != RenderTapeLiveObject::Role::PresentOutput) {
        dxmt9DeviceInfoLog(
            "render_tape_capture producer aborted reason=present_output_admission_failed "
            "kind=%u generation=%u object_id=%llu",
            identity.kind, identity.generation,
            static_cast<unsigned long long>(identity.objectId));
        return false;
    }
    auto &role = peCaptureState_->renderTapeRegistry.presentOutputRole;
    role.identity = identity;
    role.captureOwned = roleRetained ? role.captureOwned : captureOwned;
    role.held = true;
    dxmt9DeviceInfoLog(
        "render_tape_capture present_output admitted kind=%u generation=%u "
        "object_id=%llu descriptor=%zu initial_content=not_required "
        "capture_owned=%d",
        identity.kind, identity.generation,
        static_cast<unsigned long long>(identity.objectId),
        sizeof(outputDescriptor),
        peCaptureState_->renderTapeRegistry.presentOutputRole.captureOwned ? 1 : 0);
    return true;
}

bool D3D9DeviceImpl::unregisterRenderTapeObject(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
    if (!peCaptureState_) {
        return false;
    }
    const auto it = std::find_if(
        peCaptureState_->renderTapeRegistry.objects.begin(),
        peCaptureState_->renderTapeRegistry.objects.end(),
        [&](const auto &entry) {
            return renderTapeSameIdentity(entry.identity, object.identity);
        });
    if (it == peCaptureState_->renderTapeRegistry.objects.end()) {
        // Swap-chain-owned surfaces can be destroyed through the common
        // child path even though they were never admitted to the
        // value-owned capture registry. They are not part of the
        // checkpoint closure and must not poison pre-arm capture state.
        // Once the interval is active, however, an unknown destroy is a
        // closure violation and remains fail-closed.
        if (hasRenderTapeDeadObject(object)) {
            markRenderTapeInvalidOnce("object_destroy_duplicate", &object);
            if (IsRenderTapeCaptureActiveForChild()) {
                abortRenderTapeCapture("object_destroy_duplicate");
            }
        }
        return false;
    }
    // The parent texture owns aliased storage, so release-to-zero is a
    // retained state rather than tape retirement. A later wrapper can
    // reacquire the same live identity before the parent retires it.
    if (!it->lifetime.releaseWrapper()) {
        return false;
    }
    // The caller performs the shared ObjectDestroy/tombstone/erase step.
    // Keeping the live entry here makes immediate and pending retirement
    // use the same exact-once ordering.
    return true;
}

bool D3D9DeviceImpl::recordRenderTapeCpuBytes(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::uint32_t subresource, std::uint64_t byteOffset,
    std::span<const std::byte> bytes) noexcept {
    auto *entry = findRenderTapeObject(object);
    if (!entry || object.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
        subresource != 0u || entry->contentCount != 1u ||
        entry->content.size() != 1u || bytes.empty() ||
        entry->descriptor.size() != sizeof(D9CBufferDesc)) {
        markRenderTapeInvalidOnce("mutation_unknown_or_empty", &object,
                                  subresource, {.bytes = bytes.size()});
        return false;
    }
    D9CBufferDesc desc{};
    std::memcpy(&desc, entry->descriptor.data(), sizeof(desc));
    auto &existing = entry->content[subresource];
    const auto status = dxmt9::d3d9::applyRenderTapeBufferMutation(
        desc.size, byteOffset, bytes, existing);
    if (status == dxmt9::d3d9::RenderTapeBlockMutationStatus::Accepted)
        return true;
    if (status ==
        dxmt9::d3d9::RenderTapeBlockMutationStatus::IncompleteSeed) {
        // A partial write before a complete CPU-owned seed is not enough
        // to establish initial contents. Leave it unknown and fail at arm.
        return true;
    }
    markRenderTapeInvalidOnce(
        status == dxmt9::d3d9::RenderTapeBlockMutationStatus::AllocationFailed
            ? "mutation_copy_exception"
            : "mutation_extent",
        &object, subresource,
        {.format = desc.format, .bytes = bytes.size()});
    return false;
}

bool D3D9DeviceImpl::renderTapeObjectSubresourceDesc(
    const RenderTapeLiveObject &entry,
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::uint32_t subresource, D9CSurfaceDesc &out) const noexcept {
    out = {};
    if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        return renderTapeTextureSubresourceDescriptor(
            entry.descriptor, subresource, out);
    }
    if (object.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE ||
        subresource != 0u) {
        return false;
    }
    dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
    if (!dxmt9::d3d9::renderTapeLoadSurfaceDescriptorV2(
            entry.descriptor, surface) ||
        surface.storage != static_cast<std::uint32_t>(
                               dxmt9::d3d9::RenderTapeSurfaceStorage::
                                   Standalone)) {
        return false;
    }
    out = surface.surface;
    return true;
}

dxmt9::d3d9::RenderTapeBlockMutationStatus D3D9DeviceImpl::recordRenderTapeBlockBytes(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::uint32_t subresource,
    const dxmt9::d3d9::RenderTapeBlockLockLayout &layout,
    std::span<const std::byte> bytes) noexcept {
    auto *entry = findRenderTapeObject(object);
    if (!entry || subresource >= entry->content.size()) {
        return dxmt9::d3d9::RenderTapeBlockMutationStatus::InvalidLayout;
    }
    D9CSurfaceDesc desc{};
    if (!renderTapeObjectSubresourceDesc(*entry, object, subresource, desc) ||
        !renderTapeFormatIsBlockCompressed(desc.format)) {
        return dxmt9::d3d9::RenderTapeBlockMutationStatus::InvalidLayout;
    }
    dxmt9::d3d9::RenderTapeBlockLockLayout fullLayout{};
    if (dxmt9::d3d9::renderTapeBlockLockLayout(
            desc, static_cast<std::int32_t>(layout.pitch), nullptr,
            fullLayout) !=
            dxmt9::d3d9::RenderTapeBlockLayoutStatus::Accepted ||
        layout.blockBytes != fullLayout.blockBytes ||
        layout.fullRowBytes != fullLayout.fullRowBytes ||
        layout.fullRows != fullLayout.fullRows) {
        return dxmt9::d3d9::RenderTapeBlockMutationStatus::InvalidLayout;
    }
    return dxmt9::d3d9::applyRenderTapeBlockMutation(
        layout, bytes, entry->content[subresource]);
}

dxmt9::d3d9::RenderTapeBlockMutationStatus D3D9DeviceImpl::recordRenderTapeLinearBytes(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::uint32_t subresource,
    const dxmt9::d3d9::RenderTapeLinearLockLayout &layout,
    std::span<const std::byte> bytes) noexcept {
    auto *entry = findRenderTapeObject(object);
    if (!entry || subresource >= entry->content.size()) {
        return dxmt9::d3d9::RenderTapeBlockMutationStatus::InvalidLayout;
    }
    D9CSurfaceDesc desc{};
    if (!renderTapeObjectSubresourceDesc(*entry, object, subresource, desc) ||
        renderTapeFormatIsBlockCompressed(desc.format)) {
        return dxmt9::d3d9::RenderTapeBlockMutationStatus::InvalidLayout;
    }
    dxmt9::d3d9::RenderTapeLinearLockLayout fullLayout{};
    if (dxmt9::d3d9::renderTapeLinearLockLayout(
            desc, static_cast<std::int32_t>(layout.pitch), nullptr,
            fullLayout) !=
            dxmt9::d3d9::RenderTapeLinearLayoutStatus::Accepted ||
        layout.bytesPerPixel != fullLayout.bytesPerPixel ||
        layout.fullRowBytes != fullLayout.fullRowBytes ||
        layout.fullRows != fullLayout.fullRows) {
        return dxmt9::d3d9::RenderTapeBlockMutationStatus::InvalidLayout;
    }
    return dxmt9::d3d9::applyRenderTapeLinearMutation(
        layout, bytes, entry->content[subresource]);
}

void D3D9DeviceImpl::logRenderTapeMutationRejection(
    const char *reason, const char *detail,
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::uint32_t subresource, std::uint64_t bytes,
    dxmt9::d3d9::RenderTapeCaptureStatus status) noexcept {
    if (!peCaptureState_)
        return;
    const auto &limits = peCaptureState_->renderTapeCapture.limits();
    dxmt9DeviceInfoLog(
        "render_tape_capture mutation_reject reason=%s detail=%s status=%u "
        "capture_state=%u kind=%u generation=%u object_id=%llu "
        "subresource=%u bytes=%llu live_object=%d event_count=%u/%u "
        "buffered_bytes=%llu/%llu owned_blob_bytes=%llu/%llu "
        "owned_blob_entries=%u/%u",
        reason, detail, static_cast<unsigned>(status),
        static_cast<unsigned>(peCaptureState_->renderTapeCapture.state()),
        object.identity.kind, object.identity.generation,
        static_cast<unsigned long long>(object.identity.objectId),
        subresource, static_cast<unsigned long long>(bytes),
        peCaptureState_->renderTapeCapture.hasLiveObject(object.identity) ? 1 : 0,
        peCaptureState_->renderTapeCapture.eventCount(), limits.maxEvents,
        static_cast<unsigned long long>(peCaptureState_->renderTapeCapture.bufferedBytes()),
        static_cast<unsigned long long>(limits.maxEventBytes),
        static_cast<unsigned long long>(
            peCaptureState_->renderTapeCapture.ownedBlobBytes()),
        static_cast<unsigned long long>(limits.maxBlobBytes),
        peCaptureState_->renderTapeCapture.ownedBlobEntries(), limits.maxBlobEntries);
}

bool D3D9DeviceImpl::appendRenderTapeUnlockMutation(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::uint32_t subresource, const char *reason) noexcept {
    const auto *entry = findRenderTapeObject(object);
    if (!entry || subresource >= entry->content.size()) {
        dxmt9DeviceInfoLog(
            "render_tape_capture mutation_reject reason=%s detail=%s kind=%u "
            "generation=%u object_id=%llu subresource=%u content=%zu",
            reason,
            entry ? "subresource_out_of_range" : "registry_entry_missing",
            object.identity.kind, object.identity.generation,
            static_cast<unsigned long long>(object.identity.objectId),
            subresource, entry ? entry->content.size() : 0u);
        return false;
    }
    const auto &bytes = entry->content[subresource];
    dxmt9::d3d9::RenderTapeDigest digest{};
    const auto blobStatus =
        peCaptureState_->renderTapeCapture.registerBlobBytes(bytes, &digest);
    if (blobStatus != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
        logRenderTapeMutationRejection(reason, "blob_register", object,
                                       subresource, bytes.size(),
                                       blobStatus);
        return false;
    }
    const auto status = peCaptureState_->renderTapeCapture.resourceMutation(
        object.identity, dxmt9::d3d9::RenderTapeMutationKind::CpuUnlock,
        subresource, 0u, bytes.size(),
        std::span<const std::byte, dxmt9::d3d9::kRenderTapeDigestSize>(
            digest));
    if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
        logRenderTapeMutationRejection(reason, "mutation_event", object,
                                       subresource, bytes.size(), status);
        return false;
    }
    return true;
}

bool D3D9DeviceImpl::renderTapeObjectAdmitted(
    const D9CWireObjectIdentity &identity) const noexcept {
    return peCaptureState_ &&
        dxmt9::d3d9::renderTapeBootstrapClosureContains(
            peCaptureState_->renderTapeAdmittedIdentities, identity);
}

void D3D9DeviceImpl::removeRenderTapeObjectAdmitted(
    const D9CWireObjectIdentity &identity) noexcept {
    if (!peCaptureState_)
        return;
    const auto it = std::find_if(
        peCaptureState_->renderTapeAdmittedIdentities.begin(),
        peCaptureState_->renderTapeAdmittedIdentities.end(), [&](const auto &candidate) {
            return renderTapeSameIdentity(candidate, identity);
        });
    if (it != peCaptureState_->renderTapeAdmittedIdentities.end())
        peCaptureState_->renderTapeAdmittedIdentities.erase(it);
}

bool D3D9DeviceImpl::materializeRenderTapeObjectForReference(
    const D9CWireObjectIdentity &identity,
    std::uint32_t handleIndex,
    std::uint32_t recordIndex,
    std::uint32_t recordType,
    const dxmt9::d3d9::RenderTapeOriginLocator *originLocator,
    const dxmt9::d3d9::ImportedChunkView *currentChunk) noexcept {
    if (!peCaptureState_)
        return false;
    if (renderTapeObjectAdmitted(identity))
        return true;
    if (peCaptureState_->renderTapeCapture.state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        return false;
    }
    const auto reject = [&](dxmt9::d3d9::RenderTapeCaptureRejectionReason reason,
                            std::uint32_t subresource =
                                std::numeric_limits<std::uint32_t>::max()) {
        const dxmt9::d3d9::pe::PeWireObjectRef object{.identity = identity};
        RejectRenderTapeCaptureForChild(reason, object, subresource, {});
        return false;
    };
    const auto object = std::find_if(
        peCaptureState_->renderTapeRegistry.objects.begin(),
        peCaptureState_->renderTapeRegistry.objects.end(), [&](const auto &candidate) {
            return renderTapeSameIdentity(candidate.identity, identity);
        });
    if (object == peCaptureState_->renderTapeRegistry.objects.end()) {
        const dxmt9::d3d9::pe::PeWireObjectRef reference{
            .identity = identity};
        dxmt9DeviceInfoLog(
            "render_tape_capture materialize_miss profile=%u device=%p "
            "registry=%p kind=%u generation=%u object_id=%llu live=0 "
            "pending=0 admitted=%d known_dead=%d handle_index=%u "
            "record_index=%u record_type=%u",
            dxmt9PeRenderTapeCaptureProfile(), this,
            static_cast<void *>(&peCaptureState_->renderTapeRegistry), identity.kind,
            identity.generation,
            static_cast<unsigned long long>(identity.objectId),
            renderTapeObjectAdmitted(identity) ? 1 : 0,
            hasRenderTapeDeadObject(reference) ? 1 : 0, handleIndex,
            recordIndex, recordType);
        return reject(dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                          UnmaterializedPreArmObject);
    }
    if (dxmt9::d3d9::renderTapeBootstrapRequiresAllLiveObjects(
            dxmt9PeRenderTapeCaptureProfile())) {
        // An unadmitted pre-arm identity is impossible after the sequence
        // profile's complete arm snapshot. Reject before emitting an
        // ObjectDefine that the second interval grammar cannot accept.
        return reject(dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                          UnmaterializedPreArmObject);
    }
    const auto armSnapshot = std::find_if(
        peCaptureState_->renderTapeArmSnapshots.begin(), peCaptureState_->renderTapeArmSnapshots.end(),
        [&](const auto &candidate) {
            return renderTapeSameIdentity(candidate.identity, identity);
        });
    const auto armOverlay = dxmt9::d3d9::
        renderTapeSelectArmObjectSnapshotOverlay(
            object->descriptor, object->content,
            armSnapshot != peCaptureState_->renderTapeArmSnapshots.end()
                ? std::span<const std::byte>(armSnapshot->descriptor)
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
                      RenderTapeArmObjectSnapshotOverlayPolicy::PresentOutput
                : dxmt9::d3d9::
                      RenderTapeArmObjectSnapshotOverlayPolicy::Ordinary);
    if (armOverlay.source == dxmt9::d3d9::
            RenderTapeArmSnapshotOverlaySource::StaleArm) {
        abortRenderTapeCapture("jit_stale_arm_snapshot");
        return false;
    }
    const auto effectiveDescriptor = armOverlay.descriptor;
    const auto effectiveContent = armOverlay.content;
    if (object->lifetime.textureAlias &&
        !materializeRenderTapeObjectForReference(
            object->aliasParentTexture, handleIndex, recordIndex,
            recordType, originLocator, currentChunk)) {
        return false;
    }
    const bool incomplete =
        object->contentCount != effectiveContent.size() ||
        std::any_of(effectiveContent.begin(), effectiveContent.end(),
                    [](const auto &bytes) { return bytes.empty(); });
    const auto firstMissing = std::find_if(
        effectiveContent.begin(), effectiveContent.end(),
        [](const auto &bytes) { return bytes.empty(); });
    const auto firstMissingSubresource = static_cast<std::uint32_t>(
        firstMissing - effectiveContent.begin());
    bool producedByCapturedPass = false;
    if (incomplete && object->lifetime.textureAlias) {
        // A texture-derived surface owns no independent seed. Its parent
        // was resolved above; preserve the alias descriptor as Unavailable.
    } else if (incomplete && currentChunk && originLocator) {
        RenderTapeTextureDescriptorV2 texture{};
        dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 alias{};
        const auto aliasObject = std::find_if(
            peCaptureState_->renderTapeRegistry.objects.begin(),
            peCaptureState_->renderTapeRegistry.objects.end(), [&](const auto &candidate) {
                return renderTapeSameIdentity(
                    candidate.identity, originLocator->originIdentity);
            });
        const bool exactTexture =
            renderTapeLoadTextureDescriptorV2(effectiveDescriptor, texture) &&
            dxmt9::d3d9::renderTapeProducedTextureShapeSupported(texture) &&
            aliasObject != peCaptureState_->renderTapeRegistry.objects.end() &&
            aliasObject->lifetime.textureAlias &&
            renderTapeSameIdentity(aliasObject->aliasParentTexture,
                                   object->identity) &&
            renderTapeLoadSurfaceDescriptorV2(aliasObject->descriptor,
                                               alias) &&
            dxmt9::d3d9::renderTapeSurfaceAliasMatchesTextureSubresource(
                effectiveDescriptor, object->identity, alias) &&
            alias.subresource < effectiveContent.size() &&
            effectiveContent[alias.subresource].empty() &&
            ((texture.dimension == static_cast<std::uint32_t>(
                  RenderTapeTextureDimension::Texture2D) &&
              alias.subresource == firstMissingSubresource &&
              alias.subresource == 0u) ||
             (texture.dimension == static_cast<std::uint32_t>(
                  RenderTapeTextureDimension::Cube) &&
              alias.subresource < 6u));
        dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
        const bool exactStandaloneSurface =
            renderTapeLoadSurfaceDescriptorV2(effectiveDescriptor, surface) &&
            surface.storage == static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone) &&
            dxmt9::d3d9::renderTapeProducedStandaloneSurfaceSupported(
                surface.surface);
        producedByCapturedPass =
            (exactTexture &&
             dxmt9::d3d9::renderTapeProveProducedByCapturedPass(
                 *currentChunk, originLocator->originIdentity,
                 object->identity)) ||
            (exactStandaloneSurface &&
             dxmt9::d3d9::
                 renderTapeClassifyProducedStandaloneSurfaceByCapturedPass(
                     *currentChunk, object->identity, surface.surface)
                     .accepted());
    } else if (incomplete) {
        // Keep the diagnostic index identical to the existing rejection
        // below: a count mismatch with no empty slot reports content.size().
        const auto missingSubresource = static_cast<std::uint32_t>(
            firstMissing - object->content.begin());
        dxmt9::d3d9::RenderTapeOriginLocator locator{};
        locator.originIdentity = identity;
        locator.resolvedIdentity = identity;
        locator.recordIndex = recordIndex;
        locator.recordType = recordType;
        locator.handleIndex = handleIndex;
        if (originLocator) {
            locator = *originLocator;
            locator.resolvedIdentity = identity;
            locator.aliasOrigin =
                !dxmt9::d3d9::renderTapeSameWireObject(
                    locator.originIdentity, identity);
        }
        const auto missingSeed = dxmt9::d3d9::renderTapeDescribeMissingSeed(
            object->identity, effectiveDescriptor, missingSubresource,
            dxmt9::d3d9::RenderTapeReferenceProvenance{
                .handleIndex = locator.handleIndex,
                .recordIndex = locator.recordIndex,
                .recordType = locator.recordType,
            });
        dxmt9::d3d9::renderTapeFirstAccessArm(
            peCaptureState_->renderTapeFirstAccessLedger, locator.originIdentity,
            object->identity);
        dxmt9DeviceInfoLog(
            "render_tape_capture missing_seed identity_kind=%u "
            "generation=%u object_id=%llu descriptor_status=%s "
            "expected_status=%s texture_dimension=%u mip_levels=%u "
            "subresources=%u missing_subresource=%u usage=%u "
            "resource_type=%u pool=%u format=%u width=%u height=%u "
            "multisample_type=%u multisample_quality=%u "
            "expected_tight_bytes=%llu expected_tight_bytes_valid=%d "
            "handle_index=%u record_index=%u record_type=%u "
            "origin_kind=%u origin_generation=%u origin_object_id=%llu "
            "resolved_kind=%u resolved_generation=%u "
            "resolved_object_id=%llu section_kind=%u binding_slot=%u "
            "alias_origin=%d command_role=%s storage_role=%s "
            "locator_status=%s",
            missingSeed.identity.kind, missingSeed.identity.generation,
            static_cast<unsigned long long>(missingSeed.identity.objectId),
            dxmt9::d3d9::renderTapeMissingSeedDescriptorStatusName(
                missingSeed.descriptorStatus),
            dxmt9::d3d9::renderTapeExpectedContentStatusName(
                missingSeed.expectedContentStatus),
            static_cast<unsigned>(missingSeed.textureDimension),
            missingSeed.mipLevelCount, missingSeed.subresourceCount,
            missingSeed.missingSubresource, missingSeed.missingSurface.usage,
            missingSeed.missingSurface.resourceType,
            missingSeed.missingSurface.pool, missingSeed.missingSurface.format,
            missingSeed.missingSurface.width, missingSeed.missingSurface.height,
            missingSeed.missingSurface.multiSampleType,
            missingSeed.missingSurface.multiSampleQuality,
            static_cast<unsigned long long>(missingSeed.expectedTightBytes),
            missingSeed.expectedTightBytesValid ? 1 : 0,
            missingSeed.provenance.handleIndex,
            missingSeed.provenance.recordIndex,
            missingSeed.provenance.recordType, locator.originIdentity.kind,
            locator.originIdentity.generation,
            static_cast<unsigned long long>(locator.originIdentity.objectId),
            locator.resolvedIdentity.kind, locator.resolvedIdentity.generation,
            static_cast<unsigned long long>(locator.resolvedIdentity.objectId),
            locator.sectionKind, locator.bindingSlot,
            locator.aliasOrigin ? 1 : 0,
            dxmt9::d3d9::renderTapeCommandRoleName(locator.role),
            dxmt9::d3d9::renderTapeStorageRoleName(locator.storageRole),
            dxmt9::d3d9::renderTapeOriginLocatorStatusName(
                locator.status));
        if (recordType == D9C_COMMAND_RECORD_UPDATE_TEXTURE &&
            !renderTapeObjectAdmitted(identity)) {
            // UpdateTexture's destination initial bytes must precede the
            // command. Do not poison the long-lived registry when that
            // proof is absent; the post-append closure can retain a
            // complete source copy for the next arm attempt.
            abortRenderTapeCapture("update_texture_destination_unadmitted");
            return false;
        }
        return reject(
            dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                IncompleteSubresourceSeed,
            firstMissingSubresource);
    }
    dxmt9::d3d9::RenderTapeExpectedContentContract contentContract{};
    if (!producedByCapturedPass && !object->lifetime.textureAlias &&
        !renderTapeValidateExpectedContent(
            object->identity, effectiveDescriptor, effectiveContent,
            contentContract)) {
        dxmt9DeviceInfoLog(
            "render_tape_capture materialize rejected reason=expected_content_contract "
            "status=%s kind=%u generation=%u object_id=%llu expected_bytes=%llu "
            "expected_count=%u actual_count=%zu",
            dxmt9::d3d9::renderTapeExpectedContentStatusName(
                contentContract.status),
            object->identity.kind, object->identity.generation,
            static_cast<unsigned long long>(object->identity.objectId),
            static_cast<unsigned long long>(contentContract.bytes),
            contentContract.count, effectiveContent.size());
        return reject(dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                          ExpectedContentContract);
    }
    dxmt9::d3d9::RenderTapeDigest immutableDigest{};
    std::uint64_t immutableBytes = 0u;
    if (!object->immutablePayload.empty()) {
        if (peCaptureState_->renderTapeCapture.registerBlobBytes(object->immutablePayload,
                                                  &immutableDigest) !=
            dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            abortRenderTapeCapture("jit_object_define_blob");
            return false;
        }
        immutableBytes = object->immutablePayload.size();
    }
    const auto descriptorKind = static_cast<std::uint32_t>(
        dxmt9::d3d9::renderTapeDescriptorKindForObject(identity.kind));
    std::vector<std::byte> descriptor(effectiveDescriptor.begin(),
                                      effectiveDescriptor.end());
    if (producedByCapturedPass) {
        if (identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
            RenderTapeTextureDescriptorV2 texture{};
            if (!renderTapeLoadTextureDescriptorV2(descriptor, texture) ||
                texture.initialContentDisposition !=
                    static_cast<std::uint32_t>(
                        RenderTapeInitialContentDisposition::CompleteSeed)) {
                return reject(dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                                   DescriptorMismatch);
            }
            texture.initialContentDisposition = static_cast<std::uint32_t>(
                RenderTapeInitialContentDisposition::ProducedByCapturedPass);
            std::memcpy(descriptor.data(), &texture, sizeof(texture));
        } else {
            dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
            if (!renderTapeLoadSurfaceDescriptorV2(descriptor, surface) ||
                surface.storage != static_cast<std::uint32_t>(
                    dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone) ||
                !dxmt9::d3d9::renderTapeProducedStandaloneSurfaceSupported(
                    surface.surface)) {
                return reject(dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                                   DescriptorMismatch);
            }
            surface.initialContentDisposition = static_cast<std::uint32_t>(
                RenderTapeInitialContentDisposition::ProducedByCapturedPass);
            std::memcpy(descriptor.data(), &surface, sizeof(surface));
        }
    }
    if (peCaptureState_->renderTapeCapture.objectDefine(
            identity, descriptorKind, descriptor, immutableBytes,
            immutableDigest,
            producedByCapturedPass || object->lifetime.textureAlias
                ? 0u
                : contentContract.bytes,
            producedByCapturedPass || object->lifetime.textureAlias
                ? 0u
                : contentContract.count) !=
        dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
        abortRenderTapeCapture("jit_object_define");
        return false;
    }
    if (producedByCapturedPass || object->lifetime.textureAlias) {
        try {
            peCaptureState_->renderTapeAdmittedIdentities.push_back(identity);
        } catch (...) {
            abortRenderTapeCapture("jit_identity_allocation");
            return false;
        }
        return true;
    }
    for (std::uint32_t subresource = 0u;
         subresource < effectiveContent.size(); ++subresource) {
        if (peCaptureState_->renderTapeCapture.resourceMutationBytes(
                identity, dxmt9::d3d9::RenderTapeMutationKind::Upload,
                subresource, 0u, effectiveContent[subresource]) !=
            dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            abortRenderTapeCapture("jit_resource_mutation");
            return false;
        }
    }
    try {
        peCaptureState_->renderTapeAdmittedIdentities.push_back(identity);
    } catch (...) {
        abortRenderTapeCapture("jit_identity_allocation");
        return false;
    }
    return true;
}

bool D3D9DeviceImpl::admitRenderTapeChunkHandles(
    const D9CCommandChunk &chunk,
    const PeCommandChunkCommitInfo &info) noexcept {
    (void)info;
    if (chunk.recordBytes < sizeof(D9CCommandChunkWireHeader) ||
        d9cWireHandleValue(chunk.records) == 0u) {
        abortRenderTapeCapture("command_chunk_invalid");
        return false;
    }
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(static_cast<std::uintptr_t>(
            d9cWireHandleValue(chunk.records))),
        chunk.recordBytes);
    dxmt9::d3d9::ImportedChunkView imported{};
    dxmt9::d3d9::CommandChunkValidationScratch scratch{};
    const auto validation = dxmt9::d3d9::validateCommandChunk(
        bytes, dxmt9::d3d9::CommandChunkEnvelope{
                   .version = chunk.version,
                   .recordCount = info.recordCount,
                   .handleCount = info.handleCount},
        &imported, scratch);
    if (!validation.valid()) {
        abortRenderTapeCapture("command_chunk_validation");
        return false;
    }
    struct PreflightAttribution {
        dxmt9::d3d9::RenderTapeCommandAdmissionResult admission{};
        dxmt9::d3d9::RenderTapeOriginLocator locator{};
        dxmt9::d3d9::RenderTapeProducedProofResult producedProof{};
        dxmt9::d3d9::RenderTapeFirstAccessObservation firstAccess{};
        dxmt9::d3d9::RenderTapeMissingSeedDescriptor missingSeed{};
        D9CWireObjectIdentity resolvedIdentity{};
        D9CWireObjectIdentity dependencyIdentity{};
        dxmt9::d3d9::RenderTapeCommandAdmissionStatus
            dependencyAdmissionStatus =
                dxmt9::d3d9::RenderTapeCommandAdmissionStatus::OriginRejected;
        dxmt9::d3d9::RenderTapeProducedProofStatus
            dependencyProducedProofStatus =
                dxmt9::d3d9::RenderTapeProducedProofStatus::NoTerminalAccess;
        dxmt9::d3d9::RenderTapeFirstAccessStatus
            dependencyFirstAccessStatus =
                dxmt9::d3d9::RenderTapeFirstAccessStatus::Idle;
        bool hasDependency = false;
        bool registryPresent = false;
        bool admitted = false;
        bool live = false;
        bool dead = false;
        bool contentComplete = false;
        bool armSnapshotPresent = false;
        bool armSnapshotCurrent = false;
        bool textureAlias = false;
        bool producedDescriptorSupported = false;
        bool producedAliasPresent = false;
        bool producedAliasDescriptorAccepted = false;
        bool producedAliasParentMatched = false;
        bool producedAliasShapeMatched = false;
        std::uint32_t producedAliasSubresource =
            std::numeric_limits<std::uint32_t>::max();
        D9CSurfaceDesc producedAliasSurface{};
        std::size_t descriptorBytes = 0u;
        std::uint32_t expectedContentCount = 0u;
        std::size_t actualContentCount = 0u;
    };
    const auto canMaterialize = [&](const auto &self,
                                    const D9CWireObjectIdentity &identity,
                                    const dxmt9::d3d9::RenderTapeOriginLocator
                                        &originLocator,
                                    std::optional<D9CWireObjectIdentity>
                                        &producedIdentity)
        -> PreflightAttribution {
        PreflightAttribution attribution{};
        attribution.locator = originLocator;
        attribution.locator.resolvedIdentity = identity;
        attribution.locator.aliasOrigin = !renderTapeSameIdentity(
            originLocator.originIdentity, identity);
        attribution.resolvedIdentity = identity;
        attribution.registryPresent = peCaptureState_ != nullptr;
        attribution.admitted = renderTapeObjectAdmitted(identity);
        if (attribution.admitted) {
            attribution.admission =
                dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                    .originAccepted = originLocator.status ==
                        dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                    .registryPresent = attribution.registryPresent,
                    .admitted = true,
                });
            return attribution;
        }
        if (!peCaptureState_) {
            attribution.admission =
                dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                    .originAccepted = originLocator.status ==
                        dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                });
            return attribution;
        }
        const auto object = std::find_if(
            peCaptureState_->renderTapeRegistry.objects.begin(),
            peCaptureState_->renderTapeRegistry.objects.end(), [&](const auto &candidate) {
                return renderTapeSameIdentity(candidate.identity, identity);
            });
        attribution.live = object != peCaptureState_->renderTapeRegistry.objects.end();
        if (!attribution.live) {
            const dxmt9::d3d9::pe::PeWireObjectRef reference{
                .identity = identity};
            attribution.dead = hasRenderTapeDeadObject(reference);
            attribution.admission =
                dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                    .originAccepted = originLocator.status ==
                        dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                    .registryPresent = true,
                    .deadObject = attribution.dead,
                });
            return attribution;
        }
        attribution.textureAlias = object->lifetime.textureAlias;
        const auto armSnapshot = std::find_if(
            peCaptureState_->renderTapeArmSnapshots.begin(), peCaptureState_->renderTapeArmSnapshots.end(),
            [&](const auto &candidate) {
                return renderTapeSameIdentity(candidate.identity, identity);
            });
        attribution.armSnapshotPresent =
            armSnapshot != peCaptureState_->renderTapeArmSnapshots.end();
        const auto armOverlay = dxmt9::d3d9::
            renderTapeSelectArmObjectSnapshotOverlay(
                object->descriptor, object->content,
                attribution.armSnapshotPresent
                    ? std::span<const std::byte>(armSnapshot->descriptor)
                    : std::span<const std::byte>{},
                attribution.armSnapshotPresent
                    ? std::span<const std::vector<std::byte>>(
                          armSnapshot->content)
                    : std::span<const std::vector<std::byte>>{},
                attribution.armSnapshotPresent
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
        attribution.armSnapshotCurrent = armOverlay.source == dxmt9::d3d9::
            RenderTapeArmSnapshotOverlaySource::CurrentArm;
        attribution.descriptorBytes = armOverlay.descriptor.size();
        attribution.expectedContentCount = object->contentCount;
        attribution.actualContentCount = armOverlay.content.size();
        if (armOverlay.source == dxmt9::d3d9::
                RenderTapeArmSnapshotOverlaySource::StaleArm) {
            attribution.admission =
                dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                    .originAccepted = originLocator.status ==
                        dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                    .registryPresent = true,
                    .liveObject = true,
                });
            return attribution;
        }
        const auto effectiveDescriptor = armOverlay.descriptor;
        const auto effectiveContent = armOverlay.content;
        if (attribution.textureAlias) {
            auto dependency = self(self, object->aliasParentTexture,
                                   originLocator, producedIdentity);
            if (!dependency.admission.accepted()) {
                dependency.hasDependency = true;
                dependency.dependencyIdentity = object->aliasParentTexture;
                dependency.dependencyAdmissionStatus =
                    dependency.admission.status;
                dependency.dependencyProducedProofStatus =
                    dependency.producedProof.status;
                dependency.dependencyFirstAccessStatus =
                    dependency.firstAccess.status;
                dependency.admission =
                    dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                        .originAccepted = originLocator.status ==
                            dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                        .registryPresent = true,
                        .liveObject = true,
                        .aliasDependencyAccepted = false,
                        .textureAlias = true,
                    });
                return dependency;
            }
        }
        const bool incomplete =
            object->contentCount != effectiveContent.size() ||
            std::any_of(effectiveContent.begin(), effectiveContent.end(),
                        [](const auto &bytes) { return bytes.empty(); });
        attribution.contentComplete = !incomplete;
        if (!incomplete || attribution.textureAlias) {
            attribution.admission =
                dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                    .originAccepted = originLocator.status ==
                        dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                    .registryPresent = true,
                    .liveObject = true,
                    .aliasDependencyAccepted = true,
                    .contentComplete = !incomplete,
                    .textureAlias = attribution.textureAlias,
                });
            return attribution;
        }
        const auto missing = std::find_if(
            effectiveContent.begin(), effectiveContent.end(),
            [](const auto &bytes) { return bytes.empty(); });
        const auto missingSubresource = static_cast<std::uint32_t>(
            missing - effectiveContent.begin());
        attribution.missingSeed = dxmt9::d3d9::renderTapeDescribeMissingSeed(
            identity, effectiveDescriptor, missingSubresource,
            {.handleIndex = originLocator.handleIndex,
             .recordIndex = originLocator.recordIndex,
             .recordType = originLocator.recordType});
        RenderTapeTextureDescriptorV2 texture{};
        dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 surface{};
        dxmt9::d3d9::RenderTapeSurfaceDescriptorV2 alias{};
        const auto aliasObject = std::find_if(
            peCaptureState_->renderTapeRegistry.objects.begin(),
            peCaptureState_->renderTapeRegistry.objects.end(), [&](const auto &candidate) {
                return renderTapeSameIdentity(
                    candidate.identity, originLocator.originIdentity);
            });
        attribution.producedAliasPresent =
            aliasObject != peCaptureState_->renderTapeRegistry.objects.end();
        attribution.producedAliasDescriptorAccepted =
            attribution.producedAliasPresent &&
            renderTapeLoadSurfaceDescriptorV2(aliasObject->descriptor,
                                               alias);
        if (attribution.producedAliasDescriptorAccepted) {
            attribution.producedAliasSubresource = alias.subresource;
            attribution.producedAliasSurface = alias.surface;
            attribution.producedAliasParentMatched =
                renderTapeSameIdentity(alias.parentTexture,
                                       object->identity);
            attribution.producedAliasShapeMatched = dxmt9::d3d9::
                renderTapeSurfaceAliasMatchesTextureSubresource(
                    effectiveDescriptor, object->identity, alias);
        }
        const bool producedTexture =
            renderTapeLoadTextureDescriptorV2(effectiveDescriptor, texture) &&
            dxmt9::d3d9::renderTapeProducedTextureShapeSupported(texture) &&
            attribution.producedAliasPresent &&
            aliasObject->lifetime.textureAlias &&
            renderTapeSameIdentity(aliasObject->aliasParentTexture,
                                   object->identity) &&
            attribution.producedAliasDescriptorAccepted &&
            attribution.producedAliasShapeMatched &&
            alias.subresource < effectiveContent.size() &&
            effectiveContent[alias.subresource].empty() &&
            ((texture.dimension == static_cast<std::uint32_t>(
                  RenderTapeTextureDimension::Texture2D) &&
              alias.subresource == missingSubresource &&
              alias.subresource == 0u) ||
             (texture.dimension == static_cast<std::uint32_t>(
                  RenderTapeTextureDimension::Cube) &&
              alias.subresource < 6u));
        const bool producedStandaloneSurface =
            renderTapeLoadSurfaceDescriptorV2(effectiveDescriptor, surface) &&
            surface.storage == static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeSurfaceStorage::Standalone) &&
            dxmt9::d3d9::renderTapeProducedStandaloneSurfaceSupported(
                surface.surface);
        attribution.producedDescriptorSupported =
            producedTexture || producedStandaloneSurface;
        if (producedTexture) {
            attribution.producedProof = dxmt9::d3d9::
                renderTapeClassifyProducedByCapturedPass(
                    imported, originLocator.originIdentity, identity);
            attribution.firstAccess =
                attribution.producedProof.observation;
        } else if (producedStandaloneSurface) {
            attribution.producedProof = dxmt9::d3d9::
                renderTapeClassifyProducedStandaloneSurfaceByCapturedPass(
                    imported, identity, surface.surface);
            attribution.firstAccess = attribution.producedProof.observation;
        } else {
            // Diagnostic-only generic observation: unlike the production
            // ProducedByCapturedPass grammar, this deliberately permits a
            // direct standalone surface identity so r29 can prove whether
            // the D24X8 binding is overwritten before any read.
            dxmt9::d3d9::RenderTapeFirstAccessLedger ledger{};
            dxmt9::d3d9::renderTapeFirstAccessArm(
                ledger, originLocator.originIdentity, identity);
            attribution.firstAccess =
                dxmt9::d3d9::renderTapeFirstAccessObserve(ledger, imported);
        }
        attribution.admission =
            dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                .originAccepted = originLocator.status ==
                    dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                .registryPresent = true,
                .liveObject = true,
                .aliasDependencyAccepted = true,
                .contentComplete = false,
                .textureAlias = false,
                .producedDescriptorSupported =
                    attribution.producedDescriptorSupported,
                .producedProofAccepted =
                    attribution.producedProof.accepted(),
            });
        if (attribution.admission.accepted())
            producedIdentity = identity;
        return attribution;
    };
    std::optional<D9CWireObjectIdentity> producedIdentity;
    for (std::size_t handleIndex = 0u;
         handleIndex < imported.handles.size(); ++handleIndex) {
        const auto &handle = imported.handles[handleIndex];
        const D9CWireObjectIdentity identity{
            .kind = handle.kind,
            .generation = handle.generation,
            .objectId = handle.objectId};
        const auto originLocator = dxmt9::d3d9::renderTapeLocateOrigin(
            imported, static_cast<std::uint32_t>(handleIndex), identity);
        const auto attribution = canMaterialize(
            canMaterialize, identity, originLocator, producedIdentity);
        if (!attribution.admission.accepted()) {
            const auto &locator = attribution.locator;
            const auto &missing = attribution.missingSeed;
            const auto &observation = attribution.firstAccess;
            dxmt9DeviceInfoLog(
                "render_tape_capture command_chunk_preflight "
                "status=%s handle_index=%u record_index=%u record_type=%u "
                "section_kind=%u binding_slot=%u command_role=%s "
                "storage_role=%s locator_status=%s origin_kind=%u "
                "origin_generation=%u origin_object_id=%llu "
                "resolved_kind=%u resolved_generation=%u "
                "resolved_object_id=%llu alias_origin=%d registry=%d "
                "admitted=%d live=%d dead=%d content_complete=%d "
                "arm_snapshot_present=%d arm_snapshot_current=%d "
                "content_expected=%u content_actual=%zu descriptor_bytes=%zu "
                "descriptor_status=%s expected_status=%s dimension=%u "
                "mips=%u subresources=%u missing_subresource=%u format=%u "
                "width=%u height=%u multisample_type=%u usage=%u "
                "produced_descriptor=%d produced_proof=%s "
                "first_access_status=%s first_access_class=%s "
                "produced_alias_present=%d produced_alias_descriptor=%d "
                "produced_alias_parent=%d produced_alias_shape=%d "
                "produced_alias_subresource=%u produced_alias_format=%u "
                "produced_alias_width=%u produced_alias_height=%u "
                "produced_alias_usage=%u "
                "dependency_present=%d dependency_kind=%u "
                "dependency_generation=%u dependency_object_id=%llu "
                "dependency_status=%s dependency_produced_proof=%s "
                "dependency_first_access_status=%s "
                "produced_candidate_present=%d produced_candidate_kind=%u "
                "produced_candidate_generation=%u "
                "produced_candidate_object_id=%llu",
                dxmt9::d3d9::renderTapeCommandAdmissionStatusName(
                    attribution.admission.status),
                locator.handleIndex, locator.recordIndex,
                locator.recordType, locator.sectionKind,
                locator.bindingSlot,
                dxmt9::d3d9::renderTapeCommandRoleName(locator.role),
                dxmt9::d3d9::renderTapeStorageRoleName(locator.storageRole),
                dxmt9::d3d9::renderTapeOriginLocatorStatusName(locator.status),
                locator.originIdentity.kind,
                locator.originIdentity.generation,
                static_cast<unsigned long long>(
                    locator.originIdentity.objectId),
                attribution.resolvedIdentity.kind,
                attribution.resolvedIdentity.generation,
                static_cast<unsigned long long>(
                    attribution.resolvedIdentity.objectId),
                locator.aliasOrigin ? 1 : 0,
                attribution.registryPresent ? 1 : 0,
                attribution.admitted ? 1 : 0,
                attribution.live ? 1 : 0,
                attribution.dead ? 1 : 0,
                attribution.contentComplete ? 1 : 0,
                attribution.armSnapshotPresent ? 1 : 0,
                attribution.armSnapshotCurrent ? 1 : 0,
                attribution.expectedContentCount,
                attribution.actualContentCount,
                attribution.descriptorBytes,
                dxmt9::d3d9::renderTapeMissingSeedDescriptorStatusName(
                    missing.descriptorStatus),
                dxmt9::d3d9::renderTapeExpectedContentStatusName(
                    missing.expectedContentStatus),
                static_cast<unsigned>(missing.textureDimension),
                missing.mipLevelCount, missing.subresourceCount,
                missing.missingSubresource, missing.missingSurface.format,
                missing.missingSurface.width, missing.missingSurface.height,
                missing.missingSurface.multiSampleType,
                missing.missingSurface.usage,
                attribution.producedDescriptorSupported ? 1 : 0,
                dxmt9::d3d9::renderTapeProducedProofStatusName(
                    attribution.producedProof.status),
                dxmt9::d3d9::renderTapeFirstAccessStatusName(
                    observation.status),
                dxmt9::d3d9::renderTapeFirstAccessClassName(
                    observation.classification),
                attribution.producedAliasPresent ? 1 : 0,
                attribution.producedAliasDescriptorAccepted ? 1 : 0,
                attribution.producedAliasParentMatched ? 1 : 0,
                attribution.producedAliasShapeMatched ? 1 : 0,
                attribution.producedAliasSubresource,
                attribution.producedAliasSurface.format,
                attribution.producedAliasSurface.width,
                attribution.producedAliasSurface.height,
                attribution.producedAliasSurface.usage,
                attribution.hasDependency ? 1 : 0,
                attribution.dependencyIdentity.kind,
                attribution.dependencyIdentity.generation,
                static_cast<unsigned long long>(
                    attribution.dependencyIdentity.objectId),
                dxmt9::d3d9::renderTapeCommandAdmissionStatusName(
                    attribution.dependencyAdmissionStatus),
                dxmt9::d3d9::renderTapeProducedProofStatusName(
                    attribution.dependencyProducedProofStatus),
                dxmt9::d3d9::renderTapeFirstAccessStatusName(
                    attribution.dependencyFirstAccessStatus),
                producedIdentity ? 1 : 0,
                producedIdentity ? producedIdentity->kind : 0u,
                producedIdentity ? producedIdentity->generation : 0u,
                static_cast<unsigned long long>(
                    producedIdentity ? producedIdentity->objectId : 0u));
            abortRenderTapeCapture("command_chunk_produced_pass_preflight");
            return false;
        }
    }
    for (std::size_t handleIndex = 0u;
         handleIndex < imported.handles.size(); ++handleIndex) {
        const auto &handle = imported.handles[handleIndex];
        const D9CWireObjectIdentity identity{
            .kind = handle.kind,
            .generation = handle.generation,
            .objectId = handle.objectId};
        const auto originLocator = dxmt9::d3d9::renderTapeLocateOrigin(
            imported, static_cast<std::uint32_t>(handleIndex), identity);
        if (!materializeRenderTapeObjectForReference(
            identity, originLocator.handleIndex,
            originLocator.recordIndex, originLocator.recordType,
            &originLocator, &imported)) {
            return false;
        }
    }
    return true;
}

void D3D9DeviceImpl::observeRenderTapeFirstAccessChunk(
    const D9CCommandChunk &chunk,
    const PeCommandChunkCommitInfo &info) noexcept {
    if (!peCaptureState_ ||
        !peCaptureState_->renderTapeFirstAccessLedger.armed ||
        chunk.recordBytes < sizeof(D9CCommandChunkWireHeader) ||
        d9cWireHandleValue(chunk.records) == 0u) {
        return;
    }
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(static_cast<std::uintptr_t>(
            d9cWireHandleValue(chunk.records))),
        chunk.recordBytes);
    dxmt9::d3d9::ImportedChunkView imported{};
    dxmt9::d3d9::CommandChunkValidationScratch scratch{};
    const auto validation = dxmt9::d3d9::validateCommandChunk(
        bytes, dxmt9::d3d9::CommandChunkEnvelope{
                   .version = chunk.version,
                   .recordCount = info.recordCount,
                   .handleCount = info.handleCount},
        &imported, scratch);
    if (!validation.valid()) {
        if (!peCaptureState_->renderTapeFirstAccessLedger.terminal) {
            peCaptureState_->renderTapeFirstAccessLedger.terminal = true;
            dxmt9DeviceInfoLog(
                "render_tape_capture first_access status=malformed "
                "class=unknown reason=chunk_validation origin_kind=%u "
                "origin_generation=%u origin_object_id=%llu "
                "resolved_kind=%u resolved_generation=%u "
                "resolved_object_id=%llu",
                peCaptureState_->renderTapeFirstAccessLedger.originIdentity.kind,
                peCaptureState_->renderTapeFirstAccessLedger.originIdentity.generation,
                static_cast<unsigned long long>(
                    peCaptureState_->renderTapeFirstAccessLedger.originIdentity.objectId),
                peCaptureState_->renderTapeFirstAccessLedger.resolvedIdentity.kind,
                peCaptureState_->renderTapeFirstAccessLedger.resolvedIdentity.generation,
                static_cast<unsigned long long>(
                    peCaptureState_->renderTapeFirstAccessLedger.resolvedIdentity.objectId));
        }
        return;
    }
    const auto observation = dxmt9::d3d9::renderTapeFirstAccessObserve(
        peCaptureState_->renderTapeFirstAccessLedger, imported);
    if (observation.status !=
            dxmt9::d3d9::RenderTapeFirstAccessStatus::Terminal &&
        observation.status !=
            dxmt9::d3d9::RenderTapeFirstAccessStatus::Malformed) {
        return;
    }
    dxmt9DeviceInfoLog(
        "render_tape_capture first_access status=%s class=%s "
        "origin_kind=%u origin_generation=%u origin_object_id=%llu "
        "resolved_kind=%u resolved_generation=%u resolved_object_id=%llu "
        "observed_kind=%u observed_generation=%u observed_object_id=%llu "
        "record_index=%u record_type=%u handle_index=%u section_kind=%u "
        "binding_slot=%u alias_origin=%d",
        dxmt9::d3d9::renderTapeFirstAccessStatusName(observation.status),
        dxmt9::d3d9::renderTapeFirstAccessClassName(
            observation.classification),
        observation.originIdentity.kind, observation.originIdentity.generation,
        static_cast<unsigned long long>(observation.originIdentity.objectId),
        observation.resolvedIdentity.kind,
        observation.resolvedIdentity.generation,
        static_cast<unsigned long long>(observation.resolvedIdentity.objectId),
        observation.observedIdentity.kind,
        observation.observedIdentity.generation,
        static_cast<unsigned long long>(observation.observedIdentity.objectId),
        observation.recordIndex, observation.recordType,
        observation.handleIndex, observation.sectionKind,
        observation.bindingSlot, observation.aliasOrigin ? 1 : 0);
}

bool D3D9DeviceImpl::prepareRenderTapeChunkCapture(
    const D9CCommandChunk& chunk,
    const PeCommandChunkCommitInfo& info) noexcept {
    if (!peCaptureState_ ||
        peCaptureState_->renderTapeCapture.state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        return false;
    }
    if (peCaptureState_->renderTapeArmBoundaryPhase ==
            dxmt9::d3d9::RenderTapeArmBoundaryPhase::Armed &&
        !advanceRenderTapeArmBoundary(dxmt9::d3d9::
            RenderTapeArmBoundaryPhase::FirstCapturedChunk)) {
        abortRenderTapeCapture("arm_boundary_order");
        return false;
    }
    if (peCaptureState_->renderTapeArmBoundaryPhase != dxmt9::d3d9::
            RenderTapeArmBoundaryPhase::FirstCapturedChunk) {
        abortRenderTapeCapture("arm_boundary_order");
        return false;
    }
    if (!admitRenderTapeChunkHandles(chunk, info)) {
        // The missing-seed arm can happen while admitting this very
        // chunk. Re-scan it now that the exact target is known.
        observeRenderTapeFirstAccessChunk(chunk, info);
        return false;
    }
    return true;
}

void D3D9DeviceImpl::captureCommittedRenderTapeChunk(
    const D9CCommandChunk& chunk,
    const PeCommandChunkCommitInfo& info) noexcept {
    if (!peCaptureState_ ||
        peCaptureState_->renderTapeCapture.state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        return;
    }
    const auto status = peCaptureState_->renderTapeCapture.commandChunk(
        dxmt9::d3d9::CommandChunkEnvelope{
            .version = chunk.version,
            .recordCount = info.recordCount,
            .handleCount = info.handleCount,
        },
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(
                d9cWireHandleValue(chunk.records))),
            chunk.recordBytes));
    if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
        dxmt9DeviceInfoLog(
            "render_tape_capture command_chunk aborted status=%u records=%u "
            "handles=%u",
            static_cast<unsigned>(status), info.recordCount, info.handleCount);
        abortRenderTapeCapture("command_chunk");
    }
}

void D3D9DeviceImpl::applyRenderTapeUpdateTextureClosure(
    const dxmt9::d3d9::pe::PeWireObjectRef &source,
    const dxmt9::d3d9::pe::PeWireObjectRef &destination) noexcept {
    if (!peCaptureState_ ||
        source.identity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE ||
        destination.identity.kind != D9C_CHUNK_HANDLE_KIND_TEXTURE) {
        return;
    }
    if (renderTapeSameIdentity(source.identity, destination.identity)) {
        if (IsRenderTapeCaptureActiveForChild())
            abortRenderTapeCapture("update_texture_self_copy");
        return;
    }
    auto *sourceEntry = findRenderTapeObject(source);
    auto *destinationEntry = findRenderTapeObject(destination);
    const bool active = IsRenderTapeCaptureActiveForChild();
    if (!sourceEntry || !destinationEntry) {
        if (active)
            abortRenderTapeCapture("update_texture_registry_missing");
        return;
    }
    const bool destinationAdmitted =
        renderTapeObjectAdmitted(destination.identity);
    if (active && !destinationAdmitted) {
        // The destination's pre-copy bytes are not in the session. Keep
        // this interval fail-closed rather than defining it with the
        // post-copy source bytes before the UpdateTexture command.
        abortRenderTapeCapture("update_texture_destination_unadmitted");
    }
    const auto status = dxmt9::d3d9::applyRenderTapeUpdateTextureClosure(
        sourceEntry->descriptor, sourceEntry->content,
        destinationEntry->descriptor, destinationEntry->content);
    if (status == dxmt9::d3d9::RenderTapeUpdateTextureStatus::Accepted)
        return;
    const auto reason =
        status == dxmt9::d3d9::RenderTapeUpdateTextureStatus::IncompleteSource
            ? dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                  IncompleteSubresourceSeed
            : dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                  DescriptorMismatch;
    if (active)
        abortRenderTapeCapture(
            reason == dxmt9::d3d9::RenderTapeCaptureRejectionReason::
                          IncompleteSubresourceSeed
                ? "update_texture_source_incomplete"
                : "update_texture_descriptor_mismatch");
    dxmt9DeviceInfoLog(
        "render_tape_capture update_texture_closure status=%u active=%d "
        "source_generation=%u source_object_id=%llu "
        "destination_generation=%u destination_object_id=%llu",
        static_cast<unsigned>(status), active ? 1 : 0,
        source.identity.generation,
        static_cast<unsigned long long>(source.identity.objectId),
        destination.identity.generation,
        static_cast<unsigned long long>(destination.identity.objectId));
}
