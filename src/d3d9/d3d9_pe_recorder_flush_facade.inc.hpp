// Ordered child-callback facade fragment. Included after D3D9StateBlockShadow
// is complete so the callback contract stays one non-data-bearing base with
// no capability subobjects or child-side pointer growth.
struct D3D9PeRecorderFlush {
  virtual HRESULT FlushPeRecorderForChild() noexcept = 0;
  virtual bool IsStateBlockRecordingForChild() const noexcept = 0;
  virtual void LockStateBlockOperationForChild() noexcept = 0;
  virtual void UnlockStateBlockOperationForChild() noexcept = 0;
  virtual bool IsStateBlockRecorderPoisonedForChild() const noexcept = 0;
  virtual HRESULT PrepareStateBlockApplyForChild(
      const D3D9StateBlockShadow &shadow) noexcept = 0;
  virtual void CommitStateBlockApplyForChild(
      const D3D9StateBlockShadow &shadow) noexcept = 0;
  virtual void DiscardPreparedStateBlockApplyForChild() noexcept = 0;
  virtual void PoisonStateBlockRecorderForChild() noexcept = 0;
  virtual void InvalidateStateBlockShadowForChild() noexcept = 0;
  virtual void AddDefaultPoolResourceRefForChild() noexcept = 0;
  virtual void ReleaseDefaultPoolResourceRefForChild() noexcept = 0;
  virtual bool IsChunkRecorderEnabledForChild() const noexcept = 0;
  virtual HRESULT AppendQueryIssueForChild(
      std::uint32_t flags,
      const dxmt9::d3d9::pe::QueryRef &query) noexcept = 0;
  virtual HRESULT FlushPeRecorderForBufferHazardForChild(
      D9CBuffer *buffer) noexcept = 0;
  virtual void NotifyRenderTapeObjectDefineForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &object,
      std::span<const std::byte> descriptor,
      std::span<const std::byte> immutablePayload = {}) noexcept = 0;
  virtual void NotifyRenderTapeObjectDestroyForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &object) noexcept = 0;
  virtual void NotifyRenderTapeResourceMutationForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &object,
      dxmt9::d3d9::RenderTapeMutationKind kind, std::uint32_t subresource,
      std::uint64_t byteOffset, std::span<const std::byte> bytes,
      dxmt9::d3d9::RenderTapeBufferMutationDisposition bufferDisposition =
          dxmt9::d3d9::RenderTapeBufferMutationDisposition::Plain) noexcept = 0;
  virtual void NotifyRenderTapeOrderedControlForChild(
      const dxmt9::d3d9::RenderTapeOrderedControlHeader &fixed,
      std::span<const std::byte> payload) noexcept = 0;
  virtual bool IsRenderTapeCaptureActiveForChild() const noexcept = 0;
  virtual bool IsRenderTapeCaptureTrackingEnabledForChild() const noexcept = 0;
  virtual void AbortRenderTapeCaptureForChild() noexcept = 0;
  virtual void RejectRenderTapeCaptureForChild(
      dxmt9::d3d9::RenderTapeCaptureRejectionReason reason,
      const dxmt9::d3d9::pe::PeWireObjectRef &object,
      std::uint32_t subresource,
      const dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic &diagnostic =
          {}) noexcept = 0;
  virtual dxmt9::d3d9::RenderTapeFullSnapshotStatus
  RenderTapeFullSnapshotStatusForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &object,
      std::uint32_t subresource, std::uint32_t fullRowBytes,
      std::uint32_t fullRows, std::uint64_t fullBytes) const noexcept = 0;
  virtual void NotifyRenderTapeBlockMutationForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &object,
      std::uint32_t subresource,
      const dxmt9::d3d9::RenderTapeBlockLockLayout &layout,
      std::span<const std::byte> bytes) noexcept = 0;
  virtual void NotifyRenderTapeLinearMutationForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &object,
      std::uint32_t subresource,
      const dxmt9::d3d9::RenderTapeLinearLockLayout &layout,
      std::span<const std::byte> bytes) noexcept = 0;
  virtual void NotifyRenderTapeSurfaceAliasForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &surface,
      const dxmt9::d3d9::pe::PeWireObjectRef &parentTexture,
      std::uint32_t subresource,
      const D9CSurfaceDesc &descriptor) noexcept = 0;
  virtual void NotifyRenderTapeStandaloneSurfaceForChild(
      const dxmt9::d3d9::pe::PeWireObjectRef &surface,
      const D9CSurfaceDesc &descriptor) noexcept = 0;
  virtual HRESULT CaptureStateBlockShadowForChild(
      D3D9StateBlockShadow &out,
      StateBlockCaptureDisposition disposition) noexcept = 0;

protected:
  ~D3D9PeRecorderFlush() = default;
};
