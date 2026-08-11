#pragma once

#include <cstdint>
#include <string_view>

namespace dxmt9::render {

enum class PartitionExecutionMode : std::uint8_t {
  IdentitySerial,
  ExplicitSerial,
};

enum class PartitionModeRequest : std::uint8_t {
  Default,
  Identity,
  Serial,
  Parallel,
  Invalid,
};

enum class PartitionModeFallback : std::uint8_t {
  None,
  ParallelUnsupported,
  InvalidValue,
};

struct RenderPartitionConfig {
  PartitionModeRequest requested = PartitionModeRequest::Default;
  PartitionExecutionMode resolved = PartitionExecutionMode::IdentitySerial;
  PartitionModeFallback fallback = PartitionModeFallback::None;

  friend constexpr bool operator==(const RenderPartitionConfig&,
                                   const RenderPartitionConfig&) = default;
};

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
        .resolved = PartitionExecutionMode::IdentitySerial,
        .fallback = PartitionModeFallback::ParallelUnsupported,
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
  }
  return "invalid";
}

}  // namespace dxmt9::render
