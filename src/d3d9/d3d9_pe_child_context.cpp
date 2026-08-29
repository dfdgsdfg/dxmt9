#include "d3d9_pe_device_impl.hpp"

HRESULT D3D9PeStateBlockContext::FlushPeRecorderForChild() noexcept {
  return device ? device->FlushPeRecorderForChild() : S_OK;
}
bool D3D9PeStateBlockContext::IsStateBlockRecordingForChild() const noexcept {
  return device && device->IsStateBlockRecordingForChild();
}
void D3D9PeStateBlockContext::LockStateBlockOperationForChild() noexcept {
  if (device) device->LockStateBlockOperationForChild();
}
void D3D9PeStateBlockContext::UnlockStateBlockOperationForChild() noexcept {
  if (device) device->UnlockStateBlockOperationForChild();
}
bool D3D9PeStateBlockContext::IsStateBlockRecorderPoisonedForChild() const noexcept {
  return device && device->IsStateBlockRecorderPoisonedForChild();
}
HRESULT D3D9PeStateBlockContext::PrepareStateBlockApplyForChild(
    const D3D9StateBlockShadow &shadow) noexcept {
  return device ? device->PrepareStateBlockApplyForChild(shadow) : E_FAIL;
}
void D3D9PeStateBlockContext::CommitStateBlockApplyForChild(
    const D3D9StateBlockShadow &shadow,
    StateBlockCaptureDisposition disposition) noexcept {
  if (device) device->CommitStateBlockApplyForChild(shadow, disposition);
}
void D3D9PeStateBlockContext::DiscardPreparedStateBlockApplyForChild() noexcept {
  if (device) device->DiscardPreparedStateBlockApplyForChild();
}
void D3D9PeStateBlockContext::PoisonStateBlockRecorderForChild() noexcept {
  if (device) device->PoisonStateBlockRecorderForChild();
}
HRESULT D3D9PeStateBlockContext::CaptureStateBlockShadowForChild(
    D3D9StateBlockShadow &out, StateBlockCaptureDisposition disposition) noexcept {
  return device ? device->CaptureStateBlockShadowForChild(out, disposition) : E_FAIL;
}

HRESULT D3D9PeBufferContext::FlushPeRecorderForChild() noexcept {
  return device ? device->FlushPeRecorderForChild() : S_OK;
}
HRESULT D3D9PeBufferContext::FlushPeRecorderForBufferHazardForChild(
    D9CBuffer *buffer) noexcept {
  return device ? device->FlushPeRecorderForBufferHazardForChild(buffer) : S_OK;
}
void D3D9PeBufferContext::AddDefaultPoolResourceRefForChild() noexcept {
  if (device) device->AddDefaultPoolResourceRefForChild();
}
void D3D9PeBufferContext::ReleaseDefaultPoolResourceRefForChild() noexcept {
  if (device) device->ReleaseDefaultPoolResourceRefForChild();
}
void D3D9PeBufferContext::NotifyRenderTapeObjectDestroyForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
  if (device) device->NotifyRenderTapeObjectDestroyForChild(object);
}
void D3D9PeBufferContext::NotifyRenderTapeResourceMutationForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    dxmt9::d3d9::RenderTapeMutationKind kind, std::uint32_t subresource,
    std::uint64_t byteOffset, std::span<const std::byte> bytes,
    dxmt9::d3d9::RenderTapeBufferMutationDisposition disposition) noexcept {
  if (device) device->NotifyRenderTapeResourceMutationForChild(
      object, kind, subresource, byteOffset, bytes, disposition);
}
bool D3D9PeBufferContext::IsRenderTapeCaptureActiveForChild() const noexcept {
  return device && device->IsRenderTapeCaptureActiveForChild();
}
bool D3D9PeBufferContext::IsRenderTapeCaptureTrackingEnabledForChild() const noexcept {
  return device && device->IsRenderTapeCaptureTrackingEnabledForChild();
}
void D3D9PeBufferContext::AbortRenderTapeCaptureForChild() noexcept {
  if (device) device->AbortRenderTapeCaptureForChild();
}
void D3D9PeBufferContext::RejectRenderTapeCaptureForChild(
    dxmt9::d3d9::RenderTapeCaptureRejectionReason reason,
    const dxmt9::d3d9::pe::PeWireObjectRef &object, std::uint32_t subresource,
    const dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic &diagnostic) noexcept {
  if (device) device->RejectRenderTapeCaptureForChild(reason, object, subresource,
                                                       diagnostic);
}
dxmt9::d3d9::RenderTapeFullSnapshotStatus
D3D9PeBufferContext::RenderTapeFullSnapshotStatusForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object, std::uint32_t subresource,
    std::uint32_t fullRowBytes, std::uint32_t fullRows,
    std::uint64_t fullBytes) const noexcept {
  return device ? device->RenderTapeFullSnapshotStatusForChild(
                      object, subresource, fullRowBytes, fullRows, fullBytes)
                : dxmt9::d3d9::RenderTapeFullSnapshotStatus{};
}
void D3D9PeBufferContext::NotifyRenderTapeOrderedControlForChild(
    const dxmt9::d3d9::RenderTapeOrderedControlHeader &fixed,
    std::span<const std::byte> payload) noexcept {
  if (device) device->NotifyRenderTapeOrderedControlForChild(fixed, payload);
}

HRESULT D3D9PeSurfaceTextureContext::FlushPeRecorderForChild() noexcept {
  return device ? device->FlushPeRecorderForChild() : S_OK;
}
void D3D9PeSurfaceTextureContext::AddDefaultPoolResourceRefForChild() noexcept {
  if (device) device->AddDefaultPoolResourceRefForChild();
}
void D3D9PeSurfaceTextureContext::ReleaseDefaultPoolResourceRefForChild() noexcept {
  if (device) device->ReleaseDefaultPoolResourceRefForChild();
}
void D3D9PeSurfaceTextureContext::NotifyRenderTapeObjectDestroyForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
  if (device) device->NotifyRenderTapeObjectDestroyForChild(object);
}
void D3D9PeSurfaceTextureContext::NotifyRenderTapeResourceMutationForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object,
    dxmt9::d3d9::RenderTapeMutationKind kind, std::uint32_t subresource,
    std::uint64_t byteOffset, std::span<const std::byte> bytes,
    dxmt9::d3d9::RenderTapeBufferMutationDisposition disposition) noexcept {
  if (device) device->NotifyRenderTapeResourceMutationForChild(
      object, kind, subresource, byteOffset, bytes, disposition);
}
void D3D9PeSurfaceTextureContext::NotifyRenderTapeOrderedControlForChild(
    const dxmt9::d3d9::RenderTapeOrderedControlHeader &fixed,
    std::span<const std::byte> payload) noexcept {
  if (device) device->NotifyRenderTapeOrderedControlForChild(fixed, payload);
}
bool D3D9PeSurfaceTextureContext::IsRenderTapeCaptureTrackingEnabledForChild()
    const noexcept {
  return device && device->IsRenderTapeCaptureTrackingEnabledForChild();
}
void D3D9PeSurfaceTextureContext::AbortRenderTapeCaptureForChild() noexcept {
  if (device) device->AbortRenderTapeCaptureForChild();
}
void D3D9PeSurfaceTextureContext::RejectRenderTapeCaptureForChild(
    dxmt9::d3d9::RenderTapeCaptureRejectionReason reason,
    const dxmt9::d3d9::pe::PeWireObjectRef &object, std::uint32_t subresource,
    const dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic &diagnostic) noexcept {
  if (device) device->RejectRenderTapeCaptureForChild(reason, object, subresource,
                                                       diagnostic);
}
dxmt9::d3d9::RenderTapeFullSnapshotStatus
D3D9PeSurfaceTextureContext::RenderTapeFullSnapshotStatusForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object, std::uint32_t subresource,
    std::uint32_t fullRowBytes, std::uint32_t fullRows,
    std::uint64_t fullBytes) const noexcept {
  return device ? device->RenderTapeFullSnapshotStatusForChild(
                      object, subresource, fullRowBytes, fullRows, fullBytes)
                : dxmt9::d3d9::RenderTapeFullSnapshotStatus{};
}
void D3D9PeSurfaceTextureContext::NotifyRenderTapeBlockMutationForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object, std::uint32_t subresource,
    const dxmt9::d3d9::RenderTapeBlockLockLayout &layout,
    std::span<const std::byte> bytes) noexcept {
  if (device) device->NotifyRenderTapeBlockMutationForChild(object, subresource,
                                                              layout, bytes);
}
void D3D9PeSurfaceTextureContext::NotifyRenderTapeLinearMutationForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object, std::uint32_t subresource,
    const dxmt9::d3d9::RenderTapeLinearLockLayout &layout,
    std::span<const std::byte> bytes) noexcept {
  if (device) device->NotifyRenderTapeLinearMutationForChild(object, subresource,
                                                               layout, bytes);
}
void D3D9PeSurfaceTextureContext::NotifyRenderTapeSurfaceAliasForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &surface,
    const dxmt9::d3d9::pe::PeWireObjectRef &parentTexture,
    std::uint32_t subresource, const D9CSurfaceDesc &descriptor) noexcept {
  if (device) device->NotifyRenderTapeSurfaceAliasForChild(
      surface, parentTexture, subresource, descriptor);
}
void D3D9PeSurfaceTextureContext::NotifyRenderTapeStandaloneSurfaceForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &surface,
    const D9CSurfaceDesc &descriptor) noexcept {
  if (device) device->NotifyRenderTapeStandaloneSurfaceForChild(surface,
                                                                  descriptor);
}

HRESULT D3D9PeQueryContext::FlushPeRecorderForChild() noexcept {
  return device ? device->FlushPeRecorderForChild() : S_OK;
}
bool D3D9PeQueryContext::IsChunkRecorderEnabledForChild() const noexcept {
  return device && device->IsChunkRecorderEnabledForChild();
}
HRESULT D3D9PeQueryContext::AppendQueryIssueForChild(
    std::uint32_t flags, const dxmt9::d3d9::pe::QueryRef &query) noexcept {
  return device ? device->AppendQueryIssueForChild(flags, query) : E_FAIL;
}
void D3D9PeQueryContext::NotifyRenderTapeObjectDestroyForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
  if (device) device->NotifyRenderTapeObjectDestroyForChild(object);
}
void D3D9PeQueryContext::NotifyRenderTapeOrderedControlForChild(
    const dxmt9::d3d9::RenderTapeOrderedControlHeader &fixed,
    std::span<const std::byte> payload) noexcept {
  if (device) device->NotifyRenderTapeOrderedControlForChild(fixed, payload);
}

HRESULT D3D9PePresentationContext::FlushPeRecorderForChild() noexcept {
  return device ? device->FlushPeRecorderForChild() : S_OK;
}
void D3D9PePresentationContext::LockStateBlockOperationForChild() noexcept {
  if (device) device->LockStateBlockOperationForChild();
}
void D3D9PePresentationContext::UnlockStateBlockOperationForChild() noexcept {
  if (device) device->UnlockStateBlockOperationForChild();
}
D3D9PeSurfaceTextureContext *
D3D9PePresentationContext::surfaceTextureContextForChild() noexcept {
  return device ? device->surfaceTextureContext() : nullptr;
}
void D3D9PePresentationContext::NotifyRenderTapeObjectDestroyForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
  if (device) device->NotifyRenderTapeObjectDestroyForChild(object);
}

void D3D9PeShaderDeclarationContext::NotifyRenderTapeObjectDestroyForChild(
    const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept {
  if (device) device->NotifyRenderTapeObjectDestroyForChild(object);
}
