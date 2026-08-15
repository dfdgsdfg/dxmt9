#pragma once

#include "device_c_render_tape.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

inline constexpr std::uint64_t kRenderTapeIdentityMagic =
    0x31444954544d5844ull;
inline constexpr std::uint32_t kRenderTapeIdentityVersion = 1u;
inline constexpr char kRenderTapeIdentitySchema[] =
    "dxmt9.render_tape.identity.v1";

enum class RenderTapeIdentityAuthority : std::uint32_t {
  Capture = 1u,
  ProviderReplay = 2u,
};

struct RenderTapeIdentityHeader {
  std::uint64_t magic = kRenderTapeIdentityMagic;
  std::uint64_t eventsBytes = 0u;
  std::uint64_t frameId = 0u;
  std::uint64_t presentOrdinal = 0u;
  std::uint64_t captureToken = 0u;
  RenderTapeDigest eventsDigest{};
  std::uint32_t version = kRenderTapeIdentityVersion;
  std::uint32_t headerSize = sizeof(RenderTapeIdentityHeader);
  std::uint32_t sourceEntrySize = 0u;
  std::uint32_t rangeEntrySize = 0u;
  std::uint32_t sourceTableOffset = 0u;
  std::uint32_t sourceCount = 0u;
  std::uint32_t rangeTableOffset = 0u;
  std::uint32_t rangeCount = 0u;
  std::uint32_t authority = 0u;
  std::uint32_t reserved0 = 0u;
};

struct RenderTapeIdentitySource {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t seqId = 0u;
  std::uint64_t captureToken = 0u;
  std::uint32_t recordCount = 0u;
  std::uint32_t firstRange = 0u;
  std::uint32_t rangeCount = 0u;
  std::uint32_t reserved0 = 0u;
};

struct RenderTapeIdentityRange {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t seqId = 0u;
  std::uint64_t logicalPassId = 0u;
  std::uint32_t firstRecord = 0u;
  std::uint32_t recordCount = 0u;
  std::uint32_t dagPassIndex = 0u;
  std::uint32_t passKind = 0u;
};

static_assert(sizeof(RenderTapeIdentityHeader) == 112u);
static_assert(sizeof(RenderTapeIdentitySource) == 48u);
static_assert(sizeof(RenderTapeIdentityRange) == 48u);

enum class RenderTapeIdentityStatus : std::uint8_t {
  Valid,
  InvalidTape,
  MissingHeader,
  InvalidHeader,
  InvalidLayout,
  EventsMismatch,
  InvalidFrameIdentity,
  SourceCoverageMismatch,
  NonMonotoneSource,
  InvalidRange,
  RecordCoverageMismatch,
  PassIdentityMismatch,
  AllocationFailed,
};

struct RenderTapeIdentityView {
  RenderTapeIdentityHeader header{};
  std::vector<RenderTapeIdentitySource> sources{};
  std::vector<RenderTapeIdentityRange> ranges{};
};

struct RenderTapeIdentityValidationResult {
  RenderTapeIdentityStatus status = RenderTapeIdentityStatus::MissingHeader;
  RenderTapeValidationResult tapeValidation{};
  std::uint32_t failedSource = 0xffffffffu;
  std::uint32_t failedRange = 0xffffffffu;

  bool valid() const noexcept {
    return status == RenderTapeIdentityStatus::Valid;
  }
};

struct RenderTapeProductionPassRange {
  std::uint32_t firstRecord = 0u;
  std::uint32_t recordCount = 0u;
  std::uint32_t dagPassIndex = 0u;
  std::uint32_t passKind = 0u;
};

// Unix-provider capture ledger. The offload worker is the sole appender and
// the PE Present boundary reads it only after drainDeferredReplay(), but the
// mutex keeps teardown and diagnostic misuse fail-closed rather than relying
// on that external scheduling fact.
class RenderTapeProductionIdentityLedger {
public:
  bool append(std::uint64_t captureToken, std::uint64_t eventOrdinal,
              std::uint64_t sourceOrdinal, std::uint64_t seqId,
              std::uint32_t recordCount,
              std::span<const RenderTapeProductionPassRange> ranges) noexcept;
  void fail(std::uint64_t captureToken) noexcept;
  bool copy(std::uint64_t captureToken,
            D9CRenderTapeIdentityCaptureResult& out,
            std::span<std::byte> bytes) const noexcept;

private:
  mutable std::mutex mutex_{};
  std::uint64_t captureToken_ = 0u;
  std::uint64_t nextLogicalPassId_ = 1u;
  bool failed_ = false;
  std::vector<D9CRenderTapeIdentitySourceEntry> sources_{};
  std::vector<D9CRenderTapeIdentityRangeEntry> ranges_{};
};

RenderTapeIdentityValidationResult validateRenderTapeIdentity(
    std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::span<const std::byte> sidecar,
    RenderTapeIdentityView* out = nullptr) noexcept;

std::vector<std::byte> buildRenderTapeIdentity(
    std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue& verifiedCatalogue,
    std::uint64_t frameId, std::uint64_t presentOrdinal,
    std::uint64_t captureToken, RenderTapeIdentityAuthority authority,
    std::span<const RenderTapeIdentitySource> sources,
    std::span<const RenderTapeIdentityRange> ranges,
    RenderTapeIdentityValidationResult* validationResult = nullptr);

bool renderTapeIdentityOwnsSelection(
    const RenderTapeIdentityView& identity, std::uint64_t eventOrdinal,
    std::uint32_t firstRecord, std::uint32_t recordCount,
    std::uint64_t* logicalPassId = nullptr) noexcept;

const char* renderTapeIdentityStatusName(
    RenderTapeIdentityStatus status) noexcept;

} // namespace dxmt9::d3d9
