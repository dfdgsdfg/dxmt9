#pragma once

#include "dxmt9_render_tape_runtime_observer.hpp"
#include "dxmt9_render_tape_topology_observer.hpp"

#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::core::metalqueue {
struct QueueCompletionSource;
struct ResolvedPublishedSource;
} // namespace dxmt9::core::metalqueue
namespace dxmt9::encoders { struct EncodeChunkOptions; }

namespace dxmt9::render {

// Physical offsets never become source-local zero merely because Encode
// consumes a partial span. These copied values carry no payload ownership.
struct TapeTopologySourceRange {
  core::CpuReadyTape::SourceRef source{};
  std::uint64_t sequence = 0u;
  std::uint32_t commandBegin = 0u;
  std::uint32_t commandCount = 0u;
  friend constexpr bool operator==(TapeTopologySourceRange,
                                   TapeTopologySourceRange) noexcept = default;
};

struct TapeTopologyInvocationHandle {
  TapeTopologyCaptureIdentity capture{};
  std::uint64_t attempt = 0u;
  explicit operator bool() const noexcept { return capture.valid() && attempt; }
  friend constexpr bool operator==(TapeTopologyInvocationHandle,
                                   TapeTopologyInvocationHandle) noexcept = default;
};

// Cold, retained invocation storage. Queue code keeps this outside execution
// payloads/return types and carries explicit handles across DCE prefix/suffix
// work. All root access is an admitted short visit; retaining this object is
// not admission and never delays retirement for the duration of Encode.
class TapeTopologyInvocationContext {
public:
  static std::shared_ptr<TapeTopologyInvocationContext> create(
      TapeRuntimeCaptureLease lease, TapeTopologyCaptureIdentity capture,
      std::span<const TapeTopologySourceRange> ranges) noexcept;
  ~TapeTopologyInvocationContext();
  TapeTopologyInvocationContext(const TapeTopologyInvocationContext&) = delete;
  TapeTopologyInvocationContext& operator=(const TapeTopologyInvocationContext&) = delete;

  bool active() const noexcept;
  std::uint32_t commandCount() const noexcept;
  bool commandIndex(core::CpuReadyTape::SourceRef source, std::uint64_t sequence,
      std::uint32_t physicalCommandIndex, std::uint32_t& out) const noexcept;

  // order indexes the bound physical refs, in actual execution order. A
  // no-DAG call may select a subset, e.g. a DCE prefix. The ledger receives
  // that subset sorted physically plus its exact execution permutation.
  bool stageWithoutDag(d3d9::RenderTapeExecutionTopologyV1Route route,
      d3d9::RenderTapeExecutionTopologyV1FallbackReason reason,
      std::span<const std::uint32_t> order, TapeTopologyInvocationHandle& out) noexcept;
  // For source-local/combined DAGs bind full physical source ranges, in FIFO
  // order, so graph command_index retains its original flattened meaning.
  // A partial graph must instead supply its exact physical mapping at create.
  bool stageSelectedDag(d3d9::RenderTapeExecutionTopologyV1FallbackReason reason,
      const framegraph::FrameGraph& graph, std::span<const std::uint32_t> actualOrder,
      std::span<const std::uint32_t> alreadyEncoded,
      TapeTopologyInvocationHandle& out) noexcept;

  // Call only after the real queue/session registration succeeds. Locator
  // equality validates the receipt; it is not authority to register a source.
  // Receipts may cover more than this invocation's partial range, but must
  // identify a bound source exactly and cover each selected physical command.
  bool registerSources(std::span<const TapeTopologySourceRange> receipts) noexcept;
  bool acceptRegisteredBatch(std::span<const TapeTopologyInvocationHandle> handles) noexcept;
  bool observeEncoded(TapeTopologyInvocationHandle handle,
      std::span<const std::uint32_t> actualOrder) noexcept;
  // Cancellation is legal only before registration or successful Encode.
  // Losing an invocation after either effect fails capture, never rendering.
  bool reject(TapeTopologyInvocationHandle handle) noexcept;

  // A combined selected invocation is staged once. Fragment backend scopes
  // suppress restaging; successful Encode order and successful result folds
  // are independent facts. Only finishAggregate records the whole invocation.
  bool beginAggregate(TapeTopologyInvocationHandle handle,
      std::uint32_t expectedFragments) noexcept;
  bool observeFragmentEncoded(TapeTopologyInvocationHandle handle,
      std::span<const std::uint32_t> actualOrder) noexcept;
  bool foldFragment(TapeTopologyInvocationHandle handle) noexcept;
  bool finishAggregate(TapeTopologyInvocationHandle handle) noexcept;
  void fail() noexcept;

private:
  friend class ScopedTapeTopologyInvocation;
  friend class TapeTopologyInvocationStore;
  friend void rejectTapeTopologyCandidate(TapeRuntimeCaptureLease,
      std::span<const TapeTopologySourceRange>) noexcept;
  friend bool activateTapeTopologyCandidate(TapeRuntimeCaptureLease,
      std::span<const TapeTopologySourceRange>, std::uint32_t) noexcept;
  friend std::shared_ptr<TapeTopologyInvocationContext> tapeTopologyCandidateContext(
      TapeRuntimeCaptureLease, std::span<const TapeTopologySourceRange>,
      TapeTopologyInvocationHandle&) noexcept;
  friend void observeTapeTopologyInlineTerminal(TapeRuntimeCaptureLease,
      TapeTopologySourceRange) noexcept;
  struct Invocation {
    TapeTopologyInvocationHandle handle{};
    std::vector<TapeTopologyCommandRef> commands{};
    std::vector<std::uint32_t> executionOrder{};
    std::vector<std::uint32_t> fragmentOrder{};
    std::uint32_t expectedFragments = 0u;
    std::uint32_t encodedFragments = 0u;
    std::uint32_t foldedFragments = 0u;
    bool accepted = false;
    bool encoded = false;
  };
  TapeTopologyInvocationContext(TapeRuntimeCaptureLease lease,
      TapeTopologyCaptureIdentity capture) noexcept;
  Invocation* find(TapeTopologyInvocationHandle handle) noexcept;
  bool hasRegistration(const TapeTopologyCommandRef& command) const noexcept;
  bool registrationEffect(const Invocation& invocation) const noexcept;
  bool invalidate() noexcept;
  bool recordEncoded(Invocation& invocation,
      std::span<const std::uint32_t> actualOrder) noexcept;
  bool fragmentActive(TapeTopologyInvocationHandle handle) const noexcept;

  TapeRuntimeCaptureLease lease_{};
  TapeTopologyCaptureIdentity capture_{};
  std::vector<TapeTopologySourceRange> ranges_{};
  std::vector<TapeTopologyCommandRef> commands_{};
  std::vector<TapeTopologySourceRange> registered_{};
  std::vector<Invocation> invocations_{};
  mutable std::mutex mutex_{};
  bool failed_ = false;
};

class ScopedTapeTopologyInvocation;
inline thread_local ScopedTapeTopologyInvocation* activeTapeTopologyInvocation = nullptr;

// Install only under the existing capture-enabled branch. The TLS pointer
// reaches retained cold storage, not an unguarded topology/root pointer.
class ScopedTapeTopologyInvocation {
public:
  explicit ScopedTapeTopologyInvocation(
      std::shared_ptr<TapeTopologyInvocationContext> context,
      TapeTopologyInvocationHandle handle = {}, bool aggregateFragment = false) noexcept;
  ~ScopedTapeTopologyInvocation();
  ScopedTapeTopologyInvocation(const ScopedTapeTopologyInvocation&) = delete;
  ScopedTapeTopologyInvocation& operator=(const ScopedTapeTopologyInvocation&) = delete;

  bool stageWithoutDag(d3d9::RenderTapeExecutionTopologyV1Route route,
      d3d9::RenderTapeExecutionTopologyV1FallbackReason reason,
      std::span<const std::uint32_t> order) noexcept;
  bool stageSelectedDag(d3d9::RenderTapeExecutionTopologyV1FallbackReason reason,
      const framegraph::FrameGraph& graph, std::span<const std::uint32_t> actualOrder,
      std::span<const std::uint32_t> alreadyEncoded = {}) noexcept;
  bool observeSuccessfulEncode(std::span<const std::uint32_t> actualOrder) noexcept;
  bool commandIndex(core::CpuReadyTape::SourceRef source, std::uint64_t sequence,
      std::uint32_t physicalCommandIndex, std::uint32_t& out) const noexcept;
  bool stageSourceWithoutDag(d3d9::RenderTapeExecutionTopologyV1Route route,
      d3d9::RenderTapeExecutionTopologyV1FallbackReason reason,
      core::CpuReadyTape::SourceRef source, std::uint64_t sequence,
      std::uint32_t commandBegin, std::uint32_t commandCount,
      bool explicitOrder, std::span<const std::uint32_t> physicalOrder) noexcept;
  bool prepareSourceEncode(core::CpuReadyTape::SourceRef source, std::uint64_t sequence,
      std::uint32_t commandBegin, std::uint32_t commandCount,
      bool explicitOrder, std::span<const std::uint32_t> physicalOrder) noexcept;
  bool observePreparedEncode(bool succeeded) noexcept;
  void fail() noexcept { if (context_) context_->fail(); }
  TapeTopologyInvocationHandle handle() const noexcept { return handle_; }
  bool suppressesFragmentStaging() const noexcept { return aggregateFragment_; }

private:
  std::shared_ptr<TapeTopologyInvocationContext> context_{};
  TapeTopologyInvocationHandle handle_{};
  ScopedTapeTopologyInvocation* previous_ = nullptr;
  std::vector<std::uint32_t> actualOrder_{};
  bool prepared_ = false;
  bool aggregateFragment_ = false;
};

// Thread-local, bounded pending receipts survive a synchronous backend scope,
// never an execution payload. Arms are capture+queue qualified, and every
// operation reacquires admission. Tentative candidates retain explicit owned
// invocation handles until the coordinator selects or rejects that source set.
std::shared_ptr<TapeTopologyInvocationContext> retainTapeTopologyInvocation(
    TapeRuntimeCaptureLease lease, std::span<const TapeTopologySourceRange> ranges,
    bool tentative = false) noexcept;
void registerTapeTopologySources(TapeRuntimeCaptureLease lease,
    std::span<const TapeTopologySourceRange> receipts) noexcept;
void rejectTapeTopologyCandidate(TapeRuntimeCaptureLease lease,
    std::span<const TapeTopologySourceRange> ranges) noexcept;
bool activateTapeTopologyCandidate(TapeRuntimeCaptureLease lease,
    std::span<const TapeTopologySourceRange> ranges, std::uint32_t fragments) noexcept;
std::shared_ptr<TapeTopologyInvocationContext> tapeTopologyCandidateContext(
    TapeRuntimeCaptureLease lease, std::span<const TapeTopologySourceRange> ranges,
    TapeTopologyInvocationHandle& handle) noexcept;
void foldTapeTopologyCandidate(TapeRuntimeCaptureLease lease,
    std::span<const TapeTopologySourceRange> ranges, bool finish) noexcept;
void observeTapeTopologyInlineTerminal(TapeRuntimeCaptureLease lease,
    TapeTopologySourceRange source) noexcept;
std::vector<TapeTopologySourceRange> tapeTopologySourceRanges(
    std::span<const core::metalqueue::ResolvedPublishedSource> sources) noexcept;
void registerTapeTopologyCompletionSources(TapeRuntimeCaptureLease lease,
    std::span<const core::metalqueue::QueueCompletionSource> sources) noexcept;
void stageTapeTopologySourceWithoutDag(core::SourcePayloadView payload, std::uint64_t sequence,
    const encoders::EncodeChunkOptions& options,
    d3d9::RenderTapeExecutionTopologyV1FallbackReason reason) noexcept;
void prepareTapeTopologySourceEncode(core::SourcePayloadView payload, std::uint64_t sequence,
    const encoders::EncodeChunkOptions& options) noexcept;

template <typename Fn>
decltype(auto) withTapeTopologyInvocation(TapeRuntimeCaptureLease lease,
    TapeTopologySourceRange range, Fn&& encode) {
  ScopedTapeTopologyInvocation scope(retainTapeTopologyInvocation(
      std::move(lease), std::span(&range, 1u)));
  return std::forward<Fn>(encode)();
}

} // namespace dxmt9::render
