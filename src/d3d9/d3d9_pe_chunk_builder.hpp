#pragma once

#include "d3d9_pe_retainer.hpp"
#include "device_c_chunk_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace dxmt9::d3d9::pe {

struct PeWireObjectRef {
  D9CWireObjectIdentity identity{};
  void* object = nullptr;

  bool valid(std::uint32_t expectedKind) const noexcept {
    return object && identity.kind == expectedKind &&
           identity.kind <= D9C_CHUNK_HANDLE_KIND_QUERY &&
           identity.generation != 0u && identity.objectId != 0u;
  }
};

void publishCachedWireObjectRef(const PeWireObjectRef& object) noexcept;
void unpublishCachedWireObjectRef(const PeWireObjectRef& object) noexcept;
void noteWireIdentityGetterCall() noexcept;
std::uint64_t wireIdentityGetterCallCount() noexcept;

template <typename Object, typename Getter>
bool cacheWireObjectRef(Object* object, std::uint32_t expectedKind,
                        Getter&& getter, PeWireObjectRef& out) {
  out = {};
  if (!object) {
    return false;
  }
  noteWireIdentityGetterCall();
  D9CWireObjectIdentity identity{};
  if (getter(object, &identity) < 0 || identity.kind != expectedKind ||
      identity.generation == 0u || identity.objectId == 0u) {
    return false;
  }
  out = PeWireObjectRef{
      .identity = identity,
      .object = object,
  };
  publishCachedWireObjectRef(out);
  return true;
}

struct CommandChunkBuilderCapacities {
  std::size_t records = 64u;
  std::size_t handles = 256u;
  std::size_t payloadBytes = 256u * 1024u;
  std::size_t sealedBytes = 272u * 1024u;
};

struct SealedCommandChunk {
  std::span<const std::byte> blob{};
  std::uint32_t recordCount = 0u;
  std::uint32_t handleCount = 0u;

  bool valid() const noexcept { return !blob.empty(); }
};

// R-BACK-43.4 `producer-owned` (PE game thread). Every member below —
// `records_`, `handles_`, `handleObjects_`, `payload_`, `sealedBlob_`,
// `retainer_`, `active_`, `sealed_` — is written and read only on the thread
// driving the D3D9 recorder, and none of it is reachable from the replay
// worker, encode thread, or completion path: the builder's output crosses to
// unix as the sealed POD blob, never as live state.
//
// Enforcement is at the `D3D9DeviceImpl` call boundary
// (`assertRecorderThreadConfined()`, R-BACK-43.5 shape (c)), not with a token
// here — see the same note on `D3D9PePendingCommandRetainer` for why a
// builder-local construction-bound token would be incorrect under
// `D3DCREATE_MULTITHREADED` rather than merely duplicated.
class CommandChunkBuilder {
 public:
  explicit CommandChunkBuilder(
      const CommandChunkBuilderCapacities& capacities = {});
  ~CommandChunkBuilder() = default;

  CommandChunkBuilder(const CommandChunkBuilder&) = delete;
  CommandChunkBuilder& operator=(const CommandChunkBuilder&) = delete;

  bool beginRecord(std::uint32_t type) noexcept;
  bool appendPayload(std::span<const std::byte> bytes,
                     std::uint32_t alignment = 1u,
                     std::uint32_t* recordRelativeOffset = nullptr) noexcept;

  template <typename T>
  bool appendPayloadValue(const T& value,
                          std::uint32_t* recordRelativeOffset = nullptr) {
    return appendPayload(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(&value), sizeof(value)),
        alignof(T), recordRelativeOffset);
  }

  bool overwritePayload(std::uint32_t recordRelativeOffset,
                        std::span<const std::byte> bytes) noexcept;
  bool appendHandle(const PeWireObjectRef& object,
                    std::uint32_t expectedKind,
                    std::uint32_t& absoluteIndex) noexcept;
  bool commitRecord() noexcept;
  void rollbackRecord() noexcept;

  SealedCommandChunk seal() noexcept;
  // Chunk boundary after a successful commit: keeps recently-named wrapper
  // pins warm across the boundary (see D3D9PePendingCommandRetainer).
  void reset() noexcept;
  // Discard: same as reset() but also releases every warm pin. Use at device
  // teardown / Reset / ResetEx.
  void resetAndReleaseRetained() noexcept;

  bool recordActive() const noexcept { return active_.active; }
  bool sealed() const noexcept { return sealed_; }
  std::size_t recordCount() const noexcept { return records_.size(); }
  std::size_t handleCount() const noexcept { return handles_.size(); }
  std::size_t payloadBytes() const noexcept { return payload_.size(); }
  std::size_t retainedObjectCount() const noexcept { return retainer_.size(); }
  bool referencesObject(void* object) const noexcept;

  const std::vector<D9CCommandChunkWireRecordHeader>& recordsForTest()
      const noexcept {
    return records_;
  }
  const std::vector<D9CCommandChunkWireHandleEntry>& handlesForTest()
      const noexcept {
    return handles_;
  }
  const std::vector<D9CCommandChunkWireHandleEntry>& handles() const noexcept {
    return handles_;
  }
  const std::vector<std::byte>& payloadForTest() const noexcept {
    return payload_;
  }

 private:
  struct ActiveRecord {
    bool active = false;
    std::uint32_t type = 0u;
    std::size_t recordCheckpoint = 0u;
    std::size_t handleCheckpoint = 0u;
    std::size_t payloadCheckpoint = 0u;
    std::size_t payloadStart = 0u;
    D3D9PePendingCommandRetainer::Acquired retainedCheckpoint{};
  };

  bool failActiveRecord() noexcept;

  std::vector<D9CCommandChunkWireRecordHeader> records_;
  std::vector<D9CCommandChunkWireHandleEntry> handles_;
  std::vector<void*> handleObjects_;
  std::vector<std::byte> payload_;
  std::vector<std::byte> sealedBlob_;
  D3D9PePendingCommandRetainer retainer_;
  ActiveRecord active_{};
  bool sealed_ = false;
};

template <typename Wire>
struct SparseBindingInput {
  static_assert(std::is_trivially_copyable_v<Wire>);

  Wire wire{};
  PeWireObjectRef object{};
};

struct SparseConstantRangeInput {
  std::uint32_t startRegister = 0u;
  std::uint32_t registerCount = 0u;
  std::span<const std::byte> registerBytes{};

  bool present() const noexcept { return registerCount != 0u; }
};

struct SparseStateInput {
  std::span<const D9CCommandChunkWireRenderState> renderStates{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireTextureBinding>> textures{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireStreamBinding>> streams{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireShaderBinding>> shaders{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireVertexInput>> vertexInputs{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireIndexBinding>> indexBuffers{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireRenderTargetBinding>> renderTargets{};
  std::span<const SparseBindingInput<
      D9CCommandChunkWireDepthStencilBinding>> depthStencils{};
  std::span<const D9CViewport> viewports{};
  std::span<const D9CRect> scissors{};
  std::span<const D9CMaterial> materials{};
  std::span<const D9CCommandChunkWireClipPlane> clipPlanes{};
  std::span<const D9CDrawPacketTextureStageState> textureStageStates{};
  std::span<const D9CDrawPacketSamplerState> samplerStates{};
  std::span<const D9CDrawPacketTransform> transforms{};
  std::span<const D9CCommandChunkWireLight> lights{};
  std::span<const D9CCommandChunkWireLightEnable> lightEnables{};
  SparseConstantRangeInput vsFloatConstants{};
  SparseConstantRangeInput vsIntConstants{};
  SparseConstantRangeInput vsBoolConstants{};
  SparseConstantRangeInput psFloatConstants{};
  SparseConstantRangeInput psIntConstants{};
  SparseConstantRangeInput psBoolConstants{};
  std::span<const std::byte> upIndexData{};
  std::span<const std::byte> upVertexData{};
};

bool appendSparseRecord(CommandChunkBuilder& builder,
                          std::uint32_t type,
                          D9CCommandChunkWireDrawHeader draw,
                          const SparseStateInput& state) noexcept;
bool appendApplyState(CommandChunkBuilder& builder,
                        std::uint32_t flags,
                        const SparseStateInput& state) noexcept;

bool appendSetConstants(
    CommandChunkBuilder& builder, std::uint32_t type,
    std::uint32_t startRegister, std::uint32_t registerCount,
    std::span<const std::byte> registerBytes) noexcept;
bool appendClear(CommandChunkBuilder& builder,
                   D9CCommandChunkWireClear fixed,
                   std::span<const D9CRect> rects) noexcept;
bool appendPresent(CommandChunkBuilder& builder,
                     D9CCommandChunkWirePresent fixed,
                     const PeWireObjectRef& source) noexcept;
bool appendStretchRect(CommandChunkBuilder& builder,
                         D9CCommandChunkWireStretchRect fixed,
                         const PeWireObjectRef& src,
                         const PeWireObjectRef& dst) noexcept;
bool appendColorFill(CommandChunkBuilder& builder,
                       D9CCommandChunkWireColorFill fixed,
                       const PeWireObjectRef& surface) noexcept;
bool appendUpdateTexture(CommandChunkBuilder& builder,
                           const PeWireObjectRef& src,
                           const PeWireObjectRef& dst) noexcept;
bool appendUpdateSurface(CommandChunkBuilder& builder,
                           D9CCommandChunkWireUpdateSurface fixed,
                           const PeWireObjectRef& src,
                           const PeWireObjectRef& dst) noexcept;
bool appendQueryIssue(CommandChunkBuilder& builder,
                        std::uint32_t flags,
                        const PeWireObjectRef& query) noexcept;
bool appendReadback(CommandChunkBuilder& builder,
                      const PeWireObjectRef& src,
                      const PeWireObjectRef& dst) noexcept;
bool appendReszDepthResolve(CommandChunkBuilder& builder,
                              const PeWireObjectRef& msaaDepth,
                              const PeWireObjectRef& intzDest) noexcept;


}  // namespace dxmt9::d3d9::pe
