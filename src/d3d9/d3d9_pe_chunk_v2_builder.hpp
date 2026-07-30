#pragma once

#include "d3d9_pe_retainer.hpp"
#include "device_c_chunk_v2_schema.hpp"

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
bool lookupCachedWireObjectRef(void* object, std::uint32_t expectedKind,
                               PeWireObjectRef& out) noexcept;
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

struct CommandChunkV2BuilderCapacities {
  std::size_t records = 64u;
  std::size_t handles = 256u;
  std::size_t payloadBytes = 256u * 1024u;
  std::size_t sealedBytes = 272u * 1024u;
};

struct SealedCommandChunkV2 {
  std::span<const std::byte> blob{};
  std::uint32_t recordCount = 0u;
  std::uint32_t handleCount = 0u;

  bool valid() const noexcept { return !blob.empty(); }
};

class CommandChunkV2Builder {
 public:
  explicit CommandChunkV2Builder(
      const CommandChunkV2BuilderCapacities& capacities = {});
  ~CommandChunkV2Builder() = default;

  CommandChunkV2Builder(const CommandChunkV2Builder&) = delete;
  CommandChunkV2Builder& operator=(const CommandChunkV2Builder&) = delete;

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

  SealedCommandChunkV2 seal() noexcept;
  void reset() noexcept;

  bool recordActive() const noexcept { return active_.active; }
  bool sealed() const noexcept { return sealed_; }
  std::size_t recordCount() const noexcept { return records_.size(); }
  std::size_t handleCount() const noexcept { return handles_.size(); }
  std::size_t payloadBytes() const noexcept { return payload_.size(); }
  std::size_t retainedObjectCount() const noexcept { return retainer_.size(); }
  bool referencesObject(void* object) const noexcept;

  const std::vector<D9CCommandChunkWireRecordHeaderV2>& recordsForTest()
      const noexcept {
    return records_;
  }
  const std::vector<D9CCommandChunkWireHandleEntryV2>& handlesForTest()
      const noexcept {
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

  std::vector<D9CCommandChunkWireRecordHeaderV2> records_;
  std::vector<D9CCommandChunkWireHandleEntryV2> handles_;
  std::vector<void*> handleObjects_;
  std::vector<std::byte> payload_;
  std::vector<std::byte> sealedBlob_;
  D3D9PePendingCommandRetainer retainer_;
  ActiveRecord active_{};
  bool sealed_ = false;
};

template <typename Wire>
struct SparseBindingV2Input {
  static_assert(std::is_trivially_copyable_v<Wire>);

  Wire wire{};
  PeWireObjectRef object{};
};

struct SparseConstantRangeV2Input {
  std::uint32_t startRegister = 0u;
  std::uint32_t registerCount = 0u;
  std::span<const std::byte> registerBytes{};

  bool present() const noexcept { return registerCount != 0u; }
};

struct SparseStateV2Input {
  std::span<const D9CCommandChunkWireRenderStateV2> renderStates{};
  std::span<const SparseBindingV2Input<
      D9CCommandChunkWireTextureBindingV2>> textures{};
  std::span<const SparseBindingV2Input<
      D9CCommandChunkWireStreamBindingV2>> streams{};
  std::span<const SparseBindingV2Input<
      D9CCommandChunkWireShaderBindingV2>> shaders{};
  std::span<const SparseBindingV2Input<
      D9CCommandChunkWireVertexInputV2>> vertexInputs{};
  std::span<const SparseBindingV2Input<
      D9CCommandChunkWireIndexBindingV2>> indexBuffers{};
  std::span<const SparseBindingV2Input<
      D9CCommandChunkWireRenderTargetBindingV2>> renderTargets{};
  std::span<const SparseBindingV2Input<
      D9CCommandChunkWireDepthStencilBindingV2>> depthStencils{};
  std::span<const D9CViewport> viewports{};
  std::span<const D9CRect> scissors{};
  std::span<const D9CMaterial> materials{};
  std::span<const D9CCommandChunkWireClipPlaneV2> clipPlanes{};
  std::span<const D9CDrawPacketTextureStageState> textureStageStates{};
  std::span<const D9CDrawPacketSamplerState> samplerStates{};
  std::span<const D9CDrawPacketTransform> transforms{};
  std::span<const D9CCommandChunkWireLightV2> lights{};
  std::span<const D9CCommandChunkWireLightEnableV2> lightEnables{};
  SparseConstantRangeV2Input vsFloatConstants{};
  SparseConstantRangeV2Input vsIntConstants{};
  SparseConstantRangeV2Input vsBoolConstants{};
  SparseConstantRangeV2Input psFloatConstants{};
  SparseConstantRangeV2Input psIntConstants{};
  SparseConstantRangeV2Input psBoolConstants{};
  std::span<const std::byte> upIndexData{};
  std::span<const std::byte> upVertexData{};
};

bool appendSparseRecordV2(CommandChunkV2Builder& builder,
                          std::uint32_t type,
                          D9CCommandChunkWireDrawHeaderV2 draw,
                          const SparseStateV2Input& state) noexcept;
bool appendApplyStateV2(CommandChunkV2Builder& builder,
                        std::uint32_t flags,
                        const SparseStateV2Input& state) noexcept;

bool appendSetConstantsV2(
    CommandChunkV2Builder& builder, std::uint32_t type,
    std::uint32_t startRegister, std::uint32_t registerCount,
    std::span<const std::byte> registerBytes) noexcept;
bool appendClearV2(CommandChunkV2Builder& builder,
                   D9CCommandChunkWireClearV2 fixed,
                   std::span<const D9CRect> rects) noexcept;
bool appendPresentV2(CommandChunkV2Builder& builder,
                     const D9CCommandChunkWirePresentV2& fixed) noexcept;
bool appendStretchRectV2(CommandChunkV2Builder& builder,
                         D9CCommandChunkWireStretchRectV2 fixed,
                         const PeWireObjectRef& src,
                         const PeWireObjectRef& dst) noexcept;
bool appendColorFillV2(CommandChunkV2Builder& builder,
                       D9CCommandChunkWireColorFillV2 fixed,
                       const PeWireObjectRef& surface) noexcept;
bool appendUpdateTextureV2(CommandChunkV2Builder& builder,
                           const PeWireObjectRef& src,
                           const PeWireObjectRef& dst) noexcept;
bool appendUpdateSurfaceV2(CommandChunkV2Builder& builder,
                           D9CCommandChunkWireUpdateSurfaceV2 fixed,
                           const PeWireObjectRef& src,
                           const PeWireObjectRef& dst) noexcept;
bool appendQueryIssueV2(CommandChunkV2Builder& builder,
                        std::uint32_t flags,
                        const PeWireObjectRef& query) noexcept;
bool appendReadbackV2(CommandChunkV2Builder& builder,
                      const PeWireObjectRef& src,
                      const PeWireObjectRef& dst) noexcept;
bool appendReszDepthResolveV2(CommandChunkV2Builder& builder,
                              const PeWireObjectRef& msaaDepth,
                              const PeWireObjectRef& intzDest) noexcept;


}  // namespace dxmt9::d3d9::pe
