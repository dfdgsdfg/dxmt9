#pragma once

#include "dxmt9_cpu_supply_trace.hpp"

namespace dxmt9::core { struct ChunkSlot; }

namespace dxmt9::diagnostic {

// Cold provider-call observer only. No Slot fields, wire ABI, or lifetime token.
// A callback must delegate to the unchanged validator with this hook disabled.
using SlotValidationProbe = bool (*)(const core::ChunkSlot&) noexcept;
inline thread_local SlotValidationProbe slotValidationProbe = nullptr;

class ScopedSlotValidationProbe {
public:
  explicit ScopedSlotValidationProbe(SlotValidationProbe probe) noexcept
      : previous_(slotValidationProbe) { slotValidationProbe = probe; }
  ~ScopedSlotValidationProbe() { slotValidationProbe = previous_; }
  ScopedSlotValidationProbe(const ScopedSlotValidationProbe&) = delete;
  ScopedSlotValidationProbe& operator=(const ScopedSlotValidationProbe&) = delete;
private:
  SlotValidationProbe previous_;
};

}  // namespace dxmt9::diagnostic
