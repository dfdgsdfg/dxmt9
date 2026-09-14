#pragma once

#include "dxmt9_cpu_ready_tape.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace dxmt9::encoders {

// Diagnostic-owned values only. They confer no replay, retention or completion
// authority. The producer records them under the queue lock before Ready can
// be consumed; the observer matches the actual encoder SourceRef and seq.
struct ColorAttachmentCaptureSource {
  std::uintptr_t owner = 0u;
  core::CpuReadyPublicationTicket ticket{};
};

template<std::size_t Capacity>
class ColorAttachmentCaptureTable {
  static_assert(Capacity != 0u);
  std::array<ColorAttachmentCaptureSource, Capacity> entries_{};

public:
  void record(std::uintptr_t owner,
              const core::CpuReadyPublicationTicket& ticket) noexcept {
    if (owner == 0u || !ticket.valid() || ticket.seqId == 0u ||
        ticket.sourceOrdinal == 0u) return;
    // Overwrite can lose coverage, never manufacture a match. No unbounded
    // per-frame log or resource ownership is retained here.
    entries_[ticket.seqId % Capacity] = {owner, ticket};
  }

  std::optional<ColorAttachmentCaptureSource> find(
      std::uintptr_t owner, std::uint64_t seq,
      const core::CpuReadyTape::SourceRef& source) const noexcept {
    if (owner == 0u || seq == 0u || !source.valid()) return std::nullopt;
    const auto& entry = entries_[seq % Capacity];
    if (entry.owner != owner || entry.ticket.seqId != seq ||
        entry.ticket.id != source.id || entry.ticket.storage != source.storage) {
      return std::nullopt;
    }
    return entry;
  }

  void forget(std::uintptr_t owner) noexcept {
    for (auto& entry : entries_) {
      if (entry.owner == owner) entry = {};
    }
  }
};

bool captureColorAttachmentObservationEnabled();
void recordCaptureColorAttachmentSource(
    std::uintptr_t owner, const core::CpuReadyPublicationTicket& ticket) noexcept;
std::optional<ColorAttachmentCaptureSource> findCaptureColorAttachmentSource(
    std::uintptr_t owner, std::uint64_t seq,
    const core::CpuReadyTape::SourceRef& source) noexcept;
void forgetCaptureColorAttachmentOwner(std::uintptr_t owner) noexcept;

}  // namespace dxmt9::encoders
