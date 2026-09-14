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

}  // namespace dxmt9::d3d9
