#include "device_c_render_tape_identity_transfer_v3.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace dxmt9::d3d9 {
namespace {

using Status = RenderTapeIdentityV3TransferStatus;

bool snapshotHeaderAdmitted(const RenderTapeIdentityV3Header& header) noexcept {
  return header.magic == kRenderTapeIdentityMagic &&
      header.version == kRenderTapeIdentityV3Version &&
      header.headerSize == sizeof(header) && header.captureToken &&
      header.captureEpoch && !header.eventsBytes &&
      std::all_of(header.eventsDigest.begin(), header.eventsDigest.end(),
          [](std::byte value) { return value == std::byte{}; }) &&
      !header.reserved0 && !header.reserved1 &&
      !header.reserved2 && !header.reserved3;
}

template <typename T>
bool layoutTable(RenderTapeIdentityV3Table& table,
                 std::uint64_t& cursor, const std::vector<T>& rows) noexcept {
  if (rows.size() > kRenderTapeIdentityV3MaxRows || cursor > UINT32_MAX)
    return false;
  table = {static_cast<std::uint32_t>(cursor),
           static_cast<std::uint32_t>(rows.size()), sizeof(T)};
  cursor += std::uint64_t{sizeof(T)} * rows.size();
  return cursor <= UINT32_MAX;
}

template <typename T>
Status admitTable(RenderTapeIdentityV3Table table,
                  std::uint64_t& cursor) noexcept {
  if (table.count > kRenderTapeIdentityV3MaxRows) return Status::CapacityExceeded;
  if (table.offset != cursor || table.entrySize != sizeof(T))
    return Status::InvalidLayout;
  cursor += std::uint64_t{sizeof(T)} * table.count;
  return cursor <= UINT32_MAX ? Status::Valid : Status::CapacityExceeded;
}

template <typename T>
void storeTable(std::vector<std::byte>& bytes, RenderTapeIdentityV3Table table,
                const std::vector<T>& rows) noexcept {
  if (!rows.empty())
    std::memcpy(bytes.data() + table.offset, rows.data(), rows.size() * sizeof(T));
}

template <typename T>
void copyTable(std::vector<T>& rows, std::span<const std::byte> bytes,
               RenderTapeIdentityV3Table table) {
  rows.resize(table.count);
  if (!rows.empty())
    std::memcpy(rows.data(), bytes.data() + table.offset, rows.size() * sizeof(T));
}

} // namespace

Status encodeRenderTapeIdentityV3SnapshotTransfer(
    const RenderTapeIdentityV3View& snapshot,
    std::vector<std::byte>& out) noexcept {
  out.clear();
  if (!snapshotHeaderAdmitted(snapshot.header)) return Status::InvalidHeader;
  RenderTapeIdentityV3TransferHeader transfer{};
  transfer.identity = snapshot.header;
  auto& header = transfer.identity;
  std::uint64_t size = sizeof(transfer);
  if (!layoutTable(header.sources, size, snapshot.sources) ||
      !layoutTable(header.initializers, size, snapshot.initializers) ||
      !layoutTable(header.commands, size, snapshot.commands) ||
      !layoutTable(header.dispositions, size, snapshot.dispositions) ||
      !layoutTable(header.coordinators, size, snapshot.coordinators) ||
      !layoutTable(header.settlements, size, snapshot.settlements) ||
      size > std::numeric_limits<std::size_t>::max()) return Status::CapacityExceeded;
  transfer.totalBytes = size;
  try {
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::memcpy(bytes.data(), &transfer, sizeof(transfer));
    storeTable(bytes, header.sources, snapshot.sources);
    storeTable(bytes, header.initializers, snapshot.initializers);
    storeTable(bytes, header.commands, snapshot.commands);
    storeTable(bytes, header.dispositions, snapshot.dispositions);
    storeTable(bytes, header.coordinators, snapshot.coordinators);
    storeTable(bytes, header.settlements, snapshot.settlements);
    out = std::move(bytes);
    return Status::Valid;
  } catch (...) {
    return Status::AllocationFailed;
  }
}

Status decodeRenderTapeIdentityV3SnapshotTransfer(
    std::span<const std::byte> bytes,
    RenderTapeIdentityV3CaptureKey expectedCapture,
    RenderTapeIdentityV3View& out) noexcept {
  out = {};
  RenderTapeIdentityV3TransferHeader transfer{};
  if (bytes.size() < sizeof(transfer)) return Status::InvalidHeader;
  std::memcpy(&transfer, bytes.data(), sizeof(transfer));
  const auto& header = transfer.identity;
  if (transfer.magic != kRenderTapeIdentityV3TransferMagic ||
      transfer.version != kRenderTapeIdentityV3TransferVersion ||
      transfer.headerSize != sizeof(transfer) || transfer.reserved0 ||
      transfer.reserved1 || !snapshotHeaderAdmitted(header)) return Status::InvalidHeader;
  if (!expectedCapture.token || !expectedCapture.epoch ||
      header.captureToken != expectedCapture.token ||
      header.captureEpoch != expectedCapture.epoch) return Status::CaptureMismatch;
  if (transfer.totalBytes != bytes.size()) return Status::InvalidLayout;
  std::uint64_t size = sizeof(transfer);
  for (const auto status : {
      admitTable<RenderTapeIdentityV3Source>(header.sources, size),
      admitTable<RenderTapeIdentityV3Initializer>(header.initializers, size),
      admitTable<RenderTapeIdentityV3Command>(header.commands, size),
      admitTable<RenderTapeIdentityV3Disposition>(header.dispositions, size),
      admitTable<RenderTapeIdentityV3Coordinator>(header.coordinators, size),
      admitTable<RenderTapeIdentityV3ReplaySettlement>(header.settlements, size)}) {
    if (status != Status::Valid) return status;
  }
  if (size != transfer.totalBytes) return Status::InvalidLayout;
  try {
    RenderTapeIdentityV3View candidate;
    candidate.header = header;
    copyTable(candidate.sources, bytes, header.sources);
    copyTable(candidate.initializers, bytes, header.initializers);
    copyTable(candidate.commands, bytes, header.commands);
    copyTable(candidate.dispositions, bytes, header.dispositions);
    copyTable(candidate.coordinators, bytes, header.coordinators);
    copyTable(candidate.settlements, bytes, header.settlements);
    out = std::move(candidate);
    return Status::Valid;
  } catch (...) {
    return Status::AllocationFailed;
  }
}

const char* renderTapeIdentityV3TransferStatusName(Status status) noexcept {
  switch (status) {
  case Status::Valid: return "valid";
  case Status::InvalidHeader: return "invalid-header";
  case Status::InvalidLayout: return "invalid-layout";
  case Status::CaptureMismatch: return "capture-mismatch";
  case Status::CapacityExceeded: return "capacity-exceeded";
  case Status::AllocationFailed: return "allocation-failed";
  }
  return "unknown";
}

} // namespace dxmt9::d3d9
