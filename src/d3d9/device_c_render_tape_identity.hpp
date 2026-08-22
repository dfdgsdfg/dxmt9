#pragma once

#include "device_c_render_tape.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <mutex>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

inline constexpr std::uint64_t kRenderTapeIdentityMagic =
    0x31444954544d5844ull;
inline constexpr std::uint32_t kRenderTapeIdentityVersion = 2u;
inline constexpr char kRenderTapeIdentitySchema[] =
    "dxmt9.render_tape.identity.v2";

enum class RenderTapeIdentityAuthority : std::uint32_t {
  Capture = 1u,
  ProviderReplay = 2u,
  // Offline projection provenance. This is deliberately not accepted as
  // capture authority; it authenticates only the newly materialized bytes.
  DerivedProjection = 3u,
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
  std::uint32_t settlementEntrySize = 0u;
  std::uint32_t settlementTableOffset = 0u;
  std::uint32_t settlementCount = 0u;
  std::uint32_t reserved1 = 0u;
};

struct RenderTapeIdentitySource {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t seqId = 0u;
  std::uint64_t captureToken = 0u;
  // Event-local record interval owned by this authoritative source.
  std::uint32_t firstRecord = 0u;
  std::uint32_t recordCount = 0u;
  std::uint32_t firstRange = 0u;
  std::uint32_t rangeCount = 0u;
};

struct RenderTapeIdentityRange {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t seqId = 0u;
  std::uint64_t logicalPassId = 0u;
  // Ranges use the parent command event's coordinate space, not a
  // source-local rebasing. This authenticates coverage across fragments.
  std::uint32_t firstRecord = 0u;
  std::uint32_t recordCount = 0u;
  std::uint32_t dagPassIndex = 0u;
  std::uint32_t passKind = 0u;
};

// The sidecar settlement table is the bounded, value-owned evidence for
// event completion. It is deliberately separate from source/range identity:
// a derived projection may carry provenance locators without claiming that
// those locators were queue-completed by the original capture.
struct RenderTapeIdentitySettlement {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t rawOrdinal = 0u;
  std::uint64_t buildGeneration = 0u;
  std::uint64_t firstSourceOrdinal = 0u;
  std::uint64_t tailSeqId = 0u;
  std::uint32_t sourceCount = 0u;
  std::uint32_t reserved0 = 0u;
};

static_assert(sizeof(RenderTapeIdentityHeader) == 128u);
static_assert(sizeof(RenderTapeIdentitySource) == 48u);
static_assert(sizeof(RenderTapeIdentityRange) == 48u);
static_assert(sizeof(RenderTapeIdentitySettlement) == 48u);

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
  std::vector<RenderTapeIdentitySettlement> settlements{};
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
  // Zero allocates a fresh pass ID. Non-zero is an authenticated pass ID
  // supplied by a trusted producer when a pass continues across source
  // boundaries.
  std::uint64_t logicalPassId = 0u;
};

struct RenderTapeIdentityEventSettlement {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t seqId = 0u;
  std::uint32_t count = 0u;
};

static_assert(sizeof(D9CRenderTapeIdentitySettlementEntry) == 48u);
inline constexpr std::size_t kMaxRenderTapeIdentitySettlements = 4096u;

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
  bool append(std::uint64_t captureToken, std::uint64_t eventOrdinal,
              std::uint64_t sourceOrdinal, std::uint64_t seqId,
              std::uint32_t firstRecord, std::uint32_t recordCount,
              std::span<const RenderTapeProductionPassRange> ranges) noexcept;
  // Registers one expected event tail at publication. Completion is recorded
  // separately only after QueueLifecycle authenticates the exact tail.
  bool registerExpectedSettlement(std::uint64_t captureToken,
              std::uint64_t eventOrdinal,
              std::uint64_t rawOrdinal, std::uint64_t buildGeneration,
              std::uint64_t firstSourceOrdinal, std::uint64_t tailSeqId,
              std::uint32_t sourceCount) noexcept;
  bool completeSettlement(std::uint64_t captureToken,
                          std::uint64_t eventOrdinal) noexcept;
  bool expectedTail(std::uint64_t captureToken,
                    D9CRenderTapeIdentitySettlementEntry& out) const noexcept;
  bool markAllSettled(std::uint64_t captureToken) noexcept;
  void fail(std::uint64_t captureToken) noexcept;
  bool copy(std::uint64_t captureToken,
            D9CRenderTapeIdentityCaptureResult& out,
            std::span<std::byte> bytes) const noexcept;

private:
  bool appendImpl(std::uint64_t captureToken, std::uint64_t eventOrdinal,
                  std::uint64_t sourceOrdinal, std::uint64_t seqId,
                  std::uint32_t firstRecord, std::uint32_t recordCount,
                  std::span<const RenderTapeProductionPassRange> ranges,
                  bool rangesAreSourceLocal) noexcept;

  mutable std::mutex mutex_{};
  std::uint64_t captureToken_ = 0u;
  std::uint64_t nextLogicalPassId_ = 1u;
  bool failed_ = false;
  bool settled_ = false;
  std::uint64_t settledEventOrdinal_ = 0u;
  std::uint64_t settledSourceOrdinal_ = 0u;
  std::uint64_t settledSeqId_ = 0u;
  std::array<D9CRenderTapeIdentitySettlementEntry,
             kMaxRenderTapeIdentitySettlements>
      settlements_{};
  std::size_t settlementCount_ = 0u;
  std::array<bool, kMaxRenderTapeIdentitySettlements> settlementCompleted_{};
  std::size_t completedSettlementCount_ = 0u;
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
    RenderTapeIdentityValidationResult* validationResult = nullptr,
    std::span<const RenderTapeIdentitySettlement> settlements = {});

bool renderTapeIdentityOwnsSelection(
    const RenderTapeIdentityView& identity, std::uint64_t eventOrdinal,
    std::uint32_t firstRecord, std::uint32_t recordCount,
    std::uint64_t* logicalPassId = nullptr) noexcept;

// Resolves one authenticated event-local record to its source-qualified
// segment and frozen pass membership. The caller must have validated the
// complete sidecar first; this helper remains fail-closed for stale/missing
// rows and never infers identity from record order.
bool renderTapeIdentityLocateRecord(
    const RenderTapeIdentityView& identity, std::uint64_t eventOrdinal,
    std::uint32_t recordIndex, RenderTapeIdentitySource* source = nullptr,
    RenderTapeIdentityRange* range = nullptr) noexcept;

const char* renderTapeIdentityStatusName(
    RenderTapeIdentityStatus status) noexcept;

} // namespace dxmt9::d3d9
