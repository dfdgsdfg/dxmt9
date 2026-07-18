#include "device_c_provider.hpp"
#include "device_c_chunk_v2_registry.hpp"

#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace dxmt9::d3d9 {

namespace {

std::atomic<std::uint64_t> gNextWireRegistryId{1u};

std::uint32_t allocateWireRegistryId() {
  const auto id = gNextWireRegistryId.fetch_add(1u, std::memory_order_relaxed);
  if (id == 0u || id > std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error("wire object registry ID space exhausted");
  }
  return static_cast<std::uint32_t>(id);
}

}  // namespace

WireObjectRegistry::WireObjectRegistry()
    : registryId_(allocateWireRegistryId()) {}

bool WireObjectRegistry::identityMatches(const Slot& slot, std::uint32_t kind,
                                         std::uint32_t generation) {
  return !slot.retired && slot.object != nullptr && slot.kind == kind &&
         slot.generation == generation && generation != 0u;
}

D9CWireObjectIdentity WireObjectRegistry::insert(std::uint32_t kind,
                                                 void* object) {
  if (!object || kind > D9C_CHUNK_HANDLE_KIND_QUERY) {
    return {};
  }

  std::lock_guard lock(mutex_);
  std::uint32_t slotIndex = 0u;
  if (!freeSlots_.empty()) {
    slotIndex = freeSlots_.back();
    freeSlots_.pop_back();
  } else {
    if (slots_.size() >= std::numeric_limits<std::uint32_t>::max()) {
      return {};
    }
    slotIndex = static_cast<std::uint32_t>(slots_.size());
    slots_.push_back({});
  }

  auto& slot = slots_[static_cast<std::size_t>(slotIndex)];
  if (slot.retired || slot.object || slot.generation == 0u) {
    return {};
  }
  slot.object = object;
  slot.kind = kind;
  ++activeCount_;
  return D9CWireObjectIdentity{
      .kind = kind,
      .generation = slot.generation,
      .objectId = (static_cast<std::uint64_t>(registryId_) << 32u) |
                  (static_cast<std::uint64_t>(slotIndex) + 1u),
  };
}

bool WireObjectRegistry::erase(const D9CWireObjectIdentity& identity,
                               const void* object) {
  if (!object || identity.objectId == 0u || identity.generation == 0u) {
    return false;
  }

  std::lock_guard lock(mutex_);
  auto* slot = findLocked(identity.objectId);
  if (!slot || !identityMatches(*slot, identity.kind, identity.generation) ||
      slot->object != object) {
    return false;
  }

  slot->object = nullptr;
  slot->kind = 0u;
  --activeCount_;
  const auto advance = advanceGeneration(slot->generation);
  slot->generation = advance.generation;
  slot->retired = advance.retired;
  if (!slot->retired) {
    freeSlots_.push_back(
        static_cast<std::uint32_t>((identity.objectId & 0xffffffffu) - 1u));
  }
  return true;
}

bool WireObjectRegistry::contains(const D9CWireObjectIdentity& identity,
                                  const void* object) const {
  std::lock_guard lock(mutex_);
  const auto* slot = findLocked(identity.objectId);
  return slot && identityMatches(*slot, identity.kind, identity.generation) &&
         (!object || slot->object == object);
}

bool WireObjectRegistry::resolveAndRetain(
    std::span<const D9CCommandChunkWireHandleEntryV2> entries,
    std::span<void*> objects,
    RetainFn retain) const {
  if (objects.size() != entries.size() || (!entries.empty() && !retain)) {
    return false;
  }

  std::lock_guard lock(mutex_);
  for (const auto& entry : entries) {
    const auto* slot = findLocked(entry.objectId);
    if (!slot || !identityMatches(*slot, entry.kind, entry.generation)) {
      return false;
    }
  }

  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto* slot = findLocked(entries[i].objectId);
    objects[i] = slot->object;
    retain(entries[i].kind, slot->object);
  }
  return true;
}

std::size_t WireObjectRegistry::activeCount() const {
  std::lock_guard lock(mutex_);
  return activeCount_;
}

const WireObjectRegistry::Slot* WireObjectRegistry::findLocked(
    std::uint64_t objectId) const {
  if (objectId == 0u) {
    return nullptr;
  }
  const auto registryId = static_cast<std::uint32_t>(objectId >> 32u);
  const auto slotId = static_cast<std::uint32_t>(objectId & 0xffffffffu);
  if (registryId != registryId_ || slotId == 0u || slotId > slots_.size()) {
    return nullptr;
  }
  return &slots_[static_cast<std::size_t>(slotId - 1u)];
}

WireObjectRegistry::Slot* WireObjectRegistry::findLocked(
    std::uint64_t objectId) {
  return const_cast<Slot*>(
      static_cast<const WireObjectRegistry*>(this)->findLocked(objectId));
}

}  // namespace dxmt9::d3d9

namespace {

template <typename Object>
int32_t copyWireIdentity(Object* object, std::uint32_t expectedKind,
                         D9CWireObjectIdentity* out) {
  if (!out) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  std::memset(out, 0, sizeof(*out));
  if (!object || !object->device ||
      object->wireIdentity.kind != expectedKind ||
      !object->device->wireObjects.contains(object->wireIdentity, object)) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }
  *out = object->wireIdentity;
  return dxmt9::core::D3D_OK;
}

}  // namespace

extern "C" int32_t dxmt9c_device_negotiate_command_chunk(
    D9CDevice* device, D9CCommandChunkNegotiation* negotiation) {
  if (!device || !negotiation || negotiation->reserved0 != 0u ||
      negotiation->reserved1 != 0u || negotiation->reserved2 != 0u ||
      negotiation->reserved3 != 0u) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  negotiation->unixSupportedVersions = D9C_COMMAND_CHUNK_CAP_VERSION_1;
  negotiation->selectedVersion = 0u;
  if ((negotiation->peSupportedVersions &
       D9C_COMMAND_CHUNK_CAP_VERSION_1) == 0u) {
    return dxmt9::core::D3DERR_INVALIDCALL;
  }

  negotiation->selectedVersion = D9C_COMMAND_CHUNK_VERSION;
  return dxmt9::core::D3D_OK;
}

extern "C" int32_t dxmt9c_texture_get_wire_identity(
    D9CTexture* texture, D9CWireObjectIdentity* out) {
  return copyWireIdentity(texture, D9C_CHUNK_HANDLE_KIND_TEXTURE, out);
}

extern "C" int32_t dxmt9c_buffer_get_wire_identity(
    D9CBuffer* buffer, D9CWireObjectIdentity* out) {
  return copyWireIdentity(buffer, D9C_CHUNK_HANDLE_KIND_BUFFER, out);
}

extern "C" int32_t dxmt9c_surface_get_wire_identity(
    D9CSurface* surface, D9CWireObjectIdentity* out) {
  return copyWireIdentity(surface, D9C_CHUNK_HANDLE_KIND_SURFACE, out);
}

extern "C" int32_t dxmt9c_shader_get_wire_identity(
    D9CShader* shader, D9CWireObjectIdentity* out) {
  return copyWireIdentity(shader, D9C_CHUNK_HANDLE_KIND_SHADER, out);
}

extern "C" int32_t dxmt9c_vdecl_get_wire_identity(
    D9CVertexDecl* declaration, D9CWireObjectIdentity* out) {
  return copyWireIdentity(declaration, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, out);
}

extern "C" int32_t dxmt9c_query_get_wire_identity(
    D9CQuery* query, D9CWireObjectIdentity* out) {
  return copyWireIdentity(query, D9C_CHUNK_HANDLE_KIND_QUERY, out);
}
