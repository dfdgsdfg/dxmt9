#pragma once

#include <cstdint>

namespace dxmt9::trace::gpu::retirement {

enum class SlotState : std::uint8_t {
  Free,
  Initializing,
  Reserved,
  Completing,
  Ready,
  Draining,
};

constexpr bool completionTicketMatches(std::uint64_t currentSession,
                                      std::uint32_t currentSlot,
                                      std::uint64_t currentSlotGeneration,
                                      std::uint64_t ticketSession,
                                      std::uint32_t ticketSlot,
                                      std::uint64_t ticketSlotGeneration) noexcept {
  return currentSession != 0 && currentSlotGeneration != 0 &&
      currentSession == ticketSession && currentSlot == ticketSlot &&
      currentSlotGeneration == ticketSlotGeneration;
}

constexpr bool canBeginSession(bool open, std::uint32_t activeReservations,
                               bool allFree) noexcept {
  return !open && activeReservations == 0 && allFree;
}

constexpr bool mayRetireStorage(bool open, std::uint32_t activeObservers,
                                std::uint32_t pendingSamples, bool slotsFree,
                                std::uint32_t readers) noexcept {
  return !open && activeObservers == 0 && pendingSamples == 0 && slotsFree &&
      readers == 0;
}

}  // namespace dxmt9::trace::gpu::retirement
