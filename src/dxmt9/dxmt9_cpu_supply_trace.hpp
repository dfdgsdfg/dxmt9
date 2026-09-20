#pragma once

#include <cstdint>
#include <cstdlib>

namespace dxmt9::supply {

// Diagnostic storage is cold, process-local and never part of a wire/Slot.
inline bool enabled() noexcept {
  static const bool value = [] {
    const char* path = std::getenv("DXMT9_CPU_SUPPLY_TRACE");
    return path && path[0] && path[0] != '0';
  }();
  return value;
}

enum class Event : std::uint32_t {
  Present, ImportBegin, ImportEnd, RawReady, MutationReady, StateBlockReady,
  ReplayBegin, ReplayEnd, QueueEmpty, QueueReserved, QueueReady,
  PopAttempt, PopWait, PopWake, Publish, EncodeBegin, EncodeEnd, Submit,
  ReplayLedger, WriterWaitBegin, WriterWaitEnd, CommitWaitBegin, CommitWaitEnd,
  PublishReason, PublishCommands, ScopeBoundary, QueryIssue,
  CompletionDequeue, CompletionObserved, Retire,
  EncodePrefetchBegin, EncodePrefetchEnd, EncodeBackendBegin, EncodeBackendEnd,
  FramegraphEncodeBegin, FramegraphEncodeEnd,
};

// Call only behind enabled(). No output, allocation or sink mutex in the
// measured window. Timestamp/cpu/TID and a bounded event live in cold storage.
void record(Event event, const void* owner, std::uint64_t id = 0,
            std::uint64_t value = 0) noexcept;
void present(std::uint64_t ordinal) noexcept;

}  // namespace dxmt9::supply
