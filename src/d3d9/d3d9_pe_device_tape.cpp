/* src/d3d9/d3d9_pe_device_tape.cpp — D3D9DeviceImpl Render Tape capture.
 *
 * The PE-side Render Tape v2 capture owner: the live value registry, object
 * registration/retirement, arm/finish at the Present boundary, chunk handle
 * admission, bootstrap production, and the *ForChild mutation notifications
 * the child wrappers call through D3D9PeRecorderFlush.
 *
 * All of it is gated on DXMT9_RENDER_TAPE_CAPTURE, default off, and every
 * entry point tests dxmt9PeRenderTapeCaptureEnabled() (a cached bool) or a
 * null renderTapeCapture_ before doing anything.
 *
 * Out-lining the twelve virtual *ForChild overrides here is only safe because
 * d3d9_pe_device.cpp pins an explicit key function; without it the first cold
 * TU to out-line a virtual claims the vtable and the whole COM surface with
 * it. See the comment on FlushPeRecorderForChild in the class header. */

#include "d3d9_pe_device_impl.hpp"

RenderTapeLiveObject *D3D9DeviceImpl::findRenderTapeObject(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
    if (!renderTapeRegistry_) {
        return nullptr;
    }
    const auto it = std::find_if(
        renderTapeRegistry_->objects.begin(),
        renderTapeRegistry_->objects.end(),
        [&](const auto &entry) {
            return renderTapeSameIdentity(entry.identity, object.identity);
        });
    return it == renderTapeRegistry_->objects.end() ? nullptr : &*it;
}

const RenderTapeLiveObject *D3D9DeviceImpl::findRenderTapeObject(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) const noexcept {
    if (!renderTapeRegistry_) {
        return nullptr;
    }
    const auto it = std::find_if(
        renderTapeRegistry_->objects.begin(),
        renderTapeRegistry_->objects.end(),
        [&](const auto &entry) {
            return renderTapeSameIdentity(entry.identity, object.identity);
        });
    return it == renderTapeRegistry_->objects.end() ? nullptr : &*it;
}

bool D3D9DeviceImpl::hasRenderTapeDeadObject(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) const noexcept {
    return renderTapeRegistry_ &&
           std::any_of(renderTapeRegistry_->knownDead.begin(),
                       renderTapeRegistry_->knownDead.end(),
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
    if (!renderTapeRegistry_ || renderTapeRegistry_->invalid) {
        return;
    }
    renderTapeRegistry_->invalid = true;
    renderTapeRegistry_->invalidReason = reason;
    renderTapeRegistry_->invalidSubresource = subresource;
    renderTapeRegistry_->invalidLayout = diagnostic;
    if (object) {
        renderTapeRegistry_->invalidKind = object->identity.kind;
        renderTapeRegistry_->invalidGeneration = object->identity.generation;
        renderTapeRegistry_->invalidObjectId = object->identity.objectId;
    }
}

void D3D9DeviceImpl::abortRenderTapeCapture(const char *reason) noexcept {
    if (!renderTapeCapture_ || !renderTapeCapture_->enabled()) {
        return;
    }
    if (!renderTapeAbortReason_) {
        renderTapeAbortReason_ = reason;
        dxmt9DeviceInfoLog("render_tape_capture first_abort reason=%s",
                           reason);
    }
    renderTapeCapture_->abort();
    renderTapeArmBoundaryPhase_ =
        dxmt9::d3d9::RenderTapeArmBoundaryPhase::Disabled;
    renderTapeArmSnapshots_.clear();
    renderTapeExpectedDigest_.reset();
    renderTapeExpectedPixels_.clear();
    renderTapeExpectedSourcePixels_.clear();
    renderTapeOutputDesc_.reset();
    renderTapeActiveCaptureToken_ = 0u;
}

RenderTapeObjectRegistration D3D9DeviceImpl::registerRenderTapeObject(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::span<const std::byte> descriptor,
    std::span<const std::byte> immutablePayload,
    RenderTapeLiveObject::Role role,
    std::uint32_t replacementRestart) noexcept {
    if (!renderTapeRegistry_) {
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
        if (std::any_of(renderTapeRegistry_->objects.begin(),
                        renderTapeRegistry_->objects.end(),
                        [&](const auto &candidate) {
                            return generationDoesNotAdvance(
                                candidate.identity);
                        }) ||
            std::any_of(renderTapeRegistry_->knownDead.begin(),
                        renderTapeRegistry_->knownDead.end(),
                        generationDoesNotAdvance)) {
            markRenderTapeInvalidOnce("non_monotone_generation", &object);
            if (IsRenderTapeCaptureActiveForChild())
                abortRenderTapeCapture("non_monotone_generation");
            return RenderTapeObjectRegistration::Rejected;
        }
        const auto replacement = std::find_if(
            renderTapeRegistry_->objects.begin(),
            renderTapeRegistry_->objects.end(),
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
        if (replacement != renderTapeRegistry_->objects.end()) {
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
                    static_cast<void *>(&*renderTapeRegistry_),
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
                replacement - renderTapeRegistry_->objects.begin());
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
            auto &prior = renderTapeRegistry_->objects[replacementIndex];
            const bool priorAdmitted = renderTapeObjectAdmitted(prior.identity);
            renderTapeRegistry_->knownDead.reserve(
                renderTapeRegistry_->knownDead.size() + 1u);
            if (priorAdmitted && IsRenderTapeCaptureActiveForChild()) {
                const auto destroyStatus =
                    renderTapeCapture_->objectDestroy(prior.identity);
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
            renderTapeRegistry_->knownDead.push_back(prior.identity);
            prior = std::move(entry);
            return RenderTapeObjectRegistration::New;
        }
        renderTapeRegistry_->objects.push_back(std::move(entry));
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
    if (!renderTapeRegistry_) {
        return;
    }
    // Only a pre-arm or aborted lifecycle may move the role. An armed or
    // capturing interval owns its holder until it terminates.
    if (renderTapeCapture_ &&
        renderTapeCapture_->state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Disabled &&
        renderTapeCapture_->state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Aborted) {
        return;
    }
    auto &role = renderTapeRegistry_->presentOutputRole;
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
        renderTapeRegistry_->presentOutputPriorContentCount;
    auto priorDescriptor =
        std::move(renderTapeRegistry_->presentOutputPriorDescriptor);
    auto priorContent =
        std::move(renderTapeRegistry_->presentOutputPriorContent);
    role = {};
    renderTapeRegistry_->presentOutputPriorDescriptor.clear();
    renderTapeRegistry_->presentOutputPriorContentCount = 0u;
    renderTapeRegistry_->presentOutputPriorContent.clear();
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
        renderTapeRegistry_->knownDead.push_back(identity);
    } catch (...) {
        markRenderTapeInvalidOnce("present_output_tombstone_allocation",
                                  &priorObject);
        return;
    }
    if (releaseAdmissionRef)
        (void)prior->lifetime.releaseWrapper();
    renderTapeRegistry_->objects.erase(
        renderTapeRegistry_->objects.begin() +
        (prior - renderTapeRegistry_->objects.data()));
    dxmt9DeviceInfoLog(
        "render_tape_capture present_output released transition=%s kind=%u "
        "generation=%u object_id=%llu",
        dxmt9::d3d9::renderTapePresentOutputRoleTransitionName(transition),
        identity.kind, identity.generation,
        static_cast<unsigned long long>(identity.objectId));
}

bool D3D9DeviceImpl::admitRenderTapePresentOutput() noexcept {
    if (!renderTapeRegistry_) {
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
    auto *surface = D3D9PeRawSurface(backBuffer);
    D9CWireObjectIdentity identity{};
    D9CWireObjectIdentity rawIdentity{};
    D9CSurfaceDesc descriptor{};
    const auto &cachedWireObject = D3D9PeWireSurface(backBuffer);
    const bool rawIdentityOk =
        dxmt9c_surface_get_wire_identity(surface, &rawIdentity) >= 0;
    identity = rawIdentity;
    const bool identityOk = cachedWireObject.valid(
                                D9C_CHUNK_HANDLE_KIND_SURFACE) &&
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
    renderTapeOutputDesc_ = descriptor;
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
    const bool roleRetained = renderTapeRegistry_->presentOutputRole.held;
    auto *existing = findRenderTapeObject(object);
    const bool captureOwned = existing == nullptr;
    if (existing) {
        if (!roleRetained) {
            renderTapeRegistry_->presentOutputPriorDescriptor =
                existing->descriptor;
            renderTapeRegistry_->presentOutputPriorContentCount =
                existing->contentCount;
            renderTapeRegistry_->presentOutputPriorContent =
                std::move(existing->content);
        }
        existing->role = RenderTapeLiveObject::Role::PresentOutput;
        existing->descriptor.assign(
            reinterpret_cast<const std::byte *>(&outputDescriptor),
            reinterpret_cast<const std::byte *>(&outputDescriptor + 1u));
        existing->contentCount = 0u;
        existing->content.clear();
    } else {
        renderTapeRegistry_->presentOutputPriorDescriptor.clear();
        renderTapeRegistry_->presentOutputPriorContentCount = 0u;
        renderTapeRegistry_->presentOutputPriorContent.clear();
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
    auto &role = renderTapeRegistry_->presentOutputRole;
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
        renderTapeRegistry_->presentOutputRole.captureOwned ? 1 : 0);
    return true;
}

bool D3D9DeviceImpl::unregisterRenderTapeObject(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
    if (!renderTapeRegistry_) {
        return false;
    }
    const auto it = std::find_if(
        renderTapeRegistry_->objects.begin(),
        renderTapeRegistry_->objects.end(),
        [&](const auto &entry) {
            return renderTapeSameIdentity(entry.identity, object.identity);
        });
    if (it == renderTapeRegistry_->objects.end()) {
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

bool D3D9DeviceImpl::produceRenderTapeBootstrap(
    dxmt9::d3d9::RenderTapeCaptureBootstrapSeed &seed) noexcept {
    if (!renderTapeRegistry_) {
        dxmt9DeviceInfoLog("render_tape_capture producer aborted reason=registry_missing");
        return false;
    }
    if (renderTapeRegistry_->invalid) {
        dxmt9DeviceInfoLog(
            "render_tape_capture producer aborted reason=registry_invalid "
            "detail=%s kind=%u generation=%u object_id=%llu subresource=%u "
            "format=%u width=%u height=%u pitch=%d bytes=%llu objects=%zu",
            renderTapeRegistry_->invalidReason
                ? renderTapeRegistry_->invalidReason
                : "unknown",
            renderTapeRegistry_->invalidKind,
            renderTapeRegistry_->invalidGeneration,
            static_cast<unsigned long long>(renderTapeRegistry_->invalidObjectId),
            renderTapeRegistry_->invalidSubresource,
            renderTapeRegistry_->invalidLayout.format,
            renderTapeRegistry_->invalidLayout.width,
            renderTapeRegistry_->invalidLayout.height,
            renderTapeRegistry_->invalidLayout.pitch,
            static_cast<unsigned long long>(
                renderTapeRegistry_->invalidLayout.bytes),
            renderTapeRegistry_->objects.size());
        return false;
    }
    if (!admitRenderTapePresentOutput() || renderTapeRegistry_->invalid) {
        return false;
    }
    const auto findArmSnapshot = [&](const auto &identity) {
        return std::find_if(
            renderTapeArmSnapshots_.begin(), renderTapeArmSnapshots_.end(),
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
        populateBindingView(peBindingView_, true, true);
        const bool snapshotBuilt = dxmt9::d3d9::pe::buildFullSnapshotState(
            peState_, peConsts_, peBindingView_, peSparseScratch_,
            peSparseHeader_, peSparseState_);
        const bool snapshotAppended =
            snapshotBuilt && dxmt9::d3d9::pe::appendApplyState(
                                 builder, peSparseHeader_.flags,
                                 peSparseState_);
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

        std::vector<const RenderTapeLiveObject *> objects;
        objects.reserve(renderTapeRegistry_->objects.size());
        for (const auto &object : renderTapeRegistry_->objects) {
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
                    armSnapshot != renderTapeArmSnapshots_.end()
                        ? std::span<const std::byte>(
                              armSnapshot->descriptor)
                        : std::span<const std::byte>{},
                    armSnapshot != renderTapeArmSnapshots_.end()
                        ? std::span<const std::vector<std::byte>>(
                              armSnapshot->content)
                        : std::span<const std::vector<std::byte>>{},
                    armSnapshot != renderTapeArmSnapshots_.end()
                        ? armSnapshot->armOrdinal
                        : 0u,
                    renderTapeArmSnapshotOrdinal_,
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
                    armSnapshot != renderTapeArmSnapshots_.end()
                        ? std::span<const std::byte>(
                              armSnapshot->descriptor)
                        : std::span<const std::byte>{},
                    armSnapshot != renderTapeArmSnapshots_.end()
                        ? std::span<const std::vector<std::byte>>(
                              armSnapshot->content)
                        : std::span<const std::vector<std::byte>>{},
                    armSnapshot != renderTapeArmSnapshots_.end()
                        ? armSnapshot->armOrdinal
                        : 0u,
                    renderTapeArmSnapshotOrdinal_,
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
        return true;
    } catch (...) {
        dxmt9DeviceInfoLog(
            "render_tape_capture producer aborted reason=exception");
        return false;
    }
}

bool D3D9DeviceImpl::advanceRenderTapeArmBoundary(
    dxmt9::d3d9::RenderTapeArmBoundaryPhase requested) noexcept {
    const auto transition = dxmt9::d3d9::renderTapeAdvanceArmBoundary(
        renderTapeArmBoundaryPhase_, requested);
    if (!transition.accepted) {
        dxmt9DeviceInfoLog(
            "render_tape_capture arm_boundary rejected current=%u requested=%u",
            static_cast<unsigned>(renderTapeArmBoundaryPhase_),
            static_cast<unsigned>(requested));
        return false;
    }
    renderTapeArmBoundaryPhase_ = transition.next;
    return true;
}

bool D3D9DeviceImpl::snapshotRenderTapeResourcesAtArmBoundary() noexcept {
    if (!renderTapeRegistry_ || renderTapeRegistry_->invalid)
        return false;
    try {
        renderTapeArmSnapshots_.clear();
        const auto epoch = dxmt9::d3d9::renderTapeNextArmSnapshotEpoch(
            renderTapeArmSnapshotOrdinal_);
        if (!epoch.valid) {
            return false;
        }
        renderTapeArmSnapshotOrdinal_ = epoch.ordinal;
        for (std::size_t index = 0u;
             index < renderTapeRegistry_->objects.size(); ++index) {
            const auto &object = renderTapeRegistry_->objects[index];
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
            renderTapeArmSnapshots_.push_back(std::move(snapshot));
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
    if (!renderTapeCapture_ ||
        !renderTapeCapture_->enabled() ||
        (renderTapeCapture_->state() !=
             dxmt9::d3d9::RenderTapeCaptureState::Disabled &&
         renderTapeCapture_->state() !=
             dxmt9::d3d9::RenderTapeCaptureState::Aborted)) {
        return false;
    }
    if (renderTapeArmPresentSkipRemaining_ != 0u) {
        --renderTapeArmPresentSkipRemaining_;
        return false;
    }
    // An interval that aborted after arming still holds the role; release
    // it here so a retry starts from exactly one live present output.
    releaseRenderTapePresentOutputRole(nullptr);
    // Keep the first-abort marker sticky only for this arm/interval
    // lifecycle; a retry must get independent attribution.
    renderTapeAbortReason_ = nullptr;
    renderTapeAdmittedIdentities_.clear();
    renderTapeExpectedDigest_.reset();
    renderTapeExpectedPixels_.clear();
    renderTapeExpectedSourcePixels_.clear();
    renderTapeOutputDesc_.reset();
    renderTapeFirstAccessLedger_ = {};
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
            renderTapeCapture_->enabled(), producer, publisher)) {
        dxmt9DeviceInfoLog(
            "render_tape_capture requested without artifact publisher; "
            "capture remains off");
        return false;
    }
    // Both callers reach this point only after the Present bridge call or
    // Present-record chunk commit returned success.
    renderTapeArmBoundaryPhase_ =
        dxmt9::d3d9::RenderTapeArmBoundaryPhase::Disabled;
    if (!advanceRenderTapeArmBoundary(dxmt9::d3d9::
            RenderTapeArmBoundaryPhase::PresentFlushed)) {
        return false;
    }
    // Admit the just-presented swap-chain backbuffer before taking the arm
    // snapshot. The backbuffer is the only capture identity whose role is
    // assigned lazily by the bootstrap producer; without this ordering its
    // actual post-arm bytes cannot be captured as starting content.
    if (!admitRenderTapePresentOutput() || renderTapeRegistry_->invalid) {
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
        armStatus = renderTapeCapture_->armWithBlobs(
            seed.bootstrapOverlay, seed.blobs, seed.gammaRamp);
        if (armStatus == dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            intervalStatus = renderTapeCapture_->beginPresentInterval();
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
        const auto status = renderTapeCapture_->objectDefine(
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
            renderTapeAdmittedIdentities_.push_back(object.identity);
        } catch (...) {
            abortRenderTapeCapture("seed_identity_allocation");
            return false;
        }
    }
    for (const auto& mutation : seed.mutations) {
        const auto status = renderTapeCapture_->resourceMutation(
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
    renderTapeCaptureOracle_ = std::move(seed.oracleAttachments);
    if (!advanceRenderTapeArmBoundary(dxmt9::d3d9::
            RenderTapeArmBoundaryPhase::Armed)) {
        abortRenderTapeCapture("arm_boundary_order");
        return false;
    }
    if (renderTapeNextCaptureToken_ ==
        std::numeric_limits<std::uint64_t>::max()) {
        renderTapeNextCaptureToken_ = 1u;
    } else {
        ++renderTapeNextCaptureToken_;
    }
    renderTapeActiveCaptureToken_ = renderTapeNextCaptureToken_;
    return true;
}

void D3D9DeviceImpl::logRenderTapeMutationRejection(
    const char *reason, const char *detail,
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::uint32_t subresource, std::uint64_t bytes,
    dxmt9::d3d9::RenderTapeCaptureStatus status) noexcept {
    const auto &limits = renderTapeCapture_->limits();
    dxmt9DeviceInfoLog(
        "render_tape_capture mutation_reject reason=%s detail=%s status=%u "
        "capture_state=%u kind=%u generation=%u object_id=%llu "
        "subresource=%u bytes=%llu live_object=%d event_count=%u/%u "
        "buffered_bytes=%llu/%llu owned_blob_bytes=%llu/%llu "
        "owned_blob_entries=%u/%u",
        reason, detail, static_cast<unsigned>(status),
        static_cast<unsigned>(renderTapeCapture_->state()),
        object.identity.kind, object.identity.generation,
        static_cast<unsigned long long>(object.identity.objectId),
        subresource, static_cast<unsigned long long>(bytes),
        renderTapeCapture_->hasLiveObject(object.identity) ? 1 : 0,
        renderTapeCapture_->eventCount(), limits.maxEvents,
        static_cast<unsigned long long>(renderTapeCapture_->bufferedBytes()),
        static_cast<unsigned long long>(limits.maxEventBytes),
        static_cast<unsigned long long>(
            renderTapeCapture_->ownedBlobBytes()),
        static_cast<unsigned long long>(limits.maxBlobBytes),
        renderTapeCapture_->ownedBlobEntries(), limits.maxBlobEntries);
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
        renderTapeCapture_->registerBlobBytes(bytes, &digest);
    if (blobStatus != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
        logRenderTapeMutationRejection(reason, "blob_register", object,
                                       subresource, bytes.size(),
                                       blobStatus);
        return false;
    }
    const auto status = renderTapeCapture_->resourceMutation(
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
    return dxmt9::d3d9::renderTapeBootstrapClosureContains(
        renderTapeAdmittedIdentities_, identity);
}

void D3D9DeviceImpl::removeRenderTapeObjectAdmitted(
    const D9CWireObjectIdentity &identity) noexcept {
    const auto it = std::find_if(
        renderTapeAdmittedIdentities_.begin(),
        renderTapeAdmittedIdentities_.end(), [&](const auto &candidate) {
            return renderTapeSameIdentity(candidate, identity);
        });
    if (it != renderTapeAdmittedIdentities_.end())
        renderTapeAdmittedIdentities_.erase(it);
}

bool D3D9DeviceImpl::materializeRenderTapeObjectForReference(
    const D9CWireObjectIdentity &identity,
    std::uint32_t handleIndex,
    std::uint32_t recordIndex,
    std::uint32_t recordType,
    const dxmt9::d3d9::RenderTapeOriginLocator *originLocator,
    const dxmt9::d3d9::ImportedChunkView *currentChunk) noexcept {
    if (renderTapeObjectAdmitted(identity))
        return true;
    if (!renderTapeRegistry_) {
        abortRenderTapeCapture("jit_materialize_registry_missing");
        return false;
    }
    if (!renderTapeCapture_ ||
        renderTapeCapture_->state() !=
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
        renderTapeRegistry_->objects.begin(),
        renderTapeRegistry_->objects.end(), [&](const auto &candidate) {
            return renderTapeSameIdentity(candidate.identity, identity);
        });
    if (object == renderTapeRegistry_->objects.end()) {
        const dxmt9::d3d9::pe::PeWireObjectRef reference{
            .identity = identity};
        dxmt9DeviceInfoLog(
            "render_tape_capture materialize_miss profile=%u device=%p "
            "registry=%p kind=%u generation=%u object_id=%llu live=0 "
            "pending=0 admitted=%d known_dead=%d handle_index=%u "
            "record_index=%u record_type=%u",
            dxmt9PeRenderTapeCaptureProfile(), this,
            static_cast<void *>(&*renderTapeRegistry_), identity.kind,
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
        renderTapeArmSnapshots_.begin(), renderTapeArmSnapshots_.end(),
        [&](const auto &candidate) {
            return renderTapeSameIdentity(candidate.identity, identity);
        });
    const auto armOverlay = dxmt9::d3d9::
        renderTapeSelectArmObjectSnapshotOverlay(
            object->descriptor, object->content,
            armSnapshot != renderTapeArmSnapshots_.end()
                ? std::span<const std::byte>(armSnapshot->descriptor)
                : std::span<const std::byte>{},
            armSnapshot != renderTapeArmSnapshots_.end()
                ? std::span<const std::vector<std::byte>>(
                      armSnapshot->content)
                : std::span<const std::vector<std::byte>>{},
            armSnapshot != renderTapeArmSnapshots_.end()
                ? armSnapshot->armOrdinal
                : 0u,
            renderTapeArmSnapshotOrdinal_,
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
            renderTapeRegistry_->objects.begin(),
            renderTapeRegistry_->objects.end(), [&](const auto &candidate) {
                return renderTapeSameIdentity(
                    candidate.identity, originLocator->originIdentity);
            });
        const bool exactTexture =
            renderTapeLoadTextureDescriptorV2(effectiveDescriptor, texture) &&
            dxmt9::d3d9::renderTapeProducedTextureShapeSupported(texture) &&
            aliasObject != renderTapeRegistry_->objects.end() &&
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
            renderTapeFirstAccessLedger_, locator.originIdentity,
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
        if (renderTapeCapture_->registerBlobBytes(object->immutablePayload,
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
    if (renderTapeCapture_->objectDefine(
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
            renderTapeAdmittedIdentities_.push_back(identity);
        } catch (...) {
            abortRenderTapeCapture("jit_identity_allocation");
            return false;
        }
        return true;
    }
    for (std::uint32_t subresource = 0u;
         subresource < effectiveContent.size(); ++subresource) {
        if (renderTapeCapture_->resourceMutationBytes(
                identity, dxmt9::d3d9::RenderTapeMutationKind::Upload,
                subresource, 0u, effectiveContent[subresource]) !=
            dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
            abortRenderTapeCapture("jit_resource_mutation");
            return false;
        }
    }
    try {
        renderTapeAdmittedIdentities_.push_back(identity);
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
        attribution.registryPresent = renderTapeRegistry_.has_value();
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
        if (!renderTapeRegistry_) {
            attribution.admission =
                dxmt9::d3d9::renderTapeClassifyCommandAdmission({
                    .originAccepted = originLocator.status ==
                        dxmt9::d3d9::RenderTapeOriginLocatorStatus::Accepted,
                });
            return attribution;
        }
        const auto object = std::find_if(
            renderTapeRegistry_->objects.begin(),
            renderTapeRegistry_->objects.end(), [&](const auto &candidate) {
                return renderTapeSameIdentity(candidate.identity, identity);
            });
        attribution.live = object != renderTapeRegistry_->objects.end();
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
            renderTapeArmSnapshots_.begin(), renderTapeArmSnapshots_.end(),
            [&](const auto &candidate) {
                return renderTapeSameIdentity(candidate.identity, identity);
            });
        attribution.armSnapshotPresent =
            armSnapshot != renderTapeArmSnapshots_.end();
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
                renderTapeArmSnapshotOrdinal_,
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
            renderTapeRegistry_->objects.begin(),
            renderTapeRegistry_->objects.end(), [&](const auto &candidate) {
                return renderTapeSameIdentity(
                    candidate.identity, originLocator.originIdentity);
            });
        attribution.producedAliasPresent =
            aliasObject != renderTapeRegistry_->objects.end();
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
    if (!renderTapeFirstAccessLedger_.armed ||
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
        if (!renderTapeFirstAccessLedger_.terminal) {
            renderTapeFirstAccessLedger_.terminal = true;
            dxmt9DeviceInfoLog(
                "render_tape_capture first_access status=malformed "
                "class=unknown reason=chunk_validation origin_kind=%u "
                "origin_generation=%u origin_object_id=%llu "
                "resolved_kind=%u resolved_generation=%u "
                "resolved_object_id=%llu",
                renderTapeFirstAccessLedger_.originIdentity.kind,
                renderTapeFirstAccessLedger_.originIdentity.generation,
                static_cast<unsigned long long>(
                    renderTapeFirstAccessLedger_.originIdentity.objectId),
                renderTapeFirstAccessLedger_.resolvedIdentity.kind,
                renderTapeFirstAccessLedger_.resolvedIdentity.generation,
                static_cast<unsigned long long>(
                    renderTapeFirstAccessLedger_.resolvedIdentity.objectId));
        }
        return;
    }
    const auto observation = dxmt9::d3d9::renderTapeFirstAccessObserve(
        renderTapeFirstAccessLedger_, imported);
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
    if (!renderTapeCapture_ ||
        renderTapeCapture_->state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        return false;
    }
    if (renderTapeArmBoundaryPhase_ ==
            dxmt9::d3d9::RenderTapeArmBoundaryPhase::Armed &&
        !advanceRenderTapeArmBoundary(dxmt9::d3d9::
            RenderTapeArmBoundaryPhase::FirstCapturedChunk)) {
        abortRenderTapeCapture("arm_boundary_order");
        return false;
    }
    if (renderTapeArmBoundaryPhase_ != dxmt9::d3d9::
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
    if (!renderTapeCapture_ ||
        renderTapeCapture_->state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        return;
    }
    const auto status = renderTapeCapture_->commandChunk(
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

void D3D9DeviceImpl::finishRenderTapeCaptureAtPresentBoundary() noexcept {
    if (!renderTapeCapture_ ||
        renderTapeCapture_->state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        return;
    }
    if (!renderTapeExpectedDigest_) {
        abortRenderTapeCapture("present_output_capture_missing");
        return;
    }
    const std::uint64_t capturedPresentOrdinal =
        renderTapeCapture_->eventCount();
    const auto status = renderTapeCapture_->completePresent(
        capturedPresentOrdinal,
        ++renderTapeCompletionOrdinal_,
        dxmt9::d3d9::RenderTapeDigestValidity::Sha256,
        *renderTapeExpectedDigest_,
        std::as_bytes(std::span(renderTapeCaptureOracle_)),
        renderTapeExpectedPixels_, renderTapeExpectedSourcePixels_);
    if (status != dxmt9::d3d9::RenderTapeCaptureStatus::Accepted &&
        status != dxmt9::d3d9::RenderTapeCaptureStatus::Complete) {
        dxmt9DeviceInfoLog(
            "render_tape_capture completion aborted status=%u events=%u "
            "chunks=%llu present_chunk_seen=%d oracle_bytes=%zu validation=%u",
            static_cast<unsigned>(status), renderTapeCapture_->eventCount(),
            static_cast<unsigned long long>(commandChunkCommits_),
            renderTapeCapture_->presentChunkSeen() ? 1 : 0,
            std::as_bytes(std::span(renderTapeCaptureOracle_)).size(),
            static_cast<unsigned>(renderTapeCapture_->validationStatus()));
        const auto &validation = renderTapeCapture_->validationResult();
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
        renderTapeExpectedDigest_.reset();
        renderTapeExpectedPixels_.clear();
        renderTapeExpectedSourcePixels_.clear();
        return;
    }
    D9CRenderTapeIdentityCaptureResult identityResult{};
    if (renderTapeActiveCaptureToken_ == 0u ||
        FAILED(hr32(dxmt9c_device_finish_render_tape_identity_capture(
            dev_, renderTapeActiveCaptureToken_, &identityResult,
            nullptr, 0u))) ||
        identityResult.status !=
            D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE ||
        identityResult.captureToken != renderTapeActiveCaptureToken_ ||
        identityResult.sourceCount == 0u ||
        identityResult.rangeCount == 0u ||
        identityResult.reserved0 != 0u ||
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
            dev_, renderTapeActiveCaptureToken_, &copiedIdentity,
            identityBytes.data(), identityBytes.size()))) ||
        copiedIdentity.status !=
            D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE ||
        copiedIdentity.sourceCount != identityResult.sourceCount ||
        copiedIdentity.rangeCount != identityResult.rangeCount ||
        copiedIdentity.captureToken != identityResult.captureToken ||
        copiedIdentity.byteCount != identityResult.byteCount) {
        abortRenderTapeCapture("identity_copy");
        return;
    }
    if (static_cast<std::size_t>(copiedIdentity.sourceCount) >
            std::numeric_limits<std::size_t>::max() /
                sizeof(D9CRenderTapeIdentitySourceEntry) ||
        static_cast<std::size_t>(copiedIdentity.rangeCount) >
            std::numeric_limits<std::size_t>::max() /
                sizeof(D9CRenderTapeIdentityRangeEntry)) {
        abortRenderTapeCapture("identity_layout");
        return;
    }
    const std::size_t sourceBytes =
        static_cast<std::size_t>(copiedIdentity.sourceCount) *
        sizeof(D9CRenderTapeIdentitySourceEntry);
    const std::size_t rangeBytes =
        static_cast<std::size_t>(copiedIdentity.rangeCount) *
        sizeof(D9CRenderTapeIdentityRangeEntry);
    if (sourceBytes > identityBytes.size() ||
        rangeBytes != identityBytes.size() - sourceBytes) {
        abortRenderTapeCapture("identity_layout");
        return;
    }
    std::vector<dxmt9::d3d9::RenderTapeIdentitySource> identitySources;
    std::vector<dxmt9::d3d9::RenderTapeIdentityRange> identityRanges;
    try {
        identitySources.resize(copiedIdentity.sourceCount);
        identityRanges.resize(copiedIdentity.rangeCount);
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
    if (renderTapeCapture_->attachCaptureIdentity(
            renderTapeActiveCaptureToken_, capturedPresentOrdinal,
            identitySources, identityRanges) !=
        dxmt9::d3d9::RenderTapeCaptureStatus::Complete) {
        const auto& identityValidation =
            renderTapeCapture_->identityValidationResult();
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
        publisher && publisher(renderTapeCapture_->publicationBundle());
    dxmt9DeviceInfoLog("render_tape_capture publication published=%d",
                       published ? 1 : 0);
    if (!published) {
        abortRenderTapeCapture("publication");
    }
    if (dxmt9::d3d9::renderTapeArmSnapshotCompletionAction(true) ==
        dxmt9::d3d9::RenderTapeArmSnapshotCompletionAction::Clear) {
        renderTapeArmSnapshots_.clear();
    }
    renderTapeExpectedDigest_.reset();
    renderTapeExpectedPixels_.clear();
    renderTapeExpectedSourcePixels_.clear();
    renderTapeActiveCaptureToken_ = 0u;
}

void D3D9DeviceImpl::NotifyRenderTapeObjectDefineForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::span<const std::byte> descriptor,
    std::span<const std::byte> immutablePayload) noexcept {
    if (!renderTapeCapture_ ||
        renderTapeCapture_->state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        return;
    }
    try {
        dxmt9::d3d9::RenderTapeDigest digest{};
        std::uint64_t bytes = 0u;
        if (!immutablePayload.empty()) {
            const auto status = renderTapeCapture_->registerBlobBytes(
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
        const auto status = renderTapeCapture_->objectDefine(
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
    return renderTapeCapture_ &&
           renderTapeCapture_->state() ==
               dxmt9::d3d9::RenderTapeCaptureState::Capturing;
}

bool D3D9DeviceImpl::IsRenderTapeCaptureTrackingEnabledForChild() const noexcept {
    return renderTapeRegistry_.has_value();
}

void D3D9DeviceImpl::AbortRenderTapeCaptureForChild() noexcept {
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
    const char *name =
        dxmt9::d3d9::renderTapeCaptureRejectionReasonName(reason);
    const bool first = renderTapeRegistry_ && !renderTapeRegistry_->invalid;
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
    if (!renderTapeRegistry_ ||
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

void D3D9DeviceImpl::applyRenderTapeUpdateTextureClosure(
    const dxmt9::d3d9::pe::PeWireObjectRef &source,
    const dxmt9::d3d9::pe::PeWireObjectRef &destination) noexcept {
    if (!renderTapeRegistry_ ||
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

void D3D9DeviceImpl::NotifyRenderTapeSurfaceAliasForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &surface,
    const dxmt9::d3d9::pe::PeWireObjectRef &parentTexture,
    std::uint32_t subresource,
    const D9CSurfaceDesc &descriptor) noexcept {
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
    if (!renderTapeRegistry_)
        return false;
    const auto it = std::find_if(
        renderTapeRegistry_->objects.begin(),
        renderTapeRegistry_->objects.end(), [&](const auto &entry) {
            return renderTapeSameIdentity(entry.identity, identity);
        });
    if (it == renderTapeRegistry_->objects.end())
        return false;
    const bool admitted = renderTapeObjectAdmitted(identity);
    if (recordDestroy && admitted && renderTapeCapture_ &&
        renderTapeCapture_->state() ==
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        const auto status = renderTapeCapture_->objectDestroy(identity);
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
        renderTapeRegistry_->knownDead.push_back(identity);
    } catch (...) {
        const dxmt9::d3d9::pe::PeWireObjectRef object{.identity = identity};
        markRenderTapeInvalidOnce("object_destroy_tombstone_allocation",
                                 &object);
        return false;
    }
    renderTapeRegistry_->objects.erase(it);
    return true;
}

void D3D9DeviceImpl::retireRenderTapeAliasesForParent(
    const D9CWireObjectIdentity &parent, bool recordDestroy) noexcept {
    if (!renderTapeRegistry_) {
        return;
    }
    for (auto it = renderTapeRegistry_->objects.begin();
         it != renderTapeRegistry_->objects.end();) {
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
        (void)retireRenderTapeObject(identity, recordDestroy,
                                      "alias_object_destroy");
        // The helper erases the identity. Restarting from a value lookup
        // keeps iterator invalidation out of this bounded registry walk.
        it = renderTapeRegistry_->objects.begin();
    }
}

void D3D9DeviceImpl::drainPendingRenderTapeChunk(bool recordDestroy) noexcept {
    if (!renderTapeRegistry_)
        return;
    // Handles are intentionally walked after the command has been
    // materialized. Duplicate handles across records are harmless because
    // the bounded lifetime ref reaches zero on the first visit.
    for (const auto &handle : commandChunk_.handles()) {
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
        if (isTexture)
            retireRenderTapeAliasesForParent(identity, recordDestroy);
        // Preserve the established alias-before-parent event order. The
        // parent entry remains in the registry until the alias scan has
        // completed, so the scan is safe for both immediate and pending
        // retirement.
        retireRenderTapeObject(identity, recordDestroy, "object_destroy");
    }
}

void D3D9DeviceImpl::NotifyRenderTapeObjectDestroyForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
    if (renderTapeRegistry_) {
        auto *entry = findRenderTapeObject(object);
        // The PE wrapper destructor has already delivered this callback.
        // Transfer the logical lifetime to the bounded pending chunk ref;
        // drain it after command materialization and before raw D9C
        // retainer reset.
        if (entry && entry->lifetime.wrapperRefs == 1u &&
            entry->lifetime.pendingChunkRefs == 0u &&
            commandChunk_.referencesObject(object.object) &&
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
    const bool registryAccepted =
        recordRenderTapeCpuBytes(object, subresource, byteOffset, bytes);
    if (!registryAccepted) {
        if (IsRenderTapeCaptureActiveForChild())
            abortRenderTapeCapture("resource_mutation_registry");
        return;
    }
    if (!renderTapeCapture_ ||
        renderTapeCapture_->state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        return;
    }
    if (!renderTapeObjectAdmitted(object.identity))
        return;
    try {
        if (renderTapeCapture_->resourceMutationBytes(
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
    if (!renderTapeCapture_ ||
        renderTapeCapture_->state() !=
            dxmt9::d3d9::RenderTapeCaptureState::Capturing) {
        return;
    }
    if (fixed.identity.objectId != 0u &&
        !materializeRenderTapeObjectForReference(fixed.identity)) {
        return;
    }
    auto recorded = fixed;
    recorded.completionOrdinal = ++renderTapeCompletionOrdinal_;
    if (renderTapeCapture_->orderedControl(recorded, payload) !=
        dxmt9::d3d9::RenderTapeCaptureStatus::Accepted) {
        abortRenderTapeCapture("ordered_control");
    }
}

void D3D9DeviceImpl::notifyRenderTapeCreatedObject(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    std::span<const std::byte> descriptor,
    std::span<const std::byte> immutablePayload) noexcept {
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
