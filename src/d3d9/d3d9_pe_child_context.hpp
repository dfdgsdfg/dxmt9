#pragma once

#include "d3d9_pe.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_state_shadow.hpp"
#include "device_c_render_tape.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_capture_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

class D3D9DeviceImpl;
class D3D9StateBlockShadow;

// The six contexts are deliberately independent, standard-layout, and
// pointer-sized. Their only state is the device pointer; implementation is
// out-of-line in d3d9_pe_child_context.cpp.
struct D3D9PeStateBlockContext {
  D3D9DeviceImpl *device = nullptr;
  HRESULT FlushPeRecorderForChild() noexcept;
  bool IsStateBlockRecordingForChild() const noexcept;
  void LockStateBlockOperationForChild() noexcept;
  void UnlockStateBlockOperationForChild() noexcept;
  bool IsStateBlockRecorderPoisonedForChild() const noexcept;
  HRESULT PrepareStateBlockApplyForChild(const D3D9StateBlockShadow &) noexcept;
  void CommitStateBlockApplyForChild(const D3D9StateBlockShadow &) noexcept;
  void DiscardPreparedStateBlockApplyForChild() noexcept;
  void PoisonStateBlockRecorderForChild() noexcept;
  HRESULT CaptureStateBlockShadowForChild(
      D3D9StateBlockShadow &, StateBlockCaptureDisposition) noexcept;
};

struct D3D9PeBufferContext {
  D3D9DeviceImpl *device = nullptr;
  HRESULT FlushPeRecorderForChild() noexcept;
  HRESULT FlushPeRecorderForBufferHazardForChild(D9CBuffer *) noexcept;
  void AddDefaultPoolResourceRefForChild() noexcept;
  void ReleaseDefaultPoolResourceRefForChild() noexcept;
  void NotifyRenderTapeObjectDestroyForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &) noexcept;
  void NotifyRenderTapeResourceMutationForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &, dxmt9::d3d9::RenderTapeMutationKind,
      std::uint32_t, std::uint64_t, std::span<const std::byte>,
      dxmt9::d3d9::RenderTapeBufferMutationDisposition =
          dxmt9::d3d9::RenderTapeBufferMutationDisposition::Plain) noexcept;
  bool IsRenderTapeCaptureActiveForChild() const noexcept;
  bool IsRenderTapeCaptureTrackingEnabledForChild() const noexcept;
  void AbortRenderTapeCaptureForChild() noexcept;
  void RejectRenderTapeCaptureForChild(
      dxmt9::d3d9::RenderTapeCaptureRejectionReason,
      const dxmt9::d3d9::pe::PeWireObjectRef &, std::uint32_t,
      const dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic & = {}) noexcept;
  dxmt9::d3d9::RenderTapeFullSnapshotStatus RenderTapeFullSnapshotStatusForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &, std::uint32_t, std::uint32_t,
      std::uint32_t, std::uint64_t) const noexcept;
  void NotifyRenderTapeOrderedControlForChild(
      const dxmt9::d3d9::RenderTapeOrderedControlHeader &,
      std::span<const std::byte>) noexcept;
};

struct D3D9PeSurfaceTextureContext {
  D3D9DeviceImpl *device = nullptr;
  HRESULT FlushPeRecorderForChild() noexcept;
  void AddDefaultPoolResourceRefForChild() noexcept;
  void ReleaseDefaultPoolResourceRefForChild() noexcept;
  void NotifyRenderTapeObjectDestroyForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &) noexcept;
  void NotifyRenderTapeResourceMutationForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &, dxmt9::d3d9::RenderTapeMutationKind,
      std::uint32_t, std::uint64_t, std::span<const std::byte>,
      dxmt9::d3d9::RenderTapeBufferMutationDisposition =
          dxmt9::d3d9::RenderTapeBufferMutationDisposition::Plain) noexcept;
  void NotifyRenderTapeOrderedControlForChild(
      const dxmt9::d3d9::RenderTapeOrderedControlHeader &,
      std::span<const std::byte>) noexcept;
  bool IsRenderTapeCaptureTrackingEnabledForChild() const noexcept;
  void AbortRenderTapeCaptureForChild() noexcept;
  void RejectRenderTapeCaptureForChild(
      dxmt9::d3d9::RenderTapeCaptureRejectionReason,
      const dxmt9::d3d9::pe::PeWireObjectRef &, std::uint32_t,
      const dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic & = {}) noexcept;
  dxmt9::d3d9::RenderTapeFullSnapshotStatus RenderTapeFullSnapshotStatusForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &, std::uint32_t, std::uint32_t,
      std::uint32_t, std::uint64_t) const noexcept;
  void NotifyRenderTapeBlockMutationForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &, std::uint32_t,
      const dxmt9::d3d9::RenderTapeBlockLockLayout &, std::span<const std::byte>) noexcept;
  void NotifyRenderTapeLinearMutationForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &, std::uint32_t,
      const dxmt9::d3d9::RenderTapeLinearLockLayout &, std::span<const std::byte>) noexcept;
  void NotifyRenderTapeSurfaceAliasForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &,
      const dxmt9::d3d9::pe::PeWireObjectRef &, std::uint32_t,
      const D9CSurfaceDesc &) noexcept;
  void NotifyRenderTapeStandaloneSurfaceForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &, const D9CSurfaceDesc &) noexcept;
};

struct D3D9PeQueryContext {
  D3D9DeviceImpl *device = nullptr;
  HRESULT FlushPeRecorderForChild() noexcept;
  bool IsChunkRecorderEnabledForChild() const noexcept;
  HRESULT AppendQueryIssueForChild(
      std::uint32_t, const dxmt9::d3d9::pe::QueryRef &) noexcept;
  void NotifyRenderTapeObjectDestroyForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &) noexcept;
  void NotifyRenderTapeOrderedControlForChild(
      const dxmt9::d3d9::RenderTapeOrderedControlHeader &,
      std::span<const std::byte>) noexcept;
};

struct D3D9PePresentationContext {
  D3D9DeviceImpl *device = nullptr;
  HRESULT FlushPeRecorderForChild() noexcept;
  void LockStateBlockOperationForChild() noexcept;
  void UnlockStateBlockOperationForChild() noexcept;
  D3D9PeSurfaceTextureContext *surfaceTextureContextForChild() noexcept;
  void NotifyRenderTapeObjectDestroyForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &) noexcept;
};

struct D3D9PeShaderDeclarationContext {
  D3D9DeviceImpl *device = nullptr;
  void NotifyRenderTapeObjectDestroyForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &) noexcept;
};

static_assert(std::is_standard_layout_v<D3D9PeStateBlockContext> &&
                  sizeof(D3D9PeStateBlockContext) == sizeof(void *));
static_assert(std::is_standard_layout_v<D3D9PeBufferContext> &&
                  sizeof(D3D9PeBufferContext) == sizeof(void *));
static_assert(std::is_standard_layout_v<D3D9PeSurfaceTextureContext> &&
                  sizeof(D3D9PeSurfaceTextureContext) == sizeof(void *));
static_assert(std::is_standard_layout_v<D3D9PeQueryContext> &&
                  sizeof(D3D9PeQueryContext) == sizeof(void *));
static_assert(std::is_standard_layout_v<D3D9PePresentationContext> &&
                  sizeof(D3D9PePresentationContext) == sizeof(void *));
static_assert(std::is_standard_layout_v<D3D9PeShaderDeclarationContext> &&
                  sizeof(D3D9PeShaderDeclarationContext) == sizeof(void *));
