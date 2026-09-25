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
  FramegraphPoolBinding, ChunkBodyBegin, ChunkBodyEnd,
  ChunkCommandBegin, ChunkCommandEnd, ChunkSplitBegin, ChunkSplitEnd,
  EncodeDrawBegin, EncodeDrawEnd,
  ParallelObserveBegin, ParallelObserveCounts, ParallelObserveDraws,
  ParallelObserveChildren, ParallelObserveReject, ParallelObserveEnd,
  ParallelObserveStatus, ParallelObserveImbalance,
  ParallelObserveResources,
  ParallelAccessRange, ParallelAccessCounts, ParallelAccessTouches,
  ParallelAccessReuse, ParallelAccessFlags,
  PreparedBegin, PreparedAllocated, PreparedAppended, PreparedSealed,
  PreparedStatus, PreparedIdentities, PreparedAccesses, PreparedRanges,
  PreparedUnresolved, PreparedEnd,
  PreparedSourceIndex, PreparedSourceGeneration, PreparedStorageGeneration,
  PreparedStoragePages,
  PreparedScreen,
  PreparedStorageCapacity,
  PreparedCardinalityBegin, PreparedCardinalityStatus, PreparedCardinalityCommands,
  PreparedCardinalityUnresolved, PreparedCardinalityAccesses,
  PreparedCardinalityIdentities, PreparedCardinalityEnd,
  FramegraphPlanningPhase,
};

inline bool planningSplitEnabled() noexcept {
  static const bool value = [] {
    const char* flag = std::getenv("DXMT9_CPU_SUPPLY_PLANNING_SPLIT");
    return enabled() && flag && flag[0] && flag[0] != '0';
  }();
  return value;
}

inline bool commandSplitEnabled() noexcept {
  static const bool value = [] {
    const char* flag = std::getenv("DXMT9_CPU_SUPPLY_COMMAND_SPLIT");
    return enabled() && flag && flag[0] && flag[0] != '0';
  }();
  return value;
}

inline bool drawSplitEnabled() noexcept {
  static const bool value = [] {
    const char* flag = std::getenv("DXMT9_CPU_SUPPLY_DRAW_SPLIT");
    return commandSplitEnabled() && flag && flag[0] && flag[0] != '0';
  }();
  return value;
}

inline bool parallelObserveEnabled() noexcept {
  static const bool value = [] {
    const char* flag = std::getenv("DXMT9_CPU_SUPPLY_PARALLEL_OBSERVER");
    return enabled() && flag && flag[0] && flag[0] != '0';
  }();
  return value;
}

inline bool preparedObserveEnabled() noexcept {
  static const bool value = [] {
    const char* flag = std::getenv("DXMT9_CPU_SUPPLY_PREPARED_OBSERVER");
    return enabled() && flag && flag[0] && flag[0] != '0';
  }();
  return value;
}

// Call behind an enabled diagnostic gate. This is observation admission only,
// not a source-lifetime or scheduler capability; endpoints may still clip.
bool windowActive() noexcept;

// Call only behind enabled(). No output, allocation or sink mutex in the
// measured window. Timestamp/cpu/TID and a bounded event live in cold storage.
void record(Event event, const void* owner, std::uint64_t id = 0,
            std::uint64_t value = 0) noexcept;
void present(std::uint64_t ordinal) noexcept;

}  // namespace dxmt9::supply
