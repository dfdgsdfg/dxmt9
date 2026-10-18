#pragma once

#include "diagnostic_sink.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dxmt9::observations {

// Shutdown reports are acquired into a fixed snapshot, then projected as one
// contiguous trace batch. Contention and overflow lose the whole report.
class ValueBatch {
 public:
  static constexpr std::size_t capacity = 3072;

  explicit ValueBatch(const DiagnosticContext& context) noexcept
      : context_(context) {
    if (!context_.generation) return;
    for (auto& slot : slots()) {
      bool expected = false;
      if (slot.busy.compare_exchange_strong(expected, true,
                                           std::memory_order_acquire)) {
        slot_ = &slot;
        slot_->count = 0;
        break;
      }
    }
    if (!slot_) diagnostic_loss(context_.generation);
  }

  ValueBatch(const ValueBatch&) = delete;
  ValueBatch& operator=(const ValueBatch&) = delete;

  ~ValueBatch() {
    if (slot_) slot_->busy.store(false, std::memory_order_release);
  }

  void value(Source source, std::string_view key, std::uint64_t value,
             const char* semantics) noexcept {
    if (!context_.generation || failed_) return;
    if (!slot_ || slot_->count == capacity || key.size() >= 128) {
      failed_ = true;
      return;
    }
    Semantics meaning{};
    if (!parseSemantics(semantics, meaning)) {
      failed_ = true;
      return;
    }
    auto& record = slot_->records[slot_->count++];
    record.source = source;
    record.value = value;
    record.semantics = meaning;
    std::memcpy(record.key, key.data(), key.size());
    record.key[key.size()] = '\0';
  }

  void commit() noexcept {
    if (!context_.generation || committed_) return;
    committed_ = true;
    if (!slot_ || failed_) {
      if (slot_) diagnostic_loss(context_.generation);
      return;
    }
    for (std::size_t i = 0; i < slot_->count; ++i) {
      const auto& record = slot_->records[i];
      auto& event = slot_->events[i];
      event = diagnostic_event(record.source, context_);
      std::memcpy(event.diagnostic_schema, record.key, std::strlen(record.key));
      std::strcpy(event.diagnostic_payload, semanticsText(record.semantics));
      event.metric_value = record.value;
      event.observation_mode = 2;
      if (record.value <= INT64_MAX && record.semantics != Semantics::LatestId) {
        event.phase = static_cast<std::uint8_t>(tracing::Phase::Counter);
        event.metric_id = static_cast<std::uint16_t>(tracing::Metric::DiagnosticValue);
      }
      event.diagnostic_parts = 1;
    }
    if (slot_->count && !tracing::emit_observations(
            {slot_->events.data(), slot_->count}, context_.generation)) {
      diagnostic_loss(context_.generation);
    }
  }

 private:
  enum class Semantics : std::uint8_t {
    BytesCumulative,
    CountCumulative,
    PercentilePopulation,
    LatestId,
    IntegerSourceDefined,
    PercentileRingNs,
    SourceDefinedNs,
    LatestPixels,
  };

  struct ValueRecord {
    Source source{};
    std::uint64_t value = 0;
    Semantics semantics{};
    char key[128]{};
  };

  struct Slot {
    std::atomic<bool> busy{false};
    std::array<ValueRecord, capacity> records{};
    std::array<tracing::TraceEvent, capacity> events{};
    std::size_t count = 0;
  };

  static const char* semanticsText(Semantics semantics) noexcept {
    switch (semantics) {
      case Semantics::BytesCumulative: return "bytes:cumulative";
      case Semantics::CountCumulative: return "count:cumulative";
      case Semantics::PercentilePopulation: return "count:percentile-population";
      case Semantics::LatestId: return "id:latest";
      case Semantics::IntegerSourceDefined: return "integer:source-defined";
      case Semantics::PercentileRingNs: return "ns:percentile-ring";
      case Semantics::SourceDefinedNs: return "ns:source-defined";
      case Semantics::LatestPixels: return "pixels:latest";
    }
    return "";
  }

  static bool parseSemantics(const char* text, Semantics& result) noexcept {
    if (std::strcmp(text, "bytes:cumulative") == 0) result = Semantics::BytesCumulative;
    else if (std::strcmp(text, "count:cumulative") == 0) result = Semantics::CountCumulative;
    else if (std::strcmp(text, "count:percentile-population") == 0) result = Semantics::PercentilePopulation;
    else if (std::strcmp(text, "id:latest") == 0) result = Semantics::LatestId;
    else if (std::strcmp(text, "integer:source-defined") == 0) result = Semantics::IntegerSourceDefined;
    else if (std::strcmp(text, "ns:percentile-ring") == 0) result = Semantics::PercentileRingNs;
    else if (std::strcmp(text, "ns:source-defined") == 0) result = Semantics::SourceDefinedNs;
    else if (std::strcmp(text, "pixels:latest") == 0) result = Semantics::LatestPixels;
    else return false;
    return true;
  }

  static std::array<Slot, 2>& slots() noexcept {
    static std::array<Slot, 2> value{};
    return value;
  }

  const DiagnosticContext& context_;
  Slot* slot_ = nullptr;
  bool failed_ = false;
  bool committed_ = false;
};

}  // namespace dxmt9::observations
