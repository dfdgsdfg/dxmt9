#pragma once

#include "d3d9_pe_retainer.hpp"
#include "device_c_chunk_v2_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
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

template <typename Object, typename Getter>
bool cacheWireObjectRef(Object* object, std::uint32_t expectedKind,
                        Getter&& getter, PeWireObjectRef& out) {
  out = {};
  if (!object) {
    return false;
  }
  D9CWireObjectIdentity identity{};
  if (getter(object, &identity) < 0 || identity.kind != expectedKind ||
      identity.generation == 0u || identity.objectId == 0u) {
    return false;
  }
  out = PeWireObjectRef{
      .identity = identity,
      .object = object,
  };
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

}  // namespace dxmt9::d3d9::pe
