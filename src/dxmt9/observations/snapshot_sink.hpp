#pragma once

#include "metadata.hpp"
#include "../dxmt9_trace.hpp"
#include <atomic>

namespace dxmt9::observations {
// Projection ordinals are process/source-local, never legacy log row numbers.
inline std::atomic<std::uint64_t> snapshot_ordinals[7]{};
class SnapshotSink {
 public:
  SnapshotSink(Source source, std::uint16_t mode, std::uint16_t stride,
               std::uint64_t work = 0) noexcept {
    const auto kind = static_cast<std::uint16_t>(source);
    if (kind < 3 || kind > 6 || !tracing::enabled()) return;
    generation_ = tracing::capture_generation();
    if (!generation_) return;
    event_.timestamp_ns = tracing::now_ns();
    event_.observation_kind = static_cast<std::uint16_t>(source);
    event_.observation_record = ++snapshot_ordinals[event_.observation_kind];
    event_.observation_mode = mode;
    event_.observation_stride = stride;
    event_.work_id = work;
    event_.category = static_cast<std::uint8_t>(source == Source::QueryBackend ||
        source == Source::QueryReceipt ? tracing::Category::Wait : tracing::Category::Encode);
    event_.event_id = static_cast<std::uint16_t>(tracing::EventId::Metric);
  }
  bool value(std::uint16_t index, std::uint64_t value) noexcept {
    if (!generation_ || failed_) return false;
    const auto descriptor = field(event_.observation_kind, index);
    const bool identity = descriptor.unit[0] == 'i';
    event_.observation_field = index;
    event_.metric_value = value;
    event_.metric_id = static_cast<std::uint16_t>(identity ? tracing::Metric::SnapshotIdentity :
        descriptor.unit[0] == 'n' ? tracing::Metric::SnapshotElapsedNs : tracing::Metric::SnapshotCount);
    event_.phase = static_cast<std::uint8_t>(identity ? tracing::Phase::Instant : tracing::Phase::Counter);
    if (index != count_ || count_ >= events_.size() || descriptor.unit[0] == 'u' ||
        (!identity && value > INT64_MAX)) {
      failed_ = true;
      // Generation-qualified failure; never corrupt a subsequent session.
      auto invalid = event_;
      invalid.category = static_cast<std::uint8_t>(tracing::Category::Count);
      (void)tracing::emit_batch_captured({&invalid, 1}, generation_, 1);
      return false;
    }
    events_[count_++] = event_;
    // Earlier returns indicate staging. Only the final field performs admission;
    // a close/rearm or exhausted buffer can never retain a partial snapshot.
    return count_ != field_count(event_.observation_kind) ||
        tracing::emit_observations({events_.data(), count_}, generation_);
  }
 private:
  // Uninitialized slots are never read; avoid zero-filling the bounded batch.
  std::array<tracing::TraceEvent, max_snapshot_fields> events_;
  std::size_t count_ = 0;
  bool failed_ = false;
  tracing::TraceEvent event_{};
  std::uint64_t generation_ = 0;
};
}  // namespace dxmt9::observations
