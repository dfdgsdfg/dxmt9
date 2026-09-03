#pragma once

#include "dxmt9_backend_types.hpp"

#include <cstddef>
#include <limits>

namespace dxmt9::core {

// Physical bytes retained by one persistent compatibility payload. This is the
// complete payload footprint, including exact-fit-only dimensions such as
// readback records; lookup arrays are real retained storage even when their
// logical heads are empty. Provisioning price remains deliberately separate
// and fail-closed for readback because the direct branch has no readback
// appender.
inline std::size_t chunkSlotPhysicalRetainedBytes(
    const ChunkSlot& slot) noexcept {
  const auto bytes = [](std::size_t count, std::size_t element) noexcept {
    return count > std::numeric_limits<std::size_t>::max() / element
        ? std::numeric_limits<std::size_t>::max()
        : count * element;
  };
  std::size_t total = 0;
  const auto add = [&](std::size_t value) noexcept {
    total = value > std::numeric_limits<std::size_t>::max() - total
        ? std::numeric_limits<std::size_t>::max()
        : total + value;
  };
#define DXMT9_PRICE_CHUNK_SLOT_OWNER_Inline(storage, element)              \
  add(bytes(slot.storage.capacity(), sizeof(element)));
#define DXMT9_PRICE_CHUNK_SLOT_OWNER_Detached(storage, element)            \
  add(bytes(slot.storage.capacity(), sizeof(element)));                    \
  add(slot.detachedOwnerMarker.retainedBytes);
#define DXMT9_PRICE_CHUNK_SLOT_DIMENSION(                                  \
    region, plan, storage, element, physical, provision, allocation,       \
    lookup, owner)                                                         \
  DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(                      \
      DXMT9_PRICE_CHUNK_SLOT_OWNER_##owner(storage, element))
  DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(DXMT9_PRICE_CHUNK_SLOT_DIMENSION)
#undef DXMT9_PRICE_CHUNK_SLOT_DIMENSION
#undef DXMT9_PRICE_CHUNK_SLOT_OWNER_Detached
#undef DXMT9_PRICE_CHUNK_SLOT_OWNER_Inline
  return total;
}

}  // namespace dxmt9::core
