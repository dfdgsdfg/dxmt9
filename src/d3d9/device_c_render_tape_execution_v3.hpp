#pragma once

#include "device_c_chunk_replay.hpp"
#include "device_c_render_tape_source_plan_v3.hpp"
#include "device_c_render_tape_runtime_identity_v3.hpp"
#include "dxmt9/dxmt9_render_tape_runtime_observer.hpp"

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

namespace render_tape_runtime { struct GeneralPreflightPlan; }

struct RenderTapeSourceReplayV3Publication {
  std::uint32_t capturedIndex = kRenderTapeIdentityV3NoIndex;
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t localSequence = 0u;
  core::CpuReadyTape::SourceRef source{};
  core::CpuReadySourceMetadata metadata{};
  std::size_t controlIndex = std::numeric_limits<std::size_t>::max();
  std::uint32_t commandCount = 0u;
  bool hasPresent = false;
  bool expected = false;
  bool submitted = false;
  bool completed = false;
};

struct RenderTapeSourceReplayV3Initializer {
  std::uint32_t capturedIndex = kRenderTapeIdentityV3NoIndex;
  std::uint64_t localSequence = 0u;
  bool expected = false;
  bool submitted = false;
  bool completed = false;
};

struct RenderTapeSourceReplayV3Settlement {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t localRawOrdinal = 0u;
  std::uint32_t recordCount = 0u;
};

struct RenderTapeSourceReplayV3Evidence {
  bool requested = false;
  bool validated = false;
  bool executed = false;
  bool complete = false;
  bool lifecycleComplete = false;
  bool storageOverflow = false;
  // Compatibility execution is a first gate, not ProductionSlot evidence.
  bool compatibility = true;
  bool qualifierOnly = true;
  bool timingEligible = false;
  RenderTapeDigest planDigest{};
  std::size_t dispatchedRecords = 0u;
  std::size_t replayedEvents = 0u;
  std::size_t externalPublicationCuts = 0u;
  std::vector<RenderTapeSourceReplayV3Publication> publications{};
  std::vector<RenderTapeSourceReplayV3Initializer> initializers{};
  std::vector<RenderTapeSourceReplayV3Settlement> settlements{};
};

// Shared cold CPU/Metal execution owner. Admission binds exact immutable
// bytes before effects; the supplied owned plan and result must outlive this
// synchronous session. The queue observer copies values only, under its lock.
// Receipt consumers run later, outside that lock, and may drain a fake backend.
class RenderTapeExecutionV3 {
public:
  struct SourceConsumer {
    void* context = nullptr;
    bool (*published)(void*,
        const core::metalqueue::ReplaySourcePublicationReceipt&) noexcept = nullptr;
  };

  struct WritingContinuation {
    query::WriterIdentity writer{};
    core::CpuReadyTape::SourceRef source{};
    std::size_t commandCount = 0u;
  };

  RenderTapeExecutionV3() = default;
  ~RenderTapeExecutionV3();
  RenderTapeExecutionV3(const RenderTapeExecutionV3&) = delete;
  RenderTapeExecutionV3& operator=(const RenderTapeExecutionV3&) = delete;

  bool admit(const CaptureSourceReplayPlanV3& plan,
             const render_tape_runtime::GeneralPreflightPlan& replay,
             std::span<const std::byte> tape,
             RenderTapeSourceReplayV3Evidence& evidence) noexcept;
  // Setup seeds must have completed in the local backend before arm(). Arm
  // neither flushes nor copies captured completion labels into the queue.
  bool arm(D9CDevice* device) noexcept;
  bool arm(D9CDevice* device, SourceConsumer consumer) noexcept;
  bool beginEvent(std::uint32_t eventIndex) noexcept;
  bool replayedEvent(std::uint32_t eventIndex) noexcept;
  bool bindCommandChunk(std::span<const std::byte> bytes,
                        const CommandChunkEnvelope& envelope,
                        std::span<void* const> objects) noexcept;
  // A CPU queue snapshot is not authority by itself. It may account for one
  // retained Writing resident only when it matches this armed session's real
  // authenticated Append observations at the next original event boundary.
  bool ownsWritingContinuation(const WritingContinuation&) const noexcept;
  ProviderReplayRecordConsumer recordConsumer() noexcept;
  // Revokes callbacks before their evidence/consumer storage or device owners
  // can be destroyed. Idempotent; incomplete execution fail-stops the queue.
  void cancel() noexcept;
  bool finish() noexcept;
  bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
  const char* failure() const noexcept { return failure_.load(std::memory_order_acquire); }
  std::uint32_t snapshotPrefixEvents() const noexcept { return snapshotPrefixEvents_; }

private:
  static void observe(void*, const render::TapeRuntimeObservation&) noexcept;
  void record(const render::TapeRuntimeObservation&) noexcept;
  bool beforeRecord(std::uint32_t index, std::uint32_t type) noexcept;
  bool beforeDispatch(std::uint32_t index, std::uint32_t type) noexcept;
  bool startEvent(std::uint64_t raw, std::uint32_t count) noexcept;
  bool startTransaction(const ReplayTransactionState&) noexcept;
  bool commitTransaction(const ReplayTransactionState&) noexcept;
  bool afterRecord(std::uint32_t index, std::uint32_t type,
                   std::int32_t result) noexcept;
  bool settleEvent(std::uint64_t raw, std::uint32_t count) noexcept;
  // Applies a preadmitted external cut separately from Expect actions. A
  // coordinator/Present cut must already have its real publication receipt.
  bool cut(std::uint32_t kind, std::uint32_t record,
           bool externalAllowed) noexcept;
  bool reject(const char* reason) noexcept;
  bool acknowledge(CaptureSourceReplayPlanV3ActionKind kind,
                   std::uint32_t record = kRenderTapeIdentityV3NoIndex) noexcept;
  bool lifecycleComplete() const noexcept;

  const CaptureSourceReplayPlanV3* plan_ = nullptr;
  RenderTapeSourceReplayV3Evidence* evidence_ = nullptr;
  CaptureSourceReplayCursorV3 cursor_{};
  D9CDevice* device_ = nullptr;
  std::shared_ptr<dxmt9::Device> backend_{};
  render::TapeRuntimeBaseline baseline_{};
  SourceConsumer consumer_{};
  RenderTapeRuntimeIdentityV3 localIdentity_{};
  RenderTapeIdentityV3CaptureKey localKey_{};
  mutable std::mutex observationsMutex_{};
  ImportedChunkView chunk_{};
  std::span<void* const> resolved_{};
  std::uint32_t event_ = kRenderTapeIdentityV3NoIndex;
  std::uint32_t nextRecord_ = 0u;
  std::uint64_t localRawOrdinal_ = 0u;
  std::uint32_t snapshotPrefixEvents_ = 0u;
  std::uint32_t expectedSources_ = 0u;
  std::uint32_t expectedInitializers_ = 0u;
  std::uint64_t observedPublicationEdge_ = 0u;
  std::vector<std::uint64_t> sourceEdges_{};
  std::vector<std::uint64_t> initializerEdges_{};
  std::vector<std::vector<std::uint32_t>> recordTypes_{};
  std::vector<RenderTapeDigest> commandDigests_{};
  std::vector<CommandChunkEnvelope> commandEnvelopes_{};
  WritingContinuation writingContinuation_{};
  bool armed_ = false;
  std::atomic<bool> failed_{false};
  std::atomic<const char*> failure_{"none"};
};

} // namespace dxmt9::d3d9
