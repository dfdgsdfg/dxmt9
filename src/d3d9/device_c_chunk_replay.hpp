#pragma once

#include "device_c_chunk_validate.hpp"
#include "dxmt9/core_snapshots.hpp"

#include <cstdint>
#include <limits>
#include <span>

struct D9CDevice;

namespace dxmt9::core::metalqueue {
struct ReplaySourcePublicationReceipt;
}

namespace dxmt9::d3d9 {

struct ReplayTransactionState;

inline constexpr std::int32_t kCommandChunkDecodeFailure =
    std::numeric_limits<std::int32_t>::min();

struct ResolvedRecordView {
  ImportedRecordView wire{};
  std::span<void* const> objects{};

  void* objectForAbsoluteIndex(std::uint32_t index) const noexcept;
};

struct ResolvedChunkView {
  ImportedChunkView wire{};
  std::span<void* const> objects{};

  ResolvedRecordView record(std::size_t index) const noexcept;
};

class NonDrawReplaySink {
 public:
  virtual ~NonDrawReplaySink() = default;

  virtual std::int32_t setConstants(
      std::uint32_t type, const D9CCommandChunkWireSetConst& fixed,
      std::span<const std::byte> registerBytes) = 0;
  virtual std::int32_t clear(
      const D9CCommandChunkWireClear& fixed,
      std::span<const D9CRect> rects) = 0;
  virtual std::int32_t present(
      const D9CCommandChunkWirePresent& fixed) = 0;
  virtual std::int32_t stretchRect(
      const D9CCommandChunkWireStretchRect& fixed, void* src,
      void* dst) = 0;
  virtual std::int32_t colorFill(
      const D9CCommandChunkWireColorFill& fixed, void* surface) = 0;
  virtual std::int32_t updateTexture(
      const D9CCommandChunkWireUpdateTexture& fixed, void* src,
      void* dst) = 0;
  virtual std::int32_t updateSurface(
      const D9CCommandChunkWireUpdateSurface& fixed, void* src,
      void* dst) = 0;
  virtual std::int32_t queryIssue(
      const D9CCommandChunkWireQueryIssue& fixed, void* query) = 0;
  virtual std::int32_t readback(
      const D9CCommandChunkWireReadback& fixed, void* src,
      void* dst) = 0;
  virtual std::int32_t reszDepthResolve(
      const D9CCommandChunkWireReszDepthResolve& fixed, void* msaaDepth,
      void* intzDest) = 0;
  virtual std::int32_t generateMipmaps(
      const D9CCommandChunkWireGenerateMipmaps& fixed, void* texture) = 0;
  virtual std::int32_t applyState(
      const ResolvedRecordView& record) = 0;
};

struct SparseDrawCall {
  dxmt9::core::DrawParam param{};
  dxmt9::core::DrawParamPayloadView payload{};
  std::uint32_t flags = 0u;
  std::uint32_t minVertex = 0u;
  std::uint32_t numVertices = 0u;
  std::uint32_t stride = 0u;
  std::uint32_t indexFormat = 0u;
};

class SparseReplaySink {
 public:
  virtual ~SparseReplaySink() = default;

  virtual std::int32_t setRenderStates(
      std::span<const D9CCommandChunkWireRenderState> values) = 0;
  virtual std::int32_t setTexture(std::uint32_t slot, void* texture) = 0;
  virtual std::int32_t setStream(
      const D9CCommandChunkWireStreamBinding& value, void* buffer) = 0;
  virtual std::int32_t setShader(std::uint32_t stage, void* shader) = 0;
  virtual std::int32_t setVertexInput(
      std::uint32_t kind, std::uint32_t value, void* declaration) = 0;
  virtual std::int32_t setIndexBuffer(void* buffer) = 0;
  virtual std::int32_t setRenderTarget(
      std::uint32_t slot, void* surface) = 0;
  virtual std::int32_t setDepthStencil(void* surface) = 0;
  virtual std::int32_t setViewport(const D9CViewport& value) = 0;
  virtual std::int32_t setScissor(const D9CRect& value) = 0;
  virtual std::int32_t setMaterial(const D9CMaterial& value) = 0;
  virtual std::int32_t setClipPlane(
      const D9CCommandChunkWireClipPlane& value) = 0;
  virtual std::int32_t setTextureStageStates(
      std::span<const D9CDrawPacketTextureStageState> values) = 0;
  virtual std::int32_t setSamplerStates(
      std::span<const D9CDrawPacketSamplerState> values) = 0;
  virtual std::int32_t setTransforms(
      std::span<const D9CDrawPacketTransform> values) = 0;
  virtual std::int32_t setLights(
      std::span<const D9CCommandChunkWireLight> values) = 0;
  virtual std::int32_t setLightEnables(
      std::span<const D9CCommandChunkWireLightEnable> values) = 0;
  virtual std::int32_t setConstants(
      std::uint16_t sectionKind,
      const D9CCommandChunkWireConstantRange& range,
      std::span<const std::byte> registerBytes) = 0;
  virtual std::int32_t finishApplyState(std::uint32_t flags) = 0;
  virtual std::int32_t draw(const SparseDrawCall& call) = 0;
};

bool isSparseRecord(std::uint32_t type) noexcept;

std::int32_t replaySparseRecord(
    const ResolvedRecordView& record,
    SparseReplaySink& sink) noexcept;

bool isNonDrawRecord(std::uint32_t type) noexcept;

std::int32_t replayNonDrawRecord(
    const ResolvedRecordView& record,
    NonDrawReplaySink& sink) noexcept;

// Cold provider-replay routing policy. Historical preserves the pre-policy
// entry behavior for existing callers. Compatibility is an explicit
// compatibility-only candidate; ProductionSlot
// enters the same planner/transaction/queue admission path used by production
// replay, including its explicit compatibility fallback when the candidate is
// not eligible. A qualified producer interval is preserved; an absent
// interval remains an explicitly provider-local unqualified replay identity,
// never inferred capture provenance.
enum class ProviderReplayPolicy : std::uint8_t {
  // Historical provider callers: preserve the pre-policy entry semantics.
  // In particular, a planning-disabled backend may still use its existing
  // direct ChunkSlot route even though Arena admission was disabled.
  Historical,
  Compatibility,
  ProductionSlot,
};

struct ProviderReplayRecordRange {
  std::uint32_t firstRecord = 0u;
  std::uint32_t recordCount = 0u;
};

// Cold harness bound, not a production wire limit. This admits at most 4096
// plans and 8193 publication receipts for one raw, before any replay effects.
inline constexpr std::uint32_t kProviderSourceReplayMaxRecords = 4096u;

enum class ProviderReplaySourceSettlementKind : std::uint8_t {
  Published,
  PresentPublished,
  Empty,
};

// Borrowed only for the synchronous settlement callback. Capture provenance
// stays in the caller's authenticated source plan, not in local queue IDs.
// A policy may publish several local sources inside one captured range.
struct ProviderReplaySourceSettlement {
  ProviderReplayRecordRange range{};
  // Provider-local private raw identity; never a captured source/sequence ID.
  std::uint64_t rawOrdinal = 0u;
  std::uint32_t rangeIndex = 0u;
  ProviderReplaySourceSettlementKind kind =
      ProviderReplaySourceSettlementKind::Empty;
  std::span<const core::metalqueue::ReplaySourcePublicationReceipt> publications{};
};

struct ProviderReplaySourceConsumer {
  void* context = nullptr;
  // Runs outside the queue lock, after range publication, while the full raw
  // owner and its wrapper/buffer snapshots are still retained. CPU harnesses
  // drain their controlled completion here; GPU callers need not wait.
  bool (*settled)(void*, const ProviderReplaySourceSettlement&) noexcept = nullptr;
};

// Cold v3 execution observation. The original full raw, transaction, sink and
// retained wrapper/snapshot owners stay live across every callback. A record
// callback may qualify an authenticated cut, but never replaces dispatch.
struct ProviderReplayRecordConsumer {
  void* context = nullptr;
  bool (*eventStarted)(void*, std::uint64_t, std::uint32_t) noexcept = nullptr;
  bool (*transactionStarted)(void*, const ReplayTransactionState&) noexcept = nullptr;
  bool (*transactionCommitted)(void*, const ReplayTransactionState&) noexcept = nullptr;
  bool (*beforeRecord)(void*, std::uint32_t, std::uint32_t) noexcept = nullptr;
  bool (*beforeDispatch)(void*, std::uint32_t, std::uint32_t) noexcept = nullptr;
  bool (*recordDispatched)(void*, std::uint32_t, std::uint32_t,
                           std::int32_t) noexcept = nullptr;
  bool (*eventSettled)(void*, std::uint64_t, std::uint32_t) noexcept = nullptr;
};

// Independent cold V4 observation. Local capture credentials come from the
// actual arm receipt, never the captured tape's queue/raw IDs. rawStarted is
// required to authenticate eventOrdinal/count against the executor's admitted
// owned plan before observations begin. All callbacks borrow synchronously;
// rejection fails qualification only, never renderer dispatch or its HRESULT.
// Callbacks must not re-enter replay, publish sources, or alter queue routing.
// They execute outside capture admission/root locks; synchronous cancellation
// revokes later observations and qualification without changing replay effects.
struct ProviderReplayObservationV4 {
  std::uint64_t captureToken = 0u;
  std::uint64_t captureEpoch = 0u;
  std::uint64_t queue = 0u;
  std::uint64_t eventOrdinal = 0u;
  void* context = nullptr;
  bool (*rawStarted)(void*, std::uint64_t originalEvent,
      std::uint64_t localRaw, std::uint32_t count) noexcept = nullptr;
  bool (*beforeRecord)(void*, std::uint32_t index, std::uint32_t type) noexcept = nullptr;
  bool (*beforeDispatch)(void*, std::uint32_t index, std::uint32_t type) noexcept = nullptr;
  bool (*recordDispatched)(void*, std::uint32_t index, std::uint32_t type,
      std::int32_t result) noexcept = nullptr;
  bool (*rawSettled)(void*, std::uint64_t localRaw, std::uint32_t count,
      bool succeeded) noexcept = nullptr;
};

// Separate from the actual renderer result and present only on the new cold
// entry. Ledger settlement is not publication, submission or GPU completion.
struct ProviderReplayObservationV4Result {
  bool captureAccepted = false;
  bool qualificationSucceeded = false;
  bool ledgerSettlementObserved = false;
  std::uint64_t localRawOrdinal = 0u;
};

// Version selection, not live observer admission, owns this legacy veto.
// Once V4 is selected, stale/revoked capture tags remain neutral metadata.
constexpr bool legacyRenderTapeCaptureRouteRequested(
    bool captureTagged, bool v4Selected) noexcept {
  return captureTagged && !v4Selected;
}

// Used only by the original record walker. Outside the synchronous cold
// entry below these hooks are inert; no callback or span survives that entry.
bool providerReplayRecordConsumerEnabled() noexcept;
bool observeProviderReplayTransactionStarted(const ReplayTransactionState&) noexcept;
bool observeProviderReplayTransactionCommitted(const ReplayTransactionState&) noexcept;
bool observeProviderReplayRecordBefore(std::uint32_t recordIndex,
                                       std::uint32_t recordType) noexcept;
bool observeProviderReplayRecordAfter(std::uint32_t recordIndex,
                                      std::uint32_t recordType,
                                      std::int32_t result) noexcept;
bool observeProviderReplayRecordDispatch(std::uint32_t recordIndex,
                                         std::uint32_t recordType) noexcept;

bool validateProviderReplayRecordRanges(
    std::uint32_t totalRecords,
    std::span<const ProviderReplayRecordRange> ranges) noexcept;

// Sorted, unique cuts strictly inside a source (never at publication edges).
bool validateProviderReplayBatchCuts(
    std::uint32_t totalRecords,
    std::span<const ProviderReplayRecordRange> ranges,
    std::span<const std::uint32_t> batchCuts) noexcept;

// Provider-replay entry for a canonical chunk whose recorded handle table has
// already been resolved to replay-owned wrappers. The canonical bytes are
// imported unchanged and dispatched by DeviceReplaySink; this does not add a
// second record switch or consult the live capture registry.
std::int32_t replayPrevalidatedResolvedCommandChunk(
    D9CDevice* device, std::span<const std::byte> bytes,
    const CommandChunkEnvelope& envelope,
    std::span<void* const> resolvedObjects,
    ProviderReplayPolicy policy = ProviderReplayPolicy::Historical) noexcept;

// Full original raw and unchanged normal policy/planner route. Unlike the V3
// record consumer, this does not force Compatibility, split source ranges,
// seal leases, or request publication. Stale arms/callback rejection appear
// only in observationResult; valid replay retains its real renderer HRESULT.
std::int32_t replayPrevalidatedResolvedCommandChunkObservedV4(
    D9CDevice* device, std::span<const std::byte> bytes,
    const CommandChunkEnvelope& envelope,
    std::span<void* const> resolvedObjects, ProviderReplayPolicy policy,
    const ProviderReplayObservationV4& observation,
    ProviderReplayObservationV4Result& observationResult) noexcept;

std::int32_t replayPrevalidatedResolvedCommandChunkAtRecordBoundaries(
    D9CDevice* device, std::span<const std::byte> bytes,
    const CommandChunkEnvelope& envelope,
    std::span<void* const> resolvedObjects,
    std::span<const ProviderReplayRecordRange> ranges,
    std::uint32_t& completedRanges,
    std::span<const std::uint32_t> batchCuts = {},
    std::uint32_t* completedBatchCuts = nullptr) noexcept;

// Shared normal CPU/GPU source driver. Uses ordinary Direct ChunkSlot or
// source-limited compatibility, never substitutes Arena or repacks the raw.
// The caller must authenticate ranges against CaptureSourceReplayPlan first.
std::int32_t replayPrevalidatedResolvedCommandChunkBySources(
    D9CDevice* device, std::span<const std::byte> bytes,
    const CommandChunkEnvelope& envelope,
    std::span<void* const> resolvedObjects,
    ProviderReplayPolicy policy,
    std::span<const ProviderReplayRecordRange> ranges,
    ProviderReplaySourceConsumer consumer = {}) noexcept;

// Original-byte, full-raw v3 compatibility execution. Event settlement does
// not publish the ordinary writer: one writer may span canonical events.
// ProductionSlot is deliberately rejected until its source-wide planner can
// preserve the same publication cuts without per-record lease sealing.
std::int32_t replayPrevalidatedResolvedCommandChunkV3(
    D9CDevice* device, std::span<const std::byte> bytes,
    const CommandChunkEnvelope& envelope,
    std::span<void* const> resolvedObjects,
    ProviderReplayPolicy policy,
    ProviderReplayRecordConsumer consumer) noexcept;

}  // namespace dxmt9::d3d9
