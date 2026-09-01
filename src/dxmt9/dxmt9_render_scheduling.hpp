#pragma once

#include <cstdint>
#include <string_view>

namespace dxmt9::render {

// Final source representation and Metal execution placement are independent
// policy axes.  The stable production topology materializes one queue-owned
// final ChunkSlot on the Replay worker and lets the dedicated encode worker
// consume it.  A future fused worker changes only placement; it does not
// reopen the source representation or reintroduce a per-draw carrier.
enum class FinalSourceStorage : std::uint8_t {
  OwnedRawFinalChunkSlot,
  CpuReadyArena,
  CompactParallelSoA,
  Count,
};

enum class EncodeExecutionPlacement : std::uint8_t {
  DedicatedEncodeThread,
  ReplayWorker,
  ParallelCoordinator,
  Count,
};

struct EncodeExecutionTopology {
  FinalSourceStorage storage = FinalSourceStorage::OwnedRawFinalChunkSlot;
  EncodeExecutionPlacement placement =
      EncodeExecutionPlacement::DedicatedEncodeThread;

  friend constexpr bool operator==(const EncodeExecutionTopology&,
                                   const EncodeExecutionTopology&) = default;
};

inline constexpr EncodeExecutionTopology kStableOwnedRawSlotTopology{};

constexpr bool encodeExecutionTopologyValid(
    EncodeExecutionTopology topology) noexcept {
  switch (topology.storage) {
  case FinalSourceStorage::OwnedRawFinalChunkSlot:
    return topology.placement ==
               EncodeExecutionPlacement::DedicatedEncodeThread ||
           topology.placement == EncodeExecutionPlacement::ReplayWorker;
  case FinalSourceStorage::CpuReadyArena:
    return topology.placement ==
           EncodeExecutionPlacement::DedicatedEncodeThread;
  case FinalSourceStorage::CompactParallelSoA:
    return topology.placement ==
           EncodeExecutionPlacement::ParallelCoordinator;
  case FinalSourceStorage::Count:
    return false;
  }
  return false;
}

// Only the stable topology is selectable by the current runtime.  Keeping
// future combinations in the algebra makes their ownership requirements
// explicit without creating an environment selector or a half-enabled lane.
constexpr bool encodeExecutionTopologyImplemented(
    EncodeExecutionTopology topology) noexcept {
  return topology == kStableOwnedRawSlotTopology;
}

constexpr std::uint8_t dedicatedEncodeWorkerCount(
    EncodeExecutionTopology topology) noexcept {
  switch (topology.placement) {
  case EncodeExecutionPlacement::DedicatedEncodeThread:
  case EncodeExecutionPlacement::ParallelCoordinator:
    return 1u;
  case EncodeExecutionPlacement::ReplayWorker:
  case EncodeExecutionPlacement::Count:
    return 0u;
  }
  return 0u;
}

enum class PartitionExecutionMode : std::uint8_t {
  IdentitySerial,
  ExplicitSerial,
  ExplicitParallel,
  Count,
};

enum class PartitionModeRequest : std::uint8_t {
  Default,
  Identity,
  Serial,
  Parallel,
  Invalid,
  Count,
};

enum class PartitionModeFallback : std::uint8_t {
  None,
  InvalidValue,
};

enum class SourceIdentityMode : std::uint8_t { EventSerial, SegmentSerial };
enum class SourceIdentityModeRequest : std::uint8_t {
  Default,
  Event,
  Segment,
  Invalid,
};

struct SourceIdentityConfig {
  SourceIdentityModeRequest requested = SourceIdentityModeRequest::Default;
  SourceIdentityMode resolved = SourceIdentityMode::EventSerial;
  PartitionModeFallback fallback = PartitionModeFallback::None;
  friend constexpr bool operator==(const SourceIdentityConfig&,
                                   const SourceIdentityConfig&) = default;
};

struct RenderPartitionConfig {
  PartitionModeRequest requested = PartitionModeRequest::Default;
  PartitionExecutionMode resolved = PartitionExecutionMode::IdentitySerial;
  PartitionModeFallback fallback = PartitionModeFallback::None;
  SourceIdentityConfig sourceIdentity{};

  friend constexpr bool operator==(const RenderPartitionConfig&,
                                   const RenderPartitionConfig&) = default;
};

constexpr SourceIdentityConfig resolveSourceIdentityConfig(
    const char* value) noexcept {
  if (!value) {
    return {};
  }
  const std::string_view selected(value);
  if (selected == "event") {
    return {.requested = SourceIdentityModeRequest::Event};
  }
  if (selected == "segment") {
    return {.requested = SourceIdentityModeRequest::Segment,
            .resolved = SourceIdentityMode::SegmentSerial};
  }
  return {.requested = SourceIdentityModeRequest::Invalid,
          .fallback = PartitionModeFallback::InvalidValue};
}

constexpr RenderPartitionConfig resolveRenderPartitionConfig(
    const char* value) noexcept {
  if (!value) {
    return {};
  }
  const std::string_view selected(value);
  if (selected == "identity") {
    return RenderPartitionConfig{
        .requested = PartitionModeRequest::Identity,
        .resolved = PartitionExecutionMode::IdentitySerial,
    };
  }
  if (selected == "serial") {
    return RenderPartitionConfig{
        .requested = PartitionModeRequest::Serial,
        .resolved = PartitionExecutionMode::ExplicitSerial,
    };
  }
  if (selected == "parallel") {
    return RenderPartitionConfig{
        .requested = PartitionModeRequest::Parallel,
        .resolved = PartitionExecutionMode::ExplicitParallel,
    };
  }
  return RenderPartitionConfig{
      .requested = PartitionModeRequest::Invalid,
      .resolved = PartitionExecutionMode::IdentitySerial,
      .fallback = PartitionModeFallback::InvalidValue,
  };
}

constexpr const char* partitionModeName(
    PartitionExecutionMode mode) noexcept {
  switch (mode) {
    case PartitionExecutionMode::IdentitySerial:
      return "identity";
    case PartitionExecutionMode::ExplicitSerial:
      return "serial";
    case PartitionExecutionMode::ExplicitParallel:
      return "parallel";
    case PartitionExecutionMode::Count:
      return "invalid";
  }
  return "identity";
}

constexpr const char* partitionModeRequestName(
    PartitionModeRequest request) noexcept {
  switch (request) {
    case PartitionModeRequest::Default:
      return "default";
    case PartitionModeRequest::Identity:
      return "identity";
    case PartitionModeRequest::Serial:
      return "serial";
    case PartitionModeRequest::Parallel:
      return "parallel";
    case PartitionModeRequest::Invalid:
      return "invalid";
    case PartitionModeRequest::Count:
      return "invalid";
  }
  return "invalid";
}

}  // namespace dxmt9::render
