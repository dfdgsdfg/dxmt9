#include "device_c_ordered_control.hpp"

#include <cstring>

namespace dxmt9::d3d9 {
namespace {

template <typename T>
bool loadFixed(const ImportedRecordView& record, T& value) noexcept {
  if (record.payload.size() < sizeof(T)) {
    return false;
  }
  std::memcpy(&value, record.payload.data(), sizeof(T));
  return true;
}

bool handleWithinRecord(const OrderedControlDisposition& disposition,
                        std::uint32_t handleIndex) noexcept {
  const auto end = static_cast<std::uint64_t>(disposition.firstHandle) +
                   disposition.handleCount;
  return handleIndex != kNoOrderedControlHandleIndex &&
         handleIndex >= disposition.firstHandle && handleIndex < end;
}

}  // namespace

bool OrderedControlDisposition::valid() const noexcept {
  if (rawOrdinal == 0 || recordIndex == kNoOrderedControlRecordIndex ||
      handleCount == 0 || !handleWithinRecord(*this, primaryHandleIndex)) {
    return false;
  }

  switch (kind) {
  case OrderedControlKind::Query:
    return recordType == D9C_COMMAND_RECORD_QUERY_ISSUE &&
           requiredReleaseAction ==
               core::metalqueue::SessionReleaseAction::ClosePass &&
           handleCount == 1 &&
           secondaryHandleIndex == kNoOrderedControlHandleIndex;
  case OrderedControlKind::Readback:
    return recordType == D9C_COMMAND_RECORD_READBACK &&
           requiredReleaseAction ==
               core::metalqueue::SessionReleaseAction::SubmitAndWait &&
           handleCount == 2 && controlFlags == 0 &&
           handleWithinRecord(*this, secondaryHandleIndex) &&
           primaryHandleIndex != secondaryHandleIndex;
  case OrderedControlKind::UpdateTexture:
    return recordType == D9C_COMMAND_RECORD_UPDATE_TEXTURE &&
           requiredReleaseAction ==
               core::metalqueue::SessionReleaseAction::SubmitSession &&
           handleCount == 2 && controlFlags == 0 &&
           handleWithinRecord(*this, secondaryHandleIndex) &&
           primaryHandleIndex != secondaryHandleIndex;
  case OrderedControlKind::Invalid:
    return false;
  }
  return false;
}

std::optional<OrderedControlDisposition> makeOrderedControlDisposition(
    const ImportedRecordView& record,
    std::uint64_t rawOrdinal,
    std::size_t recordIndex) noexcept {
  if (rawOrdinal == 0 ||
      recordIndex >= kNoOrderedControlRecordIndex) {
    return std::nullopt;
  }

  OrderedControlDisposition disposition{
      .rawOrdinal = rawOrdinal,
      .recordIndex = static_cast<std::uint32_t>(recordIndex),
      .recordType = record.header.type,
      .firstHandle = record.header.firstHandle,
      .handleCount = record.header.handleCount,
  };
  switch (record.header.type) {
  case D9C_COMMAND_RECORD_QUERY_ISSUE: {
    D9CCommandChunkWireQueryIssue query{};
    if (!loadFixed(record, query)) {
      return std::nullopt;
    }
    disposition.kind = OrderedControlKind::Query;
    disposition.requiredReleaseAction =
        core::metalqueue::SessionReleaseAction::ClosePass;
    disposition.primaryHandleIndex = query.queryHandleIndex;
    disposition.controlFlags = query.flags;
    break;
  }
  case D9C_COMMAND_RECORD_READBACK: {
    D9CCommandChunkWireReadback readback{};
    if (!loadFixed(record, readback)) {
      return std::nullopt;
    }
    disposition.kind = OrderedControlKind::Readback;
    disposition.requiredReleaseAction =
        core::metalqueue::SessionReleaseAction::SubmitAndWait;
    disposition.primaryHandleIndex = readback.srcHandleIndex;
    disposition.secondaryHandleIndex = readback.dstHandleIndex;
    break;
  }
  case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
    D9CCommandChunkWireUpdateTexture update{};
    if (!loadFixed(record, update)) {
      return std::nullopt;
    }
    disposition.kind = OrderedControlKind::UpdateTexture;
    disposition.requiredReleaseAction =
        core::metalqueue::SessionReleaseAction::SubmitSession;
    disposition.primaryHandleIndex = update.srcHandleIndex;
    disposition.secondaryHandleIndex = update.dstHandleIndex;
    break;
  }
  default:
    return std::nullopt;
  }

  return disposition.valid()
             ? std::optional<OrderedControlDisposition>(disposition)
             : std::nullopt;
}

}  // namespace dxmt9::d3d9
