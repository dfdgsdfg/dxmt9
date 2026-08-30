#pragma once

#include "d3d9_pe_diagnostic_observer.hpp"
#include "d3d9_pe_recorder.hpp"
#include "d3d9_pe_semantic_tokens.hpp"
#include "d3d9_pe_stats_decimation.hpp"
#include "dxmt9/copy_materialization_ledger.hpp"
#include "dxmt9/device_c.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace dxmt9::d3d9::pe {
class PeThreadSampler;
}

struct PeDiagnosticsConfig {
  bool recorderStats = false;
  bool recorderChunkLog = false;
  std::uint32_t statsDecimationN = 0;
  bool vsConstSetterRange = false;
  bool moduleMap = false;
  bool threadSampler = false;
  bool debugLog = false;
  bool scalarSemanticObserver = false;
  bool copyMaterializationLedger = false;
  std::uint32_t threadSamplerHz = 250;

  constexpr bool enabled() const noexcept {
    return recorderStats || recorderChunkLog || statsDecimationN != 0 ||
           vsConstSetterRange || moduleMap || threadSampler || debugLog ||
           scalarSemanticObserver || copyMaterializationLedger;
  }

  constexpr bool chunkCommitTimingEnabled() const noexcept {
    return recorderStats || recorderChunkLog;
  }
};

// Cached ownership gates keep unrelated observer knobs off the entry-point
// hot path.  In particular, module-map, thread-sampler, and debug-log output
// may need a diagnostics owner for their cold lifecycle, but they do not
// justify constructing a call scope/timer or reading a clock on Set/Draw.
struct PeDiagnosticsFeatureGates {
  bool callScope = false;
  bool hotSetterTimer = false;
  bool chunkCommitTiming = false;
  bool vsConstSetterRange = false;
  bool moduleMap = false;
  bool threadSampler = false;
  bool debugLog = false;
  bool scalarSemanticObserver = false;
  bool copyMaterializationLedger = false;

  constexpr bool any() const noexcept {
    return callScope || hotSetterTimer || chunkCommitTiming ||
           vsConstSetterRange || moduleMap || threadSampler || debugLog ||
           scalarSemanticObserver || copyMaterializationLedger;
  }

  static constexpr PeDiagnosticsFeatureGates fromConfig(
      const PeDiagnosticsConfig &config) noexcept {
    const bool scope = config.recorderStats || config.statsDecimationN != 0;
    return PeDiagnosticsFeatureGates{
        .callScope = scope,
        .hotSetterTimer = scope,
        .chunkCommitTiming = config.chunkCommitTimingEnabled(),
        .vsConstSetterRange = config.vsConstSetterRange,
        .moduleMap = config.moduleMap,
        .threadSampler = config.threadSampler,
        .debugLog = config.debugLog,
        .scalarSemanticObserver = config.scalarSemanticObserver,
        .copyMaterializationLedger = config.copyMaterializationLedger,
    };
  }
};

enum class VsConstSetterRangePhase : std::uint32_t {
  Call = 1,
  Flush = 2,
};

struct VsConstRangeChange {
  std::uint32_t changedRegs = 0;
  std::uint32_t changedSpanRegs = 0;
};

struct VsConstSetterRangeBucket {
  bool used = false;
  VsConstSetterRangePhase phase = VsConstSetterRangePhase::Call;
  std::uint64_t vsHash = 0;
  std::uint64_t psHash = 0;
  std::uint32_t start = 0;
  std::uint32_t count = 0;
  std::uint64_t events = 0;
  std::uint64_t rangeRegs = 0;
  std::uint64_t changedRegs = 0;
  std::uint64_t changedSpanRegs = 0;
  std::uint64_t fullRangeEvents = 0;
  std::uint64_t fullChangedEvents = 0;
};

struct VsConstSetterRangeOverflow {
  std::uint64_t events = 0;
  std::uint64_t rangeRegs = 0;
  std::uint64_t changedRegs = 0;
  std::uint64_t changedSpanRegs = 0;
  std::uint64_t fullRangeEvents = 0;
  std::uint64_t fullChangedEvents = 0;
};

struct VsConstSetterRangePerf {
  static constexpr std::size_t kBucketCount = 256;
  std::array<VsConstSetterRangeBucket, kBucketCount> buckets{};
  std::array<VsConstSetterRangeOverflow, 3> overflow{};
};

struct PeInterAppendCallSiteLocalKey {
  std::uint32_t prevCallName = 0;
  std::uint32_t nextCallName = 0;
  const void *callerPc = nullptr;

  bool operator==(const PeInterAppendCallSiteLocalKey &other) const noexcept {
    return prevCallName == other.prevCallName &&
           nextCallName == other.nextCallName && callerPc == other.callerPc;
  }
};

struct PeInterAppendCallSiteKey {
  std::uint32_t focusPair = 0;
  std::uint32_t prevCallName = 0;
  std::uint32_t nextCallName = 0;
  const void *callerPc = nullptr;

  bool operator==(const PeInterAppendCallSiteKey &other) const noexcept {
    return focusPair == other.focusPair &&
           prevCallName == other.prevCallName &&
           nextCallName == other.nextCallName && callerPc == other.callerPc;
  }
};

struct PeInterAppendCallSiteLocalKeyHash {
  std::size_t operator()(const PeInterAppendCallSiteLocalKey &key) const noexcept {
    std::size_t h = static_cast<std::size_t>(key.prevCallName);
    h = h * 1315423911u + static_cast<std::size_t>(key.nextCallName);
    h ^= reinterpret_cast<std::uintptr_t>(key.callerPc) +
         0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  }
};

struct PeInterAppendCallSiteKeyHash {
  std::size_t operator()(const PeInterAppendCallSiteKey &key) const noexcept {
    std::size_t h = static_cast<std::size_t>(key.focusPair);
    h = h * 1315423911u + static_cast<std::size_t>(key.prevCallName);
    h = h * 1315423911u + static_cast<std::size_t>(key.nextCallName);
    h ^= reinterpret_cast<std::uintptr_t>(key.callerPc) +
         0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  }
};

struct PeInterAppendCallSiteStats {
  std::uint64_t samples = 0;
  std::uint64_t totalNs = 0;
  std::uint64_t maxNs = 0;
};

struct PeDiagnosticsState {
  explicit PeDiagnosticsState(D3D9DeviceImpl *device,
                              const PeDiagnosticsConfig &resolved)
      : config(resolved), gates(PeDiagnosticsFeatureGates::fromConfig(resolved)),
        childObserver(device),
        scalarSemanticTokens(resolved.scalarSemanticObserver
            ? std::make_unique<dxmt9::d3d9::pe::PeScalarSemanticTokenLedger>()
            : nullptr),
        allFamilySemanticTokens(resolved.scalarSemanticObserver
            ? std::make_unique<
                  dxmt9::d3d9::pe::PeAllFamilySemanticTokenLedger>()
            : nullptr) {}

  static constexpr std::size_t kPeAppendTypeBuckets = 8;
  static std::size_t peAppendTypeBucket(std::uint32_t type) noexcept {
    switch (type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
      return 0;
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
      return 1;
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
      return 2;
    case D9C_COMMAND_RECORD_APPLY_STATE:
      return 3;
    case D9C_COMMAND_RECORD_SET_VS_CONST_F:
    case D9C_COMMAND_RECORD_SET_VS_CONST_I:
    case D9C_COMMAND_RECORD_SET_VS_CONST_B:
      return 4;
    case D9C_COMMAND_RECORD_SET_PS_CONST_F:
    case D9C_COMMAND_RECORD_SET_PS_CONST_I:
    case D9C_COMMAND_RECORD_SET_PS_CONST_B:
      return 5;
    case D9C_COMMAND_RECORD_CLEAR:
      return 6;
    default:
      return 7;
    }
  }

  PeDiagnosticsConfig config{};
  PeDiagnosticsFeatureGates gates{};
  D3D9PeDiagnosticObserver childObserver;
  // Cold exact semantic witness. The default-off owner is absent, so the
  // production recorder retains its pinned footprint and setters allocate
  // nothing. When enabled, this bounded 8,864-byte diagnostic ledger records
  // the source ordinal that PendingDelta intentionally does not retain.
  std::unique_ptr<dxmt9::d3d9::pe::PeScalarSemanticTokenLedger>
      scalarSemanticTokens{};
  // Same default-off proof gate, now extended across every PE record family.
  // The large bounded ledger remains heap-owned and therefore does not change
  // PeRecorderState, D3D9DeviceImpl, or child wrapper layout on x86/x64.
  std::unique_ptr<dxmt9::d3d9::pe::PeAllFamilySemanticTokenLedger>
      allFamilySemanticTokens{};
  VsConstSetterRangePerf vsConstSetterRangePerf_{};
  PeRecorderStats peRecorderStats_{};
  PeDecimatedScopeStats peChunkAppendDecimatedStats_{};
  std::uint64_t peAppendTypeCounts_[kPeAppendTypeBuckets]{};
  std::uint64_t peAppendTypeBytes_[kPeAppendTypeBuckets]{};
  PeDecimatedScopeStats peAppendPhaseEncode_{};
  PeDecimatedScopeStats peAppendPhaseFlush_{};
  PeDecimatedScopeStats peConstFlushDecimatedStats_{};
  PeDecimatedScopeStats peEntryConstDecimatedStats_{};
  PeDecimatedScopeStats peEntryDrawDecimatedStats_{};
  PeDecimatedScopeStats peEntryStateDecimatedStats_{};
  PeDecimatedScopeStats peDrawPhaseSwvpDecimatedStats_{};
  PeDecimatedScopeStats peDrawPhaseRecordDecimatedStats_{};
  PeDecimatedScopeStats peDrawPacketDecimatedStats_ = [] {
    PeDecimatedScopeStats stats{};
    stats.phaseOffset = 2;
    return stats;
  }();
  std::uint64_t peStatsDecimationPresents_ = 0;
  std::uint64_t peCopyMaterializationReportPresents_ = 0;
  dxmt9::d3d9::pe::PeThreadSampler *peThreadSampler_ = nullptr;
  std::uint64_t peThreadSamplerPresents_ = 0;
  bool peThreadSamplerPresentThreadChecked_ = false;
  std::uint64_t peRecorderStatsLastLoggedCommitCount_ = 0;
  std::int64_t peRecorderLastChunkReturnNs_ = 0;
  std::int64_t peRecorderCurrentChunkFirstAppendNs_ = 0;
  std::int64_t peRecorderLastAppendReturnNs_ = 0;
  std::int64_t peRecorderLastAppendCallEntryNs_ = 0;
  std::int64_t peRecorderLastAppendCallExitNs_ = 0;
  std::uint32_t peRecorderLastAppendRecordType_ = 0;
  bool peRecorderBetweenCallsActive_ = false;
  std::int64_t peRecorderBetweenCallsStartNs_ = 0;
  std::array<std::uint64_t, kPeInterAppendCallFamilyCount>
      peRecorderBetweenCallFamilySamples_{};
  std::array<std::uint64_t, kPeInterAppendCallNameCount>
      peRecorderBetweenCallNameSamples_{};
  std::array<std::uint64_t, kPeInterAppendCallNameCount>
      peRecorderBetweenCallNameCpuNsTotal_{};
  std::array<std::uint64_t, kPeInterAppendCallNameCount>
      peRecorderBetweenCallNameCpuNsMax_{};
  PeInterAppendCallFamily peRecorderBetweenLastCallFamily_ =
      PeInterAppendCallFamily::Unknown;
  PeInterAppendCallName peRecorderBetweenLastCallName_ =
      PeInterAppendCallName::Unknown;
  std::int64_t peRecorderBetweenLastCallExitNs_ = 0;
  std::array<std::uint64_t, kPeInterAppendCallFamilyCount *
                                kPeInterAppendCallFamilyCount>
      peRecorderBetweenCallTransitionSamples_{};
  std::array<std::uint64_t, kPeInterAppendCallFamilyCount *
                                kPeInterAppendCallFamilyCount>
      peRecorderBetweenCallTransitionNsTotal_{};
  std::array<std::uint64_t, kPeInterAppendCallFamilyCount *
                                kPeInterAppendCallFamilyCount>
      peRecorderBetweenCallTransitionNsMax_{};
  std::array<std::uint64_t,
             kPeInterAppendCallNameCount * kPeInterAppendCallNameCount>
      peRecorderBetweenCallNameTransitionSamples_{};
  std::array<std::uint64_t,
             kPeInterAppendCallNameCount * kPeInterAppendCallNameCount>
      peRecorderBetweenCallNameTransitionNsTotal_{};
  std::array<std::uint64_t,
             kPeInterAppendCallNameCount * kPeInterAppendCallNameCount>
      peRecorderBetweenCallNameTransitionNsMax_{};
  std::unordered_map<PeInterAppendCallSiteLocalKey, PeInterAppendCallSiteStats,
                     PeInterAppendCallSiteLocalKeyHash>
      peRecorderBetweenCallNameTransitionSites_{};
  std::unordered_map<PeInterAppendCallSiteKey, PeInterAppendCallSiteStats,
                     PeInterAppendCallSiteKeyHash>
      peRecorderFocusBetweenCallNameTransitionSites_{};
  std::uint64_t peRecorderBetweenCallBodyCalls_ = 0;
  std::uint64_t peRecorderBetweenCallBodyCpuNsTotal_ = 0;
  std::uint64_t peRecorderBetweenCallBodyCpuNsMax_ = 0;
  std::atomic<std::uint64_t> pePresentCadenceOrdinal_{0};
  std::atomic<std::uint64_t> pePresentCadencePendingOrdinal_{0};
  std::atomic<std::uint64_t> pePresentCallMilestonePendingOrdinal_{0};
  std::atomic<std::uint32_t> pePresentCallCount_{0};
  std::atomic<std::uint32_t> pePresentCallMilestoneMask_{0};
  std::atomic<std::uint64_t> pePresentChunkPendingOrdinal_{0};
  std::atomic<std::uint64_t> pePresentRecordPendingOrdinal_{0};
  std::atomic<std::uint32_t> pePresentRecordMilestoneMask_{0};
  std::atomic<std::int64_t> pePresentCadenceReturnNs_{0};
};

static_assert(
    sizeof(decltype(PeDiagnosticsState::scalarSemanticTokens)) ==
        sizeof(void*),
    "cold scalar observer owner must remain one nullable pointer");
static_assert(
    sizeof(decltype(PeDiagnosticsState::allFamilySemanticTokens)) ==
        sizeof(void*),
    "cold all-family observer owner must remain one nullable pointer");

inline std::unique_ptr<PeDiagnosticsState>
makePeDiagnosticsState(D3D9DeviceImpl *device,
                       const PeDiagnosticsConfig &config) {
  if (!config.enabled()) {
    return nullptr;
  }
  return std::make_unique<PeDiagnosticsState>(device, config);
}

template <typename Fn>
inline void peDiagnosticsCall(PeDiagnosticsState *diagnostics,
                              Fn &&call) noexcept(
    noexcept(std::forward<Fn>(call)(*diagnostics))) {
  if (diagnostics) {
    std::forward<Fn>(call)(*diagnostics);
  }
}

template <typename Fn>
inline auto peDiagnosticsRead(PeDiagnosticsState *diagnostics,
                              Fn &&read) noexcept(
    noexcept(std::forward<Fn>(read)(*diagnostics)))
    -> std::invoke_result_t<Fn, PeDiagnosticsState &> {
  using Result = std::invoke_result_t<Fn, PeDiagnosticsState &>;
  if (!diagnostics) {
    return Result{};
  }
  return std::forward<Fn>(read)(*diagnostics);
}

// Value-only timer passed to append emitters. Keeping its implementation with
// the cold diagnostics state prevents the device declaration shell from
// reacquiring observer implementation detail.
struct PeAppendPhaseTimer {
  PeDiagnosticsState *diagnostics = nullptr;

  std::chrono::steady_clock::time_point begin() const noexcept {
    return peDiagnosticsRead(diagnostics, [](PeDiagnosticsState &) noexcept {
      return std::chrono::steady_clock::now();
    });
  }

  void recordEncode(
      std::chrono::steady_clock::time_point t0) const noexcept {
    if (!diagnostics) return;
    PeDecimatedScopeTimer::recordSample(
        diagnostics->peAppendPhaseEncode_,
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count()));
  }

  void recordFlush(
      std::chrono::steady_clock::time_point t0) const noexcept {
    if (!diagnostics) return;
    PeDecimatedScopeTimer::recordSample(
        diagnostics->peAppendPhaseFlush_,
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count()));
  }
};
