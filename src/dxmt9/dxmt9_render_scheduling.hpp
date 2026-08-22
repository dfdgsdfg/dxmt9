#pragma once

#include <cstdint>
#include <string_view>

namespace dxmt9::render {

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
