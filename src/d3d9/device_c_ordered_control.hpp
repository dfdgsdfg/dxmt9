#pragma once

#include "device_c_chunk_v2_validate.hpp"
#include "../dxmt9/dxmt9_session_release.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace dxmt9::d3d9 {

inline constexpr std::uint32_t kNoOrderedControlRecordIndex =
    std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t kNoOrderedControlHandleIndex =
    D9C_COMMAND_CHUNK_V2_NULL_HANDLE_INDEX;

enum class OrderedControlKind : std::uint8_t {
  Invalid,
  Query,
  Readback,
  UpdateTexture,
};

// Pointer-free locator for one compatibility-path control at its exact place
// in the immutable raw record stream. The handle fields are indices into the
// raw V2 handle table; firstHandle/handleCount preserve the validated record
// slice used to resolve them later.
struct OrderedControlDisposition {
  OrderedControlKind kind = OrderedControlKind::Invalid;
  core::metalqueue::SessionReleaseAction requiredReleaseAction =
      core::metalqueue::SessionReleaseAction::ClosePass;
  std::uint64_t rawOrdinal = 0;
  std::uint32_t recordIndex = kNoOrderedControlRecordIndex;
  std::uint32_t recordType = 0;
  std::uint32_t firstHandle = 0;
  std::uint32_t handleCount = 0;
  std::uint32_t primaryHandleIndex = kNoOrderedControlHandleIndex;
  std::uint32_t secondaryHandleIndex = kNoOrderedControlHandleIndex;
  std::uint32_t controlFlags = 0;

  bool valid() const noexcept;

  friend constexpr bool operator==(const OrderedControlDisposition&,
                                   const OrderedControlDisposition&) =
      default;
};

// Builds a disposition only from an already-imported record's wire metadata.
// It does not resolve handles, execute the control, or retain source storage.
std::optional<OrderedControlDisposition> makeOrderedControlDisposition(
    const ImportedRecordV2View& record,
    std::uint64_t rawOrdinal,
    std::size_t recordIndex) noexcept;

static_assert(std::is_trivially_copyable_v<OrderedControlDisposition>);
static_assert(std::is_standard_layout_v<OrderedControlDisposition>);

}  // namespace dxmt9::d3d9
