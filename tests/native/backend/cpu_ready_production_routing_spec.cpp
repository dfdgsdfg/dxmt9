#include "device_c_common.hpp"
#include "device_c_cpu_ready_plan.hpp"
#include "device_c_chunk_replay.hpp"
#include "device_c_cpu_ready_transfer.hpp"
#include "device_c_render_tape_completion.hpp"
#include "device_c_render_tape_completion_plan.hpp"
#include "device_c_render_tape_publication.hpp"
#include "device_c_render_tape.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_action_profile_component.hpp"
#include "device_c_render_tape_provider.hpp"
#include "device_c_render_tape_controlled_cpu_v4.hpp"
#include "device_c_render_tape_execution_v4.hpp"
#include "device_c_render_tape_runtime_binding_v4.hpp"
#include "device_c_render_tape_runtime_identity_v4.hpp"
#include "render_tape_completion_fixture.hpp"
#include "render_tape_cpu_grammar.hpp"
#include "render_tape_cpu_general_grammar.hpp"
#include "render_tape_cpu_general_executor.hpp"
#include "render_tape_cpu_policy.hpp"
#include "render_tape_v4_capacity_policy.hpp"
#include "render_tape_cpu_source_admission.hpp"
#include "render_tape_cpu_destroy_fixture.hpp"
#include "render_tape_cpu_alias_fixture.hpp"
#include "render_tape_cpu_mipmap_fixture.hpp"
#include "render_tape_cpu_query_fixture.hpp"
#include "render_tape_cpu_event_checkpoint_fixture.hpp"
#include "render_tape_cpu_d24s8_fixture.hpp"
#include "render_tape_cpu_volume_fixture.hpp"
#include "render_tape_cpu_bootstrap_pages_fixture.hpp"
#include "render_tape_cpu_v4_consumer.hpp"
#include "render_tape_cpu_v4_admission.hpp"
#include "render_tape_cpu_resource_context.hpp"
#include "render_tape_test_metal_device.hpp"
#include "device_c_replay_offload.hpp"
#include "dxmt9/com.hpp"
#include "dxmt9/copy_materialization_ledger.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_direct_continuation.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_draw_encoder_draw_internal.hpp"
#include "dxmt9/dxmt9_draw_state.hpp"
#include "d3d9_pe_wire_handle.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <exception>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {

using dxmt9::test::CpuSourceOracleRow;
using dxmt9::test::decodeCpuSourceOracle;

struct RuntimeFixture;
void controlledCpuV4ConsumerUsesRealQueue();

// Keep the bounded no-worker policy proof tied to the actual grammar and
// queue ring, rather than silently proving a stale test-only capacity.
static_assert(dxmt9::test::kCpuTapeMaxRecordsPerRaw ==
              dxmt9::d3d9::render_tape_runtime::GeneralCpuAdmissionCaps{}.maxRecordsPerChunk);
static_assert(dxmt9::test::kCpuTapeMaxPendingPerRaw == dxmt9::kMaxQueuedChunks);
static_assert(dxmt9::test::kCpuTapeMinSlotHeadroomBytes >=
              dxmt9::core::kDirectSlotMinHeadroomBytes);

static_assert(!std::is_copy_constructible_v<
              dxmt9::d3d9::CpuReadySemanticTransfer>);
static_assert(!std::is_copy_assignable_v<
              dxmt9::d3d9::CpuReadySemanticTransfer>);
static_assert(std::is_move_constructible_v<
              dxmt9::d3d9::CpuReadySemanticTransfer>);

void digestBytes(std::uint64_t& hash, const void* data,
                 std::size_t size) noexcept {
  constexpr std::uint64_t kPrime = 1099511628211ull;
  const auto* bytes = static_cast<const std::byte*>(data);
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<std::uint8_t>(bytes[i]);
    hash *= kPrime;
  }
}

std::uint64_t digestByteSpan(std::span<const std::uint8_t> bytes) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  digestBytes(hash, bytes.data(), bytes.size());
  return hash;
}

const char* completionPlanDispositionName(
    dxmt9::d3d9::RenderTapeCompletionPlanDisposition disposition) noexcept {
  using D = dxmt9::d3d9::RenderTapeCompletionPlanDisposition;
  switch (disposition) {
  case D::Accepted: return "accepted";
  case D::InvalidSize: return "invalid-size";
  case D::InvalidHeader: return "invalid-header";
  case D::InvalidDigest: return "invalid-digest";
  case D::InvalidRoute: return "invalid-route";
  case D::InvalidRepeat: return "invalid-repeat";
  case D::InvalidCapacity: return "invalid-capacity";
  case D::InvalidEvent: return "invalid-event";
  case D::NonMonotonicSubmit: return "nonmonotonic-submit";
  case D::DuplicateSubmit: return "duplicate-submit";
  case D::DuplicateRelease: return "duplicate-release";
  case D::MissingRelease: return "missing-release";
  case D::InvalidReleaseBoundary: return "invalid-release-boundary";
  case D::RouteMismatch: return "route-mismatch";
  case D::StaleIteration: return "stale-iteration";
  case D::StaleEvent: return "stale-event";
  case D::UnexpectedSubmit: return "unexpected-submit";
  case D::DuplicateSubmission: return "duplicate-submission";
  case D::InvalidActualSource: return "invalid-actual-source";
  case D::ActualSourceOrder: return "actual-source-order";
  case D::ReleaseBeforeSubmit: return "release-before-submit";
  case D::SkippedReleaseFrontier: return "skipped-release-frontier";
  case D::DuplicateReleaseAttempt: return "duplicate-release";
  case D::CallbackFailure: return "callback-failure";
  case D::MissingSubmit: return "missing-submit";
  case D::Exhausted: return "exhausted";
  case D::Unresolved: return "unresolved";
  }
  return "unknown";
}

template <typename T>
void digestValue(std::uint64_t& hash, const T& value) noexcept {
  digestBytes(hash, &value, sizeof(value));
}

void digestHandle(std::uint64_t& hash, const dxmt9::core::Handle value) noexcept {
  digestValue(hash, value.value);
}

void digestRect(std::uint64_t& hash, const dxmt9::core::Rect& value) noexcept {
  digestValue(hash, value.left);
  digestValue(hash, value.top);
  digestValue(hash, value.right);
  digestValue(hash, value.bottom);
}

void digestColor(std::uint64_t& hash,
                 const dxmt9::core::ColorRGBA& value) noexcept {
  digestValue(hash, value.r);
  digestValue(hash, value.g);
  digestValue(hash, value.b);
  digestValue(hash, value.a);
}

void digestRange(std::uint64_t& hash,
                 const dxmt9::core::DrawPayloadRange& value) noexcept {
  digestValue(hash, value.offset);
  digestValue(hash, value.size);
}

void digestDrawParam(std::uint64_t& hash,
                     const dxmt9::core::DrawParam& value) noexcept {
  digestValue(hash, value.primitiveType);
  digestValue(hash, value.primitiveCount);
  digestValue(hash, value.startVertex);
  digestValue(hash, value.baseVertexIndex);
  digestValue(hash, value.startIndex);
  digestValue(hash, value.indexType);
  digestValue(hash, value.indexed);
  digestValue(hash, value.instanceCount);
  digestRange(hash, value.userVertexRange);
  digestRange(hash, value.userIndexRange);
  digestRange(hash, value.bindingOverrideRange);
  digestRange(hash, value.bindingSnapshotRange);
  digestValue(hash, value.uniformHandle.index);
  digestValue(hash, value.uniformHandle.generation);
  digestValue(hash, value.uniformHandle.hash);
}

void digestBindingSnapshot(
    std::uint64_t& hash,
    const dxmt9::core::DrawBufferBindingSnapshot& value) noexcept {
  digestValue(hash, value.metalHandle);
  // contentsAddress is process-local evidence, not replay semantics.
  digestValue(hash, value.byteSize);
  digestValue(hash, value.contentRevision);
}

void digestDrawPayloadSemantics(
    std::uint64_t& hash, const dxmt9::core::DrawParam& param,
    std::span<const dxmt9::core::u8> arena) noexcept {
  const auto digestRawRange = [&](dxmt9::core::DrawPayloadRange range) {
    const auto bytes = dxmt9::core::drawPayloadRangeBytes(range, arena);
    digestValue(hash, bytes.size());
    digestBytes(hash, bytes.data(), bytes.size());
  };
  digestRawRange(param.userVertexRange);
  digestRawRange(param.userIndexRange);

  const auto overrideBytes = dxmt9::core::drawPayloadRangeBytes(
      param.bindingOverrideRange, arena);
  digestValue(hash, overrideBytes.size());
  if (overrideBytes.size() == sizeof(dxmt9::core::DrawBindingOverride)) {
    dxmt9::core::DrawBindingOverride value{};
    std::memcpy(&value, overrideBytes.data(), sizeof(value));
    for (const auto& stream : value.streams) {
      digestHandle(hash, stream.buffer);
      digestValue(hash, stream.offset);
      digestValue(hash, stream.stride);
    }
    digestValue(hash, value.streamMask);
    digestHandle(hash, value.indexBuffer);
    digestValue(hash, value.indexType);
    digestValue(hash, value.indexBufferValid);
    digestValue(hash, value.alphaTestEnable);
    digestValue(hash, value.alphaTestFunc);
    digestValue(hash, value.alphaTestRef);
    digestValue(hash, value.alphaTestStateValid);
  } else {
    digestBytes(hash, overrideBytes.data(), overrideBytes.size());
  }

  const auto snapshotBytes = dxmt9::core::drawPayloadRangeBytes(
      param.bindingSnapshotRange, arena);
  digestValue(hash, snapshotBytes.size());
  if (snapshotBytes.size() == sizeof(dxmt9::core::DrawBindingSnapshot)) {
    dxmt9::core::DrawBindingSnapshot value{};
    std::memcpy(&value, snapshotBytes.data(), sizeof(value));
    for (const auto& stream : value.streams) {
      digestHandle(hash, stream.buffer);
      digestValue(hash, stream.offset);
      digestValue(hash, stream.stride);
      digestBindingSnapshot(hash, stream.snapshot);
    }
    digestValue(hash, value.streamMask);
    digestHandle(hash, value.indexBuffer);
    digestValue(hash, value.indexType);
    digestBindingSnapshot(hash, value.indexSnapshot);
    digestValue(hash, value.indexSnapshotValid);
  } else {
    digestBytes(hash, snapshotBytes.data(), snapshotBytes.size());
  }
}

// One command's storage-independent effective semantics. Split out of the
// whole-payload digest below so a raw whose commands are spread over several
// published sources can be compared against a single-source lane command by
// command, in source order: a per-source digest embeds that source's command
// count and cannot be concatenated.
std::uint64_t effectiveCommandDigest(
    const dxmt9::core::SourcePayloadView& payload,
    std::size_t index) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  {
    const auto source = payload.commandAt(index);
    const auto& command = source.command;
    digestValue(hash, command.kind);
    switch (command.kind) {
    case dxmt9::core::MetalCommandKind::DrawRun:
      // Draw state/PSO locators are checked by DrawDigest below; the
      // effective stream digest deliberately contains only storage-independent
      // command and draw payload semantics.
      for (const auto& param : command.drawParams) {
        digestValue(hash, param.primitiveType);
        digestValue(hash, param.primitiveCount);
        digestValue(hash, param.startVertex);
        digestValue(hash, param.baseVertexIndex);
        digestValue(hash, param.startIndex);
        digestValue(hash, param.indexType);
        digestValue(hash, param.indexed);
        digestValue(hash, param.instanceCount);
        digestDrawPayloadSemantics(hash, param, command.drawPayloadBytes);
      }
      break;
    case dxmt9::core::MetalCommandKind::Clear:
      if (source.clear) {
        digestValue(hash, source.clear->clearColor);
        digestValue(hash, source.clear->clearDepth);
        digestValue(hash, source.clear->clearStencil);
        digestColor(hash, source.clear->color);
        digestValue(hash, source.clear->depth);
        digestValue(hash, source.clear->stencil);
        for (const auto& rect : source.clear->rects) digestRect(hash, rect);
      }
      break;
    case dxmt9::core::MetalCommandKind::SurfaceCopy:
      if (command.surfaceCopy) {
        const auto& copy = *command.surfaceCopy;
        digestHandle(hash, copy.source);
        digestHandle(hash, copy.destination);
        digestRect(hash, copy.sourceRect);
        digestRect(hash, copy.destinationRect);
        digestValue(hash, copy.sourceLevel);
        digestValue(hash, copy.destinationLevel);
        digestValue(hash, copy.linear);
        digestValue(hash, copy.sourceSampleCount);
        digestValue(hash, copy.destinationSampleCount);
      }
      break;
    case dxmt9::core::MetalCommandKind::StretchRect:
      if (command.stretchRect) {
        const auto& stretch = *command.stretchRect;
        digestHandle(hash, stretch.source);
        digestHandle(hash, stretch.destination);
        digestRect(hash, stretch.sourceRect);
        digestRect(hash, stretch.destinationRect);
        digestValue(hash, stretch.linear);
        digestValue(hash, stretch.sourceSampleCount);
        digestValue(hash, stretch.destinationSampleCount);
      }
      break;
    case dxmt9::core::MetalCommandKind::Readback:
      if (command.readback) {
        const auto& readback = *command.readback;
        digestHandle(hash, readback.source);
        digestHandle(hash, readback.destination);
        digestRect(hash, readback.sourceRect);
        digestValue(hash, readback.sourceLevel);
        digestValue(hash, readback.sourceSampleCount);
        digestValue(hash, readback.destinationSampleCount);
      }
      break;
    case dxmt9::core::MetalCommandKind::ColorFill:
      if (command.colorFill) {
        const auto& fill = *command.colorFill;
        digestHandle(hash, fill.destination);
        digestRect(hash, fill.rect);
        digestColor(hash, fill.color);
      }
      break;
    case dxmt9::core::MetalCommandKind::DepthResolve:
      if (command.depthResolve) {
        const auto& resolve = *command.depthResolve;
        digestHandle(hash, resolve.msaaDepth);
        digestHandle(hash, resolve.intzDest);
      }
      break;
    case dxmt9::core::MetalCommandKind::GenerateMipmaps:
      if (command.generateMipmaps) {
        digestHandle(hash, command.generateMipmaps->texture);
      }
      break;
    case dxmt9::core::MetalCommandKind::Present:
      // The empty wire Present acquires a runtime swapchain source and
      // PresentId. Sequential Direct/Legacy oracle runs therefore compare the
      // ordered barrier kind, not those newly issued runtime identities.
      break;
    }
  }
  return hash;
}

std::uint64_t effectivePayloadDigest(
    const dxmt9::core::SourcePayloadView& payload) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  digestValue(hash, payload.commandCount());
  for (std::size_t index = 0; index < payload.commandCount(); ++index) {
    digestValue(hash, effectiveCommandDigest(payload, index));
  }
  return hash;
}

}  // namespace

namespace dxmt9 {

struct CommandQueueArenaLeaseTestAccess {
  // The borrowed wrapper/probe context is valid only while RuntimeFixture's
  // separate weak token is live. Retaining this state or the backend is not
  // wrapper/reset authority. Replay probes never recursively take mutex_.
  struct ControlledCpuFixtureAuthority {
    D9CDevice* owner = nullptr;
    Device* backend = nullptr;
    CommandQueue* queue = nullptr;
    bool workersDisabledAtIssue = false;
    std::atomic<bool> isolated{true};
    std::atomic<bool> producerClosed{false};
    std::atomic<bool> consumerJoined{true};
  };

  enum class ControlledCpuActualPhase {
    Empty, Represented, Submitted, Completed, Retired, Unknown,
  };
  enum class ControlledCpuFault {
    None, ObservationAllocation, AfterSubmit, AfterAcknowledge,
  };
  struct ControlledCpuBackendEvidence {
    test::CpuV4ConsumerIdentity identity{};
    ControlledCpuActualPhase phase = ControlledCpuActualPhase::Empty;
    bool borrowReleased = true;
    bool observedOutsideQueueMutex = false;
    bool queueQuarantined = false;
    std::size_t observed = 0u, submitted = 0u, acknowledged = 0u, retired = 0u;
  };
  struct ControlledCpuBackendState {
    mutable std::mutex evidenceMutex{};
    ControlledCpuBackendEvidence evidence{};
    std::atomic<bool> localStop{false}, poisoned{false};

    ControlledCpuBackendEvidence snapshot() const {
      std::lock_guard lock(evidenceMutex);
      return evidence;
    }
  };

  // Installed at construction, before the worker starts. The cold session
  // owns semantic results separately from this backend's metadata lifecycle.
  // Payload projections must not escape the synchronous pinned callback.
  using ControlledCpuPayloadObserver = std::function<void(CommandQueue&,
      const core::metalqueue::ReplaySourcePublicationReceipt&,
      const core::metalqueue::SynchronousSourcePayloadBorrow&)>;

  class ControlledCpuBackend {
  public:
    using Clock = std::chrono::steady_clock;
    using Identity = test::CpuV4ConsumerIdentity;
    using Failure = test::CpuV4ConsumerFailure;
    using Phase = ControlledCpuActualPhase;
    ControlledCpuBackend(ControlledCpuBackend&&) noexcept = default;
    ControlledCpuBackend& operator=(ControlledCpuBackend&&) noexcept = default;
    ControlledCpuBackend(const ControlledCpuBackend&) = delete;
    ControlledCpuBackend& operator=(const ControlledCpuBackend&) = delete;

    std::shared_ptr<ControlledCpuBackendState> state() const noexcept { return state_; }

    test::CpuV4ConsumerBinding binding() {
      const bool valid = visitQueue([](CommandQueue&) noexcept {});
      return valid ? binding_ : test::CpuV4ConsumerBinding{};
    }

    bool producerClosedAndWriterEmpty() {
      bool empty = false;
      return visitQueue([&](CommandQueue& queue) {
        std::lock_guard lock(queue.mutex_);
        empty = authority_->producerClosed.load(std::memory_order_acquire) &&
            controlledCpuWriterEmptyLocked(queue);
      }) && empty;
    }

    test::CpuV4ConsumerWait waitReady(Clock::time_point deadline) {
      auto result = test::CpuV4ConsumerWait::Failed;
      if (!visitQueue([&](CommandQueue& queue) {
            std::unique_lock lock(queue.mutex_);
            const bool awake = queue.encodeCv_.wait_until(lock, deadline, [&] {
              return state_->localStop.load(std::memory_order_acquire) ||
                  queue.stop_ || !queue.cpuReadyTape_.readyEmpty();
            });
            if (!awake) result = test::CpuV4ConsumerWait::Deadline;
            else if (queue.stop_ || state_->poisoned.load(std::memory_order_acquire))
              result = test::CpuV4ConsumerWait::Failed;
            else if (!queue.cpuReadyTape_.readyEmpty())
              result = test::CpuV4ConsumerWait::Ready;
            else if (authority_->producerClosed.load(std::memory_order_acquire) &&
                     controlledCpuWriterEmptyLocked(queue))
              result = test::CpuV4ConsumerWait::Stopped;
          })) return test::CpuV4ConsumerWait::LeaseRevoked;
      return result;
    }

    Failure observeReady(std::size_t remainingCommands,
                         test::CpuV4ConsumerObservation& out) {
      Failure result = Failure::Observation;
      const bool visited = visitQueue([&](CommandQueue& queue) {
        std::unique_lock lock(queue.mutex_);
        if (claimed_) { result = Failure::InvalidLifecycle; return; }
        std::array<core::CpuReadyTape::ReadyEntry, 1u> ready{};
        if (queue.stop_ || queue.cpuReadyTape_.readyEmpty() ||
            queue.cpuReadyTape_.copyReadyPrefix(ready) != 1u) return;
        if (!ready[0].semantic.commandCount ||
            ready[0].semantic.commandCount > remainingCommands) {
          result = Failure::Capacity;
          return;
        }
        core::metalqueue::ReadySlotSnapshot source{};
        // The sole consumer checked Ready under this same lock, so the
        // production dequeue's blocking predicate is already satisfied.
        if (!queue.queueLifecycle_.dequeueReadySlot(lock, source)) return;
        held_ = core::metalqueue::completionSourceForReadySlot(source);
        claimed_ = true; // Record the actual effect before any allocation.
        identity_ = {binding_.queue, binding_.captureToken, binding_.captureEpoch,
            source.seqId, source.sourceId.index, source.sourceId.generation,
            source.storage.firstPage, source.storage.pageCount, source.storage.generation};
        updateEvidence([&](auto& value) {
          value.identity = identity_;
          value.phase = Phase::Represented;
          value.borrowReleased = false;
        });
        const auto resolved = queue.queueLifecycle_.resolveRepresentedSource(source);
        if (!resolved.valid()) {
          updateEvidence([](auto& value) { value.borrowReleased = true; });
          return;
        }
        test::CpuV4ConsumerObservation observed;
        observed.identity = identity_;
        const bool borrowed = queue.queueLifecycle_.visitRepresentedSourceBorrows(
            lock, std::span<const core::metalqueue::ResolvedPublishedSource>(&resolved, 1u),
            [&](const core::metalqueue::SynchronousSourceBorrowBatch& batch) noexcept {
              return batch.visitAt(0u,
                  [&](const core::metalqueue::GenerationQualifiedSourceBorrow& borrow) noexcept {
                    struct Unlock {
                      std::unique_lock<std::mutex>& lock;
                      explicit Unlock(std::unique_lock<std::mutex>& value) : lock(value) { lock.unlock(); }
                      ~Unlock() { lock.lock(); }
                    } unlocked(lock);
                    return borrow.visitPayload(
                        [&](const core::metalqueue::SynchronousSourcePayloadBorrow& payload) noexcept {
                          try {
                            const bool outsideQueueMutex = !lock.owns_lock();
                            updateEvidence([&](auto& value) {
                              value.observedOutsideQueueMutex = outsideQueueMutex;
                            });
                            if (!outsideQueueMutex) return false;
                            if (fault_ == ControlledCpuFault::ObservationAllocation)
                              throw std::bad_alloc();
                            observed.rawOrdinal = borrow.metadata().rawOrdinal;
                            observed.sourceOrdinal = borrow.metadata().sourceOrdinal;
                            observed.commands.reserve(payload.commandCount());
                            for (std::size_t index = 0u; index < payload.commandCount(); ++index) {
                              const auto command = payload.commandAt(index);
                              if (command.command.drawParams.size() > UINT32_MAX) return false;
                              observed.commands.push_back({static_cast<std::uint32_t>(command.kind()),
                                  static_cast<std::uint32_t>(command.command.drawParams.size())});
                            }
                            if (payloadObserver_) {
                              const core::metalqueue::ReplaySourcePublicationReceipt publication{
                                  borrow.source(), borrow.metadata(), borrow.slotIndex(), borrow.hasPresent()};
                              payloadObserver_(queue, publication, payload);
                            }
                            return true;
                          } catch (const std::bad_alloc&) { result = Failure::Allocation; }
                          catch (...) { result = Failure::Backend; }
                          return false;
                        });
                  });
            });
        updateEvidence([](auto& value) { value.borrowReleased = true; });
        if (!borrowed || observed.commands.size() != source.commandCount) return;
        out = std::move(observed);
        updateEvidence([](auto& value) { ++value.observed; });
        result = Failure::None;
      });
      return visited ? result : Failure::LeaseRevoked;
    }

    bool submit(const Identity& identity) {
      bool applied = false;
      return visitQueue([&](CommandQueue& queue) {
        std::unique_lock lock(queue.mutex_);
        if (!exact(identity) || actualPhaseLocked(queue) != Phase::Represented || queue.stop_) return;
        core::metalqueue::QueueSubmissionRecord record{};
        record.testOnlyAllowNullCommandBuffer = true;
        record.slotIndex = held_.slotIndex;
        record.seqId = held_.seqId;
        core::metalqueue::QueueLifecycleController::PendingCompletion pending{};
        pending.slotIndex = held_.slotIndex;
        pending.seqId = held_.seqId;
        const std::array sources{held_};
        if (!record.assignFixedCompletionSources(sources) ||
            !pending.assignFixedCompletionSources(sources)) return;
        const bool submitted = queue.queueLifecycle_.submitEncodedSubmission(lock, record);
        const auto actual = actualPhaseLocked(queue);
        if (actual == Phase::Submitted) updateEvidence([](auto& value) { ++value.submitted; });
        if (!submitted || actual != Phase::Submitted || fault_ == ControlledCpuFault::AfterSubmit) return;
        // Null-CB submit does not enqueue completion itself. Only the owned,
        // exact locator receipt enters the real pending FIFO, once.
        queue.queueLifecycle_.enqueuePendingCompletionForTest(std::move(pending));
        pendingEnqueued_ = true;
        applied = true;
      }) && applied;
    }

    bool acknowledge(const Identity& identity) {
      bool applied = false;
      return visitQueue([&](CommandQueue& queue) {
        {
          std::lock_guard lock(queue.mutex_);
          if (!exact(identity) || !pendingEnqueued_ || acknowledgementRequested_ ||
              actualPhaseLocked(queue) != Phase::Submitted || queue.stop_ ||
              queue.queueLifecycle_.pendingCompletionCountForTest() != 1u) return;
          acknowledgementRequested_ = true;
        }
        const bool processed = queue.queueLifecycle_.processOnePendingCompletion();
        std::lock_guard lock(queue.mutex_);
        const auto actual = actualPhaseLocked(queue);
        if (actual == Phase::Completed) updateEvidence([](auto& value) { ++value.acknowledged; });
        applied = processed && actual == Phase::Completed &&
            fault_ != ControlledCpuFault::AfterAcknowledge;
      }) && applied;
    }

    bool retire(const Identity& identity) {
      bool applied = false;
      return visitQueue([&](CommandQueue& queue) {
        std::unique_lock lock(queue.mutex_);
        if (!exact(identity) || !acknowledgementRequested_ || queue.stop_ ||
            actualPhaseLocked(queue) != Phase::Completed ||
            queue.completedSeqQueue_.empty() || queue.completedSeqQueue_.front() != held_.seqId) return;
        const bool finished = queue.queueLifecycle_.runFinishIteration(lock);
        const auto actual = actualPhaseLocked(queue);
        if (actual == Phase::Retired) updateEvidence([](auto& value) { ++value.retired; });
        applied = finished && actual == Phase::Retired;
      }) && applied;
    }

    void requestStop() noexcept {
      if (!backend_) return;
      auto& queue = backend_->queue();
      {
        std::lock_guard lock(queue.mutex_);
        state_->localStop.store(true, std::memory_order_release);
      }
      queue.encodeCv_.notify_all(); // Never sets the real queue's stop latch.
    }

    void poison(Failure) noexcept {
      state_->poisoned.store(true, std::memory_order_release);
      if (backend_) backend_->queue().queueLifecycle_.poisonTapeFailure();
    }

    bool cleanup(Clock::time_point) noexcept {
      // Independent pin supports safety inspection after weak lease expiry;
      // cleanup never submits, acknowledges, finishes, aborts or republishes.
      if (!backend_) return !claimed_;
      auto& queue = backend_->queue();
      std::lock_guard lock(queue.mutex_);
      if (!claimed_) return true;
      const auto actual = actualPhaseLocked(queue);
      if (actual == Phase::Retired) {
        claimed_ = false;
        held_ = {};
        identity_ = {};
        pendingEnqueued_ = acknowledgementRequested_ = false;
        return true;
      }
      queue.queueLifecycle_.poisonTapeFailureLocked();
      updateEvidence([](auto& value) { value.queueQuarantined = true; });
      // Retained Represented/Submitted/Completed ownership is safe fail-stop,
      // not cleanup completion. Fixture teardown owns its eventual reset.
      return false;
    }

  private:
    friend struct CommandQueueArenaLeaseTestAccess;
    ControlledCpuBackend(d3d9::RenderTapeControlledCpuV4Lease lease,
        std::shared_ptr<Device> backend, std::shared_ptr<ControlledCpuFixtureAuthority> authority,
        test::CpuV4ConsumerBinding binding, ControlledCpuFault fault,
        ControlledCpuPayloadObserver payloadObserver = {})
        : lease_(std::move(lease)), backend_(std::move(backend)), authority_(std::move(authority)),
          binding_(binding), state_(std::make_shared<ControlledCpuBackendState>()), fault_(fault),
          payloadObserver_(std::move(payloadObserver)) {}

    template <typename Fn> bool visitQueue(Fn&& fn) {
      using Callable = std::remove_reference_t<Fn>;
      struct Visit { ControlledCpuBackend* self; Callable* fn; } visit{this, &fn};
      return lease_.visitBackend(d3d9::RenderTapeControlledCpuV4Boundary::Replay, &visit,
          [](void* context, D9CDevice*, Device& backend) noexcept {
            auto& value = *static_cast<Visit*>(context);
            if (&backend != value.self->backend_.get() ||
                &backend.queue() != value.self->authority_->queue) return false;
            try { std::invoke(*value.fn, backend.queue()); return true; }
            catch (...) { return false; }
          });
    }
    template <typename Fn> void updateEvidence(Fn&& fn) {
      // Only queue -> evidence ordering is permitted. Readers copy evidence
      // without taking the queue mutex; stop/poison never touch held locators.
      std::lock_guard lock(state_->evidenceMutex);
      std::invoke(fn, state_->evidence);
    }
    bool exact(const Identity& identity) const noexcept {
      return claimed_ && identity.valid() && identity == identity_;
    }
    Phase actualPhaseLocked(CommandQueue& queue) {
      Phase phase = Phase::Unknown;
      const auto oldest = queue.cpuReadyTape_.oldestResident();
      if (oldest && oldest->source == held_.source && oldest->seqId == held_.seqId) {
        switch (oldest->state) {
        case core::CpuReadyTape::State::Represented: phase = Phase::Represented; break;
        case core::CpuReadyTape::State::Submitted: phase = Phase::Submitted; break;
        case core::CpuReadyTape::State::Completed: phase = Phase::Completed; break;
        default: break;
        }
      } else if (queue.completedSeqIdLocked() >= held_.seqId &&
                 queue.cpuReadyTape_.sourceGenerationAt(held_.source.id.index) != held_.source.id.generation)
        phase = Phase::Retired;
      updateEvidence([&](auto& value) { value.phase = phase; });
      return phase;
    }

    d3d9::RenderTapeControlledCpuV4Lease lease_{};
    std::shared_ptr<Device> backend_{};
    std::shared_ptr<ControlledCpuFixtureAuthority> authority_{};
    test::CpuV4ConsumerBinding binding_{};
    std::shared_ptr<ControlledCpuBackendState> state_{};
    ControlledCpuFault fault_ = ControlledCpuFault::None;
    ControlledCpuPayloadObserver payloadObserver_{};
    // Worker-only claim state; control-thread stop/poison use atomics above.
    core::metalqueue::QueueCompletionSource held_{};
    Identity identity_{};
    bool claimed_ = false, pendingEnqueued_ = false, acknowledgementRequested_ = false;
  };

  static d3d9::RenderTapeControlledCpuV4Capability mintControlledCpu(
      D9CDevice* owner, std::shared_ptr<Device> backend, std::weak_ptr<void> lifetime,
      ControlledCpuFixtureAuthority& authority) noexcept {
    return d3d9::RenderTapeControlledCpuV4Capability::mint(owner, std::move(backend),
        std::move(lifetime), &authority, controlledCpuIsolationProbe);
  }

  static std::optional<ControlledCpuBackend> makeControlledCpuBackend(
      const d3d9::RenderTapeControlledCpuV4Capability& capability,
      std::shared_ptr<Device> backend, std::shared_ptr<ControlledCpuFixtureAuthority> authority,
      const D9CRenderTapeIdentityCaptureV4ArmResult& arm,
      ControlledCpuPayloadObserver payloadObserver = {}) {
    if (!backend || !authority || backend.get() != authority->backend ||
        arm.status != D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE ||
        arm.version != 4u || !arm.captureToken || !arm.captureEpoch || !arm.queue)
      return std::nullopt;
    test::CpuV4ConsumerBinding binding{};
    struct Context {
      ControlledCpuFixtureAuthority* authority;
      const D9CRenderTapeIdentityCaptureV4ArmResult* arm;
      test::CpuV4ConsumerBinding* binding;
    } context{authority.get(), &arm, &binding};
    const bool valid = capability.lease().visitBackend(
        d3d9::RenderTapeControlledCpuV4Boundary::Replay, &context,
        [](void* input, D9CDevice* owner, Device& backend) noexcept {
          auto& value = *static_cast<Context*>(input);
          std::lock_guard bindingLock(owner->renderTapeRuntimeIdentityV4Mutex);
          const auto* runtime = owner->renderTapeRuntimeBindingV4.get();
          if (!runtime || runtime->domain != d3d9::RenderTapeRuntimeBindingV4Domain::ControlledCpu ||
              runtime->captureToken != value.arm->captureToken ||
              runtime->captureEpoch != value.arm->captureEpoch || runtime->queue != value.arm->queue ||
              value.authority->backend != &backend || value.authority->queue != &backend.queue()) return false;
          auto& queue = backend.queue();
          std::lock_guard queueLock(queue.mutex_);
          if (queue.queryQueueIdentity_ != value.arm->queue || !controlledCpuQuiescentLocked(queue)) return false;
          *value.binding = {value.arm->queue, value.arm->captureToken, value.arm->captureEpoch,
              queue.completedSeqIdLocked()};
          return true;
        });
    if (!valid) return std::nullopt;
    return ControlledCpuBackend(capability.lease(), std::move(backend), std::move(authority),
        binding, ControlledCpuFault::None, std::move(payloadObserver));
  }

  static bool closeControlledCpuProducer(ControlledCpuFixtureAuthority& authority) {
    if (!authority.queue || !authority.isolated.load(std::memory_order_acquire) ||
        authority.producerClosed.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(authority.queue->mutex_);
    if (!controlledCpuWriterEmptyLocked(*authority.queue)) return false;
    authority.producerClosed.store(true, std::memory_order_release);
    return true;
  }

  static bool controlledCpuQueueStopped(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.stop_;
  }

private:
  friend struct ::RuntimeFixture;
  static bool controlledCpuWriterEmptyLocked(CommandQueue& queue) noexcept {
    return !queue.arenaAdmissionActive_.load(std::memory_order_relaxed) &&
        !queue.activeArenaBuild_.load(std::memory_order_relaxed) &&
        !queue.activeDirectChunkSlotBuild_.load(std::memory_order_relaxed) &&
        !queue.arenaBuildContext_ && !queue.directChunkSlotBuildContext_ &&
        (!queue.writingSlot_ || (*queue.writingSlot_ < queue.slots_.size() &&
         queue.slots_[*queue.writingSlot_].payload &&
         queue.slots_[*queue.writingSlot_].payload->commandsEmpty()));
  }
  static bool controlledCpuQuiescentLocked(CommandQueue& queue) noexcept {
    return !queue.stop_ && !queue.encodeThread_.joinable() && !queue.finishThread_.joinable() &&
        !queue.completionThread_.joinable() && !queue.initializer_ &&
        queue.renderPartitionConfig_.sourceIdentity.resolved != dxmt9::render::SourceIdentityMode::SegmentSerial &&
        !queue.arenaBuildPoisoned_.load(std::memory_order_relaxed) &&
        !queue.queryPendingTerminal_.load(std::memory_order_relaxed) &&
        !queue.queueLifecycle_.queryCompletionFailed() && controlledCpuWriterEmptyLocked(queue) &&
        queue.cpuReadyTape_.residentCount() == 0u && queue.cpuReadyTape_.readyEmpty() &&
        !queue.inflightCount_ && queue.completedSeqQueue_.empty() &&
        queue.completedSeqIdLocked() == queue.lastCommittedSeqId_ &&
        queue.queueLifecycle_.pendingCompletionCountForTest() == 0u;
  }
  static bool controlledCpuIsolationProbe(void* context, D9CDevice* owner,
      const Device& backend, d3d9::RenderTapeControlledCpuV4Boundary boundary) noexcept {
    const auto* authority = static_cast<ControlledCpuFixtureAuthority*>(context);
    if (!authority || !owner || owner != authority->owner || &backend != authority->backend ||
        !authority->queue || !authority->workersDisabledAtIssue ||
        !authority->isolated.load(std::memory_order_acquire)) return false;
    if (boundary == d3d9::RenderTapeControlledCpuV4Boundary::Replay) return true;
    try {
      std::lock_guard lock(authority->queue->mutex_);
      return authority->consumerJoined.load(std::memory_order_acquire) &&
          controlledCpuQuiescentLocked(*authority->queue) &&
          (boundary != d3d9::RenderTapeControlledCpuV4Boundary::Finish ||
           authority->producerClosed.load(std::memory_order_acquire));
    } catch (...) { return false; }
  }
  // Private queue-only seam: local labels deliberately do not install a V4
  // root, arm a capture, qualify provenance or authorize finalization.
  static std::optional<ControlledCpuBackend> makeQueueOnlyControlledCpuBackendForTest(
      const d3d9::RenderTapeControlledCpuV4Capability& capability,
      std::shared_ptr<Device> backend, std::shared_ptr<ControlledCpuFixtureAuthority> authority,
      ControlledCpuFault fault) {
    if (!backend || !authority || backend.get() != authority->backend) return std::nullopt;
    test::CpuV4ConsumerBinding binding{};
    struct Context { test::CpuV4ConsumerBinding* binding; } context{&binding};
    const bool valid = capability.lease().visitBackend(
        d3d9::RenderTapeControlledCpuV4Boundary::Mint, &context,
        [](void* input, D9CDevice*, Device& backend) noexcept {
          auto& queue = backend.queue();
          std::lock_guard lock(queue.mutex_);
          static std::atomic<std::uint64_t> token{1u};
          const auto local = token.fetch_add(1u, std::memory_order_relaxed);
          if (!local || !controlledCpuQuiescentLocked(queue)) return false;
          *static_cast<Context*>(input)->binding = {queue.queryQueueIdentity_, local, 1u,
              queue.completedSeqIdLocked()};
          return true;
        });
    if (!valid) return std::nullopt;
    return ControlledCpuBackend(capability.lease(), std::move(backend), std::move(authority), binding, fault);
  }

public:
  struct CompletionResult {
    bool dequeued = false;
    bool arena = false;
    bool clear = false;
    bool hasPresent = false;
    bool finalPresent = false;
    bool submitted = false;
    bool completed = false;
    bool reclaimed = false;
    std::uint64_t seqId = 0;
    std::uint64_t rawOrdinal = 0;
    std::uint64_t sourceOrdinal = 0;
    std::uint64_t effectiveDigest = 0;
    // One entry per command of this source, in source order. Concatenating
    // these across the sources one raw published gives that raw's exact
    // ordered command identity/parameter stream, which a per-source digest
    // cannot express because it embeds the source's own command count.
    std::vector<std::uint64_t> commandDigests{};
    std::vector<std::uint8_t> commandKinds{};
    std::size_t commandCount = 0;
    std::size_t arenaSegmentCount = 0;
    core::Handle presentSource{};
    core::metalqueue::QueueCompletionSource completion{};
  };

  struct BufferLifetimeEvidence {
    bool alive = false;
    bool destroyPending = false;
    std::uint64_t lastUsedSeq = 0;
    std::uint64_t completedSeq = 0;
  };

  struct DrawDigest {
    std::uint64_t record = 0;
    std::uint64_t pso = 0;
    std::uint64_t hot = 0;
    std::uint64_t params = 0;
    std::uint64_t payload = 0;
    core::DrawParam param{};
    std::size_t payloadBytes = 0;
  };

  static std::uint64_t nextSeqId(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.nextSeqId_;
  }

  static std::uint64_t completedSeqId(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.completedSeqIdLocked();
  }

  static void bindDevice(CommandQueue& queue, WMT::Device device) {
    std::lock_guard lock(queue.mutex_);
    queue.device_ = device;
  }

  // Keep the CPU-only fixture's upload path on the same Pool entry points as
  // DeviceImpl. The watermark comes from the queue's owner-published
  // completion state so DEFAULT+DYNAMIC DISCARD rotation uses the real
  // admission value rather than a test-local counter.
  static void uploadBufferData(CommandQueue& queue, WMT::Device device,
                               core::BufferHandle handle,
                               std::span<const std::uint8_t> bytes) {
    std::lock_guard lock(queue.mutex_);
    queue.pool_.uploadBufferData(device, handle.value, bytes.data(),
                                 bytes.size(), queue.completedSeqIdLocked());
  }

  static void uploadBufferDataRange(
      CommandQueue& queue, WMT::Device device, core::BufferHandle handle,
      std::span<const std::uint8_t> fullBytes, std::uint64_t offset,
      std::uint64_t byteCount) {
    if (offset > fullBytes.size() ||
        byteCount > fullBytes.size() - static_cast<std::size_t>(offset)) {
      return;
    }
    const auto* rangeBytes =
        byteCount == 0u ? nullptr : fullBytes.data() + offset;
    std::lock_guard lock(queue.mutex_);
    queue.pool_.uploadBufferDataRange(
        device, handle.value, offset, rangeBytes,
        static_cast<std::size_t>(byteCount), queue.completedSeqIdLocked());
  }

  static std::size_t readyCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.readyCount();
  }

  static std::size_t residentCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.residentCount();
  }

  struct ControlledCpuParkSnapshot {
    bool acquired = false, writerPressure = false, writing = false;
    std::size_t ready = 0u, resident = 0u, inflight = 0u;
  };

  // Read-only, bounded diagnostic. Full storage alone is not evidence that
  // the producer actually entered its ordinary publication wait.
  static ControlledCpuParkSnapshot controlledCpuParkSnapshot(CommandQueue& queue) {
    const bool pressure = queue.queueLifecycle_.producerWriterPressureActive();
    std::unique_lock lock(queue.mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return {};
    return {.acquired = true, .writerPressure = pressure,
            .writing = queue.writingSlot_.has_value(),
            .ready = queue.cpuReadyTape_.readyCount(),
            .resident = queue.cpuReadyTape_.residentCount(),
            .inflight = queue.inflightCount_};
  }

  static std::optional<core::CpuReadyTape::State> oldestSourceState(
      CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    const auto oldest = queue.cpuReadyTape_.oldestResident();
    return oldest ? std::optional<core::CpuReadyTape::State>(oldest->state)
                  : std::nullopt;
  }

  static bool publishWriting(CommandQueue& queue) {
    std::unique_lock lock(queue.mutex_);
    if (!queue.writingSlot_ ||
        queue.slots_[*queue.writingSlot_].payload->commandsEmpty()) return true;
    return queue.queueLifecycle_.commitCurrentChunk(lock, kMaxQueuedChunks);
  }

  static std::uint64_t reclaimCompletedPool(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    const auto completed = queue.completedSeqIdLocked();
    queue.pool_.reclaimCompleted(completed);
    return completed;
  }

  static core::metalqueue::QueueCompletionSource peekReadyCompletion(
      CommandQueue& queue) {
    using namespace core::metalqueue;
    std::lock_guard lock(queue.mutex_);
    std::array<core::CpuReadyTape::ReadyEntry, 1> entries{};
    if (queue.cpuReadyTape_.copyReadyPrefix(entries) != 1u) {
      return {};
    }
    const auto& entry = entries[0];
    ReadySlotSnapshot snapshot{
        .slotIndex = entry.controlIndex,
        .seqId = entry.seqId,
        .metadata = entry.metadata,
        .semantic = entry.semantic,
        .hasPresent = entry.semantic.hasPresent(),
        .commandBegin = 0u,
        .commandCount = entry.semantic.commandCount,
        .sourceId = entry.source.id,
        .storage = entry.source.storage,
    };
    return completionSourceForReadySlot(snapshot);
  }

  struct NoWaitHeadroom {
    dxmt9::test::CpuTapeSourceAdmissionInputs inputs{};

    bool admitsOne() const noexcept {
      return dxmt9::test::admitCpuTapeSource(inputs) ==
             dxmt9::test::CpuTapeSourceAdmission::Admitted;
    }
  };

  static bool cpuTapeWorkersDisabled(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return !queue.stop_ && !queue.encodeThread_.joinable() &&
        !queue.finishThread_.joinable() && !queue.completionThread_.joinable();
  }

  // Read the same live control/tape/inflight state consulted by production
  // writer admission. This is intentionally a snapshot, not a reservation;
  // the single-thread fixture rechecks it immediately before each mutation.
  static NoWaitHeadroom noWaitHeadroom(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    const bool writerOwned = queue.writingSlot_.has_value();
    const bool writerIndexValid =
        writerOwned && *queue.writingSlot_ < queue.slots_.size();
    const auto* writer = writerIndexValid ? &queue.slots_[*queue.writingSlot_]
                                          : nullptr;
    const bool writerValid =
        writer && writer->state == core::ChunkSlot::State::Writing &&
        writer->sourceId.valid() && writer->storage.valid() &&
        writer->payload != nullptr &&
        queue.cpuReadyTape_.resolveForWrite(core::CpuReadyPublicationTicket{
            .id = writer->sourceId,
            .storage = writer->storage,
        }) == writer->payload;
    const bool writerEmpty =
        writerValid && writer->payload->commandsEmpty();
    const bool nextControlFree =
        queue.writeIndex_ < queue.slots_.size() &&
        queue.slots_[queue.writeIndex_].state == core::ChunkSlot::State::Free;
    return {.inputs = {
                .stopped = queue.stop_,
                .writerOwned = writerOwned,
                .writerValid = writerValid,
                .writerEmpty = writerEmpty,
                .nextControlFree = nextControlFree,
                // This is deliberately a one-source probe. The caller must
                // re-read it before every publication; it is never summed
                // or used as a two-source aggregate credit.
                .tapeCanReserve = queue.cpuReadyTape_.canReserve(),
                .inflightCount = queue.inflightCount_,
                .inflightLimit = kMaxQueuedChunks,
            }};
  }

  static std::size_t writingCommandCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    if (!queue.writingSlot_ || *queue.writingSlot_ >= queue.slots_.size() ||
        !queue.slots_[*queue.writingSlot_].payload) {
      return 0;
    }
    return queue.slots_[*queue.writingSlot_].payload->commandCount();
  }

  static std::optional<d3d9::RenderTapeExecutionV3::WritingContinuation>
  writingContinuation(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    if (!queue.writingSlot_ || *queue.writingSlot_ >= queue.slots_.size())
      return std::nullopt;
    const auto& slot = queue.slots_[*queue.writingSlot_];
    const auto state = queue.cpuReadyTape_.state(slot.sourceId, slot.storage);
    if (!slot.payload || slot.payload->commandsEmpty() ||
        state != core::CpuReadyTape::State::Writing)
      return std::nullopt;
    return d3d9::RenderTapeExecutionV3::WritingContinuation{
        .writer = {queue.queryQueueIdentity_, slot.sourceId.index,
                   slot.sourceId.generation, slot.storage.generation},
        .source = {.id = slot.sourceId, .storage = slot.storage},
        .commandCount = slot.payload->commandCount()};
  }

  static void reserveDirectContinuationHeadroom(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    if (!queue.writingSlot_ || *queue.writingSlot_ >= queue.slots_.size() ||
        !queue.slots_[*queue.writingSlot_].payload) {
      return;
    }
    auto& slot = *queue.slots_[*queue.writingSlot_].payload;
    constexpr std::size_t kDrawHeadroom = 16u;
    slot.commandHeaders.reserve(kDrawHeadroom);
    slot.drawHotStates.reserve(kDrawHeadroom);
    slot.drawShaderLayouts.reserve(kDrawHeadroom);
    slot.drawDebugSnapshots.reserve(kDrawHeadroom);
    slot.drawPsoSubviews.reserve(kDrawHeadroom);
    slot.drawUniformFixedPayloads.reserve(kDrawHeadroom);
    slot.drawUniformVertexConstants.reserve(kDrawHeadroom);
    slot.drawUniformPixelConstants.reserve(kDrawHeadroom);
    slot.drawUniformPayloads.reserve(kDrawHeadroom);
    slot.drawParams.reserve(kDrawHeadroom);
    slot.drawRunRecords.reserve(kDrawHeadroom);
    // The direct planner accounts for binding override/snapshot records in
    // this byte arena even for non-UP draws. Leave a deliberately generous,
    // explicit byte budget so the fixture proves the admission predicate
    // rather than depending on vector growth heuristics.
    slot.drawPayloadArena.reserve(1u << 20);
    slot.drawUniformVertexConstantBytes.reserve(
        kDrawHeadroom * sizeof(core::VertexShaderConstants));
    slot.drawUniformPixelConstantBytes.reserve(
        kDrawHeadroom * sizeof(core::PixelShaderConstants));
    slot.reserveDrawUniformPayloadLookup(kDrawHeadroom);
    slot.reserveDrawUniformStageLookup(
        slot.drawUniformVertexConstants,
        slot.drawUniformVertexConstantsLookupHeads,
        slot.drawUniformVertexConstantsLookupTails,
        slot.drawUniformVertexConstantsLookupNext, kDrawHeadroom);
    slot.reserveDrawUniformStageLookup(
        slot.drawUniformPixelConstants,
        slot.drawUniformPixelConstantsLookupHeads,
        slot.drawUniformPixelConstantsLookupTails,
        slot.drawUniformPixelConstantsLookupNext, kDrawHeadroom);
  }

  static core::DetachedCapacityReceipt qualifyEmptyWritingCapacity(
      CommandQueue& queue, const core::SourcePayloadCapacity& plan) {
    std::lock_guard lock(queue.mutex_);
    if (!queue.writingSlot_) return {};
    auto& control = queue.slots_[*queue.writingSlot_];
    auto& payload = *control.payload;
    const auto index = queue.cpuReadyTape_.compatibilityPayloadIndex(
        {.id = control.sourceId, .storage = control.storage});
    if (!index) return {};
    std::array<std::uint64_t, core::kDirectSlotCapacityPayloadCount> actual{};
    for (std::size_t i = 0; i < actual.size(); ++i) {
      const auto bytes = queue.cpuReadyTape_.compatibilityPayloadRetainedBytes(i);
      if (!bytes) return {};
      actual[i] = *bytes;
    }
    auto lease = core::LeaseHeld::tryAcquire(
        queue.directSlotCapacityLedger_, payload, actual, *index, 1ull << 30,
        core::directSlotProvisionRetainedBytes(plan));
    auto staged = core::StagedDirectSlot::create(std::move(lease), plan);
    if (!staged.valid() || !std::move(staged).commit()) return {};
    const auto entry = queue.directSlotCapacityLedger_.entries[*index];
    payload.admissionCapacityReceipt = core::DetachedCapacityReceipt::ledgerQualified(
        static_cast<std::uint32_t>(*index), entry.generation, entry.retainedBytes);
    return payload.admissionCapacityReceipt;
  }

  static core::DetachedCapacityReceipt writingCapacityReceipt(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.writingSlot_
        ? queue.slots_[*queue.writingSlot_].payload->admissionCapacityReceipt
        : core::DetachedCapacityReceipt{};
  }

  static bool ordinaryCapacityFailure(CommandQueue& queue, unsigned faultPoint) {
    std::unique_lock lock(queue.mutex_);
    auto& payload = *queue.slots_[*queue.writingSlot_].payload;
    unsigned point = 0;
    try {
      queue.queueLifecycle_.mutateWritingPayload(payload, [&]() -> bool {
#define DXMT9_FAIL_ORDINARY_RESERVE(region, plan, storage, element, physical, \
                                 provision, allocation, lookup, owner)      \
        DXMT9_DIRECT_CHUNK_SLOT_EXPAND_PHYSICAL_##physical(                 \
          payload.storage.reserve(payload.storage.capacity() + 1);         \
          if (point++ == faultPoint) throw std::bad_alloc();)
        DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(DXMT9_FAIL_ORDINARY_RESERVE)
#undef DXMT9_FAIL_ORDINARY_RESERVE
        return true;
      });
      return false;
    } catch (const std::bad_alloc&) {
      const auto receipt = payload.admissionCapacityReceipt;
      return queue.stop_ && queue.cpuReadyTape_.readyCount() == 0 &&
          receipt.retainedBytes() == core::directSlotPhysicalRetainedBytes(payload) &&
          queue.directSlotCapacityLedger_.entries[receipt.payloadIndex()].retainedBytes ==
              receipt.retainedBytes() && queue.directSlotCapacityLedger_.stagedBytes == 0;
    }
  }

  static void growWithoutCapacityWitness(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    auto& payload = *queue.slots_[*queue.writingSlot_].payload;
    payload.appendClear(core::ClearDesc{});
    payload.appendClear(core::ClearDesc{});
  }

  static void enableDirectLifecycleObservation(CommandQueue& queue) {
    queue.pipelineLifecycleObserver_ =
        std::make_unique<dxmt9::queue::PipelineLifecycleObserver>(true, true);
    queue.bindSelfLifecycle({});
  }

  static queue::DirectSourceLifecycleState directLifecycleState(
      CommandQueue& commandQueue) {
    std::lock_guard lock(commandQueue.mutex_);
    return commandQueue.pipelineLifecycleObserver_
        ? commandQueue.pipelineLifecycleObserver_->directState()
        : queue::DirectSourceLifecycleState{};
  }

  static queue::DirectSourceLifecycleError directLifecycleError(
      CommandQueue& commandQueue) {
    std::lock_guard lock(commandQueue.mutex_);
    return commandQueue.pipelineLifecycleObserver_
        ? commandQueue.pipelineLifecycleObserver_->directError()
        : queue::DirectSourceLifecycleError::InvalidTransition;
  }

  static bool stopped(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.stop_;
  }

  static DrawDigest writingDrawDigest(CommandQueue& queue,
                                      std::size_t commandIndex = 0u) {
    std::lock_guard lock(queue.mutex_);
    DrawDigest result{};
    if (!queue.writingSlot_) return result;
    const core::SourcePayloadView view(
        *queue.slots_[*queue.writingSlot_].payload);
    if (commandIndex >= view.commandCount()) return result;
    const auto& command = view.commandAt(commandIndex).command;
    constexpr std::uint64_t offset = 1469598103934665603ull;
    result.record = result.pso = result.hot = result.params = result.payload =
        offset;
    if (command.drawRunRecord) {
      const auto& record = *command.drawRunRecord;
      digestValue(result.record, record.stateIndex);
      digestValue(result.record, record.firstParam);
      digestValue(result.record, record.paramCount);
      digestValue(result.record, record.payloadOffset);
      digestValue(result.record, record.payloadSize);
      digestValue(result.record, record.uniformHandle.index);
      digestValue(result.record, record.uniformHandle.generation);
      digestValue(result.record, record.uniformHandle.hash);
      digestValue(result.record, record.invariant.viewportScissorHash);
      digestValue(result.record, record.invariant.runStableBindingHash);
      digestValue(result.record, record.invariant.streamMask);
      digestValue(result.record, record.invariant.textureMask);
      digestValue(result.record, record.invariant.samplerStateMask);
    }
    if (command.drawPsoSubview) {
      const auto& pso = *command.drawPsoSubview;
      digestValue(result.pso, pso.hasShaderContext);
      digestValue(result.pso, pso.vertexShaderHash);
      digestValue(result.pso, pso.pixelShaderHash);
      digestValue(result.pso, pso.vertexDeclHash);
      digestValue(result.pso, pso.renderStateHash);
      digestValue(result.pso, pso.textureMask);
      digestValue(result.pso, pso.samplerStateMask);
      digestValue(result.pso, pso.renderTargetMask);
      for (const auto handle : pso.colorAttachmentHandles) {
        digestHandle(result.pso, handle);
      }
      digestHandle(result.pso, pso.depthStencilHandle);
    }
    if (command.drawState.hot) {
      const auto& hot = *command.drawState.hot;
      digestValue(result.hot, hot.key.renderStateHash);
      digestValue(result.hot, hot.streamMask);
      digestValue(result.hot, hot.textureMask);
      digestValue(result.hot, hot.renderTargetMask);
      digestHandle(result.hot, hot.indexBuffer);
      for (const auto handle : hot.streamBuffers) digestHandle(result.hot, handle);
      for (const auto handle : hot.textures) digestHandle(result.hot, handle);
    }
    for (const auto& param : command.drawParams) {
      digestDrawParam(result.params, param);
    }
    if (!command.drawParams.empty()) result.param = command.drawParams.front();
    result.payloadBytes = command.drawPayloadBytes.size();
    for (const auto& param : command.drawParams) {
      digestDrawPayloadSemantics(result.payload, param,
                                 command.drawPayloadBytes);
    }
    return result;
  }

  static std::vector<std::array<core::u32, core::kMaxStreams>>
  writingDrawStreamFrequencies(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    std::vector<std::array<core::u32, core::kMaxStreams>> result;
    const auto append = [&](const core::SourcePayloadView& view) {
      for (std::size_t index = 0u; index < view.commandCount(); ++index) {
        const auto& command = view.commandAt(index).command;
        if (command.drawState.hot) {
          for (std::size_t draw = 0u; draw < command.drawParams.size(); ++draw)
            result.push_back(command.drawState.hot->streamFrequencies);
        }
      }
    };
    std::vector<core::CpuReadyTape::ReadyEntry> ready(
        queue.cpuReadyTape_.readyCount());
    if (!ready.empty() && queue.cpuReadyTape_.copyReadyPrefix(ready) ==
                              ready.size()) {
      for (const auto& entry : ready) {
        append(queue.cpuReadyTape_.resolveSourcePayload(
            entry.source.id, entry.source.storage,
            core::CpuReadyTape::State::Ready));
      }
    }
    if (queue.writingSlot_) {
      append(core::SourcePayloadView(*queue.slots_[*queue.writingSlot_].payload));
    }
    return result;
  }

  static std::vector<core::u32> writingDrawInstanceCounts(
      CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    std::vector<core::u32> result;
    const auto append = [&](const core::SourcePayloadView& view) {
      for (std::size_t index = 0u; index < view.commandCount(); ++index) {
        const auto& command = view.commandAt(index).command;
        if (!command.drawState.hot) continue;
        for (const auto& param : command.drawParams) {
          result.push_back(param.instanceCount);
        }
      }
    };
    std::vector<core::CpuReadyTape::ReadyEntry> ready(
        queue.cpuReadyTape_.readyCount());
    if (!ready.empty() && queue.cpuReadyTape_.copyReadyPrefix(ready) ==
                              ready.size()) {
      for (const auto& entry : ready) {
        append(queue.cpuReadyTape_.resolveSourcePayload(
            entry.source.id, entry.source.storage,
            core::CpuReadyTape::State::Ready));
      }
    }
    if (queue.writingSlot_) {
      append(core::SourcePayloadView(*queue.slots_[*queue.writingSlot_].payload));
    }
    return result;
  }

  static std::vector<core::Handle> writingDrawOverrideBuffers(
      CommandQueue& queue, std::size_t commandIndex = 0u) {
    std::lock_guard lock(queue.mutex_);
    std::vector<core::Handle> result;
    if (!queue.writingSlot_) return result;
    const core::SourcePayloadView view(
        *queue.slots_[*queue.writingSlot_].payload);
    if (commandIndex >= view.commandCount()) return result;
    const auto command = view.commandAt(commandIndex).command;
    for (const auto& param : command.drawParams) {
      const auto bytes = core::drawPayloadRangeBytes(
          param.bindingOverrideRange, command.drawPayloadBytes);
      if (bytes.size() != sizeof(core::DrawBindingOverride)) {
        result.push_back({});
        continue;
      }
      core::DrawBindingOverride binding{};
      std::memcpy(&binding, bytes.data(), sizeof(binding));
      result.push_back((binding.streamMask & 1u) != 0u
                           ? binding.streams[0].buffer
                           : core::Handle{});
    }
    return result;
  }

  static std::uint64_t bufferLastUsedSeqId(CommandQueue& queue,
                                            core::Handle handle) {
    std::lock_guard lock(queue.mutex_);
    const auto* record = queue.pool_.findBuffer(handle.value);
    return record ? record->lastUsedSeqId : 0u;
  }

  static bool bufferAlive(CommandQueue& queue, core::Handle handle) {
    std::lock_guard lock(queue.mutex_);
    return queue.pool_.findBuffer(handle.value) != nullptr;
  }

  static BufferLifetimeEvidence bufferLifetime(CommandQueue& queue,
                                               core::Handle handle) {
    std::lock_guard lock(queue.mutex_);
    BufferLifetimeEvidence result{};
    if (const auto* record = queue.pool_.findBuffer(handle.value)) {
      result.alive = true;
      result.destroyPending = record->destroyPending;
      result.lastUsedSeq = record->lastUsedSeqId;
    }
    result.completedSeq = queue.completedSeqIdLocked();
    return result;
  }

  // Diagnostic value projection only. A queue mutex does not serialize the
  // Pool's in-place/rename mutation paths. Callers must independently exclude
  // later in-place mutation and hold the represented source pin until their
  // outside-lock byte proof returns; this is not general concurrent safety.
  static bool copyControlledCpuBufferProofView(CommandQueue& queue,
      core::Handle handle, std::uint64_t sourceSequence,
      const core::DrawBufferBindingSnapshot* captured,
      test::CpuTapeLiveBufferView& out, bool& destroyPending) {
    std::lock_guard lock(queue.mutex_);
    const auto* record = queue.pool_.findBuffer(handle.value);
    if (!record || record->lastUsedSeqId < sourceSequence ||
        queue.completedSeqIdLocked() >= sourceSequence) return false;
    const bool validBacking = captured ? captured->valid() : bool(record->buffer) && record->contents;
    const auto route = test::cpuTapeBufferObservationRoute(
        record->hasVersionedBacking(), captured != nullptr, validBacking);
    if (captured) {
      if (route != test::CpuTapeBufferObservationRoute::Captured ||
          queue.pool_.validateCapturedBufferSnapshot(handle, *captured) !=
              resources::CapturedBufferSnapshotStatus::Valid) return false;
      out = {captured->metalHandle, captured->contentsAddress,
          captured->byteSize, captured->contentRevision};
    } else {
      if (route != test::CpuTapeBufferObservationRoute::Live) return false;
      out = {record->buffer.handle,
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(record->contents)),
          record->desc.size, record->contentRevision};
    }
    destroyPending = record->destroyPending;
    return true;
  }

  static void markBufferUseForTest(CommandQueue& queue, core::Handle handle,
                                   std::uint64_t seqId) {
    std::lock_guard lock(queue.mutex_);
    queue.pool_.markBufferUse(handle, seqId);
  }

  static bool destroyBufferForTest(CommandQueue& queue,
                                   core::Handle handle,
                                   std::uint64_t completedSeqId) {
    std::lock_guard lock(queue.mutex_);
    return queue.pool_.markBufferDestroyAndGc(handle.value, completedSeqId);
  }

  static void reclaimPoolForTest(CommandQueue& queue,
                                 std::uint64_t completedSeqId) {
    std::lock_guard lock(queue.mutex_);
    queue.pool_.reclaimCompleted(completedSeqId);
  }

  static void forceNextBatchBuilderFailure(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyForceNextCpuReadyArenaBuilderFailure_ = true;
  }

  static void forceNextCaptureIdentityBeginFailure(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyForceNextCpuReadyArenaCaptureIdentityBeginFailure_ = true;
  }

  static void forceNextPostSemanticPublishFailure(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyForceNextCpuReadyArenaPostSemanticPublishFailure_ = true;
  }

  static void forceNextCapacityFailure(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyForceNextCpuReadyArenaCapacityFailure_ = true;
  }

  static void forceNextValidationFailure(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyForceNextCpuReadyArenaValidationFailure_ = true;
  }

  static void forceNextResourceRetainFailure(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyForceNextCpuReadyArenaResourceRetainFailure_ = true;
  }

  static void forceNextPublicationFailure(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyForceNextCpuReadyArenaPublicationFailure_ = true;
  }

  static void forceNextDirectChunkSlotCommitFailure(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyForceNextDirectChunkSlotCommitFailure_ = true;
  }

  static void forceNextCompletionFailure(CommandQueue& queue) {
    queue.queueLifecycle_.forceNextCompletionFailureForTest();
  }

  static std::size_t pendingCompletionCount(CommandQueue& queue) {
    return queue.queueLifecycle_.pendingCompletionCountForTest();
  }

  static CommandQueue::CpuReadyArenaFailureSnapshot takeFailure(
      CommandQueue& queue) {
    return queue.takeCpuReadyArenaFailure();
  }

  static CommandQueue::CpuReadyArenaFailureSnapshot activeFailure(
      CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.arenaBuildContext_
        ? queue.arenaBuildContext_->firstFailure()
        : CommandQueue::CpuReadyArenaFailureSnapshot{};
  }

  static std::uint32_t admissionWaiterCount(CommandQueue& queue) {
    return queue.arenaAdmissionWaiterCount_.load(std::memory_order_acquire);
  }

  static CompletionResult consumeOne(CommandQueue& queue,
                                     bool deferFinish = false) {
    using namespace core::metalqueue;
    CompletionResult result{};
    ReadySlotSnapshot source{};
    QueueCompletionSource completion{};
    {
      std::unique_lock lock(queue.mutex_);
      result.dequeued = queue.queueLifecycle_.dequeueReadySlot(lock, source);
      if (!result.dequeued) {
        return result;
      }
      result.seqId = source.seqId;
      const auto resolved =
          queue.queueLifecycle_.resolveRepresentedSource(source);
      result.arena = resolved.valid() && resolved.payload.isArena() &&
                     resolved.slot == nullptr;
      result.commandCount = resolved.payload.commandCount();
      result.arenaSegmentCount = resolved.payload.arenaSegmentCount();
      result.rawOrdinal = resolved.metadata.rawOrdinal;
      result.sourceOrdinal = resolved.metadata.sourceOrdinal;
      result.effectiveDigest = effectivePayloadDigest(resolved.payload);
      result.commandDigests.reserve(result.commandCount);
      result.commandKinds.reserve(result.commandCount);
      for (std::size_t index = 0; index < result.commandCount; ++index) {
        result.commandDigests.push_back(
            effectiveCommandDigest(resolved.payload, index));
        result.commandKinds.push_back(static_cast<std::uint8_t>(
            resolved.payload.commandAt(index).kind()));
      }
      result.clear = result.arena && resolved.payload.commandCount() == 1u &&
                     resolved.payload.commandAt(0).kind() ==
                         core::MetalCommandKind::Clear;
      result.finalPresent = result.arena && result.commandCount != 0 &&
          resolved.payload.commandAt(result.commandCount - 1u).kind() ==
              core::MetalCommandKind::Present;
      result.hasPresent = result.commandCount != 0 &&
          resolved.payload.commandAt(result.commandCount - 1u).kind() ==
              core::MetalCommandKind::Present;
      if (result.finalPresent) {
        const auto present =
            resolved.payload.commandAt(result.commandCount - 1u).command.present;
        if (present) {
          result.presentSource = present->presentSource;
        }
      }

      QueueSubmissionRecord record{};
      record.testOnlyAllowNullCommandBuffer = true;
      record.slotIndex = source.slotIndex;
      record.seqId = source.seqId;
      completion = completionSourceForReadySlot(source);
      result.completion = completion;
      const std::array sources{completion};
      if (!record.assignFixedCompletionSources(sources)) {
        return result;
      }
      result.submitted =
          queue.queueLifecycle_.submitEncodedSubmission(lock, record);
    }
    if (!result.submitted) {
      return result;
    }

    core::metalqueue::QueueLifecycleController::PendingCompletion pending{};
    pending.slotIndex = completion.slotIndex;
    pending.seqId = completion.seqId;
    const std::array pendingSources{completion};
    if (!pending.assignFixedCompletionSources(pendingSources)) {
      return result;
    }
    queue.queueLifecycle_.enqueuePendingCompletionForTest(std::move(pending));
    result.completed = queue.queueLifecycle_.processOnePendingCompletion();
    if (!deferFinish) {
      std::unique_lock lock(queue.mutex_);
      result.reclaimed = result.completed &&
          queue.queueLifecycle_.runFinishIteration(lock);
    }
    return result;
  }

  // Submit one already-published source through the production queue path but
  // leave its completion acknowledgement pending.  The caller controls when
  // processOnePendingCompletion() and runFinishIteration() occur; this is the
  // concrete binding used by the bounded completion schedule tests below.
  static CompletionResult submitOneWithheld(
      CommandQueue& queue,
      const std::function<void(const core::SourcePayloadView&)>& observe = {}) {
    using namespace core::metalqueue;
    CompletionResult result{};
    ReadySlotSnapshot source{};
    QueueCompletionSource completion{};
    {
      std::unique_lock lock(queue.mutex_);
      result.dequeued = queue.queueLifecycle_.dequeueReadySlot(lock, source);
      if (!result.dequeued) return result;
      result.seqId = source.seqId;
      const auto resolved =
          queue.queueLifecycle_.resolveRepresentedSource(source);
      if (observe) observe(resolved.payload);
      result.arena = resolved.valid() && resolved.payload.isArena() &&
                     resolved.slot == nullptr;
      result.commandCount = resolved.payload.commandCount();
      result.arenaSegmentCount = resolved.payload.arenaSegmentCount();
      result.rawOrdinal = resolved.metadata.rawOrdinal;
      result.sourceOrdinal = resolved.metadata.sourceOrdinal;
      result.effectiveDigest = effectivePayloadDigest(resolved.payload);
      result.commandDigests.reserve(result.commandCount);
      result.commandKinds.reserve(result.commandCount);
      for (std::size_t index = 0; index < result.commandCount; ++index) {
        result.commandDigests.push_back(
            effectiveCommandDigest(resolved.payload, index));
        result.commandKinds.push_back(static_cast<std::uint8_t>(
            resolved.payload.commandAt(index).kind()));
      }
      completion = completionSourceForReadySlot(source);
      result.completion = completion;

      QueueSubmissionRecord record{};
      record.testOnlyAllowNullCommandBuffer = true;
      record.slotIndex = source.slotIndex;
      record.seqId = source.seqId;
      const std::array sources{completion};
      if (!record.assignFixedCompletionSources(sources)) return result;
      result.submitted =
          queue.queueLifecycle_.submitEncodedSubmission(lock, record);
    }
    if (!result.submitted) return result;

    QueueLifecycleController::PendingCompletion pending{};
    pending.slotIndex = completion.slotIndex;
    pending.seqId = completion.seqId;
    const std::array pendingSources{completion};
    if (!pending.assignFixedCompletionSources(pendingSources)) return result;
    queue.queueLifecycle_.enqueuePendingCompletionForTest(std::move(pending));
    return result;
  }

  static bool acknowledgeOne(CommandQueue& queue) {
    return queue.queueLifecycle_.processOnePendingCompletion();
  }

  static bool finishQueue(CommandQueue& queue) {
    std::unique_lock lock(queue.mutex_);
    return queue.queueLifecycle_.runFinishIteration(lock);
  }

  static bool finishIteration(CommandQueue& queue) {
    std::unique_lock lock(queue.mutex_);
    return queue.queueLifecycle_.runFinishIteration(lock);
  }
};

}  // namespace dxmt9

namespace {

using namespace dxmt9::core;
using dxmt9::AttachmentContentOwner;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

SourcePayloadCapacity oneDrawContinuationCapacity(
    std::size_t drawCount = 1u) {
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = drawCount;
  capacity.drawHotStates = drawCount;
  capacity.drawShaderLayouts = drawCount;
  capacity.drawDebugSnapshots = drawCount;
  capacity.drawPsoSubviews = drawCount;
  capacity.drawUniformFixedPayloads = drawCount;
  capacity.drawUniformVertexConstants = drawCount;
  capacity.drawUniformVertexConstantBytes =
      drawCount * sizeof(VertexShaderConstants);
  capacity.drawUniformPixelConstants = drawCount;
  capacity.drawUniformPixelConstantBytes =
      drawCount * sizeof(PixelShaderConstants);
  capacity.drawUniformPayloads = drawCount;
  const auto buckets = detail::chunkSlotUniformLookupBucketCount(drawCount);
  capacity.drawUniformPayloadLookupHeads = buckets;
  capacity.drawUniformPayloadLookupTails = buckets;
  capacity.drawUniformPayloadLookupNext = drawCount;
  capacity.drawUniformVertexConstantsLookupHeads = buckets;
  capacity.drawUniformVertexConstantsLookupTails = buckets;
  capacity.drawUniformVertexConstantsLookupNext = drawCount;
  capacity.drawUniformPixelConstantsLookupHeads = buckets;
  capacity.drawUniformPixelConstantsLookupTails = buckets;
  capacity.drawUniformPixelConstantsLookupNext = drawCount;
  capacity.drawParams = drawCount;
  capacity.drawRunRecords = drawCount;
  return capacity;
}

ChunkSlot validDirectContinuationSlot() {
  ChunkSlot slot;
  constexpr std::size_t kCapacity = 2u;
  slot.commandHeaders.reserve(kCapacity);
  slot.drawHotStates.reserve(kCapacity);
  slot.drawShaderLayouts.reserve(kCapacity);
  slot.drawDebugSnapshots.reserve(kCapacity);
  slot.drawPsoSubviews.reserve(kCapacity);
  slot.drawUniformFixedPayloads.reserve(kCapacity);
  slot.drawUniformVertexConstants.reserve(kCapacity);
  slot.drawUniformVertexConstantBytes.reserve(
      kCapacity * sizeof(VertexShaderConstants));
  slot.drawUniformPixelConstants.reserve(kCapacity);
  slot.drawUniformPixelConstantBytes.reserve(
      kCapacity * sizeof(PixelShaderConstants));
  slot.drawUniformPayloads.reserve(kCapacity);
  slot.drawParams.reserve(kCapacity);
  slot.drawRunRecords.reserve(kCapacity);
  slot.commandHeaders.push_back({
      MetalCommandKind::DrawRun,
      CommandPayloadIndex::fromU32(0u),
  });
  slot.drawHotStates.resize(1u);
  slot.drawShaderLayouts.resize(1u);
  slot.drawDebugSnapshots.resize(1u);
  slot.drawPsoSubviews.resize(1u);
  slot.drawUniformFixedPayloads.resize(1u);
  slot.drawUniformVertexConstants.resize(1u);
  slot.drawUniformPixelConstants.resize(1u);
  slot.drawUniformPayloads.resize(1u);
  slot.drawParams.resize(1u);
  slot.drawRunRecords.push_back({
      .stateIndex = 0u,
      .firstParam = 0u,
      .paramCount = 1u,
  });
  const auto buckets = detail::chunkSlotUniformLookupBucketCount(1u);
  slot.drawUniformPayloadLookupHeads.assign(
      buckets, detail::kChunkSlotInvalidUniformIndex);
  slot.drawUniformPayloadLookupTails.assign(
      buckets, detail::kChunkSlotInvalidUniformIndex);
  slot.drawUniformPayloadLookupNext.reserve(2u);
  slot.drawUniformPayloadLookupNext.assign(
      1u, detail::kChunkSlotInvalidUniformIndex);
  slot.drawUniformVertexConstantsLookupHeads.assign(
      buckets, detail::kChunkSlotInvalidUniformIndex);
  slot.drawUniformVertexConstantsLookupTails.assign(
      buckets, detail::kChunkSlotInvalidUniformIndex);
  slot.drawUniformVertexConstantsLookupNext.reserve(2u);
  slot.drawUniformVertexConstantsLookupNext.assign(
      1u, detail::kChunkSlotInvalidUniformIndex);
  slot.drawUniformPixelConstantsLookupHeads.assign(
      buckets, detail::kChunkSlotInvalidUniformIndex);
  slot.drawUniformPixelConstantsLookupTails.assign(
      buckets, detail::kChunkSlotInvalidUniformIndex);
  slot.drawUniformPixelConstantsLookupNext.reserve(2u);
  slot.drawUniformPixelConstantsLookupNext.assign(
      1u, detail::kChunkSlotInvalidUniformIndex);
  slot.drawPayloadArena.reserve(1024u);
  return slot;
}

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
  std::vector<std::byte> bytes(sizeof(T));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

struct RecordSpec {
  std::uint32_t type = 0;
  std::vector<std::byte> payload;
  std::vector<D9CCommandChunkWireHandleEntry> handles;
};

struct WireFixture {
  std::vector<std::byte> bytes;
  dxmt9::d3d9::CommandChunkEnvelope envelope{};
};

WireFixture makeWireFixture(std::span<const RecordSpec> specs) {
  std::vector<D9CCommandChunkWireRecordHeader> records;
  std::vector<D9CCommandChunkWireHandleEntry> handles;
  std::vector<std::byte> payload;
  for (const auto& spec : specs) {
    const auto* rule = dxmt9::d3d9::recordRule(spec.type);
    check(rule != nullptr, "production routing fixture record must be known");
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    records.push_back({
        .type = spec.type,
        .flags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(spec.payload.size()),
        .firstHandle = static_cast<std::uint32_t>(handles.size()),
        .handleCount = static_cast<std::uint32_t>(spec.handles.size()),
    });
    handles.insert(handles.end(), spec.handles.begin(), spec.handles.end());
    payload.insert(payload.end(), spec.payload.begin(), spec.payload.end());
  }

  D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  header.handleTableOffset = static_cast<std::uint32_t>(alignUp(
      header.recordTableOffset + records.size() * sizeof(records[0]),
      alignof(D9CCommandChunkWireHandleEntry)));
  header.payloadArenaOffset = static_cast<std::uint32_t>(alignUp(
      header.handleTableOffset + handles.size() * sizeof(handles[0]),
      alignof(std::uint32_t)));

  WireFixture fixture;
  fixture.bytes.resize(header.payloadArenaOffset + payload.size());
  std::memcpy(fixture.bytes.data(), &header, sizeof(header));
  std::memcpy(fixture.bytes.data() + header.recordTableOffset,
              records.data(), records.size() * sizeof(records[0]));
  if (!handles.empty()) {
    std::memcpy(fixture.bytes.data() + header.handleTableOffset,
                handles.data(), handles.size() * sizeof(handles[0]));
  }
  if (!payload.empty()) {
    std::memcpy(fixture.bytes.data() + header.payloadArenaOffset,
                payload.data(), payload.size());
  }
  fixture.envelope = {
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = header.recordCount,
      .handleCount = header.handleCount,
  };
  return fixture;
}

void retainOracleObject(std::uint32_t kind, void* object) noexcept {
  if (!object) return;
  if (kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
    dxmt9c_buffer_addref(static_cast<D9CBuffer*>(object));
  } else if (kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
    dxmt9c_surface_addref(static_cast<D9CSurface*>(object));
  }
}

RecordSpec clearRecord(std::uint32_t rectCount = 0) {
  const D9CCommandChunkWireClear clear{
      .flags = 1u,
      .colorARGB = 0xff123456u,
      .z = 1.0f,
      .stencil = 0,
      .rectCount = rectCount,
      .rectOffset = sizeof(D9CCommandChunkWireClear),
  };
  RecordSpec record{
      .type = D9C_COMMAND_RECORD_CLEAR,
  };
  record.payload.resize(sizeof(clear) +
                        static_cast<std::size_t>(rectCount) *
                            sizeof(D9CRect));
  std::memcpy(record.payload.data(), &clear, sizeof(clear));
  return record;
}

std::size_t clearRectCountForPlannerPages(std::size_t targetPages) {
  for (std::size_t rectCount = 0;
       rectCount <= targetPages * 4096u / sizeof(D9CRect) + 1024u;
       ++rectCount) {
    dxmt9::core::SourcePayloadCapacity capacity{};
    capacity.commandHeaders = 1;
    capacity.clearRecords = 1;
    capacity.clearRects = rectCount;
    capacity.drawUniformPayloadLookupHeads = 8;
    capacity.drawUniformPayloadLookupTails = 8;
    capacity.drawUniformVertexConstantsLookupHeads = 8;
    capacity.drawUniformVertexConstantsLookupTails = 8;
    capacity.drawUniformPixelConstantsLookupHeads = 8;
    capacity.drawUniformPixelConstantsLookupTails = 8;
    const auto layout = dxmt9::core::makeSourcePayloadLayout(
        capacity, 4096u, std::numeric_limits<std::uint32_t>::max());
    check(layout.has_value(), "planner clear boundary layout must build");
    if (layout->pageCount == targetPages) {
      return rectCount;
    }
  }
  throw TestFailure("planner clear page boundary must be reachable");
}

RecordSpec presentRecord() {
  return {
      .type = D9C_COMMAND_RECORD_PRESENT,
      .payload = bytesOf(D9CCommandChunkWirePresent{}),
  };
}

RecordSpec applyRenderStateRecord(std::uint32_t state,
                                  std::uint32_t value) {
  D9CCommandChunkWireDrawHeader draw{
      .sectionCount = 1,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
  };
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
      sizeof(draw) + sizeof(D9CCommandChunkWireSectionDesc),
      alignof(D9CCommandChunkWireRenderState)));
  const D9CCommandChunkWireSectionDesc section{
      .kind = D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
      .elementSize = sizeof(D9CCommandChunkWireRenderState),
      .count = 1,
      .payloadOffset = draw.sectionPayloadOffset,
      .byteSize = sizeof(D9CCommandChunkWireRenderState),
  };
  const D9CCommandChunkWireRenderState renderState{
      .state = state,
      .value = value,
  };
  std::vector<std::byte> payload(
      draw.sectionPayloadOffset + sizeof(renderState));
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + draw.sectionTableOffset,
              &section, sizeof(section));
  std::memcpy(payload.data() + draw.sectionPayloadOffset,
              &renderState, sizeof(renderState));
  return {
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = std::move(payload),
  };
}

RecordSpec applyRenderStateThenInvalidTransformRecord(
    std::uint32_t state, std::uint32_t value) {
  D9CCommandChunkWireDrawHeader draw{
      .sectionCount = 2,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
  };
  const auto tableEnd = sizeof(draw) +
      2u * sizeof(D9CCommandChunkWireSectionDesc);
  const auto renderOffset = alignUp(
      tableEnd, alignof(D9CCommandChunkWireRenderState));
  const auto transformOffset = alignUp(
      renderOffset + sizeof(D9CCommandChunkWireRenderState),
      alignof(D9CDrawPacketTransform));
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(renderOffset);
  const std::array sections{
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_RENDER_STATE,
          .elementSize = sizeof(D9CCommandChunkWireRenderState),
          .count = 1,
          .payloadOffset = static_cast<std::uint32_t>(renderOffset),
          .byteSize = sizeof(D9CCommandChunkWireRenderState),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_TRANSFORM,
          .elementSize = sizeof(D9CDrawPacketTransform),
          .count = 1,
          .payloadOffset = static_cast<std::uint32_t>(transformOffset),
          .byteSize = sizeof(D9CDrawPacketTransform),
      },
  };
  const D9CCommandChunkWireRenderState renderState{
      .state = state,
      .value = value,
  };
  const D9CDrawPacketTransform transform{
      .state = dxmt9::core::kMaxTransformSlots,
  };
  std::vector<std::byte> payload(transformOffset + sizeof(transform));
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + draw.sectionTableOffset,
              sections.data(), sizeof(sections));
  std::memcpy(payload.data() + renderOffset,
              &renderState, sizeof(renderState));
  std::memcpy(payload.data() + transformOffset,
              &transform, sizeof(transform));
  return {
      .type = D9C_COMMAND_RECORD_APPLY_STATE,
      .payload = std::move(payload),
  };
}

RecordSpec floatConstantRecord(std::uint32_t type,
                               std::uint32_t startRegister) {
  const D9CCommandChunkWireSetConst fixed{
      .startRegister = startRegister,
      .registerCount = 1u,
  };
  auto payload = bytesOf(fixed);
  payload.resize(payload.size() + 4u * sizeof(float));
  return {.type = type, .payload = std::move(payload)};
}

RecordSpec streamDrawRecord(
    std::optional<D9CWireObjectIdentity> bufferIdentity,
    std::uint32_t streamHandleIndex, std::uint32_t frequency,
    std::uint32_t streamSlot = 0u) {
  D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4u,
      .primitiveCount = 1u,
      .sectionCount = 1u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
  };
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
      sizeof(draw) + sizeof(D9CCommandChunkWireSectionDesc),
      alignof(D9CCommandChunkWireStreamBinding)));
  const D9CCommandChunkWireSectionDesc section{
      .kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
      .elementSize = sizeof(D9CCommandChunkWireStreamBinding),
      .count = 1u,
      .payloadOffset = draw.sectionPayloadOffset,
      .byteSize = sizeof(D9CCommandChunkWireStreamBinding),
  };
  const D9CCommandChunkWireStreamBinding stream{
      .slot = streamSlot,
      .valid = 1u,
      .handleIndex = streamHandleIndex,
      .offset = 0u,
      .stride = 16u,
      .frequency = frequency,
  };
  std::vector<std::byte> payload(
      draw.sectionPayloadOffset + sizeof(stream));
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + draw.sectionTableOffset,
              &section, sizeof(section));
  std::memcpy(payload.data() + draw.sectionPayloadOffset,
              &stream, sizeof(stream));
  RecordSpec record{
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .payload = std::move(payload),
  };
  if (bufferIdentity) {
    record.handles.push_back(dxmt9::d3d9::wireHandleEntry(*bufferIdentity));
  }
  return record;
}

RecordSpec drawRecord(const D9CWireObjectIdentity& bufferIdentity,
                      std::uint32_t streamHandleIndex = 0u,
                      std::uint32_t frequency = 1u) {
  return streamDrawRecord(bufferIdentity, streamHandleIndex, frequency);
}

RecordSpec nullStreamDrawRecord(std::uint32_t frequency) {
  return streamDrawRecord(std::nullopt,
                          D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX, frequency);
}

RecordSpec stretchRectRecord(const D9CWireObjectIdentity& source,
                             const D9CWireObjectIdentity& destination,
                             std::uint32_t firstHandle = 0u) {
  const D9CCommandChunkWireStretchRect fixed{
      .srcHandleIndex = firstHandle,
      .dstHandleIndex = firstHandle + 1u,
      .hasSrcRect = 1u,
      .hasDstRect = 1u,
      .srcRect = {.left = 0, .top = 0, .right = 8, .bottom = 8},
      .dstRect = {.left = 0, .top = 0, .right = 8, .bottom = 8},
  };
  return {
      .type = D9C_COMMAND_RECORD_STRETCH_RECT,
      .payload = bytesOf(fixed),
      .handles = {dxmt9::d3d9::wireHandleEntry(source),
                  dxmt9::d3d9::wireHandleEntry(destination)},
  };
}

RecordSpec readbackRecord(const D9CWireObjectIdentity& source,
                          const D9CWireObjectIdentity& destination,
                          std::uint32_t firstHandle = 0u) {
  const D9CCommandChunkWireReadback fixed{
      .srcHandleIndex = firstHandle,
      .dstHandleIndex = firstHandle + 1u,
  };
  return {
      .type = D9C_COMMAND_RECORD_READBACK,
      .payload = bytesOf(fixed),
      .handles = {dxmt9::d3d9::wireHandleEntry(source),
                  dxmt9::d3d9::wireHandleEntry(destination)},
  };
}

dxmt9::d3d9::RawCommandChunk makeRaw(const WireFixture& fixture,
                                      std::uint64_t rawOrdinal,
                                      bool captureIdentity = false,
                                      dxmt9::d3d9::WireObjectRegistry* registryOverride = nullptr) {
  dxmt9::d3d9::WireObjectRegistry localRegistry;
  auto& registry = registryOverride ? *registryOverride : localRegistry;
  dxmt9::d3d9::RawCommandChunk raw;
  const bool prepared = dxmt9::d3d9::prepareOffloadChunk(
      fixture.bytes, fixture.envelope, registry, retainOracleObject, raw);
  check(prepared, "production raw chunk must pass owned preflight");
  raw.replaySeq = rawOrdinal;
  raw.cpuReadyTapePlanningEnabled = true;
  if (captureIdentity) {
    raw.renderTapeCaptureToken = 0xfeedu;
    raw.renderTapeEventOrdinal = rawOrdinal;
  }
  return raw;
}

struct RoutingDevice final : dxmt9::Device {
  AttachmentContentOwner attachmentOwner = AttachmentContentOwner::CpuShadow;
  bool readbackAvailableForTest = false;
  AttachmentContentOwner attachmentContentOwner() const noexcept override {
    return attachmentOwner;
  }
  bool supportsGpuReadback() const override { return readbackAvailableForTest; }
  // Only admitted query tapes opt into this backend. The query-free fixture
  // retains its existing policy. Result storage is substituted CPU evidence;
  // fences and scope lifetime still belong to the production queue.
  bool controlledQueries = false;
  bool usesAsyncQueryFences() const noexcept override { return controlledQueries; }
  bool usesMetalOcclusion() const noexcept override { return controlledQueries; }
  bool setOcclusionScope(dxmt9::query::OcclusionScopeRef scope) noexcept override {
    return controlledQueries && queue_.setOcclusionScope(std::move(scope));
  }
  dxmt9::query::EndReceipt publishQueryEnd() noexcept override {
    return controlledQueries ? queue_.publishQueryEnd() : dxmt9::query::EndReceipt{};
  }
  dxmt9::query::Poll pollQueryFence(const dxmt9::query::EndReceipt& receipt,
                                   bool flush) noexcept override {
    return controlledQueries ? queue_.pollQueryFence(receipt, flush)
                             : dxmt9::query::Poll::Invalid;
  }
  dxmt9::query::Progress queryProgress() noexcept override {
    return controlledQueries ? queue_.queryProgress() : dxmt9::query::Progress{};
  }
  static constexpr std::size_t kTextureUploadObservationCapacity = 16u;
  static constexpr std::size_t kTextureUploadObservationBytes = 64u * 1024u;

  struct TextureUploadObservation {
    TextureHandle handle{};
    std::uint32_t level = 0u;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::uint32_t depth = 0u;
    std::uint32_t pitch = 0u;
    std::uint32_t slicePitch = 0u;
    std::uint64_t contentDigest = 0u;
    std::vector<std::uint8_t> bytes{};
  };

  struct CapturedBindingObservation {
    std::uint64_t metalHandle = 0u;
    std::uint64_t contentsAddress = 0u;
    std::uint64_t contentRevision = 0u;
    std::vector<std::uint8_t> bytes;
  };

  explicit RoutingDevice(bool rejectAfterClear = false,
                         bool segmentSerial = false,
                         bool directChunkSlot = false,
                         bool cpuReadyArena = true,
                         WMT::Device resourceDevice = {},
                         bool exactRenderTapeActionProfile = false)
      : limits_([&] {
          BackendLimits limits{};
          if (resourceDevice)
            limits.supportsDepth24Stencil8 = resourceDevice.supportsDepth24Stencil8();
          return limits;
        }()),
        queue_(
            dxmt9::CommandQueue::ArenaLeaseTestQueueTag{}, limits_, {},
            segmentSerial
                ? dxmt9::render::RenderPartitionConfig{
                      .sourceIdentity =
                          dxmt9::render::SourceIdentityConfig{
                              .requested = dxmt9::render::
                                  SourceIdentityModeRequest::Segment,
                              .resolved = dxmt9::render::
                                  SourceIdentityMode::SegmentSerial}}
                : dxmt9::render::RenderPartitionConfig{},
            exactRenderTapeActionProfile),
        rejectAfterClear_(rejectAfterClear),
        directChunkSlot_(directChunkSlot),
        cpuReadyArena_(cpuReadyArena),
        controlledResourcePool_(exactRenderTapeActionProfile && static_cast<bool>(resourceDevice)),
        resourceDevice_(resourceDevice) {
    if (static_cast<bool>(resourceDevice_)) {
      dxmt9::CommandQueueArenaLeaseTestAccess::bindDevice(queue_,
                                                          resourceDevice_);
    }
  }

  WMT::Device wmtDevice() override { return resourceDevice_; }
  dxmt9::CommandQueue& queue() override { return queue_; }
  dxmt9::resources::Pool* pool() override {
    // Only the genuine resource-backed Exact fixture exposes its existing
    // queue pool to shared production preparation. This grants no readback
    // or GPU execution support and leaves ordinary legacy fixtures inert.
    return controlledResourcePool_ ? &queue_.pool() : nullptr;
  }
  const BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<BackendDevice> backend() override { return {}; }
  void flush() override {
    ++flushCalls;
    if (flushHook) flushHook();
  }
  bool supportsCpuReadyArenaReplay() const noexcept override {
    return cpuReadyArena_;
  }
  bool supportsDirectChunkSlotReplay() const noexcept override {
    return directChunkSlot_;
  }
  bool dynamicBufferRenameEnabled() const noexcept override { return true; }

  void* mapBuffer(BufferHandle handle, std::uint32_t flags) override {
    return queue_.mapBuffer(handle, flags);
  }

  void unmapBuffer(BufferHandle) override {}

  void uploadBufferData(BufferHandle handle,
                        std::span<const std::uint8_t> bytes) override {
    dxmt9::CommandQueueArenaLeaseTestAccess::uploadBufferData(
        queue_, resourceDevice_, handle, bytes);
  }

  void uploadBufferDataRange(BufferHandle handle,
                             std::span<const std::uint8_t> fullBytes,
                             std::uint64_t offset,
                             std::uint64_t byteCount) override {
    dxmt9::CommandQueueArenaLeaseTestAccess::uploadBufferDataRange(
        queue_, resourceDevice_, handle, fullBytes, offset,
        byteCount);
  }

  void uploadTextureLevel(TextureHandle handle, std::uint32_t level,
                          std::uint32_t width, std::uint32_t height,
                          std::uint32_t depth, std::uint32_t pitch,
                          std::uint32_t slicePitch,
                          std::span<const std::uint8_t> bytes) override {
    // A null resource device retains the old no-op fixture behavior. The
    // resource-backed path records the logical core upload only; it does not
    // touch the Metal texture or submit an initializer/blit command.
    if (!resourceDevice_) return;
    std::lock_guard lock(queue_.mutex_);
    const auto* record = queue_.pool().findTexture(handle.value);
    std::uint64_t faceCount = 1u;
    if (record) {
      switch (record->desc.type) {
      case TextureType::TwoD:
      case TextureType::Volume:
        break;
      case TextureType::Cube:
        faceCount = 6u;
        break;
      default:
        faceCount = 0u;
        break;
      }
    }
    const auto mipLevels = record ? std::max(1u, record->desc.levels) : 0u;
    const auto subresourceCount = record
        ? static_cast<std::uint64_t>(mipLevels) * faceCount
        : 0u;
    const auto mipLevel = record && record->desc.type == TextureType::Cube
        ? level % std::max(1u, mipLevels)
        : level;
    const bool descriptorValid = record && record->texture && faceCount != 0u &&
        mipLevels != 0u && record->desc.width != 0u &&
        record->desc.height != 0u &&
        (record->desc.type != TextureType::Volume || record->desc.depth != 0u) &&
        mipLevel < mipLevels && mipLevel < 32u;
    const auto expectedWidth = descriptorValid
        ? std::max(1u, record->desc.width >> mipLevel)
        : 0u;
    const auto expectedHeight = descriptorValid
        ? std::max(1u, record->desc.height >> mipLevel)
        : 0u;
    const auto expectedDepth = descriptorValid &&
            record->desc.type == TextureType::Volume
        ? std::max(1u, record->desc.depth >> mipLevel)
        : descriptorValid ? 1u : 0u;
    const auto expectedPitch = descriptorValid
        ? dxmt9::core::formatRowPitch(record->desc.format, expectedWidth)
        : 0u;
    const auto expectedSlicePitch = descriptorValid
        ? dxmt9::core::formatByteSize(record->desc.format, expectedWidth,
                                       expectedHeight)
        : 0u;
    const bool sizeOverflow =
        descriptorValid && expectedSlicePitch != 0u && expectedDepth != 0u &&
        expectedSlicePitch >
            std::numeric_limits<std::size_t>::max() / expectedDepth;
    const auto expectedBytes = sizeOverflow
        ? std::numeric_limits<std::size_t>::max()
        : expectedSlicePitch * expectedDepth;
    if (!descriptorValid || level >= subresourceCount || expectedPitch == 0u ||
        expectedSlicePitch == 0u || sizeOverflow || width != expectedWidth ||
        height != expectedHeight || depth != expectedDepth ||
        pitch != expectedPitch || slicePitch != expectedSlicePitch ||
        bytes.empty() || bytes.size() != expectedBytes) {
      textureUploadFailed = true;
      return;
    }
    try {
      // General tape execution consumes a qualified logical upload receipt.
      // Validation above remains identical to the bounded fixture. The cold
      // callback runs under the queue lock and must never re-enter the queue.
      if (generalTextureUpload) {
        if (!generalTextureUpload(handle, level, width, height, depth, pitch,
                                  slicePitch, bytes)) {
          textureUploadFailed = true;
        }
        return;
      }
      if (bytes.size() > kTextureUploadObservationBytes ||
          textureUploadObservations.size() >= kTextureUploadObservationCapacity) {
        textureUploadFailed = true;
        return;
      }
      TextureUploadObservation observation{
          .handle = handle,
          .level = level,
          .width = width,
          .height = height,
          .depth = depth,
          .pitch = pitch,
          .slicePitch = slicePitch,
          .contentDigest = digestByteSpan(bytes),
          .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
      };
      textureUploadObservations.push_back(std::move(observation));
    } catch (...) {
      textureUploadFailed = true;
    }
  }

  BufferHandle createBuffer(const BufferDesc& desc) override {
    return queue_.pool().createBuffer(resourceDevice_, desc);
  }

  TextureHandle createTexture(const TextureDesc& desc) override {
    if (!resourceDevice_) return {};
    // Pool creation allocates the backing record/Metal resource only; no
    // command buffer or initializer submission is needed here.
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().createTexture(resourceDevice_, limits_, desc);
  }

  SurfaceHandle createSurface(const SurfaceDesc& desc) override {
    if (!resourceDevice_) return SurfaceHandle{nextHandle_++};
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().createSurface(resourceDevice_, limits_, desc);
  }

  SurfaceHandle createSurfaceForTexture(TextureHandle texture, std::uint32_t level,
                                        const SurfaceDesc& desc) override {
    if (!resourceDevice_) return {};
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().createSurfaceForTexture(texture, level, desc);
  }

  bool bufferHasVersionedBacking(BufferHandle handle) override {
    return queue_.pool().bufferHasVersionedBacking(handle.value);
  }

  dxmt9::resources::ManagedBufferMutationLease rotateManagedBufferForMutation(
      BufferHandle handle) override {
    std::lock_guard lock(queue_.mutex_);
    return queue_.pool().rotateManagedBufferForMutation(
        resourceDevice_, handle.value, queue_.completedSeqIdLocked());
  }

  dxmt9::resources::ManagedBufferMutationApplyResult applyManagedBufferMutation(
      BufferHandle handle,
      const dxmt9::resources::ManagedBufferMutationLease& lease,
      std::uint64_t offset,
      std::span<const std::uint8_t> bytes) override {
    ++mutationApplies;
    return queue_.pool().applyManagedBufferMutation(
        handle.value, lease, offset, bytes.data(), bytes.size());
  }

  void destroyBuffer(BufferHandle handle) override {
    // Same retire predicate as DeviceImpl; the base test Device callback is
    // intentionally a no-op and cannot witness ObjectDestroy lifetime.
    const auto completed = dxmt9::CommandQueueArenaLeaseTestAccess::bufferLifetime(
        queue_, handle).completedSeq;
    dxmt9::CommandQueueArenaLeaseTestAccess::destroyBufferForTest(queue_, handle, completed);
  }

  void destroyTexture(TextureHandle handle) override {
    if (!resourceDevice_) return;
    std::lock_guard lock(queue_.mutex_);
    queue_.pool().markTextureDestroyAndGc(handle.value,
                                          queue_.completedSeqIdLocked());
  }

  void destroySurface(SurfaceHandle handle) override {
    if (!resourceDevice_) return;
    std::lock_guard lock(queue_.mutex_);
    queue_.pool().markSurfaceDestroyAndGc(handle.value,
                                          queue_.completedSeqIdLocked());
  }

  ChunkBufferBindingCaptureResult captureChunkBufferBindings(
      std::span<const ChunkHandleEntry> entries,
      std::vector<ChunkBufferBindingSnapshot>& snapshots) override {
    ++captureCalls;
    captureThread = std::this_thread::get_id();
    capturedResources.assign(entries.begin(), entries.end());
    if (entries.size() == 1u &&
        entries.front().kind == ChunkHandleKind::Buffer) {
      if (const auto* record =
              queue_.pool().findBuffer(entries.front().handle.value)) {
        capturedBufferLastUsedSeq = record->lastUsedSeqId;
      }
    }
    const auto result = queue_.captureChunkBufferBindings(entries, snapshots);
    observeCapturedBindings(snapshots);
    return result;
  }

  ChunkBufferBindingCaptureResult markChunkResourcesAndCaptureBufferBindings(
      std::span<const ChunkHandleEntry> entries,
      std::vector<ChunkBufferBindingSnapshot>& snapshots) override {
    ++combinedMarkCalls;
    const auto result =
        queue_.markChunkResourcesAndCaptureBufferBindings(entries, snapshots);
    observeCapturedBindings(snapshots);
    return result;
  }

  void observeCapturedBindings(
      std::span<const ChunkBufferBindingSnapshot> snapshots) {
    if (!recordBoundedCapturedBindings) return;
    for (const auto& capture : snapshots) {
      if (!capture.snapshot.valid() || capture.snapshot.contentsAddress == 0u ||
          capture.snapshot.byteSize == 0u) {
        continue;
      }
      const auto* bytes = reinterpret_cast<const std::uint8_t*>(
          static_cast<std::uintptr_t>(capture.snapshot.contentsAddress));
      CapturedBindingObservation observation{
          .metalHandle = capture.snapshot.metalHandle,
          .contentsAddress = capture.snapshot.contentsAddress,
          .contentRevision = capture.snapshot.contentRevision,
          .bytes = std::vector<std::uint8_t>(
              bytes, bytes + static_cast<std::size_t>(capture.snapshot.byteSize))};
      capturedBindingObservations.push_back(std::move(observation));
    }
  }

  void markChunkResources(
      std::span<const ChunkHandleEntry> entries) override {
    ++legacyMarkCalls;
    legacyMarkThread = std::this_thread::get_id();
    queue_.markChunkResources(entries);
  }

  void submitClear(const ClearDesc& clear) override {
    ++clearCalls;
    queue_.submitClear(clear);
    if (rejectAfterClear_ || (rejectAfterFinalClear_ && clearCalls >= 3u)) {
      queue_.rejectActiveCpuReadyArenaSource();
      organicFailure = dxmt9::CommandQueueArenaLeaseTestAccess::activeFailure(queue_);
      organicFailureObserved = true;
    }
  }

  void submitDrawRun(CanonicalDrawState state,
                     const DrawUniformPayload& uniforms,
                     std::span<const DrawParam> draws,
                     std::span<const DrawParamPayloadView> payloads) override {
    // Count DRAWS, not calls: the ordinary lane folds consecutive
    // identical-state draws into one run, so a call is a queue acquisition and
    // no longer a draw. `ordinaryDrawRunSubmissions` is the acquisition count.
    const auto count = static_cast<std::uint32_t>(draws.size());
    drawCalls += count;
    ordinaryDrawCalls += count;
    ++ordinaryDrawRunSubmissions;
    lastDrawRunSize = count;
    queue_.submitDrawRun(std::move(state), uniforms, draws, payloads);
  }

  bool submitDrawRunBatch(
      std::span<const dxmt9::core::DrawRunBatchEntry> entries) override {
    std::uint32_t count = 0;
    for (const auto& entry : entries) {
      if (!entry.valid()) continue;
      count += static_cast<std::uint32_t>(entry.draws.size());
    }
    drawCalls += count;
    ordinaryDrawCalls += count;
    ++ordinaryDrawRunSubmissions;
    lastDrawRunSize = count;
    return queue_.submitDrawRunBatch(entries);
  }

  DirectReplayDrawDisposition submitDirectReplayDraw(
      const DirectReplayDrawInput& input) noexcept override {
    const auto disposition = queue_.submitDirectReplayDraw(input);
    if (disposition == DirectReplayDrawDisposition::Appended ||
        disposition == DirectReplayDrawDisposition::AcceptedFailStop) {
      ++drawCalls;
    }
    return disposition;
  }

  void present(const SwapDesc& desc) override {
    ++presentCalls;
    lastPresentSeqId = queue_.submitPresent(desc);
  }

  void submitGenerateMipmaps(const GenerateMipmapsDesc& desc) override {
    queue_.submitGenerateMipmaps(desc);
  }

  BackendLimits limits_{};
  dxmt9::CommandQueue queue_;
  bool rejectAfterClear_ = false;
  bool rejectAfterFinalClear_ = false;
  bool directChunkSlot_ = false;
  bool cpuReadyArena_ = true;
  bool controlledResourcePool_ = false;
  WMT::Device resourceDevice_{};
  std::uint64_t nextHandle_ = 1;
  std::atomic<std::uint32_t> clearCalls{0};
  std::atomic<std::uint32_t> drawCalls{0};
  std::atomic<std::uint32_t> ordinaryDrawCalls{0};
  std::atomic<std::uint32_t> ordinaryDrawRunSubmissions{0};
  std::atomic<std::uint32_t> presentCalls{0};
  std::uint32_t flushCalls = 0u;
  std::function<void()> flushHook;
  std::uint32_t lastDrawRunSize = 0;
  std::uint64_t lastPresentSeqId = 0;
  std::uint32_t captureCalls = 0;
  std::uint32_t legacyMarkCalls = 0;
  std::uint32_t combinedMarkCalls = 0;
  std::atomic<std::uint32_t> mutationApplies{0};
  std::uint64_t capturedBufferLastUsedSeq = UINT64_MAX;
  std::thread::id captureThread{};
  std::thread::id legacyMarkThread{};
  std::vector<ChunkHandleEntry> capturedResources;
  std::vector<CapturedBindingObservation> capturedBindingObservations;
  // Tiny backing-lifetime fixtures keep complete historical bytes. General
  // execution instead observes each final-slot snapshot at consumption time.
  bool recordBoundedCapturedBindings = true;
  std::vector<TextureUploadObservation> textureUploadObservations;
  std::function<bool(TextureHandle, std::uint32_t, std::uint32_t,
                     std::uint32_t, std::uint32_t, std::uint32_t,
                     std::uint32_t, std::span<const std::uint8_t>)>
      generalTextureUpload;
  bool textureUploadFailed = false;
  dxmt9::CommandQueue::CpuReadyArenaFailureSnapshot organicFailure{};
  bool organicFailureObserved = false;
};

struct RuntimeFixture {
  using ControlledAccess = dxmt9::CommandQueueArenaLeaseTestAccess;
  using ControlledConsumer = dxmt9::test::CpuV4ControlledConsumer<ControlledAccess::ControlledCpuBackend>;
  struct ControlledConsumerHandle {
    std::shared_ptr<ControlledConsumer> consumer{};
    std::shared_ptr<ControlledAccess::ControlledCpuBackendState> backendState{};

    ~ControlledConsumerHandle() { cancelAndJoin(); }
    bool start() { return !joined_ && consumer && consumer->start(); }
    // Finish admission observes this post-join acknowledgement, not merely a
    // workerExited snapshot or an independently retained backend object.
    void join() {
      if (joined_ || !consumer) return;
      if (consumer->snapshot().state.phase == dxmt9::test::CpuV4ConsumerPhase::Idle)
        consumer->failAndJoin(dxmt9::test::CpuV4ConsumerFailure::Cancelled);
      else consumer->join();
      joined_ = true;
      authority_->consumerJoined.store(true, std::memory_order_release);
    }
    void cancelAndJoin() {
      if (joined_ || !consumer) return;
      const auto phase = consumer->snapshot().state.phase;
      if (phase == dxmt9::test::CpuV4ConsumerPhase::Stopped ||
          phase == dxmt9::test::CpuV4ConsumerPhase::Failed) consumer->join();
      else consumer->failAndJoin(dxmt9::test::CpuV4ConsumerFailure::Cancelled);
      joined_ = true;
      authority_->consumerJoined.store(true, std::memory_order_release);
    }

  private:
    friend struct RuntimeFixture;
    std::shared_ptr<ControlledAccess::ControlledCpuFixtureAuthority> authority_{};
    bool joined_ = false;
  };

  explicit RuntimeFixture(bool rejectAfterClear = false,
                          bool segmentSerial = false,
                          bool directChunkSlot = false,
                          bool cpuReadyArena = true,
                          WMT::Device resourceDevice = {},
                          dxmt9::d3d9::ReplayQueueAllocationFailure*
                              allocationFailure = nullptr,
                          const PresentParameters* outputParameters = nullptr,
                          bool exactRenderTapeActionProfile = false) {
    allocationFailure_ = allocationFailure;
    auto upper = std::make_unique<RoutingDevice>(rejectAfterClear,
                                                  segmentSerial,
                                                  directChunkSlot,
                                                  cpuReadyArena,
                                                  resourceDevice,
                                                  exactRenderTapeActionProfile);
    routing = upper.get();
    factory = dxmt9::com::Direct3DCreate9Ex(
        dxmt9::com::D3D_SDK_VERSION, std::move(upper));
    check(factory != nullptr, "routing factory must construct");

    PresentParameters params{};
    params.backBufferWidth = 16;
    params.backBufferHeight = 16;
    params.backBufferFormat = Format::A8R8G8B8;
    params.windowed = true;
    params.deviceWindow = Handle{91};
    params.presentationInterval = PresentInterval::Immediate;
    if (outputParameters) params = *outputParameters;
    device = factory->CreateDeviceEx(0, params, nullptr);
    check(device != nullptr, "routing core device must construct");
    device->AddRef();
    cDevice = std::make_unique<D9CDevice>(device);
    if (allocationFailure) {
      cDevice->replayOffload = std::make_unique<
          dxmt9::d3d9::ReplayOffloadWorker>(
          dxmt9::d3d9::ReplayOffloadWorker::TestAllocationConfig{
              allocationFailure});
    }
  }

  ~RuntimeFixture() {
    releaseCoreForInspection();
  }

  dxmt9::d3d9::RenderTapeControlledCpuV4Capability issueControlledCpuCapability() {
    if (!cDevice || capabilityIssued_) return {};
    const auto backend = cDevice->dev().upperDevice();
    if (!backend || backend.get() != routing || cDevice->replayOffload) return {};
    controlledLifetime_ = std::make_shared<std::uint8_t>(0u);
    controlledAuthority_ = std::make_shared<ControlledAccess::ControlledCpuFixtureAuthority>();
    controlledAuthority_->owner = cDevice.get();
    controlledAuthority_->backend = backend.get();
    controlledAuthority_->queue = &routing->queue_;
    controlledAuthority_->workersDisabledAtIssue = ControlledAccess::cpuTapeWorkersDisabled(routing->queue_);
    auto capability = ControlledAccess::mintControlledCpu(cDevice.get(), backend,
        controlledLifetime_, *controlledAuthority_);
    if (!capability.lease().visitBackend(dxmt9::d3d9::RenderTapeControlledCpuV4Boundary::Mint,
            nullptr, [](void*, D9CDevice*, dxmt9::Device&) noexcept { return true; })) {
      controlledLifetime_.reset();
      controlledAuthority_.reset();
      return {};
    }
    capabilityIssued_ = true;
    return capability;
  }

  std::shared_ptr<ControlledConsumerHandle> makeControlledCpuConsumer(
      const dxmt9::d3d9::RenderTapeControlledCpuV4Capability& capability,
      const D9CRenderTapeIdentityCaptureV4ArmResult& arm,
      dxmt9::test::CpuV4ConsumerLimits limits,
      std::chrono::steady_clock::time_point deadline,
      ControlledAccess::ControlledCpuPayloadObserver payloadObserver = {}) {
    if (!cDevice || consumerRegistered_ || !controlledAuthority_) return {};
    auto backend = ControlledAccess::makeControlledCpuBackend(capability,
        cDevice->dev().upperDevice(), controlledAuthority_, arm, std::move(payloadObserver));
    return backend ? registerControlledCpuConsumer(std::move(*backend), limits, deadline) : nullptr;
  }

  bool closeControlledCpuProducer() {
    return controlledAuthority_ && ControlledAccess::closeControlledCpuProducer(*controlledAuthority_);
  }

  // Cold CPU iterations are fresh-device, not warm resets. Keep the real
  // backend alive while destroying every core/cache owner, so Pool and queue
  // reclamation can be checked afterwards without a dangling routing pointer.
  void releaseCoreForInspection() {
    // Join every surviving handle before token expiry, root/reset operations,
    // or wrapper destruction. The handle itself also joins on stack unwind.
    for (auto& weak : controlledConsumers_)
      if (const auto handle = weak.lock()) handle->cancelAndJoin();
    if (controlledAuthority_)
      controlledAuthority_->isolated.store(false, std::memory_order_release);
    controlledLifetime_.reset();
    if (cDevice && !inspectionBackend_)
      inspectionBackend_ = cDevice->dev().backend();
    cDevice.reset();
    if (device) {
      (void)device->Release();
      device = nullptr;
    }
    if (factory) {
      (void)factory->Release();
      factory = nullptr;
    }
  }

  RoutingDevice* routing = nullptr;
  dxmt9::d3d9::ReplayQueueAllocationFailure* allocationFailure_ = nullptr;
  dxmt9::com::IDirect3D9Ex* factory = nullptr;
  dxmt9::com::IDirect3DDevice9Ex* device = nullptr;
  std::unique_ptr<D9CDevice> cDevice;
  std::shared_ptr<dxmt9::Device> inspectionBackend_;

private:
  friend void controlledCpuV4ConsumerUsesRealQueue();
  std::shared_ptr<ControlledConsumerHandle> makeQueueOnlyControlledCpuConsumerForTest(
      const dxmt9::d3d9::RenderTapeControlledCpuV4Capability& capability,
      ControlledAccess::ControlledCpuFault fault,
      std::chrono::steady_clock::time_point deadline,
      dxmt9::test::CpuV4ConsumerLimits limits = {}) {
    if (!cDevice || consumerRegistered_ || !controlledAuthority_) return {};
    auto backend = ControlledAccess::makeQueueOnlyControlledCpuBackendForTest(capability,
        cDevice->dev().upperDevice(), controlledAuthority_, fault);
    return backend ? registerControlledCpuConsumer(std::move(*backend), limits, deadline) : nullptr;
  }
  std::shared_ptr<ControlledConsumerHandle> registerControlledCpuConsumer(
      ControlledAccess::ControlledCpuBackend backend, dxmt9::test::CpuV4ConsumerLimits limits,
      std::chrono::steady_clock::time_point deadline) {
    auto handle = std::make_shared<ControlledConsumerHandle>();
    handle->backendState = backend.state();
    handle->consumer = std::make_shared<ControlledConsumer>(std::move(backend), limits, deadline);
    handle->authority_ = controlledAuthority_;
    controlledConsumers_.push_back(handle);
    consumerRegistered_ = true;
    controlledAuthority_->consumerJoined.store(false, std::memory_order_release);
    return handle;
  }
  std::shared_ptr<void> controlledLifetime_{};
  std::shared_ptr<ControlledAccess::ControlledCpuFixtureAuthority> controlledAuthority_{};
  std::vector<std::weak_ptr<ControlledConsumerHandle>> controlledConsumers_{};
  bool capabilityIssued_ = false, consumerRegistered_ = false;
};

void controlledCpuV4ConsumerUsesRealQueue() {
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Boundary = dxmt9::d3d9::RenderTapeControlledCpuV4Boundary;
  using Failure = dxmt9::test::CpuV4ConsumerFailure;
  using Actual = Access::ControlledCpuActualPhase;
  using Fault = Access::ControlledCpuFault;
  using Clock = std::chrono::steady_clock;
  const auto accepts = [](const auto& capability, Boundary boundary = Boundary::Mint) {
    return capability.lease().visitBackend(boundary, nullptr,
        [](void*, D9CDevice*, dxmt9::Device&) noexcept { return true; });
  };

  {
    RuntimeFixture normal;
    const auto profile = normal.routing->queue_.renderTapeActionProfileSnapshot();
    check(profile.valid && profile.constructionProfile == dxmt9::kRenderTapeActionProfileNormal &&
              profile.currentProfile == dxmt9::kRenderTapeActionProfileNormal && !profile.revision,
          "ordinary inert fixture constructs Normal action history at revision zero");
    auto capability = normal.issueControlledCpuCapability();
    check(accepts(capability) && !accepts(normal.issueControlledCpuCapability()),
          "one cold exact-owner fixture grants one move-only CPU capability");
    const D9CRenderTapeIdentityCaptureV4ArmResult fabricated{
        D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE, 4u, 1u, 1u, 1u};
    check(!normal.makeControlledCpuConsumer(capability, fabricated, {}, Clock::now() + std::chrono::seconds(5)),
          "production adapter rejects fabricated localArm labels without a live Bound root");
    auto token = std::make_shared<std::uint8_t>(0u);
    RuntimeFixture different;
    Access::ControlledCpuFixtureAuthority wrongOwner;
    wrongOwner.owner = different.cDevice.get();
    wrongOwner.backend = normal.routing;
    wrongOwner.queue = &normal.routing->queue_;
    wrongOwner.workersDisabledAtIssue = true;
    check(!accepts(Access::mintControlledCpu(normal.cDevice.get(), normal.cDevice->dev().upperDevice(),
              token, wrongOwner)), "mint cannot substitute a different borrowed wrapper identity");
    Access::ControlledCpuFixtureAuthority wrongBackend;
    wrongBackend.owner = normal.cDevice.get();
    wrongBackend.backend = different.routing;
    wrongBackend.queue = &different.routing->queue_;
    wrongBackend.workersDisabledAtIssue = true;
    check(!accepts(Access::mintControlledCpu(normal.cDevice.get(), normal.cDevice->dev().upperDevice(),
              token, wrongBackend)), "mint cannot substitute another inert backend or queue identity");
  }
  {
    RuntimeFixture exact(false, false, false, true, {}, nullptr, nullptr, true);
    const auto arm = exact.routing->queue_.renderTapeActionProfileSnapshot();
    auto capability = exact.issueControlledCpuCapability();
    check(accepts(capability) && arm.valid && !arm.revision &&
              arm.constructionProfile == dxmt9::kRenderTapeActionProfileExact &&
              dxmt9::renderTapeActionProfileStableExact(arm, arm),
          "opted exact fixture constructs genuine Exact action history at revision zero");
    exact.routing->queue_.setRenderTapeExactAttachmentPreservation(false);
    exact.routing->queue_.setRenderTapeExactAttachmentPreservation(true);
    const auto freeze = exact.routing->queue_.renderTapeActionProfileSnapshot();
    check(freeze.revision == 2u && !dxmt9::renderTapeActionProfileStableExact(arm, freeze),
          "setter ABA never manufactures a stable construction Exact witness");
  }
  {
    RuntimeFixture populated;
    populated.routing->submitClear(ClearDesc{});
    check(!accepts(populated.issueControlledCpuCapability()),
          "mint rejects a populated writer before publication or CPU effects");
    dxmt9::d3d9::ReplayQueueAllocationFailure fault{};
    RuntimeFixture offloaded(false, false, false, true, {}, &fault);
    check(!accepts(offloaded.issueControlledCpuCapability()), "mint rejects replay-offload authority");
    RuntimeFixture segmented(false, true);
    check(!accepts(segmented.issueControlledCpuCapability()),
          "one-source consumer rejects group-held SegmentSerial retirement contracts");
  }
  {
    // Queue-only local labels below are NOT V4 arm/finalization evidence.
    RuntimeFixture empty;
    auto capability = empty.issueControlledCpuCapability();
    auto handle = empty.makeQueueOnlyControlledCpuConsumerForTest(capability, Fault::None,
        Clock::now() + std::chrono::seconds(5));
    check(handle && handle->start(), "controlled consumer starts one owned real-CV worker");
    check(empty.closeControlledCpuProducer() && handle->consumer->closeProducer() &&
              handle->consumer->stopAndJoin(), "producer close wakes empty encode CV and joins normally");
    handle->join();
    const auto snapshot = handle->consumer->snapshot();
    check(snapshot.workerExited && snapshot.cleanupComplete && !snapshot.cleanupFailed &&
              !snapshot.state.observed && !Access::controlledCpuQueueStopped(empty.routing->queue_),
          "empty controlled stop never poisons or sets production queue stop");
  }
  {
    RuntimeFixture burst;
    auto capability = burst.issueControlledCpuCapability();
    auto handle = burst.makeQueueOnlyControlledCpuConsumerForTest(capability, Fault::None,
        Clock::now() + std::chrono::seconds(10));
    check(handle && handle->start(), "burst consumer starts before one compatibility raw");
    constexpr std::size_t kSources = dxmt9::kMaxQueuedChunks + 33u;
    const std::vector<RecordSpec> records(kSources, presentRecord());
    const auto wire = makeWireFixture(records);
    check(dxmt9::d3d9::replayPrevalidatedResolvedCommandChunk(burst.cDevice.get(),
              wire.bytes, wire.envelope, {}, dxmt9::d3d9::ProviderReplayPolicy::Compatibility) == D3D_OK,
          "one unchanged raw publishes more than the ring capacity while consumer drains concurrently");
    check(burst.closeControlledCpuProducer() && handle->consumer->closeProducer() &&
              handle->consumer->stopAndJoin(), "natural Present cuts drain without per-raw synthetic publication");
    handle->join();
    const auto receipts = handle->consumer->takeReceipts();
    const auto snapshot = handle->consumer->snapshot();
    check(receipts.size() == kSources && snapshot.state.observed == kSources &&
              snapshot.state.submitted == kSources && snapshot.state.acknowledged == kSources &&
              snapshot.state.retired == kSources && snapshot.state.commands == kSources &&
              snapshot.state.lifecycleActions == 4u * kSources && snapshot.cleanupComplete &&
              !snapshot.payloadDigestPresent && !snapshot.fullSemanticOracleComplete &&
              !snapshot.actualMetal && !snapshot.gpuResultOracle && !snapshot.timingEligible &&
              Access::residentCount(burst.routing->queue_) == 0u,
          "actual FIFO receipts prove only controlled queue retirement and owned metadata");
    for (std::size_t index = 0u; index < receipts.size(); ++index) {
      const auto& row = receipts[index];
      check(row.observation && row.observation->identity.sequence == index + 1u &&
                row.observation->commands.size() == 1u &&
                row.observation->commands[0].kind == static_cast<std::uint32_t>(MetalCommandKind::Present) &&
                !row.observation->commands[0].drawCount && row.submitted && row.acknowledged &&
                row.retired && row.cleanupComplete,
            "consumer preserves every natural source cut and command in producer order");
    }
    const auto target = receipts.back().observation->identity;
    check(handle->consumer->waitThrough(target, Clock::now() + std::chrono::seconds(1)),
          "waitThrough accepts only an exact actual retired receipt");
    for (unsigned dimension = 0u; dimension < 9u; ++dimension) {
      auto stale = target;
      switch (dimension) {
      case 0u: ++stale.queue; break;
      case 1u: ++stale.captureToken; break;
      case 2u: ++stale.captureEpoch; break;
      case 3u: ++stale.sequence; break;
      case 4u: ++stale.sourceIndex; break;
      case 5u: ++stale.sourceGeneration; break;
      case 6u: ++stale.storageFirstPage; break;
      case 7u: ++stale.storagePageCount; break;
      case 8u: ++stale.storageGeneration; break;
      }
      check(!handle->consumer->waitThrough(stale, Clock::now() + std::chrono::seconds(1)),
          "a watermark or partial source identity cannot satisfy an exact target");
    }
    const auto actual = handle->backendState->snapshot();
    check(actual.phase == Actual::Retired && actual.borrowReleased &&
              actual.observedOutsideQueueMutex && !actual.queueQuarantined,
          "successful cleanup follows actual retirement and releases every synchronous borrow");
  }
  for (const auto fault : {Fault::ObservationAllocation, Fault::AfterSubmit, Fault::AfterAcknowledge}) {
    RuntimeFixture fixture;
    auto capability = fixture.issueControlledCpuCapability();
    auto handle = fixture.makeQueueOnlyControlledCpuConsumerForTest(capability, fault,
        Clock::now() + std::chrono::seconds(5));
    check(handle && handle->start(), "failure consumer starts from a clean fixture");
    fixture.routing->present(SwapDesc{});
    handle->join();
    const auto snapshot = handle->consumer->snapshot();
    const auto actual = handle->backendState->snapshot();
    const auto expectedPhase = fault == Fault::ObservationAllocation ? Actual::Represented
        : fault == Fault::AfterSubmit ? Actual::Submitted : Actual::Completed;
    const auto expectedFailure = fault == Fault::ObservationAllocation ? Failure::Allocation
        : fault == Fault::AfterSubmit ? Failure::Submit : Failure::Acknowledge;
    check(snapshot.workerExited && snapshot.failure == expectedFailure && snapshot.cleanupFailed &&
              !snapshot.cleanupComplete && !snapshot.receipts && actual.identity.valid() &&
              actual.phase == expectedPhase && actual.borrowReleased && actual.queueQuarantined &&
              !actual.retired && Access::residentCount(fixture.routing->queue_) == 1u &&
              Access::completedSeqId(fixture.routing->queue_) == 0u,
          "failure after an actual queue effect quarantines exact ownership without inventing completion");
  }
  {
    RuntimeFixture capacity;
    auto capability = capacity.issueControlledCpuCapability();
    dxmt9::test::CpuV4ConsumerLimits limits{};
    limits.maxCommands = 1u;
    auto handle = capacity.makeQueueOnlyControlledCpuConsumerForTest(capability, Fault::None,
        Clock::now() + std::chrono::seconds(5), limits);
    check(handle && handle->start(), "bounded consumer starts before a two-command natural source");
    capacity.routing->submitClear(ClearDesc{});
    capacity.routing->present(SwapDesc{});
    handle->join();
    const auto snapshot = handle->consumer->snapshot();
    const auto actual = handle->backendState->snapshot();
    check(snapshot.failure == Failure::Capacity && snapshot.cleanupComplete &&
              !snapshot.cleanupFailed && !snapshot.state.observed && !actual.identity.valid() &&
              !actual.queueQuarantined && Access::readyCount(capacity.routing->queue_) == 1u,
          "command bound rejects before dequeue; Ready backlog remains queue-owned without a claimed borrow");
  }
  {
    RuntimeFixture revoked;
    auto capability = revoked.issueControlledCpuCapability();
    auto handle = revoked.makeQueueOnlyControlledCpuConsumerForTest(capability, Fault::None,
        Clock::now() + std::chrono::seconds(5));
    check(handle && revoked.closeControlledCpuProducer(), "revocation fixture has no live worker or claimed source");
    capability.revoke();
    check(!accepts(capability, Boundary::Replay) && !handle->consumer->closeProducer(),
          "adapter rechecks weak authority at the next synchronous boundary");
    handle->consumer->failAndJoin(Failure::Cancelled);
    handle->join();
    check(handle->consumer->snapshot().cleanupComplete,
          "independent backend pin proves empty cleanup after lease revocation without replay effects");
  }
  {
    dxmt9::d3d9::RenderTapeControlledCpuV4Capability capability;
    RuntimeFixture expired;
    capability = expired.issueControlledCpuCapability();
    const auto lease = capability.lease();
    check(accepts(capability), "weak-lifetime fixture initially grants live authority");
    expired.releaseCoreForInspection();
    check(!lease.visitBackend(Boundary::Replay, nullptr,
              [](void*, D9CDevice*, dxmt9::Device&) noexcept { return true; }),
          "backend pin alone cannot keep destroyed wrapper/probe authority live");
  }
}

// Authored reference provenance is authenticated before effects. Candidate
// provenance below comes only from the production root and replay callbacks.
dxmt9::d3d9::CaptureSourceReplayPlanV4 controlledCpuV4ReferencePlan(
    std::span<const std::byte> tape,
    const dxmt9::d3d9::render_tape_runtime::GeneralPreflightPlan& general,
    bool groupingQuery = false) {
  using namespace dxmt9::d3d9;
  RenderTapeIdentityV4View identity;
  identity.header.coordinatorDomain =
      static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial);
  identity.header.frameId = 9u;
  identity.header.captureToken = 100u;
  identity.header.captureEpoch = 101u;
  identity.header.presentOrdinal = general.presentOrdinal;
  identity.header.queue = 102u;
  identity.header.initializerEpoch = 103u;
  identity.header.baselineCompletionEdge = 1u;
  identity.header.baselineKind =
      static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline);
  RenderTapeExecutionTopologyV1View topology;
  topology.header.captureToken = identity.header.captureToken;
  topology.header.captureEpoch = identity.header.captureEpoch;
  topology.header.queue = identity.header.queue;
  std::uint64_t edge = 2u, generation = 1u;
  std::size_t expectedCommands = 0u;
  for (const auto& event : general.events) {
    if (event.type != RenderTapeEventType::CommandChunk) continue;
    const auto ordinal = general.tape.event(event.eventIndex).header.ordinal;
    const auto raw = 500u + ordinal;
    const auto& admitted = general.commandChunks[event.index];
    ImportedChunkView chunk;
    check(importPrevalidatedCommandChunk(admitted.bytes, admitted.envelope, chunk) &&
              !chunk.records.empty() && chunk.records.size() <= (groupingQuery ? 5u : 2u) &&
              std::all_of(chunk.records.begin(), chunk.records.end(), [groupingQuery](const auto& record) {
                return record.type == D9C_COMMAND_RECORD_CLEAR ||
                    record.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE ||
                    record.type == D9C_COMMAND_RECORD_PRESENT ||
                    (groupingQuery && record.type == D9C_COMMAND_RECORD_QUERY_ISSUE);
              }),
          "small General reference admits original GPU/coordinator records without selecting candidate cuts");
    const auto commandCount = static_cast<std::uint32_t>(std::count_if(
        chunk.records.begin(), chunk.records.end(), [](const auto& record) {
          return record.type != D9C_COMMAND_RECORD_QUERY_ISSUE;
        }));
    expectedCommands += commandCount;
    const auto begin = edge++, effect = edge++;
    const auto source = static_cast<std::uint32_t>(identity.sources.size());
    const auto command = static_cast<std::uint32_t>(identity.commands.size());
    const auto publication = edge++, acceptance = edge++, submission = edge++, completion = edge++;
    const auto buildGeneration = generation++;
    identity.sources.push_back({.lifecycle = {.sourceOrdinal = source + 1u, .seqId = source + 1u,
        .writerCaptureId = source + 1u, .writerGeneration = 2u, .storageGeneration = 3u,
        .publicationEdge = publication, .submissionEdge = submission, .completionEdge = completion,
        .completedSequence = source + 1u, .cutEventOrdinal = ordinal, .commandCount = commandCount,
        .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)},
        .rawOrdinal = raw, .publicationBuildGeneration = buildGeneration,
        .publicationControlIndex = source});
    std::uint32_t physicalCommand = 0u;
    for (std::uint32_t record = 0u; record < chunk.records.size(); ++record) {
      const auto type = chunk.records[record].type;
      if (type == D9C_COMMAND_RECORD_QUERY_ISSUE) {
        D9CCommandChunkWireQueryIssue issue{};
        const auto view = chunk.record(record);
        check(groupingQuery && source == 0u && record == 4u && commandCount == 4u &&
                  chunk.records.size() == 5u && view.payload.size() == sizeof(issue),
              "grouping reference retains one final END after four pre-END commands");
        std::memcpy(&issue, view.payload.data(), sizeof(issue));
        check(issue.flags == 1u && issue.queryHandleIndex < chunk.handles.size() &&
                  chunk.handles[issue.queryHandleIndex].kind == D9C_CHUNK_HANDLE_KIND_QUERY,
              "grouping reference resolves the authenticated EVENT END handle");
        const auto& handle = chunk.handles[issue.queryHandleIndex];
        const auto admission = edge++, dispatch = edge++;
        const auto coordinator = static_cast<std::uint32_t>(identity.coordinators.size());
        identity.coordinators.push_back({.provenance = {.eventOrdinal = ordinal,
            .rawOrdinal = raw, .buildGeneration = buildGeneration, .dispatchEdge = dispatch,
            .queryIdentity = {handle.kind, handle.generation, handle.objectId},
            .issueCycle = 1u, .issueOrdinal = 1u, .deviceEpoch = 77u,
            .writerCaptureId = source + 1u, .writerGeneration = 2u, .storageGeneration = 3u,
            .prefixSequence = source + 1u, .recordIndex = record,
            .recordType = type, .flags = issue.flags, .queryType = 8u,
            .prefixKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::RealSource),
            .sourceIndex = source, .commandEnd = commandCount}, .admissionEdge = admission,
            .authority = static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial)});
        identity.dispositions.push_back({.eventOrdinal = ordinal, .firstRecord = record,
            .recordCount = 1u,
            .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::Coordinator),
            .coordinatorIndex = coordinator});
        continue;
      }
      const bool draw = type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
      identity.commands.push_back({.sourceIndex = source, .physicalCommandIndex = physicalCommand,
          .kind = static_cast<std::uint32_t>(renderTapeIdentityV3RecordCommandKind(type)), .drawCount = draw ? 1u : 0u});
      identity.dispositions.push_back({.eventOrdinal = ordinal, .firstRecord = record, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
          .commandIndex = command + physicalCommand, .subdrawCount = draw ? 1u : 0u});
      topology.placements.push_back({.planIndex = source, .commandIndex = command + physicalCommand,
          .outcome = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Outcome::ExecutedWithoutDag),
          .executionOrdinal = physicalCommand});
      ++physicalCommand;
    }
    topology.plans.push_back({.planId = source + 11u, .acceptanceEdge = acceptance,
        .route = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::NaturalOrder),
        .firstPlacement = command, .placementCount = commandCount, .executedCount = commandCount});
    const RenderTapeIdentityV4ReplaySourceIdentity projection{
        identity.header.frameId, 600u + ordinal, 600u + ordinal, raw};
    identity.attempts.push_back({.eventOrdinal = ordinal, .rawOrdinal = raw,
        .attemptGeneration = buildGeneration, .beginEdge = begin, .terminalEdge = edge++,
        .effectEdge = effect, .identity = projection,
        .receipt = {.identity = projection, .queueSequence = raw,
            .kind = static_cast<std::uint32_t>(RenderTapeIdentityV4ReceiptKind::Compatibility),
            .commandCount = commandCount}, .recordCount = admitted.envelope.recordCount,
        .outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::Committed),
        .stagedCommandCount = commandCount});
    identity.settlements.push_back({ordinal, raw, edge++, admitted.envelope.recordCount});
  }
  check(identity.sources.size() == (groupingQuery ? 3u : 2u) &&
            identity.commands.size() == expectedCommands && identity.initializers.empty() &&
            identity.coordinators.size() == (groupingQuery ? 1u : 0u),
        "reference authenticates its original source obligations and optional exact END coordinator");
  RenderTapeIdentityV4Status identityStatus{};
  const auto identityBytes = buildRenderTapeIdentityV4(tape, general.catalogue, identity, &identityStatus);
  check(identityStatus == RenderTapeIdentityV4Status::Valid, "reference identity authenticates original raws");
  RenderTapeExecutionTopologyV1Status topologyStatus{};
  const auto topologyBytes = buildRenderTapeExecutionTopologyV1(
      tape, general.catalogue, identityBytes, topology, &topologyStatus);
  CaptureSourceReplayPlanV4 source;
  check(topologyStatus == RenderTapeExecutionTopologyV1Status::Valid &&
            buildCaptureSourceReplayPlanV4(tape, general.catalogue, identityBytes,
                topologyBytes, source) == CaptureSourceReplayPlanV4Status::Valid,
        "cold V4 reference pair authenticates before resource/replay/queue effects");
  return source;
}

struct MetalDeviceLease {
  obj_handle_t handle = NULL_OBJECT_HANDLE;
  MetalDeviceLease() = default;

  ~MetalDeviceLease() {
    if (handle != NULL_OBJECT_HANDLE) {
      dxmt9_test_release_metal_device(handle);
    }
  }

  MetalDeviceLease(const MetalDeviceLease&) = delete;
  MetalDeviceLease& operator=(const MetalDeviceLease&) = delete;
};

// This small unchanged fixture has no mutation after its seeded prefix. The
// proof deliberately does not generalize to concurrent in-place buffer writes.
// All producer-side wrappers/tables are absent from the consumer session.
struct ControlledCpuV4FixtureOracle {
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Version = dxmt9::test::CpuTapeResourceVersion;
  using Context = dxmt9::test::CpuTapeResourceContext;
  using Root = dxmt9::d3d9::RenderTapeRuntimeIdentityV4;

  std::shared_ptr<Root> root;
  dxmt9::render::TapeTopologyCaptureIdentity capture{};
  std::shared_ptr<const Context> authenticContext;
  dxmt9::test::CpuTapeRecordContextStore records{};
  const bool laterInPlaceMutationExcluded = true;
  const char* failure = nullptr;
  std::uint64_t digest = 1469598103934665603ull;
  std::size_t sources = 0u, successfulSources = 0u, locations = 0u, draws = 0u;
  std::size_t snapshots = 0u, liveBindings = 0u, bufferProofs = 0u, destroyPendingBufferProofs = 0u;
  std::size_t mipmaps = 0u;
  std::vector<dxmt9::render::TapeEmissionRecordLocator> selectedRecords{};

  bool fail(const char* reason) noexcept {
    if (!failure) failure = reason;
    return false;
  }

  void observe(dxmt9::CommandQueue& queue,
      const dxmt9::core::metalqueue::ReplaySourcePublicationReceipt& publication,
      const dxmt9::core::metalqueue::SynchronousSourcePayloadBorrow& payload) noexcept {
    using namespace dxmt9;
    ++sources;
    if (failure) return;
    try {
      d3d9::RenderTapeRuntimePublishedSourceOriginsV4 origins;
      if (!root || !authenticContext || !authenticContext->hasSemanticUniqueNativeAuthority() ||
          !laterInPlaceMutationExcluded ||
          !root->copyPublishedSourceOrigins(capture, publication, origins) ||
          origins.commands.size() != payload.commandCount()) {
        fail("fixture-source-origins-unavailable");
        return;
      }
      for (std::size_t index = 0u; index < payload.commandCount(); ++index) {
        const auto command = payload.commandAt(index);
        if (origins.commands[index].physicalCommandIndex != index ||
            origins.commands[index].kind != render::tapeTopologyCommandKind(command.kind()) ||
            origins.commands[index].drawCount != command.command.drawParams.size()) {
          fail("fixture-source-origin-extent-mismatch");
          return;
        }
      }
      std::vector<bool> selected(origins.origins.size(), false);
      std::shared_ptr<const Context> context;
      auto candidateDigest = digest;
      const auto select = [&](std::size_t command, core::MetalCommandKind kind, std::size_t subdraw) {
        const auto canonicalKind = render::tapeTopologyCommandKind(kind);
        const bool draw = kind == core::MetalCommandKind::DrawRun;
        const d3d9::RenderTapeRuntimePublishedSourceOriginV4* found = nullptr;
        std::size_t foundIndex = 0u;
        for (std::size_t index = 0u; index < origins.origins.size(); ++index) {
          const auto& origin = origins.origins[index];
          if (origin.physicalCommandIndex != command || origin.kind != canonicalKind ||
              (draw ? origin.firstSubdraw != subdraw || origin.subdrawCount != 1u
                    : origin.firstSubdraw || origin.subdrawCount || subdraw != test::kCpuTapeNoSubdraw))
            continue;
          if (found) return fail("fixture-ambiguous-source-origin");
          found = &origin;
          foundIndex = index;
        }
        if (!found || selected[foundIndex]) return fail("fixture-missing-or-reused-source-origin");
        const auto next = records.lookup(found->record);
        if (!next) return fail("fixture-missing-record-context");
        if (!next->hasSemanticUniqueNativeAuthority())
          return fail("fixture-inventory-context-has-no-semantic-authority");
        context = next;
        selected[foundIndex] = true;
        selectedRecords.push_back(found->record);
        ++locations;
        return true;
      };
      const auto qualifiedVersion = [&](std::uint32_t kind, core::Handle handle)
          -> std::shared_ptr<const Version> {
        const auto version = context ? context->find(kind, handle) : nullptr;
        const auto authentic = authenticContext->find(kind, handle);
        if (!version || !authentic || version->nativeKind() != kind ||
            version->nativeHandle() != handle ||
            !d3d9::renderTapeIdentityV3SameIdentity(version->identity(), authentic->identity()) ||
            version->contentEpoch() != authentic->contentEpoch()) {
          fail("fixture-stale-resource-version-mapping");
          return {};
        }
        return version;
      };
      const auto resource = [&](std::uint32_t kind, core::Handle handle, std::uint64_t& hash) {
        const auto version = qualifiedVersion(kind, handle);
        return version && test::appendCpuTapeResourceObservation(version->observation(), hash);
      };
      const auto observeBuffer = [&](core::Handle handle,
          const core::DrawBufferBindingSnapshot* snapshot,
          std::uint64_t& hash, std::uint64_t& revision) {
        const auto version = qualifiedVersion(D9C_CHUNK_HANDLE_KIND_BUFFER, handle);
        test::CpuTapeLiveBufferView view{};
        bool destroyPending = false;
        if (!version || !Access::copyControlledCpuBufferProofView(queue, handle,
                publication.metadata.seqId, snapshot, view, destroyPending))
          return fail("fixture-buffer-physical-view-unavailable");
        // The genuine borrow pins this source until the fresh arena-locked
        // comparison returns. A whole-buffer range is valid only because this
        // fixture excludes later in-place writes; it is not a draw footprint.
        const auto expected = version->expectedBufferBytes();
        const std::array ranges{resources::BufferReadProofRange{
            .byteSize = static_cast<std::uint64_t>(expected.size())}};
        const auto proof = queue.pool().proveBufferReadFootprint({
            .buffer = handle,
            .sourceSequence = publication.metadata.seqId,
            .completedSequence = queue.queryProgress().completed,
            .snapshot = snapshot ? std::optional<core::DrawBufferBindingSnapshot>{*snapshot}
                                 : std::nullopt,
            .expectedBytes = expected,
            .ranges = ranges});
        if (!proof.proved()) return fail("fixture-buffer-physical-bytes-mismatch");
        // Preserve the serial oracle's FNV byte stream using owned immutable
        // bytes, never a copied physical address after the arena lock ends.
        std::uint64_t content = test::detail::kCpuTapeFnvOffset;
        test::detail::cpuTapeDigestBytes(content, expected.data(), expected.size());
        test::detail::cpuTapeDigestScalar(hash, view.byteSize);
        test::detail::cpuTapeDigestScalar(hash, content);
        revision = view.contentRevision;
        ++bufferProofs;
        destroyPendingBufferProofs += destroyPending;
        return true;
      };
      const auto snapshot = [&](core::Handle handle,
          const core::DrawBufferBindingSnapshot& value, std::uint64_t& hash) {
        std::uint64_t revision = 0u;
        return observeBuffer(handle, &value, hash, revision);
      };
      const auto live = [&](core::Handle handle, std::uint64_t& hash, std::uint64_t& revision) {
        return observeBuffer(handle, nullptr, hash, revision);
      };
      const auto draw = [&](std::uint64_t) { ++draws; return true; };
      std::string semanticFailure;
      if (!test::observeCpuTapePayload(payload, candidateDigest, resource, draw,
              semanticFailure, &snapshots, snapshot, &liveBindings, live, &mipmaps, select)) {
        fail("fixture-payload-semantics-incomplete");
        return;
      }
      if (std::any_of(selected.begin(), selected.end(), [](bool value) { return !value; })) {
        fail("fixture-unvisited-source-origin");
        return;
      }
      digest = candidateDigest;
      ++successfulSources;
    } catch (...) {
      fail("fixture-semantic-observation-allocation");
    }
  }

  bool qualified(const dxmt9::test::CpuGeneralTapeExecutionResult& reference) const noexcept {
    return !failure && sources == 1u && successfulSources == 1u && locations == 2u &&
        draws == 1u && bufferProofs == 2u && !mipmaps &&
        snapshots == reference.observedSnapshots && liveBindings == reference.observedLiveBindings &&
        digest == reference.digest;
  }
};

void controlledCpuV4IntegrationUsesObservedReplay() {
  using namespace dxmt9::d3d9;
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<RuntimeFixture, Access>;
  using ResourceContext = dxmt9::test::CpuTapeResourceContext;
  using ResourceVersion = dxmt9::test::CpuTapeResourceVersion;
  using ContextStatus = dxmt9::test::CpuTapeResourceContextStatus;
  using Clock = std::chrono::steady_clock;
  enum class Scenario {
    Complete, MissingRawAckObservation, UnfinishedConsumer, UnacknowledgedJoin,
    WrongExpectedByte, MissingRecordContext, StaleNativeMapping, StaleIdentityMapping,
    StaleVersionMapping,
  };
  const auto oracleNegative = [](Scenario scenario) {
    return scenario == Scenario::WrongExpectedByte || scenario == Scenario::MissingRecordContext ||
        scenario == Scenario::StaleNativeMapping || scenario == Scenario::StaleIdentityMapping ||
        scenario == Scenario::StaleVersionMapping;
  };
  const auto emptyCpuOutput = [](const RenderTapeControlledCpuV4Evidence& output) {
    check(output.domain == RenderTapeControlledCpuV4Evidence::Domain::None &&
              !output.controlledCpuComplete && !output.canonicalTraversalComplete &&
              !output.rawAckCoverageComplete && !output.semanticMappingComplete &&
              !output.semanticMapping && output.actualIdentity.sources.empty() &&
              output.actualIdentity.commands.empty() && output.actualIdentity.attempts.empty() &&
              output.actionWitness == RenderTapeActionWitnessV1{},
          "rejected controlled finish exposes no partial CPU evidence or witness");
  };
  RenderTapeControlledCpuV4Evidence owned;
  std::shared_ptr<const ResourceContext> ownedContext;
  std::uint64_t ownedExpectedBytesDigest = 1469598103934665603ull;
  const auto expectedBytesDigest = [](const ResourceContext& context) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto& version : context.entries())
      dxmt9::test::detail::cpuGeneralHashBytes(hash, version->expectedBufferBytes());
    return hash;
  };
  {
    const auto captured = test::makeGeneralCpuDestroyFixture(false, false);
    std::vector<RenderTapeProviderBlob> blobs;
    for (const auto& bytes : captured.blobs)
      blobs.push_back({RenderTapeCaptureSession::sha256(bytes), bytes});
    runtime::GeneralPreflightPlan general;
    check(runtime::buildGeneralReplayPlan(captured.tape, blobs, general).complete(),
          "unchanged seeded destroy fixture passes the production General grammar");
    const auto source = controlledCpuV4ReferencePlan(captured.tape, general);
    PresentParameters output{};
    output.backBufferWidth = general.outputDesc.width;
    output.backBufferHeight = general.outputDesc.height;
    output.backBufferFormat = dxmt9::d3d9::devicec::fmtFromD3D(general.outputDesc.format);
    output.backBufferCount = 1u;
    output.windowed = true;
    output.deviceWindow = Handle{91};
    output.presentationInterval = PresentInterval::Immediate;
    MetalDeviceLease resourceDevice;
    resourceDevice.handle = dxmt9_test_acquire_metal_device();
    check(resourceDevice.handle != NULL_OBJECT_HANDLE,
        "immutable CPU oracle requires real CPU-visible resource backing, not a GPU result oracle");
    dxmt9::test::CpuGeneralTapeExecutionResult reference;
    {
      RuntimeFixture serial(false, false, false, true,
          WMT::Device{resourceDevice.handle}, nullptr, &output, true);
      // The established serial executor is an independent normalized oracle;
      // its publication schedule is not copied into the V4 candidate below.
      Executor executor(serial, general, blobs, {.route = ProviderReplayPolicy::Compatibility});
      reference = executor.run();
      check(reference.complete && reference.logicalResourceOracleComplete && reference.resetOk &&
                reference.draws == 1u && reference.sourceSummaryCommands == 2u &&
                reference.observedSnapshots + reference.observedLiveBindings == 2u,
            "independent serial execution observes the unchanged fixture's full normalized semantics: failure=" +
                reference.failure + " failed_event=" + std::to_string(reference.failedEvent) +
                " complete=" + std::to_string(reference.complete) +
                " logical_oracle=" + std::to_string(reference.logicalResourceOracleComplete) +
                " reset=" + std::to_string(reference.resetOk) +
                " commands=" + std::to_string(reference.commands) +
                " source_commands=" + std::to_string(reference.sourceSummaryCommands) +
                " draws=" + std::to_string(reference.draws) +
                " snapshots=" + std::to_string(reference.observedSnapshots) +
                " live=" + std::to_string(reference.observedLiveBindings));
    }
    for (const auto scenario : {Scenario::Complete, Scenario::MissingRawAckObservation,
                               Scenario::UnfinishedConsumer, Scenario::UnacknowledgedJoin,
                               Scenario::WrongExpectedByte, Scenario::MissingRecordContext,
                               Scenario::StaleNativeMapping, Scenario::StaleIdentityMapping,
                               Scenario::StaleVersionMapping}) {
      RuntimeFixture fixture(false, false, false, true,
          WMT::Device{resourceDevice.handle}, nullptr, &output, true);
      struct Owners {
        std::vector<runtime::OwnedObject> values{};
        ~Owners() { for (auto& value : values) runtime::releaseOwnedObject(value); }
      } objects;
      objects.values.reserve(general.definitions.size());
      std::vector<dxmt9::test::detail::LogicalReceipt> logicalReceipts;
      Executor normalizer(fixture, general, blobs);
      RenderTapeExecutionV4Evidence evidence;
      RenderTapeExecutionV4 execution;
      check(execution.admit(source, general, captured.tape, evidence,
                RenderTapeExecutionV4PublicationMode::SemanticMapping,
                RenderTapeExecutionV4ProducerContract::None) &&
                execution.snapshotPrefixEvents() == 8u,
            "controlled gate admits exact immutable input before real setup effects");
      for (const auto& definition : general.definitions) {
        auto* value = runtime::createRenderTapeObject(fixture.cDevice.get(), definition.fixed,
            definition.descriptor, blobs, objects.values);
        check(value != nullptr, "prefix acquires each real provider object exactly once");
        objects.values.push_back({definition.fixed.identity, value});
      }
      check(general.mutations.size() == 2u, "original prefix contains both authenticated buffer seeds");
      for (const auto& mutation : general.mutations) {
        const auto* blob = dxmt9::test::detail::cpuGeneralFindBlob(blobs, mutation.digest);
        auto* buffer = static_cast<D9CBuffer*>(runtime::findExactObject(objects.values, mutation.identity));
        RenderTapeBufferMutationReplayPlan plan{};
        void* bits = nullptr;
        check(blob && buffer && blob->bytes.size() == mutation.byteSize &&
                  renderTapeBufferMutationReplayPlan(mutation, plan) &&
                  dxmt9c_buffer_lock(buffer, plan.byteOffset, plan.byteSize, &bits,
                      plan.lockFlags) == D3D_OK && bits,
              "authenticated buffer seed enters the actual provider lock path");
        std::memcpy(bits, blob->bytes.data(), blob->bytes.size());
        check(dxmt9c_buffer_unlock(buffer) == D3D_OK,
              "actual provider unlock completes the promised prefix seed");
        dxmt9::test::detail::cpuGeneralRecordReceipt(logicalReceipts,
            mutation.identity, mutation.subresource, mutation.digest);
      }
      // Bootstrap refers to later definitions. Complete its real state replay
      // after those owners/seeds exist, before arm or prefix acknowledgment.
      for (const auto& chunk : general.bootstrapChunks) {
        std::vector<void*> resolved(chunk.envelope.handleCount);
        std::size_t count = 0u;
        check(runtime::resolveCommandHandles(chunk, objects.values, resolved, count) &&
                  replayPrevalidatedResolvedCommandChunk(fixture.cDevice.get(), chunk.bytes,
                      chunk.envelope, std::span<void* const>(resolved.data(), count),
                      ProviderReplayPolicy::Compatibility) == D3D_OK,
              "real bootstrap resolves its complete resource closure before arm");
      }
      check(general.bootstrapChunks.size() == 1u && !fixture.routing->drawCalls &&
                !fixture.routing->presentCalls && !Access::writingCommandCount(fixture.routing->queue_) &&
                !Access::readyCount(fixture.routing->queue_) && !Access::residentCount(fixture.routing->queue_),
            "completed prefix emits no interval source or synthetic publication");
      check(std::none_of(general.events.begin(), general.events.end(), [&](const auto& event) {
                return event.eventIndex >= execution.snapshotPrefixEvents() &&
                    event.type == RenderTapeEventType::ResourceMutation;
              }),
          "physical fixture proof explicitly excludes every later in-place mutation");
      const auto freezeActiveResources = [&] {
        std::vector<std::shared_ptr<const ResourceVersion>> versions;
        for (const auto& object : objects.values) {
          if (!object.value) continue;
          dxmt9::core::Handle native{};
          const D9CBuffer* buffer = nullptr;
          if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
            buffer = static_cast<const D9CBuffer*>(object.value);
            if (buffer->obj) native = buffer->obj->handle();
          } else if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
            const auto* surface = static_cast<const D9CSurface*>(object.value);
            if (surface->obj) native = surface->obj->handle();
          } else if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_TEXTURE) {
            const auto* texture = static_cast<const D9CTexture*>(object.value);
            if (texture->obj) native = texture->obj->handle();
          } else continue;
          dxmt9::test::CpuTapeResourceObservation observation;
          check(native && normalizer.captureResourceObservation(objects.values, logicalReceipts, {},
                    object.identity.kind, native, observation),
              "producer captures admitted normalization from the actual still-owned wrapper");
          std::uint64_t epoch = 1u;
          for (const auto& receipt : observation.receipts()) epoch = std::max(epoch, receipt.revision);
          const dxmt9::test::CpuTapeResourceVersionInput input{
              .nativeKind = object.identity.kind, .nativeHandle = native, .contentEpoch = epoch,
              .expectedBufferByteSize = buffer ? buffer->desc.size : 0u,
              .expectedBufferBytes = buffer ? buffer->obj->bytes() : std::span<const std::uint8_t>{}};
          std::shared_ptr<const ResourceVersion> version;
          check(dxmt9::test::buildCpuTapeResourceVersion(input, observation, version) == ContextStatus::Accepted,
              "producer freezes complete CPU expected bytes and normalized facts into an immutable version");
          versions.push_back(std::move(version));
        }
        std::shared_ptr<const ResourceContext> context;
        check(dxmt9::test::buildCpuTapeResourceContext(versions, context) == ContextStatus::Accepted,
            "original record context owns only immutable value versions, never producer wrapper tables");
        return context;
      };
      const auto authenticContext = freezeActiveResources();
      for (std::uint32_t index = 0u; index < execution.snapshotPrefixEvents(); ++index)
        check(execution.beginEvent(index) && execution.replayedEvent(index),
              "original prefix cursor acknowledges already-completed real setup effects");
      auto capability = fixture.issueControlledCpuCapability();
      check(execution.armControlledCpu(fixture.cDevice.get(), capability) && evidence.armed &&
                evidence.localArm.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE &&
                evidence.localArm.version == 4u && evidence.localArm.captureToken &&
                evidence.localArm.captureEpoch && evidence.localArm.queue,
            "real isolated fixture arm creates the candidate-local Bound root");
      const auto deadline = Clock::now() + std::chrono::seconds(10);
      for (unsigned dimension = 0u; dimension < 5u; ++dimension) {
        auto foreign = evidence.localArm;
        switch (dimension) {
        case 0u: ++foreign.captureToken; break;
        case 1u: ++foreign.captureEpoch; break;
        case 2u: ++foreign.queue; break;
        case 3u: ++foreign.version; break;
        case 4u: foreign.status = D9C_RENDER_TAPE_IDENTITY_CAPTURE_FAILED; break;
        }
        check(!fixture.makeControlledCpuConsumer(capability, foreign, {}, deadline),
              "adapter rejects each mismatched genuine localArm dimension before registration");
      }
      // Destruction joins this handle before capability revocation or owner
      // release, including stack unwinding after any failed assertion.
      auto oracle = std::make_shared<ControlledCpuV4FixtureOracle>();
      oracle->root = acquireRenderTapeRuntimeIdentityV4(fixture.cDevice.get());
      oracle->capture = {evidence.localArm.captureToken, evidence.localArm.captureEpoch, evidence.localArm.queue};
      oracle->authenticContext = authenticContext;
      check(oracle->root != nullptr, "consumer session retains the actual local Bound root before start");
      auto handle = fixture.makeControlledCpuConsumer(capability, evidence.localArm, {}, deadline,
          [oracle](auto& queue, const auto& publication, const auto& payload) noexcept {
            oracle->observe(queue, publication, payload);
          });
      check(handle && handle->start(), "real localArm factory starts consumer before Compatibility replay");
      std::size_t publicationCursor = 0u;
      std::vector<dxmt9::core::metalqueue::ReplaySourcePublicationReceipt> publications;
      for (const auto& event : general.events) {
        if (event.eventIndex < execution.snapshotPrefixEvents()) continue;
        check(execution.beginEvent(event.eventIndex), "original interval event starts exactly once");
        if (event.type == RenderTapeEventType::CommandChunk) {
          const auto& chunk = general.commandChunks[event.index];
          ImportedChunkView imported;
          check(importPrevalidatedCommandChunk(chunk.bytes, chunk.envelope, imported),
                "original raw remains unchanged after cold admission");
          const bool present = imported.records[0].type == D9C_COMMAND_RECORD_PRESENT;
          auto context = freezeActiveResources();
          if (!present && oracleNegative(scenario) && scenario != Scenario::MissingRecordContext) {
            std::vector<std::shared_ptr<const ResourceVersion>> versions(context->entries().begin(),
                context->entries().end());
            const auto target = std::find_if(versions.begin(), versions.end(), [&](const auto& version) {
              return renderTapeIdentityV3SameIdentity(version->identity(),
                  test::general_cpu_destroy_fixture_detail::kVertexBuffer);
            });
            check(target != versions.end(), "negative changes exactly one previously authenticated buffer version");
            auto observation = (*target)->observation();
            const auto expected = (*target)->expectedBufferBytes();
            std::vector<std::uint8_t> bytes(expected.begin(), expected.end());
            dxmt9::test::CpuTapeResourceVersionInput input{
                .nativeKind = (*target)->nativeKind(), .nativeHandle = (*target)->nativeHandle(),
                .contentEpoch = (*target)->contentEpoch(), .expectedBufferByteSize = bytes.size(),
                .expectedBufferBytes = bytes};
            if (scenario == Scenario::WrongExpectedByte) {
              check(!bytes.empty(), "wrong-byte negative targets an owned complete nonempty extent");
              bytes[0] ^= 1u;
            } else if (scenario == Scenario::StaleNativeMapping) {
              input.nativeHandle.value ^= std::uint64_t{1u} << 63u;
            } else if (scenario == Scenario::StaleVersionMapping) {
              ++input.contentEpoch;
            } else {
              check(scenario == Scenario::StaleIdentityMapping, "remaining negative changes identity only");
              auto identity = observation.identity();
              ++identity.generation;
              const dxmt9::test::CpuTapeResourceObservationInput stale{
                  .identity = identity, .contentIdentity = identity, .receipts = observation.receipts()};
              check(dxmt9::test::buildCpuTapeResourceObservation(stale, observation) ==
                        dxmt9::test::CpuTapeResourceObservationStatus::Accepted,
                  "stale identity is an internally valid normalization, not producer acquisition authority");
            }
            std::shared_ptr<const ResourceVersion> wrong;
            check(dxmt9::test::buildCpuTapeResourceVersion(input, observation, wrong) == ContextStatus::Accepted,
                "negative immutable version remains well-formed without proving authentic record binding");
            *target = std::move(wrong);
            check(dxmt9::test::buildCpuTapeResourceContext(versions, context) == ContextStatus::Accepted,
                "negative context is separately owned and cannot mutate the authentic frozen versions");
          }
          if (present || scenario != Scenario::MissingRecordContext) {
            for (std::uint32_t record = 0u; record < imported.records.size(); ++record)
              check(oracle->records.bind({general.tape.event(event.eventIndex).header.ordinal, record}, context) ==
                        ContextStatus::Accepted,
                  "every original canonical locator binds its immutable context before dispatch/publication");
          }
          if (present) {
            const auto writing = Access::writingContinuation(fixture.routing->queue_);
            check(writing && writing->commandCount == 1u &&
                      execution.ownsWritingContinuation({writing->writer, writing->source,
                          writing->commandCount}),
                  "actual root owns draw continuation across both original destroy events");
            check(!runtime::findExactObject(objects.values, test::general_cpu_destroy_fixture_detail::kVertexBuffer) &&
                      !runtime::findExactObject(objects.values, test::general_cpu_destroy_fixture_detail::kIndexBuffer) &&
                      authenticContext->entries().size() == 3u && context->entries().size() == 1u,
                "frozen draw versions retain expected bytes after both original wrapper destroys before natural Present");
            auto wrong = RenderTapeExecutionV4::WritingContinuation{
                writing->writer, writing->source, writing->commandCount};
            ++wrong.writer.generation;
            check(!execution.ownsWritingContinuation(wrong) && !execution.failed(),
                  "nonmatching continuation cannot borrow actual unpublished provenance");
          }
          std::vector<void*> resolved(chunk.envelope.handleCount);
          std::size_t count = 0u;
          check(runtime::resolveCommandHandles(chunk, objects.values, resolved, count) &&
                    execution.bindCommandChunk(chunk.bytes, chunk.envelope,
                        std::span<void* const>(resolved.data(), count)),
                "exact command bytes and acquired wrappers bind before observation");
          auto observation = execution.observation();
          if (present && scenario == Scenario::MissingRawAckObservation)
            observation.rawSettled = nullptr;
          ProviderReplayObservationV4Result result;
          check(replayPrevalidatedResolvedCommandChunkObservedV4(fixture.cDevice.get(),
                    chunk.bytes, chunk.envelope, std::span<void* const>(resolved.data(), count),
                    ProviderReplayPolicy::Compatibility, observation, result) == D3D_OK &&
                    result.captureAccepted && result.qualificationSucceeded &&
                    result.ledgerSettlementObserved && result.localRawOrdinal && !execution.failed(),
                "genuine Compatibility dispatch supplies actual root coverage and renderer raw ACK");
          check(execution.publishedSources(publicationCursor, publications),
                "outside-lock pull obtains genuine candidate publication receipts");
          if (!present) {
            check(publications.empty() && !Access::readyCount(fixture.routing->queue_) &&
                      Access::writingCommandCount(fixture.routing->queue_) == 1u &&
                      evidence.acknowledgedRaws == 1u,
                  "indexed draw ACK leaves an unpublished writer without forcing the captured cut");
          } else {
            check(publications.size() == 1u && publications[0].hasPresent &&
                      publications[0].source.valid() && publications[0].metadata.valid(),
                  "original Present naturally publishes one combined candidate source");
            if (scenario == Scenario::MissingRawAckObservation) {
              check(evidence.acknowledgedRaws == 1u,
                    "renderer ACK without executor settlement callback cannot acknowledge the original raw");
              break; // Original PresentComplete cannot follow an unacknowledged raw.
            }
          }
        } else {
          if (event.type == RenderTapeEventType::ObjectDestroy) {
            const auto& identity = general.destroys[event.index].identity;
            const auto owner = std::find_if(objects.values.begin(), objects.values.end(),
                [&](const auto& value) { return renderTapeIdentityV3SameIdentity(value.identity, identity); });
            check(owner != objects.values.end() && owner->value,
                  "original destroy locates exactly one still-owned provider wrapper");
            runtime::releaseOwnedObject(*owner);
            check(!runtime::findExactObject(objects.values, identity),
                  "actual destroy releases acquisition while identity remains bookkeeping only");
          } else {
            check(event.type == RenderTapeEventType::PresentComplete,
                  "minimal interval has no unimplemented resource/query/control events");
          }
          check(execution.replayedEvent(event.eventIndex), "noncommand event acknowledges its actual effect");
        }
      }
      check(publications.size() == 1u, "one natural publication supplies the exact retirement target");
      const auto& publication = publications[0];
      const dxmt9::test::CpuV4ConsumerIdentity target{
          .queue = evidence.localArm.queue, .captureToken = evidence.localArm.captureToken,
          .captureEpoch = evidence.localArm.captureEpoch, .sequence = publication.metadata.seqId,
          .sourceIndex = publication.source.id.index, .sourceGeneration = publication.source.id.generation,
          .storageFirstPage = publication.source.storage.firstPage,
          .storagePageCount = publication.source.storage.pageCount,
          .storageGeneration = publication.source.storage.generation};
      check(handle->consumer->waitThrough(target, deadline),
            "actual publication target retires through exact submit/acknowledge/FIFO finish");
      RenderTapeControlledCpuV4Evidence cpu;
      if (scenario == Scenario::UnfinishedConsumer) {
        check(!execution.finishControlledCpu(capability, cpu) && execution.failed(),
              "fully retired sources cannot finalize while producer/consumer remain open and unjoined");
        emptyCpuOutput(cpu);
      }
      check(fixture.closeControlledCpuProducer() && handle->consumer->closeProducer() &&
                handle->consumer->stopAndJoin(),
            "producer closes and actual consumer stops/joins without extra publication");
      if (scenario == Scenario::UnacknowledgedJoin) {
        check(!execution.finishControlledCpu(capability, cpu) && execution.failed(),
              "worker join without fixture handle acknowledgment cannot finalize controlled authority");
        emptyCpuOutput(cpu);
      }
      handle->join();
      const auto receipts = handle->consumer->takeReceipts();
      const auto consumer = handle->consumer->snapshot();
      const auto actual = handle->backendState->snapshot();
      check(receipts.size() == 1u && receipts[0].observation &&
                receipts[0].observation->identity == target && receipts[0].submitted &&
                receipts[0].acknowledged && receipts[0].retired && receipts[0].cleanupComplete &&
                receipts[0].observation->commands.size() == 2u &&
                receipts[0].observation->commands[0].kind ==
                    static_cast<std::uint32_t>(MetalCommandKind::DrawRun) &&
                receipts[0].observation->commands[0].drawCount == 1u &&
                receipts[0].observation->commands[1].kind ==
                    static_cast<std::uint32_t>(MetalCommandKind::Present) &&
                !receipts[0].observation->commands[1].drawCount &&
                consumer.workerExited && consumer.cleanupComplete && !consumer.cleanupFailed &&
                consumer.state.retired == 1u && consumer.state.commands == 2u &&
                actual.observedOutsideQueueMutex && actual.borrowReleased && !actual.queueQuarantined &&
                actual.phase == Access::ControlledCpuActualPhase::Retired,
            "owned consumer metadata conserves the genuine grouped draw/Present source and retirement");
      check(!consumer.payloadDigestPresent && !consumer.fullSemanticOracleComplete &&
                !consumer.actualMetal && !consumer.gpuResultOracle && !consumer.policyQualified &&
                !consumer.timingEligible && !Access::residentCount(fixture.routing->queue_) &&
                !Access::controlledCpuQueueStopped(fixture.routing->queue_),
            "metadata retirement is not an immutable resource/GPU/policy/timing oracle or queue poison");
      const bool oracleComplete = oracle->qualified(reference);
      if (oracleNegative(scenario)) {
        check(!oracleComplete && oracle->failure && oracle->sources == 1u && !oracle->successfulSources,
            "authentic metadata retirement alone cannot qualify a failed immutable resource oracle");
        const char* expectedFailure = scenario == Scenario::WrongExpectedByte
            ? "fixture-buffer-physical-bytes-mismatch"
            : scenario == Scenario::MissingRecordContext
                ? "fixture-missing-record-context" : "fixture-stale-resource-version-mapping";
        check(std::strcmp(oracle->failure, expectedFailure) == 0,
            "each negative is detected at its intended context/version/actual-byte boundary");
      } else {
        check(oracleComplete && oracle->records.size() == 2u && oracle->selectedRecords.size() == 2u &&
                  oracle->selectedRecords[0] == dxmt9::render::TapeEmissionRecordLocator{9u, 0u} &&
                  oracle->selectedRecords[1] == dxmt9::render::TapeEmissionRecordLocator{12u, 0u},
            "real grouped source origins select the original Draw/Present immutable contexts and match serial semantics: failure=" +
                std::string(oracle->failure ? oracle->failure : "none") +
                " scenario=" + std::to_string(static_cast<unsigned>(scenario)) +
                " sources=" + std::to_string(oracle->sources) +
                " successful_sources=" + std::to_string(oracle->successfulSources) +
                " locations=" + std::to_string(oracle->locations) +
                " draws=" + std::to_string(oracle->draws) +
                " byte_proofs=" + std::to_string(oracle->bufferProofs) +
                " snapshots=" + std::to_string(oracle->snapshots) +
                " live=" + std::to_string(oracle->liveBindings) +
                " digest=" + std::to_string(oracle->digest) +
                " reference_digest=" + std::to_string(reference.digest));
      }
      if (scenario == Scenario::Complete) {
        check(oracleComplete && execution.finishControlledCpu(capability, cpu) && !execution.failed() &&
                  cpu.domain == RenderTapeControlledCpuV4Evidence::Domain::ControlledCpu &&
                  cpu.controlledCpuComplete && cpu.canonicalTraversalComplete &&
                  cpu.rawAckCoverageComplete && cpu.semanticMappingComplete &&
                  cpu.planDigest == source.digest() && cpu.sourceObligationsRegistered == 2u &&
                  cpu.sourceObligationsQualified == 2u && cpu.semanticMapping && cpu.semanticMapping->valid(),
              "real arm/replay/join finalizes separate owned CPU provenance and semantic correspondence");
        check(cpu.actualIdentity.sources.size() == 1u && cpu.actualIdentity.commands.size() == 2u &&
                  cpu.actualIdentity.settlements.size() == 2u && cpu.actualIdentity.attempts.size() == 2u &&
                  cpu.semanticMapping->records().size() == 2u && cpu.semanticMapping->sources().size() == 2u &&
                  cpu.semanticMapping->obligations().size() == 2u,
              "two original reference sources map to one genuinely published candidate source");
        for (std::uint32_t index = 0u; index < 2u; ++index) {
          const auto& relation = cpu.semanticMapping->sources()[index];
          check(relation.capturedIndex == index && relation.candidateIndex == 0u &&
                    relation.recordCount == 1u && !relation.coordinatorCommandCount &&
                    relation.candidateSequence == target.sequence &&
                    cpu.semanticMapping->obligations()[index].sourceRelationCount == 1u,
                "each original publication obligation is discharged by its actual contributing record");
          check(cpu.actualIdentity.attempts[index].receipt.kind ==
                    static_cast<std::uint32_t>(RenderTapeIdentityV4ReceiptKind::Compatibility),
                "actual provenance witnesses Compatibility rather than test-authored candidate routing");
        }
        check(renderTapeActionWitnessV1MatchesBinding(cpu.actionWitness, evidence.localArm.captureToken,
                  evidence.localArm.captureEpoch, evidence.localArm.queue) &&
                  renderTapeActionWitnessV1StableExact(cpu.actionWitness) &&
                  !acquireRenderTapeRuntimeIdentityV4(fixture.cDevice.get()) &&
                  !fixture.cDevice->renderTapeRuntimeBindingV4 &&
                  !fixture.routing->queue_.acquireTapeRuntimeCapture(),
              "real stable construction Exact witness is owned after exact binding detachment");
        owned = std::move(cpu);
        ownedContext = authenticContext;
        ownedExpectedBytesDigest = expectedBytesDigest(*ownedContext);
      } else {
        if (scenario == Scenario::MissingRawAckObservation) {
          check(!execution.finishControlledCpu(capability, cpu) && execution.failed(),
                "missing executor raw ACK fails original traversal even after genuine consumer join");
          emptyCpuOutput(cpu);
        }
        if (oracleNegative(scenario)) {
          check(execution.finishControlledCpu(capability, cpu) && cpu.controlledCpuComplete &&
                    cpu.semanticMappingComplete && !oracleComplete,
              "CPU metadata/provenance can finish honestly while independent full-fixture semantic qualification rejects");
        }
        execution.cancel();
      }
      check(!evidence.complete && !evidence.actualLifecycleComplete && !evidence.actualTopologyComplete &&
                !evidence.semanticMappingComplete && !evidence.semanticMapping && evidence.qualifierOnly &&
                !evidence.policyQualified && !evidence.queryReadinessQualified && !evidence.pixelsQualified &&
                !evidence.timingEligible && !evidence.presentPreparationAttempted,
            "CPU gate never promotes production GPU/topology/full-resource/pixel/timing evidence");
    }
  }
  check(owned.controlledCpuComplete && owned.actualIdentity.sources.size() == 1u &&
            owned.semanticMapping && owned.semanticMapping->valid() &&
            owned.semanticMapping->records().size() == 2u &&
            renderTapeActionWitnessV1StableExact(owned.actionWitness),
        "successful CPU mapping/provenance/witness outlive fixture, blobs and authenticated input owners");
  check(ownedContext && ownedContext->entries().size() == 3u &&
            expectedBytesDigest(*ownedContext) == ownedExpectedBytesDigest &&
            std::count_if(ownedContext->entries().begin(), ownedContext->entries().end(), [](const auto& version) {
              return version->nativeKind() == D9C_CHUNK_HANDLE_KIND_BUFFER &&
                  !version->expectedBufferBytes().empty() && version->observation().receipts().size() == 1u;
            }) == 2u,
      "immutable CPU seed versions/receipts survive wrapper destruction, fixture teardown and original input owners");
}

// Exercise the executor entrypoint itself on the unchanged small fixture.
// This is metadata/resource-version inventory, not capture29, a source-cut
// equivalence claim, or the independent full semantic oracle above.
void controlledCpuV4ExecutorInventoriesOriginalTape() {
  using namespace dxmt9::d3d9;
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<RuntimeFixture, Access>;
  const auto captured = test::makeGeneralCpuDestroyFixture(false, false);
  std::vector<RenderTapeProviderBlob> blobs;
  for (const auto& bytes : captured.blobs)
    blobs.push_back({RenderTapeCaptureSession::sha256(bytes), bytes});
  runtime::GeneralPreflightPlan general;
  const auto preflight = runtime::buildGeneralReplayPlan(captured.tape, blobs, general);
  check(preflight.complete(), std::string("controlled executor fixture preflight: status=") +
      frameTapeReplayStatusName(preflight.status) + " failed_event=" + std::to_string(preflight.failedEventIndex));
  const auto source = controlledCpuV4ReferencePlan(captured.tape, general);
  std::vector<dxmt9::render::TapeEmissionRecordLocator> originalRecords;
  for (const auto& event : general.events)
    if (event.type == RenderTapeEventType::CommandChunk)
      originalRecords.push_back({general.tape.event(event.eventIndex).header.ordinal, 0u});
  check(general.events.size() == 13u && general.definitions.size() == 5u && general.mutations.size() == 2u &&
            general.destroys.size() == 2u && originalRecords.size() == 2u,
        "direct executor spec retains the original seed, Draw, both destroys and terminal Present events");

  PresentParameters output{};
  output.backBufferWidth = general.outputDesc.width;
  output.backBufferHeight = general.outputDesc.height;
  output.backBufferFormat = devicec::fmtFromD3D(general.outputDesc.format);
  output.backBufferCount = 1u;
  output.windowed = true;
  output.deviceWindow = Handle{91};
  output.presentationInterval = PresentInterval::Immediate;
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE,
        "direct controlled executor inventory requires genuine CPU-visible buffer backing");
  RuntimeFixture fixture(false, false, false, true,
      WMT::Device{resourceDevice.handle}, nullptr, &output, true);
  fixture.routing->recordBoundedCapturedBindings = false;
  Executor executor(fixture, general, blobs);
  Executor::ControlledV4InventoryOptions options;
  options.sourcePlan = &source;
  options.exactTape = captured.tape;
  options.expectedBufferBudget = {1024u * 1024u};
  options.consumerLimits = {.maxSources = 8u, .maxCommands = 8u, .maxLifecycleActions = 32u};
  options.initialSetupLimits = {.maxObjects = 8u, .maxSeeds = 8u,
      .maxSeedBytes = 1024u * 1024u, .maxAllocationBytes = 1024u * 1024u};
  options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  options.maxQueryOraclePolls = 0u;
  const auto result = executor.runControlledV4Inventory(options);
  const auto& consumer = result.consumer;
  const auto& state = consumer.state;
  const auto& provenance = result.provenance;
  const auto& cpu = result.controlledCpu;
  check(result.complete && result.metadataInventoryComplete && result.sourceProgressComplete && result.resetOk &&
            result.runtimeBudgetedAttempt && result.failure.empty(),
        "actual controlled executor completes owned metadata inventory: failure=" + result.failure +
            " failed_event=" + std::to_string(result.failedEvent) + " events=" + std::to_string(result.events) +
            " commands=" + std::to_string(result.commands) + " draws=" + std::to_string(result.draws) +
            " raw_acks=" + std::to_string(provenance.acknowledgedRaws) + " retired=" + std::to_string(state.retired) +
            " consumer_failure=" + std::to_string(static_cast<unsigned>(consumer.failure)) +
            " reset=" + std::to_string(result.resetOk));
  check(result.inputAdmission.accepted() && !result.inputAdmission.counts().queryIssues &&
            result.events == general.events.size() && result.commands == 3u && result.draws == 1u &&
            result.resourcesCreated == general.definitions.size() && result.resourcesReleased == result.resourcesCreated &&
            result.observedMutations == general.mutations.size() && !result.observedTextureUploads &&
            result.recordContexts == 2u && !result.setupSubmissions && !result.setupAcknowledgements &&
            result.bufferVersionsObserved == 2u && !result.bufferVersionsUnavailable &&
            !result.resourceVersionFailure && result.initialSetup.hostCheckpointJoined && !result.initialSetup.owners,
        "executor counters come from original events, genuine object/seed effects and producer version observations");
  check(provenance.requested && provenance.validated && provenance.armed && provenance.actualAttemptsObserved &&
            provenance.canonicalTraversalComplete && provenance.rawAckCoverageComplete &&
            provenance.replayedEvents == 13u && provenance.dispatchedRecords == 2u && provenance.acknowledgedRaws == 2u &&
            cpu.domain == RenderTapeControlledCpuV4Evidence::Domain::ControlledCpu && cpu.controlledCpuComplete &&
            cpu.canonicalTraversalComplete && cpu.rawAckCoverageComplete && cpu.semanticMappingComplete &&
            cpu.planDigest == source.digest() && cpu.sourceObligationsRegistered == 2u && cpu.sourceObligationsQualified == 2u &&
            cpu.semanticMapping && cpu.semanticMapping->valid() && cpu.semanticMapping->records().size() == 2u &&
            cpu.actualIdentity.sources.size() == 1u && cpu.actualIdentity.commands.size() == 2u &&
            cpu.actualIdentity.attempts.size() == 2u && cpu.actualIdentity.settlements.size() == 2u &&
            renderTapeActionWitnessV1StableExact(cpu.actionWitness),
        "executor authenticates original traversal/raw ACKs and the natural grouped candidate's separate CPU provenance");
  check(state.phase == dxmt9::test::CpuV4ConsumerPhase::Stopped && state.producerClosed &&
            state.sourcePhase == dxmt9::test::CpuV4ConsumerSourcePhase::Empty &&
            state.limits == options.consumerLimits && state.observed == 1u && state.submitted == 1u &&
            state.acknowledged == 1u && state.retired == 1u && state.commands == 2u && state.lifecycleActions == 4u &&
            consumer.failure == dxmt9::test::CpuV4ConsumerFailure::None && consumer.workerExited &&
            consumer.cleanupComplete && !consumer.cleanupFailed && consumer.receipts == 1u &&
            result.consumerReceipts.size() == 1u && result.inventory.size() == 1u &&
            result.consumerReceipts[0].submitted && result.consumerReceipts[0].acknowledged &&
            result.consumerReceipts[0].retired && result.consumerReceipts[0].cleanupComplete &&
            result.consumerReceipts[0].observation && result.consumerReceipts[0].observation->commands.size() == 2u,
        "executor's real consumer observes, submits, acknowledges, retires, cleans up and joins within explicit bounds");
  const auto& inventory = result.inventory[0];
  check(inventory.commands.size() == 2u && inventory.origins.size() == 2u && inventory.draws.size() == 1u &&
            inventory.origins[0].origin.record == originalRecords[0] &&
            inventory.origins[1].origin.record == originalRecords[1] &&
            inventory.origins[0].context && inventory.origins[0].context->entries().size() == 3u &&
            inventory.origins[1].context && inventory.origins[1].context->entries().size() == 1u &&
            inventory.draws[0].record == originalRecords[0] && inventory.draws[0].parameters.indexed &&
            inventory.draws[0].parameters.primitiveCount == 1u && inventory.draws[0].streamMask == 1u &&
            inventory.draws[0].streamStrides[0] == 28u && inventory.draws[0].indexBuffer &&
            !inventory.draws[0].userVertexBytes && !inventory.draws[0].userIndexBytes &&
            result.canonicalDrawRecords.size() == 1u && result.canonicalDrawRecords[0].record == originalRecords[0],
        "owned inventory maps the actual grouped payload to unchanged Draw/Present records and immutable versions after both destroys");
  ImportedChunkView originalDraw;
  check(importPrevalidatedCommandChunk(general.commandChunks[0].bytes, general.commandChunks[0].envelope, originalDraw),
        "canonical draw body remains importable after executor reset");
  const auto originalPayload = originalDraw.record(0u).payload;
  check(result.canonicalDrawRecords[0].payload.size() == originalPayload.size() &&
            std::equal(originalPayload.begin(), originalPayload.end(), result.canonicalDrawRecords[0].payload.begin()) &&
            result.expectedBufferBudget.maxRetainedExpectedBufferBytes == options.expectedBufferBudget.maxRetainedExpectedBufferBytes &&
            result.expectedBufferBudget.highWaterExpectedBufferBytes == 90u &&
            result.expectedBufferBudget.chargedExpectedBufferBytes == 90u,
        "executor owns exact original Draw bytes and charges the actual 84-byte vertex plus 6-byte index seed versions");
  check(!result.fullSemanticOracleComplete && !result.actualMetal && !result.gpuResultOracle &&
            !result.policyQualified && !result.timingEligible && !consumer.payloadDigestPresent &&
            !consumer.fullSemanticOracleComplete && !consumer.actualMetal && !consumer.gpuResultOracle &&
            !consumer.policyQualified && !consumer.timingEligible && !result.queryReplay.present &&
            !result.queryReplay.actualMetal && !result.queryReplay.gpuResultOracle &&
            !provenance.complete && !provenance.actualLifecycleComplete && !provenance.actualTopologyComplete &&
            !provenance.semanticMappingComplete && !provenance.semanticMapping && provenance.qualifierOnly &&
            !provenance.policyQualified && !provenance.queryReadinessQualified && !provenance.pixelsQualified &&
            !provenance.timingEligible && !provenance.presentPreparationAttempted &&
            !fixture.cDevice && !fixture.device && !fixture.factory &&
            !Access::readyCount(fixture.routing->queue_) && !Access::pendingCompletionCount(fixture.routing->queue_) &&
            !Access::writingCommandCount(fixture.routing->queue_) && !Access::residentCount(fixture.routing->queue_) &&
            !Access::controlledCpuQueueStopped(fixture.routing->queue_),
        "completed metadata inventory resets the real fixture without full semantics, production GPU/topology/pixels/policy/timing authority");
  std::cout << "controlled CPU V4 executor spec passed: events=" << result.events << " commands=" << result.commands
            << " draws=" << result.draws << " raw_acks=" << provenance.acknowledgedRaws
            << " retired=" << state.retired << " resources_created=" << result.resourcesCreated
            << " resources_released=" << result.resourcesReleased << " record_contexts=" << result.recordContexts << '\n';
}

dxmt9::d3d9::test::GeneralCpuDestroyFixture controlledCpuV4PePresentTape(bool clearBeforeDraw = false) {
  using namespace dxmt9::d3d9;
  auto fixture = test::makeGeneralCpuDestroyFixture(false, false);
  ImportedRenderTapeView original;
  check(importPrevalidatedRenderTape(fixture.tape, original), "authored PE Present seed tape imports");
  // Dedicated authored input: two adjacent authentic raws are the producer
  // contract's prerequisite. The existing destroy fixture stays unchanged.
  RenderTapeBuilder builder;
  std::uint64_t nextOrdinal = 1u, lastCommandOrdinal = 0u;
  bool firstRaw = true;
  for (std::uint32_t index = 0u; index < original.header.eventCount; ++index) {
    const auto event = original.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::ObjectDestroy) continue;
    if (type == RenderTapeEventType::PresentComplete) {
      auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
      RenderTapePresentCompleteHeader fixed{};
      check(payload.size() >= sizeof(fixed) && lastCommandOrdinal,
            "authored PE Present completion keeps its genuine terminal raw");
      std::memcpy(&fixed, payload.data(), sizeof(fixed));
      fixed.presentOrdinal = lastCommandOrdinal;
      std::memcpy(payload.data(), &fixed, sizeof(fixed));
      builder.appendRawEvent(type, payload);
    } else if (type == RenderTapeEventType::CommandChunk && firstRaw && clearBeforeDraw) {
      ImportedChunkView chunk;
      RenderTapeCommandChunkHeader fixed{};
      check(event.payload.size() >= sizeof(fixed), "capacity fixture retains the authentic first raw envelope");
      std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
      check(importPrevalidatedCommandChunk(event.payload.subspan(sizeof(fixed)),
                {fixed.wireVersion, fixed.recordCount, fixed.handleCount}, chunk) && chunk.records.size() == 1u,
            "capacity fixture imports the unchanged indexed Draw record");
      const auto draw = chunk.record(0u);
      const std::array records{clearRecord(), RecordSpec{draw.header.type,
          {draw.payload.begin(), draw.payload.end()}, {draw.handles.begin(), draw.handles.end()}}};
      // Clear has no handles; Draw's original 0-based references and snapshot
      // body stay unchanged. Only this dedicated authored raw gains a Clear.
      const auto wire = makeWireFixture(records);
      check(validateCommandChunk(wire.bytes, wire.envelope).valid(), "capacity fixture authenticates Clear+Draw wire");
      builder.appendCommandChunk(wire.envelope, wire.bytes);
      lastCommandOrdinal = nextOrdinal;
      firstRaw = false;
    } else {
      builder.appendRawEvent(type, event.payload);
      if (type == RenderTapeEventType::CommandChunk) {
        lastCommandOrdinal = nextOrdinal;
        firstRaw = false;
      }
    }
    ++nextOrdinal;
  }
  fixture.tape = builder.seal();
  return fixture;
}

void controlledCpuV4ExecutorPreservesPePresentContract() {
  using namespace dxmt9::d3d9;
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<RuntimeFixture, Access>;
  using Contract = RenderTapeExecutionV4ProducerContract;
  const auto captured = controlledCpuV4PePresentTape();
  std::vector<RenderTapeProviderBlob> blobs;
  for (const auto& bytes : captured.blobs)
    blobs.push_back({RenderTapeCaptureSession::sha256(bytes), bytes});
  runtime::GeneralPreflightPlan general;
  const auto preflight = runtime::buildGeneralReplayPlan(captured.tape, blobs, general);
  check(preflight.complete(), std::string("authored PE Present preflight: status=") +
      frameTapeReplayStatusName(preflight.status) + " failed_event=" + std::to_string(preflight.failedEventIndex));
  const auto source = controlledCpuV4ReferencePlan(captured.tape, general);
  check(general.events.size() == 11u && general.commandChunks.size() == 2u && general.destroys.empty() &&
            general.presentOrdinal == 10u && general.events[8u].type == RenderTapeEventType::CommandChunk &&
            general.events[9u].type == RenderTapeEventType::CommandChunk &&
            source.identity().settlements[0u].eventOrdinal == 9u && source.identity().settlements[1u].eventOrdinal == 10u,
        "PE contract fixture authenticates original Draw9 immediately before Present10 without authored state cuts");
  PresentParameters output{};
  output.backBufferWidth = general.outputDesc.width;
  output.backBufferHeight = general.outputDesc.height;
  output.backBufferFormat = devicec::fmtFromD3D(general.outputDesc.format);
  output.backBufferCount = 1u;
  output.windowed = true;
  output.deviceWindow = Handle{91};
  output.presentationInterval = PresentInterval::Immediate;
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE, "PE contract inventory uses genuine CPU-visible resources");
  const auto optionsFor = [&](Contract contract) {
    Executor::ControlledV4InventoryOptions options;
    options.sourcePlan = &source;
    options.exactTape = captured.tape;
    options.expectedBufferBudget = {1024u * 1024u};
    options.consumerLimits = {.maxSources = 8u, .maxCommands = 8u, .maxLifecycleActions = 32u};
    options.initialSetupLimits = {.maxObjects = 8u, .maxSeeds = 8u,
        .maxSeedBytes = 1024u * 1024u, .maxAllocationBytes = 1024u * 1024u};
    options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    options.maxQueryOraclePolls = 0u;
    options.producerContract = contract;
    return options;
  };
  for (const auto contract : {Contract::None, Contract::PePresentReservationV1}) {
    const bool selected = contract == Contract::PePresentReservationV1;
    RuntimeFixture fixture(false, false, false, true,
        WMT::Device{resourceDevice.handle}, nullptr, &output, true);
    fixture.routing->recordBoundedCapturedBindings = false;
    check(fixture.routing->pool() == &fixture.routing->queue_.pool() && !fixture.routing->supportsGpuReadback(),
        "resource-backed Exact fixture forwards its genuine queue pool without GPU/readback support");
    Executor executor(fixture, general, blobs);
    const auto result = executor.runControlledV4Inventory(optionsFor(contract));
    const auto& provenance = result.provenance;
    const auto& cpu = result.controlledCpu;
    const auto& consumer = result.consumer;
    const auto& state = consumer.state;
    const auto sources = selected ? 2u : 1u;
    check(result.complete && result.metadataInventoryComplete && result.resetOk && result.failure.empty(),
        "PE contract controlled executor finishes: selected=" + std::to_string(selected) + " failure=" + result.failure +
            " failed_event=" + std::to_string(result.failedEvent) + " raw_acks=" + std::to_string(provenance.acknowledgedRaws) +
            " retired=" + std::to_string(state.retired) + " consumer_failure=" +
            std::to_string(static_cast<unsigned>(consumer.failure)) + " reset=" + std::to_string(result.resetOk) +
            " preparation_attempted=" + std::to_string(provenance.presentPreparationAttempted) +
            " preparation_completed=" + std::to_string(provenance.presentPreparationCompleted) +
            " preparation_hresult=" + std::to_string(provenance.presentPreparationHresult) +
            " flush_calls_including_teardown=" + std::to_string(fixture.routing->flushCalls));
    const auto expectValue = [&](std::uint64_t actual, std::uint64_t expected, const char* field) {
      check(actual == expected, std::string("PE contract executor invariant: ") + field +
          " selected=" + std::to_string(selected) + " actual=" + std::to_string(actual) +
          " expected=" + std::to_string(expected));
    };
    expectValue(static_cast<unsigned>(provenance.producerContract), static_cast<unsigned>(contract), "producerContract");
    expectValue(provenance.presentPreparationAttempted, selected, "presentPreparationAttempted");
    expectValue(provenance.presentPreparationCompleted, selected, "presentPreparationCompleted");
    if (selected) {
      expectValue(provenance.presentPreparationHresult, D3D_OK, "presentPreparationHresult");
      expectValue(provenance.presentPreparationEventIndex, 9u, "presentPreparationEventIndex");
    }
    // runControlledV4Inventory releases the core before returning; the real
    // core Device destructor calls upperDevice_->flush() once after the
    // controlled adapter has been removed. Only the selected contract adds
    // the earlier preparation flush; the origin/completion proof below binds
    // that flush to Draw's genuine pre-Present publication, not teardown.
    expectValue(fixture.routing->flushCalls, 1u + std::uint32_t(selected), "flushCallsIncludingTeardown");
    expectValue(bool(fixture.routing->flushHook), false, "flushHookRetained");
    expectValue(result.events, 11u, "events");
    expectValue(result.commands, 3u, "commands");
    expectValue(result.draws, 1u, "draws");
    expectValue(provenance.replayedEvents, 11u, "replayedEvents");
    expectValue(provenance.dispatchedRecords, 2u, "dispatchedRecords");
    expectValue(provenance.acknowledgedRaws, 2u, "acknowledgedRaws");
    expectValue(result.resourcesCreated, 5u, "resourcesCreated");
    expectValue(result.resourcesReleased, 5u, "resourcesReleased");
    expectValue(result.observedMutations, 2u, "observedMutations");
    expectValue(result.recordContexts, 2u, "recordContexts");
    expectValue(result.sourceProgressComplete, true, "sourceProgressComplete");
    expectValue(cpu.controlledCpuComplete, true, "controlledCpuComplete");
    expectValue(cpu.rawAckCoverageComplete, true, "rawAckCoverageComplete");
    expectValue(cpu.semanticMappingComplete, true, "semanticMappingComplete");
    expectValue(cpu.actualIdentity.sources.size(), sources, "actualSources");
    expectValue(cpu.actualIdentity.commands.size(), 2u, "actualCommands");
    expectValue(cpu.actualIdentity.attempts.size(), 2u, "actualAttempts");
    expectValue(cpu.actualIdentity.settlements.size(), 2u, "actualSettlements");
    expectValue(cpu.sourceObligationsRegistered, 2u, "sourceObligationsRegistered");
    expectValue(cpu.sourceObligationsQualified, 2u, "sourceObligationsQualified");
    if (selected) {
      const auto& beforePresent = cpu.actualIdentity.sources[0u].lifecycle;
      const auto presentAttempt = std::find_if(cpu.actualIdentity.attempts.begin(), cpu.actualIdentity.attempts.end(),
          [](const auto& attempt) { return attempt.eventOrdinal == 10u; });
      check(beforePresent.cutEventOrdinal == 9u && beforePresent.cutKind ==
                static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent) && !beforePresent.cutRecord &&
                beforePresent.commandCount == 1u && beforePresent.publicationEdge && beforePresent.submissionEdge &&
                beforePresent.completionEdge && beforePresent.completedSequence >= beforePresent.seqId &&
                presentAttempt != cpu.actualIdentity.attempts.end() &&
                beforePresent.completionEdge < presentAttempt->beginEdge &&
                result.inventory.size() == 2u && result.inventory[0u].commands.size() == 1u &&
                result.inventory[0u].draws.size() == 1u && result.inventory[0u].origins.size() == 1u &&
                result.inventory[0u].origins[0u].origin.record == dxmt9::render::TapeEmissionRecordLocator{9u, 0u} &&
                result.inventory[1u].commands.size() == 1u && result.inventory[1u].draws.empty() &&
                result.inventory[1u].origins.size() == 1u &&
                result.inventory[1u].origins[0u].origin.record == dxmt9::render::TapeEmissionRecordLocator{10u, 0u},
          "genuine pre-Present flush publishes Draw at its retained AfterEvent cut and completes before the original Present attempt");
      const auto& drawContext = result.inventory[0u].origins[0u].context;
      const auto& presentContext = result.inventory[1u].origins[0u].context;
      check(drawContext && presentContext && drawContext == presentContext && drawContext->entries().size() == 3u &&
                result.bufferVersions.size() == 2u,
          "unchanged original Draw/Present records retain the same immutable historical resource inventory after reset");
      std::size_t historicalBuffers = 0u;
      for (const auto& version : drawContext->entries()) {
        if (version->nativeKind() != D9C_CHUNK_HANDLE_KIND_BUFFER) continue;
        ++historicalBuffers;
        const auto& facts = version->localBufferFacts();
        const auto observed = std::find_if(result.bufferVersions.begin(), result.bufferVersions.end(), [&](const auto& item) {
          return renderTapeIdentityV3SameIdentity(item.identity, version->identity()) &&
              item.logicalContentEpoch == version->contentEpoch();
        });
        // The authentic seed upload advanced local revision from 1 to 2;
        // first immutable logical normalization independently chose epoch 1.
        // Preserve both named facts; revision equality is never authority for
        // the independently established logical epoch or future backing bytes.
        check(facts && facts->observed() && version->contentEpoch() == 1u && facts->contentRevision == 2u &&
                  facts->contentRevision != version->contentEpoch() && facts->buffer == version->nativeHandle() &&
                  facts->byteSize == version->expectedBufferBytes().size() && !facts->hasVersionedBacking &&
                  facts->currentBacking.metalHandle && observed != result.bufferVersions.end() &&
                  facts->status == observed->observation.status && facts->buffer == observed->observation.buffer &&
                  facts->byteSize == observed->observation.byteSize &&
                  facts->contentRevision == observed->observation.contentRevision &&
                  facts->hasVersionedBacking == observed->observation.hasVersionedBacking &&
                  facts->currentBacking.metalHandle == observed->observation.currentBacking.metalHandle &&
                  facts->currentBacking.byteSize == observed->observation.currentBacking.byteSize,
            "original record-owned buffer versions preserve optional local backing facts distinctly from logical epoch after reset");
      }
      check(historicalBuffers == 2u, "both authentic seeded buffers retain their immutable producer-returned local facts");
      const auto& actualDraw = result.inventory[0u].draws[0u];
      const auto& sourceRows = actualDraw.sourceRows;
      check(actualDraw.record == dxmt9::render::TapeEmissionRecordLocator{9u, 0u} &&
                actualDraw.physicalCommandIndex == 0u && actualDraw.subdrawIndex == 0u &&
                result.canonicalDrawRecords.size() == 1u && result.canonicalDrawRecords[0u].record == actualDraw.record &&
                sourceRows.status == dxmt9::test::CpuTapeSourceObservationStatus::Qualified && sourceRows.qualified() &&
                sourceRows.sourceSequence == result.inventory[0u].publication.metadata.seqId &&
                sourceRows.sourceSequence == beforePresent.seqId && sourceRows.completedSequence < sourceRows.sourceSequence &&
                sourceRows.effectiveIndexed && sourceRows.sourceBase == 0u && sourceRows.sourceStride == 28u &&
                sourceRows.sourceByteSize == 84u && sourceRows.plannerStatus &&
                *sourceRows.plannerStatus == dxmt9::test::CpuTapeSourceRowsStatus::Qualified &&
                sourceRows.qualifiedPrimitiveCount == 1u && sourceRows.sourceElementCount == 3u &&
                sourceRows.uniqueVertexRows == 3u && sourceRows.vertexRangeCount == 1u &&
                sourceRows.indexRange && sourceRows.indexRange->byteOffset == 0u && sourceRows.indexRange->byteSize == 6u &&
                sourceRows.plannedReadBytes == 90u && sourceRows.readProof && sourceRows.readProof->proved() &&
                sourceRows.readProof->provedRequests == 2u && sourceRows.readProof->provedRanges == 2u &&
                sourceRows.readProof->provedBytes == 90u,
          "actual authenticated Draw source owns a qualified instantaneous IB/VB row proof: status=" +
              std::to_string(static_cast<unsigned>(sourceRows.status)) + " source_sequence=" +
              std::to_string(sourceRows.sourceSequence) + " completed_sequence=" + std::to_string(sourceRows.completedSequence) +
              " primitive_count=" + std::to_string(sourceRows.qualifiedPrimitiveCount) +
              " source_elements=" + std::to_string(sourceRows.sourceElementCount) +
              " unique_rows=" + std::to_string(sourceRows.uniqueVertexRows) +
              " vertex_ranges=" + std::to_string(sourceRows.vertexRangeCount) +
              " planned_bytes=" + std::to_string(sourceRows.plannedReadBytes) +
              " proof_status=" + (sourceRows.readProof ? std::to_string(static_cast<unsigned>(sourceRows.readProof->status)) : "absent"));
    } else {
      check(result.inventory.size() == 1u && result.inventory[0u].commands.size() == 2u &&
                result.inventory[0u].draws.size() == 1u && result.inventory[0u].origins.size() == 2u &&
                cpu.actualIdentity.sources[0u].lifecycle.commandCount == 2u,
          "NONE keeps its natural grouped Draw/Present publication without inferred PE preparation");
    }
    check(state.phase == dxmt9::test::CpuV4ConsumerPhase::Stopped && state.producerClosed &&
              state.observed == sources && state.submitted == sources && state.acknowledged == sources &&
              state.retired == sources && state.commands == 2u && state.lifecycleActions == 4u * sources &&
              consumer.workerExited && consumer.cleanupComplete && !consumer.cleanupFailed &&
              consumer.failure == dxmt9::test::CpuV4ConsumerFailure::None && result.consumerReceipts.size() == sources &&
              std::all_of(result.consumerReceipts.begin(), result.consumerReceipts.end(), [](const auto& receipt) {
                return receipt.observation && receipt.submitted && receipt.acknowledged && receipt.retired && receipt.cleanupComplete;
              }) && !Access::residentCount(fixture.routing->queue_) && !Access::controlledCpuQueueStopped(fixture.routing->queue_),
        "only the actual consumer submits/acknowledges/retires both genuine publications and joins before clean reset");
    check(!result.fullSemanticOracleComplete && !result.actualMetal && !result.gpuResultOracle &&
              !result.policyQualified && !result.timingEligible && !consumer.payloadDigestPresent &&
              !consumer.fullSemanticOracleComplete && !consumer.actualMetal && !consumer.gpuResultOracle &&
              !consumer.policyQualified && !consumer.timingEligible && !provenance.complete &&
              !provenance.actualLifecycleComplete && !provenance.actualTopologyComplete && !provenance.semanticMappingComplete &&
              !provenance.policyQualified && !provenance.queryReadinessQualified && !provenance.pixelsQualified &&
              !provenance.timingEligible && provenance.qualifierOnly && renderTapeActionWitnessV1StableExact(cpu.actionWitness),
        "real PE preparation changes no Exact profile or metadata-only full semantics/GPU/pixel/policy/timing non-claims");
    std::cout << "controlled CPU V4 PE Present executor spec passed: selected=" << selected
              << " events=" << result.events << " raw_acks=" << provenance.acknowledgedRaws
              << " flushes=" << fixture.routing->flushCalls << " retired=" << state.retired << '\n';
  }
  {
    RuntimeFixture fixture(false, false, false, true,
        WMT::Device{resourceDevice.handle}, nullptr, &output, true);
    std::size_t hookCalls = 0u;
    fixture.routing->flushHook = [&] { ++hookCalls; };
    Executor executor(fixture, general, blobs);
    const auto result = executor.runControlledV4Inventory(optionsFor(Contract::PePresentReservationV1));
    check(!result.complete && result.failure == "controlled-v4-inventory-flush-adapter-preexisting" &&
              result.failedEvent == 0xffffffffu && !result.provenance.requested && !result.provenance.armed &&
              !result.resourcesCreated && !result.observedMutations && !result.recordContexts &&
              !result.provenance.acknowledgedRaws && result.inventory.empty() && !result.consumer.state.observed &&
              !fixture.routing->flushCalls && !hookCalls && fixture.routing->flushHook &&
              !Access::residentCount(fixture.routing->queue_) && !Access::controlledCpuQueueStopped(fixture.routing->queue_),
        "preexisting flush ownership rejects the PE executor before effects and is never replaced or invoked");
    fixture.routing->flushHook = {};
  }
  {
    const auto capacityTape = controlledCpuV4PePresentTape(true);
    std::vector<RenderTapeProviderBlob> capacityBlobs;
    for (const auto& bytes : capacityTape.blobs)
      capacityBlobs.push_back({RenderTapeCaptureSession::sha256(bytes), bytes});
    runtime::GeneralPreflightPlan capacityGeneral;
    check(runtime::buildGeneralReplayPlan(capacityTape.tape, capacityBlobs, capacityGeneral).complete(),
        "dedicated pre-Present capacity fixture passes authenticated General preflight");
    const auto capacitySource = controlledCpuV4ReferencePlan(capacityTape.tape, capacityGeneral);
    check(capacityGeneral.commandChunks.size() == 2u && capacityGeneral.commandChunks[0u].envelope.recordCount == 2u &&
              capacityGeneral.commandChunks[1u].envelope.recordCount == 1u &&
              capacitySource.identity().sources[0u].lifecycle.commandCount == 2u,
        "first authentic flush source contains Clear+Draw, exceeding the explicit one-command consumer budget");
    RuntimeFixture fixture(false, false, false, true,
        WMT::Device{resourceDevice.handle}, nullptr, &output, true);
    fixture.routing->recordBoundedCapturedBindings = false;
    Executor executor(fixture, capacityGeneral, capacityBlobs);
    auto options = optionsFor(Contract::PePresentReservationV1);
    options.sourcePlan = &capacitySource;
    options.exactTape = capacityTape.tape;
    options.consumerLimits = {.maxSources = 1u, .maxCommands = 1u, .maxLifecycleActions = 4u};
    const auto result = executor.runControlledV4Inventory(options);
    const auto& provenance = result.provenance;
    const auto& consumer = result.consumer;
    const auto& state = consumer.state;
    check(!result.complete && !result.metadataInventoryComplete && !result.sourceProgressComplete && !result.resetOk &&
              result.failure == "v4-present-preparation-failed" && result.failedEvent == 9u &&
              provenance.armed && provenance.presentPreparationAttempted && !provenance.presentPreparationCompleted &&
              provenance.presentPreparationHresult != D3D_OK &&
              consumer.failure == dxmt9::test::CpuV4ConsumerFailure::Capacity && consumer.workerExited &&
              state.phase == dxmt9::test::CpuV4ConsumerPhase::Failed,
        "real pre-Present flush capacity failure poisons and wakes preparation: failure=" + result.failure +
            " failed_event=" + std::to_string(result.failedEvent) + " preparation_hresult=" +
            std::to_string(provenance.presentPreparationHresult) + " flushes=" + std::to_string(fixture.routing->flushCalls) +
            " consumer_failure=" + std::to_string(static_cast<unsigned>(consumer.failure)) +
            " worker_exited=" + std::to_string(consumer.workerExited) + " reset=" + std::to_string(result.resetOk));
    check(provenance.acknowledgedRaws == 1u && provenance.dispatchedRecords == 2u &&
              provenance.actualIdentity.attempts.size() == 1u && provenance.actualIdentity.attempts[0u].eventOrdinal == 9u &&
              provenance.actualIdentity.settlements.size() == 1u && provenance.actualIdentity.settlements[0u].eventOrdinal == 9u &&
              fixture.routing->clearCalls.load() == 1u && fixture.routing->drawCalls.load() == 1u &&
              !fixture.routing->presentCalls.load() && result.commands == 3u && result.draws == 1u && result.recordContexts == 2u &&
              !provenance.rawAckCoverageComplete && !provenance.canonicalTraversalComplete,
        "the genuine preceding Clear+Draw raw ACKs once; failed preparation never starts, dispatches or ACKs original Present");
    check(!state.observed && !state.submitted && !state.acknowledged && !state.retired && !state.commands &&
              !state.lifecycleActions && result.inventory.empty() && result.consumerReceipts.empty() &&
              consumer.cleanupComplete && !consumer.cleanupFailed &&
              Access::readyCount(fixture.routing->queue_) == 1u && Access::residentCount(fixture.routing->queue_) == 1u &&
              !Access::pendingCompletionCount(fixture.routing->queue_) &&
              Access::controlledCpuQueueStopped(fixture.routing->queue_) &&
              fixture.routing->flushCalls == 2u && !fixture.routing->flushHook &&
              !fixture.cDevice && !fixture.device && !fixture.factory,
        "capacity rejection claims no source: joined adapter cleanup is complete but the real Ready source stays quarantined without synthetic retirement/reset");
    check(result.controlledCpu.domain == RenderTapeControlledCpuV4Evidence::Domain::None &&
              !result.controlledCpu.controlledCpuComplete && !result.controlledCpu.semanticMappingComplete &&
              result.controlledCpu.actualIdentity.sources.empty() && result.controlledCpu.actionWitness == RenderTapeActionWitnessV1{} &&
              !result.fullSemanticOracleComplete && !result.actualMetal && !result.gpuResultOracle &&
              !result.policyQualified && !result.timingEligible && !consumer.actualMetal && !consumer.gpuResultOracle &&
              !consumer.fullSemanticOracleComplete && !consumer.policyQualified && !consumer.timingEligible &&
              !provenance.complete && !provenance.actualLifecycleComplete && !provenance.actualTopologyComplete &&
              !provenance.policyQualified && !provenance.pixelsQualified && !provenance.timingEligible,
        "failed preparation exposes no CPU finalization or semantics/GPU/pixel/policy/timing authority");
    std::cout << "controlled CPU V4 PE Present capacity failure spec passed: raw_acks=" << provenance.acknowledgedRaws
              << " present_calls=" << fixture.routing->presentCalls.load() << " consumer_failure="
              << static_cast<unsigned>(consumer.failure) << " quarantined_ready=" << Access::readyCount(fixture.routing->queue_)
              << " reset=" << result.resetOk << '\n';
  }
}

void controlledCpuV4ExecutorHonorsRequestedRoute() {
  using namespace dxmt9::d3d9;
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<RuntimeFixture, Access>;
  using Policy = ProviderReplayPolicy;
  check(dxmt9::perf::enabled(), "route spec pins existing perf counters before any fixture or cached use");
  const auto captured = controlledCpuV4PePresentTape();
  std::vector<RenderTapeProviderBlob> blobs;
  for (const auto& bytes : captured.blobs)
    blobs.push_back({RenderTapeCaptureSession::sha256(bytes), bytes});
  runtime::GeneralPreflightPlan general;
  const auto preflight = runtime::buildGeneralReplayPlan(captured.tape, blobs, general);
  check(preflight.complete(), std::string("controlled route fixture preflight: status=") +
      frameTapeReplayStatusName(preflight.status) + " failed_event=" + std::to_string(preflight.failedEventIndex));
  const auto source = controlledCpuV4ReferencePlan(captured.tape, general);
  check(general.events.size() == 11u && general.commandChunks.size() == 2u &&
            general.presentOrdinal == 10u && source.identity().settlements.size() == 2u &&
            source.identity().settlements[0].eventOrdinal == 9u &&
            source.identity().settlements[1].eventOrdinal == 10u,
        "route test retains the authored original PE Draw9/Present10 tape and authenticated source obligations");
  PresentParameters output{};
  output.backBufferWidth = general.outputDesc.width;
  output.backBufferHeight = general.outputDesc.height;
  output.backBufferFormat = devicec::fmtFromD3D(general.outputDesc.format);
  output.backBufferCount = 1u;
  output.windowed = true;
  output.deviceWindow = Handle{91};
  output.presentationInterval = PresentInterval::Immediate;
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE,
        "controlled route test owns genuine CPU-visible buffer backing without GPU execution");
  const auto optionsFor = [&](Policy route, bool logical) {
    Executor::ControlledV4InventoryOptions options;
    options.route = route;
    options.sourcePlan = &source;
    options.exactTape = captured.tape;
    options.expectedBufferBudget = {1024u * 1024u};
    options.consumerLimits = {.maxSources = 8u, .maxCommands = 8u, .maxLifecycleActions = 32u};
    options.initialSetupLimits = {.maxObjects = 8u, .maxSeeds = 8u,
        .maxSeedBytes = 1024u * 1024u, .maxAllocationBytes = 1024u * 1024u};
    options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    options.maxQueryOraclePolls = 0u;
    options.producerContract = RenderTapeExecutionV4ProducerContract::PePresentReservationV1;
    options.observeLogicalSemantics = logical;
    options.legalCandidateNoOverwriteContract = logical;
    options.freshNoAutoDepthSurfaceDefaults = !output.enableAutoDepthStencil;
    return options;
  };
  struct RejectedRoute {
    Policy requested;
    bool direct, arena;
    const char* failure;
  };
  const std::array rejectedRoutes{
      RejectedRoute{Policy::Compatibility, true, true, "controlled-v4-inventory-route-contract"},
      RejectedRoute{Policy::Compatibility, false, false, "controlled-v4-inventory-route-contract"},
      RejectedRoute{Policy::ProductionSlot, false, false, "controlled-v4-inventory-route-contract"},
      RejectedRoute{Policy::ProductionSlot, true, true, "controlled-v4-inventory-route-contract"},
      RejectedRoute{static_cast<Policy>(0xffu), false, true, "controlled-v4-inventory-route-policy"}};
  for (const auto& rejectedRoute : rejectedRoutes) {
    RuntimeFixture fixture(false, false, rejectedRoute.direct, rejectedRoute.arena,
        WMT::Device{resourceDevice.handle}, nullptr, &output, true);
    Executor executor(fixture, general, blobs);
    const auto result = executor.runControlledV4Inventory(optionsFor(rejectedRoute.requested, true));
    check(!result.complete && !result.metadataInventoryComplete && !result.sourceProgressComplete &&
              !result.resetOk && result.failure == rejectedRoute.failure && result.failedEvent == 0xffffffffu &&
              !result.provenance.requested && !result.provenance.armed &&
              result.controlledCpu.domain == RenderTapeControlledCpuV4Evidence::Domain::None &&
              !result.resourcesCreated && !result.resourcesReleased && !result.observedMutations &&
              !result.commands && !result.draws && !result.recordContexts && !result.setupSubmissions &&
              !result.setupAcknowledgements && result.inventory.empty() && result.consumerReceipts.empty(),
          "requested route/support mismatch or unknown policy rejects before original replay effects: failure=" + result.failure);
    check(fixture.cDevice && fixture.device && fixture.factory &&
              !fixture.cDevice->renderTapeRuntimeIdentityV4 && !fixture.routing->flushHook &&
              !fixture.routing->drawCalls && !fixture.routing->clearCalls && !fixture.routing->presentCalls &&
              !fixture.routing->flushCalls && !fixture.routing->captureCalls && !fixture.routing->mutationApplies &&
              !Access::readyCount(fixture.routing->queue_) && !Access::writingCommandCount(fixture.routing->queue_) &&
              !Access::pendingCompletionCount(fixture.routing->queue_) && !Access::residentCount(fixture.routing->queue_) &&
              !Access::controlledCpuQueueStopped(fixture.routing->queue_),
          "pre-effect route rejection keeps the genuine fixture unarmed, unsubmitted and available to its owner");
    const auto& route = result.routeCoverage;
    const auto& logical = result.logicalSemantics;
    check(!route.policySelected && !route.complete && !route.directLeases && !route.directDraws &&
              !route.compatibilityDraws && !route.compatibilityBatches && !route.compatibilityUnbatchedDraws &&
              !route.fallbackDraws && logical.requested && !logical.accepted && !logical.complete &&
              !logical.logicalStreamComplete && !logical.fullLogicalSemanticOracleComplete &&
              !logical.originalSemanticEquivalenceQualified && !logical.crossPolicyEquivalenceQualified,
          "failed route admission cannot mint route coverage or logical/equivalence capabilities");
  }
  for (const auto requested : {Policy::Compatibility, Policy::ProductionSlot}) {
    const bool production = requested == Policy::ProductionSlot;
    for (const bool logicalRequested : {false, true}) {
      // These are the exact harness support contracts. Production keeps the
      // ordinary Direct ChunkSlot path; no Arena/Direct fallback is forced.
      RuntimeFixture fixture(false, false, production, !production,
          WMT::Device{resourceDevice.handle}, nullptr, &output, true);
      fixture.routing->recordBoundedCapturedBindings = false;
      check(fixture.routing->supportsDirectChunkSlotReplay() == production &&
                fixture.routing->supportsCpuReadyArenaReplay() == !production &&
                fixture.routing->pool() == &fixture.routing->queue_.pool() && !fixture.routing->supportsGpuReadback(),
          "requested route matches both actual fixture supports and the genuine resource-backed CPU domain");
      Executor executor(fixture, general, blobs);
      const auto result = executor.runControlledV4Inventory(optionsFor(requested, logicalRequested));
      const auto& provenance = result.provenance;
      const auto& cpu = result.controlledCpu;
      const auto& consumer = result.consumer;
      const auto& state = consumer.state;
      const auto& route = result.routeCoverage;
      const auto& logical = result.logicalSemantics;
      check(result.complete && result.metadataInventoryComplete && result.sourceProgressComplete && result.resetOk &&
                result.failure.empty() && result.events == 11u && result.commands == 3u && result.draws == 1u &&
                result.resourcesCreated == 5u && result.resourcesReleased == 5u && result.observedMutations == 2u &&
                result.recordContexts == 2u && !result.setupSubmissions && !result.setupAcknowledgements,
          "both genuine replay routes complete the unchanged bounded original tape: route=" +
              std::to_string(static_cast<unsigned>(requested)) + " logical=" + std::to_string(logicalRequested) +
              " failure=" + result.failure + " direct=" + std::to_string(route.directDraws) +
              " compatibility=" + std::to_string(route.compatibilityDraws));
      check(route.requestedRoute == requested && route.effectiveRoute == requested && route.policySelected &&
                route.observed && route.monotonic && route.complete &&
                route.directDraws + route.compatibilityDraws == result.draws &&
                (production ? route.directLeases && route.directDraws == result.draws &&
                    !route.compatibilityDraws && !route.fallbackDraws : !route.directDraws && !route.directLeases &&
                    route.compatibilityDraws == result.draws),
          "actual production replay deltas cover every draw; eligible Direct is exercised without relabeling Compatibility");
      check(provenance.requested && provenance.validated && provenance.armed && provenance.actualAttemptsObserved &&
                provenance.canonicalTraversalComplete && provenance.rawAckCoverageComplete &&
                provenance.replayedEvents == 11u && provenance.dispatchedRecords == 2u &&
                provenance.acknowledgedRaws == 2u && provenance.presentPreparationAttempted &&
                provenance.presentPreparationCompleted && provenance.presentPreparationHresult == D3D_OK &&
                cpu.domain == RenderTapeControlledCpuV4Evidence::Domain::ControlledCpu && cpu.controlledCpuComplete &&
                cpu.canonicalTraversalComplete && cpu.rawAckCoverageComplete && cpu.semanticMappingComplete &&
                cpu.planDigest == source.digest() && cpu.sourceObligationsRegistered == 2u &&
                cpu.sourceObligationsQualified == 2u && cpu.semanticMapping && cpu.semanticMapping->valid() &&
                cpu.actualIdentity.sources.size() == 2u && cpu.actualIdentity.commands.size() == 2u &&
                cpu.actualIdentity.attempts.size() == 2u && cpu.actualIdentity.settlements.size() == 2u &&
                renderTapeActionWitnessV1StableExact(cpu.actionWitness),
          "requested route preserves authentic canonical traversal, actual raw ACKs and separate controlled provenance");
      check(cpu.actualIdentity.attempts[0].eventOrdinal == 9u && cpu.actualIdentity.attempts[1].eventOrdinal == 10u &&
                cpu.actualIdentity.settlements[0].eventOrdinal == 9u && cpu.actualIdentity.settlements[1].eventOrdinal == 10u &&
                cpu.actualIdentity.settlements[0].rawOrdinal == cpu.actualIdentity.attempts[0].rawOrdinal &&
                cpu.actualIdentity.settlements[1].rawOrdinal == cpu.actualIdentity.attempts[1].rawOrdinal &&
                cpu.actualIdentity.attempts[0].receipt.kind == static_cast<std::uint32_t>(production
                    ? RenderTapeIdentityV4ReceiptKind::DirectChunkSlot : RenderTapeIdentityV4ReceiptKind::Compatibility),
          "actual Draw route receipt and raw settlements remain joined to their unchanged original events");
      check(result.inventory.size() == 2u && result.inventory[0].commands.size() == 1u &&
                result.inventory[1].commands.size() == 1u && result.inventory[0].draws.size() == 1u &&
                result.inventory[1].draws.empty() && result.inventory[0].origins.size() == 1u &&
                result.inventory[1].origins.size() == 1u &&
                result.inventory[0].origins[0].origin.record == dxmt9::render::TapeEmissionRecordLocator{9u, 0u} &&
                result.inventory[1].origins[0].origin.record == dxmt9::render::TapeEmissionRecordLocator{10u, 0u} &&
                result.inventory[0].publication.metadata.seqId < result.inventory[1].publication.metadata.seqId &&
                result.inventory[0].draws[0].sourceRows.qualified() &&
                cpu.actualIdentity.sources[0].lifecycle.completionEdge < cpu.actualIdentity.attempts[1].beginEdge,
          "real PE preparation and consumer preserve original Draw-before-Present order and qualified source-row proof");
      check(state.phase == dxmt9::test::CpuV4ConsumerPhase::Stopped && state.producerClosed &&
                state.sourcePhase == dxmt9::test::CpuV4ConsumerSourcePhase::Empty && state.observed == 2u &&
                state.submitted == 2u && state.acknowledged == 2u && state.retired == 2u && state.commands == 2u &&
                state.lifecycleActions == 8u && consumer.workerExited && consumer.cleanupComplete &&
                !consumer.cleanupFailed && consumer.failure == dxmt9::test::CpuV4ConsumerFailure::None &&
                result.consumerReceipts.size() == 2u &&
                std::all_of(result.consumerReceipts.begin(), result.consumerReceipts.end(), [](const auto& receipt) {
                  return receipt.observation && receipt.submitted && receipt.acknowledged && receipt.retired && receipt.cleanupComplete;
                }), "the actual controlled consumer submits, acknowledges, retires and joins both route publications");
      check(logical.requested == logicalRequested && logical.logicalStreamComplete == logicalRequested &&
                logical.accepted == logicalRequested && logical.complete == logicalRequested &&
                logical.receipts.size() == (logicalRequested ? 2u : 0u) &&
                (!logicalRequested || (logical.sourceRowsRequired == 1u && logical.sourceRowsQualified == 1u &&
                    logical.conditionalOnLegalNoOverwrite && logical.legalCandidateNoOverwriteContract)) &&
                !logical.fullLogicalSemanticOracleComplete && !logical.originalSemanticEquivalenceQualified &&
                !logical.crossPolicyEquivalenceQualified && !logical.gpuResultOracle && !logical.policyQualified &&
                !logical.timingEligible && !logical.futurePreservationQualified && !logical.fullShaderFetchCoverageQualified &&
                !result.fullSemanticOracleComplete && !result.actualMetal && !result.gpuResultOracle &&
                !result.policyQualified && !result.timingEligible && !consumer.payloadDigestPresent &&
                !consumer.fullSemanticOracleComplete && !consumer.actualMetal && !consumer.gpuResultOracle &&
                !consumer.policyQualified && !consumer.timingEligible && !provenance.complete &&
                !provenance.actualLifecycleComplete && !provenance.actualTopologyComplete && !provenance.semanticMappingComplete &&
                provenance.qualifierOnly && !provenance.policyQualified && !provenance.pixelsQualified && !provenance.timingEligible,
          "route coverage preserves metadata versus conditional logical-stream authority without semantic/GPU/policy promotion");
      check(!fixture.cDevice && !fixture.device && !fixture.factory && !fixture.routing->flushHook &&
                !Access::readyCount(fixture.routing->queue_) && !Access::writingCommandCount(fixture.routing->queue_) &&
                !Access::pendingCompletionCount(fixture.routing->queue_) && !Access::residentCount(fixture.routing->queue_) &&
                !Access::controlledCpuQueueStopped(fixture.routing->queue_),
          "both route executions join consumer ownership before clean exact-profile queue and fixture reset");
      std::cout << "controlled CPU V4 route spec passed: route=" << (production ? "production-slot" : "compatibility")
                << " logical=" << logicalRequested << " direct=" << route.directDraws
                << " compatibility=" << route.compatibilityDraws << " raw_acks=" << provenance.acknowledgedRaws
                << " retired=" << state.retired << '\n';
    }
  }
}

// The original query is deliberately an unissued TIMESTAMPDISJOINT: its
// defined constant-false result exercises real GetData/control replay without
// inventing a query END publication, captured coordinator, or GPU sample
// oracle. Absolute TIMESTAMP is excluded by the General preflight contract.
dxmt9::d3d9::test::GeneralCpuDestroyFixture controlledCpuV4ExecuteQueryTape() {
  using namespace dxmt9::d3d9;
  auto fixture = controlledCpuV4PePresentTape();
  ImportedRenderTapeView original;
  check(importPrevalidatedRenderTape(fixture.tape, original), "execution query seed tape imports");
  const D9CWireObjectIdentity queryIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_QUERY, .generation = 7u, .objectId = 71u};
  const RenderTapeQueryDescriptorV2 descriptor{.type = 11u, .dataBytes = 4u};
  const RenderTapeQueryGetDataControlV2 poll{
      .hasOutput = 1u, .definedBytes = 4u, .dataSize = 4u};
  RenderTapeBuilder builder;
  std::uint64_t nextOrdinal = 1u, lastCommandOrdinal = 0u;
  bool polled = false;
  for (std::uint32_t index = 0u; index < original.header.eventCount; ++index) {
    const auto event = original.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::CommandChunk && !polled) {
      builder.appendOrderedControl({.identity = queryIdentity,
          .kind = static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData),
          .disposition = static_cast<std::uint32_t>(RenderTapeControlDisposition::Completed),
          .resultCode = D3D_OK, .controlBytes = sizeof(poll), .completionOrdinal = 1u},
          std::as_bytes(std::span(&poll, 1u)));
      ++nextOrdinal;
      polled = true;
    }
    if (type == RenderTapeEventType::PresentComplete) {
      auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
      RenderTapePresentCompleteHeader fixed{};
      check(payload.size() >= sizeof(fixed) && lastCommandOrdinal,
            "execution query tape retains genuine terminal Present completion");
      std::memcpy(&fixed, payload.data(), sizeof(fixed));
      fixed.presentOrdinal = lastCommandOrdinal;
      std::memcpy(payload.data(), &fixed, sizeof(fixed));
      builder.appendRawEvent(type, payload);
    } else {
      builder.appendRawEvent(type, event.payload);
      if (type == RenderTapeEventType::CommandChunk) lastCommandOrdinal = nextOrdinal;
    }
    ++nextOrdinal;
    if (type == RenderTapeEventType::BootstrapState) {
      builder.appendObjectDefine(queryIdentity,
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
          std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
      ++nextOrdinal;
    }
  }
  check(polled, "execution query tape retains one original pre-issue GetData before adjacent Draw/Present raws");
  fixture.tape = builder.seal();
  return fixture;
}

void controlledCpuV4ExecutorOmitsObservationForExecution() {
  using namespace dxmt9::d3d9;
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<RuntimeFixture, Access>;
  using Policy = ProviderReplayPolicy;
  using Mode = dxmt9::test::CpuGeneralTapeControlledV4ExecutionMode;
  check(dxmt9::perf::enabled(), "execution spec pins genuine replay counters before cached use");
  const auto captured = controlledCpuV4ExecuteQueryTape();
  std::vector<RenderTapeProviderBlob> blobs;
  for (const auto& bytes : captured.blobs)
    blobs.push_back({RenderTapeCaptureSession::sha256(bytes), bytes});
  runtime::GeneralPreflightPlan general;
  const auto preflight = runtime::buildGeneralReplayPlan(captured.tape, blobs, general);
  check(preflight.complete(), std::string("execution query tape preflight: status=") +
      frameTapeReplayStatusName(preflight.status) + " failed_event=" + std::to_string(preflight.failedEventIndex) +
      " query_failure=" + std::to_string(static_cast<unsigned>(preflight.queryReplay.failure)));
  const auto source = controlledCpuV4ReferencePlan(captured.tape, general);
  check(general.events.size() == 13u && general.commandChunks.size() == 2u &&
            general.presentOrdinal == 12u && source.identity().settlements[0].eventOrdinal == 11u &&
            source.identity().settlements[1].eventOrdinal == 12u,
        "execution fixture preserves original query-before-Draw11-before-Present12 order and two real sources");
  PresentParameters output{};
  output.backBufferWidth = general.outputDesc.width;
  output.backBufferHeight = general.outputDesc.height;
  output.backBufferFormat = devicec::fmtFromD3D(general.outputDesc.format);
  output.backBufferCount = 1u;
  output.windowed = true;
  output.deviceWindow = Handle{91};
  output.presentationInterval = PresentInterval::Immediate;
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE,
        "execution test owns genuine CPU-visible provider resources without GPU execution");
  const auto optionsFor = [&](Policy route, Mode mode, bool logical) {
    Executor::ControlledV4InventoryOptions options;
    options.route = route;
    options.executionMode = mode;
    options.sourcePlan = &source;
    options.exactTape = captured.tape;
    // A zero diagnostic image budget must not reject genuine execution.
    options.expectedBufferBudget = {mode == Mode::Observe ? 1024u * 1024u : 0u};
    options.consumerLimits = {.maxSources = 8u, .maxCommands = 8u, .maxLifecycleActions = 32u};
    options.initialSetupLimits = {.maxObjects = 8u, .maxSeeds = 8u,
        .maxSeedBytes = 1024u * 1024u, .maxAllocationBytes = 1024u * 1024u};
    options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    options.maxQueryOraclePolls = 0u;
    options.producerContract = RenderTapeExecutionV4ProducerContract::PePresentReservationV1;
    options.observeLogicalSemantics = logical;
    options.legalCandidateNoOverwriteContract = true;
    options.freshNoAutoDepthSurfaceDefaults = !output.enableAutoDepthStencil;
    return options;
  };
  const auto clockAbsent = [](const auto& clock) {
    return !clock.started && !clock.valid && !clock.complete && !clock.wallNs && !clock.processCpuNs;
  };
  for (const auto mode : {Mode::ExecutionOnly, static_cast<Mode>(0xffu)}) {
    const bool mixed = mode == Mode::ExecutionOnly;
    RuntimeFixture fixture(false, false, false, true,
        WMT::Device{resourceDevice.handle}, nullptr, &output, true);
    Executor executor(fixture, general, blobs);
    const auto result = executor.runControlledV4Inventory(optionsFor(Policy::Compatibility, mode, mixed));
    check(!result.complete && !result.metadataInventoryComplete && !result.sourceProgressComplete &&
              !result.resetOk && result.failedEvent == 0xffffffffu &&
              result.failure == (mixed ? "controlled-v4-execution-observer-contract"
                  : "controlled-v4-inventory-execution-mode") &&
              !result.provenance.requested && !result.provenance.armed &&
              result.controlledCpu.domain == RenderTapeControlledCpuV4Evidence::Domain::None &&
              !result.resourcesCreated && !result.resourcesReleased && !result.observedMutations &&
              !result.commands && !result.draws && !result.recordContexts && !result.setupSubmissions &&
              !result.setupAcknowledgements && result.inventory.empty() && result.consumerReceipts.empty() &&
              !result.executionOnly.complete && !result.executionOnly.lifecycleCoverageComplete &&
              clockAbsent(result.clocks.setup) && clockAbsent(result.clocks.body) && clockAbsent(result.clocks.teardown),
          "mixed observation/execution contract and unknown mode reject before effects or clock scopes: " + result.failure);
    check(fixture.cDevice && fixture.device && fixture.factory &&
              !fixture.cDevice->renderTapeRuntimeIdentityV4 && !fixture.routing->flushHook &&
              !fixture.routing->drawCalls && !fixture.routing->clearCalls && !fixture.routing->presentCalls &&
              !fixture.routing->flushCalls && !fixture.routing->captureCalls && !fixture.routing->mutationApplies &&
              !fixture.routing->controlledQueries && !result.queryReplay.present &&
              !Access::readyCount(fixture.routing->queue_) && !Access::writingCommandCount(fixture.routing->queue_) &&
              !Access::pendingCompletionCount(fixture.routing->queue_) && !Access::residentCount(fixture.routing->queue_) &&
              !Access::controlledCpuQueueStopped(fixture.routing->queue_),
          "pre-effect mode rejection leaves genuine query/resource/device/queue ownership untouched");
  }
  for (const auto requested : {Policy::Compatibility, Policy::ProductionSlot}) {
    const bool production = requested == Policy::ProductionSlot;
    std::array<std::uint32_t, 7u> baselineEffects{};
    const auto run = [&](Mode mode) {
      const bool executionOnly = mode == Mode::ExecutionOnly;
      RuntimeFixture fixture(false, false, production, !production,
          WMT::Device{resourceDevice.handle}, nullptr, &output, true);
      fixture.routing->recordBoundedCapturedBindings = false;
      Executor executor(fixture, general, blobs);
      auto result = executor.runControlledV4Inventory(optionsFor(requested, mode, !executionOnly));
      const auto& provenance = result.provenance;
      const auto& cpu = result.controlledCpu;
      const auto& consumer = result.consumer;
      const auto& state = consumer.state;
      const auto& route = result.routeCoverage;
      const auto& query = result.queryReplay;
      const auto& logical = result.logicalSemantics;
      check(result.complete && result.sourceProgressComplete && result.resetOk && result.failure.empty() &&
                result.executionMode == mode && result.metadataInventoryComplete == !executionOnly &&
                result.events == 13u && result.commands == 3u && result.draws == 1u &&
                result.resourcesCreated == 6u && result.resourcesReleased == 6u && result.observedMutations == 2u &&
                !result.setupSubmissions && !result.setupAcknowledgements,
          "both genuine routes execute unchanged query/resource/Draw/Present effects in both modes: " + result.failure);
      check(provenance.requested && provenance.validated && provenance.armed && provenance.actualAttemptsObserved &&
                provenance.canonicalTraversalComplete && provenance.rawAckCoverageComplete &&
                provenance.replayedEvents == 13u && provenance.dispatchedRecords == 2u &&
                provenance.acknowledgedRaws == 2u && provenance.queryObjectsMapped == 1u &&
                !provenance.queryBaselineExpected && !provenance.queryBaselinesJoined &&
                provenance.presentPreparationAttempted && provenance.presentPreparationCompleted &&
                provenance.presentPreparationHresult == D3D_OK &&
                cpu.domain == RenderTapeControlledCpuV4Evidence::Domain::ControlledCpu && cpu.controlledCpuComplete &&
                cpu.canonicalTraversalComplete && cpu.rawAckCoverageComplete && cpu.semanticMappingComplete &&
                cpu.planDigest == source.digest() && cpu.sourceObligationsRegistered == 2u &&
                cpu.sourceObligationsQualified == 2u && cpu.semanticMapping && cpu.semanticMapping->valid() &&
                cpu.actualIdentity.sources.size() == 2u && cpu.actualIdentity.commands.size() == 2u &&
                cpu.actualIdentity.attempts.size() == 2u && cpu.actualIdentity.settlements.size() == 2u &&
                renderTapeActionWitnessV1StableExact(cpu.actionWitness),
          "observer omission preserves authentic original traversal, query binding, raw ACK and CPU-only provenance");
      check(cpu.semanticMapping->records().size() == 2u,
            "both genuine original raw records remain covered before inspecting their preserved locations");
      for (std::size_t index = 0u; index < 2u; ++index) {
        const auto& attempt = cpu.actualIdentity.attempts[index];
        const auto& settlement = cpu.actualIdentity.settlements[index];
        const auto& record = cpu.semanticMapping->records()[index];
        check(attempt.eventOrdinal == 11u + index && settlement.eventOrdinal == attempt.eventOrdinal &&
                  settlement.rawOrdinal == attempt.rawOrdinal && record.eventOrdinal == attempt.eventOrdinal &&
                  record.recordIndex == 0u && record.recordType == (index == 0u
                      ? D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE : D9C_COMMAND_RECORD_PRESENT) &&
                  record.capturedRawOrdinal == source.identity().settlements[index].rawOrdinal,
              "execution-only retains every original record location and captured/raw settlement join");
      }
      check(cpu.actualIdentity.attempts[0].receipt.kind == static_cast<std::uint32_t>(production
                    ? RenderTapeIdentityV4ReceiptKind::DirectChunkSlot : RenderTapeIdentityV4ReceiptKind::Compatibility) &&
                cpu.actualIdentity.sources[0].lifecycle.completionEdge < cpu.actualIdentity.attempts[1].beginEdge &&
                route.requestedRoute == requested && route.effectiveRoute == requested && route.policySelected &&
                route.observed && route.monotonic && route.complete &&
                (production ? route.directLeases && route.directDraws == 1u &&
                    !route.compatibilityDraws && !route.fallbackDraws : !route.directLeases && !route.directDraws &&
                    route.compatibilityDraws == 1u),
          "genuine route counters and Draw-before-Present completion remain observed without payload inventory");
      check(query.present && query.transcriptComplete && query.oracleComplete &&
                query.queryDefinitions == 1u && !query.issueRecords && !query.replayedIssueRecords &&
                query.recordedPolls == 1u && query.replayedPolls == 1u && query.capturedReady == 1u &&
                query.replayReady == 1u && !query.capturedPending && !query.replayPending &&
                query.requiredResults == 1u && query.matchedResults == 1u && query.readinessRequired == 1u &&
                query.readinessSatisfied == 1u && !query.substitutedResults && !query.supplementalPolls &&
                !query.supplementalDrains && !query.checkpointDefinitions && query.rows.size() == 1u &&
                query.rows[0].eventOrdinal == 10u && !query.rows[0].issueCycle &&
                !query.rows[0].localIssueOrdinal && query.rows[0].capturedResult == D3D_OK &&
                query.rows[0].replayResult == D3D_OK && query.rows[0].oracleResult == D3D_OK &&
                query.rows[0].definedBytes == 4u && query.rows[0].resultMatched &&
                query.rows[0].readinessSatisfied && !query.rows[0].resultSubstituted &&
                query.rows[0].capturedBytes == query.rows[0].replayBytes && !query.actualMetal && !query.gpuResultOracle,
          "execution-only preserves the precise unissued TIMESTAMPDISJOINT GetData/readiness/defined-constant-false obligation");
      check(state.phase == dxmt9::test::CpuV4ConsumerPhase::Stopped && state.producerClosed &&
                state.sourcePhase == dxmt9::test::CpuV4ConsumerSourcePhase::Empty && state.observed == 2u &&
                state.submitted == 2u && state.acknowledged == 2u && state.retired == 2u && state.commands == 2u &&
                state.lifecycleActions == 8u && consumer.workerExited && consumer.cleanupComplete &&
                !consumer.cleanupFailed && consumer.failure == dxmt9::test::CpuV4ConsumerFailure::None &&
                result.consumerReceipts.size() == 2u &&
                std::all_of(result.consumerReceipts.begin(), result.consumerReceipts.end(), [](const auto& receipt) {
                  return receipt.observation && receipt.submitted && receipt.acknowledged && receipt.retired && receipt.cleanupComplete;
                }), "real consumer lifecycle metadata survives omission of the optional payload observer");
      if (executionOnly) {
        const auto& omitted = result.executionOnly;
        const auto& budget = result.expectedBufferBudget;
        const auto clockComplete = [](const auto& clock) {
          return clock.started && clock.valid && clock.complete && clock.wallNs && clock.processCpuNs;
        };
        check(omitted.requested && omitted.complete && omitted.lifecycleCoverageComplete &&
                  omitted.resourceVersionsOmitted && omitted.recordContextsOmitted && omitted.expectedImagesOmitted &&
                  omitted.canonicalDrawCopiesOmitted && omitted.payloadObserverOmitted && omitted.sourceRowReadsOmitted &&
                  omitted.logicalHashingOmitted && omitted.diagnosticMutationReceiptsOmitted &&
                  omitted.capturedBindingImagesOmitted && omitted.uploadByteComparisonsOmitted &&
                  omitted.sources == 2u && omitted.commands == 2u && omitted.draws == 1u &&
                  omitted.submitted == 2u && omitted.acknowledged == 2u && omitted.retired == 2u &&
                  result.inventory.empty() && result.canonicalDrawRecords.empty() && result.bufferVersions.empty() &&
                  !result.resourceVersionFailure && !result.recordContexts && !result.bufferVersionsObserved &&
                  !result.bufferVersionsUnavailable && !budget.maxRetainedExpectedBufferBytes &&
                  !budget.chargedExpectedBufferBytes && !budget.highWaterExpectedBufferBytes &&
                  !logical.requested && !logical.accepted && !logical.complete && !logical.logicalStreamComplete &&
                  logical.receipts.empty() && !logical.sourceRowsRequired && !logical.sourceRowsQualified &&
                  clockComplete(result.clocks.setup) && clockComplete(result.clocks.body) && clockComplete(result.clocks.teardown),
            "execution-only omits heavy diagnostic storage/work while reporting genuine complete wall/process-CPU scopes");
      } else {
        check(!result.executionOnly.requested && !result.executionOnly.complete &&
                  result.inventory.size() == 2u && result.canonicalDrawRecords.size() == 1u &&
                  result.bufferVersions.size() == 2u && result.recordContexts == 2u &&
                  result.expectedBufferBudget.highWaterExpectedBufferBytes && logical.requested && logical.accepted &&
                  logical.complete && logical.logicalStreamComplete && logical.receipts.size() == 2u &&
                  logical.sourceRowsRequired == 1u && logical.sourceRowsQualified == 1u &&
                  clockAbsent(result.clocks.setup) && clockAbsent(result.clocks.body) && clockAbsent(result.clocks.teardown),
            "logical Observe baseline actually executes diagnostic copies, source-row proof and hashing omitted by ExecutionOnly");
      }
      check(!result.fullSemanticOracleComplete && !result.actualMetal && !result.gpuResultOracle &&
                !result.policyQualified && !result.timingEligible && !consumer.payloadDigestPresent &&
                !consumer.fullSemanticOracleComplete && !consumer.actualMetal && !consumer.gpuResultOracle &&
                !consumer.policyQualified && !consumer.timingEligible && !provenance.complete &&
                !provenance.actualLifecycleComplete && !provenance.actualTopologyComplete && !provenance.semanticMappingComplete &&
                provenance.qualifierOnly && !provenance.policyQualified && !provenance.pixelsQualified && !provenance.timingEligible &&
                !logical.fullLogicalSemanticOracleComplete && !logical.originalSemanticEquivalenceQualified &&
                !logical.crossPolicyEquivalenceQualified && !logical.gpuResultOracle && !logical.policyQualified &&
                !logical.timingEligible && !logical.futurePreservationQualified && !logical.fullShaderFetchCoverageQualified,
          "real clocks and observer-light completion grant no original equivalence, GPU, public policy or timing authority");
      const std::array effects{fixture.routing->clearCalls.load(), fixture.routing->drawCalls.load(),
          fixture.routing->presentCalls.load(), fixture.routing->flushCalls, fixture.routing->captureCalls,
          fixture.routing->combinedMarkCalls, fixture.routing->mutationApplies.load()};
      if (!executionOnly) baselineEffects = effects;
      else check(effects == baselineEffects, "observer omission preserves actual provider replay/flush/resource effects");
      check(!fixture.cDevice && !fixture.device && !fixture.factory && !fixture.routing->flushHook &&
                !fixture.routing->recordBoundedCapturedBindings && fixture.routing->capturedBindingObservations.empty() &&
                fixture.routing->textureUploadObservations.empty() && !fixture.routing->textureUploadFailed &&
                !Access::readyCount(fixture.routing->queue_) && !Access::writingCommandCount(fixture.routing->queue_) &&
                !Access::pendingCompletionCount(fixture.routing->queue_) && !Access::residentCount(fixture.routing->queue_) &&
                !Access::controlledCpuQueueStopped(fixture.routing->queue_),
          "both modes join the genuine consumer before resource/device/queue reset without captured byte images");
      return result;
    };
    const auto baseline = run(Mode::Observe);
    const auto executed = run(Mode::ExecutionOnly);
    check(executed.events == baseline.events && executed.commands == baseline.commands && executed.draws == baseline.draws &&
              executed.resourcesCreated == baseline.resourcesCreated && executed.resourcesReleased == baseline.resourcesReleased &&
              executed.observedMutations == baseline.observedMutations &&
              executed.observedTextureUploads == baseline.observedTextureUploads &&
              executed.provenance.replayedEvents == baseline.provenance.replayedEvents &&
              executed.provenance.dispatchedRecords == baseline.provenance.dispatchedRecords &&
              executed.provenance.acknowledgedRaws == baseline.provenance.acknowledgedRaws &&
              executed.routeCoverage.directLeases == baseline.routeCoverage.directLeases &&
              executed.routeCoverage.directDraws == baseline.routeCoverage.directDraws &&
              executed.routeCoverage.compatibilityDraws == baseline.routeCoverage.compatibilityDraws &&
              executed.routeCoverage.compatibilityBatches == baseline.routeCoverage.compatibilityBatches &&
              executed.routeCoverage.compatibilityUnbatchedDraws == baseline.routeCoverage.compatibilityUnbatchedDraws &&
              executed.routeCoverage.fallbackDraws == baseline.routeCoverage.fallbackDraws,
          "ExecutionOnly matches the genuine logical baseline's original effects, provenance counts and requested-route deltas");
    for (std::size_t index = 0u; index < 2u; ++index) {
      const auto& before = *baseline.consumerReceipts[index].observation;
      const auto& after = *executed.consumerReceipts[index].observation;
      check(before.commands == after.commands && before.sourceOrdinal == after.sourceOrdinal,
            "cheap authentic consumer command projection remains unchanged without comparing fresh local identities");
    }
    std::cout << "controlled CPU V4 execute spec passed: route=" << (production ? "production-slot" : "compatibility")
              << " original_query=unissued-timestampdisjoint" << " direct=" << executed.routeCoverage.directDraws
              << " compatibility=" << executed.routeCoverage.compatibilityDraws
              << " raw_acks=" << executed.provenance.acknowledgedRaws
              << " body_wall_ns=" << executed.clocks.body.wallNs
              << " body_process_cpu_ns=" << executed.clocks.body.processCpuNs << '\n';
  }
}

// This authored Tape is sealed once and reused byte-for-byte by every mode
// and policy. Clears separate otherwise identical draws into real physical
// commands; the original EVENT END fixes the prefix before the younger draw.
dxmt9::d3d9::test::GeneralCpuDestroyFixture controlledCpuV4GroupingQueryTape() {
  using namespace dxmt9::d3d9;
  auto fixture = controlledCpuV4PePresentTape();
  ImportedRenderTapeView original;
  check(importPrevalidatedRenderTape(fixture.tape, original), "grouping query seed tape imports");
  const D9CWireObjectIdentity queryIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_QUERY, .generation = 7u, .objectId = 71u};
  const RenderTapeQueryDescriptorV2 descriptor{.type = 8u, .dataBytes = 4u};
  // EVENT GetData supplies readiness but the existing query transcript
  // contract defines no output bytes, even for a successful four-byte poll.
  RenderTapeQueryGetDataControlV2 poll{
      .hasOutput = 1u, .dataSize = 4u, .issueCycle = 1u};
  RenderTapeBuilder builder;
  std::uint64_t nextOrdinal = 1u, presentOrdinal = 0u;
  bool appendedPrefix = false;
  for (std::uint32_t index = 0u; index < original.header.eventCount; ++index) {
    const auto event = original.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::CommandChunk && !appendedPrefix) {
      RenderTapeCommandChunkHeader fixed{};
      check(event.payload.size() >= sizeof(fixed), "grouping prefix retains the original draw envelope");
      std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
      ImportedChunkView chunk;
      check(importPrevalidatedCommandChunk(event.payload.subspan(sizeof(fixed)),
                {fixed.wireVersion, fixed.recordCount, fixed.handleCount}, chunk) &&
                chunk.records.size() == 1u &&
                chunk.records[0].type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
          "grouping prefix uses the authentic seeded indexed draw");
      const auto view = chunk.record(0u);
      const RecordSpec draw{view.header.type, {view.payload.begin(), view.payload.end()},
          {view.handles.begin(), view.handles.end()}};
      auto secondDraw = draw;
      // Only authoring rebases the second copied handle slice. Once sealed,
      // policy changes never rewrite records, handles or original ordinals.
      D9CCommandChunkWireDrawHeader drawHeader{};
      check(secondDraw.payload.size() >= sizeof(drawHeader), "grouping copied draw retains its header");
      std::memcpy(&drawHeader, secondDraw.payload.data(), sizeof(drawHeader));
      for (std::uint32_t sectionIndex = 0u; sectionIndex < drawHeader.sectionCount; ++sectionIndex) {
        const auto offset = static_cast<std::size_t>(drawHeader.sectionTableOffset) +
            sectionIndex * sizeof(D9CCommandChunkWireSectionDesc);
        check(offset <= secondDraw.payload.size() &&
                  sizeof(D9CCommandChunkWireSectionDesc) <= secondDraw.payload.size() - offset,
              "grouping draw section table is bounds checked before authoring");
        D9CCommandChunkWireSectionDesc section{};
        std::memcpy(&section, secondDraw.payload.data() + offset, sizeof(section));
        const auto* field = sectionHandleFieldRule(section.kind);
        if (!field) continue;
        for (std::uint32_t element = 0u; element < section.count; ++element) {
          const auto handleOffset = static_cast<std::size_t>(section.payloadOffset) +
              static_cast<std::size_t>(element) * section.elementSize + field->payloadOffset;
          check(handleOffset <= secondDraw.payload.size() &&
                    sizeof(std::uint32_t) <= secondDraw.payload.size() - handleOffset,
              "grouping draw handle stays inside its authenticated payload");
          std::uint32_t handle = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
          std::memcpy(&handle, secondDraw.payload.data() + handleOffset, sizeof(handle));
          if (handle == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX) continue;
          check(handle < draw.handles.size(), "grouping seed handle names its original one-record slice");
          handle += static_cast<std::uint32_t>(draw.handles.size());
          std::memcpy(secondDraw.payload.data() + handleOffset, &handle, sizeof(handle));
        }
      }
      const D9CCommandChunkWireQueryIssue end{
          .queryHandleIndex = static_cast<std::uint32_t>(draw.handles.size() * 2u), .flags = 1u};
      const D9CCommandChunkWireHandleEntry queryHandle{
          .kind = queryIdentity.kind, .generation = queryIdentity.generation, .objectId = queryIdentity.objectId};
      const auto endBytes = std::as_bytes(std::span(&end, 1u));
      const std::array records{draw, clearRecord(), secondDraw, clearRecord(),
          RecordSpec{D9C_COMMAND_RECORD_QUERY_ISSUE, {endBytes.begin(), endBytes.end()}, {queryHandle}}};
      const auto wire = makeWireFixture(records);
      check(wire.envelope.recordCount == 5u && validateCommandChunk(wire.bytes, wire.envelope).valid(),
          "grouping prefix authenticates four GPU records followed by one real EVENT END");
      builder.appendCommandChunk(wire.envelope, wire.bytes);
      ++nextOrdinal;
      builder.appendOrderedControl({.identity = queryIdentity,
          .kind = static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData),
          .disposition = static_cast<std::uint32_t>(RenderTapeControlDisposition::Completed),
          .resultCode = D3D_OK, .controlBytes = sizeof(poll), .completionOrdinal = 1u},
          std::as_bytes(std::span(&poll, 1u)));
      ++nextOrdinal;
      // The younger draw and terminal Present remain adjacent authentic raws,
      // retaining the genuine PE Present preparation contract unchanged.
      builder.appendRawEvent(type, event.payload);
      ++nextOrdinal;
      appendedPrefix = true;
      continue;
    }
    if (type == RenderTapeEventType::PresentComplete) {
      auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
      RenderTapePresentCompleteHeader fixed{};
      check(payload.size() >= sizeof(fixed) && presentOrdinal,
          "grouping query tape retains the terminal Present completion");
      std::memcpy(&fixed, payload.data(), sizeof(fixed));
      fixed.presentOrdinal = presentOrdinal;
      std::memcpy(payload.data(), &fixed, sizeof(fixed));
      builder.appendRawEvent(type, payload);
    } else {
      builder.appendRawEvent(type, event.payload);
      if (type == RenderTapeEventType::CommandChunk) presentOrdinal = nextOrdinal;
    }
    ++nextOrdinal;
    if (type == RenderTapeEventType::BootstrapState) {
      builder.appendObjectDefine(queryIdentity,
          static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
          std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
      ++nextOrdinal;
    }
  }
  check(appendedPrefix && presentOrdinal, "grouping fixture authors one prefix and one terminal Present");
  fixture.tape = builder.seal();
  return fixture;
}

void controlledCpuV4GroupingPreservesQueryEnd(std::size_t drawCommandLimit) {
  using namespace dxmt9::d3d9;
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<RuntimeFixture, Access>;
  using Mode = dxmt9::test::CpuGeneralTapeControlledV4ExecutionMode;
  const dxmt9::test::RenderTapeV4ReplayPolicy policy{.drawCommandLimit = drawCommandLimit};
  check((drawCommandLimit == 0u || drawCommandLimit == 3u || drawCommandLimit == 64u) &&
            dxmt9::test::renderTapeV4ReplayPolicyValid(policy) && dxmt9::perf::enabled(),
      "grouping spec requires admitted cold Compatibility policy and existing route counters");
  const auto captured = controlledCpuV4GroupingQueryTape();
  const auto originalBytes = captured.tape;
  std::vector<RenderTapeProviderBlob> blobs;
  for (const auto& bytes : captured.blobs)
    blobs.push_back({RenderTapeCaptureSession::sha256(bytes), bytes});
  runtime::GeneralPreflightPlan general;
  const auto preflight = runtime::buildGeneralReplayPlan(captured.tape, blobs, general);
  check(preflight.complete(), std::string("grouping query preflight: status=") +
      frameTapeReplayStatusName(preflight.status) + " failed_event=" + std::to_string(preflight.failedEventIndex) +
      " query_failure=" + std::to_string(static_cast<unsigned>(preflight.queryReplay.failure)));
  const auto source = controlledCpuV4ReferencePlan(captured.tape, general, true);
  const auto grammar = dxmt9::test::admitControlledCpuV4Input(general);
  check(grammar.accepted() && grammar.counts().records == 7u && grammar.counts().draws == 3u &&
            grammar.counts().queryIssues == 1u && grammar.counts().queryPolls == 1u &&
            general.events.size() == 14u && general.commandChunks.size() == 3u &&
            general.presentOrdinal == 13u && source.identity().coordinators.size() == 1u,
      "unchanged grouping Tape authenticates real END, required-ready poll and younger Draw/Present");
  PresentParameters output{};
  output.backBufferWidth = general.outputDesc.width;
  output.backBufferHeight = general.outputDesc.height;
  output.backBufferFormat = devicec::fmtFromD3D(general.outputDesc.format);
  output.backBufferCount = 1u;
  output.windowed = true;
  output.deviceWindow = Handle{91};
  output.presentationInterval = PresentInterval::Immediate;
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE, "grouping spec owns CPU-visible provider resources only");
  const auto expectedSources = drawCommandLimit == 3u ? 4u : 3u;
  const auto run = [&](Mode mode) {
    const bool executionOnly = mode == Mode::ExecutionOnly;
    RuntimeFixture fixture(false, false, false, true,
        WMT::Device{resourceDevice.handle}, nullptr, &output, true);
    fixture.routing->recordBoundedCapturedBindings = false;
    Executor executor(fixture, general, blobs);
    Executor::ControlledV4InventoryOptions options;
    options.route = ProviderReplayPolicy::Compatibility;
    options.executionMode = mode;
    options.sourcePlan = &source;
    options.exactTape = captured.tape;
    options.expectedBufferBudget = {executionOnly ? 0u : 1024u * 1024u};
    options.consumerLimits = {.maxSources = 8u, .maxCommands = 16u, .maxLifecycleActions = 32u};
    options.initialSetupLimits = {.maxObjects = 8u, .maxSeeds = 8u,
        .maxSeedBytes = 1024u * 1024u, .maxAllocationBytes = 1024u * 1024u};
    options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    options.maxQueryOraclePolls = 1u;
    options.producerContract = RenderTapeExecutionV4ProducerContract::PePresentReservationV1;
    options.observeLogicalSemantics = !executionOnly;
    options.legalCandidateNoOverwriteContract = true;
    options.freshNoAutoDepthSurfaceDefaults = !output.enableAutoDepthStencil;
    auto result = executor.runControlledV4Inventory(options);
    const auto& cpu = result.controlledCpu;
    const auto& consumer = result.consumer;
    const auto& state = consumer.state;
    const auto& query = result.queryReplay;
    const auto& route = result.routeCoverage;
    check(result.complete && result.sourceProgressComplete && result.resetOk && result.failure.empty() &&
              result.metadataInventoryComplete == !executionOnly && result.events == 14u &&
              result.commands == 8u && result.draws == 3u && result.resourcesCreated == 6u &&
              result.resourcesReleased == 6u && result.observedMutations == 2u &&
              cpu.controlledCpuComplete && cpu.rawAckCoverageComplete && cpu.semanticMappingComplete &&
              cpu.semanticMapping && cpu.semanticMapping->valid() && cpu.semanticMapping->records().size() == 7u &&
              cpu.actualIdentity.attempts.size() == 3u && cpu.actualIdentity.settlements.size() == 3u &&
              cpu.sourceObligationsRegistered == 3u && cpu.sourceObligationsQualified == 3u &&
              cpu.actualIdentity.sources.size() == expectedSources && cpu.actualIdentity.commands.size() == 6u &&
              renderTapeActionWitnessV1StableExact(cpu.actionWitness),
        "genuine grouping replay completes every original event/record/ACK with actual changed sources: limit=" +
            std::to_string(drawCommandLimit) + " execution=" + std::to_string(executionOnly) +
            " failure=" + result.failure);
    check(cpu.actualIdentity.coordinators.size() == 1u && cpu.semanticMapping->coordinators().size() == 1u,
        "both modes retain the actual original EVENT END coordinator");
    const auto& end = cpu.actualIdentity.coordinators[0].provenance;
    const auto& mappedEnd = cpu.semanticMapping->coordinators()[0];
    const auto& first = cpu.actualIdentity.sources[0].lifecycle;
    const auto endSource = drawCommandLimit == 3u ? 1u : 0u;
    check(end.eventOrdinal == 10u && end.recordIndex == 4u && end.recordType == D9C_COMMAND_RECORD_QUERY_ISSUE &&
              end.queryType == 8u && end.flags == 1u && end.issueCycle == 1u && end.issueOrdinal == 1u &&
              end.prefixKind == static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::RealSource) &&
              end.sourceIndex == endSource && end.commandEnd == (drawCommandLimit == 3u ? 1u : 4u) &&
              mappedEnd.prefixRecordCount == 4u && !mappedEnd.prefixCoordinatorCommandCount &&
              mappedEnd.candidatePrefixSequence == end.prefixSequence &&
              first.commandCount == (drawCommandLimit == 3u ? 3u : 4u),
        "natural command-limit publications preserve exactly four pre-END GPU records, never younger work");
    const auto& mapping = *cpu.semanticMapping;
    const auto younger = std::find_if(mapping.records().begin(), mapping.records().end(), [](const auto& record) {
      return record.eventOrdinal == 12u && record.recordIndex == 0u;
    });
    check(younger != mapping.records().end() && younger->recordType == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE &&
              younger->candidateSourceIndex > end.sourceIndex &&
              cpu.actualIdentity.sources[younger->candidateSourceIndex].lifecycle.seqId > end.prefixSequence,
        "END's immutable actual prefix excludes the authentic younger Draw and terminal Present");
    check(query.present && query.transcriptComplete && query.oracleComplete && query.queryDefinitions == 1u &&
              query.issueRecords == 1u && query.replayedIssueRecords == 1u && query.recordedPolls == 1u &&
              query.replayedPolls == 1u && query.readinessRequired == 1u && query.readinessSatisfied == 1u &&
              !query.requiredResults && !query.matchedResults && !query.substitutedResults &&
              !query.supplementalDrains && query.rows.size() == 1u && query.rows[0].eventOrdinal == 11u &&
              query.rows[0].issueCycle == 1u && query.rows[0].flags == 0u && !query.rows[0].definedBytes &&
              query.rows[0].hasOutput && query.rows[0].dataSize == 4u &&
              query.rows[0].capturedResult == D3D_OK && query.rows[0].oracleResult == D3D_OK &&
              query.rows[0].localSequence == end.prefixSequence && query.rows[0].readinessSatisfied &&
              !query.rows[0].resultMatched && query.rows[0].capturedBytes == query.rows[0].replayBytes &&
              !query.actualMetal && !query.gpuResultOracle,
        "original required-ready EVENT poll completes only the actual END prefix without invented result bytes");
    check(state.phase == dxmt9::test::CpuV4ConsumerPhase::Stopped && state.producerClosed &&
              state.sourcePhase == dxmt9::test::CpuV4ConsumerSourcePhase::Empty && state.observed == expectedSources &&
              state.submitted == expectedSources && state.acknowledged == expectedSources &&
              state.retired == expectedSources && state.commands == 6u && state.lifecycleActions == expectedSources * 4u &&
              consumer.workerExited && consumer.cleanupComplete && !consumer.cleanupFailed &&
              consumer.failure == dxmt9::test::CpuV4ConsumerFailure::None &&
              result.consumerReceipts.size() == expectedSources &&
              std::all_of(result.consumerReceipts.begin(), result.consumerReceipts.end(), [](const auto& receipt) {
                return receipt.observation && receipt.submitted && receipt.acknowledged && receipt.retired && receipt.cleanupComplete;
              }) && route.policySelected && route.observed && route.complete && route.monotonic &&
              route.requestedRoute == ProviderReplayPolicy::Compatibility &&
              route.effectiveRoute == ProviderReplayPolicy::Compatibility && route.compatibilityDraws == 3u &&
              !route.directDraws && !route.directLeases && !route.fallbackDraws,
        "actual source publications conserve FIFO submit/ACK/retire and requested Compatibility work in both modes");
    if (executionOnly) {
      check(result.executionOnly.complete && result.executionOnly.lifecycleCoverageComplete &&
                result.executionOnly.sources == expectedSources && result.executionOnly.commands == 6u &&
                result.executionOnly.draws == 3u && result.inventory.empty() && result.canonicalDrawRecords.empty() &&
                !result.recordContexts && !result.logicalSemantics.requested && result.logicalSemantics.receipts.empty(),
          "ExecutionOnly omits payload observation while retaining genuine grouping/query/lifetime checks");
    } else {
      check(result.logicalSemantics.complete && result.logicalSemantics.logicalStreamComplete &&
                result.logicalSemantics.receipts.size() == 6u && result.logicalSemantics.sourceRowsQualified == 3u &&
                result.inventory.size() == expectedSources && result.canonicalDrawRecords.size() == 3u,
          "Observe qualifies every original GPU item and authentic draw source row across changed partitions");
    }
    check(captured.tape == originalBytes && !result.actualMetal && !result.gpuResultOracle &&
              !result.fullSemanticOracleComplete && !result.policyQualified && !result.timingEligible &&
              !fixture.cDevice && !fixture.device && !fixture.factory && !fixture.routing->flushHook &&
              !Access::readyCount(fixture.routing->queue_) && !Access::writingCommandCount(fixture.routing->queue_) &&
              !Access::pendingCompletionCount(fixture.routing->queue_) && !Access::residentCount(fixture.routing->queue_) &&
              !Access::controlledCpuQueueStopped(fixture.routing->queue_),
        "grouping never rewrites original Tape or promotes native CPU evidence to public/GPU/timing authority");
    return result;
  };
  const auto observed = run(Mode::Observe);
  const auto executed = run(Mode::ExecutionOnly);
  check(observed.events == executed.events && observed.commands == executed.commands && observed.draws == executed.draws &&
            observed.resourcesCreated == executed.resourcesCreated && observed.resourcesReleased == executed.resourcesReleased &&
            observed.provenance.acknowledgedRaws == executed.provenance.acknowledgedRaws &&
            observed.queryReplay.issueRecords == executed.queryReplay.issueRecords &&
            observed.queryReplay.readinessSatisfied == executed.queryReplay.readinessSatisfied &&
            observed.consumer.state.observed == executed.consumer.state.observed &&
            observed.consumer.state.commands == executed.consumer.state.commands &&
            observed.routeCoverage.compatibilityDraws == executed.routeCoverage.compatibilityDraws &&
            observed.routeCoverage.compatibilityBatches == executed.routeCoverage.compatibilityBatches &&
            observed.routeCoverage.compatibilityUnbatchedDraws == executed.routeCoverage.compatibilityUnbatchedDraws,
      "Observe and ExecutionOnly execute the same admitted grouping policy, original semantics and real lifecycle");
  for (std::size_t index = 0u; index < observed.consumerReceipts.size(); ++index)
    check(observed.consumerReceipts[index].observation->commands == executed.consumerReceipts[index].observation->commands,
        "both modes observe the same actual physical command partition without comparing fresh local identities");
  std::uint64_t queryDigest = 1469598103934665603ull;
  const auto& query = observed.queryReplay.rows[0];
  digestBytes(queryDigest, &query.eventOrdinal, sizeof(query.eventOrdinal));
  digestBytes(queryDigest, &query.issueCycle, sizeof(query.issueCycle));
  digestBytes(queryDigest, &query.flags, sizeof(query.flags));
  digestBytes(queryDigest, &query.capturedResult, sizeof(query.capturedResult));
  digestBytes(queryDigest, query.capturedBytes.data(), query.definedBytes);
  std::cout << "{\"schema\":\"dxmt9.native.controlled_cpu_v4.grouping_end.v1\",\"draw_command_limit\":"
            << drawCommandLimit << ",\"sources\":" << observed.consumer.state.observed
            << ",\"physical_commands\":" << observed.consumer.state.commands
            << ",\"events\":" << observed.events << ",\"records\":7,\"draws\":" << observed.draws
            << ",\"end_prefix_records\":4,\"logical_digest\":" << observed.logicalSemantics.digest
            << ",\"query_digest\":" << queryDigest << ",\"original_items\":" << observed.logicalSemantics.receipts.size()
            << ",\"query_issues\":1,\"recorded_polls\":1,\"required_ready\":1,\"raw_acks\":"
            << observed.provenance.acknowledgedRaws << ",\"retired\":" << observed.consumer.state.retired
            << ",\"execute_sources\":" << executed.consumer.state.observed
            << ",\"execute_retired\":" << executed.consumer.state.retired
            << ",\"input_unchanged\":true,\"actual_metal\":false,\"timing_eligible\":false}\n";
}

// An explicitly authored native stress input, not a capture29 transcript or
// an original-production cut-equivalence claim. Only the supported limit=3
// policy creates candidate publications; this fixture never requests a cut.
dxmt9::d3d9::test::GeneralCpuDestroyFixture controlledCpuV4ProgressTape() {
  using namespace dxmt9::d3d9;
  auto fixture = test::makeGeneralCpuDestroyFixture(false, false);
  ImportedRenderTapeView original;
  check(importPrevalidatedRenderTape(fixture.tape, original), "progress seed tape imports");
  const auto oneRecord = [](const std::vector<std::byte>& bytes, std::uint32_t handles) {
    ImportedChunkView chunk;
    check(importPrevalidatedCommandChunk(bytes, {.recordCount = 1u, .handleCount = handles}, chunk),
          "progress GPU record retains the authentic fixture resource closure");
    const auto record = chunk.record(0u);
    return RecordSpec{record.header.type, {record.payload.begin(), record.payload.end()},
        {record.handles.begin(), record.handles.end()}};
  };
  const auto draw = oneRecord(test::general_cpu_destroy_fixture_detail::indexedDrawChunk(), 5u);
  const auto present = oneRecord(test::general_cpu_destroy_fixture_detail::presentChunk(), 1u);
  std::vector<RecordSpec> records;
  records.reserve(256u);
  std::uint32_t firstHandle = 0u;
  const auto appendRecord = [&](RecordSpec record) {
    // Wire payload references are chunk-global, not record-local. The seed
    // records each came from a one-record chunk with firstHandle=0; copying
    // their handle slices must rebase every non-null embedded reference.
    const auto rebaseHandle = [&](std::size_t offset) {
      check(offset <= record.payload.size() && sizeof(std::uint32_t) <= record.payload.size() - offset,
            "stress handle field stays inside its authenticated record payload");
      std::uint32_t handle = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX;
      std::memcpy(&handle, record.payload.data() + offset, sizeof(handle));
      if (handle == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX) return;
      check(handle < record.handles.size(), "stress seed handle names its original one-record slice");
      handle += firstHandle;
      std::memcpy(record.payload.data() + offset, &handle, sizeof(handle));
    };
    if (record.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
      D9CCommandChunkWireDrawHeader header{};
      check(record.payload.size() >= sizeof(header), "stress indexed draw keeps its authentic header");
      std::memcpy(&header, record.payload.data(), sizeof(header));
      for (std::uint32_t index = 0u; index < header.sectionCount; ++index) {
        const auto offset = static_cast<std::size_t>(header.sectionTableOffset) +
            index * sizeof(D9CCommandChunkWireSectionDesc);
        check(offset <= record.payload.size() && sizeof(D9CCommandChunkWireSectionDesc) <= record.payload.size() - offset,
              "stress indexed draw keeps its authentic section table");
        D9CCommandChunkWireSectionDesc section{};
        std::memcpy(&section, record.payload.data() + offset, sizeof(section));
        if (const auto* field = sectionHandleFieldRule(section.kind)) {
          for (std::uint32_t element = 0u; element < section.count; ++element)
            rebaseHandle(static_cast<std::size_t>(section.payloadOffset) +
                static_cast<std::size_t>(element) * section.elementSize + field->payloadOffset);
        }
      }
    } else {
      for (const auto& field : kRecordHandleFieldRules)
        if (field.recordType == record.type) rebaseHandle(field.payloadOffset);
    }
    firstHandle += static_cast<std::uint32_t>(record.handles.size());
    records.push_back(std::move(record));
  };
  for (std::size_t index = 0u; index < 128u; ++index) {
    appendRecord(draw);
    if (index != 127u) appendRecord(clearRecord());
  }
  appendRecord(present);
  const auto wire = makeWireFixture(records);
  check(wire.envelope.recordCount == 256u, "one stress raw stays within the General record cap");
  const auto wireValidation = validateCommandChunk(wire.bytes, wire.envelope);
  if (!wireValidation.valid())
    std::cerr << "native stress wire validation: status=" << static_cast<unsigned>(wireValidation.status)
              << " failedRecord=" << wireValidation.failedRecordIndex
              << " failedSection=" << wireValidation.failedSectionIndex
              << " failedHandle=" << wireValidation.failedHandleIndex
              << " byteOffset=" << wireValidation.byteOffset << '\n';
  check(wireValidation.valid(), "stress raw validates every rebased record-local handle slice");
  RenderTapeBuilder builder;
  bool appended = false;
  std::uint64_t nextOrdinal = 1u, presentOrdinal = 0u;
  for (std::uint32_t index = 0u; index < original.header.eventCount; ++index) {
    const auto event = original.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::ObjectDestroy) continue;
    if (type == RenderTapeEventType::CommandChunk) {
      if (appended) continue;
      builder.appendCommandChunk(wire.envelope, wire.bytes);
      appended = true;
      presentOrdinal = nextOrdinal++;
    } else if (type == RenderTapeEventType::PresentComplete) {
      auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
      RenderTapePresentCompleteHeader fixed{};
      std::memcpy(&fixed, payload.data(), sizeof(fixed));
      fixed.presentOrdinal = presentOrdinal;
      std::memcpy(payload.data(), &fixed, sizeof(fixed));
      builder.appendRawEvent(type, payload);
      ++nextOrdinal;
    } else {
      builder.appendRawEvent(type, event.payload);
      ++nextOrdinal;
    }
  }
  check(appended && presentOrdinal, "stress tape retains one terminal Present and the actual seeded prefix");
  fixture.tape = builder.seal();
  return fixture;
}

// This authenticated authored reference deliberately has one source per GPU
// record. SemanticMapping is cut-agnostic: it cannot force that morphology on
// the actual candidate or substitute these labels for local queue receipts.
dxmt9::d3d9::CaptureSourceReplayPlanV4 controlledCpuV4ProgressReference(
    std::span<const std::byte> tape,
    const dxmt9::d3d9::render_tape_runtime::GeneralPreflightPlan& general) {
  using namespace dxmt9::d3d9;
  RenderTapeIdentityV4View identity;
  identity.header.coordinatorDomain = static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial);
  identity.header.frameId = 9u;
  identity.header.captureToken = 100u;
  identity.header.captureEpoch = 101u;
  identity.header.presentOrdinal = general.presentOrdinal;
  identity.header.queue = 102u;
  identity.header.initializerEpoch = 103u;
  identity.header.baselineCompletionEdge = 1u;
  identity.header.baselineKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline);
  RenderTapeExecutionTopologyV1View topology;
  topology.header.captureToken = identity.header.captureToken;
  topology.header.captureEpoch = identity.header.captureEpoch;
  topology.header.queue = identity.header.queue;
  check(general.commandChunks.size() == 1u, "progress reference authenticates one unchanged full raw");
  const auto event = std::find_if(general.events.begin(), general.events.end(), [](const auto& value) {
    return value.type == RenderTapeEventType::CommandChunk;
  });
  check(event != general.events.end(), "progress reference locates its original command event");
  const auto ordinal = general.tape.event(event->eventIndex).header.ordinal;
  const auto raw = 500u + ordinal;
  const auto& admitted = general.commandChunks[0u];
  ImportedChunkView chunk;
  check(importPrevalidatedCommandChunk(admitted.bytes, admitted.envelope, chunk) && chunk.records.size() == 256u,
        "reference validates the original 256-record raw before authoring provenance");
  std::uint64_t edge = 2u;
  const auto begin = edge++, effect = edge++;
  for (std::uint32_t record = 0u; record < chunk.records.size(); ++record) {
    const auto type = chunk.records[record].type;
    const bool draw = type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
    check(draw || type == D9C_COMMAND_RECORD_CLEAR || type == D9C_COMMAND_RECORD_PRESENT,
          "progress reference admits only the authored GPU record kinds");
    const auto source = static_cast<std::uint32_t>(identity.sources.size());
    const auto publication = edge++, acceptance = edge++, submission = edge++, completion = edge++;
    identity.sources.push_back({.lifecycle = {.sourceOrdinal = source + 1u, .seqId = source + 1u,
        .writerCaptureId = source + 1u, .writerGeneration = 2u, .storageGeneration = 3u,
        .publicationEdge = publication, .submissionEdge = submission, .completionEdge = completion,
        .completedSequence = source + 1u, .cutEventOrdinal = ordinal, .commandCount = 1u,
        .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterRecord),
        .cutRecord = record + 1u}, .rawOrdinal = raw, .publicationBuildGeneration = 1u,
        .publicationControlIndex = source});
    identity.commands.push_back({.sourceIndex = source, .physicalCommandIndex = 0u,
        .kind = static_cast<std::uint32_t>(renderTapeIdentityV3RecordCommandKind(type)),
        .drawCount = draw ? 1u : 0u});
    identity.dispositions.push_back({.eventOrdinal = ordinal, .firstRecord = record, .recordCount = 1u,
        .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
        .commandIndex = source, .subdrawCount = draw ? 1u : 0u});
    topology.plans.push_back({.planId = source + 11u, .acceptanceEdge = acceptance,
        .route = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::NaturalOrder),
        .firstPlacement = source, .placementCount = 1u, .executedCount = 1u});
    topology.placements.push_back({.planIndex = source, .commandIndex = source,
        .outcome = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Outcome::ExecutedWithoutDag),
        .executionOrdinal = 0u}); // Dense order is plan-local; each authored plan executes one command.
  }
  const RenderTapeIdentityV4ReplaySourceIdentity projection{9u, 600u + ordinal, 600u + ordinal, raw};
  identity.attempts.push_back({.eventOrdinal = ordinal, .rawOrdinal = raw, .attemptGeneration = 1u,
      .beginEdge = begin, .terminalEdge = edge++, .effectEdge = effect, .identity = projection,
      .receipt = {.identity = projection, .queueSequence = raw,
          .kind = static_cast<std::uint32_t>(RenderTapeIdentityV4ReceiptKind::Compatibility), .commandCount = 256u},
      .recordCount = 256u, .outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::Committed),
      .stagedCommandCount = 256u});
  identity.settlements.push_back({ordinal, raw, edge++, 256u});
  RenderTapeIdentityV4Status identityStatus{};
  const auto identityBytes = buildRenderTapeIdentityV4(tape, general.catalogue, identity, &identityStatus);
  if (identityStatus != RenderTapeIdentityV4Status::Valid)
    std::cerr << "native stress reference identity: status=" << static_cast<unsigned>(identityStatus)
              << " sources=" << identity.sources.size() << " commands=" << identity.commands.size()
              << " dispositions=" << identity.dispositions.size() << " attempts=" << identity.attempts.size()
              << " settlements=" << identity.settlements.size() << '\n';
  check(identityStatus == RenderTapeIdentityV4Status::Valid, "native stress reference provenance authenticates");
  RenderTapeExecutionTopologyV1Status topologyStatus{};
  const auto topologyBytes = buildRenderTapeExecutionTopologyV1(tape, general.catalogue, identityBytes,
      topology, &topologyStatus);
  if (topologyStatus != RenderTapeExecutionTopologyV1Status::Valid) {
    std::cerr << "native stress reference topology: status=" << static_cast<unsigned>(topologyStatus)
              << " rowsStatus=" << static_cast<unsigned>(validateRenderTapeExecutionTopologyV1Rows(identity, topology))
              << " plans=" << topology.plans.size() << " placements=" << topology.placements.size() << '\n';
    for (std::size_t index = 0u; index < topology.placements.size(); ++index) {
      const auto& placement = topology.placements[index];
      if (placement.planIndex < topology.plans.size() &&
          renderTapeExecutionTopologyV1PlacementAdmitted(topology.plans[placement.planIndex], placement)) continue;
      std::cerr << "native stress reference invalid placement: row=" << index
                << " planIndex=" << placement.planIndex << " commandIndex=" << placement.commandIndex
                << " outcome=" << placement.outcome << " executionOrdinal=" << placement.executionOrdinal << '\n';
      break;
    }
  }
  check(topologyStatus == RenderTapeExecutionTopologyV1Status::Valid,
        "native stress reference topology authenticates before source-plan admission");
  CaptureSourceReplayPlanV4 source;
  const auto sourceStatus = buildCaptureSourceReplayPlanV4(tape, general.catalogue, identityBytes, topologyBytes, source);
  if (sourceStatus != CaptureSourceReplayPlanV4Status::Valid)
    std::cerr << "native stress reference source-plan: status=" << captureSourceReplayPlanV4StatusName(sourceStatus)
              << " identityStatus=" << static_cast<unsigned>(identityStatus)
              << " topologyStatus=" << static_cast<unsigned>(topologyStatus) << '\n';
  check(sourceStatus == CaptureSourceReplayPlanV4Status::Valid,
        "native stress reference pair is validated before candidate effects");
  return source;
}

void controlledCpuV4RootProgressUsesNaturalPublications() {
  using namespace dxmt9::d3d9;
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<RuntimeFixture, Access>;
  using Context = dxmt9::test::CpuTapeResourceContext;
  using ContextStatus = dxmt9::test::CpuTapeResourceContextStatus;
  using Clock = std::chrono::steady_clock;
  const auto captured = controlledCpuV4ProgressTape();
  std::vector<RenderTapeProviderBlob> blobs;
  for (const auto& bytes : captured.blobs)
    blobs.push_back({RenderTapeCaptureSession::sha256(bytes), bytes});
  runtime::GeneralPreflightPlan general;
  const auto preflight = runtime::buildGeneralReplayPlan(captured.tape, blobs, general);
  if (!preflight.complete()) {
    RenderTapeBlobCatalogue catalogue;
    for (const auto& blob : blobs)
      catalogue.blobs.push_back({.digest = blob.digest, .size = blob.bytes.size(), .verified = 1u});
    const auto validation = validateRenderTape(captured.tape, catalogue);
    std::cerr << "native stress General preflight: status=" << frameTapeReplayStatusName(preflight.status)
              << " failedEvent=" << preflight.failedEventIndex
              << " validation=" << renderTapeValidationStatusName(validation.status)
              << " validationFailedEvent=" << validation.failedEventIndex
              << " failedEventType=" << validation.failedEventType
              << " chunkStatus=" << static_cast<unsigned>(validation.chunkStatus)
              << " incompleteFrameReason=" << renderTapeIncompleteFrameReasonName(validation.incompleteFrameReason)
              << " objectDefineReason=" << renderTapeObjectDefineValidationSubreasonName(validation.objectDefine.subreason)
              << '\n';
  }
  check(preflight.complete(),
        "native stress input passes actual General preflight");
  const auto grammar = dxmt9::test::admitControlledCpuV4Input(general);
  check(grammar.accepted() && grammar.counts().records == 256u && grammar.counts().draws == 128u &&
            grammar.counts().presents == 1u && !grammar.counts().queryIssues &&
            grammar.sourceProgressRequirement() ==
                dxmt9::test::ControlledCpuV4SourceProgressRequirement::RequiredUnproven,
        "Raw256 input admission preserves one terminal Present but grants no source-progress authority");
  const auto source = controlledCpuV4ProgressReference(captured.tape, general);

  for (const bool failConsumer : {false, true}) {
    RuntimeFixture fixture(false, false, false, true, {}, nullptr, nullptr, true);
    fixture.routing->recordBoundedCapturedBindings = false;
    struct Owners {
      std::vector<runtime::OwnedObject> values;
      ~Owners() { for (auto& value : values) runtime::releaseOwnedObject(value); }
    } objects;
    RenderTapeExecutionV4Evidence evidence;
    RenderTapeExecutionV4 execution;
    check(execution.admit(source, general, captured.tape, evidence,
              RenderTapeExecutionV4PublicationMode::SemanticMapping), "stress V4 input admits before setup");
    for (const auto& definition : general.definitions) {
      auto* value = runtime::createRenderTapeObject(fixture.cDevice.get(), definition.fixed,
          definition.descriptor, blobs, objects.values);
      check(value != nullptr, "stress prefix acquires the actual provider object");
      objects.values.push_back({definition.fixed.identity, value});
    }
    std::vector<dxmt9::test::detail::LogicalReceipt> receipts;
    for (const auto& mutation : general.mutations) {
      const auto* blob = dxmt9::test::detail::cpuGeneralFindBlob(blobs, mutation.digest);
      auto* buffer = static_cast<D9CBuffer*>(runtime::findExactObject(objects.values, mutation.identity));
      RenderTapeBufferMutationReplayPlan plan{};
      void* bits = nullptr;
      check(blob && buffer && renderTapeBufferMutationReplayPlan(mutation, plan) &&
                dxmt9c_buffer_lock(buffer, plan.byteOffset, plan.byteSize, &bits, plan.lockFlags) == D3D_OK && bits,
          "stress prefix enters the genuine buffer seed lock");
      std::memcpy(bits, blob->bytes.data(), blob->bytes.size());
      check(dxmt9c_buffer_unlock(buffer) == D3D_OK, "stress prefix completes actual seed upload");
      dxmt9::test::detail::cpuGeneralRecordReceipt(receipts, mutation.identity, mutation.subresource, mutation.digest);
    }
    for (const auto& bootstrap : general.bootstrapChunks) {
      std::vector<void*> resolved(bootstrap.envelope.handleCount);
      std::size_t count = 0u;
      check(runtime::resolveCommandHandles(bootstrap, objects.values, resolved, count) &&
                replayPrevalidatedResolvedCommandChunk(fixture.cDevice.get(), bootstrap.bytes, bootstrap.envelope,
                    std::span<void* const>(resolved.data(), count), ProviderReplayPolicy::Compatibility) == D3D_OK,
          "stress prefix replays authentic bootstrap against still-owned seeded wrappers");
    }
    Executor normalizer(fixture, general, blobs);
    std::vector<std::shared_ptr<const dxmt9::test::CpuTapeResourceVersion>> versions;
    for (const auto& object : objects.values) {
      dxmt9::core::Handle native{};
      const D9CBuffer* buffer = nullptr;
      if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        buffer = static_cast<const D9CBuffer*>(object.value);
        if (buffer->obj) native = buffer->obj->handle();
      } else if (object.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
        const auto* surface = static_cast<const D9CSurface*>(object.value);
        if (surface->obj) native = surface->obj->handle();
      } else continue;
      dxmt9::test::CpuTapeResourceObservation observation;
      check(native && normalizer.captureResourceObservation(objects.values, receipts, {},
                object.identity.kind, native, observation), "stress producer freezes actual normalized resource facts");
      std::shared_ptr<const dxmt9::test::CpuTapeResourceVersion> version;
      check(dxmt9::test::buildCpuTapeResourceVersion({.nativeKind = object.identity.kind, .nativeHandle = native,
                .contentEpoch = 1u, .expectedBufferByteSize = buffer ? buffer->desc.size : 0u,
                .expectedBufferBytes = buffer ? buffer->obj->bytes() : std::span<const std::uint8_t>{}},
                observation, version) == ContextStatus::Accepted, "stress record context owns copied values only");
      versions.push_back(std::move(version));
    }
    std::shared_ptr<const Context> context;
    check(dxmt9::test::buildCpuTapeResourceContext(versions, context) == ContextStatus::Accepted &&
              context->entries().size() == 3u, "stress immutable context retains the unchanged resource closure");
    for (std::uint32_t index = 0u; index < execution.snapshotPrefixEvents(); ++index)
      check(execution.beginEvent(index) && execution.replayedEvent(index), "stress prefix acknowledges completed real effects");
    auto capability = fixture.issueControlledCpuCapability();
    check(execution.armControlledCpu(fixture.cDevice.get(), capability), "stress consumer has a genuine locally Bound V4 root");

    struct Observer {
      std::shared_ptr<RenderTapeRuntimeIdentityV4> root;
      dxmt9::render::TapeTopologyCaptureIdentity capture{};
      std::shared_ptr<const Context> context;
      dxmt9::test::CpuTapeRecordContextStore records;
      std::atomic<bool> rawAck{false}, producerDone{false};
      std::atomic<std::size_t> originsBeforeAck{0u};
      std::mutex mutex;
      std::condition_variable cv;
      bool entered = false, release = false, injectFailure = false;
      Clock::time_point deadline;
      std::size_t sources = 0u, origins = 0u;
      std::vector<bool> selected = std::vector<bool>(256u, false);

      void observe(const dxmt9::core::metalqueue::ReplaySourcePublicationReceipt& publication,
          const dxmt9::core::metalqueue::SynchronousSourcePayloadBorrow& payload) {
        RenderTapeRuntimePublishedSourceOriginsV4 copied;
        check(root->copyPublishedSourceOrigins(capture, publication, copied) &&
                  copied.commands.size() == payload.commandCount(), "stress consumer pulls real origins during the raw");
        for (std::size_t index = 0u; index < payload.commandCount(); ++index) {
          const auto command = payload.commandAt(index);
          check(copied.commands[index].physicalCommandIndex == index &&
                    copied.commands[index].kind == dxmt9::render::tapeTopologyCommandKind(command.kind()) &&
                    copied.commands[index].drawCount == command.command.drawParams.size(),
                "stress source origin extent matches the retained payload borrow");
        }
        for (const auto& origin : copied.origins) {
          check(origin.record.recordIndex < selected.size() && !selected[origin.record.recordIndex] &&
                    records.lookup(origin.record) == context, "stress canonical origin selects its pre-dispatch immutable context");
          selected[origin.record.recordIndex] = true;
          ++origins;
        }
        ++sources;
        if (!rawAck.load(std::memory_order_acquire)) originsBeforeAck.fetch_add(copied.origins.size());
        if (sources == 1u) {
          std::unique_lock lock(mutex);
          entered = true;
          cv.notify_all();
          check(cv.wait_until(lock, deadline, [&] { return release; }), "stress observer gate reaches its bounded release");
          if (injectFailure) throw TestFailure("injected synchronous consumer observation failure");
        }
      }
    };
    auto observer = std::make_shared<Observer>();
    observer->root = acquireRenderTapeRuntimeIdentityV4(fixture.cDevice.get());
    observer->capture = {evidence.localArm.captureToken, evidence.localArm.captureEpoch, evidence.localArm.queue};
    observer->context = context;
    observer->deadline = Clock::now() + std::chrono::seconds(10);
    const auto& chunk = general.commandChunks[0u];
    const auto commandEvent = std::find_if(general.events.begin(), general.events.end(), [](const auto& event) {
      return event.type == RenderTapeEventType::CommandChunk;
    });
    check(commandEvent != general.events.end() && observer->root, "stress interval retains one real root and original event");
    const auto ordinal = general.tape.event(commandEvent->eventIndex).header.ordinal;
    for (std::uint32_t index = 0u; index < chunk.envelope.recordCount; ++index)
      check(observer->records.bind({ordinal, index}, context) == ContextStatus::Accepted,
            "stress producer binds every canonical record context before dispatch");
    auto handle = fixture.makeControlledCpuConsumer(capability, evidence.localArm, {}, observer->deadline,
        [observer](auto&, const auto& publication, const auto& payload) {
          observer->observe(publication, payload);
        });
    check(handle && handle->start(), "stress root consumer starts before producer replay");
    std::vector<void*> resolved(chunk.envelope.handleCount);
    std::size_t count = 0u;
    check(execution.beginEvent(commandEvent->eventIndex) &&
              runtime::resolveCommandHandles(chunk, objects.values, resolved, count) &&
              execution.bindCommandChunk(chunk.bytes, chunk.envelope, std::span<void* const>(resolved.data(), count)),
        "stress binds unchanged original raw bytes and genuine wrappers");
    struct Forward {
      ProviderReplayObservationV4 upstream;
      std::shared_ptr<Observer> observer;
      std::shared_ptr<RuntimeFixture::ControlledConsumerHandle> handle;
      std::size_t retiredBeforeAck = 0u;
    } forward{execution.observation(), observer, handle};
    auto observation = forward.upstream;
    observation.context = &forward;
    observation.rawStarted = [](void* p, auto event, auto raw, auto records) noexcept {
      auto& value = *static_cast<Forward*>(p);
      return value.upstream.rawStarted(value.upstream.context, event, raw, records);
    };
    observation.beforeRecord = [](void* p, auto index, auto type) noexcept {
      auto& value = *static_cast<Forward*>(p);
      return value.upstream.beforeRecord(value.upstream.context, index, type);
    };
    observation.beforeDispatch = [](void* p, auto index, auto type) noexcept {
      auto& value = *static_cast<Forward*>(p);
      return value.upstream.beforeDispatch(value.upstream.context, index, type);
    };
    observation.recordDispatched = [](void* p, auto index, auto type, auto result) noexcept {
      auto& value = *static_cast<Forward*>(p);
      return value.upstream.recordDispatched(value.upstream.context, index, type, result);
    };
    observation.rawSettled = [](void* p, auto raw, auto records, bool succeeded) noexcept {
      auto& value = *static_cast<Forward*>(p);
      value.retiredBeforeAck = value.handle->consumer->snapshot().state.retired;
      value.observer->rawAck.store(true, std::memory_order_release);
      return value.upstream.rawSettled(value.upstream.context, raw, records, succeeded);
    };
    ProviderReplayObservationV4Result replayResult;
    std::int32_t replayHr = D3DERR_INVALIDCALL;
    std::exception_ptr producerException;
    struct Threads {
      std::shared_ptr<Observer> observer;
      std::shared_ptr<RuntimeFixture::ControlledConsumerHandle> handle;
      std::thread producer;
      ~Threads() {
        {
          std::lock_guard lock(observer->mutex);
          observer->release = true;
        }
        observer->cv.notify_all();
        // Real poison, while authority remains alive, wakes any parked writer.
        // Join before capability/root/wrapper teardown, also on assertion unwind.
        handle->cancelAndJoin();
        if (producer.joinable()) producer.join();
      }
    } threads{observer, handle, {}};
    threads.producer = std::thread([&] {
      try {
        replayHr = replayPrevalidatedResolvedCommandChunkObservedV4(fixture.cDevice.get(), chunk.bytes,
            chunk.envelope, std::span<void* const>(resolved.data(), count), ProviderReplayPolicy::Compatibility,
            observation, replayResult);
      } catch (...) { producerException = std::current_exception(); }
      observer->producerDone.store(true, std::memory_order_release);
      observer->cv.notify_all();
    });
    {
      std::unique_lock lock(observer->mutex);
      check(observer->cv.wait_until(lock, observer->deadline, [&] {
                return observer->entered || observer->producerDone.load(std::memory_order_acquire);
              }) && observer->entered && !observer->rawAck.load(std::memory_order_acquire),
            "consumer copies actual root origins before the original raw can ACK");
    }
    Access::ControlledCpuParkSnapshot parked;
    while (Clock::now() < observer->deadline && !observer->producerDone.load(std::memory_order_acquire)) {
      parked = Access::controlledCpuParkSnapshot(fixture.routing->queue_);
      if (parked.acquired && parked.writerPressure) break;
      std::unique_lock lock(observer->mutex);
      observer->cv.wait_for(lock, std::chrono::milliseconds(1));
    }
    check(parked.acquired && parked.writerPressure && parked.writing &&
              parked.inflight == dxmt9::kMaxQueuedChunks && parked.ready + 1u == parked.inflight &&
              parked.resident == dxmt9::kMaxQueuedChunks + 1u && !observer->rawAck.load(),
          "actual producer park owns Ready backlog plus one Represented borrow and one Writing source");
    {
      std::lock_guard lock(observer->mutex);
      observer->injectFailure = failConsumer;
      observer->release = true;
    }
    observer->cv.notify_all();
    {
      std::unique_lock lock(observer->mutex);
      check(observer->cv.wait_until(lock, observer->deadline, [&] {
                return observer->producerDone.load(std::memory_order_acquire);
              }), "retirement or actual consumer poison wakes the parked producer before deadline");
    }
    threads.producer.join();
    if (producerException) std::rethrow_exception(producerException);
    if (failConsumer) {
      handle->join();
      const auto consumer = handle->consumer->snapshot();
      const auto actual = handle->backendState->snapshot();
      check(replayHr < 0 && consumer.workerExited &&
                consumer.failure == dxmt9::test::CpuV4ConsumerFailure::Backend &&
                consumer.state.retired == 0u && !consumer.cleanupComplete && consumer.cleanupFailed &&
                actual.queueQuarantined && actual.borrowReleased &&
                actual.phase == Access::ControlledCpuActualPhase::Represented &&
                !actual.submitted && !actual.acknowledged && !actual.retired &&
                Access::controlledCpuQueueStopped(fixture.routing->queue_),
            "failed real observation poisons/wakes the producer without fabricated completion or reclamation");
      execution.cancel();
      continue;
    }
    check(replayHr == D3D_OK && replayResult.captureAccepted && replayResult.qualificationSucceeded &&
              replayResult.ledgerSettlementObserved && forward.retiredBeforeAck > 0u &&
              observer->originsBeforeAck.load() > 0u && !execution.failed(),
          "root origin consumption and actual FIFO retirement progress while the full raw remains unacknowledged");
    for (const auto& event : general.events) {
      if (event.eventIndex <= commandEvent->eventIndex) continue;
      check(event.type == RenderTapeEventType::PresentComplete && execution.beginEvent(event.eventIndex) &&
                execution.replayedEvent(event.eventIndex), "stress traversal acknowledges the original terminal completion event");
    }
    check(fixture.closeControlledCpuProducer() && handle->consumer->closeProducer() &&
              handle->consumer->stopAndJoin(), "stress consumer drains natural sources and joins without a synthetic tail cut");
    handle->join();
    const auto consumer = handle->consumer->snapshot();
    const auto actual = handle->backendState->snapshot();
    check(observer->sources > dxmt9::kMaxQueuedChunks && observer->origins == 256u &&
              std::all_of(observer->selected.begin(), observer->selected.end(), [](bool value) { return value; }) &&
              consumer.state.observed == observer->sources && consumer.state.retired == observer->sources &&
              consumer.state.commands == 256u && consumer.cleanupComplete && !consumer.cleanupFailed &&
              actual.retired == observer->sources && actual.observedOutsideQueueMutex && actual.borrowReleased &&
              !Access::residentCount(fixture.routing->queue_),
          "actual observed counts conserve every original record across more publications than the queue cap");
    RenderTapeControlledCpuV4Evidence cpu;
    check(execution.finishControlledCpu(capability, cpu) && cpu.controlledCpuComplete &&
              cpu.rawAckCoverageComplete && cpu.semanticMappingComplete && cpu.actualIdentity.settlements.size() == 1u &&
              cpu.actualIdentity.sources.size() == observer->sources && cpu.semanticMapping->records().size() == 256u &&
              !consumer.fullSemanticOracleComplete && !consumer.payloadDigestPresent &&
              !consumer.actualMetal && !consumer.gpuResultOracle && !consumer.timingEligible &&
              !evidence.complete && !evidence.policyQualified && !evidence.timingEligible,
          "native stress proves controlled root/progress only, not original cuts, full resource semantics, GPU or timing authority");
  }
}

// Every command the replayed raw published, in source order, across every
// source it published. A replayed raw may seal more than one source -- for
// example, a Present publishes its slot -- so the ordered command stream of
// the raw is the concatenation, not any single source's payload.
//
// The trailing `present()` flushes a still-open writing slot; the drain is then
// bounded by the non-blocking `readyCount` snapshot. `consumeOne` is NOT an
// empty-queue probe: one extra call past the last ready source waits on a
// condition variable and wedges the test thread.
std::vector<dxmt9::core::MetalCommandKind> drainOrderedCommandKinds(
    RuntimeFixture& fixture) {
  fixture.routing->present(SwapDesc{});
  const auto ready = dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
      fixture.routing->queue_);
  check(ready != 0u, "a replayed raw publishes at least one source");
  std::vector<dxmt9::core::MetalCommandKind> kinds;
  for (std::size_t i = 0; i < ready; ++i) {
    const auto completion =
        dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
            fixture.routing->queue_);
    check(completion.dequeued,
          "every counted ready source dequeues without waiting");
    for (const auto kind : completion.commandKinds) {
      kinds.push_back(static_cast<dxmt9::core::MetalCommandKind>(kind));
    }
  }
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
            fixture.routing->queue_) == 0u,
        "the bounded drain consumes exactly the published sources");
  // The flushing present above contributes the final Present barrier; the
  // records under test own everything before it.
  check(!kinds.empty() && kinds.back() == dxmt9::core::MetalCommandKind::Present,
        "the flushing present closes the drained stream");
  kinds.pop_back();
  return kinds;
}

void directRawPublishesAndCompletesArenaSource() {
  RuntimeFixture fixture;
  const std::array records{clearRecord()};
  auto raw = makeRaw(makeWireFixture(records), 1);
  const auto hr = dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw);
  check(hr == D3D_OK && fixture.routing->clearCalls == 1,
        "capability=true replayRawChunk must replay Direct semantics once");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
            fixture.routing->queue_) == 2 &&
            dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                fixture.routing->queue_) == 1 &&
            dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
                fixture.routing->queue_) == 1,
        "Direct replay must consume one strict ticket and publish one source");

  const auto completion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(completion.dequeued && completion.arena && completion.clear &&
            completion.submitted && completion.completed &&
            completion.reclaimed && completion.seqId == 1,
        "Direct source must survive serial consume, completion, and reclaim");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
            fixture.routing->queue_) == 0,
        "completed Direct source must release Tape residency");
}

void semanticTransferOwnsArenaTransactionUntilSettlement() {
  RuntimeFixture fixture;
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1u;
  capacity.clearRecords = 1u;
  const auto limits = fixture.routing->queue_.cpuReadyArenaPlanLimits();
  const auto segment = makeSourcePayloadLayout(
      capacity, limits.pageSize, limits.maxOrdinaryPagesPerSegment);
  check(segment.has_value(), "transfer fixture segment must build");
  const auto layout = makeArenaSourcePayloadLayout(
      std::array{*segment}, limits.pageSize, limits.maxPagesPerSource);
  check(layout.has_value(), "transfer fixture layout must build");

  auto raw = makeRaw(makeWireFixture(std::array{clearRecord()}), 77u);
  const auto originalBytes = raw.recordBlob;
  auto begin = fixture.routing->queue_.beginCpuReadyArenaSource(77u, *layout);
  check(begin.has_value(), "transfer fixture must reserve Tape storage");
  dxmt9::d3d9::CpuReadySemanticTransfer transfer(raw, std::move(*begin));
  check(transfer.stage() ==
            dxmt9::d3d9::CpuReadySemanticTransferStage::Reserved &&
            transfer.identity().rawOrdinal == 77u &&
            transfer.identity().seqId == 1u && raw.recordBlob.empty(),
        "transfer must move Raw ownership into the reserved transaction");
  check(transfer.adopt() && transfer.markEmitted(),
        "transfer must enforce Reserve-Adopt-Emit order");
  fixture.routing->queue_.submitClear(ClearDesc{});
  check(transfer.publish() ==
            dxmt9::CommandQueue::CpuReadyArenaPublishStatus::Published &&
            transfer.stage() ==
                dxmt9::d3d9::CpuReadySemanticTransferStage::Published,
        "transfer must publish the emitted Tape source exactly once");
  transfer.restoreToSource();
  check(raw.recordBlob == originalBytes,
        "published transfer must restore worker Raw ownership without copy");
  const auto completion = dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
      fixture.routing->queue_);
  check(completion.arena && completion.clear && completion.reclaimed,
        "published transfer must reach normal completion and reclaim");
}

void semanticTransferPreEffectAbortRestoresOwner() {
  RuntimeFixture fixture;
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1u;
  capacity.clearRecords = 1u;
  const auto limits = fixture.routing->queue_.cpuReadyArenaPlanLimits();
  const auto segment = makeSourcePayloadLayout(
      capacity, limits.pageSize, limits.maxOrdinaryPagesPerSegment);
  check(segment.has_value(), "abort fixture segment must build");
  const auto layout = makeArenaSourcePayloadLayout(
      std::array{*segment}, limits.pageSize, limits.maxPagesPerSource);
  check(layout.has_value(), "abort fixture layout must build");
  auto raw = makeRaw(makeWireFixture(std::array{clearRecord()}), 78u);
  const auto originalBytes = raw.recordBlob;
  auto begin = fixture.routing->queue_.beginCpuReadyArenaSource(78u, *layout);
  check(begin.has_value(), "abort fixture must reserve Tape storage");
  dxmt9::d3d9::CpuReadySemanticTransfer transfer(raw, std::move(*begin));
  check(transfer.adopt() && !transfer.abortForFallback() &&
            transfer.stage() ==
                dxmt9::d3d9::CpuReadySemanticTransferStage::FailStopped &&
            transfer.failure() ==
                dxmt9::d3d9::CpuReadySemanticTransferFailure::AbortFailed &&
            fixture.routing->queue_.cpuReadyArenaPoisoned(),
        "single-source abort must remain terminal rather than claim rollback");
  transfer.restoreToSource();
  check(raw.recordBlob == originalBytes &&
            dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                fixture.routing->queue_) == 0u,
        "terminal abort must return Raw after releasing Tape storage");
}

void semanticTransferBatchAbortRestoresOwner() {
  RuntimeFixture fixture;
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1u;
  capacity.clearRecords = 1u;
  const auto limits = fixture.routing->queue_.cpuReadyArenaPlanLimits();
  const auto segment = makeSourcePayloadLayout(
      capacity, limits.pageSize, limits.maxOrdinaryPagesPerSegment);
  check(segment.has_value(), "batch abort fixture segment must build");
  const auto layout = makeArenaSourcePayloadLayout(
      std::array{*segment}, limits.pageSize, limits.maxPagesPerSource);
  check(layout.has_value(), "batch abort fixture layout must build");
  const std::array layouts{*layout, *layout};
  auto raw = makeRaw(
      makeWireFixture(std::array{clearRecord(), clearRecord()}), 80u);
  const auto originalBytes = raw.recordBlob;
  auto begin = fixture.routing->queue_.beginCpuReadyArenaSources(80u, layouts);
  check(begin.has_value(), "batch abort fixture must reserve Tape storage");
  dxmt9::d3d9::CpuReadySemanticTransfer transfer(raw, std::move(*begin));
  check(transfer.adopt() && transfer.abortForFallback() &&
            transfer.stage() ==
                dxmt9::d3d9::CpuReadySemanticTransferStage::Aborted,
        "batch pre-effect abort must be recoverable");
  transfer.restoreToSource();
  check(raw.recordBlob == originalBytes &&
            dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                fixture.routing->queue_) == 0u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
                fixture.routing->queue_) == 1u &&
            !fixture.routing->queue_.cpuReadyArenaPoisoned(),
        "batch abort must restore Raw, cursors, and an unpoisoned queue");
}

void semanticTransferPostEffectFailureIsTerminal() {
  RuntimeFixture fixture;
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1u;
  capacity.clearRecords = 1u;
  const auto limits = fixture.routing->queue_.cpuReadyArenaPlanLimits();
  const auto segment = makeSourcePayloadLayout(
      capacity, limits.pageSize, limits.maxOrdinaryPagesPerSegment);
  check(segment.has_value(), "failure fixture segment must build");
  const auto layout = makeArenaSourcePayloadLayout(
      std::array{*segment}, limits.pageSize, limits.maxPagesPerSource);
  check(layout.has_value(), "failure fixture layout must build");
  auto raw = makeRaw(makeWireFixture(std::array{clearRecord()}), 79u);
  auto begin = fixture.routing->queue_.beginCpuReadyArenaSource(79u, *layout);
  check(begin.has_value(), "failure fixture must reserve Tape storage");
  dxmt9::d3d9::CpuReadySemanticTransfer transfer(raw, std::move(*begin));
  check(transfer.adopt(), "failure fixture must adopt before effects");
  fixture.routing->queue_.submitClear(ClearDesc{});
  check(transfer.markEmitted(), "failure fixture must record emission");
  dxmt9::CommandQueueArenaLeaseTestAccess::forceNextPublicationFailure(
      fixture.routing->queue_);
  check(transfer.publish() ==
            dxmt9::CommandQueue::CpuReadyArenaPublishStatus::FailStopped &&
            transfer.stage() ==
                dxmt9::d3d9::CpuReadySemanticTransferStage::FailStopped &&
            transfer.failure() ==
                dxmt9::d3d9::CpuReadySemanticTransferFailure::PublishFailStopped &&
            fixture.routing->queue_.cpuReadyArenaPoisoned(),
        "post-effect publication failure must be typed and terminal");
}

void ordinarySegmentConfiguredRawKeepsOneSourceAt512Pages() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/true);
  const auto rects = clearRectCountForPlannerPages(20);
  const std::array records{clearRecord(rects), clearRecord(rects),
                           clearRecord(rects), clearRecord(rects),
                           clearRecord(rects), presentRecord()};
  auto raw = makeRaw(makeWireFixture(records), 2);
  const auto hr = dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw);
  check(hr == D3D_OK, "non-captured startup raw must replay successfully");
  check(fixture.routing->clearCalls == 5u && fixture.routing->presentCalls == 1u,
        "non-captured startup raw must apply all clears and Present once");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
              fixture.routing->queue_) == 1u,
        "non-captured startup raw must publish one source");
  check(fixture.routing->queue_.cpuReadyArenaPlanLimits().maxPagesPerSource ==
            512u,
        "non-captured SegmentSerial startup raw must retain the 512-page "
        "queue bound");
}

void capturedLargeRawPublishesTwoAuthenticatedSources() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/true);
  const auto rects = clearRectCountForPlannerPages(20);
  // Keep a StateOnly record after each early source candidate.  These records
  // do not produce Arena commands, so the projection must close each source's
  // final command anchor at that source's raw boundary rather than only at the
  // event's final anchor.
  const std::array records{clearRecord(rects),
                           applyRenderStateRecord(RS_TEXTURE_FACTOR, 1u),
                           clearRecord(rects),
                           applyRenderStateRecord(RS_TEXTURE_FACTOR, 2u),
                           clearRecord(rects),
                           applyRenderStateRecord(RS_TEXTURE_FACTOR, 3u),
                           clearRecord(rects),
                           applyRenderStateRecord(RS_TEXTURE_FACTOR, 4u),
                           clearRecord(rects),
                           presentRecord()};
  auto raw = makeRaw(makeWireFixture(records), 4, /*captureIdentity=*/true);
  const auto hr = dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw);
  check(hr == D3D_OK,
        "captured large raw with source-boundary StateOnly records must "
        "replay successfully");
  check(fixture.routing->clearCalls == 5u && fixture.routing->presentCalls == 1u,
        "captured large raw must preserve one-pass clear/present counts");
  const auto readySources = dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
      fixture.routing->queue_);
  check(readySources >= 2u,
        "captured event larger than 64 pages must publish multiple Ready sources "
        "(got " + std::to_string(readySources) + ")");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
              fixture.routing->queue_) == readySources + 1u &&
            !fixture.routing->queue_.cpuReadyArenaPoisoned(),
        "captured SegmentSerial publication must consume all source seqs without "
        "poison");
  std::vector<dxmt9::CommandQueueArenaLeaseTestAccess::CompletionResult>
      completions;
  completions.reserve(readySources);
  for (std::size_t i = 0; i < readySources; ++i) {
    completions.push_back(dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
        fixture.routing->queue_, /*deferFinish=*/true));
  }
  for (std::size_t i = 0;
       i < readySources + 1u &&
       dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
           fixture.routing->queue_) != 0u;
       ++i) {
    (void)dxmt9::CommandQueueArenaLeaseTestAccess::finishIteration(
        fixture.routing->queue_);
  }
  check(fixture.routing->queue_.waitForCpuReadyEventSettlement(
            /*rawOrdinal=*/4u, /*buildGeneration=*/1u,
            /*firstSourceOrdinal=*/1u, /*tailSeqId=*/readySources,
            static_cast<std::uint32_t>(readySources)),
        "captured SegmentSerial group must expose exact final settlement");
  const bool completionsValid =
      std::all_of(completions.begin(), completions.end(),
                  [](const auto& completion) {
                    return completion.arena && completion.submitted &&
                        completion.completed;
                  }) &&
      std::all_of(completions.begin(), completions.end() - 1,
                  [](const auto& completion) {
                    return !completion.finalPresent;
                  }) &&
      completions.back().finalPresent && completions.front().seqId == 1u &&
      completions.back().seqId == readySources &&
      dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
          fixture.routing->queue_) == 0u;
  if (!completionsValid) {
    std::string details = "captured SegmentSerial completion mismatch:";
    for (const auto& completion : completions) {
      details += " [seq=" + std::to_string(completion.seqId) +
          " arena=" + std::to_string(completion.arena) +
          " submitted=" + std::to_string(completion.submitted) +
          " completed=" + std::to_string(completion.completed) +
          " reclaimed=" + std::to_string(completion.reclaimed) +
          " final=" + std::to_string(completion.finalPresent) +
          " segments=" + std::to_string(completion.arenaSegmentCount) +
          " cmds=" + std::to_string(completion.commandCount) + "]";
    }
    details += " resident=" + std::to_string(
        dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
            fixture.routing->queue_));
    throw TestFailure(details);
  }
}

void eventCompletionBridgeWaitsExactCapturedEvent() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/true);
  constexpr std::uint64_t kCaptureToken = 0xfeedu;
  constexpr std::uint64_t kFirstEvent = 4u;
  constexpr std::uint64_t kSecondEvent = 5u;
  auto first = makeRaw(makeWireFixture(std::array{clearRecord()}), kFirstEvent,
                       /*captureIdentity=*/true);
  auto second = makeRaw(makeWireFixture(std::array{clearRecord()}),
                        kSecondEvent, /*captureIdentity=*/true);
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), first) == D3D_OK &&
            dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), second) == D3D_OK,
        "captured event bridge fixture must publish both real sources");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
            fixture.routing->queue_) == 2u,
        "captured event bridge fixture must expose two Ready sources");
  const auto nextSeqBeforeWait =
      dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
          fixture.routing->queue_);

  fixture.routing->flushHook = [&] {
    const auto ready = dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
        fixture.routing->queue_);
    for (std::size_t i = 0u; i < ready; ++i) {
      const auto completion = dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
      check(completion.dequeued && completion.submitted && completion.completed,
            "flush hook must consume each real captured source");
    }
  };
  const auto expectNoFlush = [&](std::uint64_t token, std::uint64_t event,
                                 std::string_view reason) {
    const auto priorFlushes = fixture.routing->flushCalls;
    const auto priorSeq = dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
        fixture.routing->queue_);
    check(dxmt9c_device_wait_render_tape_event_completion(
              fixture.cDevice.get(), token, event) == D3DERR_INVALIDCALL &&
              fixture.routing->flushCalls == priorFlushes &&
              dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
                  fixture.routing->queue_) == priorSeq,
          reason);
  };
  expectNoFlush(0u, kFirstEvent,
                "zero capture token must reject without flushing");
  expectNoFlush(0xdeadbeefu, kFirstEvent,
                "foreign capture token must reject without flushing");
  expectNoFlush(kCaptureToken, 999u,
                "missing event ordinal must reject without flushing");

  const auto priorFlushes = fixture.routing->flushCalls;
  check(dxmt9c_device_wait_render_tape_event_completion(
            fixture.cDevice.get(), kCaptureToken, kFirstEvent) == D3D_OK &&
            fixture.routing->flushCalls == priorFlushes + 1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
                fixture.routing->queue_) == nextSeqBeforeWait &&
            fixture.routing->clearCalls == 2u,
        "valid earlier event must flush and settle only existing sources");
  D9CRenderTapeIdentityCaptureResult incomplete{};
  check(!fixture.cDevice->renderTapeIdentityCapture.copy(
            kCaptureToken, incomplete, {}),
        "capture ledger copy must remain incomplete until the newer event settles");

  const auto secondPriorFlushes = fixture.routing->flushCalls;
  check(dxmt9c_device_wait_render_tape_event_completion(
            fixture.cDevice.get(), kCaptureToken, kSecondEvent) == D3D_OK &&
            fixture.routing->flushCalls == secondPriorFlushes + 1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
                fixture.routing->queue_) == nextSeqBeforeWait &&
            dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                fixture.routing->queue_) == 0u,
        "newer event must settle through its exact ledger row without appending");
  D9CRenderTapeIdentityCaptureResult complete{};
  check(fixture.cDevice->renderTapeIdentityCapture.copy(kCaptureToken, complete,
                                                        {}) &&
            complete.status == D9C_RENDER_TAPE_IDENTITY_CAPTURE_COMPLETE &&
            complete.settlementCount == 2u,
        "capture ledger copy must complete after both exact event waits");
}

void rejectedCaptureStillCompletesAcceptedWork() {
  RuntimeFixture fixture(false, true);
  auto raw = makeRaw(makeWireFixture(std::array{clearRecord()}), 4u,
                     /*captureIdentity=*/true);
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK,
        "capture-failure fixture publishes actual work");
  fixture.cDevice->renderTapeIdentityCapture.fail(0xfeedu);
  check(!fixture.routing->queue_.hasCompletedPendingWork(),
        "published but uncompleted work cannot pass the cold completion check");
  // No-op flush is deliberately insufficient. This is a no-thread fixture;
  // the production check must detect the incomplete real queue waterline.
  check(dxmt9c_device_wait_for_pending_work(fixture.cDevice.get()) < 0,
        "flush return alone is not proof of successful completion");
  fixture.routing->flushHook = [&] {
    const auto ready = dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
        fixture.routing->queue_);
    for (std::size_t i = 0u; i < ready; ++i) {
      const auto completion = dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
      check(completion.submitted && completion.completed && completion.reclaimed,
            "fallback fence uses actual source completion and reclaim");
    }
  };
  check(dxmt9c_device_wait_for_pending_work(fixture.cDevice.get()) == D3D_OK &&
            fixture.routing->queue_.hasCompletedPendingWork() &&
            fixture.routing->clearCalls == 1u,
        "rejected capture retains the application fence without repeating work");
  D9CRenderTapeIdentityCaptureResult capture{};
  check(!fixture.cDevice->renderTapeIdentityCapture.copy(0xfeedu, capture, {}),
        "unqualified completion cannot turn rejected capture into valid evidence");
  fixture.routing->queue_.failStopCpuReadyArena();
  check(!fixture.routing->queue_.hasCompletedPendingWork(),
        "terminal stop cannot masquerade as a successful flush");
}

void mixedSourceLeaseSelectionPreservesSourceOrder() {
  // This is the queue API coordinate-contract pin: a hand-built source may
  // contain multiple physical segments, even though the production planner's
  // current equal 64-page source/segment bounds commonly emit one segment per
  // source for canonical replay. The capturedLargeRaw test above is the
  // production replayRawChunk multi-source pin; this test does not claim that
  // production fixture exercises a mixed source shape.
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/true);
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1u;
  capacity.clearRecords = 1u;
  const auto segment = makeSourcePayloadLayout(
      capacity, fixture.routing->queue_.cpuReadyArenaPlanLimits().pageSize,
      fixture.routing->queue_.cpuReadyArenaPlanLimits()
          .maxOrdinaryPagesPerSegment);
  check(segment.has_value(), "mixed-source selection segment must build");
  const std::array firstSourceSegments{*segment, *segment};
  const auto firstSource = makeArenaSourcePayloadLayout(
      firstSourceSegments, fixture.routing->queue_.cpuReadyArenaPlanLimits()
                               .pageSize,
      fixture.routing->queue_.cpuReadyArenaPlanLimits().maxPagesPerSource);
  const std::array secondSourceSegments{*segment};
  const auto secondSource = makeArenaSourcePayloadLayout(
      secondSourceSegments, fixture.routing->queue_.cpuReadyArenaPlanLimits()
                                .pageSize,
      fixture.routing->queue_.cpuReadyArenaPlanLimits().maxPagesPerSource);
  check(firstSource.has_value() && secondSource.has_value(),
        "mixed-source selection layouts must build");
  const std::array layouts{*firstSource, *secondSource};
  auto begin = fixture.routing->queue_.beginCpuReadyArenaSources(44u, layouts);
  check(begin.has_value(), "mixed-source selection batch must admit");
  check(begin->selectSourceSegment(0u, 0u) &&
            begin->selectSourceSegment(0u, 1u) &&
            begin->selectSourceSegment(1u, 0u),
        "source-local segment selection must cross a multi-segment source "
        "only at its exact next source edge");
  begin->abortForFallback();
}

void providerResolvedEntryRoutesExistingClearPresent() {
  RuntimeFixture fixture;
  const std::array records{clearRecord(), presentRecord()};
  const auto wire = makeWireFixture(records);
  const auto before = wire.bytes;
  dxmt9::core::CopyMaterializationLedger peLedger;
  dxmt9::core::CopyMaterializationLedger unixLedger;
  dxmt9::core::ScopedCopyMaterializationLedger observeUnix(
      dxmt9::core::CopyMaterializationOwner::Unix, unixLedger);
  const auto hr = dxmt9::d3d9::replayPrevalidatedResolvedCommandChunk(
      fixture.cDevice.get(), wire.bytes, wire.envelope, {});
  check(hr == D3D_OK && fixture.routing->clearCalls == 1 &&
            fixture.routing->presentCalls == 1,
        "provider entry must route Clear and Present through DeviceReplaySink");
  check(wire.bytes == before,
        "provider entry must not rewrite canonical command bytes");
  const auto unixRaw = unixLedger.snapshot(
      dxmt9::core::CopyMaterializationClass::BridgeRawOwnership);
  const auto peRaw = peLedger.snapshot(
      dxmt9::core::CopyMaterializationClass::BridgeRawOwnership);
  check(unixRaw.calls == 1u && unixRaw.bytes == wire.bytes.size() &&
            peRaw.calls == 0u && peRaw.bytes == 0u,
        "provider replay routes bridge raw ownership to Unix only");
}

void providerRoutePinsDirectCompatibilityAndMalformedIdentity() {
  // This is the provider-local route pin: all lanes enter the actual
  // prevalidated provider entry with the same canonical bytes and resolved
  // object role. ProductionSlot has two independent implementations: the
  // populated CPU-ready Arena and the ordinary direct ChunkSlot.
  RuntimeFixture arenaFixture(/*rejectAfterClear=*/false,
                              /*segmentSerial=*/false,
                              /*directChunkSlot=*/false,
                              /*cpuReadyArena=*/true);
  RuntimeFixture directFixture(/*rejectAfterClear=*/false,
                               /*segmentSerial=*/false,
                               /*directChunkSlot=*/true,
                               /*cpuReadyArena=*/false);
  dxmt9::CommandQueueArenaLeaseTestAccess::enableDirectLifecycleObservation(
      directFixture.routing->queue_);
  RuntimeFixture compatibilityFixture(/*rejectAfterClear=*/false,
                                      /*segmentSerial=*/false,
                                      /*directChunkSlot=*/true,
                                      /*cpuReadyArena=*/false);
  auto* arenaBuffer = dxmt9c_device_create_vertex_buffer(
      arenaFixture.cDevice.get(), 256u, 0u, 0u, 0u);
  auto* directBuffer = dxmt9c_device_create_vertex_buffer(
      directFixture.cDevice.get(), 256u, 0u, 0u, 0u);
  auto* compatibilityBuffer = dxmt9c_device_create_vertex_buffer(
      compatibilityFixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(arenaBuffer != nullptr && directBuffer != nullptr &&
            compatibilityBuffer != nullptr,
        "provider route buffers must construct");

  // The object-id/generation is canonical wire identity; each provider call
  // supplies its local resolved wrapper at the same handle-table position.
  const D9CWireObjectIdentity wireIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
      .generation = 1u,
      .objectId = 1u};
  const auto records = std::array{drawRecord(wireIdentity)};
  auto directWire = makeWireFixture(records);
  directWire.envelope.producerIdentity = {
      .firstEventOrdinal = 17u,
      .lastEventOrdinal = 17u,
      .firstSourceOrdinal = 3u,
      .lastSourceOrdinal = 3u};
  const auto compatibilityWire = directWire;
  const auto arenaWire = directWire;
  std::array<void*, 1> arenaObjects{arenaBuffer};
  std::array<void*, 1> directObjects{directBuffer};
  std::array<void*, 1> compatibilityObjects{compatibilityBuffer};

  check(dxmt9::d3d9::replayPrevalidatedResolvedCommandChunk(
            arenaFixture.cDevice.get(), arenaWire.bytes, arenaWire.envelope,
            arenaObjects, dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot) ==
            D3D_OK,
        "provider ProductionSlot Arena route must accept qualified input");
  arenaFixture.routing->present(SwapDesc{});
  const auto arenaCompletion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          arenaFixture.routing->queue_);
  check(arenaCompletion.arena && arenaCompletion.reclaimed &&
            arenaFixture.routing->legacyMarkCalls == 0u,
        "ProductionSlot Arena route must publish an Arena completion without "
        "the compatibility mark path");

  check(dxmt9::d3d9::replayPrevalidatedResolvedCommandChunk(
            directFixture.cDevice.get(), directWire.bytes,
            directWire.envelope, directObjects,
            dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot) == D3D_OK,
        "provider ProductionSlot direct ChunkSlot route must accept qualified input");
  const auto directWritingCommands =
      dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
          directFixture.routing->queue_);
  check(directWritingCommands != 0u &&
            directFixture.routing->combinedMarkCalls == 0u &&
            directFixture.routing->captureCalls == 1u,
        "ProductionSlot direct ChunkSlot must reach a direct-owned route before effects");
  const auto directLifecycle =
      dxmt9::CommandQueueArenaLeaseTestAccess::directLifecycleState(
          directFixture.routing->queue_);
  check(directLifecycle.recordCount != 0u &&
            directLifecycle.records[0].phase !=
                dxmt9::queue::DirectSourcePhase::Empty,
        "ProductionSlot direct ChunkSlot must expose a committed lifecycle witness");

  check(dxmt9::d3d9::replayPrevalidatedResolvedCommandChunk(
            compatibilityFixture.cDevice.get(), compatibilityWire.bytes,
            compatibilityWire.envelope, compatibilityObjects,
            dxmt9::d3d9::ProviderReplayPolicy::Compatibility) == D3D_OK,
        "provider Compatibility route must accept the same canonical input");
  check(compatibilityFixture.routing->drawCalls == 1u &&
            compatibilityFixture.routing->combinedMarkCalls == 0u &&
            compatibilityFixture.routing->legacyMarkCalls == 1u,
        "provider Compatibility must establish marks at the selected boundary");

  directFixture.routing->present(SwapDesc{});
  compatibilityFixture.routing->present(SwapDesc{});
  const auto directCompletion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          directFixture.routing->queue_);
  const auto compatibilityCompletion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          compatibilityFixture.routing->queue_);
  check(!directCompletion.arena && directCompletion.reclaimed &&
            !compatibilityCompletion.arena && compatibilityCompletion.reclaimed &&
            directCompletion.commandCount == compatibilityCompletion.commandCount &&
            directCompletion.commandKinds == compatibilityCompletion.commandKinds &&
            directCompletion.effectiveDigest ==
                compatibilityCompletion.effectiveDigest,
        "provider Direct/Compatibility must preserve ordered command semantics");
  check(directFixture.routing->queue_.pool().findBuffer(
              directBuffer->obj->handle().value) != nullptr &&
            compatibilityFixture.routing->queue_.pool().findBuffer(
                compatibilityBuffer->obj->handle().value) != nullptr,
        "provider route resource identities must remain qualified through completion");

  dxmt9c_buffer_release(arenaBuffer);
  // A partially populated producer interval is rejected before import,
  // wrapper retention, queue admission, or any DeviceReplaySink call.
  RuntimeFixture malformedFixture(/*rejectAfterClear=*/false,
                                  /*segmentSerial=*/false,
                                  /*directChunkSlot=*/true,
                                  /*cpuReadyArena=*/false);
  auto* malformedBuffer = dxmt9c_device_create_vertex_buffer(
      malformedFixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(malformedBuffer != nullptr, "malformed provider buffer must construct");
  auto malformedWire = directWire;
  malformedWire.envelope.producerIdentity = {
      .firstEventOrdinal = 17u,
      .lastEventOrdinal = 17u,
      .firstSourceOrdinal = 0u,
      .lastSourceOrdinal = 3u};
  std::array<void*, 1> malformedObjects{malformedBuffer};
  const auto malformedBytes = malformedWire.bytes;
  check(dxmt9::d3d9::replayPrevalidatedResolvedCommandChunk(
            malformedFixture.cDevice.get(), malformedWire.bytes,
            malformedWire.envelope, malformedObjects,
            dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot) < D3D_OK &&
            malformedWire.bytes == malformedBytes &&
            malformedFixture.routing->drawCalls == 0u &&
            malformedFixture.routing->presentCalls == 0u &&
            malformedFixture.routing->combinedMarkCalls == 0u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                malformedFixture.routing->queue_) == 0u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
                malformedFixture.routing->queue_) == 1u &&
            !malformedFixture.routing->queue_.cpuReadyArenaPoisoned(),
        "partial producer identity must fail closed without provider effects");

  dxmt9c_buffer_release(directBuffer);
  dxmt9c_buffer_release(compatibilityBuffer);
  dxmt9c_buffer_release(malformedBuffer);
}

void oversizeSegmentedPresentTakesOneLegacyRollbackSource() {
  RuntimeFixture fixture;
  // 17K D3D rects exceed the fixed 64-page ordinary Direct footprint. The
  // complete raw therefore rolls back before construction and replays once
  // through the ordered Legacy lane, including its final Present.
  const std::array records{clearRecord(17000), presentRecord()};
  auto raw = makeRaw(makeWireFixture(records), 1);
  const auto hr = dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw);
  check(hr == D3D_OK && fixture.routing->clearCalls == 1 &&
            fixture.routing->presentCalls == 1 &&
            fixture.routing->lastPresentSeqId == 1,
        "oversize rollback applies Clear and final Present exactly once at "
        "the reserved seq");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
            fixture.routing->queue_) == 1 &&
            dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
                fixture.routing->queue_) == 2,
        "oversize rollback publishes one Legacy Ready source and consumes "
        "one queue sequence");

  const auto completion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(completion.dequeued && !completion.arena &&
            completion.arenaSegmentCount == 0 &&
            completion.commandCount == 2 && !completion.finalPresent &&
            completion.submitted && completion.completed &&
            completion.reclaimed && completion.seqId == 1,
        "oversize Clear/Present rolls back to one ordered Legacy source and "
        "one completion identity");
}

void productionGateIsExplicitAndDefaultOff() {
  check(!dxmt9::resolveCpuReadyTapeDirectReplayEnabled(nullptr) &&
            !dxmt9::resolveCpuReadyTapeDirectReplayEnabled("") &&
            !dxmt9::resolveCpuReadyTapeDirectReplayEnabled("0") &&
            dxmt9::resolveCpuReadyTapeDirectReplayEnabled("1") &&
            dxmt9::resolveCpuReadyTapeDirectReplayEnabled("yes") &&
            dxmt9::resolveCpuReadyTapeDirectReplayEnabled("01") &&
            dxmt9::resolveCpuReadyTapeDirectReplayEnabled("0foo"),
        "CPU-ready Tape promotion gate must be unset/zero off and explicit "
        "non-zero on");
  const std::array<const char*, 7> values{nullptr, "", "0", "1", "yes", "01", "0foo"};
  const std::array<const char*, 5> captures{nullptr, "", "0", "1", "yes"};
  const std::array<const char*, 3> roots{nullptr, "", "Z:\\\\capture"};
  for (const auto* value : values) {
    for (const auto* capture : captures) {
      for (const auto* root : roots) {
        check(dxmt9::resolveCpuReadyTapeDirectReplayEnabled(value, capture, root) ==
                  dxmt9::resolveCpuReadyTapeDirectReplayEnabled(value),
              "Render Tape publication configuration never changes the "
              "independently selected CPU-ready source provider");
      }
    }
  }
  check(!dxmt9::resolveRenderTapePublisherCaptureEnabled(nullptr, "Z:\\\\capture") &&
            !dxmt9::resolveRenderTapePublisherCaptureEnabled("0", "Z:\\\\capture") &&
            !dxmt9::resolveRenderTapePublisherCaptureEnabled("1", nullptr) &&
            !dxmt9::resolveRenderTapePublisherCaptureEnabled("1", "") &&
            dxmt9::resolveRenderTapePublisherCaptureEnabled("1", "Z:\\\\capture"),
        "capture and an output root still independently enable the publisher");
}

void resourceBearingDirectCapturesThenMarksExactTicketAndPublishes() {
  RuntimeFixture fixture;
  const auto admissionThread = std::this_thread::get_id();
  auto buffer = fixture.device->CreateBuffer(BufferDesc{
      .size = 256u,
      .pool = Pool::Default,
      .usage = UsageVertexBuffer,
  });
  check(buffer != nullptr, "resource-bearing fixture buffer must construct");
  D9CBuffer wireBuffer(buffer, fixture.cDevice.get());
  const std::array records{drawRecord(wireBuffer.wireIdentity)};
  const auto wire = makeWireFixture(records);
  D9CCommandChunk chunk{
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = wire.envelope.recordCount,
      .recordBytes = static_cast<std::uint32_t>(wire.bytes.size()),
      .records = toWireHandle(wire.bytes.data()),
      .handleCount = wire.envelope.handleCount,
  };

  const auto status = dxmt9c_device_commit_chunk(
      fixture.cDevice.get(), &chunk);
  check(status == D3D_OK,
        "resource-bearing Direct admission must accept canonical wire data");
  dxmt9::d3d9::drainDeferredReplay(
      fixture.cDevice.get(), "cpu-ready-resource-direct");

  const auto* record = fixture.routing->queue_.pool().findBuffer(
      buffer->handle().value);
  check(fixture.routing->captureCalls == 1u &&
            fixture.routing->captureThread == admissionThread &&
            fixture.routing->capturedResources.size() == 1u &&
            fixture.routing->capturedResources.front().kind ==
                ChunkHandleKind::Buffer &&
            fixture.routing->capturedResources.front().handle ==
                buffer->handle() &&
            fixture.routing->capturedBufferLastUsedSeq == 0u,
        "Direct admission must capture retained resource inputs on the app "
        "thread without prematurely marking a queue sequence");
  check(record != nullptr && record->lastUsedSeqId == 1u &&
            fixture.routing->legacyMarkCalls == 0u &&
            fixture.routing->drawCalls == 1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                fixture.routing->queue_) == 1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
                fixture.routing->queue_) == 2u,
        "Direct publication must stamp the exact reserved seq and publish "
        "without the Legacy current-next mark path");

  const auto completion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(completion.dequeued && completion.arena && completion.submitted &&
            completion.completed && completion.reclaimed &&
            completion.seqId == 1u,
        "resource-bearing Direct source must complete and reclaim by its "
        "published arena identity");
}

void ordinaryIngressDefersMarksUntilReplay() {
  for (const bool direct : {false, true}) {
    RuntimeFixture fixture(false, false, direct, false);
    auto buffer = fixture.device->CreateBuffer(BufferDesc{
        .size = 256u, .pool = Pool::Default, .usage = UsageVertexBuffer});
    check(buffer != nullptr, "ordinary ingress buffer must construct");
    D9CBuffer wireBuffer(buffer, fixture.cDevice.get());
    const std::array records{drawRecord(wireBuffer.wireIdentity)};
    const auto wire = makeWireFixture(records);
    D9CCommandChunk chunk{
        .version = D9C_COMMAND_CHUNK_VERSION,
        .recordCount = wire.envelope.recordCount,
        .recordBytes = static_cast<std::uint32_t>(wire.bytes.size()),
        .records = toWireHandle(wire.bytes.data()),
        .handleCount = wire.envelope.handleCount};
    const auto appThread = std::this_thread::get_id();
    check(dxmt9c_device_commit_chunk(fixture.cDevice.get(), &chunk) == D3D_OK,
          "ordinary capture-only ingress must accept input");
    check(dxmt9::d3d9::drainDeferredReplay(
              fixture.cDevice.get(), "deferred-ingress-test"),
          "ordinary capture-only Replay must finish");
    const auto* resource =
        fixture.routing->queue_.pool().findBuffer(buffer->handle().value);
    check(fixture.routing->combinedMarkCalls == 0u &&
              fixture.routing->captureCalls == 1u &&
              fixture.routing->captureThread == appThread &&
              fixture.routing->capturedBufferLastUsedSeq == 0u &&
              resource && resource->lastUsedSeqId != 0u,
          "ordinary ingress must capture unstamped input and Replay must "
          "establish execution-use coverage");
    if (!direct) {
      check(fixture.routing->legacyMarkCalls == 1u &&
                fixture.routing->legacyMarkThread != appThread,
            "compatibility bulk marking must move to the Replay worker");
    }
  }
}

// Uses the worker's existing ReplayFn seam only to park before real dispatch
// and inject a terminal result. The successful/partial-effect paths still run
// replayRawChunk, including production direct/compatibility publication.
struct IngressHandoffGate {
  enum class Failure { None, BeforeReplay, AfterReplay };
  inline static IngressHandoffGate* active = nullptr;
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool released = false;
  Failure failure = Failure::None;
  std::uint32_t calls = 0;
  bool protectedAtReturn = true;

  void release() {
    std::lock_guard lock(mutex);
    released = true;
    cv.notify_all();
  }

  static int32_t replay(D9CDevice* device,
                        dxmt9::d3d9::RawCommandChunk& raw) {
    auto& gate = *active;
    {
      std::unique_lock lock(gate.mutex);
      gate.entered = true;
      gate.cv.notify_all();
      gate.cv.wait(lock, [&] { return gate.released; });
    }
    ++gate.calls;
    if (gate.failure == Failure::BeforeReplay) return D3DERR_INVALIDCALL;
    const auto hr = dxmt9::d3d9::replayRawChunk(device, raw);
    auto& pool = device->dev().upperDevice()->queue().pool();
    for (const auto& entry : raw.resourceEntries) {
      if (entry.kind != ChunkHandleKind::Buffer) continue;
      const auto* record = pool.findBuffer(entry.handle.value);
      gate.protectedAtReturn &= record && record->lastUsedSeqId != 0u;
    }
    gate.protectedAtReturn &= !raw.retainedWrappers.empty();
    return gate.failure == Failure::AfterReplay ? D3DERR_INVALIDCALL : hr;
  }
};

void deferredIngressAdoptionAndFailureKeepRawOwnership() {
  using Failure = IngressHandoffGate::Failure;
  for (const bool direct : {false, true}) {
    for (const auto failure :
         {Failure::None, Failure::BeforeReplay, Failure::AfterReplay}) {
      IngressHandoffGate gate;
      gate.failure = failure;
      RuntimeFixture fixture(false, false, direct, false);
      auto first = fixture.device->CreateBuffer(BufferDesc{
          .size = 256u, .pool = Pool::Default, .usage = UsageVertexBuffer});
      auto second = fixture.device->CreateBuffer(BufferDesc{
          .size = 256u, .pool = Pool::Default, .usage = UsageVertexBuffer});
      check(first && second, "handoff buffers must construct");
      // Stack wrappers outlive the worker; test refs must settle to exactly 1.
      D9CBuffer firstWire(first, fixture.cDevice.get());
      D9CBuffer secondWire(second, fixture.cDevice.get());
      IngressHandoffGate::active = &gate;
      fixture.cDevice->replayOffload =
          std::make_unique<dxmt9::d3d9::ReplayOffloadWorker>(
              &IngressHandoffGate::replay);
      auto& worker = *fixture.cDevice->replayOffload;
      struct Settlement {
        IngressHandoffGate& gate;
        dxmt9::d3d9::ReplayOffloadWorker& worker;
        ~Settlement() {
          gate.release();
          worker.stop();
          IngressHandoffGate::active = nullptr;
        }
      } settlement{gate, worker};
      check(worker.start(fixture.cDevice.get()), "handoff worker must start");
      const auto commit = [&](D9CBuffer& buffer) {
        const std::array records{drawRecord(buffer.wireIdentity)};
        const auto wire = makeWireFixture(records);
        const D9CCommandChunk chunk{
            .version = D9C_COMMAND_CHUNK_VERSION,
            .recordCount = wire.envelope.recordCount,
            .recordBytes = static_cast<std::uint32_t>(wire.bytes.size()),
            .records = toWireHandle(wire.bytes.data()),
            .handleCount = wire.envelope.handleCount};
        return dxmt9c_device_commit_chunk(fixture.cDevice.get(), &chunk);
      };
      check(commit(firstWire) == D3D_OK,
            "first ingress must return while Replay is parked");
      {
        std::unique_lock lock(gate.mutex);
        check(gate.cv.wait_for(lock, std::chrono::seconds(5),
                              [&] { return gate.entered; }),
              "worker must reach the deterministic pre-Replay gate");
      }
      check(commit(secondWire) == D3D_OK,
            "second ingress must transfer ownership while first is parked");
      auto& ledger = fixture.cDevice->replayDrainLedger;
      check(firstWire.refs.load() == 2u && secondWire.refs.load() == 2u &&
                ledger.pending(*firstWire.replayDrainTarget) &&
                ledger.pending(*secondWire.replayDrainTarget),
            "accepted raw entries must own refs and publish mutation targets");
      check(fixture.routing->combinedMarkCalls == 0u &&
                fixture.routing->legacyMarkCalls == 0u &&
                fixture.routing->queue_.pool().findBuffer(
                    first->handle().value)->lastUsedSeqId == 0u &&
                fixture.routing->queue_.pool().findBuffer(
                    second->handle().value)->lastUsedSeqId == 0u,
            "accepted pending work must require neither ingress mark nor "
            "Replay/GPU completion to keep its objects alive");
      gate.release();
      worker.stop(); // joins and settles both active and unreplayed entries
      check(firstWire.refs.load() == 1u && secondWire.refs.load() == 1u,
            "success and poison cleanup must each release raw refs once");
      check(gate.calls == (failure == Failure::None ? 2u : 1u) &&
                worker.failed() == (failure != Failure::None),
            "failure must discard the queued successor without replay/retry");
      check(gate.protectedAtReturn,
            "real Replay must establish marks while raw refs are still held");
      if (failure != Failure::None) {
        check(ledger.terminal() &&
                  fixture.routing->queue_.pool().findBuffer(
                      second->handle().value)->lastUsedSeqId == 0u,
              "terminal cleanup must not invent use of an unreplayed source");
      }
    }
    // The opposite ownership boundary: a stopped queue never adopts the raw.
    // Retention acquired during import must unwind locally, not via a worker.
    RuntimeFixture rejected(false, false, direct, false);
    auto buffer = rejected.device->CreateBuffer(BufferDesc{
        .size = 256u, .pool = Pool::Default, .usage = UsageVertexBuffer});
    check(buffer != nullptr, "rejected-ingress buffer must construct");
    D9CBuffer wireBuffer(buffer, rejected.cDevice.get());
    rejected.cDevice->replayOffload =
        std::make_unique<dxmt9::d3d9::ReplayOffloadWorker>();
    rejected.cDevice->replayOffload->queue().stop();
    const std::array records{drawRecord(wireBuffer.wireIdentity)};
    const auto wire = makeWireFixture(records);
    const D9CCommandChunk chunk{
        .version = D9C_COMMAND_CHUNK_VERSION,
        .recordCount = wire.envelope.recordCount,
        .recordBytes = static_cast<std::uint32_t>(wire.bytes.size()),
        .records = toWireHandle(wire.bytes.data()),
        .handleCount = wire.envelope.handleCount};
    check(dxmt9c_device_commit_chunk(rejected.cDevice.get(), &chunk) < D3D_OK &&
              wireBuffer.refs.load() == 1u &&
              !rejected.cDevice->replayDrainLedger.pending(
                  *wireBuffer.replayDrainTarget) &&
              rejected.routing->captureCalls == 1u &&
              rejected.routing->combinedMarkCalls == 0u &&
              rejected.routing->queue_.pool().findBuffer(
                  buffer->handle().value)->lastUsedSeqId == 0u,
          "pre-adoption refusal must release locally without pending/use marks");
  }
}

void sameRawLegacyAndDirectProductionOracle() {
  RuntimeFixture fixture;
  auto* source = dxmt9c_device_create_render_target(
      fixture.cDevice.get(), 16u, 16u, 21u, 0u, 0u, 0u, nullptr);
  auto* destination = dxmt9c_device_create_render_target(
      fixture.cDevice.get(), 16u, 16u, 21u, 0u, 0u, 0u, nullptr);
  check(source != nullptr && destination != nullptr,
        "same-raw oracle surfaces must construct");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto sourceIdentity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_SURFACE, source);
  const auto destinationIdentity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_SURFACE, destination);
  check(sourceIdentity.objectId != 0u && destinationIdentity.objectId != 0u,
        "same-raw oracle must register both surface identities");
  const std::array records{
      clearRecord(), stretchRectRecord(sourceIdentity, destinationIdentity),
      presentRecord()};
  const auto wire = makeWireFixture(records);

  // Both lanes start from independent RawOwned wrappers over the exact same
  // immutable validated bytes; only the routing disposition differs.
  auto direct = makeRaw(wire, 61u, false, &registry);
  auto legacy = makeRaw(wire, 61u, false, &registry);
  const std::array resources{
      ChunkHandleEntry{.kind = ChunkHandleKind::Surface,
                       .handle = source->obj->handle()},
      ChunkHandleEntry{.kind = ChunkHandleKind::Surface,
                       .handle = destination->obj->handle()}};
  direct.resourceEntries.assign(resources.begin(), resources.end());
  legacy.resourceEntries.assign(resources.begin(), resources.end());
  legacy.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), direct) == D3D_OK,
        "same-raw Direct oracle must replay successfully");
  const auto directCompletion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), legacy) == D3D_OK,
        "same-raw Legacy oracle must replay successfully");
  const auto legacyCompletion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(directCompletion.dequeued && directCompletion.arena &&
            directCompletion.submitted && directCompletion.completed &&
            directCompletion.reclaimed &&
            legacyCompletion.dequeued && !legacyCompletion.arena &&
            legacyCompletion.submitted && legacyCompletion.completed &&
            legacyCompletion.reclaimed,
        "same-raw lanes must both complete and reclaim their source");
  // Direct admission publishes the original raw identity. Compatibility
  // publication predates that identity domain and intentionally leaves it
  // absent; the oracle normalizes that optional field to the known input raw
  // rather than fabricating a Legacy queue identity. PresentId is likewise a
  // runtime-issued ordinal, so the payload digest compares only the original
  // Present fields above.
  check(directCompletion.rawOrdinal == 61u &&
            legacyCompletion.rawOrdinal == 0u &&
            directCompletion.sourceOrdinal == directCompletion.seqId &&
            legacyCompletion.sourceOrdinal == legacyCompletion.seqId &&
            directCompletion.sourceOrdinal - directCompletion.seqId ==
                legacyCompletion.sourceOrdinal - legacyCompletion.seqId &&
            directCompletion.commandCount == legacyCompletion.commandCount &&
            directCompletion.effectiveDigest ==
                legacyCompletion.effectiveDigest &&
            directCompletion.hasPresent && legacyCompletion.hasPresent,
        "same-raw lanes must preserve lane-normalized ordinals, command layout, "
        "payload bytes, and Present barrier");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
            fixture.routing->queue_) == 0u,
        "a completed source must leave no duplicate Ready publication");
  check(wire.bytes == makeWireFixture(records).bytes &&
            direct.resourceEntries.size() == legacy.resourceEntries.size() &&
            direct.resourceEntries.size() == 2u &&
            direct.resourceEntries[0].kind == ChunkHandleKind::Surface &&
            direct.resourceEntries[0].handle ==
                legacy.resourceEntries[0].handle &&
            direct.resourceEntries[1].handle ==
                legacy.resourceEntries[1].handle &&
            fixture.routing->legacyMarkCalls == 1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
                fixture.routing->queue_) == 0u,
        "same-raw lanes must retain the exact resource identity through the "
        "Direct/Legacy mark paths and release all Tape residency");

  // Readback is a synchronous observation and therefore intentionally cannot
  // be a Direct-Arena child. Keep it in the same production oracle as an
  // explicit barrier disposition over the same registered resource identities
  // so the differential does not accidentally claim an impossible lane.
  const std::array barrierRecords{
      clearRecord(), stretchRectRecord(sourceIdentity, destinationIdentity),
      readbackRecord(sourceIdentity, destinationIdentity, 2u), presentRecord()};
  const auto barrierWire = makeWireFixture(barrierRecords);
  dxmt9::d3d9::ImportedChunkView barrierImported;
  check(validateCommandChunk(barrierWire.bytes, barrierWire.envelope,
                             &barrierImported)
            .valid(),
        "same-raw Readback barrier fixture must validate");
  const auto barrierPlan = dxmt9::d3d9::planCpuReadyChunk(barrierImported, 61u);
  check(barrierPlan.containsOrderedControls &&
            barrierPlan.lane == dxmt9::d3d9::ReplayLane::Inline &&
            barrierPlan.reason == dxmt9::d3d9::ReplayReason::Readback,
        "same-raw Readback must retain its synchronous Inline barrier");
  dxmt9::d3d9::releaseRetainedWrappers(direct);
  dxmt9::d3d9::releaseRetainedWrappers(legacy);
  dxmt9c_surface_release(source);
  dxmt9c_surface_release(destination);
}

void ordinaryChunkSlotDirectMatchesLegacyCadenceAndCompletion() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  RuntimeFixture legacyFixture(/*rejectAfterClear=*/false,
                               /*segmentSerial=*/false,
                               /*directChunkSlot=*/false);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  auto* source = dxmt9c_device_create_render_target(
      fixture.cDevice.get(), 16u, 16u, 21u, 0u, 0u, 0u, nullptr);
  auto* destination = dxmt9c_device_create_render_target(
      fixture.cDevice.get(), 16u, 16u, 21u, 0u, 0u, 0u, nullptr);
  check(buffer != nullptr && source != nullptr && destination != nullptr,
        "ordinary direct/Legacy draw and copy resources construct");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  const auto sourceIdentity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_SURFACE, source);
  const auto destinationIdentity =
      registry.insert(D9C_CHUNK_HANDLE_KIND_SURFACE, destination);
  const std::array records{
      drawRecord(identity),
      applyRenderStateRecord(RS_TEXTURE_FACTOR, 0x01020304u),
      drawRecord(identity, 1u)};
  const auto wire = makeWireFixture(records);
  auto directRaw = makeRaw(wire, 71u, false, &registry);
  auto* legacyBuffer = dxmt9c_device_create_vertex_buffer(
      legacyFixture.cDevice.get(), 256u, 0u, 0u, 0u);
  auto* legacySource = dxmt9c_device_create_render_target(
      legacyFixture.cDevice.get(), 16u, 16u, 21u, 0u, 0u, 0u, nullptr);
  auto* legacyDestination = dxmt9c_device_create_render_target(
      legacyFixture.cDevice.get(), 16u, 16u, 21u, 0u, 0u, 0u, nullptr);
  check(legacyBuffer != nullptr && legacySource != nullptr &&
            legacyDestination != nullptr,
        "ordinary Legacy differential resources construct");
  dxmt9::d3d9::WireObjectRegistry legacyRegistry;
  const auto legacyIdentity = legacyRegistry.insert(
      D9C_CHUNK_HANDLE_KIND_BUFFER, legacyBuffer);
  const auto legacySourceIdentity = legacyRegistry.insert(
      D9C_CHUNK_HANDLE_KIND_SURFACE, legacySource);
  const auto legacyDestinationIdentity = legacyRegistry.insert(
      D9C_CHUNK_HANDLE_KIND_SURFACE, legacyDestination);
  (void)legacySourceIdentity;
  (void)legacyDestinationIdentity;
  const std::array legacyRecords{
      drawRecord(legacyIdentity),
      applyRenderStateRecord(RS_TEXTURE_FACTOR, 0x01020304u),
      drawRecord(legacyIdentity, 1u)};
  const auto legacyWire = makeWireFixture(legacyRecords);
  auto legacyRaw = makeRaw(legacyWire, 71u, false, &legacyRegistry);
  const std::array resources{
      ChunkHandleEntry{.kind = ChunkHandleKind::Buffer,
                       .handle = buffer->obj->handle()},
      ChunkHandleEntry{.kind = ChunkHandleKind::Surface,
                       .handle = source->obj->handle()},
      ChunkHandleEntry{.kind = ChunkHandleKind::Surface,
                       .handle = destination->obj->handle()},
  };
  const std::array legacyResources{
      ChunkHandleEntry{.kind = ChunkHandleKind::Buffer,
                       .handle = legacyBuffer->obj->handle()},
      ChunkHandleEntry{.kind = ChunkHandleKind::Surface,
                       .handle = legacySource->obj->handle()},
      ChunkHandleEntry{.kind = ChunkHandleKind::Surface,
                       .handle = legacyDestination->obj->handle()},
  };
  directRaw.resourceEntries.assign(resources.begin(), resources.end());
  legacyRaw.resourceEntries.assign(legacyResources.begin(), legacyResources.end());
  directRaw.cpuReadyTapePlanningEnabled = false;
  legacyRaw.cpuReadyTapePlanningEnabled = false;

  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), directRaw) == D3D_OK,
        "ordinary direct-ChunkSlot raw replays successfully");
  const auto directWritingCommandCount =
      dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
          fixture.routing->queue_);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                fixture.routing->queue_) == 0u &&
            directWritingCommandCount != 0u &&
            fixture.routing->drawCalls == 0u,
        "direct Draw/APPLY_STATE/Draw construction preserves the ordinary "
        "source/CB publication cadence");
  check(fixture.routing->legacyMarkCalls == 0u,
        "direct commit owns exact resource closure without Legacy pre-marking");
  const auto directDrawDigest =
      dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawDigest(
          fixture.routing->queue_);

  fixture.routing->present(SwapDesc{});
  const auto directCompletion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(dxmt9::d3d9::replayRawChunk(legacyFixture.cDevice.get(), legacyRaw) ==
                D3D_OK &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                legacyFixture.routing->queue_) == directWritingCommandCount &&
            legacyFixture.routing->drawCalls == 2u &&
            legacyFixture.routing->legacyMarkCalls == 1u,
        "the same raw takes the typed Legacy lane on a fresh equivalent fixture");
  const auto legacyDrawDigest =
      dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawDigest(
          legacyFixture.routing->queue_);
  legacyFixture.routing->present(SwapDesc{});
  const auto legacyCompletion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          legacyFixture.routing->queue_);
  check(directCompletion.dequeued && !directCompletion.arena &&
            directCompletion.submitted && directCompletion.completed &&
            directCompletion.reclaimed &&
            legacyCompletion.dequeued && !legacyCompletion.arena &&
            legacyCompletion.submitted && legacyCompletion.completed &&
            legacyCompletion.reclaimed &&
            directCompletion.commandCount == directWritingCommandCount + 1u &&
            directCompletion.commandCount == legacyCompletion.commandCount &&
            directCompletion.rawOrdinal == legacyCompletion.rawOrdinal &&
            directCompletion.sourceOrdinal == legacyCompletion.sourceOrdinal &&
            directCompletion.seqId == legacyCompletion.seqId &&
            directCompletion.effectiveDigest == legacyCompletion.effectiveDigest &&
            directDrawDigest.params == legacyDrawDigest.params &&
            directDrawDigest.record == legacyDrawDigest.record &&
            directDrawDigest.pso == legacyDrawDigest.pso &&
            directDrawDigest.hot == legacyDrawDigest.hot &&
            directDrawDigest.payload == legacyDrawDigest.payload &&
            directDrawDigest.payloadBytes == legacyDrawDigest.payloadBytes,
        "ordinary direct and Legacy final ChunkSlots preserve completion and normalized draw semantics: "
        "direct count=" + std::to_string(directCompletion.commandCount) +
        " legacy count=" + std::to_string(legacyCompletion.commandCount) +
        " direct seq=" + std::to_string(directCompletion.seqId) +
        " legacy seq=" + std::to_string(legacyCompletion.seqId) +
        " direct digest=" + std::to_string(directCompletion.effectiveDigest) +
        " legacy digest=" + std::to_string(legacyCompletion.effectiveDigest) +
        " buffer=" + std::to_string(buffer->obj->handle().value) +
        " record=" + std::to_string(directDrawDigest.record) + "/" +
            std::to_string(legacyDrawDigest.record) +
        " pso=" + std::to_string(directDrawDigest.pso) + "/" +
            std::to_string(legacyDrawDigest.pso) +
        " hot=" + std::to_string(directDrawDigest.hot) + "/" +
            std::to_string(legacyDrawDigest.hot) +
        " params=" + std::to_string(directDrawDigest.params) + "/" +
            std::to_string(legacyDrawDigest.params) +
        " payload=" + std::to_string(directDrawDigest.payload) + "/" +
            std::to_string(legacyDrawDigest.payload) +
        " payload bytes=" + std::to_string(directDrawDigest.payloadBytes) +
            "/" + std::to_string(legacyDrawDigest.payloadBytes) +
        " override=" +
            std::to_string(directDrawDigest.param.bindingOverrideRange.offset) +
            ":" +
            std::to_string(directDrawDigest.param.bindingOverrideRange.size) +
            "/" +
            std::to_string(legacyDrawDigest.param.bindingOverrideRange.offset) +
            ":" +
            std::to_string(legacyDrawDigest.param.bindingOverrideRange.size) +
        " snapshot=" +
            std::to_string(directDrawDigest.param.bindingSnapshotRange.offset) +
            ":" +
            std::to_string(directDrawDigest.param.bindingSnapshotRange.size) +
            "/" +
            std::to_string(legacyDrawDigest.param.bindingSnapshotRange.offset) +
            ":" +
            std::to_string(legacyDrawDigest.param.bindingSnapshotRange.size) +
        " start=" + std::to_string(directDrawDigest.param.startVertex) + "/" +
            std::to_string(legacyDrawDigest.param.startVertex) +
        " base=" +
            std::to_string(directDrawDigest.param.baseVertexIndex) + "/" +
            std::to_string(legacyDrawDigest.param.baseVertexIndex) +
        " index=" + std::to_string(directDrawDigest.param.startIndex) + "/" +
            std::to_string(legacyDrawDigest.param.startIndex) +
        " indexed=" + std::to_string(directDrawDigest.param.indexed) + "/" +
            std::to_string(legacyDrawDigest.param.indexed) +
        " instance=" +
            std::to_string(directDrawDigest.param.instanceCount) + "/" +
            std::to_string(legacyDrawDigest.param.instanceCount) +
        " uniform=" +
            std::to_string(directDrawDigest.param.uniformHandle.index) + ":" +
            std::to_string(directDrawDigest.param.uniformHandle.generation) +
            ":" + std::to_string(directDrawDigest.param.uniformHandle.hash) +
            "/" +
            std::to_string(legacyDrawDigest.param.uniformHandle.index) + ":" +
            std::to_string(legacyDrawDigest.param.uniformHandle.generation) +
            ":" + std::to_string(legacyDrawDigest.param.uniformHandle.hash));
  check(fixture.device->GetRenderState(RS_TEXTURE_FACTOR) ==
            legacyFixture.device->GetRenderState(RS_TEXTURE_FACTOR),
        "ordinary direct and Legacy preserve final D3D render state");
  const auto* directBufferRecord = fixture.routing->queue_.pool().findBuffer(
      buffer->obj->handle().value);
  const auto* legacyBufferRecord = legacyFixture.routing->queue_.pool().findBuffer(
      legacyBuffer->obj->handle().value);
  check(directBufferRecord && legacyBufferRecord &&
            directBufferRecord->lastUsedSeqId == legacyBufferRecord->lastUsedSeqId &&
            directBufferRecord->lastUsedSeqId == directCompletion.seqId,
        "ordinary direct and Legacy preserve owner resource-mark ledger identity");

  const std::array synchronousBarrierRecords{
      drawRecord(identity), clearRecord(),
      stretchRectRecord(sourceIdentity, destinationIdentity, 1u),
      readbackRecord(sourceIdentity, destinationIdentity, 3u),
      presentRecord()};
  const auto synchronousBarrierWire =
      makeWireFixture(synchronousBarrierRecords);
  dxmt9::d3d9::ImportedChunkView synchronousBarrierImported;
  check(validateCommandChunk(synchronousBarrierWire.bytes,
                             synchronousBarrierWire.envelope,
                             &synchronousBarrierImported)
                .valid(),
        "ordinary same-raw mixed Draw/Clear/copy/readback/Present fixture "
        "validates");
  const auto synchronousBarrierPlan = dxmt9::d3d9::planCpuReadyChunk(
      synchronousBarrierImported, 71u);
  check(synchronousBarrierPlan.reason ==
                dxmt9::d3d9::ReplayReason::Readback &&
            dxmt9::d3d9::classifyDirectChunkSlotReplay(
                synchronousBarrierImported, synchronousBarrierPlan,
                /*captureOrTrace=*/false) ==
                dxmt9::d3d9::DirectChunkSlotReplayDisposition::
                    InlineOrderedControl,
        "ordinary direct selection keeps same-raw synchronous Readback and "
        "Present ordered before effects");
  dxmt9::d3d9::releaseRetainedWrappers(directRaw);
  dxmt9::d3d9::releaseRetainedWrappers(legacyRaw);
  dxmt9c_buffer_release(buffer);
  dxmt9c_buffer_release(legacyBuffer);
  dxmt9c_surface_release(source);
  dxmt9c_surface_release(destination);
  dxmt9c_surface_release(legacySource);
  dxmt9c_surface_release(legacyDestination);
}

void ordinaryDrawApplyStateDrawUsesCarrierFreeDirectPath() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr,
        "Draw/ApplyState/Draw direct fixture buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  const std::array records{
      drawRecord(identity, 0u),
      applyRenderStateRecord(RS_TEXTURE_FACTOR, 0x01020304u),
      drawRecord(identity, 1u),
  };
  auto raw = makeRaw(makeWireFixture(records), 91u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;

  dxmt9::d3d9::ImportedChunkView imported;
  const auto wire = makeWireFixture(records);
  check(validateCommandChunk(wire.bytes, wire.envelope, &imported).valid(),
        "Draw/ApplyState/Draw direct fixture validates");
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(imported, 91u);
  check(plan.directArenaCandidate() && plan.sourceCount == 1u &&
            plan.segmentCount == 1u &&
            dxmt9::d3d9::classifyDirectChunkSlotReplay(
                imported, plan, /*captureOrTrace=*/false) ==
                dxmt9::d3d9::DirectChunkSlotReplayDisposition::Direct,
        "present-less Draw/ApplyState/Draw remains an ordinary Direct candidate");

  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
            fixture.routing->drawCalls == 0u &&
            fixture.routing->ordinaryDrawCalls == 0u &&
            fixture.routing->legacyMarkCalls == 0u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                fixture.routing->queue_) == 2u,
        "Direct Draw/ApplyState/Draw must use final storage directly");

  fixture.routing->present(SwapDesc{});
  const auto completion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(completion.dequeued && !completion.arena && completion.submitted &&
            completion.completed && completion.reclaimed &&
            completion.commandCount == 3u,
        "carrier-free Draw/ApplyState/Draw preserves one completion source");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

void ordinaryOversizedDrawSourceUsesCarrierFreeLifecycle() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "oversized direct fixture buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  std::vector<RecordSpec> records;
  records.reserve(640u);
  std::uint32_t nextHandleIndex = 0u;
  records.push_back(
      floatConstantRecord(D9C_COMMAND_RECORD_SET_VS_CONST_F, 0u));
  for (std::size_t i = 0; i < 640u; ++i) {
    records.push_back(drawRecord(identity, nextHandleIndex++));
    if (i % 16u == 0u) {
      records.push_back(applyRenderStateRecord(
          RS_TEXTURE_FACTOR, static_cast<std::uint32_t>(i)));
    }
    if (i == 320u) {
      records.push_back(
          floatConstantRecord(D9C_COMMAND_RECORD_SET_PS_CONST_F, 0u));
    }
  }
  const auto wire = makeWireFixture(records);
  dxmt9::d3d9::ImportedChunkView imported;
  const auto validation =
      validateCommandChunk(wire.bytes, wire.envelope, &imported);
  check(validation.valid(),
        "oversized direct Draw/APPLY_STATE fixture validates");
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(imported, 95u);
  check(plan.reason == dxmt9::d3d9::ReplayReason::Oversize &&
            plan.directChunkSlotCandidate() &&
            dxmt9::d3d9::classifyDirectChunkSlotReplay(
                imported, plan, /*captureOrTrace=*/false) ==
                dxmt9::d3d9::DirectChunkSlotReplayDisposition::DirectOversized,
        "oversized Draw/APPLY_STATE source selects the narrow direct envelope");

  auto raw = makeRaw(wire, 95u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
            fixture.routing->drawCalls == 0u &&
            fixture.routing->ordinaryDrawCalls == 0u,
        "oversized source uses final storage exactly once");
  fixture.routing->present(SwapDesc{});
  const auto completion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(completion.dequeued && !completion.arena && completion.submitted &&
            completion.completed && completion.reclaimed &&
            completion.commandCount != 0u,
        "oversized direct source retains one FIFO completion identity");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

// A raw cut by a compatibility range is the shape the whole-raw gate could
// never own: one UP/TriangleFan draw anywhere demoted every draw in the raw.
// The executable partition gives it three spans -- direct, ordinary, direct --
// so the compatibility range ends the island before it and delays the island
// after it without poisoning either.
void compatibilityCutSplitsTheRawIntoTwoLeaseSpans() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "compatibility-cut fixture buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  const auto fanRecord = [&](std::uint32_t handleIndex) {
    auto record = drawRecord(identity, handleIndex);
    D9CCommandChunkWireDrawHeader header{};
    std::memcpy(&header, record.payload.data(), sizeof(header));
    header.primitiveType = 6u;  // TriangleFan: compatibility-sink owned.
    std::memcpy(record.payload.data(), &header, sizeof(header));
    return record;
  };
  const std::array records{drawRecord(identity, 0u), fanRecord(1u),
                           drawRecord(identity, 2u)};
  const auto wire = makeWireFixture(records);
  dxmt9::d3d9::ImportedChunkView imported;
  check(validateCommandChunk(wire.bytes, wire.envelope, &imported).valid(),
        "compatibility-cut fixture validates");

  const auto plan = dxmt9::d3d9::planReplayEmission(imported, 120u, 4096u);
  check(plan.partitioned() && plan.leaseSpanCount == 3u &&
            plan.leaseOwningSpanCount() == 2u,
        "a compatibility range splits the raw into two lease spans");
  check(plan.leaseSpans[0].ownsLease && plan.leaseSpans[0].leaseOrdinal == 0u &&
            !plan.leaseSpans[0].finalLeaseSpan &&
            !plan.leaseSpans[1].ownsLease &&
            plan.leaseSpans[2].ownsLease &&
            plan.leaseSpans[2].leaseOrdinal == 1u &&
            plan.leaseSpans[2].finalLeaseSpan,
        "the two islands own adjacent lease ordinals across the cut");

  auto raw = makeRaw(wire, 120u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK,
        "a compatibility-cut raw replays successfully");
  // The descriptive partition remains available, but production cannot
  // execute two Direct leases without either rotating a populated slot or
  // risking a post-effect admission failure. Until that rotation carries a
  // complete incoming witness, the whole raw stays on the serial lane.
  check(fixture.routing->drawCalls == 3u &&
            fixture.routing->ordinaryDrawCalls == 3u,
        "a multi-lease compatibility cut falls back whole before effects");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

// A populated slot and a multi-lease incoming raw carry distinct identity
// domains. Production rejects that composition before span 0 can commit, so a
// later span can never discover an identity or capacity failure after the
// TriangleFan separator has already executed.
void populatedIdentityMultiSpanFallsBackBeforeEffects() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr,
        "populated multi-span identity fixture buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);

  auto prefixWire = makeWireFixture(
      std::array{drawRecord(identity, 0u)});
  prefixWire.envelope.producerIdentity = {
      .firstEventOrdinal = 3u,
      .lastEventOrdinal = 3u,
      .firstSourceOrdinal = 260u,
      .lastSourceOrdinal = 515u,
  };
  auto prefixRaw = makeRaw(prefixWire, 119u, false, &registry);
  prefixRaw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), prefixRaw) ==
                D3D_OK &&
            fixture.routing->drawCalls == 0u,
        "the adjacent predecessor raw constructs the populated slot");
  dxmt9::CommandQueueArenaLeaseTestAccess::reserveDirectContinuationHeadroom(
      fixture.routing->queue_);

  auto fanRecord = drawRecord(identity, 1u);
  D9CCommandChunkWireDrawHeader fanHeader{};
  std::memcpy(&fanHeader, fanRecord.payload.data(), sizeof(fanHeader));
  fanHeader.primitiveType = 6u;
  std::memcpy(fanRecord.payload.data(), &fanHeader, sizeof(fanHeader));
  auto splitWire = makeWireFixture(std::array{
      drawRecord(identity, 0u), fanRecord, drawRecord(identity, 2u)});
  splitWire.envelope.producerIdentity = {
      .firstEventOrdinal = 4u,
      .lastEventOrdinal = 4u,
      .firstSourceOrdinal = 516u,
      .lastSourceOrdinal = 640u,
  };
  auto splitRaw = makeRaw(splitWire, 120u, false, &registry);
  splitRaw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), splitRaw) ==
                D3D_OK &&
            fixture.routing->drawCalls == 3u &&
            fixture.routing->ordinaryDrawCalls == 3u &&
            !dxmt9::CommandQueueArenaLeaseTestAccess::stopped(
                fixture.routing->queue_),
        "the multi-lease raw falls back once before any span effect");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
            fixture.routing->queue_) == 4u,
        "the predecessor and direct-fan-direct raw remain one ordered stream");

  dxmt9::d3d9::releaseRetainedWrappers(splitRaw);
  dxmt9::d3d9::releaseRetainedWrappers(prefixRaw);
  dxmt9c_buffer_release(buffer);
}

void compatibilityPrefixDirectContinuationOwnsPhysicalCredit() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  dxmt9::CommandQueueArenaLeaseTestAccess::enableDirectLifecycleObservation(
      fixture.routing->queue_);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr,
        "mixed-source lifecycle fixture buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);

  // This prefix is compatibility-owned and therefore intentionally has no
  // Direct semantic row. The next raw appends a Direct span into the same
  // physical payload; its first observed sibling must represent that existing
  // payload's one physical credit owner.
  const std::array prefixDraws{DrawParam{}};
  const std::array prefixPayloads{DrawParamPayloadView{}};
  fixture.routing->queue_.submitDrawRun(
      CanonicalDrawState{}, DrawUniformPayload{}, prefixDraws, prefixPayloads);
  dxmt9::CommandQueueArenaLeaseTestAccess::reserveDirectContinuationHeadroom(
      fixture.routing->queue_);

  auto raw = makeRaw(
      makeWireFixture(std::array{drawRecord(identity, 0u),
                                 drawRecord(identity, 1u)}),
      132u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK,
        "Direct span appends after an ordinary compatibility prefix");
  const auto admitted =
      dxmt9::CommandQueueArenaLeaseTestAccess::directLifecycleState(
          fixture.routing->queue_);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::directLifecycleError(
            fixture.routing->queue_) ==
            dxmt9::queue::DirectSourceLifecycleError::None &&
            admitted.recordCount == 1u &&
            admitted.records[0].physicalCreditOwner &&
            admitted.records[0].aggregateCredit != 0u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                fixture.routing->queue_) == 2u,
        "the sole Direct source represents exactly one physical owner");

  fixture.routing->present(SwapDesc{});
  const auto completion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  const auto reclaimed =
      dxmt9::CommandQueueArenaLeaseTestAccess::directLifecycleState(
          fixture.routing->queue_);
  check(completion.reclaimed &&
            dxmt9::CommandQueueArenaLeaseTestAccess::directLifecycleError(
                fixture.routing->queue_) ==
                dxmt9::queue::DirectSourceLifecycleError::None &&
            reclaimed.recordCount == 1u &&
            reclaimed.records[0].phase ==
                dxmt9::queue::DirectSourcePhase::Reclaimed,
        "mixed physical owner preserves FIFO completion and reclaim");

  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

// A multi-lease production candidate falls back whole to the Legacy lane. Its
// ordered command kinds and draw parameter/payload bytes must match an explicit
// Legacy oracle exactly.
//
// Comparing only a total command count would pass for a *reordered* or
// differently-parameterised stream, so the comparison is the concatenated
// per-command `effectiveCommandDigest` sequence -- ordered command kind, draw
// parameters, binding-override and binding-snapshot payload bytes, and the
// coordinator descriptors -- across every source the raw published.
void compatibilityCutMatchesLegacyDrawSemantics() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "compatibility-cut differential buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  const auto fanRecord = [&](std::uint32_t handleIndex) {
    auto record = drawRecord(identity, handleIndex);
    D9CCommandChunkWireDrawHeader header{};
    std::memcpy(&header, record.payload.data(), sizeof(header));
    header.primitiveType = 6u;
    std::memcpy(record.payload.data(), &header, sizeof(header));
    return record;
  };
  const std::array records{drawRecord(identity, 0u), fanRecord(1u),
                           drawRecord(identity, 2u)};
  const auto wire = makeWireFixture(records);

  auto spanRaw = makeRaw(wire, 121u, false, &registry);
  auto legacyRaw = makeRaw(wire, 122u, false, &registry);
  spanRaw.cpuReadyTapePlanningEnabled = false;
  legacyRaw.cpuReadyTapePlanningEnabled = false;

  // Present may spread commands over more than one published source. Compare
  // what the raw actually emitted by draining every source in order.
  //
  // The drain is bounded by the non-blocking `readyCount` snapshot taken
  // after publication. `consumeOne` is NOT an empty-queue probe: its
  // `dequeueReadySlot` reaches `reserveReadySlotBatchPrefix`, which waits on
  // a condition variable, so one extra call past the last ready source wedges
  // the test thread instead of returning `dequeued == false`.
  struct DrainedStream {
    std::vector<std::uint64_t> digests;
    std::vector<std::uint8_t> kinds;
  };
  const auto drainOrderedCommands = [&]() {
    fixture.routing->present(SwapDesc{});
    const auto ready = dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
        fixture.routing->queue_);
    check(ready != 0u, "a replayed raw publishes at least one source");
    DrainedStream stream;
    for (std::size_t i = 0; i < ready; ++i) {
      const auto completion =
          dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
              fixture.routing->queue_);
      check(completion.dequeued,
            "every counted ready source dequeues without waiting");
      check(completion.commandDigests.size() == completion.commandCount &&
                completion.commandKinds.size() == completion.commandCount,
            "every published command contributes exactly one digest");
      stream.digests.insert(stream.digests.end(),
                            completion.commandDigests.begin(),
                            completion.commandDigests.end());
      stream.kinds.insert(stream.kinds.end(),
                          completion.commandKinds.begin(),
                          completion.commandKinds.end());
    }
    check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
              fixture.routing->queue_) == 0u,
          "the bounded drain consumes exactly the published sources");
    return stream;
  };

  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), spanRaw) == D3D_OK,
        "the span lane replays the differential raw");
  const auto spanStream = drainOrderedCommands();

  fixture.routing->directChunkSlot_ = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), legacyRaw) == D3D_OK,
        "the Legacy lane replays the same records");
  const auto legacyStream = drainOrderedCommands();

  check(!spanStream.digests.empty() &&
            spanStream.digests.size() == legacyStream.digests.size(),
        "span replay and Legacy replay emit the same total command count");
  check(spanStream.kinds == legacyStream.kinds,
        "span replay and Legacy replay emit the same ordered command kinds");
  check(spanStream.digests == legacyStream.digests,
        "span replay and Legacy replay emit identical ordered command "
        "identities, draw parameters, and draw payload bytes");
  dxmt9::d3d9::releaseRetainedWrappers(spanRaw);
  dxmt9::d3d9::releaseRetainedWrappers(legacyRaw);
  dxmt9c_buffer_release(buffer);
}

void drawApplyStateDrawWithPresentUsesOneLeaseSpan() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr,
        "Draw/ApplyState/Draw/Present Legacy fixture buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  const std::array records{
      drawRecord(identity, 0u),
      applyRenderStateRecord(RS_TEXTURE_FACTOR, 0x01020304u),
      drawRecord(identity, 1u),
      presentRecord(),
  };
  auto raw = makeRaw(makeWireFixture(records), 92u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;

  dxmt9::d3d9::ImportedChunkView imported;
  const auto wire = makeWireFixture(records);
  check(validateCommandChunk(wire.bytes, wire.envelope, &imported).valid(),
        "Draw/ApplyState/Draw/Present fixture validates");
  // The narrow whole-raw gate rejects the range, because one coordinator
  // command anywhere demoted the entire raw. The source-wide partition does
  // not: the raw is one lease span holding a two-draw island plus a terminal
  // Present locator, which is exactly the DirectWithPresentTail shape.
  check(dxmt9::d3d9::classifyDirectChunkSlotRange(imported) ==
            dxmt9::d3d9::DirectChunkSlotRangeClass::Unsupported,
        "Present-bearing range stays outside strict direct eligibility");
  const auto emission =
      dxmt9::d3d9::planReplayEmission(imported, 92u, 4096u);
  check(emission.partitioned() && emission.leaseSpanCount == 1u &&
            emission.leaseSpans.front().ownsLease &&
            emission.leaseSpans.front().finalLeaseSpan &&
            emission.leaseSpans.front().leaseOrdinal == 0u &&
            emission.leaseSpans.front().drawCount == 2u &&
            emission.leaseSpans.front().coordinatorCount == 1u &&
            emission.leaseSpans.front().containsTerminalPresent &&
            emission.leaseSpans.front().recordCount == 4u,
        "Present-bearing raw is one lease span with a terminal Present");
  check(dxmt9::d3d9::emissionPlanDisposition(emission, false) ==
            dxmt9::d3d9::DirectChunkSlotReplayDisposition::
                DirectWithPresentTail,
        "the emission plan reports the Present-tail disposition");

  // A direct draw is appended straight into the final ChunkSlot through the
  // borrowed appender, so it never reaches the routing backend at all: both
  // draw counters staying zero is the proof that neither draw took the
  // ordinary submission path, matching the oversized-direct case above.
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
            fixture.routing->drawCalls == 0u &&
            fixture.routing->ordinaryDrawCalls == 0u &&
            fixture.routing->presentCalls == 1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
                fixture.routing->queue_) == 2u,
        "Present-bearing Draw/ApplyState/Draw constructs through one lease "
        "span with no ordinary draw fallback");
  const auto completion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(completion.dequeued, "compatibility Present dequeues one source");
  check(!completion.arena,
        "compatibility Present retains final ChunkSlot representation");
  check(completion.hasPresent,
        "compatibility Present keeps Present as the terminal command");
  check(completion.commandCount == 3u,
        "compatibility Present keeps two draws and terminal Present");
  check(completion.seqId == 1u,
        "compatibility Present preserves the admitted sequence identity");
  // The lease span publishes with the exact raw ordinal it constructed from,
  // where whole-raw compatibility replay published an anonymous source. The
  // queue still mints the source ordinal.
  check(completion.rawOrdinal == 92u && completion.sourceOrdinal != 0u,
        "the Present-tail span publishes its exact raw identity");
  check(completion.submitted && completion.completed && completion.reclaimed,
        "compatibility Present reaches completion and reclaim");
  check(!dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
            fixture.routing->queue_),
        "compatibility Present publishes exactly one source");
  auto nextRaw = makeRaw(makeWireFixture(records), 93u, false, &registry);
  nextRaw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), nextRaw) == D3D_OK &&
            fixture.routing->presentCalls == 2u,
        "a second compatibility Present publishes without source reuse");
  const auto nextCompletion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(nextCompletion.dequeued && nextCompletion.hasPresent &&
            nextCompletion.seqId == 2u &&
            nextCompletion.rawOrdinal == 93u &&
            nextCompletion.sourceOrdinal == 2u &&
            nextCompletion.submitted && nextCompletion.completed &&
            nextCompletion.reclaimed,
        "consecutive Present-tail spans retain monotone source identity and "
        "each carry their own raw ordinal");
  dxmt9::d3d9::releaseRetainedWrappers(nextRaw);
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

// A Present is NOT appended at its serial index. `appendActiveDirectChunkSlot
// Present` parks it in the queue-owned build context and
// `commitDirectChunkSlotReplay` appends it once, last, as the slot's
// publication boundary. So a lease that owned `Draw, Present, Draw` would
// publish `Draw, Draw, Present` -- the second draw executing BEFORE the
// Present it followed in the source. A second Present in one transaction is
// refused outright (`if (context.presentAppended) return false;`) and poisons
// the build.
//
// Both shapes must therefore be routed to whole-raw compatibility replay
// BEFORE any direct, Metal, or queue side effect. These are the production
// pins for that: compatibility routing (both draws reach the ordinary
// submission path, so no direct lease was taken) plus the exact published
// source order.
void nonTerminalPresentFallsBackToCompatibilityInSourceOrder() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "non-terminal Present fixture buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);

  const std::array records{drawRecord(identity, 0u), presentRecord(),
                           drawRecord(identity, 1u)};
  const auto wire = makeWireFixture(records);
  dxmt9::d3d9::ImportedChunkView imported;
  check(validateCommandChunk(wire.bytes, wire.envelope, &imported).valid(),
        "Draw/Present/Draw fixture validates");

  const auto plan = dxmt9::d3d9::planReplayEmission(imported, 130u, 4096u);
  check(plan.partitioned() && plan.presentCount == 1u,
        "Draw,Present,Draw still partitions completely");
  check(plan.leaseBlock ==
            dxmt9::d3d9::EmissionLeaseBlock::PresentOrdering &&
            !plan.spansExecutable() && !plan.leaseEligible(),
        "a non-terminal Present blocks every lease of the raw");
  check(dxmt9::d3d9::emissionPlanDisposition(plan, /*captureOrTrace=*/false) ==
            dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyPresent,
        "a non-terminal Present reports LegacyPresent");

  auto raw = makeRaw(wire, 130u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK,
        "a non-terminal Present raw replays successfully");
  // Both draws took the ordinary submission path, which is the observable
  // proof that no direct lease was taken for any span of this raw.
  check(fixture.routing->drawCalls == 2u &&
            fixture.routing->ordinaryDrawCalls == 2u &&
            fixture.routing->presentCalls == 1u,
        "a non-terminal Present routes the whole raw to compatibility replay");

  // Exact source order. The Present publishes the slot holding the first draw,
  // so the second draw can only appear after it; a lease would have emitted
  // both draws first.
  const auto kinds = drainOrderedCommandKinds(fixture);
  check(kinds.size() == 3u &&
            kinds[0] == dxmt9::core::MetalCommandKind::DrawRun &&
            kinds[1] == dxmt9::core::MetalCommandKind::Present &&
            kinds[2] == dxmt9::core::MetalCommandKind::DrawRun,
        "the published stream keeps Draw, Present, Draw in source order");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

// Two Presents in one raw. The direct build context refuses the second append
// outright, so this must also fail closed to whole-raw compatibility replay
// before any effect rather than poison an open transaction.
void duplicatePresentFallsBackToCompatibilityInSourceOrder() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "duplicate Present fixture buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);

  const std::array records{drawRecord(identity, 0u), presentRecord(),
                           drawRecord(identity, 1u), presentRecord()};
  const auto wire = makeWireFixture(records);
  dxmt9::d3d9::ImportedChunkView imported;
  check(validateCommandChunk(wire.bytes, wire.envelope, &imported).valid(),
        "Draw/Present/Draw/Present fixture validates");

  const auto plan = dxmt9::d3d9::planReplayEmission(imported, 131u, 4096u);
  check(plan.partitioned() && plan.presentCount == 2u &&
            plan.leaseBlock ==
                dxmt9::d3d9::EmissionLeaseBlock::PresentOrdering &&
            !plan.spansExecutable(),
        "a duplicate Present blocks every lease of the raw");
  check(dxmt9::d3d9::emissionPlanDisposition(plan, /*captureOrTrace=*/false) ==
            dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyPresent,
        "a duplicate Present reports LegacyPresent");

  auto raw = makeRaw(wire, 131u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK,
        "a duplicate Present raw replays successfully");
  check(fixture.routing->drawCalls == 2u &&
            fixture.routing->ordinaryDrawCalls == 2u &&
            fixture.routing->presentCalls == 2u,
        "a duplicate Present routes the whole raw to compatibility replay");
  check(!dxmt9::CommandQueueArenaLeaseTestAccess::stopped(
            fixture.routing->queue_),
        "a duplicate Present is a pre-effect fallback, never a fail-stop");

  const auto kinds = drainOrderedCommandKinds(fixture);
  check(kinds.size() == 4u &&
            kinds[0] == dxmt9::core::MetalCommandKind::DrawRun &&
            kinds[1] == dxmt9::core::MetalCommandKind::Present &&
            kinds[2] == dxmt9::core::MetalCommandKind::DrawRun &&
            kinds[3] == dxmt9::core::MetalCommandKind::Present,
        "the published stream keeps both Presents at their source positions");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

void ordinaryUnsupportedClearRangeStaysCompatibilityOwned() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  const auto rects = clearRectCountForPlannerPages(40);
  const std::array records{clearRecord(rects), clearRecord(rects)};
  const auto wire = makeWireFixture(records);
  dxmt9::d3d9::ImportedChunkView imported;
  check(validateCommandChunk(wire.bytes, wire.envelope, &imported).valid(),
        "single-source segmented aggregate fixture validates");
  check(dxmt9::d3d9::classifyDirectChunkSlotRange(imported) ==
                dxmt9::d3d9::DirectChunkSlotRangeClass::Unsupported &&
            !dxmt9::d3d9::planDirectChunkSlotRange(imported, 4096u),
        "Clear-only ranges stay outside strict Draw/APPLY_STATE eligibility");

  auto raw = makeRaw(wire, 93u);
  raw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
            fixture.routing->clearCalls == 2u &&
            fixture.routing->ordinaryDrawCalls == 0u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                fixture.routing->queue_) == 2u,
        "Clear-only range remains compatibility-owned with no Arena carrier");
  fixture.routing->present(SwapDesc{});
  const auto completion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(completion.dequeued && !completion.arena && completion.commandCount == 3u &&
            completion.submitted && completion.completed &&
            completion.reclaimed,
        "compatibility-owned Clear range preserves one completion identity");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
}

void ordinarySegmentedDrawApplyStateDrawMatchesLegacySemantics() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/true,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr,
        "segmented Draw/ApplyState/Draw differential buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  // SegmentSerial production storage permits a bounded aggregate source. Two
  // 40-page records force physical segmentation while leaving the complete
  // Draw/APPLY_STATE/Draw sequence in one validated logical source.
  const auto rects = clearRectCountForPlannerPages(40);
  const std::array records{
      drawRecord(identity, 0u),
      applyRenderStateRecord(RS_TEXTURE_FACTOR, 0x01020304u),
      drawRecord(identity, 1u),
      clearRecord(rects),
      clearRecord(rects),
  };
  const auto wire = makeWireFixture(records);
  dxmt9::d3d9::ImportedChunkView imported;
  check(validateCommandChunk(wire.bytes, wire.envelope, &imported).valid(),
        "segmented Draw/ApplyState/Draw differential fixture validates");
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(
      imported, 94u,
      {.pageSize = 4096u,
       .maxOrdinaryPagesPerSegment = 64u,
       .maxSegmentsPerSource = 2u,
       .maxPagesPerSource = 128u,
       .maxPages = 128u,
       .maxSourcesPerChunk = 1u});
  check(plan.directArenaCandidate() &&
            dxmt9::d3d9::classifyDirectChunkSlotRange(imported) ==
                dxmt9::d3d9::DirectChunkSlotRangeClass::Unsupported,
        "legacy planner candidates with Clear boundaries stay strict-unsupported");

  auto direct = makeRaw(wire, 94u, false, &registry);
  auto legacy = makeRaw(wire, 94u, false, &registry);
  direct.cpuReadyTapePlanningEnabled = false;
  legacy.cpuReadyTapePlanningEnabled = false;
  // Draw, ApplyState, Draw, Clear, Clear is one lease span: a two-draw island
  // followed by two coordinator locators. Both draws construct directly (both
  // draw counters stay zero), and both Clears still execute at their exact
  // serial position -- they reach Device::clear as before and are appended
  // into the same private destination through the borrowed appender rather
  // than demoting the island in front of them.
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), direct) == D3D_OK &&
            fixture.routing->clearCalls == 2u &&
            fixture.routing->drawCalls == 0u &&
            fixture.routing->ordinaryDrawCalls == 0u,
        "a coordinator-bearing range constructs as one lease span");
  const auto directWritingCommandCount =
      dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
          fixture.routing->queue_);
  const auto directDrawDigest =
      dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawDigest(
          fixture.routing->queue_);
  fixture.routing->present(SwapDesc{});
  const auto directCompletion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(fixture.device->SetRenderState(RS_TEXTURE_FACTOR, 0u) == D3D_OK,
        "segmented differential restores render state before Legacy replay");

  fixture.routing->directChunkSlot_ = false;
  // Cumulative counters. The direct pass above contributed two Clears and no
  // draw submissions, so the Legacy pass adds two of each.
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), legacy) == D3D_OK &&
            fixture.routing->clearCalls == 4u &&
            fixture.routing->drawCalls == 2u &&
            fixture.routing->ordinaryDrawCalls == 2u,
        "the same segmented Draw/ApplyState/Draw uses ordinary final storage");
  const auto legacyWritingCommandCount =
      dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
          fixture.routing->queue_);
  const auto legacyDrawDigest =
      dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawDigest(
          fixture.routing->queue_);
  fixture.routing->present(SwapDesc{});
  const auto legacyCompletion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(directCompletion.dequeued && !directCompletion.arena &&
            directCompletion.submitted &&
            directCompletion.completed && directCompletion.reclaimed &&
            legacyCompletion.dequeued && legacyCompletion.submitted &&
            legacyCompletion.completed && legacyCompletion.reclaimed &&
            directWritingCommandCount == legacyWritingCommandCount &&
            directCompletion.commandCount == legacyCompletion.commandCount &&
            // Both routes may have different internal state/PSO identities.
            // Compare the stable draw
            // parameters and payload bytes, which are the semantic ABI.
            directDrawDigest.params == legacyDrawDigest.params &&
            directDrawDigest.payload == legacyDrawDigest.payload &&
            directDrawDigest.payloadBytes == legacyDrawDigest.payloadBytes,
        "segmented compatibility and Legacy preserve exact command and draw semantics");
  dxmt9::d3d9::releaseRetainedWrappers(direct);
  dxmt9::d3d9::releaseRetainedWrappers(legacy);
  dxmt9c_buffer_release(buffer);
}

void directAdmissionRejectionPreservesLegacyDrawBatchGrouping() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  auto* secondBuffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr && secondBuffer != nullptr,
        "pre-effect fallback buffers construct");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  const auto secondIdentity = registry.insert(
      D9C_CHUNK_HANDLE_KIND_BUFFER, secondBuffer);
  const std::array records{drawRecord(identity), drawRecord(secondIdentity, 1u)};
  auto raw = makeRaw(makeWireFixture(records), 73u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;

  // Leave a compatibility command in the writing slot. Direct admission
  // rejects before semantic replay, so this raw must use the Legacy path.
  fixture.routing->queue_.submitClear(ClearDesc{});
  check(dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
            fixture.routing->queue_) == 1u,
        "fallback fixture must retain its compatibility prefix");

  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK,
        "pre-effect Direct admission rejection must fall back successfully");
  // A capacity-rejected continuation is still pre-effect. Keep the populated
  // prefix in place and hand the whole raw to the authoritative serial batch
  // path; storage pressure must not manufacture a publication boundary.
  // The two draws bind different buffers through per-draw overrides, so the
  // ordinary queue may fold them into one compatible final-slot run while
  // retaining the populated Clear prefix.
  check(fixture.routing->drawCalls == 2u &&
            fixture.routing->ordinaryDrawCalls == 2u &&
            fixture.routing->ordinaryDrawRunSubmissions == 1u &&
            fixture.routing->lastDrawRunSize == 2u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                fixture.routing->queue_) == 2u,
        "a capacity-rejected span falls back whole without rotating the prefix");

  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
  dxmt9c_buffer_release(secondBuffer);
}

// The ordinary (non-Direct) production path must fold adjacent final-slot
// compatible entries even when their concrete stream bindings form A -> B ->
// A. Binding overrides are per draw, so one canonical state/DrawRun remains
// semantically complete and the queue must not consult older history.
void ordinarySourceBatchFoldsBindingAbaWithOneAcquisition() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/false);
  auto* bufferA = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  auto* bufferB = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(bufferA != nullptr && bufferB != nullptr,
        "ordinary source batch A/B buffers construct");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identityA = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, bufferA);
  const auto identityB = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, bufferB);
  const auto wire = makeWireFixture(std::array{
      drawRecord(identityA, 0u), drawRecord(identityB, 1u),
      drawRecord(identityA, 2u)});
  auto raw = makeRaw(wire, 76u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;
  dxmt9::d3d9::ImportedChunkView imported;
  check(validateCommandChunk(wire.bytes, wire.envelope, &imported).valid(),
        "ordinary source batch A/B/A fixture validates");

  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
            fixture.routing->drawCalls == 3u &&
            fixture.routing->ordinaryDrawCalls == 3u &&
            fixture.routing->ordinaryDrawRunSubmissions == 1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                fixture.routing->queue_) == 1u,
        "ordinary binding A/B/A folds into one final-slot run and one queue acquisition");
  const auto bindings =
      dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawOverrideBuffers(
          fixture.routing->queue_);
  check(bindings.size() == 3u &&
            bindings[0] == bufferA->obj->handle() &&
            bindings[1] == bufferB->obj->handle() &&
            bindings[2] == bufferA->obj->handle(),
        "folded A/B/A preserves every concrete binding override in order");

  const auto handleA = bufferA->obj->handle();
  const auto handleB = bufferB->obj->handle();
  check(dxmt9::CommandQueueArenaLeaseTestAccess::bufferLastUsedSeqId(
            fixture.routing->queue_, handleA) == 1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::bufferLastUsedSeqId(
                fixture.routing->queue_, handleB) == 1u,
        "every distinct folded binding is stamped with the open slot ticket");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(bufferA);
  dxmt9c_buffer_release(bufferB);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::bufferAlive(
            fixture.routing->queue_, handleA) &&
            dxmt9::CommandQueueArenaLeaseTestAccess::bufferAlive(
                fixture.routing->queue_, handleB),
        "folded bindings remain alive until their stamped slot completes");

  fixture.routing->present(SwapDesc{});
  const auto completion = dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
      fixture.routing->queue_);
  check(completion.reclaimed && completion.commandCount == 2u,
        "ordinary A/B/A completion retains one folded run plus Present");
}

void productionReplayAppliesSparseStreamFrequenciesTransactionally() {
  // Both generation misses and key-only encoder reuse must reject a frequency
  // change. Buffer tuples have per-draw overrides; frequencies do not.
  for (std::size_t slot = 0u; slot < dxmt9::core::kMaxStreams; ++slot) {
    dxmt9::core::FlatDrawStateRecord a{};
    a.streamFrequencies.fill(1u);
    a.key.streamFrequencies = a.streamFrequencies;
    auto b = a;
    check(dxmt9::core::drawStatesCompatibleForDrawRunBatch(a, b),
          "equal stream frequencies retain batch compatibility");
    b.key.streamFrequencies[slot] = 2u;
    check(!dxmt9::core::drawStateKeysCompatibleForDrawRunBatch(a.key, b.key) &&
              !dxmt9::core::drawStatesCompatibleForDrawRunBatch(a, b),
          "every stream frequency participates in batch and encoder key reuse");
    b = a;
    b.streamFrequencies[slot] = 2u;
    check(!dxmt9::core::drawStatesCompatibleForDrawRunBatch(a, b),
          "executed frequency mismatch cannot hide behind an equal key");
    b.key.textureMask = 1u;
    const dxmt9::core::DrawShaderLayoutContext layout{};
    check(!dxmt9::core::classifyDrawRunBatchIncompatibility(
               a, b, layout, layout).textureOnly,
          "frequency mismatch cannot be diagnosed as a texture-only break");
  }
  // A zero wire frequency is the legacy omission. It must leave a non-default
  // live frequency untouched while the stream tuple itself is replaced.
  {
    RuntimeFixture fixture;
    auto* buffer = dxmt9c_device_create_vertex_buffer(
        fixture.cDevice.get(), 256u, 0u, 0u, 0u);
    check(buffer != nullptr, "zero-frequency buffer constructs");
    check(dxmt9c_device_set_stream_source(
              fixture.cDevice.get(), 0u, buffer, 7u, 24u) == D3D_OK &&
              dxmt9c_device_set_stream_source_freq(
                  fixture.cDevice.get(), 0u, 5u) == D3D_OK,
          "zero-frequency fixture seeds a non-default stream tuple");
    dxmt9::d3d9::WireObjectRegistry registry;
    const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
    auto raw = makeRaw(
        makeWireFixture(std::array{drawRecord(identity, 0u, 0u)}), 201u,
        false, &registry);
    check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
              fixture.cDevice->dev().state().streamFrequencies[0] == 5u &&
              fixture.cDevice->dev().state().streamBuffers[0] == buffer->obj &&
              fixture.cDevice->dev().state().streamOffsets[0] == 0u &&
              fixture.cDevice->dev().state().streamStrides[0] == 16u,
          "zero wire frequency preserves the live frequency while applying the stream");
    dxmt9::d3d9::releaseRetainedWrappers(raw);
    dxmt9c_buffer_release(buffer);
  }

  // Explicit frequencies are applied after each source update, including an
  // A/B/A sequence whose final state must be the last A tuple and frequency.
  for (unsigned route = 0u; route < 3u; ++route) {
    RuntimeFixture fixture(/*rejectAfterClear=*/false, /*segmentSerial=*/false,
                           /*directChunkSlot=*/route == 1u,
                           /*cpuReadyArena=*/route == 0u);
    auto* bufferA = dxmt9c_device_create_vertex_buffer(
        fixture.cDevice.get(), 256u, 0u, 0u, 0u);
    auto* bufferB = dxmt9c_device_create_vertex_buffer(
        fixture.cDevice.get(), 256u, 0u, 0u, 0u);
    check(bufferA != nullptr && bufferB != nullptr,
          "explicit-frequency buffers construct");
    dxmt9::d3d9::WireObjectRegistry registry;
    const auto identityA = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, bufferA);
    const auto identityB = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, bufferB);
    auto raw = makeRaw(
        makeWireFixture(std::array{
            drawRecord(identityA, 0u, 2u),
            drawRecord(identityB, 1u, 3u),
            drawRecord(identityA, 2u, 2u),
        }),
        202u, false, &registry);
    raw.cpuReadyTapePlanningEnabled = route == 0u;
    check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
              fixture.cDevice->dev().state().streamBuffers[0] == bufferA->obj &&
              fixture.cDevice->dev().state().streamFrequencies[0] == 2u,
          "explicit A/B/A frequencies reach the actual final Device state");
    const auto queuedFrequencies =
        dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawStreamFrequencies(
            fixture.routing->queue_);
    const bool queuedFrequencySequenceOk =
        queuedFrequencies.size() == 3u &&
        queuedFrequencies[0][0] == 2u &&
        queuedFrequencies[1][0] == 3u &&
        queuedFrequencies[2][0] == 2u;
    if (!queuedFrequencySequenceOk) {
      std::cerr << "A/B/A queued frequency evidence rows="
                << queuedFrequencies.size();
      for (const auto& row : queuedFrequencies) {
        std::cerr << " [" << row[0] << "]";
      }
      const auto oldestState =
          dxmt9::CommandQueueArenaLeaseTestAccess::oldestSourceState(
              fixture.routing->queue_);
      std::cerr << " ready="
                << dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                       fixture.routing->queue_)
                << " resident="
                << dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
                       fixture.routing->queue_)
                << " oldest-state="
                << (oldestState
                        ? std::to_string(static_cast<unsigned>(*oldestState))
                        : std::string("none"))
                << '\n';
    }
    check(queuedFrequencySequenceOk,
          "queued DrawRuns preserve every explicit A/B/A stream frequency");
    dxmt9::d3d9::releaseRetainedWrappers(raw);
    dxmt9c_buffer_release(bufferA);
    dxmt9c_buffer_release(bufferB);
  }

  // The flagged indexed/instance-data forms must survive into the queued draw
  // state. The first draw derives its instance count from the indexed stream's
  // low frequency bits (without normalizing either flag-bearing value).
  for (unsigned route = 0u; route < 3u; ++route) {
    RuntimeFixture fixture(/*rejectAfterClear=*/false, /*segmentSerial=*/false,
                           /*directChunkSlot=*/route == 1u,
                           /*cpuReadyArena=*/route == 0u);
    auto* buffer = dxmt9c_device_create_vertex_buffer(
        fixture.cDevice.get(), 256u, 0u, 0u, 0u);
    check(buffer != nullptr, "flagged-frequency buffer constructs");
    dxmt9::d3d9::WireObjectRegistry registry;
    const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
    constexpr auto frequency =
        dxmt9::core::kStreamSourceIndexedData | 4u;
    constexpr auto instanceFrequency =
        dxmt9::core::kStreamSourceInstanceData | 3u;
    auto raw = makeRaw(
        makeWireFixture(std::array{
            drawRecord(identity, 0u, frequency),
            streamDrawRecord(identity, 1u, instanceFrequency, 1u),
        }),
        203u, false, &registry);
    raw.cpuReadyTapePlanningEnabled = route == 0u;
    check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
              fixture.cDevice->dev().state().streamFrequencies[0] == frequency &&
              fixture.cDevice->dev().state().streamFrequencies[1] ==
                  instanceFrequency,
          "flagged indexed-data frequency reaches the actual Device state");
    const auto queuedFrequencies =
        dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawStreamFrequencies(
            fixture.routing->queue_);
    const auto queuedInstanceCounts =
        dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawInstanceCounts(
            fixture.routing->queue_);
    check(queuedFrequencies.size() == 2u &&
              queuedFrequencies[0][0] == frequency &&
              queuedFrequencies[1][0] == frequency &&
              queuedFrequencies[1][1] == instanceFrequency &&
              queuedInstanceCounts.size() == 2u &&
              queuedInstanceCounts[0] == 4u &&
              queuedInstanceCounts[1] == 4u,
          "first queued draw snapshot preserves flagged frequency and instance count");
    dxmt9::d3d9::releaseRetainedWrappers(raw);
    dxmt9c_buffer_release(buffer);
  }

  // A null stream still carries a meaningful explicit frequency; the source
  // unbind and frequency update are one journaled stream transaction.
  {
    RuntimeFixture fixture;
    auto* buffer = dxmt9c_device_create_vertex_buffer(
        fixture.cDevice.get(), 256u, 0u, 0u, 0u);
    check(buffer != nullptr, "null-stream buffer constructs");
    check(dxmt9c_device_set_stream_source(
              fixture.cDevice.get(), 0u, buffer, 0u, 16u) == D3D_OK &&
              dxmt9c_device_set_stream_source_freq(
                  fixture.cDevice.get(), 0u, 7u) == D3D_OK,
          "null-stream fixture seeds an existing binding");
    auto raw = makeRaw(
        makeWireFixture(std::array{nullStreamDrawRecord(4u)}), 203u);
    check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
              fixture.cDevice->dev().state().streamBuffers[0] == nullptr &&
              fixture.cDevice->dev().state().streamFrequencies[0] == 4u,
          "null stream binding still applies its explicit frequency");
    dxmt9::d3d9::releaseRetainedWrappers(raw);
    dxmt9c_buffer_release(buffer);
  }

  // Both an invalid explicit frequency and a later replay failure must restore
  // the complete pre-replay stream tuple, including frequency.
  for (const bool laterFailure : {false, true}) {
    RuntimeFixture fixture;
    auto* oldBuffer = dxmt9c_device_create_vertex_buffer(
        fixture.cDevice.get(), 256u, 0u, 0u, 0u);
    auto* newBuffer = dxmt9c_device_create_vertex_buffer(
        fixture.cDevice.get(), 256u, 0u, 0u, 0u);
    check(oldBuffer != nullptr && newBuffer != nullptr,
          "rollback buffers construct");
    check(dxmt9c_device_set_stream_source(
              fixture.cDevice.get(), 0u, oldBuffer, 9u, 28u) == D3D_OK &&
              dxmt9c_device_set_stream_source_freq(
                  fixture.cDevice.get(), 0u, 6u) == D3D_OK,
          "rollback fixture seeds the original stream tuple");
    dxmt9::d3d9::WireObjectRegistry registry;
    const auto identity = registry.insert(
        D9C_CHUNK_HANDLE_KIND_BUFFER, newBuffer);
    std::vector<RecordSpec> records;
    records.push_back(drawRecord(
        identity, 0u,
        laterFailure ? 3u : dxmt9::core::kStreamSourceInstanceData));
    if (laterFailure) {
      records.push_back(applyRenderStateThenInvalidTransformRecord(7u, 1u));
    }
    auto raw = makeRaw(makeWireFixture(records), 204u + laterFailure,
                       false, &registry);
    check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) < D3D_OK &&
              fixture.cDevice->dev().state().streamBuffers[0] == oldBuffer->obj &&
              fixture.cDevice->dev().state().streamOffsets[0] == 9u &&
              fixture.cDevice->dev().state().streamStrides[0] == 28u &&
              fixture.cDevice->dev().state().streamFrequencies[0] == 6u,
          laterFailure
              ? "later replay failure rolls back stream tuple and frequency"
              : "invalid explicit frequency rolls back stream tuple and frequency");
    dxmt9::d3d9::releaseRetainedWrappers(raw);
    dxmt9c_buffer_release(oldBuffer);
    dxmt9c_buffer_release(newBuffer);
  }
}

void populatedSlotDrawApplyDrawUsesCarrierFreeContinuation() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "continuation fixture buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  dxmt9::core::CopyMaterializationLedger unixLedger;
  dxmt9::core::ScopedCopyMaterializationLedger observeUnix(
      dxmt9::core::CopyMaterializationOwner::Unix, unixLedger);

  // Seed the compatibility writer with one draw. The ordinary append path
  // grows all final SoA vectors with headroom; the following validated raw is
  // the first production-shaped opportunity to append without a carrier.
  const std::array seedDraws{DrawParam{}};
  const std::array seedPayloads{DrawParamPayloadView{}};
  fixture.routing->queue_.submitDrawRun(
      CanonicalDrawState{}, DrawUniformPayload{}, seedDraws, seedPayloads);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
            fixture.routing->queue_) == 1u,
        "continuation fixture must retain a populated DrawRun prefix");
  dxmt9::CommandQueueArenaLeaseTestAccess::reserveDirectContinuationHeadroom(
      fixture.routing->queue_);

  const std::array records{
      drawRecord(identity, 0u),
      applyRenderStateRecord(RS_TEXTURE_FACTOR, 0x01020304u),
      drawRecord(identity, 1u),
      applyRenderStateRecord(RS_TEXTURE_FACTOR, 0u),
      drawRecord(identity, 2u),
  };
  auto raw = makeRaw(makeWireFixture(records), 74u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
            fixture.routing->drawCalls == 0u &&
            fixture.routing->ordinaryDrawCalls == 0u,
        "populated Draw/ApplyState/Draw/ApplyState/Draw must use Direct continuation");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
            fixture.routing->queue_) == 4u,
        "continuation must preserve the existing prefix and append A→B→A draw runs");
  const auto directA = dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawDigest(
      fixture.routing->queue_, 1u);
  const auto directB = dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawDigest(
      fixture.routing->queue_, 2u);
  const auto directA2 = dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawDigest(
      fixture.routing->queue_, 3u);

  fixture.routing->present(SwapDesc{});
  const auto completion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(completion.dequeued && completion.commandCount == 5u &&
            completion.submitted && completion.completed && completion.reclaimed,
        "continuation completion must retain one ordered prefix identity");
  RuntimeFixture legacyFixture(/*rejectAfterClear=*/false,
                               /*segmentSerial=*/false,
                               /*directChunkSlot=*/false);
  auto* legacyBuffer = dxmt9c_device_create_vertex_buffer(
      legacyFixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(legacyBuffer != nullptr, "A-to-B-to-A Legacy oracle buffer constructs");
  dxmt9::d3d9::WireObjectRegistry legacyRegistry;
  const auto legacyIdentity = legacyRegistry.insert(
      D9C_CHUNK_HANDLE_KIND_BUFFER, legacyBuffer);
  const std::array legacySeedDraws{DrawParam{}};
  const std::array legacySeedPayloads{DrawParamPayloadView{}};
  legacyFixture.routing->queue_.submitDrawRun(
      CanonicalDrawState{}, DrawUniformPayload{}, legacySeedDraws,
      legacySeedPayloads);
  auto legacyRaw = makeRaw(
      makeWireFixture(std::array{
          drawRecord(legacyIdentity, 0u),
          applyRenderStateRecord(RS_TEXTURE_FACTOR, 0x01020304u),
          drawRecord(legacyIdentity, 1u),
          applyRenderStateRecord(RS_TEXTURE_FACTOR, 0u),
          drawRecord(legacyIdentity, 2u),
      }),
      74u, false, &legacyRegistry);
  legacyRaw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(legacyFixture.cDevice.get(), legacyRaw) ==
                D3D_OK &&
            legacyFixture.routing->ordinaryDrawCalls == 3u,
        "A-to-B-to-A ordinary oracle must preserve three state-separated runs");
  const auto legacyA = dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawDigest(
      legacyFixture.routing->queue_, 1u);
  const auto legacyB = dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawDigest(
      legacyFixture.routing->queue_, 2u);
  const auto legacyA2 = dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawDigest(
      legacyFixture.routing->queue_, 3u);
  legacyFixture.routing->present(SwapDesc{});
  const auto legacyCompletion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          legacyFixture.routing->queue_);
  check(legacyCompletion.dequeued && legacyCompletion.commandCount == 5u &&
            legacyCompletion.submitted && legacyCompletion.completed &&
            legacyCompletion.reclaimed &&
            directA.hot == legacyA.hot && directA.pso == legacyA.pso &&
            directB.hot == legacyB.hot && directB.pso == legacyB.pso &&
            directA2.hot == legacyA2.hot && directA2.pso == legacyA2.pso,
        "Direct continuation and Legacy A-to-B-to-A state semantics must match");
  dxmt9::d3d9::releaseRetainedWrappers(legacyRaw);
  dxmt9c_buffer_release(legacyBuffer);
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

void populatedSlotInsufficientCapacityFallsBackBeforeEffects() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "capacity fallback fixture buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);

  // The legacy append reserves exactly this seven-draw prefix. A two-draw
  // continuation cannot fit all final vectors, so admission rejects on
  // capacity before the first source semantic effect. Production must not
  // publish that prefix merely to obtain storage: rotation is an observable
  // command-buffer/pass boundary and previously corrupted GT1. The incoming
  // raw therefore falls back whole while the prefix remains in place.
  const std::array seedDraws{DrawParam{}, DrawParam{}, DrawParam{}, DrawParam{},
                             DrawParam{}, DrawParam{}, DrawParam{}};
  const std::array seedPayloads{
      DrawParamPayloadView{}, DrawParamPayloadView{}, DrawParamPayloadView{},
      DrawParamPayloadView{}, DrawParamPayloadView{}, DrawParamPayloadView{},
      DrawParamPayloadView{}};
  fixture.routing->queue_.submitDrawRun(
      CanonicalDrawState{}, DrawUniformPayload{}, seedDraws, seedPayloads);
  const std::array records{drawRecord(identity, 0u), drawRecord(identity, 1u)};
  auto raw = makeRaw(makeWireFixture(records), 75u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;
  // Both records bind the same buffer at the same offset/stride with nothing
  // between them, so the whole-raw ordinary fallback folds them into ONE
  // queue acquisition carrying two draws. The fallback itself is unchanged:
  // two draws replay, in order, through the authoritative serial path.
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
            fixture.routing->drawCalls == 2u &&
            fixture.routing->ordinaryDrawCalls == 2u &&
            fixture.routing->ordinaryDrawRunSubmissions == 1u &&
            fixture.routing->lastDrawRunSize == 2u,
        "insufficient continuation capacity falls back whole before effects");
  // The seven-draw prefix is still the slot's first command and the fallback
  // appended after it, so no rotation published anything. Two commands, not
  // three: the fallback's two draws share one run.
  check(dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
            fixture.routing->queue_) == 2u,
        "capacity rejection preserves the populated prefix without rotation");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

void populatedSlotProducerIdentityGapFallsBackBeforeEffects() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "identity-gap fallback fixture buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  const std::array records{drawRecord(identity)};

  auto firstWire = makeWireFixture(records);
  firstWire.envelope.producerIdentity = {
      .firstEventOrdinal = 10u,
      .lastEventOrdinal = 10u,
      .firstSourceOrdinal = 20u,
      .lastSourceOrdinal = 20u,
  };
  auto firstRaw = makeRaw(firstWire, 96u, false, &registry);
  firstRaw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), firstRaw) ==
                D3D_OK &&
            fixture.routing->drawCalls == 0u &&
            fixture.routing->ordinaryDrawCalls == 0u,
        "first identity-qualified raw constructs the open Direct source");
  dxmt9::CommandQueueArenaLeaseTestAccess::reserveDirectContinuationHeadroom(
      fixture.routing->queue_);

  auto gapWire = makeWireFixture(records);
  gapWire.envelope.producerIdentity = {
      .firstEventOrdinal = 12u,
      .lastEventOrdinal = 12u,
      .firstSourceOrdinal = 21u,
      .lastSourceOrdinal = 21u,
  };
  auto gapRaw = makeRaw(gapWire, 97u, false, &registry);
  gapRaw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), gapRaw) == D3D_OK &&
            fixture.routing->drawCalls == 1u &&
            fixture.routing->ordinaryDrawCalls == 1u &&
            !dxmt9::CommandQueueArenaLeaseTestAccess::stopped(
                fixture.routing->queue_) &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                fixture.routing->queue_) == 2u,
        "a forward-gapped producer interval must select Legacy before effects "
        "and apply its draw exactly once without poisoning the queue");

  dxmt9::d3d9::releaseRetainedWrappers(gapRaw);
  dxmt9::d3d9::releaseRetainedWrappers(firstRaw);
  dxmt9c_buffer_release(buffer);
}

void populatedContinuationCommitFailureIsTerminalWithoutRetry() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "continuation failure fixture buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  const std::array seedDraws{DrawParam{}};
  const std::array seedPayloads{DrawParamPayloadView{}};
  fixture.routing->queue_.submitDrawRun(
      CanonicalDrawState{}, DrawUniformPayload{}, seedDraws, seedPayloads);
  dxmt9::CommandQueueArenaLeaseTestAccess::reserveDirectContinuationHeadroom(
      fixture.routing->queue_);
  const std::array records{
      drawRecord(identity, 0u),
      applyRenderStateRecord(RS_TEXTURE_FACTOR, 0x01020304u),
      drawRecord(identity, 1u),
  };
  auto raw = makeRaw(makeWireFixture(records), 76u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;
  dxmt9::CommandQueueArenaLeaseTestAccess::forceNextDirectChunkSlotCommitFailure(
      fixture.routing->queue_);
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) != D3D_OK &&
            fixture.routing->drawCalls == 0u &&
            fixture.routing->ordinaryDrawCalls == 0u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::stopped(
                fixture.routing->queue_),
        "continuation commit failure must fail-stop after one semantic replay");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
            fixture.routing->queue_) == 3u,
        "continuation commit failure must retain the applied prefix without Legacy retry");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

void lateStateFailureDoesNotDuplicateDiscardedDirectProgress() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "late replay failure buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  constexpr std::uint32_t kInitial = 0x01020304u;
  constexpr std::uint32_t kAbandoned = 0xa0b0c0d0u;
  check(fixture.cDevice->dev().setRenderState(RS_TEXTURE_FACTOR, kInitial) ==
            D3D_OK,
        "late replay failure baseline state applies");
  const auto submittedBefore =
      fixture.cDevice->dev().submittedSequenceId();
  const std::array records{
      drawRecord(identity, 0u),
      applyRenderStateThenInvalidTransformRecord(
          RS_TEXTURE_FACTOR, kAbandoned),
  };
  auto raw = makeRaw(makeWireFixture(records), 81u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) != D3D_OK,
        "late invalid state must fail both direct and compatibility replay");
  check(fixture.cDevice->dev().submittedSequenceId() == submittedBefore + 1u,
        "discarded Direct draw must not duplicate the one compatibility "
        "prefix effect");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
            fixture.routing->queue_) == 1u &&
            !dxmt9::CommandQueueArenaLeaseTestAccess::stopped(
                fixture.routing->queue_),
        "Direct rollback must discard its private draw before the single "
        "compatibility prefix is emitted");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

void populatedSlotPresentTailFallsBackBeforeEffects() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "present-tail exclusion fixture buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  const std::array seedDraws{DrawParam{}};
  const std::array seedPayloads{DrawParamPayloadView{}};
  fixture.routing->queue_.submitDrawRun(
      CanonicalDrawState{}, DrawUniformPayload{}, seedDraws, seedPayloads);
  const std::array records{drawRecord(identity), presentRecord()};
  auto raw = makeRaw(makeWireFixture(records), 77u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;
  // A populated slot has no reserved Present row, so the coordinator
  // dimension is a capacity rejection. Production keeps the populated extent
  // in place and falls the incoming raw back before effects; it must not turn
  // a storage decision into a publication boundary.
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
            fixture.routing->drawCalls == 1u &&
            fixture.routing->ordinaryDrawCalls == 1u &&
            fixture.routing->presentCalls == 1u,
        "a Present-tail span rejected on capacity falls back before effects");
  check(!dxmt9::CommandQueueArenaLeaseTestAccess::stopped(
            fixture.routing->queue_),
        "Present-tail exclusion must remain a recoverable Legacy fallback");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

void directContinuationAdmissionMatchesShapeAndCapacityTruthTable() {
  auto slot = validDirectContinuationSlot();
  const auto valid = oneDrawContinuationCapacity();
  const auto admitted = directContinuationAdmission(slot, valid);
  check(admitted.admitted(),
        "shared continuation predicate must admit a complete reserved draw shape");

  auto malformed = valid;
  malformed.drawRunRecords = 0u;
  const auto malformedResult = directContinuationAdmission(slot, malformed);
  check(malformedResult.structuralRejected(),
        "missing DrawRun SoA row must be a structural rejection");

  auto clear = valid;
  clear.clearRecords = 1u;
  const auto clearResult = directContinuationAdmission(slot, clear);
  check(clearResult.structuralRejected(),
        "Clear must remain outside populated render continuation");

  auto readback = valid;
  readback.readbackRecords = 1u;
  const auto readbackResult = directContinuationAdmission(slot, readback);
  check(readbackResult.structuralRejected(),
        "Readback must remain outside populated render continuation");

  auto resourceMutation = valid;
  resourceMutation.surfaceCopyRecords = 1u;
  const auto resourceMutationResult =
      directContinuationAdmission(slot, resourceMutation);
  check(resourceMutationResult.structuralRejected(),
        "resource mutation must remain outside populated render continuation");

  const auto capacityRejected =
      directContinuationAdmission(slot, oneDrawContinuationCapacity(1000u));
  check(capacityRejected.capacityRejected(),
        "a complete shape without final-vector capacity must reject by capacity");
}

void triangleFanNeverEntersDirectReplay() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "TriangleFan direct exclusion buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  const auto fanRecord = [&](std::uint32_t handleIndex = 0u) {
    auto record = drawRecord(identity, handleIndex);
    D9CCommandChunkWireDrawHeader header{};
    std::memcpy(&header, record.payload.data(), sizeof(header));
    header.primitiveType = 6u;
    std::memcpy(record.payload.data(), &header, sizeof(header));
    return record;
  };
  const auto fan = fanRecord();
  const std::array records{fan};
  const auto wire = makeWireFixture(records);
  dxmt9::d3d9::ImportedChunkView imported;
  check(validateCommandChunk(wire.bytes, wire.envelope, &imported).valid(),
        "TriangleFan direct exclusion fixture validates");
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(imported, 80u);
  check(plan.directArenaCandidate() &&
            dxmt9::d3d9::classifyDirectChunkSlotReplay(
                imported, plan, /*captureOrTrace=*/false) ==
                dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyUnsupported,
        "TriangleFan must fail closed to Legacy before Direct admission");

  std::vector<RecordSpec> oversizedRecords;
  oversizedRecords.reserve(640u);
  for (std::uint32_t i = 0; i < 640u; ++i) {
    oversizedRecords.push_back(fanRecord(i));
  }
  const auto oversizedWire = makeWireFixture(oversizedRecords);
  dxmt9::d3d9::ImportedChunkView oversizedImported;
  check(validateCommandChunk(oversizedWire.bytes, oversizedWire.envelope,
                             &oversizedImported).valid(),
        "oversized TriangleFan exclusion fixture validates");
  const auto oversizedPlan =
      dxmt9::d3d9::planCpuReadyChunk(oversizedImported, 81u);
  // A TriangleFan raw is not a direct-slot candidate at all: island
  // membership now has one authority (`kRecordTopology` plus the primitive
  // predicate), so the exact capacity pass rejects the same primitive the
  // range gate rejects. Before that, the capacity pass accepted fans and only
  // the gate excluded them -- a live second opinion about the same alphabet.
  check(oversizedPlan.reason == dxmt9::d3d9::ReplayReason::Oversize &&
            !oversizedPlan.directChunkSlotCandidate() &&
            dxmt9::d3d9::classifyDirectChunkSlotReplay(
                oversizedImported, oversizedPlan,
                /*captureOrTrace=*/false) ==
                dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyOversized,
        "oversized TriangleFan must fail closed before Direct admission");

  const std::array presentTailRecords{
      clearRecord(), fan,
      applyRenderStateRecord(RS_TEXTURE_FACTOR, 0x01020304u),
      drawRecord(identity, 1u), presentRecord()};
  const auto presentTailWire = makeWireFixture(presentTailRecords);
  dxmt9::d3d9::ImportedChunkView presentTailImported;
  check(validateCommandChunk(presentTailWire.bytes, presentTailWire.envelope,
                             &presentTailImported).valid(),
        "Present-tail TriangleFan exclusion fixture validates");
  const std::array supportedPresentTailRecords{
      clearRecord(), drawRecord(identity, 0u),
      applyRenderStateRecord(RS_TEXTURE_FACTOR, 0x01020304u),
      drawRecord(identity, 1u), presentRecord()};
  const auto supportedPresentTailWire =
      makeWireFixture(supportedPresentTailRecords);
  dxmt9::d3d9::ImportedChunkView supportedPresentTailImported;
  check(validateCommandChunk(supportedPresentTailWire.bytes,
                             supportedPresentTailWire.envelope,
                             &supportedPresentTailImported).valid(),
        "supported Present-tail classifier fixture validates");
  const auto presentTailPlan = dxmt9::d3d9::planCpuReadyChunk(
      supportedPresentTailImported, 82u);
  const auto supportedPresentTailDisposition =
      dxmt9::d3d9::classifyDirectChunkSlotReplay(
          supportedPresentTailImported, presentTailPlan,
          /*captureOrTrace=*/false);
  check(supportedPresentTailDisposition ==
            dxmt9::d3d9::DirectChunkSlotReplayDisposition::DirectWithPresentTail,
        "supported Present-tail classifier fixture must select Direct: " +
            std::to_string(
                static_cast<unsigned>(supportedPresentTailDisposition)));
  const auto presentTailDisposition =
      dxmt9::d3d9::classifyDirectChunkSlotReplay(
          presentTailImported, presentTailPlan, /*captureOrTrace=*/false);
  check(presentTailDisposition ==
            dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyUnsupported,
        "Present-tail TriangleFan must fail closed before Direct admission: " +
            std::to_string(static_cast<unsigned>(presentTailDisposition)));

  const std::array seedDraws{DrawParam{}};
  const std::array seedPayloads{DrawParamPayloadView{}};
  fixture.routing->queue_.submitDrawRun(
      CanonicalDrawState{}, DrawUniformPayload{}, seedDraws, seedPayloads);
  dxmt9::CommandQueueArenaLeaseTestAccess::reserveDirectContinuationHeadroom(
      fixture.routing->queue_);
  auto raw = makeRaw(makeWireFixture(records), 80u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
            fixture.routing->drawCalls == 1u &&
            !dxmt9::CommandQueueArenaLeaseTestAccess::stopped(
                fixture.routing->queue_),
        "populated-slot TriangleFan must replay once through Legacy without "
        "poisoning Direct continuation");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

void ordinaryChunkSlotSameRawCommitFailureDoesNotRetryLegacy() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "same-raw commit failure buffer constructs");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  const std::array records{drawRecord(identity)};
  auto raw = makeRaw(makeWireFixture(records), 79u, false, &registry);
  raw.cpuReadyTapePlanningEnabled = false;
  dxmt9::CommandQueueArenaLeaseTestAccess::
      forceNextDirectChunkSlotCommitFailure(fixture.routing->queue_);

  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) != D3D_OK &&
            fixture.routing->drawCalls == 0u &&
            fixture.routing->legacyMarkCalls == 0u,
        "same-raw ordinary Direct commit failure applies one draw and never "
        "retries Legacy");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::stopped(
            fixture.routing->queue_) &&
            dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                fixture.routing->queue_) == 0u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                fixture.routing->queue_) == 1u,
        "same-raw post-effect failure fail-stops before publication without "
        "rolling back the applied prefix");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

void deniedHeadroomPreservesQualifiedCapacity() {
  using Queue = dxmt9::CommandQueue;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  for (const auto count : {1u, 2u}) {
    Queue queue(Queue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    SourcePayloadCapacity first{};
    first.commandHeaders = first.clearRecords = 1u;
    auto initial = queue.beginDirectChunkSlotReplay(1u, first, sizeof(ClearDesc));
    check(initial.lease.has_value(), "initial empty slot admission succeeds");
    check(initial.lease->rollbackPreEffect(), "initial admission rolls back");
    // A legitimate retained owner, not a forged receipt. The test's small
    // runtime budget denies the optional re-provision while preserving it.
    auto retained = first;
    retained.drawPayloadBytes = 65536u;
    const auto old = Access::qualifyEmptyWritingCapacity(queue, retained);
    check(old.qualified(), "fixture retains real qualified storage");
    SourcePayloadCapacity next = first;
    next.commandHeaders = next.clearRecords = count;
    auto begin = queue.beginDirectChunkSlotReplay(2u, next, count * sizeof(ClearDesc));
    check(begin.lease.has_value() && !Access::stopped(queue),
          "denied headroom preserves ownership and admits exact required storage");
    const auto current = Access::writingCapacityReceipt(queue);
    check(current.qualified() && current.payloadIndex() == old.payloadIndex() &&
              current.generation() == old.generation() + (count == 2u ? 1u : 0u) &&
              (count != 1u || current == old),
          "actual admission preserves reuse receipt or transfers to one fresh generation");
    for (unsigned i = 0; i < count; ++i) queue.submitClear(ClearDesc{});
    check(begin.lease->commit({}) == Queue::DirectChunkSlotReplayStatus::Committed,
          "denial recovery commits commands exactly once");
    const auto published = queue.publishReplaySourceBoundary();
    check(published.published(), "recovered source publishes once");
    const auto completed = Access::consumeOne(queue);
    check(completed.completed && completed.reclaimed && !Access::stopped(queue),
          "qualified denial recovery survives detach, completion and reclaim");
  }
}

void qualifiedCapacitySurvivesOrdinaryGrowth() {
  using Queue = dxmt9::CommandQueue;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  Queue queue(Queue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  SourcePayloadCapacity first{};
  first.commandHeaders = first.clearRecords = 1u;
  auto initial = queue.beginDirectChunkSlotReplay(1u, first, sizeof(ClearDesc));
  check(initial.lease.has_value() && initial.lease->rollbackPreEffect(),
        "ordinary growth fixture acquires an empty destination");
  const auto old = Access::qualifyEmptyWritingCapacity(queue, first);
  check(old.qualified(), "ordinary source inherits a real retained capacity owner");
  // This bypasses Direct intentionally: compatibility commands may reuse a
  // physical payload which a previous Direct source provisioned and reclaimed.
  queue.submitClear(ClearDesc{});
  check(Access::writingCapacityReceipt(queue) == old,
        "in-capacity ordinary append preserves the prior generation");
  queue.submitClear(ClearDesc{});
  const auto grown = Access::writingCapacityReceipt(queue);
  check(grown.qualified() && grown.payloadIndex() == old.payloadIndex() &&
            grown.generation() == old.generation() + 1 &&
            grown.retainedBytes() > old.retainedBytes(),
        "ordinary growth settles its same physical owner before publication");
  check(queue.publishReplaySourceBoundary().published(),
        "ordinary growth publishes the qualified source once");
  const auto completed = Access::consumeOne(queue);
  check(completed.completed && completed.reclaimed && !Access::stopped(queue),
        "ordinary growth must settle qualified credit before detach and reclaim");

  for (unsigned fault = 0; fault != kDirectChunkSlotPhysicalDimensionCount; ++fault) {
    Queue failed(Queue::ArenaLeaseTestQueueTag{}, BackendLimits{});
    auto begin = failed.beginDirectChunkSlotReplay(1u, first, sizeof(ClearDesc));
    check(begin.lease && begin.lease->rollbackPreEffect(), "fault fixture gets a writer");
    check(Access::qualifyEmptyWritingCapacity(failed, first).qualified(),
          "fault fixture starts with retained authority");
    check(Access::ordinaryCapacityFailure(failed, fault),
          "each partial ordinary reserve failure is accounted and fails before publication");
  }

  Queue invalid(Queue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto begin = invalid.beginDirectChunkSlotReplay(1u, first, sizeof(ClearDesc));
  check(begin.lease && begin.lease->rollbackPreEffect(), "stale fixture gets a writer");
  const auto original = Access::qualifyEmptyWritingCapacity(invalid, first);
  Access::growWithoutCapacityWitness(invalid);
  check(!invalid.publishReplaySourceBoundary().published() && Access::stopped(invalid) &&
            Access::writingCapacityReceipt(invalid) == original && Access::readyCount(invalid) == 0,
        "publication rejects unpriced growth instead of minting a late receipt");
}

void captureSourceBoundaryReturnsExactQueuePublication() {
  using Queue = dxmt9::CommandQueue;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Status = Queue::ReplaySourceBoundaryStatus;
  using Expected = Queue::ReplaySourceBoundaryExpectation;
  Queue queue(Queue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  const auto initialSequence = Access::nextSeqId(queue);
  const auto empty = queue.publishReplaySourceBoundary();
  check(empty.status == Status::Empty && !empty.source.valid() &&
            !empty.metadata.valid() &&
            Access::nextSeqId(queue) == initialSequence &&
            Access::readyCount(queue) == 0u,
        "empty source settlement neither publishes nor invents queue identity");

  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1u;
  capacity.clearRecords = 1u;
  const CpuReadyProducerIdentity producer{77u, 77u, 77u, 77u};
  Expected previous{};
  for (std::uint32_t source = 0u; source < 2u; ++source) {
    // One full raw owner can settle several local publications. Raw identity
    // belongs to the private lease, not Tape's strict raw-publication waterline.
    auto begin = queue.beginDirectChunkSlotReplay(
        77u, capacity, sizeof(ClearDesc), producer);
    check(begin.lease.has_value(), "each source admits its real Direct lease");
    const auto ticket = begin.lease->ticket();
    const Expected expected{
        .source = {.id = ticket.id, .storage = ticket.storage},
        .seqId = ticket.seqId,
    };
    check(queue.publishReplaySourceBoundary(expected).status == Status::Rejected,
          "a source boundary cannot cut an active Direct transaction");
    queue.submitClear(ClearDesc{});
    check(begin.lease->commit(std::span<const ChunkHandleEntry>{}) ==
              Queue::DirectChunkSlotReplayStatus::Committed,
          "Direct commit settles storage before source publication");

    std::array<Expected, 8> stale{};
    stale.fill(expected);
    ++stale[0].seqId;
    ++stale[1].source.id.index;
    ++stale[2].source.id.generation;
    ++stale[3].source.storage.firstPage;
    ++stale[4].source.storage.pageCount;
    ++stale[5].source.storage.generation;
    stale[6].source = {};
    stale[7].seqId = 0u;
    stale[7].source.storage = {};  // partial source is not an omitted witness
    for (const auto& invalid : stale) {
      check(queue.publishReplaySourceBoundary(invalid).status == Status::Rejected &&
                Access::nextSeqId(queue) == ticket.seqId &&
                Access::readyCount(queue) == 0u &&
                Access::writingCommandCount(queue) == 1u,
            "stale or malformed expectation rejects before publication effects");
    }
    if (source != 0u) {
      check(queue.publishReplaySourceBoundary(previous).status == Status::Rejected,
            "a previous reclaimed source cannot flush the new writer");
    }
    const auto receipt = queue.publishReplaySourceBoundary(expected);
    check(receipt.status == Status::Published && receipt.published() &&
              receipt.source == expected.source && receipt.metadata.valid() &&
              receipt.metadata.rawOrdinal == 0u &&
              receipt.metadata.seqId == ticket.seqId &&
              receipt.metadata.sourceOrdinal == ticket.seqId &&
              receipt.controlIndex != std::numeric_limits<std::size_t>::max() &&
              Access::nextSeqId(queue) == ticket.seqId + 1u &&
              Access::readyCount(queue) == 1u,
          "receipt captures the actual queue-local publication, not raw provenance");
    check(queue.publishReplaySourceBoundary(expected).status == Status::Rejected,
          "an already published source is not an empty or duplicate settlement");
    const auto completed = Access::consumeOne(queue);
    check(completed.dequeued && completed.completed && completed.reclaimed &&
              completed.rawOrdinal == 0u && completed.seqId == receipt.metadata.seqId &&
              completed.sourceOrdinal == receipt.metadata.sourceOrdinal &&
              Access::residentCount(queue) == 0u,
          "published receipt survives the real completion and reclamation path");
    previous = expected;
  }

  queue.submitClear(ClearDesc{});
  const auto ordinary = queue.publishReplaySourceBoundary();
  check(ordinary.status == Status::Published && ordinary.metadata.rawOrdinal == 0u &&
            Access::readyCount(queue) == 1u,
        "ordinary Clear source also returns an actual local receipt");
  check(Access::consumeOne(queue).reclaimed,
        "ordinary source retains completion/reclaim semantics");
  queue.submitPresent(SwapDesc{});
  const auto afterPresent = queue.publishReplaySourceBoundary();
  check(afterPresent.status == Status::Empty && !afterPresent.source.valid() &&
            Access::readyCount(queue) == 1u,
        "Present already published; empty boundary is not invented Present evidence");
  check(Access::consumeOne(queue).reclaimed,
        "Present source completes without an extra publication");

  Queue stopped(Queue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  {
    auto unfinished = stopped.beginDirectChunkSlotReplay(
        78u, capacity, sizeof(ClearDesc), producer);
    check(unfinished.lease.has_value(), "failure-prefix fixture admits");
    stopped.submitClear(ClearDesc{});
    unfinished.lease->markSemanticEffectsStarted();
  }
  check(Access::stopped(stopped) &&
            stopped.publishReplaySourceBoundary().status == Status::Rejected &&
            Access::readyCount(stopped) == 0u,
        "post-effect abandoned source fails closed without publishing its prefix");
}

void providerSourcePolicyPreservesRawOrderAndSettlement() {
  namespace replay = dxmt9::d3d9;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  struct Collector {
    dxmt9::CommandQueue* queue = nullptr;
    std::uint64_t raw = 0u;
    std::uint32_t ranges = 0u;
    std::uint32_t empty = 0u;
    std::uint32_t presents = 0u;
    bool reject = false;
    std::vector<std::uint64_t> commands;
    static bool settled(void* opaque,
                        const replay::ProviderReplaySourceSettlement& value) noexcept {
      auto& self = *static_cast<Collector*>(opaque);
      try {
        if (value.rawOrdinal == 0u || value.rangeIndex != self.ranges ||
            (self.raw != 0u && self.raw != value.rawOrdinal)) return false;
        self.raw = value.rawOrdinal;
        ++self.ranges;
        if (value.kind == replay::ProviderReplaySourceSettlementKind::Empty) {
          if (!value.publications.empty()) return false;
          ++self.empty;
        }
        for (const auto& receipt : value.publications) {
          if (!receipt.source.valid() || !receipt.metadata.valid() ||
              receipt.metadata.rawOrdinal != 0u ||
              Access::readyCount(*self.queue) == 0u) return false;
          const auto expected = Access::peekReadyCompletion(*self.queue);
          if (expected.source != receipt.source ||
              expected.seqId != receipt.metadata.seqId) return false;
          const auto completion = Access::consumeOne(*self.queue);
          if (completion.arena || !completion.reclaimed ||
              completion.seqId != receipt.metadata.seqId ||
              completion.hasPresent != receipt.hasPresent) return false;
          self.presents += receipt.hasPresent;
          self.commands.insert(self.commands.end(), completion.commandDigests.begin(),
                               completion.commandDigests.end());
        }
        return !self.reject && Access::readyCount(*self.queue) == 0u;
      } catch (...) {
        return false;
      }
    }
  };
  const D9CWireObjectIdentity identity{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 1u, .objectId = 1u};
  const std::array records{
      drawRecord(identity), drawRecord(identity, 1u), presentRecord(),
      applyRenderStateRecord(RS_TEXTURE_FACTOR, 0x11223344u),
      drawRecord(identity, 2u), presentRecord(),
      applyRenderStateRecord(RS_TEXTURE_FACTOR, 0x55667788u)};
  const auto wire = makeWireFixture(records);
  const auto original = wire.bytes;
  const std::array<replay::ProviderReplayRecordRange, 4> ranges{{
      {0u, 2u}, {2u, 1u}, {3u, 3u}, {6u, 1u}}};
  std::vector<std::uint64_t> reference;
  for (const auto policy : {replay::ProviderReplayPolicy::Compatibility,
                            replay::ProviderReplayPolicy::ProductionSlot}) {
    RuntimeFixture fixture(false, false, true, false);
    auto* buffer = dxmt9c_device_create_vertex_buffer(
        fixture.cDevice.get(), 256u, 0u, 0u, 0u);
    check(buffer != nullptr, "source-policy buffer constructs");
    std::array<void*, 3> resolved{buffer, buffer, buffer};
    Collector collector{.queue = &fixture.routing->queue_};
    check(replay::replayPrevalidatedResolvedCommandChunkBySources(
              fixture.cDevice.get(), wire.bytes, wire.envelope, resolved, policy,
              ranges, {&collector, Collector::settled}) == D3D_OK,
          "source policy executes one full raw through its borrowed ranges");
    check(collector.ranges == 4u && collector.empty == 1u && collector.presents == 2u &&
              wire.bytes == original && Access::residentCount(*collector.queue) == 0u &&
              !Access::stopped(*collector.queue) &&
              fixture.cDevice->dev().state().renderStates[RS_TEXTURE_FACTOR] ==
                  0x55667788u,
          "two Presents and state-only settlement preserve raw ownership and final state");
    if (policy == replay::ProviderReplayPolicy::Compatibility) {
      check(fixture.routing->drawCalls == 3u, "compatibility source path is non-vacuous");
      reference = collector.commands;
    } else {
      check(fixture.routing->drawCalls == 0u && !collector.commands.empty() &&
                collector.commands == reference,
            "ordinary Direct sources execute identical commands without a carrier or Arena");
    }
    dxmt9c_buffer_release(buffer);
  }

  RuntimeFixture oversized(false, false, true, false);
  auto* oversizedBuffer = dxmt9c_device_create_vertex_buffer(
      oversized.cDevice.get(), 256u, 0u, 0u, 0u);
  check(oversizedBuffer != nullptr, "source-policy oversize buffer constructs");
  std::array<void*, 3> oversizedResolved{oversizedBuffer, oversizedBuffer,
                                          oversizedBuffer};
  auto oversizedEnvelope = wire.envelope;
  oversizedEnvelope.recordCount = replay::kProviderSourceReplayMaxRecords + 1u;
  const std::array<replay::ProviderReplayRecordRange, 1> oversizedRanges{{
      {0u, replay::kProviderSourceReplayMaxRecords + 1u}}};
  check(replay::replayPrevalidatedResolvedCommandChunkBySources(
            oversized.cDevice.get(), wire.bytes, oversizedEnvelope,
            oversizedResolved, replay::ProviderReplayPolicy::ProductionSlot,
            oversizedRanges) != D3D_OK &&
            oversized.routing->drawCalls == 0u &&
            Access::readyCount(oversized.routing->queue_) == 0u &&
            Access::writingCommandCount(oversized.routing->queue_) == 0u,
        "source-policy rejects over-cap records before queue effects");
  dxmt9c_buffer_release(oversizedBuffer);

  RuntimeFixture failed(false, false, true, false);
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      failed.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "source-policy failure buffer constructs");
  std::array<void*, 3> resolved{buffer, buffer, buffer};
  Collector rejected{.queue = &failed.routing->queue_, .reject = true};
  check(replay::replayPrevalidatedResolvedCommandChunkBySources(
            failed.cDevice.get(), wire.bytes, wire.envelope, resolved,
            replay::ProviderReplayPolicy::ProductionSlot, ranges,
            {&rejected, Collector::settled}) != D3D_OK &&
            rejected.ranges == 1u && rejected.presents == 0u &&
            Access::stopped(*rejected.queue) && failed.routing->drawCalls == 0u,
        "consumer failure after published prefix stops; never retries the whole raw");
  dxmt9c_buffer_release(buffer);
}

void sourcePublicationCaptureOwnsEveryPublicationAndFailsClosed() {
  using Queue = dxmt9::CommandQueue;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Receipt = dxmt9::core::metalqueue::ReplaySourcePublicationReceipt;
  std::array<Receipt, 4> receipts{};
  Queue queue(Queue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto empty = queue.beginReplaySourcePublicationCapture(receipts);
  check(empty.has_value() &&
            !queue.beginReplaySourcePublicationCapture(receipts).has_value(),
        "publication scope is exclusive");
  auto moved = std::move(*empty);
  check(!empty->finish().complete, "moved-from scope has no finish authority");
  const auto noWork = moved.finish();
  check(noWork.complete && noWork.count == 0u && Access::readyCount(queue) == 0u,
        "empty scope settles without inventing source identity");

  auto implicit = queue.beginReplaySourcePublicationCapture(receipts);
  check(implicit.has_value(), "implicit publication scope starts");
  queue.submitClear(ClearDesc{});
  queue.submitPresent(SwapDesc{});
  const auto observed = implicit->finish();
  check(observed.complete && observed.count == 1u && receipts[0].hasPresent &&
            receipts[0].metadata.rawOrdinal == 0u &&
            Access::peekReadyCompletion(queue).source == receipts[0].source,
        "Present's actual publication is observed without an explicit flush");
  check(Access::consumeOne(queue).reclaimed, "implicit source completes");

  auto repeatedRaw = queue.beginReplaySourcePublicationCapture(receipts);
  check(repeatedRaw.has_value(), "repeated-private-raw capture begins");
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 2u;
  capacity.clearRecords = 1u;
  capacity.presentRecords = 1u;
  const CpuReadyProducerIdentity producer{77u, 77u, 77u, 77u};
  for (unsigned index = 0u; index < 2u; ++index) {
    auto begin = queue.beginDirectChunkSlotReplay(
        77u, capacity, sizeof(ClearDesc) + sizeof(SwapDesc), producer);
    check(begin.has_value(), "same private raw admits a fresh Direct Present source");
    queue.submitClear(ClearDesc{});
    queue.submitPresent(SwapDesc{});
    check(begin.lease->commit({}) == Queue::DirectChunkSlotReplayStatus::Committed,
          "Present keeps private raw witness but publishes queue-local metadata");
  }
  const auto repeated = repeatedRaw->finish();
  check(repeated.complete && repeated.count == 2u && receipts[0].hasPresent &&
            receipts[1].hasPresent && receipts[0].metadata.rawOrdinal == 0u &&
            receipts[1].metadata.rawOrdinal == 0u &&
            receipts[1].metadata.seqId > receipts[0].metadata.seqId &&
            Access::consumeOne(queue).reclaimed && Access::consumeOne(queue).reclaimed,
        "repeated Presents do not reuse nonzero raw-publication high water");

  Queue prefix(Queue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto incomplete = prefix.beginReplaySourcePublicationCapture(receipts);
  check(incomplete.has_value(), "unfinished source capture begins");
  prefix.submitClear(ClearDesc{});
  check(!incomplete->finish().complete && Access::stopped(prefix) &&
            Access::readyCount(prefix) == 0u && Access::writingCommandCount(prefix) == 1u,
        "unpublished writing effects cannot produce a complete settlement");

  Queue overflow(Queue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto tooSmall = overflow.beginReplaySourcePublicationCapture({});
  check(tooSmall.has_value(), "zero-capacity capture can represent Empty only");
  overflow.submitPresent(SwapDesc{});
  check(!tooSmall->finish().complete && Access::stopped(overflow),
        "receipt overflow cannot be mistaken for successful truncated evidence");

  Queue active(Queue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto early = active.beginReplaySourcePublicationCapture(receipts);
  check(early.has_value(), "active-lease capture begins");
  auto lease = active.beginDirectChunkSlotReplay(
      79u, capacity, sizeof(ClearDesc) + sizeof(SwapDesc), producer);
  check(lease.has_value(), "active-lease fixture admits");
  check(!early->finish().complete && Access::stopped(active),
        "scope cannot detach while a Direct transaction still owns publication");
}

void directChunkSlotRollbackAndPostEffectFailureAreFailSafe() {
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.clearRecords = 1;
  const auto producerIdentity = [](std::uint64_t ordinal) {
    return CpuReadyProducerIdentity{
        ordinal, ordinal, ordinal, ordinal};
  };

  dxmt9::CommandQueue rollbackQueue(
      dxmt9::CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  dxmt9::CommandQueueArenaLeaseTestAccess::enableDirectLifecycleObservation(
      rollbackQueue);
  auto rollback = rollbackQueue.beginDirectChunkSlotReplay(
      81u, capacity, sizeof(ClearDesc), producerIdentity(81u));
  check(rollback.status ==
                dxmt9::CommandQueue::DirectChunkSlotReplayStatus::Ready &&
            rollback.lease.has_value(),
        "pre-effect rollback fixture acquires a generation-qualified borrow");
  rollbackQueue.submitClear(ClearDesc{});
  check(rollback.lease->rollbackPreEffect() &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                rollbackQueue) == 0u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::directLifecycleError(
                rollbackQueue) ==
                dxmt9::queue::DirectSourceLifecycleError::None &&
            dxmt9::CommandQueueArenaLeaseTestAccess::directLifecycleState(
                rollbackQueue).recordCount == 0u &&
            !dxmt9::CommandQueueArenaLeaseTestAccess::stopped(rollbackQueue),
        "pre-effect rollback restores the slot and removes its Direct ledger row");
  rollbackQueue.submitClear(ClearDesc{});
  rollbackQueue.submitPresent(SwapDesc{});
  const auto rollbackFallback =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(rollbackQueue);
  check(rollbackFallback.reclaimed &&
            dxmt9::CommandQueueArenaLeaseTestAccess::directLifecycleError(
                rollbackQueue) ==
                dxmt9::queue::DirectSourceLifecycleError::None &&
            dxmt9::CommandQueueArenaLeaseTestAccess::directLifecycleState(
                rollbackQueue).recordCount == 0u,
        "compatibility Publish after rollback is ignored without a RolledBack leak");

  dxmt9::CommandQueue unsettledQueue(
      dxmt9::CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  {
    auto unsettled = unsettledQueue.beginDirectChunkSlotReplay(
        83u, capacity, sizeof(ClearDesc), producerIdentity(83u));
    check(unsettled.lease.has_value(),
          "post-effect unsettled fixture acquires direct destination ownership");
    unsettled.lease->markSemanticEffectsStarted();
    unsettledQueue.submitClear(ClearDesc{});
  }
  check(dxmt9::CommandQueueArenaLeaseTestAccess::stopped(unsettledQueue) &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                unsettledQueue) == 1u,
        "post-effect lease settlement abandons the applied prefix and fail-stops");

  dxmt9::CommandQueue failStopQueue(
      dxmt9::CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto failStop = failStopQueue.beginDirectChunkSlotReplay(
      82u, capacity, sizeof(ClearDesc), producerIdentity(82u));
  check(failStop.lease.has_value(),
        "post-effect fail-stop fixture acquires direct destination ownership");
  failStop.lease->markSemanticEffectsStarted();
  failStopQueue.submitClear(ClearDesc{});
  failStopQueue.submitDrawRun(CanonicalDrawState{}, DrawUniformPayload{}, {});
  const auto status = failStop.lease->commit(
      std::span<const ChunkHandleEntry>{});
  check(status ==
                dxmt9::CommandQueue::DirectChunkSlotReplayStatus::FailStopped &&
            dxmt9::CommandQueueArenaLeaseTestAccess::stopped(failStopQueue) &&
            dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(failStopQueue) ==
                0u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                failStopQueue) == 1u,
        "post-effect unsupported carrier ingress fail-stops without "
        "publication, rollback, or retry");

  const auto unsupported = failStopQueue.beginDirectChunkSlotReplay(
      0u, capacity, sizeof(ClearDesc), producerIdentity(82u));
  check(unsupported.status ==
            dxmt9::CommandQueue::DirectChunkSlotReplayStatus::LegacyUnsupported,
        "invalid pre-effect admission retains a typed Legacy disposition");
}

void directChunkSlotCapabilityLifetimeIsTerminalSafe() {
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1u;
  capacity.clearRecords = 1u;
  const auto producerIdentity = [](std::uint64_t ordinal) {
    return CpuReadyProducerIdentity{
        ordinal, ordinal, ordinal, ordinal};
  };

  dxmt9::CommandQueue commitQueue(
      dxmt9::CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto commit = commitQueue.beginDirectChunkSlotReplay(
      84u, capacity, sizeof(ClearDesc), producerIdentity(84u));
  check(commit.lease.has_value(),
        "capability lifetime commit fixture acquires a direct lease");
  const auto* committedCapability =
      commit.lease->borrowDirectRangeAppender();
  check(committedCapability && static_cast<bool>(*committedCapability),
        "borrowed capability is armed while its lease is live");
  commitQueue.submitClear(ClearDesc{});
  check(commit.lease->commit(std::span<const ChunkHandleEntry>{}) ==
                dxmt9::CommandQueue::DirectChunkSlotReplayStatus::Committed &&
            !static_cast<bool>(*committedCapability) &&
            committedCapability->append({}) ==
                dxmt9::core::DirectReplayDrawDisposition::LegacyPreEffectFailure,
        "committed capability is disarmed and cannot access stale state");

  dxmt9::CommandQueue rollbackQueue(
      dxmt9::CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto rollback = rollbackQueue.beginDirectChunkSlotReplay(
      85u, capacity, sizeof(ClearDesc), producerIdentity(85u));
  check(rollback.lease.has_value(),
        "capability lifetime rollback fixture acquires a direct lease");
  const auto* rolledBackCapability =
      rollback.lease->borrowDirectRangeAppender();
  check(rolledBackCapability && static_cast<bool>(*rolledBackCapability),
        "rollback capability is armed before terminal settlement");
  check(rollback.lease->rollbackPreEffect() &&
            !static_cast<bool>(*rolledBackCapability) &&
            rolledBackCapability->append({}) ==
                dxmt9::core::DirectReplayDrawDisposition::LegacyPreEffectFailure,
        "rolled-back capability is disarmed and cannot access stale state");

  dxmt9::CommandQueue moveQueue(
      dxmt9::CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto source = moveQueue.beginDirectChunkSlotReplay(
      86u, capacity, sizeof(ClearDesc), producerIdentity(86u));
  check(source.lease.has_value(),
        "capability lifetime move fixture acquires a direct lease");
  const auto* movedFromCapability = source.lease->borrowDirectRangeAppender();
  check(movedFromCapability && static_cast<bool>(*movedFromCapability),
        "move fixture capability is armed before lease transfer");
  auto destination = std::move(*source.lease);
  const auto* movedCapability = destination.borrowDirectRangeAppender();
  check(!static_cast<bool>(*movedFromCapability) &&
            movedFromCapability->append({}) ==
                dxmt9::core::DirectReplayDrawDisposition::LegacyPreEffectFailure &&
            movedCapability && static_cast<bool>(*movedCapability),
        "moved-from capability is disarmed while destination is re-armed");
  check(destination.rollbackPreEffect() &&
            !static_cast<bool>(*movedCapability) &&
            movedCapability->append({}) ==
                dxmt9::core::DirectReplayDrawDisposition::LegacyPreEffectFailure,
        "moved destination capability disarms at rollback");
}

void transactionalArenaFailureInjectionDisposition() {
  {
    RuntimeFixture fixture;
    SourcePayloadCapacity capacity{};
    capacity.commandHeaders = 1u;
    capacity.clearRecords = 1u;
    const auto segment = makeSourcePayloadLayout(
        capacity, fixture.routing->queue_.cpuReadyArenaPlanLimits().pageSize,
        fixture.routing->queue_.cpuReadyArenaPlanLimits().maxPagesPerSource);
    check(segment.has_value(), "capacity injection layout must build");
    const std::array segments{*segment};
    const auto layout = makeArenaSourcePayloadLayout(
        segments, fixture.routing->queue_.cpuReadyArenaPlanLimits().pageSize,
        fixture.routing->queue_.cpuReadyArenaPlanLimits().maxPagesPerSource);
    check(layout.has_value(), "capacity injection source layout must build");
    dxmt9::CommandQueueArenaLeaseTestAccess::forceNextCapacityFailure(
        fixture.routing->queue_);
    const auto begin = fixture.routing->queue_.beginCpuReadyArenaSource(
        70u, *layout);
    check(begin.status ==
              dxmt9::CommandQueue::CpuReadyArenaBeginStatus::TemporaryPressure,
          "capacity injection must deterministically report pressure");
  }
  {
    RuntimeFixture fixture;
    const std::array records{clearRecord()};
    auto raw = makeRaw(makeWireFixture(records), 71u);
    dxmt9::CommandQueueArenaLeaseTestAccess::forceNextValidationFailure(
        fixture.routing->queue_);
    check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) != D3D_OK &&
              fixture.routing->queue_.cpuReadyArenaPoisoned() &&
              dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                  fixture.routing->queue_) == 0u,
          "validation injection must fail-stop before Ready publication");
  }
  {
    RuntimeFixture fixture;
    const std::array records{clearRecord()};
    auto raw = makeRaw(makeWireFixture(records), 72u);
    dxmt9::CommandQueueArenaLeaseTestAccess::forceNextResourceRetainFailure(
        fixture.routing->queue_);
    check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) != D3D_OK &&
              fixture.routing->queue_.cpuReadyArenaPoisoned() &&
              dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                  fixture.routing->queue_) == 0u,
          "resource-retain injection must fail-stop after marks without Ready");
  }
  {
    RuntimeFixture fixture;
    const std::array records{clearRecord()};
    auto raw = makeRaw(makeWireFixture(records), 73u);
    dxmt9::CommandQueueArenaLeaseTestAccess::forceNextPublicationFailure(
        fixture.routing->queue_);
    check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) != D3D_OK &&
              fixture.routing->queue_.cpuReadyArenaPoisoned() &&
              dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                  fixture.routing->queue_) == 0u,
          "publication injection must fail-stop after ownership binding");
  }
  {
    RuntimeFixture fixture;
    const std::array records{clearRecord()};
    auto raw = makeRaw(makeWireFixture(records), 74u);
    check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK,
          "completion injection setup must publish a source");
    dxmt9::CommandQueueArenaLeaseTestAccess::forceNextCompletionFailure(
        fixture.routing->queue_);
    const auto completion =
        dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
            fixture.routing->queue_);
    check(completion.dequeued && completion.submitted &&
              !completion.completed && !completion.reclaimed &&
              dxmt9::CommandQueueArenaLeaseTestAccess::pendingCompletionCount(
                  fixture.routing->queue_) == 0u &&
              dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
                  fixture.routing->queue_) == 1u,
          "completion injection must consume its owner and fail-stop before "
          "waterline/reclaim");
  }
}

void stateOnlyRawMutatesWithoutTicket() {
  RuntimeFixture fixture;
  constexpr std::uint32_t kValue = 0x13579bdfu;
  const std::array records{
      applyRenderStateRecord(RS_TEXTURE_FACTOR, kValue),
  };
  auto raw = makeRaw(makeWireFixture(records), 1);
  const auto hr = dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw);
  check(hr == D3D_OK &&
            fixture.cDevice->dev().state().renderStates[RS_TEXTURE_FACTOR] ==
                kValue,
        "StateOnly raw must apply its replay shadow mutation");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
            fixture.routing->queue_) == 1 &&
            dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                fixture.routing->queue_) == 0 &&
            dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
                fixture.routing->queue_) == 0,
        "StateOnly raw must not reserve a ticket or publish a source");
}

void postSemanticDirectFailureDoesNotFallback() {
  RuntimeFixture fixture(/*rejectAfterClear=*/true);
  const std::array records{clearRecord()};
  auto raw = makeRaw(makeWireFixture(records), 1);
  const auto hr = dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw);
  check(hr < 0 && fixture.routing->clearCalls == 1,
        "post-semantic Direct failure must fail-stop without replaying Legacy");
  check(fixture.routing->queue_.cpuReadyArenaPoisoned() &&
            dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                fixture.routing->queue_) == 0 &&
            dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
                fixture.routing->queue_) == 0 &&
            dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
                fixture.routing->queue_) == 2,
        "failed Direct identity is consumed and reclaimed without publication");
  check(fixture.routing->organicFailureObserved &&
            fixture.routing->organicFailure.failureClass ==
                dxmt9::CommandQueue::CpuReadyArenaFailureClass::ActiveArenaRejected &&
            fixture.routing->organicFailure.source == 0u &&
            fixture.routing->organicFailure.segment == 0u,
        "organic replayRawChunk failure must retain its typed active coordinates");
}

void batchBuilderFailureRetriesCompleteEventSerialExactlyOnce() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/true);
  // Two bounded GPU records exceed the 64-page source bound together, so the
  // planner admits two SegmentSerial sources. The native seam rejects the
  // batch builder before semantic replay; production routing must roll back
  // that whole admission and replay the raw event once through EventSerial.
  const auto rects = clearRectCountForPlannerPages(40);
  const std::array records{clearRecord(rects), clearRecord(rects),
                           clearRecord(rects),
                           presentRecord()};
  auto raw = makeRaw(makeWireFixture(records), 1, /*captureIdentity=*/true);
  dxmt9::CommandQueueArenaLeaseTestAccess::forceNextBatchBuilderFailure(
      fixture.routing->queue_);
  const auto hr = dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw);
  check(hr == D3D_OK && fixture.routing->clearCalls == 3u &&
            fixture.routing->presentCalls == 1u,
        "batch builder rejection must retry the complete raw event once in "
        "EventSerial order");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
            fixture.routing->queue_) == 1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
                fixture.routing->queue_) == 2u &&
            !fixture.routing->queue_.cpuReadyArenaPoisoned(),
        "EventSerial retry must leave one ordered Ready source and no poison");
}

void captureIdentityBeginFailureRetriesCompleteEventSerialExactlyOnce() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/true);
  const auto rects = clearRectCountForPlannerPages(40);
  const std::array records{clearRecord(rects), clearRecord(rects),
                           clearRecord(rects), presentRecord()};
  auto raw = makeRaw(makeWireFixture(records), 5, /*captureIdentity=*/true);
  dxmt9::CommandQueueArenaLeaseTestAccess::forceNextCaptureIdentityBeginFailure(
      fixture.routing->queue_);
  const auto hr = dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw);
  check(hr == D3D_OK && fixture.routing->clearCalls == 3u &&
            fixture.routing->presentCalls == 1u,
        "capture identity begin rejection must retry the complete raw event "
        "once before semantic effects");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
            fixture.routing->queue_) == 1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
                fixture.routing->queue_) == 2u &&
            !fixture.routing->queue_.cpuReadyArenaPoisoned(),
        "capture identity begin rollback must leave one EventSerial Ready "
        "source and an unpoisoned queue");
}

void postSemanticBatchPublishFailureFailsStopsWithoutRetry() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/true);
  const auto rects = clearRectCountForPlannerPages(40);
  const std::array records{clearRecord(rects), clearRecord(rects),
                           clearRecord(rects),
                           presentRecord()};
  auto raw = makeRaw(makeWireFixture(records), 3, /*captureIdentity=*/true);
  dxmt9::CommandQueueArenaLeaseTestAccess::forceNextPostSemanticPublishFailure(
      fixture.routing->queue_);
  const auto hr = dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw);
  check(hr < 0 && fixture.routing->clearCalls == 3u &&
            fixture.routing->presentCalls == 1u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                fixture.routing->queue_) == 0u &&
            fixture.routing->queue_.cpuReadyArenaPoisoned(),
        "post-semantic publish failure must fail-stop without EventSerial "
        "retry or duplicate semantic effects");
}

void firstOrganicFailureIsRetainedWithCoordinates() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/true);
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1u;
  capacity.clearRecords = 1u;
  const auto limits = fixture.routing->queue_.cpuReadyArenaPlanLimits();
  const auto segment = makeSourcePayloadLayout(
      capacity, limits.pageSize, limits.maxOrdinaryPagesPerSegment);
  check(segment.has_value(), "failure retention segment must build");
  const std::array segments{*segment};
  const auto layout = makeArenaSourcePayloadLayout(
      segments, limits.pageSize, limits.maxPagesPerSource);
  check(layout.has_value(), "failure retention layout must build");
  auto lease = fixture.routing->queue_.beginCpuReadyArenaSource(31u, *layout);
  check(lease.has_value(), "failure retention lease must admit");
  fixture.routing->queue_.rejectActiveCpuReadyArenaSource();
  ClearDesc clear{};
  fixture.routing->queue_.submitClear(clear);
  const auto failure =
      dxmt9::CommandQueueArenaLeaseTestAccess::activeFailure(
          fixture.routing->queue_);
  check(failure.failureClass ==
            dxmt9::CommandQueue::CpuReadyArenaFailureClass::ActiveArenaRejected &&
            failure.source == 0u && failure.segment == 0u &&
            failure.plannedPages != std::numeric_limits<std::uint32_t>::max(),
        "first organic failure class and active coordinates must survive a "
        "later failed append");
  lease->abortForFallback();
}

void organicBatchAppendFailureFromReplayFailsOnce() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/true);
  fixture.routing->rejectAfterFinalClear_ = true;
  const auto rects = clearRectCountForPlannerPages(40);
  const std::array records{clearRecord(rects), clearRecord(rects),
                           clearRecord(rects), presentRecord()};
  auto raw = makeRaw(makeWireFixture(records), 17u, /*captureIdentity=*/true);
  const auto hr = dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw);
  const auto readback =
      dxmt9::CommandQueueArenaLeaseTestAccess::takeFailure(
          fixture.routing->queue_);
  const auto consumed =
      dxmt9::CommandQueueArenaLeaseTestAccess::takeFailure(
          fixture.routing->queue_);
  check(hr < 0 && fixture.routing->clearCalls == 3u &&
            fixture.routing->presentCalls == 1u &&
            readback.failureClass ==
                dxmt9::CommandQueue::CpuReadyArenaFailureClass::ActiveArenaRejected &&
            readback.source == 2u && readback.segment == 0u &&
            readback.actualCommands == 1u &&
            consumed.failureClass ==
                dxmt9::CommandQueue::CpuReadyArenaFailureClass::None,
        "organic replay result: hr=" + std::to_string(hr) +
        " clears=" + std::to_string(fixture.routing->clearCalls.load()) +
        " presents=" + std::to_string(fixture.routing->presentCalls.load()) +
        " class=" + std::to_string(static_cast<unsigned>(readback.failureClass)) +
        " source=" + std::to_string(readback.source) +
        " actual=" + std::to_string(readback.actualCommands) +
        " consumed=" +
            std::to_string(static_cast<unsigned>(consumed.failureClass)));
}

void workerPressureWaitResumesAfterActiveLeasePublishes() {
  RuntimeFixture fixture;
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.clearRecords = 1;
  const auto limits = fixture.routing->queue_.cpuReadyArenaPlanLimits();
  const auto segment = makeSourcePayloadLayout(
      capacity, limits.pageSize, limits.maxOrdinaryPagesPerSegment);
  check(segment.has_value(), "pressure fixture arena segment must build");
  const std::array segments{*segment};
  const auto layout = makeArenaSourcePayloadLayout(
      segments, limits.pageSize, limits.maxPagesPerSource);
  check(layout.has_value(), "pressure fixture arena layout must build");
  auto active = fixture.routing->queue_.beginCpuReadyArenaSource(1, *layout);
  check(active.has_value(), "pressure fixture must hold one active lease");

  const std::array records{clearRecord()};
  auto raw = makeRaw(makeWireFixture(records), 2);
  std::atomic<std::int32_t> replayHr{D3DERR_DEVICELOST};
  std::jthread worker([&] {
    replayHr.store(
        dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw),
        std::memory_order_release);
  });

  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::seconds(2);
  while (dxmt9::CommandQueueArenaLeaseTestAccess::admissionWaiterCount(
             fixture.routing->queue_) == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool workerReachedPressureWait =
      dxmt9::CommandQueueArenaLeaseTestAccess::admissionWaiterCount(
          fixture.routing->queue_) == 1 &&
      fixture.routing->clearCalls == 0;

  ClearDesc firstClear{};
  fixture.routing->queue_.submitClear(firstClear);
  const bool activePublished = active->publish();
  worker.join();
  check(workerReachedPressureWait,
        "only the replay worker may block before Direct semantic replay");
  check(activePublished,
        "publishing the active lease must release Direct pressure");
  check(replayHr.load(std::memory_order_acquire) == D3D_OK &&
            fixture.routing->clearCalls == 1 &&
            dxmt9::CommandQueueArenaLeaseTestAccess::admissionWaiterCount(
                fixture.routing->queue_) == 0,
        "worker Direct replay must resume and apply semantics exactly once");

  const auto first = dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
      fixture.routing->queue_);
  const auto second = dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
      fixture.routing->queue_);
  check(first.reclaimed && second.reclaimed && first.seqId == 1 &&
            second.seqId == 2 && second.arena && second.clear,
        "pressure release preserves FIFO identity through reclaim");
}

void boundedCpuGrammarRequiresUnbindBeforeDestroy() {
  for (const bool includeUnbind : {false, true}) {
    const auto fixture = dxmt9::d3d9::test::makeCompletionTapeFixture(includeUnbind);
    const dxmt9::d3d9::RenderTapeBlobCatalogue catalogue{.blobs = {{
        .digest = fixture.blobDigest, .size = fixture.blob.size(), .verified = 1u}}};
    dxmt9::d3d9::ImportedRenderTapeView imported;
    check(dxmt9::d3d9::validateRenderTape(fixture.tape, catalogue, &imported).valid(),
          "missing-unbind negative must remain a structurally valid Tape");
    check(dxmt9::d3d9::preflightRenderTapeCpuGrammar(imported).accepted() == includeUnbind,
          "CPU grammar requires an effective unbind before destroy, before any device effects");
  }
}

void boundedCompletionReducerExhaustiveNegativeControls() {
  using Schedule = dxmt9::d3d9::RenderTapeCompletionSchedule;
  using Disposition = dxmt9::d3d9::RenderTapeCompletionDisposition;
  const auto key = [](std::uint64_t iteration, std::uint64_t submission,
                      std::uint32_t source, std::uint32_t ordinal) {
    return Schedule::Key{iteration, submission, source, ordinal};
  };

  // Enumerate both bounded release orders after admitting both submissions.
  // Only the FIFO order is accepted; the opposite order is rejected at the
  // same frontier and cannot call a completion callback.
  for (const bool releaseSecondFirst : {false, true}) {
    Schedule schedule;
    check(schedule.append({key(1, 1, 0, 0)}).accepted() &&
              schedule.append({key(1, 2, 1, 1)}).accepted() &&
              schedule.beginIteration(1).accepted(),
          "bounded schedule setup must be accepted");
    bool submittedA = false;
    bool submittedB = false;
    check(schedule.submit(key(1, 1, 0, 0), [&] {
            submittedA = true;
            return true;
          }).accepted() &&
              schedule.submit(key(1, 2, 1, 1), [&] {
                submittedB = true;
                return true;
              }).accepted() &&
              submittedA && submittedB,
          "bounded schedule submission callbacks must run after admission");
    if (releaseSecondFirst) {
      bool called = false;
      check(schedule.release(key(1, 2, 1, 1), [&] {
              called = true;
              return true;
            }).disposition == Disposition::SkippedFrontier && !called,
            "skipped completion frontier must reject before production effect");
    }
    check(schedule.release(key(1, 1, 0, 0)).accepted(),
          "first completion must advance the bounded frontier");
    check(schedule.release(key(1, 1, 0, 0)).disposition ==
              Disposition::DuplicateRelease,
          "duplicate completion must be rejected");
    check(schedule.release(key(1, 2, 1, 1)).accepted() &&
              schedule.finishIteration().accepted(),
          "FIFO completion must finish the iteration");
  }

  Schedule beforeSubmit;
  check(beforeSubmit.append({key(1, 1, 0, 0)}).accepted() &&
            beforeSubmit.beginIteration(1).accepted() &&
            beforeSubmit.release(key(1, 1, 0, 0)).disposition ==
                Disposition::ReleaseBeforeSubmit,
        "release before submit must be rejected");

  Schedule stale;
  check(stale.append({key(1, 1, 0, 0)}).accepted() &&
            stale.beginIteration(1).accepted() &&
            stale.submit(key(1, 1, 0, 0)).accepted() &&
            stale.release(key(2, 1, 0, 0)).disposition ==
                Disposition::Stale,
        "completion from another iteration must be stale");

  Schedule exhausted;
  check(exhausted.append({key(1, 1, 0, 0)}).accepted() &&
            exhausted.beginIteration(1).accepted() &&
            exhausted.submit(key(1, 1, 0, 0)).accepted() &&
            exhausted.release(key(1, 1, 0, 0)).accepted() &&
            exhausted.releaseNext().disposition ==
                Disposition::Exhausted,
        "an exhausted schedule must fail closed rather than implicitly drain");

  Schedule callbackFault;
  check(callbackFault.append({key(1, 1, 0, 0)}).accepted() &&
            callbackFault.beginIteration(1).accepted() &&
            callbackFault.submit(key(1, 1, 0, 0), [] { return false; }).disposition ==
                Disposition::Unresolved &&
            callbackFault.submit(key(1, 1, 0, 0)).disposition ==
                Disposition::Unresolved &&
            callbackFault.finishIteration().disposition ==
                Disposition::Unresolved,
        "failed admission callback must latch a terminal executed prefix");

  Schedule duplicateIdentity;
  check(duplicateIdentity.append({key(1, 1, 0, 0)}).accepted() &&
            duplicateIdentity.append({key(1, 1, 0, 1)}).disposition ==
                Disposition::DuplicateAction,
        "same iteration/submission/source cannot be represented twice");

  Schedule reversedSubmit;
  check(reversedSubmit.append({key(1, 1, 0, 0)}).accepted() &&
            reversedSubmit.append({key(1, 2, 1, 1)}).accepted() &&
            reversedSubmit.beginIteration(1).accepted() &&
            reversedSubmit.submit(key(1, 2, 1, 1)).disposition ==
                Disposition::SkippedFrontier,
        "submission order must not bypass the schedule frontier");

  Schedule repeatedIteration;
  check(repeatedIteration.append({key(1, 1, 0, 0)}).accepted() &&
            repeatedIteration.beginIteration(1).accepted() &&
            repeatedIteration.submit(key(1, 1, 0, 0)).accepted() &&
            repeatedIteration.release(key(1, 1, 0, 0)).accepted() &&
            repeatedIteration.finishIteration().accepted() &&
            repeatedIteration.beginIteration(1).disposition ==
                Disposition::Stale,
        "a completed iteration cannot be replayed under the same identity");

  Schedule twoIterations;
  check(twoIterations.append({key(1, 1, 0, 0)}).accepted() &&
            twoIterations.append({key(2, 2, 0, 1)}).accepted() &&
            twoIterations.beginIteration(1).accepted() &&
            twoIterations.submit(key(1, 1, 0, 0)).accepted() &&
            twoIterations.release(key(1, 1, 0, 0)).accepted() &&
            twoIterations.finishIteration().accepted() &&
            twoIterations.beginIteration(2).accepted() &&
            twoIterations.submit(key(2, 2, 0, 1)).accepted() &&
            twoIterations.release(key(2, 2, 0, 1)).accepted() &&
            twoIterations.finishIteration().accepted(),
        "iteration ranges must advance without accepting an old frontier");

  // Exhaustively explore every four-action trace over submit(A), submit(B),
  // release(A), and release(B).  The callback count is the binding check:
  // malformed reducer transitions must never invoke the production effect.
  for (std::uint32_t trace = 0u; trace < 256u; ++trace) {
    Schedule schedule;
    check(schedule.append({key(1, 1, 0, 0)}).accepted() &&
              schedule.append({key(1, 2, 1, 1)}).accepted() &&
              schedule.beginIteration(1).accepted(),
          "exhaustive reducer trace setup must be accepted");
    std::size_t acceptedSubmits = 0u;
    std::size_t acceptedReleases = 0u;
    for (std::size_t step = 0u; step < 4u; ++step) {
      const auto operation = static_cast<std::uint32_t>(
          (trace >> (step * 2u)) & 3u);
      bool callbackCalled = false;
      Schedule::Result result;
      if (operation == 0u || operation == 1u) {
        const auto submitKey = operation == 0u ? key(1, 1, 0, 0)
                                               : key(1, 2, 1, 1);
        result = schedule.submit(submitKey, [&] {
          callbackCalled = true;
          return true;
        });
        if (result.accepted()) ++acceptedSubmits;
      } else {
        const auto releaseKey = operation == 2u ? key(1, 1, 0, 0)
                                               : key(1, 2, 1, 1);
        result = schedule.release(releaseKey, [&] {
          callbackCalled = true;
          return true;
        });
        if (result.accepted()) ++acceptedReleases;
      }
      check(callbackCalled == result.accepted(),
            "every accepted bounded transition must invoke exactly its binding");
      check(acceptedSubmits <= 2u && acceptedReleases <= 2u,
            "bounded reducer trace must retain finite submission/release counts");
    }
    check(schedule.nextAction() <= schedule.size(),
          "bounded reducer frontier must never exceed its action list");
  }

  Schedule capacity;
  for (std::uint32_t i = 0;
       i < dxmt9::d3d9::kRenderTapeCompletionScheduleCapacity; ++i) {
    check(capacity
              .append({key(1, static_cast<std::uint64_t>(i) + 1u, i, i)})
              .accepted(),
          "bounded schedule must admit its declared capacity");
  }
  check(capacity.append({key(1, 33u, 32u, 32u)}).disposition ==
            Disposition::Capacity,
        "schedule capacity must be explicit and bounded");
}

void controlledCompletionBindsProductionQueueAndPool() {
  using Schedule = dxmt9::d3d9::RenderTapeCompletionSchedule;
  RuntimeFixture fixture;
  auto* buffer = dxmt9c_device_create_vertex_buffer(
      fixture.cDevice.get(), 256u, 0u, 0u, 0u);
  check(buffer != nullptr, "completion lifecycle buffer must be allocated");
  const auto bufferHandle = buffer->obj->handle();
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER, buffer);
  const std::array records{drawRecord(identity)};
  auto raw = makeRaw(makeWireFixture(records), 901u, false, &registry);
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK,
        "completion lifecycle buffer-bound draw must publish through production replay");
  const auto stampedSeq =
      dxmt9::CommandQueueArenaLeaseTestAccess::bufferLastUsedSeqId(
          fixture.routing->queue_, bufferHandle);
  check(stampedSeq != 0u,
        "production buffer-bound draw must stamp Pool last-use before acknowledgment");

  const auto expectedCompletion =
      dxmt9::CommandQueueArenaLeaseTestAccess::peekReadyCompletion(
          fixture.routing->queue_);
  check(expectedCompletion.completionIdentityValid(),
        "completion lifecycle source must be qualified before scheduling");
  Schedule schedule;
  const Schedule::Key completionKey{
      1u,
      expectedCompletion.seqId,
      expectedCompletion.source.id.index,
      0u,
      expectedCompletion.source.id.generation};
  check(schedule.append({completionKey}).accepted() &&
            schedule.beginIteration(1u).accepted(),
        "production completion schedule must initialize");
  dxmt9::CommandQueueArenaLeaseTestAccess::CompletionResult submission{};
  check(schedule.submit(completionKey, [&] {
          submission = dxmt9::CommandQueueArenaLeaseTestAccess::submitOneWithheld(
              fixture.routing->queue_);
          return submission.submitted;
        }).accepted() && submission.dequeued && submission.submitted &&
            dxmt9::CommandQueueArenaLeaseTestAccess::pendingCompletionCount(
                fixture.routing->queue_) == 1u,
        "schedule submission must bind to the real queue pending FIFO");
  check(submission.completion.seqId == completionKey.submission &&
            submission.completion.source.id.index == completionKey.source &&
            submission.completion.source.id.generation ==
                completionKey.sourceGeneration,
        "completion key must match the qualified production source identity");

  check(dxmt9::CommandQueueArenaLeaseTestAccess::destroyBufferForTest(
            fixture.routing->queue_, bufferHandle, 0u),
        "destroy-before-completion must enter the production retire path");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::bufferAlive(
            fixture.routing->queue_, bufferHandle),
        "in-flight stamped resource must survive destroy-before-completion");

  check(schedule.release(completionKey, [&] {
          return dxmt9::CommandQueueArenaLeaseTestAccess::acknowledgeOne(
              fixture.routing->queue_);
        }).accepted(),
        "schedule release must acknowledge the real pending completion");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::finishQueue(
            fixture.routing->queue_),
        "finish iteration must consume the production completion waterline");
  dxmt9::CommandQueueArenaLeaseTestAccess::reclaimPoolForTest(
      fixture.routing->queue_, stampedSeq);
  check(schedule.finishIteration().accepted(),
        "completion schedule must close only after production reclaim path");
  check(!dxmt9::CommandQueueArenaLeaseTestAccess::bufferAlive(
            fixture.routing->queue_, bufferHandle),
        "Pool::reclaimCompleted must retire the destroyed completed resource");
  const auto reused = fixture.routing->queue_.pool().createBuffer(
      WMT::Device{NULL_OBJECT_HANDLE}, BufferDesc{.size = 256u});
  check(static_cast<bool>(reused) && reused.value != bufferHandle.value,
        "completed retirement must permit generation-qualified backing reuse");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

void twoPendingProductionSourcesPreserveQualifiedFifoLifetime() {
  namespace rt = dxmt9::d3d9;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Policy = rt::ProviderReplayPolicy;
  using Source = dxmt9::core::metalqueue::QueueCompletionSource;
  const auto qualified = [](const Source& source) {
    return rt::RenderTapeQualifiedCompletionKey{
        .seqId = source.seqId,
        .sourceIndex = source.source.id.index,
        .sourceGeneration = source.source.id.generation,
        .storageFirstPage = source.source.storage.firstPage,
        .storagePageCount = source.source.storage.pageCount,
        .storageGeneration = source.source.storage.generation};
  };

  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE,
        "two-pending source fixture requires a Metal device");
  for (const auto route : {Policy::Compatibility, Policy::ProductionSlot}) {
    RuntimeFixture fixture(false, false, true, false,
                           WMT::Device{resourceDevice.handle});
    auto& queue = fixture.routing->queue_;
    struct BufferOwner {
      D9CBuffer* value = nullptr;
      ~BufferOwner() { release(); }
      void release() {
        if (value) dxmt9c_buffer_release(std::exchange(value, nullptr));
      }
    };
    BufferOwner bufferA{dxmt9c_device_create_vertex_buffer(
        fixture.cDevice.get(), 256u, 0x200u, 0u, 0u)};
    BufferOwner bufferB{dxmt9c_device_create_vertex_buffer(
        fixture.cDevice.get(), 256u, 0x200u, 0u, 0u)};
    check(bufferA.value && bufferB.value, "source buffers construct");
    const auto handleA = bufferA.value->obj->handle();
    const auto handleB = bufferB.value->obj->handle();
    const D9CWireObjectIdentity identityA{
        .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 1u, .objectId = 901u};
    const D9CWireObjectIdentity identityB{
        .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 1u, .objectId = 902u};
    const auto wireA = makeWireFixture(std::array{drawRecord(identityA)});
    const auto wireB = makeWireFixture(std::array{drawRecord(identityB)});
    std::array<std::uint8_t, 256> bytesA{};
    std::array<std::uint8_t, 256> bytesB{};
    bytesA.fill(0xa1u);
    bytesB.fill(0xb2u);
    const auto seed = [&](D9CBuffer* buffer, const auto& bytes) {
      void* bits = nullptr;
      check(dxmt9c_buffer_lock(buffer, 0u, bytes.size(), &bits, 0u) == D3D_OK && bits,
            "source seed enters actual buffer lock");
      std::memcpy(bits, bytes.data(), bytes.size());
      check(dxmt9c_buffer_unlock(buffer) == D3D_OK,
            "source seed enters actual unlock/upload");
    };
    seed(bufferA.value, bytesA);
    seed(bufferB.value, bytesB);
    const auto backingMatches = [&](Handle handle, const auto& bytes) {
      // Worker-disabled fixture: no mutation or acknowledgment races this
      // read. Inspect actual Metal CPU-visible bytes, not only Pool shadow.
      const auto* record = queue.pool().findBuffer(handle.value);
      return record && record->buffer && record->contents &&
          record->desc.size == bytes.size() &&
          std::memcmp(record->contents, bytes.data(), bytes.size()) == 0;
    };
    check(backingMatches(handleA, bytesA) && backingMatches(handleB, bytesB),
          "both resource owners have actual seeded backing bytes");

    // Native component input, not an on-disk Tape acceptance claim. Bind
    // the existing plan controller to these immutable two wire chunks.
    std::vector<std::byte> input = wireA.bytes;
    input.insert(input.end(), wireB.bytes.begin(), wireB.bytes.end());
    rt::RenderTapeCompletionPlanFile plan;
    plan.header.byteSize = sizeof(plan);
    plan.header.tapeEventCount = 4u;
    plan.header.submitCount = plan.header.releaseCount = 2u;
    plan.header.repeatCount = 1u;
    plan.header.eventsDigest = rt::RenderTapeCaptureSession::sha256(input);
    plan.submits[0] = {1u, 1u};
    plan.submits[1] = {2u, 1u};
    plan.releases[0] = {3u, 1u, 1u};
    plan.releases[1] = {4u, 2u, 1u};
    rt::RenderTapeCompletionPlanController controller(plan, plan.header.eventsDigest);
    const auto planRoute = route == Policy::Compatibility
        ? rt::RenderTapeCompletionPlanRoute::Compatibility
        : rt::RenderTapeCompletionPlanRoute::ProductionSlot;
    check(controller.beginIteration(1u, planRoute).accepted(),
          "two-source controller begins before actual queue effects");

    const auto publishAndSubmit = [&](const WireFixture& wire, D9CBuffer* buffer,
                                      std::uint32_t event) {
      check(Access::pendingCompletionCount(queue) == event - 1u,
            "B must enter replay while A is already submitted and pending");
      check(Access::noWaitHeadroom(queue).admitsOne(),
            "fresh one-source witness precedes each actual replay");
      const std::array<void*, 1> objects{buffer};
      check(rt::replayPrevalidatedResolvedCommandChunk(
                fixture.cDevice.get(), wire.bytes, wire.envelope, objects,
                route) == D3D_OK,
            "production replay constructs the draw source");
      check(Access::publishWriting(queue) && Access::readyCount(queue) == 1u,
            "one nonempty draw-only source is published");
      const auto source = Access::peekReadyCompletion(queue);
      check(source.completionIdentityValid(), "actual completion identity exists");
      const auto key = qualified(source);
      check(controller.submit(event, 1u, key, [&] {
              const auto submitted = Access::submitOneWithheld(queue);
              return submitted.submitted && submitted.commandCount != 0u &&
                  qualified(submitted.completion) == key &&
                  std::all_of(submitted.commandKinds.begin(),
                              submitted.commandKinds.end(), [](auto kind) {
                    return kind == static_cast<std::uint8_t>(MetalCommandKind::DrawRun);
                  });
            }).accepted() && controller.endEvent(event).accepted(),
            "actual sequence/source/storage bind before withheld submission");
      return source;
    };
    const auto sourceA = publishAndSubmit(wireA, bufferA.value, 1u);
    const auto usedA = Access::bufferLifetime(queue, handleA);
    check(usedA.alive && usedA.lastUsedSeq == sourceA.seqId &&
              usedA.completedSeq < usedA.lastUsedSeq,
          "A publication applies the actual resource-use stamp");
    const auto sourceB = publishAndSubmit(wireB, bufferB.value, 2u);
    check(fixture.routing->ordinaryDrawCalls ==
              (route == Policy::Compatibility ? 2u : 0u),
          "route qualification cannot silently fall back from Direct to compatibility");
    check(sourceA.source.id != sourceB.source.id &&
              sourceA.source.storage != sourceB.source.storage &&
              sourceA.seqId < sourceB.seqId &&
              Access::pendingCompletionCount(queue) == 2u &&
              Access::residentCount(queue) == 2u,
          "A and B are distinct concurrently pending sources");

    check(dxmt9c_device_set_stream_source(
              fixture.cDevice.get(), 0u, nullptr, 0u, 0u) == D3D_OK,
          "release core binding before wrapper ownership retirement");
    bufferA.release();
    bufferB.release();
    const auto retiredA = Access::bufferLifetime(queue, handleA);
    const auto retiredB = Access::bufferLifetime(queue, handleB);
    check(retiredA.alive && retiredA.destroyPending &&
              retiredA.lastUsedSeq == sourceA.seqId &&
              retiredA.completedSeq < retiredA.lastUsedSeq &&
              retiredB.alive && retiredB.destroyPending &&
              retiredB.lastUsedSeq == sourceB.seqId &&
              retiredB.completedSeq < retiredB.lastUsedSeq &&
              backingMatches(handleA, bytesA) && backingMatches(handleB, bytesB),
          "wrapper releases retire, but cannot recycle, either pending backing");

    auto skipped = controller;
    bool skippedCalled = false;
    check(!skipped.releaseDue(4u, [&](const auto&) {
              skippedCalled = true;
              return true;
            }).accepted() && !skippedCalled &&
              Access::pendingCompletionCount(queue) == 2u,
          "B-first release boundary rejects before any acknowledgment");

    const auto release = [&](std::uint32_t event, const Source& expected) {
      std::size_t callbacks = 0u;
      check(controller.releaseDue(event, [&](const auto& actual) {
              ++callbacks;
              return actual == qualified(expected) &&
                  Access::acknowledgeOne(queue) && Access::finishQueue(queue);
            }).accepted() && callbacks == 1u &&
                controller.endEvent(event).accepted(),
            "one exact FIFO source completes at its explicit release event");
      Access::reclaimCompletedPool(queue);
    };
    release(3u, sourceA);
    const auto remainingB = Access::bufferLifetime(queue, handleB);
    check(!Access::bufferAlive(queue, handleA) && remainingB.alive &&
              remainingB.destroyPending && remainingB.lastUsedSeq == sourceB.seqId &&
              remainingB.completedSeq == sourceA.seqId &&
              backingMatches(handleB, bytesB) &&
              Access::pendingCompletionCount(queue) == 1u &&
              Access::residentCount(queue) == 1u,
          "A reclaim leaves B's exact owner, stamp and physical bytes intact");
    std::size_t duplicateCallbacks = 0u;
    check(controller.releaseDue(3u, [&](const auto&) {
              ++duplicateCallbacks;
              return true;
            }).accepted() && duplicateCallbacks == 0u &&
              Access::pendingCompletionCount(queue) == 1u,
          "repeated release boundary cannot acknowledge B as duplicate A");
    release(4u, sourceB);
    check(controller.finishIteration().accepted() &&
              Access::pendingCompletionCount(queue) == 0u &&
              Access::residentCount(queue) == 0u &&
              !Access::bufferAlive(queue, handleA) &&
              !Access::bufferAlive(queue, handleB),
          "B completion closes both source and wrapper-retirement obligations");
  }
}

void pendingSourceCapacityRejectsBeforeWorkAndReopensAfterReclaim() {
  namespace rt = dxmt9::d3d9;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Policy = rt::ProviderReplayPolicy;
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE, "capacity fixture Metal device");
  for (const auto route : {Policy::Compatibility, Policy::ProductionSlot}) {
    RuntimeFixture fixture(false, false, true, false,
                           WMT::Device{resourceDevice.handle});
    auto& queue = fixture.routing->queue_;
    auto* buffer = dxmt9c_device_create_vertex_buffer(
        fixture.cDevice.get(), 256u, 0x200u, 0u, 0u);
    check(buffer, "capacity fixture buffer");
    struct Owner {
      D9CBuffer* value;
      ~Owner() { dxmt9c_buffer_release(value); }
    } owner{buffer};
    const auto wire = makeWireFixture(std::array{drawRecord({
        .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 1u, .objectId = 903u})});
    const std::array<void*, 1> objects{buffer};
    std::uint64_t lastSequence = 0u;
    std::size_t submitted = 0u;
    const auto append = [&] {
      check(Access::noWaitHeadroom(queue).admitsOne(),
            "each capacity-test append requires a fresh live witness");
      check(rt::replayPrevalidatedResolvedCommandChunk(
                fixture.cDevice.get(), wire.bytes, wire.envelope, objects, route) == D3D_OK &&
                Access::publishWriting(queue), "capacity-test production append");
      const auto source = Access::submitOneWithheld(queue);
      check(source.submitted && source.commandCount != 0u &&
                source.completion.completionIdentityValid() &&
                source.completion.seqId > lastSequence,
            "capacity fill uses actual monotonic nonempty submissions");
      lastSequence = source.completion.seqId;
      ++submitted;
    };
    // A fixed test bound, never a new queue capacity. No acknowledgment or
    // synthetic free credit is allowed while discovering real pressure.
    while (submitted < 64u && Access::noWaitHeadroom(queue).admitsOne()) append();
    check(submitted >= 2u && submitted < 64u &&
              Access::pendingCompletionCount(queue) == submitted &&
              Access::residentCount(queue) == submitted,
          "real queue reaches bounded pressure with every source still pending");
    const auto denied = Access::noWaitHeadroom(queue);
    check(!denied.admitsOne() &&
              !Access::noWaitHeadroom(queue).admitsOne() &&
              Access::readyCount(queue) == 0u &&
              Access::writingCommandCount(queue) == 0u &&
              Access::pendingCompletionCount(queue) == submitted &&
              Access::residentCount(queue) == submitted,
          "denied probes have no work, hidden completion or capacity side effects");
    check(Access::acknowledgeOne(queue) && Access::finishQueue(queue),
          "one real FIFO completion reclaims capacity");
    Access::reclaimCompletedPool(queue);
    check(Access::noWaitHeadroom(queue).admitsOne(),
          "actual reclaim reopens one source admission");
    append();
    check(!Access::noWaitHeadroom(queue).admitsOne(),
          "reopened credit is consumed once, not reusable aggregate capacity");
    for (std::size_t remaining = submitted - 1u; remaining != 0u; --remaining) {
      check(Access::acknowledgeOne(queue) && Access::finishQueue(queue),
            "capacity fixture drains only through real completion");
    }
    Access::reclaimCompletedPool(queue);
    check(Access::pendingCompletionCount(queue) == 0u &&
              Access::residentCount(queue) == 0u &&
              Access::noWaitHeadroom(queue).admitsOne(),
          "full capacity conserved after drain");
    check(fixture.routing->ordinaryDrawCalls ==
              (route == Policy::Compatibility ? submitted : 0u),
          "capacity fill executes the selected replay route");
    std::cout << "CPU source capacity route="
              << (route == Policy::Compatibility ? "compatibility" : "production-slot")
              << " pending_limit=" << submitted - 1u
              << " denial=" << static_cast<unsigned>(dxmt9::test::admitCpuTapeSource(denied.inputs))
              << " reclaimed=" << submitted << '\n';
  }
}

enum class CpuTapeResourceBackend : std::uint8_t {
  NoGpu,
  MetalResources,
};

void cpuMipmapObserverPreservesOrderAndQualifiedTexture() {
  const auto observe = [](const std::array<std::uint64_t, 2>& handles,
                           bool resolve, std::uint32_t generation) {
    dxmt9::core::ChunkSlot slot;
    for (const auto handle : handles)
      slot.appendGenerateMipmaps(GenerateMipmapsDesc{.texture = {handle}});
    std::uint64_t digest = dxmt9::test::detail::kCpuTapeFnvOffset;
    std::size_t observed = 0u;
    std::string failure;
    const auto resource = [&](std::uint32_t kind, Handle handle,
                               std::uint64_t& hash) {
      if (!resolve || kind != D9C_CHUNK_HANDLE_KIND_TEXTURE ||
          (handle.value != 7u && handle.value != 8u)) return false;
      dxmt9::test::detail::cpuTapeDigestScalar(hash, kind);
      dxmt9::test::detail::cpuTapeDigestScalar(hash, generation);
      dxmt9::test::detail::cpuTapeDigestScalar(hash, handle.value);
      return true;
    };
    const auto draw = [](std::uint64_t) { return false; };
    const bool ok = dxmt9::test::observeCpuTapePayload(
        dxmt9::core::SourcePayloadView(slot), digest, resource, draw, failure,
        nullptr, nullptr, nullptr, nullptr, &observed);
    return std::tuple{ok, digest, observed};
  };
  const auto [ok, ab, count] = observe({7u, 8u}, true, 1u);
  check(ok && count == 2u, "mipmap observer counts actual operations, not draws");
  const auto [reverseOk, ba, reverseCount] = observe({8u, 7u}, true, 1u);
  check(reverseOk && reverseCount == 2u && ab != ba,
        "mipmap resource order participates in the semantic oracle");
  check(std::get<1>(observe({7u, 8u}, true, 2u)) != ab,
        "resource generation participates in the logical mip operation");
  check(!std::get<0>(observe({0u, 8u}, true, 1u)) &&
            !std::get<0>(observe({9u, 8u}, true, 1u)) &&
            !std::get<0>(observe({7u, 8u}, false, 1u)),
        "null, foreign and unresolved mip textures fail closed");
}

void resourceBackedRoutingDeviceUsesPoolOwnership() {
  // This component fixture pins the real Pool ownership/reclaim predicate.
  // It supplies a test watermark explicitly; it is not a Tape-driven texture
  // mutation test or proof that a GPU sampled the texture before retirement.
  MetalDeviceLease resourceDeviceLease;
  resourceDeviceLease.handle = dxmt9_test_acquire_metal_device();
  check(resourceDeviceLease.handle != NULL_OBJECT_HANDLE,
        "resource-backed routing fixture requires a Metal device");
  const WMT::Device resourceDevice{resourceDeviceLease.handle};
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/false,
                         /*cpuReadyArena=*/true, resourceDevice);

  auto texture = fixture.cDevice->dev().createTexture(TextureDesc{
      .width = 8u,
      .height = 8u,
      .depth = 1u,
      .levels = 1u,
      .format = Format::A8R8G8B8,
      .type = TextureType::TwoD,
      .pool = Pool::Default,
  });
  check(texture && texture->handle(),
        "resource-backed core texture must receive a Pool handle");
  const auto textureHandle = texture->handle();
  auto alias = texture->surfaceLevel(0u);
  check(alias && alias->handle(),
        "resource-backed texture alias must receive a Pool surface handle");
  const auto aliasHandle = alias->handle();
  auto standalone = fixture.cDevice->dev().createSurface(SurfaceDesc{
      .width = 8u,
      .height = 8u,
      .format = Format::A8R8G8B8,
      .pool = Pool::Default,
      .usage = UsageRenderTarget,
      .renderTarget = true,
  });
  check(standalone && standalone->handle(),
        "resource-backed standalone surface must receive a Pool handle");
  const auto standaloneHandle = standalone->handle();

  const auto* textureRecord = fixture.routing->queue_.pool().findTexture(
      textureHandle.value);
  const auto* aliasRecord = fixture.routing->queue_.pool().findSurface(
      aliasHandle.value);
  const auto* standaloneRecord = fixture.routing->queue_.pool().findSurface(
      standaloneHandle.value);
  check(textureRecord && textureRecord->texture &&
            aliasRecord && aliasRecord->texture &&
            aliasRecord->aliasTexture == textureHandle &&
            standaloneRecord && standaloneRecord->texture &&
            standaloneHandle.value != aliasHandle.value,
        "resource-backed wrappers must refer to real Pool texture/surface records");

  const auto retainedSeq =
      dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
          fixture.routing->queue_);
  check(retainedSeq != 0u,
        "resource fixture must expose a non-zero sequence watermark");
  fixture.routing->queue_.pool().markTextureUse(textureHandle, retainedSeq);
  fixture.routing->queue_.pool().markSurfaceUse(aliasHandle, retainedSeq);
  fixture.routing->queue_.pool().markSurfaceUse(standaloneHandle, retainedSeq);
  alias.reset();
  texture.reset();
  standalone.reset();
  check(fixture.routing->queue_.pool().findTexture(textureHandle.value) &&
            fixture.routing->queue_.pool().findSurface(aliasHandle.value) &&
            fixture.routing->queue_.pool().findSurface(standaloneHandle.value),
        "in-flight Pool records must survive wrapper release before completion");
  dxmt9::CommandQueueArenaLeaseTestAccess::reclaimPoolForTest(
      fixture.routing->queue_, retainedSeq - 1u);
  check(fixture.routing->queue_.pool().findTexture(textureHandle.value) &&
            fixture.routing->queue_.pool().findSurface(aliasHandle.value) &&
            fixture.routing->queue_.pool().findSurface(standaloneHandle.value),
        "Pool reclaim must not cross an incomplete waterline");
  dxmt9::CommandQueueArenaLeaseTestAccess::reclaimPoolForTest(
      fixture.routing->queue_, retainedSeq);
  check(!fixture.routing->queue_.pool().findTexture(textureHandle.value) &&
            !fixture.routing->queue_.pool().findSurface(aliasHandle.value) &&
            !fixture.routing->queue_.pool().findSurface(standaloneHandle.value),
        "completed waterline must reclaim retired texture and surface records");
}

void textureUploadObservationTracksPlainABAndReadOnlyLock() {
  MetalDeviceLease resourceDeviceLease;
  resourceDeviceLease.handle = dxmt9_test_acquire_metal_device();
  check(resourceDeviceLease.handle != NULL_OBJECT_HANDLE,
        "texture upload fixture requires a Metal device");
  const WMT::Device resourceDevice{resourceDeviceLease.handle};
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/false,
                         /*cpuReadyArena=*/true, resourceDevice);
  auto texture = fixture.cDevice->dev().createTexture(TextureDesc{
      .width = 4u,
      .height = 4u,
      .depth = 1u,
      .levels = 1u,
      .format = Format::A8R8G8B8,
      .type = TextureType::TwoD,
      .pool = Pool::Default,
  });
  check(texture && texture->handle(),
        "texture upload fixture must create a real Pool texture");

  struct QueueState {
    std::uint64_t nextSeq = 0u;
    std::size_t writing = 0u;
    std::size_t ready = 0u;
    std::size_t resident = 0u;
    std::size_t pending = 0u;
  };
  const auto queueState = [&]() {
    return QueueState{
        .nextSeq = dxmt9::CommandQueueArenaLeaseTestAccess::nextSeqId(
            fixture.routing->queue_),
        .writing = dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
            fixture.routing->queue_),
        .ready = dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
            fixture.routing->queue_),
        .resident = dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
            fixture.routing->queue_),
        .pending = dxmt9::CommandQueueArenaLeaseTestAccess::pendingCompletionCount(
            fixture.routing->queue_),
    };
  };
  const auto unchanged = [](const QueueState& before, const QueueState& after) {
    return before.nextSeq == after.nextSeq && before.writing == after.writing &&
        before.ready == after.ready && before.resident == after.resident &&
        before.pending == after.pending;
  };

  const std::vector<std::uint8_t> invalidBytes(4u * 4u * 4u, 0x11u);
  const auto expectRejectedUpload = [&](TextureHandle handle,
                                        std::uint32_t level,
                                        std::uint32_t width,
                                        std::uint32_t height,
                                        std::uint32_t depth,
                                        std::uint32_t pitch,
                                        std::uint32_t slicePitch,
                                        std::span<const std::uint8_t> bytes,
                                        std::string_view reason) {
    const auto before = queueState();
    const auto count = fixture.routing->textureUploadObservations.size();
    fixture.routing->uploadTextureLevel(
        handle, level, width, height, depth, pitch, slicePitch, bytes);
    check(fixture.routing->textureUploadFailed &&
              fixture.routing->textureUploadObservations.size() == count &&
              unchanged(before, queueState()), reason);
    fixture.routing->textureUploadFailed = false;
  };
  expectRejectedUpload(TextureHandle{0xdeadbeefu}, 0u, 4u, 4u, 1u, 16u,
                        64u, invalidBytes,
                        "invalid texture handle must not record an upload");
  expectRejectedUpload(texture->handle(), 1u, 4u, 4u, 1u, 16u, 64u,
                        invalidBytes,
                        "invalid texture subresource must not record an upload");
  expectRejectedUpload(texture->handle(), 0u, 8u, 4u, 1u, 16u, 64u,
                        invalidBytes,
                        "mismatched upload extent must not record an upload");
  expectRejectedUpload(texture->handle(), 0u, 4u, 4u, 1u, 0u, 0u, {},
                        "zero upload geometry must not record an upload");

  const auto beforeA = queueState();
  const std::vector<std::uint8_t> bytesA(4u * 4u * 4u, 0x11u);
  auto locked = texture->lockRect(0u, nullptr, 0u);
  check(locked.data && locked.pitch == 4u * 4u,
        "plain texture lock must expose the expected CPU layout");
  std::memcpy(locked.data, bytesA.data(), bytesA.size());
  texture->unlockRect(0u);
  const auto afterA = queueState();
  check(unchanged(beforeA, afterA) &&
            fixture.routing->textureUploadObservations.size() == 1u &&
            !fixture.routing->textureUploadFailed,
        "plain A upload must stay outside Writing/Ready/pending queue state");
  const auto& uploadA = fixture.routing->textureUploadObservations.back();
  check(uploadA.handle == texture->handle() && uploadA.level == 0u &&
            uploadA.width == 4u && uploadA.height == 4u && uploadA.depth == 1u &&
            uploadA.pitch == 16u && uploadA.slicePitch == bytesA.size() &&
            uploadA.bytes == bytesA &&
            uploadA.contentDigest == digestByteSpan(bytesA),
        "plain A upload must record exact handle, extent, layout, and bytes");

  const std::vector<std::uint8_t> bytesB(4u * 4u * 4u, 0x7bu);
  locked = texture->lockRect(0u, nullptr, 0u);
  check(locked.data && locked.pitch == 16u,
        "plain B texture lock must expose the same CPU layout");
  std::memcpy(locked.data, bytesB.data(), bytesB.size());
  texture->unlockRect(0u);
  const auto afterB = queueState();
  check(unchanged(afterA, afterB) &&
            fixture.routing->textureUploadObservations.size() == 2u,
        "plain B upload must not publish a queue source or completion");
  const auto& uploadB = fixture.routing->textureUploadObservations.back();
  check(uploadB.handle == texture->handle() && uploadB.level == 0u &&
            uploadB.bytes == bytesB &&
            uploadB.contentDigest == digestByteSpan(bytesB),
        "plain B upload must replace the observed logical content exactly");

  const auto currentBytes = texture->levelBytes(0u);
  const auto beforeReadOnly = std::vector<std::uint8_t>(
      currentBytes.begin(), currentBytes.end());
  locked = texture->lockRect(0u, nullptr, UsageReadOnly);
  check(locked.data &&
            std::memcmp(locked.data, beforeReadOnly.data(), beforeReadOnly.size()) ==
                0,
        "read-only lock must expose unchanged B content");
  texture->unlockRect(0u);
  const auto afterReadOnly = queueState();
  check(unchanged(afterB, afterReadOnly) &&
            fixture.routing->textureUploadObservations.size() == 3u &&
            fixture.routing->textureUploadObservations.back().bytes ==
                beforeReadOnly &&
            !fixture.routing->textureUploadFailed,
        "read-only unlock must preserve logical bytes and queue state");

  fixture.routing->textureUploadFailed = false;
  const auto beforeCapacity = queueState();
  const auto remaining = RoutingDevice::kTextureUploadObservationCapacity -
      fixture.routing->textureUploadObservations.size();
  for (std::size_t attempt = 0u; attempt < remaining; ++attempt) {
    fixture.routing->uploadTextureLevel(texture->handle(), 0u, 4u, 4u, 1u,
                                        16u, 64u, bytesB);
  }
  check(fixture.routing->textureUploadObservations.size() ==
            RoutingDevice::kTextureUploadObservationCapacity &&
            !fixture.routing->textureUploadFailed &&
            unchanged(beforeCapacity, queueState()),
        "upload observation budget must fill without changing queue state");
  const auto beforeExhausted = queueState();
  const auto countAtCapacity =
      fixture.routing->textureUploadObservations.size();
  fixture.routing->uploadTextureLevel(texture->handle(), 0u, 4u, 4u, 1u,
                                      16u, 64u, bytesB);
  check(fixture.routing->textureUploadFailed &&
            fixture.routing->textureUploadObservations.size() == countAtCapacity &&
            unchanged(beforeExhausted, queueState()),
        "exhausted upload observation budget must fail without effects");
}

struct CpuTapeExperimentOptions {
  enum class SourceBoundaryMode : std::uint8_t {
    Chunk,
    CaptureSource,
  };
  std::string tapePath;
  bool generalFrame = false;
  std::string inputAdmissionProfile = "default";
  bool measurement = false;
  bool diagnostic = false;
  bool policyExplicit = false;
  dxmt9::test::CpuTapePolicy policy{};
  std::vector<std::string> blobPaths;
  std::string sourceOraclePath;
  std::string sourceProjectionSha256;
  SourceBoundaryMode sourceBoundaryMode = SourceBoundaryMode::Chunk;
  std::string identityPath;
  std::string sourcePlanSha256;
  CpuTapeResourceBackend resourceBackend = CpuTapeResourceBackend::NoGpu;
  bool compatibility = false;
  bool afterDestroy = false;
  bool beforeNextWork = false;
  std::size_t repeat = 1u;
  bool repeatExplicit = false;
  std::string fault = "none";
  std::size_t maxPending = dxmt9::d3d9::kRenderTapeCompletionScheduleCapacity;
  std::string completionPlanPath;
  dxmt9::d3d9::RenderTapeCpuPublicationMode publication =
      dxmt9::d3d9::RenderTapeCpuPublicationMode::PerCommand;
};

struct CpuTapeTraceRow {
  std::uint64_t iteration = 0;
  std::uint32_t event = 0;
  std::uint32_t eventSubmission = 0;
  std::uint64_t submission = 0;
  std::uint32_t source = 0;
  std::uint64_t sourceGeneration = 0;
  std::uint32_t storageFirstPage = 0;
  std::uint32_t storagePageCount = 0;
  std::uint64_t storageGeneration = 0;
  std::uint32_t firstEvent = 0;
  std::uint32_t lastEvent = 0;
  std::uint32_t releasedAtEvent = 0;
  bool submitted = false;
  bool completed = false;
};

struct CpuTapeDrawObservation {
  std::uint32_t ordinal = 0u;
  // The event that published this physical source, not necessarily the
  // original event containing the draw (draw-island may span events).
  std::uint32_t publicationEvent = 0u;
  std::uint64_t cumulativeSemanticDigest = 0u;
};

enum class CpuTapePhase : std::uint8_t {
  Setup,
  Replay,
  Observe,
  Completion,
  Cleanup,
};

constexpr std::size_t cpuTapePhaseIndex(CpuTapePhase phase) noexcept {
  return static_cast<std::size_t>(phase);
}

// Cold-path wall ledger for the bounded CPU tape experiment. Nested scopes
// pause the active phase and resume it at the same monotonic instant, so
// semantic observation and controlled completion do not double-count their
// enclosing replay/event time. The Clock template is only for the native
// fake-clock proof below; production uses steady_clock directly.
template <typename Clock>
class CpuTapePhaseLedger final {
 public:
  using Duration = std::chrono::nanoseconds;
  using TimePoint = typename Clock::time_point;

  class Scope final {
   public:
    Scope(CpuTapePhaseLedger& ledger, CpuTapePhase phase) noexcept
        : ledger_(&ledger), hadPrevious_(ledger.active_),
          previousPhase_(ledger.activePhase_) {
      ledger_->enterNested(phase);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&& other) noexcept
        : ledger_(other.ledger_), hadPrevious_(other.hadPrevious_),
          previousPhase_(other.previousPhase_) {
      other.ledger_ = nullptr;
    }
    Scope& operator=(Scope&&) = delete;
    ~Scope() { close(); }

    void close() noexcept {
      if (!ledger_) return;
      ledger_->leaveNested(hadPrevious_, previousPhase_);
      ledger_ = nullptr;
    }

   private:
    CpuTapePhaseLedger* ledger_ = nullptr;
    bool hadPrevious_ = false;
    CpuTapePhase previousPhase_ = CpuTapePhase::Setup;
  };

  void begin(CpuTapePhase phase) noexcept {
    if (!started_) {
      started_ = true;
      firstStart_ = Clock::now();
    }
    const auto now = Clock::now();
    pauseActive(now);
    active_ = true;
    activePhase_ = phase;
    activeStart_ = now;
  }

  Scope scoped(CpuTapePhase phase) noexcept { return Scope(*this, phase); }

  void finish() noexcept {
    const auto now = Clock::now();
    pauseActive(now);
    active_ = false;
    if (started_) {
      elapsedNs_ = toNs(
          std::chrono::duration_cast<Duration>(now - firstStart_));
    }
  }

  std::uint64_t elapsedNs() const noexcept { return elapsedNs_; }
  const std::array<std::uint64_t, 5>& phaseNs() const noexcept {
    return phaseNs_;
  }

 private:
  static std::uint64_t toNs(Duration duration) noexcept {
    return static_cast<std::uint64_t>(duration.count());
  }

  void pauseActive(TimePoint now) noexcept {
    if (!active_) return;
    phaseNs_[cpuTapePhaseIndex(activePhase_)] +=
        toNs(std::chrono::duration_cast<Duration>(now - activeStart_));
  }

  void pauseActive() noexcept { pauseActive(Clock::now()); }

  void enterNested(CpuTapePhase phase) noexcept {
    const auto now = Clock::now();
    pauseActive(now);
    active_ = true;
    activePhase_ = phase;
    activeStart_ = now;
  }

  void leaveNested(bool hadPrevious, CpuTapePhase previousPhase) noexcept {
    const auto now = Clock::now();
    pauseActive(now);
    if (hadPrevious) {
      active_ = true;
      activePhase_ = previousPhase;
      activeStart_ = now;
    } else {
      active_ = false;
    }
  }

  bool started_ = false;
  bool active_ = false;
  CpuTapePhase activePhase_ = CpuTapePhase::Setup;
  TimePoint firstStart_{};
  TimePoint activeStart_{};
  std::array<std::uint64_t, 5> phaseNs_{};
  std::uint64_t elapsedNs_ = 0u;
};

struct CpuTapePhaseFakeClock {
  using time_point = std::chrono::steady_clock::time_point;
  static inline time_point value{};

  static time_point now() noexcept { return value; }
  static void advance(std::uint64_t nanoseconds) noexcept {
    value += std::chrono::nanoseconds(nanoseconds);
  }
};

std::optional<std::uint64_t> cpuProcessTimespecNs(std::int64_t seconds,
                                                  std::int64_t nanoseconds) noexcept {
  if (seconds < 0 || nanoseconds < 0 || nanoseconds >= 1000000000ll)
    return std::nullopt;
  constexpr auto kScale = std::uint64_t{1000000000};
  const auto secondsValue = static_cast<std::uint64_t>(seconds);
  const auto nanosValue = static_cast<std::uint64_t>(nanoseconds);
  if (secondsValue > (std::numeric_limits<std::uint64_t>::max() - nanosValue) /
                         kScale)
    return std::nullopt;
  return secondsValue * kScale + nanosValue;
}

std::optional<std::uint64_t> sampleProcessCpuNs() noexcept {
#if defined(CLOCK_PROCESS_CPUTIME_ID)
  timespec value{};
  if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0)
    return std::nullopt;
  return cpuProcessTimespecNs(static_cast<std::int64_t>(value.tv_sec),
                              static_cast<std::int64_t>(value.tv_nsec));
#else
  return std::nullopt;
#endif
}

dxmt9::test::CpuDiagnosticSample sampleCpuDiagnosticClocks() noexcept {
  const auto wall = static_cast<std::uint64_t>(std::chrono::duration_cast<
      std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  const auto cpu = sampleProcessCpuNs();
  return {wall, cpu.value_or(0u), cpu.has_value()};
}

void cpuTapePhaseLedgerNestedScopeSpec() {
  using Ledger = CpuTapePhaseLedger<CpuTapePhaseFakeClock>;
  CpuTapePhaseFakeClock::value = {};
  Ledger ledger;
  ledger.begin(CpuTapePhase::Setup);
  CpuTapePhaseFakeClock::advance(5u);
  try {
    auto replay = ledger.scoped(CpuTapePhase::Replay);
    CpuTapePhaseFakeClock::advance(7u);
    {
      auto observe = ledger.scoped(CpuTapePhase::Observe);
      CpuTapePhaseFakeClock::advance(3u);
      throw TestFailure("phase-ledger-unwind-probe");
    }
  } catch (const TestFailure&) {
    // Scope destruction must restore replay, then the outer setup scope.
  }
  ledger.begin(CpuTapePhase::Cleanup);
  CpuTapePhaseFakeClock::advance(4u);
  ledger.finish();
  const auto& phases = ledger.phaseNs();
  check(phases[cpuTapePhaseIndex(CpuTapePhase::Setup)] == 5u &&
            phases[cpuTapePhaseIndex(CpuTapePhase::Replay)] == 7u &&
            phases[cpuTapePhaseIndex(CpuTapePhase::Observe)] == 3u &&
            phases[cpuTapePhaseIndex(CpuTapePhase::Completion)] == 0u &&
            phases[cpuTapePhaseIndex(CpuTapePhase::Cleanup)] == 4u &&
            ledger.elapsedNs() == 19u,
        "nested CPU tape phase scopes must pause/resume exclusively on unwind");
  check(cpuProcessTimespecNs(2, 3).value() == 2000000003ull &&
            !cpuProcessTimespecNs(-1, 0).has_value() &&
            !cpuProcessTimespecNs(0, 1000000000ll).has_value() &&
            !cpuProcessTimespecNs(
                std::numeric_limits<std::int64_t>::max(), 0).has_value(),
        "process CPU clock conversion must reject invalid and overflowing samples");
}

struct CpuTapeExperimentSummary {
  bool complete = false;
  std::string failure;
  std::uint64_t digest = 1469598103934665603ull;
  std::size_t commands = 0u;
  std::size_t draws = 0u;
  std::size_t directDraws = 0u;
  std::size_t compatibilityDraws = 0u;
  std::size_t submitted = 0u;
  std::size_t completed = 0u;
  std::size_t reclaimed = 0u;
  std::size_t resourcesCreated = 0u;
  std::size_t resourcesReleased = 0u;
  std::uint64_t elapsedNs = 0u;
  std::array<std::uint64_t, 5> phaseNs{};
  std::optional<std::uint64_t> nativeCpuNs;
  bool retainedBeforeCompletion = false;
  bool bufferBackingObserved = false;
  bool capturedARequiredRetention = false;
  bool capturedABackingPreserved = false;
  bool bufferBackingRevisionAdvanced = false;
  bool distinctDynamicBackings = false;
  bool bufferBackingOracleComplete = false;
  bool faultTriggered = false;
  bool faultPreEffectOwnershipUnchanged = false;
  bool faultRetrySucceeded = false;
  std::uint32_t faultMutationApplies = 0u;
  std::string faultReceipt;
  bool resetOk = false;
  std::vector<CpuTapeTraceRow> trace;
  std::vector<CpuTapeDrawObservation> drawObservations;
  std::uint32_t groupedRanges = 0u;
  std::uint32_t groupedEvents = 0u;
  std::uint32_t groupedObservedDraws = 0u;
  bool groupedPublicationVerified = false;
  std::uint32_t capacityRejected = 0u;
  std::uint32_t populatedFallback = 0u;
  std::uint32_t capacityRotated = 0u;
};

class CpuTapeExperimentSink final : public dxmt9::d3d9::RenderTapeReplaySink {
 public:
  CpuTapeExperimentSink(RuntimeFixture& fixture,
                        const dxmt9::d3d9::RenderTapeBlobCatalogue& catalogue,
                        const std::vector<std::vector<std::byte>>& blobs,
                        bool compatibility, bool afterDestroy,
                        bool metalResources,
                        bool managedAllocationFault,
                        std::uint64_t iteration,
                        std::string fault, std::size_t maxPending,
                        CpuTapeExperimentSummary& summary,
                        CpuTapePhaseLedger<std::chrono::steady_clock>& phaseLedger,
                        const dxmt9::d3d9::RenderTapeCpuPublicationPlan*
                            publicationPlan,
                        dxmt9::d3d9::RenderTapeCompletionPlanController*
                            completionPlan)
      : fixture_(fixture), catalogue_(catalogue), blobs_(blobs),
        compatibility_(compatibility), afterDestroy_(afterDestroy),
        metalResources_(metalResources),
        managedAllocationFault_(managedAllocationFault), iteration_(iteration),
        fault_(std::move(fault)), maxPending_(maxPending), summary_(summary),
        phaseLedger_(phaseLedger),
        publicationPlan_(publicationPlan),
        externalPlan_(completionPlan) {
    if (externalPlan_) {
      const auto started = externalPlan_->beginIteration(
          static_cast<std::uint32_t>(iteration_),
          compatibility_
              ? dxmt9::d3d9::RenderTapeCompletionPlanRoute::Compatibility
              : dxmt9::d3d9::RenderTapeCompletionPlanRoute::ProductionSlot);
      if (!started.accepted()) {
        failure_ = std::string("completion-plan-begin-") +
                   completionPlanDispositionName(started.disposition);
      }
    }
  }

  bool bootstrap(const dxmt9::d3d9::RenderTapeBootstrapHeader& fixed,
                 std::span<const std::byte> overlayChunks,
                 dxmt9::d3d9::RenderTapeBootstrapReplayMode) override {
    auto phase = phaseLedger_.scoped(CpuTapePhase::Replay);
    const auto eventOrdinal = beginEvent();
    bootstrapCount_ = fixed.overlayCount;
    bootstrap_.assign(overlayChunks.begin(), overlayChunks.end());
    return finishEvent(eventOrdinal);
  }

  bool replayBootstrap() {
    const auto overlayChunks = std::span<const std::byte>(bootstrap_);
    std::size_t offset = 0u;
    for (std::uint32_t index = 0u; index < bootstrapCount_; ++index) {
      if (overlayChunks.size() - offset <
          sizeof(D9CCommandChunkWireHeader)) {
        failure_ = "bootstrap-overlay-truncated";
        return false;
      }
      D9CCommandChunkWireHeader header{};
      std::memcpy(&header, overlayChunks.data() + offset, sizeof(header));
      const std::size_t total = static_cast<std::size_t>(
          header.payloadArenaOffset) + header.payloadArenaSize;
      if (total == 0u || total > overlayChunks.size() - offset) {
        failure_ = "bootstrap-overlay-layout";
        return false;
      }
      const auto overlay = overlayChunks.subspan(offset, total);
      const dxmt9::d3d9::CommandChunkEnvelope envelope{
          .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
          .recordCount = header.recordCount,
          .handleCount = header.handleCount};
      dxmt9::d3d9::ImportedChunkView imported;
      if (!dxmt9::d3d9::importPrevalidatedCommandChunk(
              overlay, envelope, imported) ||
          !imported.handles.empty()) {
        failure_ = "unsupported-grammar: bootstrap handles";
        return false;
      }
      if (dxmt9::d3d9::replayPrevalidatedResolvedCommandChunk(
              fixture_.cDevice.get(), overlay, envelope, {},
              compatibility_ ? dxmt9::d3d9::ProviderReplayPolicy::Compatibility
                             : dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot) !=
          dxmt9::core::D3D_OK) {
        failure_ = "bootstrap-replay-failed";
        return false;
      }
      offset += total;
    }
    bootstrapCount_ = 0;
    return offset == overlayChunks.size();
  }

  bool objectDefine(const dxmt9::d3d9::RenderTapeObjectDefineHeader& fixed,
                    std::span<const std::byte> descriptor) override {
    auto phase = phaseLedger_.scoped(CpuTapePhase::Setup);
    const auto eventOrdinal = beginEvent();
    // The validator requires a live output oracle surface. CPU mode admits
    // only preflight-proven implicit swapchain metadata, never a GPU object.
    if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_SURFACE) {
      return finishEvent(eventOrdinal);
    }
    if (fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
        fixed.descriptorKind !=
            static_cast<std::uint32_t>(
                dxmt9::d3d9::RenderTapeDescriptorKind::Buffer)) {
      failure_ = "unsupported-grammar: only buffer ObjectDefine is admitted";
      return false;
    }
    D9CBufferDesc desc{};
    if (descriptor.size() != sizeof(desc)) {
      failure_ = "invalid-buffer-descriptor";
      return false;
    }
    std::memcpy(&desc, descriptor.data(), sizeof(desc));
    constexpr std::uint32_t kD3DUsageDynamic = 0x200u;
    dynamicResourceMode_ = (desc.usage & kD3DUsageDynamic) != 0u;
    auto* buffer = desc.format
                       ? dxmt9c_device_create_index_buffer(
                             fixture_.cDevice.get(), desc.size, desc.usage,
                             desc.format, desc.pool)
                       : dxmt9c_device_create_vertex_buffer(
                             fixture_.cDevice.get(), desc.size, desc.usage,
                             desc.fvf, desc.pool);
    if (!buffer) {
      failure_ = "buffer-create-failed";
      return false;
    }
    objects_.push_back({fixed.identity, buffer});
    ++summary_.resourcesCreated;
    return finishEvent(eventOrdinal);
  }

  bool objectDestroy(
      const dxmt9::d3d9::RenderTapeObjectDestroyHeader& fixed) override {
    auto phase = phaseLedger_.scoped(CpuTapePhase::Setup);
    const auto eventOrdinal = beginEvent();
    auto it = find(fixed.identity);
    if (it == objects_.end()) {
      failure_ = "stale-object-destroy";
      return false;
    }
    auto* buffer = it->buffer;
    const auto handle = buffer->obj->handle();
    dxmt9c_buffer_release(buffer);
    ++summary_.resourcesReleased;
    retiredHandles_.push_back(handle);
    objects_.erase(it);
    if (externalPlan_) {
      const bool wasPending = externalPending();
      const auto evidence =
          dxmt9::CommandQueueArenaLeaseTestAccess::bufferLifetime(
              fixture_.routing->queue_, handle);
      if (wasPending && evidence.alive && evidence.destroyPending &&
          evidence.lastUsedSeq > evidence.completedSeq) {
        summary_.retainedBeforeCompletion = true;
      } else if (wasPending) {
        failure_ = "destroy-before-completion-retention-unproven";
        return false;
      }
      if (!finishEvent(eventOrdinal)) return false;
      if (wasPending && !externalPending() &&
          dxmt9::CommandQueueArenaLeaseTestAccess::bufferAlive(
              fixture_.routing->queue_, handle)) {
        failure_ = "completed-buffer-not-reclaimed";
        return false;
      }
      return failure_.empty();
    } else if (afterDestroy_) {
      const auto evidence =
          dxmt9::CommandQueueArenaLeaseTestAccess::bufferLifetime(
              fixture_.routing->queue_, handle);
      if (!pending_.empty() && evidence.alive && evidence.destroyPending &&
          evidence.lastUsedSeq > evidence.completedSeq) {
        summary_.retainedBeforeCompletion = true;
      } else {
        failure_ = "destroy-before-completion-retention-unproven";
        return false;
      }
      // Keep the production snapshot pin through mutation B.  The dynamic
      // Metal-backed oracle releases this pending source at PresentComplete,
      // after it has read captured A's physical backing.
      if (!(dynamicResourceMode_ && metalResources_ &&
            capturedARequiredRetention_)) {
        releasePending();
      }
      if (!failure_.empty()) return false;
      if (!(dynamicResourceMode_ && metalResources_ &&
            capturedARequiredRetention_) &&
          dxmt9::CommandQueueArenaLeaseTestAccess::bufferAlive(
              fixture_.routing->queue_, handle)) {
        failure_ = "completed-buffer-not-reclaimed";
        return false;
      }
    }
    return finishEvent(eventOrdinal);
  }

  bool resourceMutation(
      const dxmt9::d3d9::RenderTapeResourceMutationHeader& fixed) override {
    auto phase = phaseLedger_.scoped(CpuTapePhase::Replay);
    const auto eventOrdinal = beginEvent();
    if (fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
        (fixed.kind != static_cast<std::uint32_t>(
                           dxmt9::d3d9::RenderTapeMutationKind::CpuUnlock) &&
         fixed.kind != static_cast<std::uint32_t>(
                           dxmt9::d3d9::RenderTapeMutationKind::Upload))) {
      failure_ = "unsupported-grammar: only buffer lock/unlock mutations are admitted";
      return false;
    }
    auto it = find(fixed.identity);
    if (it == objects_.end() || fixed.byteSize > UINT32_MAX ||
        fixed.byteOffset > UINT32_MAX) {
      failure_ = "mutation-object-or-range-invalid";
      return false;
    }
    const auto blobIt = blobsByDigest(fixed.digest, fixed.byteSize);
    if (!blobIt) {
      failure_ = "mutation-blob-missing";
      return false;
    }
    if (dynamicResourceMode_ && dynamicMutationCount_ == 1u &&
        metalResources_) {
      auto observe = phaseLedger_.scoped(CpuTapePhase::Observe);
      const auto evidence = dxmt9::CommandQueueArenaLeaseTestAccess::bufferLifetime(
          fixture_.routing->queue_, it->buffer->obj->handle());
      // The completion source is the production lifetime witness.  Only read
      // the captured A address when that source is still beyond the published
      // completion waterline; an early completion legitimately permits reuse
      // and must not be treated as a failed retention proof.
      capturedARequiredRetention_ =
          capturedASubmissionSeq_ != 0u &&
          capturedASubmissionSeq_ > evidence.completedSeq &&
          evidence.lastUsedSeq >= capturedASubmissionSeq_;
      summary_.capturedARequiredRetention = capturedARequiredRetention_;
    }
    void* mapped = nullptr;
    if (dxmt9c_buffer_lock(it->buffer, static_cast<std::uint32_t>(fixed.byteOffset),
                           static_cast<std::uint32_t>(fixed.byteSize), &mapped,
                           dxmt9::d3d9::renderTapeBufferMutationLockFlags(
                               static_cast<dxmt9::d3d9::RenderTapeBufferMutationDisposition>(
                                   fixed.bufferDisposition))) != dxmt9::core::D3D_OK ||
        !mapped) {
      failure_ = "buffer-lock-failed";
      return false;
    }
    std::memcpy(mapped, blobIt->data(), blobIt->size());
    digestValue(it->contentDigest, fixed.byteOffset);
    digestValue(it->contentDigest, fixed.byteSize);
    digestValue(it->contentDigest, fixed.bufferDisposition);
    digestBytes(it->contentDigest, mapped, blobIt->size());
    const bool injectManagedAllocationFailure =
        managedAllocationFault_ && !faultAttempted_;
    std::uint64_t revisionBeforeUnlock = 0u;
    std::size_t renameRingSizeBeforeUnlock = 0u;
    std::size_t renameActiveIndexBeforeUnlock = 0u;
    std::vector<std::uint8_t> shadowBeforeUnlock;
    std::size_t queueDepthBeforeUnlock = 0u;
    std::size_t queueBytesBeforeUnlock = 0u;
    std::uint64_t queuedMutationSeqBeforeUnlock = 0u;
    if (injectManagedAllocationFailure) {
      auto* record = fixture_.routing->queue_.pool().findBuffer(
          it->buffer->obj->handle().value);
      if (!record || !it->buffer->replayDrainTarget) {
        failure_ = "managed-allocation-fault-snapshot-missing";
        return false;
      }
      revisionBeforeUnlock = record->contentRevision;
      renameRingSizeBeforeUnlock = record->renameRing.size();
      renameActiveIndexBeforeUnlock = record->renameActiveIndex;
      shadowBeforeUnlock = record->shadow;
      queueDepthBeforeUnlock =
          fixture_.cDevice->replayOffload
              ? fixture_.cDevice->replayOffload->queue().depth()
              : 0u;
      queueBytesBeforeUnlock =
          fixture_.cDevice->replayOffload
              ? fixture_.cDevice->replayOffload->queue().queuedBytesForTest()
              : 0u;
      queuedMutationSeqBeforeUnlock =
          it->buffer->replayDrainTarget->lastQueuedSeq;
      if (!fixture_.cDevice->replayOffload) {
        failure_ = "managed-allocation-fault-worker-missing";
        return false;
      }
      fixture_.cDevice->replayOffload->queue().failNextAllocationForTest();
      faultAttempted_ = true;
    }
    const auto unlockResult = dxmt9c_buffer_unlock(it->buffer);
    if (unlockResult != dxmt9::core::D3D_OK &&
        injectManagedAllocationFailure) {
      const bool allocationHookConsumed =
          fixture_.allocationFailure_ &&
          !fixture_.allocationFailure_->failNext.load(std::memory_order_acquire);
      if (unlockResult != dxmt9::core::D3DERR_INVALIDCALL ||
          !allocationHookConsumed) {
        failure_ = "managed-allocation-fault-not-injected-invalidcall";
        return false;
      }
      auto* record = fixture_.routing->queue_.pool().findBuffer(
          it->buffer->obj->handle().value);
      const bool unchanged =
          record && record->contentRevision == revisionBeforeUnlock &&
          record->renameRing.size() == renameRingSizeBeforeUnlock &&
          record->renameActiveIndex == renameActiveIndexBeforeUnlock &&
          record->shadow == shadowBeforeUnlock &&
          fixture_.cDevice->replayOffload->queue().depth() ==
              queueDepthBeforeUnlock &&
          fixture_.cDevice->replayOffload->queue().queuedBytesForTest() ==
              queueBytesBeforeUnlock &&
          it->buffer->replayDrainTarget->lastQueuedSeq ==
              queuedMutationSeqBeforeUnlock && it->buffer->obj->locked();
      summary_.faultTriggered = true;
      summary_.faultPreEffectOwnershipUnchanged = unchanged;
      summary_.faultReceipt =
          "managed-mutation-allocation-rejected-pre-effect-public-unlock";
      if (!unchanged) {
        failure_ = "managed-allocation-fault-preeffect-ownership-changed";
        return false;
      }
      // Re-enter the public bridge so its fence recomputes admission for the
      // still-open lock. The test must not manufacture a planned capability.
      const auto retry = dxmt9c_buffer_unlock(it->buffer);
      if (retry != dxmt9::core::D3D_OK ||
          !fixture_.cDevice->replayOffload->start(fixture_.cDevice.get())) {
        failure_ = "managed-allocation-fault-retry-rejected";
        return false;
      }
      fixture_.cDevice->replayOffload->queue().waitDrained();
      summary_.faultMutationApplies =
          fixture_.routing->mutationApplies.load();
      summary_.faultRetrySucceeded = summary_.faultMutationApplies == 1u;
      if (!summary_.faultRetrySucceeded) {
        failure_ = "managed-allocation-fault-retry-apply-count";
        return false;
      }
    } else if (unlockResult != dxmt9::core::D3D_OK) {
      failure_ = "buffer-unlock-failed";
      return false;
    }
    if (dynamicResourceMode_) {
      if (dynamicMutationCount_ == 0u) {
        dynamicSeedBytes_.assign(
            reinterpret_cast<const std::uint8_t*>(blobIt->data()),
            reinterpret_cast<const std::uint8_t*>(blobIt->data()) +
                blobIt->size());
      } else if (dynamicMutationCount_ == 1u && metalResources_ &&
                 capturedARequiredRetention_) {
        if (capturedAContentsAddress_ == 0u || dynamicSeedBytes_.empty()) {
          failure_ = "dynamic-capture-A-missing-before-mutation-B";
          return false;
        }
        const auto* capturedA = reinterpret_cast<const std::uint8_t*>(
            static_cast<std::uintptr_t>(capturedAContentsAddress_));
        if (std::memcmp(capturedA, dynamicSeedBytes_.data(),
                        dynamicSeedBytes_.size()) != 0) {
          failure_ = "captured-A-backing-changed-before-completion";
          return false;
        }
        const auto resourceHandle = it->buffer->obj->handle();
        const auto* record =
            fixture_.routing->queue_.pool().findBuffer(resourceHandle.value);
        if (!record || record->contents == static_cast<const void*>(capturedA)) {
          failure_ = "dynamic-mutation-B-did-not-rotate-from-captured-A";
          return false;
        }
        summary_.capturedABackingPreserved = true;
      }
      dynamicExpectedBytes_.assign(
          reinterpret_cast<const std::uint8_t*>(blobIt->data()),
          reinterpret_cast<const std::uint8_t*>(blobIt->data()) +
              blobIt->size());
      ++dynamicMutationCount_;
    }
    return finishEvent(eventOrdinal);
  }

  bool commandChunk(const dxmt9::d3d9::CommandChunkEnvelope& envelope,
                    std::span<const std::byte> chunk) override {
    auto phase = phaseLedger_.scoped(CpuTapePhase::Replay);
    const auto eventOrdinal = beginEvent();
    const auto capturedObservationBegin =
        fixture_.routing->capturedBindingObservations.size();
    std::uint32_t eventSubmissionOrdinal = 0u;
    if (bootstrapCount_ && !replayBootstrap()) return false;
    dxmt9::d3d9::ImportedChunkView imported;
    if (!dxmt9::d3d9::importPrevalidatedCommandChunk(chunk, envelope,
                                                       imported)) {
      failure_ = "command-import-failed";
      return false;
    }
    for (const auto& handle : imported.handles) {
      if (handle.kind != D9C_CHUNK_HANDLE_KIND_BUFFER) {
        failure_ = "unsupported-grammar: command references non-buffer object";
        return false;
      }
    }
    std::vector<void*> resolved;
    resolved.reserve(imported.handles.size());
    for (const auto& handle : imported.handles) {
      const D9CWireObjectIdentity identity{
          .kind = handle.kind,
          .generation = handle.generation,
          .objectId = handle.objectId};
      const auto it = find(identity);
      if (it == objects_.end()) {
        failure_ = "command-object-not-live";
        return false;
      }
      resolved.push_back(it->buffer);
    }
    const auto policy = compatibility_
                            ? dxmt9::d3d9::ProviderReplayPolicy::Compatibility
                            : dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot;
    const auto before = dxmt9::perf::snapshotRenderTapeReplay();
    if (dxmt9::d3d9::replayPrevalidatedResolvedCommandChunk(
            fixture_.cDevice.get(), chunk, envelope, resolved, policy) !=
        dxmt9::core::D3D_OK) {
      failure_ = "provider-replay-failed";
      return false;
    }
    const bool containsDraw = std::any_of(
        imported.records.begin(), imported.records.end(), [](const auto& record) {
          return record.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
                 record.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP;
        });
    if (dynamicResourceMode_ && containsDraw) {
      const auto& observations = fixture_.routing->capturedBindingObservations;
      if (observations.size() <= capturedObservationBegin) {
        failure_ = "dynamic-capture-snapshot-missing";
        return false;
      }
      const auto& observation = observations.back();
      if (observation.bytes.empty() || observation.contentsAddress == 0u) {
        failure_ = "dynamic-capture-snapshot-bytes-missing";
        return false;
      }
      if (!dynamicExpectedBytes_.empty() &&
          observation.bytes != dynamicExpectedBytes_) {
        failure_ = "dynamic-capture-bytes-do-not-match-mutation";
        return false;
      }
      digestBytes(summary_.digest, observation.bytes.data(),
                  observation.bytes.size());
      summary_.bufferBackingObserved = true;
      ++dynamicDrawCount_;
      if (dynamicDrawCount_ == 1u) {
        capturedAContentsAddress_ = observation.contentsAddress;
        capturedAMetalHandle_ = observation.metalHandle;
        capturedARevision_ = observation.contentRevision;
      } else if (dynamicDrawCount_ == 2u) {
        summary_.bufferBackingRevisionAdvanced =
            observation.contentRevision > capturedARevision_;
        summary_.distinctDynamicBackings =
            observation.metalHandle != capturedAMetalHandle_;
        if (!summary_.bufferBackingRevisionAdvanced) {
          failure_ = "dynamic-capture-backing-revision-did-not-advance";
          return false;
        }
        if (capturedARequiredRetention_ &&
            !summary_.distinctDynamicBackings) {
          failure_ = "dynamic-capture-backing-did-not-rotate";
          return false;
        }
        if (capturedARequiredRetention_ &&
            !summary_.capturedABackingPreserved) {
          failure_ = "dynamic-capture-A-preservation-unproven";
          return false;
        }
      }
    }
    const auto after = dxmt9::perf::snapshotRenderTapeReplay();
    summary_.directDraws += after.directLeaseDraws - before.directLeaseDraws;
    summary_.compatibilityDraws += after.compatibilityDraws - before.compatibilityDraws;
    const auto* publicationRange =
        publicationPlan_ ? publicationPlan_->rangeForEvent(eventOrdinal)
                          : nullptr;
    if (publicationRange && publicationRange->firstEvent !=
                                publicationRange->lastEvent) {
      if (!groupedActive_) {
        if (eventOrdinal != publicationRange->firstEvent) {
          failure_ = "grouped-publication-unexpected-start";
          return false;
        }
        groupedActive_ = true;
        groupedExpectedRange_ = *publicationRange;
        groupedFirstEvent_ = eventOrdinal;
        groupedLastEvent_ = eventOrdinal;
        groupedCommandEvents_ = 1u;
        groupedSourceCount_ = 0u;
        groupedObservedDraws_ = 0u;
      } else {
        if (eventOrdinal != groupedLastEvent_ + 1u ||
            eventOrdinal > groupedExpectedRange_.lastEvent) {
          failure_ = "grouped-publication-range-split";
          return false;
        }
        groupedLastEvent_ = eventOrdinal;
        ++groupedCommandEvents_;
      }
      if (eventOrdinal != publicationRange->lastEvent) {
        if (dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                fixture_.routing->queue_) != 0u) {
          failure_ = "grouped-publication-early-ready";
          return false;
        }
        return finishEvent(eventOrdinal);
      }
    }
    if (!dxmt9::CommandQueueArenaLeaseTestAccess::publishWriting(fixture_.routing->queue_)) {
      failure_ = "source-publication-failed";
      return false;
    }
    groupedObservation_ = groupedActive_;
    while (dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
        fixture_.routing->queue_)) {
      const auto source =
          dxmt9::CommandQueueArenaLeaseTestAccess::peekReadyCompletion(
              fixture_.routing->queue_);
      if (!source.completionIdentityValid()) {
        failure_ = "provider-did-not-publish-completion-source";
        return false;
      }
      if (externalPlan_ && !source.source.valid()) {
        failure_ = "completion-plan-receipt-source-unsupported";
        return false;
      }
      ++eventSubmissionOrdinal;
      if (externalPlan_) {
        if (externalPending() >= maxPending_) {
          failure_ = "max-pending-capacity-exhausted";
          return false;
        }
        const dxmt9::d3d9::RenderTapeQualifiedCompletionKey actual{
            .seqId = source.seqId,
            .sourceIndex = source.source.id.index,
            .sourceGeneration = source.source.id.generation,
            .storageFirstPage = source.source.storage.firstPage,
            .storagePageCount = source.source.storage.pageCount,
            .storageGeneration = source.source.storage.generation};
        dxmt9::CommandQueueArenaLeaseTestAccess::CompletionResult submission{};
        const auto admitted = externalPlan_->submit(
            eventOrdinal, eventSubmissionOrdinal, actual, [&] {
              if (!faultTriggered_ && fault_ == "before-submit") {
                faultTriggered_ = true;
                summary_.faultTriggered = true;
                failure_ = "fault-before-submit";
                return false;
              }
              submission =
                  dxmt9::CommandQueueArenaLeaseTestAccess::submitOneWithheld(
                      fixture_.routing->queue_,
                      [&](const auto& payload) { observePayload(payload); });
              return submission.submitted;
            });
        if (!admitted.accepted()) {
          if (failure_.empty()) {
            failure_ = std::string("completion-plan-submit-") +
                       completionPlanDispositionName(admitted.disposition);
          }
          return false;
        }
        if (submission.completion.seqId != actual.seqId ||
            submission.completion.source.id.index != actual.sourceIndex ||
            submission.completion.source.id.generation !=
                actual.sourceGeneration ||
            submission.completion.source.storage.firstPage !=
                actual.storageFirstPage ||
            submission.completion.source.storage.pageCount !=
                actual.storagePageCount ||
            submission.completion.source.storage.generation !=
                actual.storageGeneration) {
          failure_ = "completion-plan-source-binding-mismatch";
          return false;
        }
        summary_.commands += submission.commandCount;
        summary_.draws = summary_.directDraws + summary_.compatibilityDraws;
        ++summary_.submitted;
        if (dynamicResourceMode_ && dynamicDrawCount_ == 1u &&
            capturedASubmissionSeq_ == 0u) {
          capturedASubmissionSeq_ = source.seqId;
        }
        summary_.trace.push_back({
            .iteration = iteration_,
            .event = eventOrdinal,
            .eventSubmission = eventSubmissionOrdinal,
            .submission = submission.completion.seqId,
            .source = submission.completion.source.id.index,
            .sourceGeneration =
                submission.completion.source.id.generation,
            .storageFirstPage =
                submission.completion.source.storage.firstPage,
            .storagePageCount =
                submission.completion.source.storage.pageCount,
            .storageGeneration =
                submission.completion.source.storage.generation,
            .firstEvent = groupedActive_ ? groupedFirstEvent_ : eventOrdinal,
            .lastEvent = groupedActive_ ? groupedLastEvent_ : eventOrdinal,
            .submitted = true});
        if (groupedActive_) ++groupedSourceCount_;
      } else {
        if (pending_.size() >= maxPending_) {
          failure_ = "max-pending-capacity-exhausted";
          return false;
        }
        Pending pending{};
        pending.key = {iteration_, source.seqId, source.source.id.index, 0u,
                       source.source.id.generation};
        if (!pending.schedule.append({pending.key}).accepted() ||
            !pending.schedule.beginIteration(iteration_).accepted()) {
          failure_ = "completion-schedule-admission-rejected";
          return false;
        }
        auto& schedule = pending.schedule;
        if (!schedule.submit(pending.key, [&] {
              if (!faultTriggered_ && fault_ == "before-submit") {
                faultTriggered_ = true;
                summary_.faultTriggered = true;
                failure_ = "fault-before-submit";
                return false;
              }
              pending.submission =
                  dxmt9::CommandQueueArenaLeaseTestAccess::submitOneWithheld(
                      fixture_.routing->queue_,
                      [&](const auto& payload) { observePayload(payload); });
              return pending.submission.submitted;
            }).accepted()) {
          if (failure_.empty()) failure_ = "completion-submit-rejected";
          return false;
        }
        summary_.commands += pending.submission.commandCount;
        summary_.draws = summary_.directDraws + summary_.compatibilityDraws;
        ++summary_.submitted;
        if (dynamicResourceMode_ && dynamicDrawCount_ == 1u &&
            capturedASubmissionSeq_ == 0u) {
          capturedASubmissionSeq_ = source.seqId;
        }
        summary_.trace.push_back({
            .iteration = iteration_,
            .event = eventOrdinal,
            .eventSubmission = eventSubmissionOrdinal,
            .submission = pending.submission.completion.seqId,
            .source = pending.submission.completion.source.id.index,
            .sourceGeneration =
                pending.submission.completion.source.id.generation,
            .storageFirstPage =
                pending.submission.completion.source.storage.firstPage,
            .storagePageCount =
                pending.submission.completion.source.storage.pageCount,
            .storageGeneration =
                pending.submission.completion.source.storage.generation,
            .firstEvent = groupedActive_ ? groupedFirstEvent_ : eventOrdinal,
            .lastEvent = groupedActive_ ? groupedLastEvent_ : eventOrdinal,
            .submitted = true});
        if (groupedActive_) ++groupedSourceCount_;
        pending_.push_back(std::move(pending));
        if (pending_.size() >
            dxmt9::d3d9::kRenderTapeCompletionScheduleCapacity) {
          failure_ = "completion-capacity-exhausted";
          return false;
        }
      }
    }
    groupedObservation_ = false;
    if (groupedActive_) {
      if (!dxmt9::d3d9::renderTapeGroupedPublicationMatches(
              groupedExpectedRange_, groupedFirstEvent_, groupedLastEvent_,
              groupedCommandEvents_, eventOrdinal, groupedSourceCount_,
              groupedObservedDraws_)) {
        failure_ = "grouped-publication-observation-mismatch";
        return false;
      }
      ++summary_.groupedRanges;
      summary_.groupedEvents += groupedCommandEvents_;
      summary_.groupedObservedDraws += groupedObservedDraws_;
      summary_.groupedPublicationVerified = true;
      groupedActive_ = false;
    }
    if (externalPlan_) return finishEvent(eventOrdinal);
    if (!afterDestroy_) releasePending();
    return finishEvent(eventOrdinal);
  }

  bool orderedControl(const dxmt9::d3d9::RenderTapeOrderedControlHeader&,
                      std::span<const std::byte>) override {
    auto phase = phaseLedger_.scoped(CpuTapePhase::Replay);
    (void)beginEvent();
    failure_ = "unsupported-grammar: ordered controls are not admitted";
    return false;
  }

  bool presentComplete(const dxmt9::d3d9::RenderTapePresentCompleteHeader&,
                       std::span<const std::byte>) override {
    auto phase = phaseLedger_.scoped(CpuTapePhase::Completion);
    const auto eventOrdinal = beginEvent();
    if (externalPlan_) return finishEvent(eventOrdinal);
    releasePending();
    if (dynamicResourceMode_ && metalResources_) {
      for (const auto handle : retiredHandles_) {
        if (dxmt9::CommandQueueArenaLeaseTestAccess::bufferAlive(
                fixture_.routing->queue_, handle)) {
          failure_ = "dynamic-completed-buffer-not-reclaimed";
          break;
        }
      }
    }
    return failure_.empty() && finishEvent(eventOrdinal);
  }

  void finish() {
    auto phase = phaseLedger_.scoped(CpuTapePhase::Cleanup);
    const auto queueHasWork = [&] {
      return dxmt9::CommandQueueArenaLeaseTestAccess::pendingCompletionCount(
                 fixture_.routing->queue_) != 0u ||
             dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                 fixture_.routing->queue_) != 0u ||
             dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                 fixture_.routing->queue_) != 0u;
    };
    if (externalPlan_) {
      auto completion = phaseLedger_.scoped(CpuTapePhase::Completion);
      if (!externalPlan_->finishIteration().accepted() && failure_.empty()) {
        failure_ = "completion-plan-finish-unresolved";
      }
      if (externalPending() || queueHasWork()) {
        if (failure_.empty()) failure_ = "unresolved-work-at-finish";
      }
    } else if (!pending_.empty() || queueHasWork()) {
      if (failure_.empty()) failure_ = "unresolved-work-at-finish";
      // Preset completion modes retain their historical explicit cleanup.
      if (!failure_.empty()) {
        auto completion = phaseLedger_.scoped(CpuTapePhase::Completion);
        while (dxmt9::CommandQueueArenaLeaseTestAccess::pendingCompletionCount(
                   fixture_.routing->queue_)) {
          if (!dxmt9::CommandQueueArenaLeaseTestAccess::acknowledgeOne(
                  fixture_.routing->queue_)) {
            break;
          }
          (void)dxmt9::CommandQueueArenaLeaseTestAccess::finishQueue(
              fixture_.routing->queue_);
        }
        pending_.clear();
        (void)dxmt9::CommandQueueArenaLeaseTestAccess::publishWriting(
            fixture_.routing->queue_);
        const auto ready =
            dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                fixture_.routing->queue_);
        for (std::size_t index = 0; index < ready; ++index) {
          (void)dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
              fixture_.routing->queue_);
        }
      }
    }
    for (auto& object : objects_) {
      const auto handle = object.buffer->obj->handle();
      dxmt9c_buffer_release(object.buffer);
      ++summary_.resourcesReleased;
      object.handle = handle;
    }
    dxmt9::CommandQueueArenaLeaseTestAccess::reclaimCompletedPool(
        fixture_.routing->queue_);
    summary_.resetOk =
        dxmt9::CommandQueueArenaLeaseTestAccess::pendingCompletionCount(
            fixture_.routing->queue_) == 0u &&
        dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
            fixture_.routing->queue_) == 0u &&
        dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
            fixture_.routing->queue_) == 0u &&
        dxmt9::CommandQueueArenaLeaseTestAccess::residentCount(
            fixture_.routing->queue_) == 0u &&
        std::all_of(retiredHandles_.begin(), retiredHandles_.end(),
                    [&](const auto handle) {
          return !dxmt9::CommandQueueArenaLeaseTestAccess::bufferAlive(
              fixture_.routing->queue_, handle);
        }) &&
        std::all_of(objects_.begin(), objects_.end(), [&](const auto& object) {
          return !dxmt9::CommandQueueArenaLeaseTestAccess::bufferAlive(
              fixture_.routing->queue_, object.handle);
        });
    objects_.clear();
  }

  const std::string& failure() const noexcept { return failure_; }

 private:
  std::uint32_t beginEvent() noexcept { return ++eventOrdinal_; }

  std::uint32_t externalPending() const noexcept {
    return externalPlan_ ? externalPlan_->submitted() - externalPlan_->released()
                         : 0u;
  }

  bool finishEvent(std::uint32_t eventOrdinal) {
    if (!externalPlan_) return failure_.empty();
    auto phase = phaseLedger_.scoped(CpuTapePhase::Completion);
    if (!failure_.empty()) return false;
    const auto release = externalPlan_->releaseDue(
        eventOrdinal, [&](const auto& actual) {
          if (!faultTriggered_ && fault_ == "before-release") {
            faultTriggered_ = true;
            summary_.faultTriggered = true;
            failure_ = "fault-before-release";
            return false;
          }
          if (!dxmt9::CommandQueueArenaLeaseTestAccess::acknowledgeOne(
                  fixture_.routing->queue_)) {
            failure_ = "completion-plan-acknowledge-failed";
            return false;
          }
          ++summary_.completed;
          for (auto& row : summary_.trace) {
            if (row.iteration == iteration_ &&
                row.submission == actual.seqId &&
                row.source == actual.sourceIndex &&
                row.sourceGeneration == actual.sourceGeneration &&
                row.storageFirstPage == actual.storageFirstPage &&
                row.storagePageCount == actual.storagePageCount &&
                row.storageGeneration == actual.storageGeneration &&
                !row.completed) {
              row.completed = true;
              row.releasedAtEvent = eventOrdinal;
              break;
            }
          }
          if (!dxmt9::CommandQueueArenaLeaseTestAccess::finishQueue(
                  fixture_.routing->queue_)) {
            failure_ = "completion-plan-finish-failed";
            return false;
          }
          const auto reclaimedThrough =
              dxmt9::CommandQueueArenaLeaseTestAccess::reclaimCompletedPool(
                  fixture_.routing->queue_);
          if (reclaimedThrough < actual.seqId) {
            failure_ = "completion-plan-reclaim-waterline-failed";
            return false;
          }
          ++summary_.reclaimed;
          return true;
        });
    if (!release.accepted()) {
      failure_ = std::string("completion-plan-release-") +
                 completionPlanDispositionName(release.disposition);
      return false;
    }
    const auto ended = externalPlan_->endEvent(eventOrdinal);
    if (!ended.accepted()) {
      failure_ = std::string("completion-plan-event-") +
                 completionPlanDispositionName(ended.disposition);
      return false;
    }
    return failure_.empty();
  }

  void digestResource(dxmt9::core::Handle handle) {
    if (!handle) { digestValue(summary_.digest, std::uint64_t{0}); return; }
    const auto found = std::find_if(objects_.begin(), objects_.end(), [&](const auto& object) {
      return object.buffer->obj->handle() == handle;
    });
    if (found == objects_.end()) {
      failure_ = "unqualified-executed-resource";
      return;
    }
    digestValue(summary_.digest, found->identity.kind);
    digestValue(summary_.digest, found->identity.objectId);
    digestValue(summary_.digest, found->identity.generation);
    digestValue(summary_.digest, found->contentDigest);
  }

  void observePayload(const dxmt9::core::SourcePayloadView& payload) {
    auto phase = phaseLedger_.scoped(CpuTapePhase::Observe);
    using namespace dxmt9;
    for (std::size_t index = 0; index < payload.commandCount(); ++index) {
      const auto source = payload.commandAt(index);
      const auto& command = source.command;
      if (source.kind() != core::MetalCommandKind::DrawRun) {
        // The bounded grammar admits only pointer-free Clear/Present here.
        digestValue(summary_.digest, effectiveCommandDigest(payload, index));
        continue;
      }
      if (groupedObservation_) {
        groupedObservedDraws_ +=
            static_cast<std::uint32_t>(command.drawParams.size());
      }
      for (const auto& param : command.drawParams) {
        if (!command.drawState.hot) { failure_ = "missing-first-draw-state"; return; }
        auto hot = *command.drawState.hot;
        core::DrawBindingOverride binding{};
        if (encoders::drawParamBindingOverride(param, command.drawPayloadBytes, binding))
          encoders::applyDrawBindingOverride(hot, nullptr, binding);
        auto volatileState = state::buildFsVolatile(command.drawState);
        if (binding.alphaTestStateValid) {
          const auto alpha = state::makeFsVolatile(
              binding.alphaTestEnable, binding.alphaTestFunc,
              binding.alphaTestRef, volatileState.sampleMask);
          volatileState.alphaTest = alpha.alphaTest;
          volatileState.alphaRef = alpha.alphaRef;
        }
        digestValue(summary_.digest, volatileState.alphaTest);
        digestValue(summary_.digest, volatileState.alphaRef);
        digestValue(summary_.digest, volatileState.sampleMask);
        // One marker per logical draw, never per physical DrawRun/source.
        digestValue(summary_.digest, core::MetalCommandKind::DrawRun);
        digestValue(summary_.digest, param.primitiveType);
        digestValue(summary_.digest, param.primitiveCount);
        digestValue(summary_.digest, param.startVertex);
        digestValue(summary_.digest, param.baseVertexIndex);
        digestValue(summary_.digest, param.startIndex);
        digestValue(summary_.digest, param.indexed);
        digestValue(summary_.digest, param.instanceCount);
        digestValue(summary_.digest, hot.key.fvf);
        digestValue(summary_.digest, hot.key.vertexDeclHash);
        digestValue(summary_.digest, hot.key.vertexShaderHash);
        digestValue(summary_.digest, hot.key.pixelShaderHash);
        digestValue(summary_.digest, hot.viewport.viewport);
        digestRect(summary_.digest, hot.viewport.scissor);
        digestValue(summary_.digest, hot.viewport.scissorEnabled);
        const auto states = [&](const auto& set) {
          digestValue(summary_.digest, set.count);
          for (std::size_t i = 0; i < set.count && i < set.entries.size(); ++i) {
            digestValue(summary_.digest, set.entries[i].state);
            digestValue(summary_.digest, set.entries[i].value);
          }
        };
        states(hot.renderStates);
        for (const auto& set : hot.textureStageStates) states(set);
        for (const auto& set : hot.samplerStates) states(set);
        digestValue(summary_.digest, hot.streamMask);
        for (std::size_t stream = 0; stream < hot.streamBuffers.size(); ++stream) {
          digestResource(hot.streamBuffers[stream]);
          digestValue(summary_.digest, hot.streamOffsets[stream]);
          digestValue(summary_.digest, hot.streamStrides[stream]);
          digestValue(summary_.digest, hot.streamFrequencies[stream]);
        }
        digestResource(hot.indexBuffer);
        core::DrawBindingSnapshot snapshots{};
        if (encoders::drawParamBindingSnapshot(param, command.drawPayloadBytes, snapshots)) {
          for (std::size_t stream = 0; stream < snapshots.streams.size(); ++stream) {
            if (!(snapshots.streamMask & (1u << stream))) continue;
            digestResource(snapshots.streams[stream].buffer);
            digestValue(summary_.digest, snapshots.streams[stream].snapshot.byteSize);
            digestValue(summary_.digest, snapshots.streams[stream].snapshot.contentRevision);
          }
        }
        core::DrawUniformPayload scratch{};
        const auto* uniforms = core::drawRunUniformPayloadForParam(command, param, scratch);
        if (!uniforms) { failure_ = "missing-draw-uniforms"; return; }
        // Member spans exclude struct padding and runtime uniform locators.
        digestValue(summary_.digest, uniforms->vsConst.float4);
        digestValue(summary_.digest, uniforms->vsConst.int4);
        digestValue(summary_.digest, uniforms->vsConst.bools);
        digestValue(summary_.digest, uniforms->psConst.float4);
        digestValue(summary_.digest, uniforms->psConst.int4);
        digestValue(summary_.digest, uniforms->psConst.bools);
        digestValue(summary_.digest, uniforms->worldViewProj.m);
        digestValue(summary_.digest, uniforms->ffpView.m);
        digestValue(summary_.digest, uniforms->ffpWorldView.m);
        digestValue(summary_.digest, uniforms->ffpNormalMatrix.m);
        digestColor(summary_.digest, uniforms->material.emissive);
        digestColor(summary_.digest, uniforms->material.ambient);
        digestColor(summary_.digest, uniforms->material.diffuse);
        digestColor(summary_.digest, uniforms->material.specular);
        digestValue(summary_.digest, uniforms->material.power);
        for (const auto& light : uniforms->lights) {
          digestValue(summary_.digest, light.type);
          digestValue(summary_.digest, light.enabled);
          digestColor(summary_.digest, light.diffuse);
          digestColor(summary_.digest, light.specular);
          digestColor(summary_.digest, light.ambient);
          digestValue(summary_.digest, light.position);
          digestValue(summary_.digest, light.direction);
          digestValue(summary_.digest, light.range);
          digestValue(summary_.digest, light.falloff);
          digestValue(summary_.digest, light.attenuation0);
          digestValue(summary_.digest, light.attenuation1);
          digestValue(summary_.digest, light.attenuation2);
          digestValue(summary_.digest, light.theta);
          digestValue(summary_.digest, light.phi);
        }
        for (const auto& matrix : uniforms->ffpBlendWorldViewProj) digestValue(summary_.digest, matrix.m);
        for (const auto& matrix : uniforms->ffpBlendWorldView) digestValue(summary_.digest, matrix.m);
        for (const auto& matrix : uniforms->ffpBlendNormalMatrix) digestValue(summary_.digest, matrix.m);
        for (const auto& matrix : uniforms->textureTransforms) digestValue(summary_.digest, matrix.m);
        digestValue(summary_.digest, uniforms->clipPlaneMask);
        digestValue(summary_.digest, uniforms->clipPlanes);
        digestValue(summary_.digest, uniforms->vertexFloatConstantCount);
        digestValue(summary_.digest, uniforms->vertexIntConstantCount);
        digestValue(summary_.digest, uniforms->vertexBoolConstantCount);
        digestValue(summary_.digest, uniforms->pixelFloatConstantCount);
        digestValue(summary_.digest, uniforms->pixelIntConstantCount);
        digestValue(summary_.digest, uniforms->pixelBoolConstantCount);
        if (summary_.drawObservations.size() >=
            dxmt9::d3d9::kRenderTapeCpuGrammarMaxCommandRecords) {
          failure_ = "draw-observation-capacity-exhausted";
          return;
        }
        summary_.drawObservations.push_back({
            .ordinal = static_cast<std::uint32_t>(
                summary_.drawObservations.size() + 1u),
            .publicationEvent = eventOrdinal_,
            .cumulativeSemanticDigest = summary_.digest});
      }
    }
  }

  struct Object {
    D9CWireObjectIdentity identity{};
    D9CBuffer* buffer = nullptr;
    dxmt9::core::Handle handle{};
    std::uint64_t contentDigest = 1469598103934665603ull;
  };
  struct Pending {
    dxmt9::d3d9::RenderTapeCompletionSchedule schedule{};
    dxmt9::d3d9::RenderTapeCompletionSchedule::Key key{};
    dxmt9::CommandQueueArenaLeaseTestAccess::CompletionResult submission{};
  };

  std::vector<Object>::iterator find(const D9CWireObjectIdentity& identity) {
    return std::find_if(objects_.begin(), objects_.end(),
                        [&](const Object& object) {
                          return object.identity.kind == identity.kind &&
                                 object.identity.generation == identity.generation &&
                                 object.identity.objectId == identity.objectId;
                        });
  }
  std::optional<std::span<const std::byte>> blobsByDigest(
      const dxmt9::d3d9::RenderTapeDigest& digest,
      std::uint64_t size) const {
    for (std::size_t i = 0; i < catalogue_.blobs.size(); ++i) {
      if (catalogue_.blobs[i].digest == digest &&
          catalogue_.blobs[i].size == size) {
        return std::span<const std::byte>(blobs_[i]);
      }
    }
    return std::nullopt;
  }
  void releasePending() {
    auto phase = phaseLedger_.scoped(CpuTapePhase::Completion);
    while (!pending_.empty()) {
      auto& pending = pending_.front();
      if (!pending.schedule.release(pending.key, [&] {
            if (!faultTriggered_ && fault_ == "before-release") {
              faultTriggered_ = true;
              summary_.faultTriggered = true;
              failure_ = "fault-before-release";
              return false;
            }
            const bool completed =
                dxmt9::CommandQueueArenaLeaseTestAccess::acknowledgeOne(
                    fixture_.routing->queue_);
            if (completed) {
              ++summary_.completed;
              const auto source = pending.key;
              const auto& completion = pending.submission.completion;
              const auto trace = std::find_if(
                  summary_.trace.begin(), summary_.trace.end(),
                  [&](const auto& row) {
                    if (row.iteration != source.iteration ||
                        row.submission != source.submission) {
                      return false;
                    }
                    if (completion.source.valid()) {
                      return row.source == completion.source.id.index &&
                             row.sourceGeneration ==
                                 completion.source.id.generation &&
                             row.storageFirstPage ==
                                 completion.source.storage.firstPage &&
                             row.storagePageCount ==
                                 completion.source.storage.pageCount &&
                             row.storageGeneration ==
                                 completion.source.storage.generation;
                    }
                    return row.source == source.source &&
                           row.sourceGeneration == source.sourceGeneration;
                  });
              if (trace != summary_.trace.end()) {
                trace->completed = true;
                trace->releasedAtEvent = eventOrdinal_;
              }
              if (dxmt9::CommandQueueArenaLeaseTestAccess::finishQueue(
                      fixture_.routing->queue_)) {
                ++summary_.reclaimed;
                dxmt9::CommandQueueArenaLeaseTestAccess::reclaimCompletedPool(fixture_.routing->queue_);
              } else {
                failure_ = "source-reclaim-failed";
                return false;
              }
            }
            return completed;
          }).accepted()) {
        if (failure_.empty()) failure_ = "completion-release-rejected";
        return;
      }
      if (!pending.schedule.finishIteration().accepted()) {
        failure_ = "completion-schedule-finish-rejected";
        return;
      }
      pending_.erase(pending_.begin());
    }
  }

  RuntimeFixture& fixture_;
  const dxmt9::d3d9::RenderTapeBlobCatalogue& catalogue_;
  const std::vector<std::vector<std::byte>>& blobs_;
  bool compatibility_ = false;
  bool afterDestroy_ = false;
  bool metalResources_ = false;
  bool managedAllocationFault_ = false;
  bool faultAttempted_ = false;
  bool dynamicResourceMode_ = false;
  std::size_t dynamicMutationCount_ = 0u;
  std::size_t dynamicDrawCount_ = 0u;
  bool capturedARequiredRetention_ = false;
  std::uint64_t capturedAContentsAddress_ = 0u;
  std::uint64_t capturedAMetalHandle_ = 0u;
  std::uint64_t capturedARevision_ = 0u;
  std::uint64_t capturedASubmissionSeq_ = 0u;
  std::vector<std::uint8_t> dynamicSeedBytes_;
  std::vector<std::uint8_t> dynamicExpectedBytes_;
  std::uint64_t iteration_ = 0;
  std::string fault_ = "none";
  std::size_t maxPending_ =
      dxmt9::d3d9::kRenderTapeCompletionScheduleCapacity;
  bool faultTriggered_ = false;
  CpuTapeExperimentSummary& summary_;
  CpuTapePhaseLedger<std::chrono::steady_clock>& phaseLedger_;
  const dxmt9::d3d9::RenderTapeCpuPublicationPlan* publicationPlan_ = nullptr;
  bool groupedActive_ = false;
  bool groupedObservation_ = false;
  dxmt9::d3d9::RenderTapeCpuPublicationRange groupedExpectedRange_{};
  std::uint32_t groupedFirstEvent_ = 0u;
  std::uint32_t groupedLastEvent_ = 0u;
  std::uint32_t groupedCommandEvents_ = 0u;
  std::uint32_t groupedSourceCount_ = 0u;
  std::uint32_t groupedObservedDraws_ = 0u;
  dxmt9::d3d9::RenderTapeCompletionPlanController* externalPlan_ = nullptr;
  std::vector<Object> objects_;
  std::vector<dxmt9::core::Handle> retiredHandles_;
  std::vector<Pending> pending_;
  std::vector<std::byte> bootstrap_;
  std::uint32_t bootstrapCount_ = 0;
  std::uint32_t eventOrdinal_ = 0u;
  std::string failure_;
};

bool readBinaryFile(const std::string& path, std::vector<std::byte>& out) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size < 0) return false;
  input.seekg(0, std::ios::beg);
  out.resize(static_cast<std::size_t>(size));
  return out.empty() ||
         static_cast<bool>(input.read(reinterpret_cast<char*>(out.data()),
                                      static_cast<std::streamsize>(out.size())));
}

bool readBoundedBinaryFile(const std::string& path, std::size_t maximum,
                           std::vector<std::byte>& out) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return false;
  const auto size = input.tellg();
  if (size < 0 || static_cast<std::uintmax_t>(size) > maximum) return false;
  input.seekg(0, std::ios::beg);
  out.resize(static_cast<std::size_t>(size));
  return out.empty() ||
         static_cast<bool>(input.read(reinterpret_cast<char*>(out.data()),
                                      static_cast<std::streamsize>(out.size())));
}

std::string renderTapeDigestHex(const dxmt9::d3d9::RenderTapeDigest& digest) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2u);
  for (const auto value : digest) {
    const auto byte = std::to_integer<unsigned>(value);
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 15u]);
  }
  return result;
}

bool isSha256Hex(std::string_view value) noexcept {
  if (value.size() != 64u) return false;
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

bool isNonzeroLowerSha256Hex(std::string_view value) noexcept {
  if (!isSha256Hex(value)) return false;
  return std::any_of(value.begin(), value.end(),
                     [](char character) { return character != '0'; });
}

void generalCpuCaptureSourcePlanUsesAuthenticatedExecutor() {
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  namespace replay = dxmt9::d3d9;
  using replay::RenderTapeIdentityRange;
  using replay::RenderTapeIdentitySettlement;
  using replay::RenderTapeIdentitySource;

  const auto fixtureTape = dxmt9::d3d9::test::makeGeneralCpuDestroyFixture(
      true, true, dxmt9::d3d9::test::GeneralCpuMutationFixture::None,
      dxmt9::d3d9::test::GeneralCpuResetFixture::A, 1u);
  std::vector<dxmt9::d3d9::RenderTapeProviderBlob> blobs;
  for (const auto& bytes : fixtureTape.blobs)
    blobs.push_back({dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes), bytes});
  runtime::GeneralPreflightPlan plan;
  check(runtime::buildGeneralReplayPlan(fixtureTape.tape, blobs, plan).complete(),
        "capture-source executor fixture preflights");

  constexpr std::uint64_t captureToken = 0x5a17u;
  std::vector<RenderTapeIdentitySource> sources;
  std::vector<RenderTapeIdentityRange> ranges;
  std::vector<RenderTapeIdentitySettlement> settlements;
  for (const auto& event : plan.events) {
    if (event.type != dxmt9::d3d9::RenderTapeEventType::CommandChunk) continue;
    const auto& chunk = plan.commandChunks[event.index];
    const auto eventOrdinal = plan.tape.events[event.eventIndex].ordinal;
    const auto sourceOrdinal = static_cast<std::uint64_t>(sources.size() + 1u);
    const auto seqId = sourceOrdinal;
    const auto firstRange = static_cast<std::uint32_t>(ranges.size());
    sources.push_back({.eventOrdinal = eventOrdinal,
                       .sourceOrdinal = sourceOrdinal,
                       .seqId = seqId,
                       .captureToken = captureToken,
                       .firstRecord = 0u,
                       .recordCount = chunk.envelope.recordCount,
                       .firstRange = firstRange,
                       .rangeCount = 1u});
    ranges.push_back({.eventOrdinal = eventOrdinal,
                      .sourceOrdinal = sourceOrdinal,
                      .seqId = seqId,
                      .logicalPassId = sourceOrdinal,
                      .firstRecord = 0u,
                      .recordCount = chunk.envelope.recordCount,
                      .dagPassIndex = 0u,
                      .passKind = 1u});
    settlements.push_back({.eventOrdinal = eventOrdinal,
                           .rawOrdinal = sourceOrdinal,
                           .buildGeneration = 1u,
                           .firstSourceOrdinal = sourceOrdinal,
                           .tailSeqId = seqId,
                           .sourceCount = 1u});
  }
  replay::RenderTapeIdentityValidationResult identityValidation{};
  const auto sidecar = dxmt9::d3d9::buildRenderTapeIdentity(
      fixtureTape.tape, plan.catalogue, 1u, plan.presentOrdinal, captureToken,
      dxmt9::d3d9::RenderTapeIdentityAuthority::Capture, sources, ranges,
      &identityValidation, settlements);
  check(identityValidation.valid() && !sidecar.empty(),
        "capture-source executor identity sidecar authenticates");
  dxmt9::d3d9::CaptureSourceReplayPlan sourcePlan;
  check(dxmt9::d3d9::buildCaptureSourceReplayPlan(
            fixtureTape.tape, plan, sidecar, sourcePlan) ==
            dxmt9::d3d9::CaptureSourceReplayPlanStatus::Valid,
        "capture-source executor source plan authenticates");

  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  if (resourceDevice.handle == NULL_OBJECT_HANDLE) return;
  PresentParameters output{};
  output.backBufferWidth = plan.outputDesc.width;
  output.backBufferHeight = plan.outputDesc.height;
  output.backBufferFormat = dxmt9::d3d9::devicec::fmtFromD3D(plan.outputDesc.format);
  output.backBufferCount = 1u;
  output.windowed = true;
  output.deviceWindow = Handle{91};
  output.presentationInterval = PresentInterval::Immediate;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<
      RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
  for (const auto policy : {dxmt9::d3d9::ProviderReplayPolicy::Compatibility,
                            dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot}) {
    RuntimeFixture fixture(false, false, true, false,
                           WMT::Device{resourceDevice.handle}, nullptr, &output);
    Executor executor(fixture, plan, blobs,
                      {.route = policy,
                       .captureSourcePlan = &sourcePlan,
                       .captureSourceTape = fixtureTape.tape,
                       .captureSourceIdentitySidecar = sidecar});
    const auto result = executor.run();
    check(result.complete && result.resetOk,
          "authenticated capture-source executor completes both routes");
    check(result.captureSourcePlanDigest == sourcePlan.digest() &&
              result.captureSourcePlanRanges == sources.size() &&
              result.captureSourcePlanPublications >= result.captureSourcePlanRanges,
          "capture-source executor reports authenticated ranges and receipts");
  }

  const auto staleTape = dxmt9::d3d9::test::makeGeneralCpuDestroyFixture(
      true, true, dxmt9::d3d9::test::GeneralCpuMutationFixture::None,
      dxmt9::d3d9::test::GeneralCpuResetFixture::B, 1u);
  std::vector<dxmt9::d3d9::RenderTapeProviderBlob> staleBlobs;
  for (const auto& bytes : staleTape.blobs)
    staleBlobs.push_back({dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes), bytes});
  runtime::GeneralPreflightPlan stalePlanInput;
  check(runtime::buildGeneralReplayPlan(staleTape.tape, staleBlobs,
                                        stalePlanInput).complete(),
        "stale capture-source fixture preflights");
  std::vector<RenderTapeIdentitySource> staleSources;
  std::vector<RenderTapeIdentityRange> staleRanges;
  std::vector<RenderTapeIdentitySettlement> staleSettlements;
  for (const auto& event : stalePlanInput.events) {
    if (event.type != dxmt9::d3d9::RenderTapeEventType::CommandChunk) continue;
    const auto& chunk = stalePlanInput.commandChunks[event.index];
    const auto eventOrdinal = stalePlanInput.tape.events[event.eventIndex].ordinal;
    const auto sourceOrdinal = static_cast<std::uint64_t>(staleSources.size() + 1u);
    staleSources.push_back({.eventOrdinal = eventOrdinal,
                            .sourceOrdinal = sourceOrdinal,
                            .seqId = sourceOrdinal,
                            .captureToken = captureToken,
                            .firstRecord = 0u,
                            .recordCount = chunk.envelope.recordCount,
                            .firstRange = static_cast<std::uint32_t>(staleRanges.size()),
                            .rangeCount = 1u});
    staleRanges.push_back({.eventOrdinal = eventOrdinal,
                           .sourceOrdinal = sourceOrdinal,
                           .seqId = sourceOrdinal,
                           .logicalPassId = sourceOrdinal,
                           .firstRecord = 0u,
                           .recordCount = chunk.envelope.recordCount,
                           .dagPassIndex = 0u,
                           .passKind = 1u});
    staleSettlements.push_back({.eventOrdinal = eventOrdinal,
                                .rawOrdinal = sourceOrdinal,
                                .buildGeneration = 1u,
                                .firstSourceOrdinal = sourceOrdinal,
                                .tailSeqId = sourceOrdinal,
                                .sourceCount = 1u});
  }
  replay::RenderTapeIdentityValidationResult staleValidation{};
  const auto staleSidecar = dxmt9::d3d9::buildRenderTapeIdentity(
      staleTape.tape, stalePlanInput.catalogue, 1u, stalePlanInput.presentOrdinal,
      captureToken, dxmt9::d3d9::RenderTapeIdentityAuthority::Capture,
      staleSources, staleRanges, &staleValidation, staleSettlements);
  dxmt9::d3d9::CaptureSourceReplayPlan staleSourcePlan;
  check(staleValidation.valid() &&
            dxmt9::d3d9::buildCaptureSourceReplayPlan(
                staleTape.tape, stalePlanInput, staleSidecar, staleSourcePlan) ==
                dxmt9::d3d9::CaptureSourceReplayPlanStatus::Valid,
        "stale capture-source plan authenticates independently");
  RuntimeFixture rejectedFixture(false, false, true, false,
                                WMT::Device{resourceDevice.handle}, nullptr, &output);
  Executor rejected(rejectedFixture, plan, blobs,
                    {.route = dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot,
                     .captureSourcePlan = &staleSourcePlan,
                     .captureSourceTape = fixtureTape.tape,
                     .captureSourceIdentitySidecar = sidecar});
  const auto rejectedResult = rejected.run();
  check(!rejectedResult.complete && rejectedResult.submitted == 0u &&
            rejectedResult.failure == "general-executor-capture-source-binding",
        "stale source plan cannot bind by range counts alone");
}

void generalCpuCaptureSourceV3PreservesCrossEventWriter(bool sharedDrawRun = false) {
  namespace replay = dxmt9::d3d9;
  namespace runtime = replay::render_tape_runtime;
  using Kind = replay::RenderTapeIdentityV3DispositionKind;
  using Command = replay::RenderTapeIdentityV3CommandKind;
  using Membership = replay::RenderTapeIdentityV3Membership;
  using Cut = replay::RenderTapeIdentityV3CutKind;

  auto tape = replay::test::makeGeneralCpuDestroyFixture(
      true, true, replay::test::GeneralCpuMutationFixture::None,
      replay::test::GeneralCpuResetFixture::A, sharedDrawRun ? 0u : 1u);
  if (sharedDrawRun) {
    replay::ImportedRenderTapeView imported;
    check(replay::importPrevalidatedRenderTape(tape.tape, imported),
          "shared DrawRun fixture imports original event bytes");
    const auto drawBytes = replay::test::general_cpu_destroy_fixture_detail::indexedDrawChunk();
    replay::ImportedChunkView draw;
    check(replay::importPrevalidatedCommandChunk(drawBytes,
              {.recordCount = 1u, .handleCount = 5u}, draw),
          "shared DrawRun fixture imports exact indexed draw");
    const auto drawPayload = draw.record(0u).payload;
    const D9CCommandChunkWireSetConst constants{.registerCount = 1u};
    std::vector<std::byte> constantPayload(sizeof(constants) + 4u * sizeof(float));
    std::memcpy(constantPayload.data(), &constants, sizeof(constants));
    std::vector<std::byte> secondDraw(drawPayload.begin(), drawPayload.end());
    D9CCommandChunkWireDrawHeader secondHeader{};
    std::memcpy(&secondHeader, secondDraw.data(), sizeof(secondHeader));
    // Canonical wire records own disjoint handle slices, even when the two
    // draws resolve to the same resources and merge into one physical run.
    for (std::uint32_t index = 0u; index < secondHeader.sectionCount; ++index) {
      D9CCommandChunkWireSectionDesc section{};
      std::memcpy(&section, secondDraw.data() + secondHeader.sectionTableOffset +
                      index * sizeof(section), sizeof(section));
      std::size_t handleOffset = 0u;
      switch (section.kind) {
      case D9C_COMMAND_CHUNK_SECTION_TEXTURE:
        handleOffset = offsetof(D9CCommandChunkWireTextureBinding, handleIndex); break;
      case D9C_COMMAND_CHUNK_SECTION_STREAM:
        handleOffset = offsetof(D9CCommandChunkWireStreamBinding, handleIndex); break;
      case D9C_COMMAND_CHUNK_SECTION_SHADER:
        handleOffset = offsetof(D9CCommandChunkWireShaderBinding, handleIndex); break;
      case D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT:
        handleOffset = offsetof(D9CCommandChunkWireVertexInput, handleIndex); break;
      case D9C_COMMAND_CHUNK_SECTION_INDEX_BUFFER:
        handleOffset = offsetof(D9CCommandChunkWireIndexBinding, handleIndex); break;
      case D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET:
        handleOffset = offsetof(D9CCommandChunkWireRenderTargetBinding, handleIndex); break;
      default: throw TestFailure("shared DrawRun fixture has an unexpected binding section");
      }
      for (std::uint32_t element = 0u; element < section.count; ++element) {
        auto* bytes = secondDraw.data() + section.payloadOffset +
            element * section.elementSize + handleOffset;
        std::uint32_t handle = 0u;
        std::memcpy(&handle, bytes, sizeof(handle));
        if (handle == D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX) continue;
        handle += static_cast<std::uint32_t>(draw.handles.size());
        std::memcpy(bytes, &handle, sizeof(handle));
      }
    }
    const std::vector<D9CCommandChunkWireHandleEntry> drawHandles(
        draw.handles.begin(), draw.handles.end());
    using Record = replay::test::general_cpu_destroy_fixture_detail::Record;
    const std::array records{
        Record{.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
            .payload = {drawPayload.begin(), drawPayload.end()}, .handles = drawHandles},
        Record{.type = D9C_COMMAND_RECORD_SET_VS_CONST_F, .payload = constantPayload},
        Record{.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
            .payload = std::move(secondDraw), .handles = drawHandles}};
    const auto chunk = replay::test::general_cpu_destroy_fixture_detail::makeChunk(records);
    replay::RenderTapeBuilder builder;
    bool replaced = false;
    for (std::size_t index = 0u; index < imported.events.size(); ++index) {
      const auto event = imported.event(index);
      const auto type = static_cast<replay::RenderTapeEventType>(event.header.type);
      if (!replaced && type == replay::RenderTapeEventType::CommandChunk) {
        builder.appendCommandChunk({.recordCount = 3u, .handleCount = 10u}, chunk);
        replaced = true;
      } else {
        builder.appendRawEvent(type, event.payload);
      }
    }
    tape.tape = builder.seal();
  }
  std::vector<replay::RenderTapeProviderBlob> blobs;
  for (const auto& bytes : tape.blobs)
    blobs.push_back({replay::RenderTapeCaptureSession::sha256(bytes), bytes});
  runtime::GeneralPreflightPlan plan;
  check(runtime::buildGeneralReplayPlan(tape.tape, blobs, plan).complete(),
        "v3 cross-event writer fixture preflights");

  replay::RenderTapeIdentityV3View identity;
  identity.header.frameId = 1u;
  identity.header.presentOrdinal = plan.presentOrdinal;
  identity.header.captureToken = 0x5a18u;
  identity.header.captureEpoch = 6u;
  identity.header.queue = 7u;
  identity.header.baselineSequence = 100u;
  identity.header.baselineCompletedSequence = 100u;
  identity.header.baselineInitializer = 17u;
  identity.header.baselineCompletedInitializer = 17u;
  identity.header.initializerEpoch = 7u;
  identity.header.baselineCompletionEdge = 1u;
  identity.header.baselineKind = static_cast<std::uint32_t>(
      replay::RenderTapeIdentityV3PrefixKind::ArmBaseline);
  std::uint64_t drawTailEvent = 0u;
  std::uint32_t draws = 0u;
  for (const auto& event : plan.events) {
    if (event.type != replay::RenderTapeEventType::CommandChunk) continue;
    const auto ordinal = plan.tape.event(event.eventIndex).header.ordinal;
    replay::ImportedChunkView chunk;
    const auto& original = plan.commandChunks[event.index];
    check(replay::importPrevalidatedCommandChunk(original.bytes, original.envelope, chunk),
          "v3 fixture retains each original full raw");
    const bool present = chunk.records[0].type == D9C_COMMAND_RECORD_PRESENT;
    const auto commandIndex = static_cast<std::uint32_t>(identity.commands.size());
    identity.commands.push_back({.logicalPassId = present ? 32u : 31u,
        .sourceIndex = present ? 1u : 0u, .physicalCommandIndex = present ? 0u : draws,
        .kind = static_cast<std::uint32_t>(present ? Command::Present : Command::DrawRun),
        .drawCount = present ? 0u : (sharedDrawRun ? 2u : 1u),
        .membership = static_cast<std::uint32_t>(Membership::Pass),
        .dagPassIndex = 0u, .passKind = present ? 4u : 1u});
    identity.dispositions.push_back({.eventOrdinal = ordinal, .recordCount = 1u,
        .kind = static_cast<std::uint32_t>(Kind::GpuCommand),
        .commandIndex = commandIndex, .subdrawCount = present ? 0u : 1u});
    if (!present && sharedDrawRun) {
      identity.dispositions.push_back({.eventOrdinal = ordinal, .firstRecord = 1u,
          .recordCount = 1u, .kind = static_cast<std::uint32_t>(Kind::StateOnly)});
      identity.dispositions.push_back({.eventOrdinal = ordinal, .firstRecord = 2u,
          .recordCount = 1u, .kind = static_cast<std::uint32_t>(Kind::GpuCommand),
          .commandIndex = commandIndex, .firstSubdraw = 1u, .subdrawCount = 1u});
    }
    identity.settlements.push_back({.eventOrdinal = ordinal,
        .rawOrdinal = 200u + identity.settlements.size(), .buildGeneration = 3u,
        .replayEdge = ordinal * 100u + 99u,
        .recordCount = static_cast<std::uint32_t>(chunk.records.size())});
    if (!present) {
      draws += sharedDrawRun ? 2u : 1u;
      drawTailEvent = ordinal;
    }
  }
  check(draws == 2u && identity.commands.size() == (sharedDrawRun ? 2u : 3u),
        "v3 fixture maps canonical draws to exact physical commands");
  const auto appendSource = [&](std::uint32_t index, std::uint64_t event,
                                Cut cut, std::uint32_t record, std::uint32_t commands) {
    identity.sources.push_back({.sourceOrdinal = 70u + index, .seqId = 101u + index,
        .writerCaptureId = 80u + index, .writerGeneration = 90u + index,
        .storageGeneration = 100u + index, .publicationEdge = event * 100u + 10u,
        .submissionEdge = event * 100u + 11u, .completionEdge = event * 100u + 12u,
        .completedSequence = 101u + index, .cutEventOrdinal = event,
        .commandCount = commands, .cutKind = static_cast<std::uint32_t>(cut),
        .cutRecord = record});
  };
  appendSource(0u, drawTailEvent, Cut::AfterEvent, 0u, sharedDrawRun ? 1u : 2u);
  appendSource(1u, plan.presentOrdinal, Cut::AfterRecord, 1u, 1u);
  replay::RenderTapeIdentityV3Status identityStatus{};
  const auto sidecar = replay::buildRenderTapeIdentityV3(
      tape.tape, plan.catalogue, identity, &identityStatus);
  check(identityStatus == replay::RenderTapeIdentityV3Status::Valid && !sidecar.empty(),
        "v3 cross-event writer sidecar fully authenticates");
  replay::CaptureSourceReplayPlanV3 sourcePlan;
  check(replay::buildCaptureSourceReplayPlanV3(tape.tape, plan.catalogue, sidecar,
            sourcePlan) == replay::CaptureSourceReplayPlanV3Status::Valid,
        "v3 original-tape source plan authenticates");

  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  if (resourceDevice.handle == NULL_OBJECT_HANDLE) return;
  PresentParameters output{};
  output.backBufferWidth = plan.outputDesc.width;
  output.backBufferHeight = plan.outputDesc.height;
  output.backBufferFormat = replay::devicec::fmtFromD3D(plan.outputDesc.format);
  output.backBufferCount = 1u;
  output.windowed = true;
  output.deviceWindow = Handle{91};
  output.presentationInterval = PresentInterval::Immediate;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<
      RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
  RuntimeFixture fixture(false, false, true, false,
                         WMT::Device{resourceDevice.handle}, nullptr, &output);
  Executor executor(fixture, plan, blobs,
                    {.route = replay::ProviderReplayPolicy::Compatibility,
                     .captureSourcePlanV3 = &sourcePlan,
                     .captureSourceTape = tape.tape,
                     .captureSourceIdentitySidecar = sidecar});
  const auto result = executor.run();
  if (!result.complete)
    std::cerr << "v3 cross-event execution failure=" << result.failure
              << " event=" << result.failedEvent << '\n';
  check(result.complete && result.resetOk && result.submitted == 2u &&
            result.completed == 2u && result.draws == 2u,
        "v3 compatibility preserves one ordinary writer and two canonical draws");
  const auto& evidence = result.captureSourceReplayV3;
  check(evidence.complete && evidence.lifecycleComplete && evidence.compatibility &&
            evidence.qualifierOnly && !evidence.timingEligible &&
            evidence.dispatchedRecords == (sharedDrawRun ? 4u : 3u) &&
            evidence.settlements.size() == (sharedDrawRun ? 2u : 3u) &&
            evidence.publications.size() == 2u && evidence.externalPublicationCuts == 1u &&
            evidence.publications[0].commandCount == (sharedDrawRun ? 1u : 2u) &&
            evidence.publications[0].eventOrdinal == drawTailEvent &&
            !evidence.publications[0].hasPresent && evidence.publications[1].hasPresent,
        "v3 reports actual receipts, complete lifecycle and original settlements");
  check(evidence.publications[0].localSequence != identity.sources[0].seqId &&
            evidence.publications[1].localSequence != identity.sources[1].seqId,
        "captured completion labels remain provenance rather than local queue authority");

  if (!sharedDrawRun) {
    for (const auto fault : {
             Executor::ContinuationIdentityFault::StaleWriterGeneration,
             Executor::ContinuationIdentityFault::ForeignQueue}) {
      RuntimeFixture foreign(false, false, true, false,
                             WMT::Device{resourceDevice.handle}, nullptr, &output);
      Executor rejectedContinuation(foreign, plan, blobs,
                                    {.route = replay::ProviderReplayPolicy::Compatibility,
                                     .continuationIdentityFault = fault,
                                     .captureSourcePlanV3 = &sourcePlan,
                                     .captureSourceTape = tape.tape,
                                     .captureSourceIdentitySidecar = sidecar});
      const auto foreignResult = rejectedContinuation.run();
      check(!foreignResult.complete &&
                foreignResult.failure == "general-executor-nonquiescent-chunk-entry" &&
                foreignResult.draws == 1u && foreignResult.resourcesCreated != 0u &&
                foreignResult.resourcesReleased == foreignResult.resourcesCreated &&
                !foreignResult.captureSourceReplayV3.complete,
            "v3 cross-event resident admission rejects stale or foreign writer identity");
    }

    // A valid authenticated sidecar may still disagree with the actual
    // execution topology. Reject only after the ordinary replay has exercised
    // the armed observer, and keep the normal executor teardown in the path.
    auto wrongTopology = identity;
    for (auto& command : wrongTopology.commands) {
      if (command.kind == static_cast<std::uint32_t>(Command::DrawRun))
        command.passKind = 2u;
    }
    const auto wrongSidecar = replay::buildRenderTapeIdentityV3(
        tape.tape, plan.catalogue, wrongTopology, &identityStatus);
    replay::CaptureSourceReplayPlanV3 wrongPlan;
    check(identityStatus == replay::RenderTapeIdentityV3Status::Valid &&
              replay::buildCaptureSourceReplayPlanV3(tape.tape, plan.catalogue,
                  wrongSidecar, wrongPlan) == replay::CaptureSourceReplayPlanV3Status::Valid,
          "v3 teardown regression authenticates a distinct expected pass topology");
    RuntimeFixture failed(false, false, true, false,
                          WMT::Device{resourceDevice.handle}, nullptr, &output);
    Executor rejectedTopology(failed, plan, blobs,
                              {.route = replay::ProviderReplayPolicy::Compatibility,
                               .captureSourcePlanV3 = &wrongPlan,
                               .captureSourceTape = tape.tape,
                               .captureSourceIdentitySidecar = wrongSidecar});
    const auto failedResult = rejectedTopology.run();
    check(!failedResult.complete &&
              failedResult.failure == "v3-execution-topology" &&
              failedResult.resourcesCreated != 0u &&
              failedResult.resourcesReleased == failedResult.resourcesCreated &&
              failedResult.submitted == 2u && failedResult.completed == 2u &&
              !failedResult.captureSourceReplayV3.complete,
          "armed v3 topology rejection settles real sources and releases tape owners");
    failed.releaseCoreForInspection();
    failed.releaseCoreForInspection();
    check(!failed.cDevice && !failed.device && !failed.factory,
          "failed v3 executor teardown is idempotent after core owner release");

    RuntimeFixture fresh(false, false, true, false,
                         WMT::Device{resourceDevice.handle}, nullptr, &output);
    Executor freshExecutor(fresh, plan, blobs,
                           {.route = replay::ProviderReplayPolicy::Compatibility,
                            .captureSourcePlanV3 = &sourcePlan,
                            .captureSourceTape = tape.tape,
                            .captureSourceIdentitySidecar = sidecar});
    const auto freshResult = freshExecutor.run();
    check(freshResult.complete && freshResult.resetOk &&
              freshResult.submitted == 2u && freshResult.completed == 2u &&
              freshResult.draws == 2u && freshResult.captureSourceReplayV3.complete,
          "fresh v3 execution succeeds after an armed rejected session teardown");
  }

  RuntimeFixture production(false, false, true, false,
                            WMT::Device{resourceDevice.handle}, nullptr, &output);
  Executor rejectedRoute(production, plan, blobs,
                         {.route = replay::ProviderReplayPolicy::ProductionSlot,
                          .captureSourcePlanV3 = &sourcePlan,
                          .captureSourceTape = tape.tape,
                          .captureSourceIdentitySidecar = sidecar});
  const auto rejected = rejectedRoute.run();
  check(!rejected.complete && rejected.resourcesCreated == 0u && rejected.submitted == 0u &&
            rejected.failure == "general-executor-v3-capture-source-admission",
        "v3 ProductionSlot rejects explicitly before any tape effects");

  ++identity.header.captureToken;
  const auto staleSidecar = replay::buildRenderTapeIdentityV3(
      tape.tape, plan.catalogue, identity, &identityStatus);
  replay::CaptureSourceReplayPlanV3 stalePlan;
  check(identityStatus == replay::RenderTapeIdentityV3Status::Valid &&
            replay::buildCaptureSourceReplayPlanV3(tape.tape, plan.catalogue, staleSidecar,
                stalePlan) == replay::CaptureSourceReplayPlanV3Status::Valid,
        "independently authenticated stale v3 plan has a distinct immutable binding");
  RuntimeFixture stale(false, false, true, false,
                       WMT::Device{resourceDevice.handle}, nullptr, &output);
  Executor rejectedBinding(stale, plan, blobs,
                           {.route = replay::ProviderReplayPolicy::Compatibility,
                            .captureSourcePlanV3 = &stalePlan,
                            .captureSourceTape = tape.tape,
                            .captureSourceIdentitySidecar = sidecar});
  const auto staleResult = rejectedBinding.run();
  check(!staleResult.complete && staleResult.resourcesCreated == 0u &&
            staleResult.submitted == 0u &&
            staleResult.failure == "general-executor-v3-capture-source-admission",
        "stale v3 digest rejects before object creation or publication");
}

void printCpuTapeLoadedImages() {
  std::cout << '[';
#if defined(__APPLE__)
  bool first = true;
  for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
    const char* name = _dyld_get_image_name(index);
    if (!name) continue;
    const auto basename = std::filesystem::path(name).filename().string();
    if (basename.find("dxmt9") == std::string::npos &&
        basename.find("winemetal") == std::string::npos) continue;
    std::vector<std::byte> bytes;
    if (!readBinaryFile(name, bytes)) throw TestFailure("cannot fingerprint loaded image");
    const auto hash = dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes);
    static constexpr char hex[] = "0123456789abcdef";
    std::string digest;
    for (const auto value : hash) {
      const auto byte = std::to_integer<unsigned>(value);
      digest += hex[byte >> 4];
      digest += hex[byte & 15];
    }
    if (!first) std::cout << ',';
    first = false;
    std::cout << R"({"path":)" << std::quoted(name)
              << R"(,"sha256":)" << std::quoted(digest)
              << R"(,"bytes":)" << bytes.size() << '}';
  }
#endif
  std::cout << ']';
}

void printGeneralCpuInputCaps(
    const dxmt9::d3d9::render_tape_runtime::GeneralCpuAdmissionCaps& caps) {
  std::cout << R"({"max_events":)" << caps.maxEvents
            << R"(,"max_objects":)" << caps.maxObjects
            << R"(,"max_mutations":)" << caps.maxMutations
            << R"(,"max_chunks":)" << caps.maxChunks
            << R"(,"max_bootstrap_chunks":)" << caps.maxChunks
            << R"(,"max_total_chunks":)" << caps.maxTotalChunks
            << R"(,"max_records":)" << caps.maxRecords
            << R"(,"max_bootstrap_records":)" << caps.maxRecords
            << R"(,"max_total_records":)" << caps.maxTotalRecords
            << R"(,"max_records_per_chunk":)" << caps.maxRecordsPerChunk
            << R"(,"max_draws":)" << caps.maxDraws
            << R"(,"max_blobs":)" << caps.maxBlobs
            << R"(,"max_blob_bytes":)" << caps.maxBlobBytes
            << R"(,"max_query_definitions":)" << caps.maxQueryDefinitions
            << R"(,"max_query_issues":)" << caps.maxQueryIssues
            << R"(,"max_query_polls":)" << caps.maxQueryPolls << '}';
}

void printGeneralCpuInputCounts(
    const dxmt9::d3d9::render_tape_runtime::GeneralCpuAdmissionCounts& counts) {
  // The public receipt validator authenticates this fixed coverage projection.
  // Detailed budget counters are disclosed separately below.
  std::cout << R"({"events":)" << counts.events
            << R"(,"objects":)" << counts.objects
            << R"(,"mutations":)" << counts.mutations
            << R"(,"chunks":)" << counts.chunks
            << R"(,"records":)" << counts.records
            << R"(,"draws":)" << counts.draws
            << R"(,"query_definitions":)" << counts.queryDefinitions
            << R"(,"query_issues":)" << counts.queryIssues
            << R"(,"query_polls":)" << counts.queryPolls << '}';
}

void printGeneralCpuInputAdmissionCounts(
    const dxmt9::d3d9::render_tape_runtime::GeneralCpuAdmissionCounts& counts) {
  std::cout << R"({"events":)" << counts.events
            << R"(,"objects":)" << counts.objects
            << R"(,"mutations":)" << counts.mutations
            << R"(,"chunks":)" << counts.chunks
            << R"(,"bootstrap_chunks":)" << counts.bootstrapChunks
            << R"(,"total_chunks":)" << counts.chunks + counts.bootstrapChunks
            << R"(,"records":)" << counts.records
            << R"(,"bootstrap_records":)" << counts.bootstrapRecords
            << R"(,"total_records":)" << counts.records + counts.bootstrapRecords
            << R"(,"draws":)" << counts.draws
            << R"(,"blobs":)" << counts.blobs
            << R"(,"blob_bytes":)" << counts.blobBytes
            << R"(,"query_definitions":)" << counts.queryDefinitions
            << R"(,"query_issues":)" << counts.queryIssues
            << R"(,"query_polls":)" << counts.queryPolls
            << R"(,"query_checkpoints":)" << counts.queryCheckpoints << '}';
}

void printCpuInputAdmissionSelection(
    std::string_view profile,
    const dxmt9::d3d9::render_tape_runtime::GeneralCpuAdmissionCaps* caps,
    const dxmt9::d3d9::render_tape_runtime::GeneralCpuSourceCapacity& sourceCapacity,
    std::uint32_t maxQueryOraclePolls) {
  std::cout << R"(,"input_admission_profile":)" << std::quoted(profile)
            << R"(,"input_admission_caps":)";
  if (caps) printGeneralCpuInputCaps(*caps);
  else std::cout << "null";
  std::cout << R"(,"source_capacity":{"max_publications":)"
            << sourceCapacity.maxPublications
            << R"(,"max_progress_steps":)" << sourceCapacity.maxProgressSteps
            << R"(},"max_query_oracle_polls_per_query":)" << maxQueryOraclePolls;
}

int rejectCpuInputAdmissionProfile(const CpuTapeExperimentOptions& options,
                                   const char* status, const char* reason) {
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<
      RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
  const Executor::Options executorOptions{};
  std::optional<runtime::GeneralCpuAdmissionCaps> caps;
  if (options.inputAdmissionProfile == "default") caps.emplace();
  else if (options.inputAdmissionProfile == "extended")
    caps = runtime::generalCpuExtendedInputCaps();
  std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1","status":)"
            << std::quoted(status) << R"(,"reason":)" << std::quoted(reason);
  printCpuInputAdmissionSelection(options.inputAdmissionProfile, caps ? &*caps : nullptr,
                                 executorOptions.sourceCapacity,
                                 executorOptions.maxQueryOraclePolls);
  std::cout << "}\n";
  return 3;
}

void printCpuQueryReplay(const dxmt9::d3d9::RenderTapeQueryReplayEvidence& evidence,
                         bool measurement, std::string_view eventsSha256) {
  const auto boolean = [](bool value) { return value ? "true" : "false"; };
  const auto bytesHex = [](const auto& bytes, std::uint32_t count) {
    constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (std::size_t index = 0u; index < std::min<std::size_t>(count, bytes.size()); ++index) {
      const auto byte = std::to_integer<unsigned>(bytes[index]);
      hex.push_back(digits[byte >> 4u]);
      hex.push_back(digits[byte & 15u]);
    }
    return hex;
  };
  constexpr std::array<const char*, 8> failures{
      "none", "unsupported-result-kind", "snapshot-unavailable", "cycle-mismatch",
      "poll-failed", "result-mismatch", "progress-exhausted", "backend-unavailable"};
  constexpr std::array<const char*, 3> progressLimits{
      "none", "poll-budget", "wall-budget"};
  std::cout << R"({"schema":"dxmt9.render_tape.query_replay.v1","backend":"cpu-controlled-production-queue","mode":)"
            << std::quoted(measurement ? "measurement" : "qualification")
            << R"(,"events_sha256":)" << std::quoted(eventsSha256)
            << R"(,"present":)" << boolean(evidence.present)
            << R"(,"transcript_complete":)" << boolean(evidence.transcriptComplete)
            << R"(,"oracle_complete":)" << boolean(evidence.oracleComplete)
            << R"(,"actual_metal":false,"gpu_result_oracle":false,"query_definitions":)" << evidence.queryDefinitions
            << R"(,"issue_records":)" << evidence.issueRecords
            << R"(,"replayed_issue_records":)" << evidence.replayedIssueRecords
            << R"(,"recorded_polls":)" << evidence.recordedPolls
            << R"(,"replayed_polls":)" << evidence.replayedPolls
            << R"(,"captured_pending":)" << evidence.capturedPending
            << R"(,"replay_pending":)" << evidence.replayPending
            << R"(,"captured_ready":)" << evidence.capturedReady
            << R"(,"replay_ready":)" << evidence.replayReady
            << R"(,"required_results":)" << evidence.requiredResults
            << R"(,"matched_results":)" << evidence.matchedResults
            << R"(,"readiness_required":)" << evidence.readinessRequired
            << R"(,"readiness_satisfied":)" << evidence.readinessSatisfied
            << R"(,"substituted_results":)" << evidence.substitutedResults
            << R"(,"supplemental_polls":)" << evidence.supplementalPolls
            << R"(,"supplemental_drains":)" << evidence.supplementalDrains
            << R"(,"checkpoint_definitions":)" << evidence.checkpointDefinitions
            << R"(,"checkpoint_setup_ends":)" << evidence.checkpointSetupEnds
            << R"(,"checkpoint_setup_publications":)" << evidence.checkpointSetupPublications
            << R"(,"checkpoint_setup_polls":)" << evidence.checkpointSetupPolls
            << R"(,"checkpoint_setup_waits":)" << evidence.checkpointSetupWaits
            << R"(,"checkpoint_completed":)" << evidence.checkpointCompleted
            << R"(,"checkpoint_activated":)" << evidence.checkpointActivated
            << R"(,"checkpoint_setup_wall_ns":)" << evidence.checkpointSetupWallNanos
            << R"(,"transcript_wall_ns":)" << evidence.transcriptWallNanos
            << R"(,"oracle_wall_ns":)" << evidence.oracleWallNanos
            << R"(,"max_oracle_polls":)" << evidence.maxOraclePolls
            << R"(,"max_oracle_wall_ns":)" << evidence.maxOracleWallNanos
            << R"(,"progress_limit":)" << std::quoted(progressLimits[std::min<std::size_t>(
                static_cast<std::size_t>(evidence.progressLimit), progressLimits.size() - 1u)])
            << R"(,"failure":)" << std::quoted(failures[std::min<std::size_t>(
                static_cast<std::size_t>(evidence.failure), failures.size() - 1u)])
            << R"(,"rows":[)";
  for (std::size_t index = 0u; index < evidence.rows.size(); ++index) {
    if (index) std::cout << ',';
    const auto& row = evidence.rows[index];
    std::cout << R"({"event_index":)" << row.eventIndex
              << R"(,"event_ordinal":)" << row.eventOrdinal
              << R"(,"identity":{"kind":)" << row.identity.kind
              << R"(,"generation":)" << row.identity.generation
              << R"(,"object_id":")" << row.identity.objectId << R"("})"
              << R"(,"issue_cycle":)" << row.issueCycle
              << R"(,"flags":)" << row.flags
              << R"(,"has_output":)" << boolean(row.hasOutput)
              << R"(,"data_size":)" << row.dataSize
              << R"(,"captured_result":)" << row.capturedResult
              << R"(,"replay_result":)" << row.replayResult
              << R"(,"oracle_result":)" << row.oracleResult
              << R"(,"defined_bytes":)" << row.definedBytes
              << R"(,"captured_hex":)" << std::quoted(bytesHex(row.capturedBytes, row.definedBytes))
              << R"(,"replay_hex":)" << std::quoted(bytesHex(row.replayBytes, row.definedBytes))
              << R"(,"local_query_identity":)" << row.localQueryIdentity
              << R"(,"local_issue_ordinal":)" << row.localIssueOrdinal
              << R"(,"local_device_epoch":)" << row.localDeviceEpoch
              << R"(,"local_queue":)" << row.localQueue
              << R"(,"local_sequence":)" << row.localSequence
              << R"(,"local_initializer":)" << row.localInitializer
              << R"(,"completed_sequence":)" << row.completedSequence
              << R"(,"completed_initializer":)" << row.completedInitializer
              << R"(,"oracle_polls":)" << row.oraclePolls
              << R"(,"result_required":)" << boolean(row.resultRequired)
              << R"(,"result_matched":)" << boolean(row.resultMatched)
              << R"(,"readiness_satisfied":)" << boolean(row.readinessSatisfied)
              << R"(,"result_substituted":)" << boolean(row.resultSubstituted)
              << R"(,"supplemental":)" << boolean(row.supplemental) << '}';
  }
  std::cout << R"(],"checkpoints":[)";
  for (std::size_t index = 0u; index < evidence.checkpoints.size(); ++index) {
    if (index) std::cout << ',';
    const auto& row = evidence.checkpoints[index];
    std::cout << R"({"identity":{"kind":)" << row.identity.kind
              << R"(,"generation":)" << row.identity.generation
              << R"(,"object_id":")" << row.identity.objectId << R"("})"
              << R"(,"captured_issue_cycle":)" << row.capturedIssueCycle
              << R"(,"captured_issue_ordinal":)" << row.capturedIssueOrdinal
              << R"(,"captured_device_epoch":)" << row.capturedDeviceEpoch
              << R"(,"local_query_identity":)" << row.localQueryIdentity
              << R"(,"local_issue_ordinal":)" << row.localIssueOrdinal
              << R"(,"local_device_epoch":)" << row.localDeviceEpoch
              << R"(,"local_queue":)" << row.localQueue
              << R"(,"local_sequence":)" << row.localSequence
              << R"(,"local_initializer":)" << row.localInitializer
              << R"(,"completed_sequence":)" << row.completedSequence
              << R"(,"completed_initializer":)" << row.completedInitializer
              << R"(,"setup_polls":)" << row.setupPolls
              << R"(,"setup_issued":)" << boolean(row.setupIssued)
              << R"(,"completed":)" << boolean(row.completed)
              << R"(,"activated":)" << boolean(row.activated) << '}';
  }
  std::cout << "]}";
}

int runGeneralCpuTapeExperiment(const CpuTapeExperimentOptions& options) {
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<
      RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
  Executor::Options executorOptions{};
  executorOptions.inputCaps = options.inputAdmissionProfile == "extended"
      ? runtime::generalCpuExtendedInputCaps() : runtime::GeneralCpuAdmissionCaps{};
  std::optional<runtime::GeneralCpuAdmissionResult> inputAdmission;
  const bool captureSourceRequested =
      options.sourceBoundaryMode ==
      CpuTapeExperimentOptions::SourceBoundaryMode::CaptureSource;
  const auto reject = [&](const char* status, const std::string& reason) {
    std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1","grammar":"general-frame","backend":"cpu-metal-resources","status":)"
              << std::quoted(status) << R"(,"reason":)" << std::quoted(reason);
    printCpuInputAdmissionSelection(options.inputAdmissionProfile,
        &executorOptions.inputCaps, executorOptions.sourceCapacity,
        executorOptions.maxQueryOraclePolls);
    if (inputAdmission) {
      std::cout << R"(,"input_counts":)";
      printGeneralCpuInputCounts(inputAdmission->counts);
      std::cout << R"(,"input_admission_counts":)";
      printGeneralCpuInputAdmissionCounts(inputAdmission->counts);
      std::cout << R"(,"input_admission_reason":)"
                << std::quoted(runtime::generalCpuAdmissionReasonName(inputAdmission->reason));
    }
    std::cout << "}\n";
    return 3;
  };
  if (captureSourceRequested &&
      (!options.generalFrame || options.identityPath.empty() ||
       !isNonzeroLowerSha256Hex(options.sourcePlanSha256) ||
       options.beforeNextWork || options.afterDestroy || options.maxPending != 1u ||
       !options.sourceOraclePath.empty() ||
       !options.sourceProjectionSha256.empty())) {
    return reject("unsupported-mode",
                  "capture-source-requires-cold-general-input-and-plan-digest");
  }
  if (captureSourceRequested && (options.measurement || options.policyExplicit) &&
      !dxmt9::test::cpuTapeCaptureSourcePolicyBounded(options.policy)) {
    return reject("unsupported-mode", "capture-source-policy-bound-unproven");
  }
  if (!dxmt9::test::cpuTapePolicyValid(options.policy) ||
      ((options.measurement || options.policyExplicit) &&
       (options.beforeNextWork || options.afterDestroy || options.maxPending != 1u ||
        !options.sourceOraclePath.empty() || !options.sourceProjectionSha256.empty()))) {
    return reject("unsupported-mode", "cpu-policy-requires-immediate-single-pending");
  }
  if (options.resourceBackend != CpuTapeResourceBackend::MetalResources ||
      options.afterDestroy || !options.completionPlanPath.empty() ||
      options.fault != "none" || options.maxPending == 0u || options.maxPending > 2u ||
      (options.maxPending > 1u && !options.beforeNextWork) ||
      options.publication != dxmt9::d3d9::RenderTapeCpuPublicationMode::PerCommand) {
    return reject("unsupported-mode", "general-frame-requires-bounded-resource-backed-execution");
  }
  const bool sourceOracleRequested = !options.sourceOraclePath.empty() ||
      !options.sourceProjectionSha256.empty();
  if (sourceOracleRequested &&
      (options.sourceOraclePath.empty() ||
       !isSha256Hex(options.sourceProjectionSha256) ||
       !options.beforeNextWork || options.maxPending != 1u ||
       options.repeat < 2u)) {
    return reject("unsupported-mode",
                  "source-oracle-requires-before-next-work-maxpending1-repeat2");
  }
  const auto importBegin = std::chrono::steady_clock::now();
  std::vector<std::byte> tape;
  if (!readBinaryFile(options.tapePath, tape))
    return reject("input-read-failed", "events");
  std::vector<std::vector<std::byte>> ownedBlobs;
  ownedBlobs.reserve(options.blobPaths.size());
  for (const auto& path : options.blobPaths) {
    ownedBlobs.emplace_back();
    if (!readBinaryFile(path, ownedBlobs.back()))
      return reject("blob-read-failed", path);
  }
  std::vector<dxmt9::d3d9::RenderTapeProviderBlob> blobs;
  blobs.reserve(ownedBlobs.size());
  for (const auto& bytes : ownedBlobs) {
    blobs.push_back({dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes), bytes});
  }
  std::vector<std::byte> identitySidecar;
  dxmt9::d3d9::CaptureSourceReplayPlan captureSourcePlan;
  std::vector<CpuSourceOracleRow> sourceOracleRows;
  std::string sourceOracleSha256;
  if (sourceOracleRequested) {
    std::vector<std::byte> sourceOracleBytes;
    if (!readBoundedBinaryFile(options.sourceOraclePath,
                               dxmt9::test::kCpuSourceOracleMaxBytes,
                               sourceOracleBytes))
      return reject("source-oracle-read-failed", "source-oracle");
    sourceOracleSha256 = renderTapeDigestHex(
        dxmt9::d3d9::RenderTapeCaptureSession::sha256(sourceOracleBytes));
    if (!decodeCpuSourceOracle(sourceOracleBytes, sourceOracleRows))
      return reject("source-oracle-invalid", "source-oracle-format");
  }
  runtime::GeneralPreflightPlan plan;
  const auto preflight = runtime::buildGeneralReplayPlan(tape, blobs, plan);
  if (!preflight.complete())
    return reject("general-preflight-failed",
                  dxmt9::d3d9::frameTapeReplayStatusName(preflight.status));
  inputAdmission = runtime::admitGeneralCpuPlan(plan, executorOptions.inputCaps);
  const auto& admission = *inputAdmission;
  if (!admission.accepted())
    return reject("unsupported-grammar", runtime::generalCpuAdmissionReasonName(admission.reason));
  if (captureSourceRequested) {
    constexpr std::size_t kMaxIdentitySidecarBytes = 16u * 1024u * 1024u;
    if (!readBoundedBinaryFile(options.identityPath, kMaxIdentitySidecarBytes,
                               identitySidecar))
      return reject("identity-read-failed", "identity");
    const auto sourceStatus = dxmt9::d3d9::buildCaptureSourceReplayPlan(
        tape, plan, identitySidecar, captureSourcePlan);
    if (sourceStatus != dxmt9::d3d9::CaptureSourceReplayPlanStatus::Valid)
      return reject("source-plan-failed",
                    dxmt9::d3d9::captureSourceReplayPlanStatusName(sourceStatus));
    if (renderTapeDigestHex(captureSourcePlan.digest()) !=
        options.sourcePlanSha256)
      return reject("source-plan-digest-mismatch", "source-plan-sha256");
  }
  const auto importNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - importBegin).count();
  const auto queryEventsSha256 = admission.counts.queryDefinitions
      ? renderTapeDigestHex(dxmt9::d3d9::RenderTapeCaptureSession::sha256(tape))
      : std::string{};

  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  if (resourceDevice.handle == NULL_OBJECT_HANDLE)
    return reject("skip-no-metal-device", "resource-backed-execution-required");
  PresentParameters output{};
  output.backBufferWidth = plan.outputDesc.width;
  output.backBufferHeight = plan.outputDesc.height;
  output.backBufferFormat = dxmt9::d3d9::devicec::fmtFromD3D(plan.outputDesc.format);
  output.backBufferCount = 1u;
  output.windowed = true;
  output.deviceWindow = Handle{91};
  output.presentationInterval = PresentInterval::Immediate;

  executorOptions.route = options.compatibility
      ? dxmt9::d3d9::ProviderReplayPolicy::Compatibility
      : dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot;
  executorOptions.beforeNextWork = options.beforeNextWork;
  executorOptions.maxPendingSources = options.maxPending;
  executorOptions.holdSafeBufferMutation = sourceOracleRequested;
  executorOptions.sourceOracle = sourceOracleRows;
  executorOptions.measurement = options.measurement;
  executorOptions.captureSourcePlan = captureSourceRequested ? &captureSourcePlan : nullptr;
  executorOptions.captureSourceTape = captureSourceRequested
      ? std::span<const std::byte>(tape) : std::span<const std::byte>{};
  executorOptions.captureSourceIdentitySidecar = captureSourceRequested
      ? std::span<const std::byte>(identitySidecar) : std::span<const std::byte>{};
  struct Iteration {
    Executor::Result execution;
    std::uint64_t direct = 0u, compatibility = 0u, elapsed = 0u;
    dxmt9::perf::RenderTapeReplaySnapshot before{}, after{};
    dxmt9::append_diag::Snapshot appendBefore{}, appendAfter{};
    std::optional<std::uint64_t> nativeCpuNs;
    std::optional<dxmt9::test::CpuDiagnosticLedger> diagnostic;
  };
  std::vector<Iteration> results;
  results.reserve(options.repeat);
  for (std::size_t index = 0u; index < options.repeat; ++index) {
    const auto begin = std::chrono::steady_clock::now();
    const auto nativeCpuBegin = sampleProcessCpuNs();
    Iteration iterationResult;
    if (options.diagnostic) iterationResult.diagnostic.emplace(sampleCpuDiagnosticClocks);
    auto* diagnostic = iterationResult.diagnostic ? &*iterationResult.diagnostic : nullptr;
    using Phase = dxmt9::test::CpuDiagnosticPhase;
    {
      dxmt9::test::CpuDiagnosticLedger::Scope phase(diagnostic, Phase::FixtureSetup);
      {
        RuntimeFixture fixture(false, false, true, false,
                               WMT::Device{resourceDevice.handle}, nullptr, &output);
        phase.set(Phase::Bookkeeping);
        fixture.routing->recordBoundedCapturedBindings = false;
        iterationResult.before = dxmt9::perf::snapshotRenderTapeReplay();
        iterationResult.appendBefore = dxmt9::append_diag::snapshot();
        auto iterationOptions = executorOptions;
        iterationOptions.diagnostic = diagnostic;
        Executor executor(fixture, plan, blobs, iterationOptions);
        iterationResult.execution = executor.run();
        iterationResult.after = dxmt9::perf::snapshotRenderTapeReplay();
        iterationResult.appendAfter = dxmt9::append_diag::snapshot();
        iterationResult.direct = iterationResult.after.directLeaseDraws -
            iterationResult.before.directLeaseDraws;
        iterationResult.compatibility = iterationResult.after.compatibilityDraws -
            iterationResult.before.compatibilityDraws;
        auto& execution = iterationResult.execution;
        if (execution.complete &&
            (execution.draws != admission.counts.draws ||
             (!options.measurement && execution.drawObservations.size() != execution.draws) ||
             execution.sourceSummaryDraws != execution.draws ||
             execution.resourcesCreated != admission.counts.objects ||
             execution.resourcesReleased != execution.resourcesCreated ||
             execution.submitted != execution.completed ||
             execution.submitted != execution.reclaimed ||
             iterationResult.direct + iterationResult.compatibility !=
                 execution.draws)) {
          execution.complete = false;
          execution.failure = "general-execution-conservation-failed";
        }
        phase.set(Phase::FixtureTeardown);
      }
    }
    if (diagnostic) diagnostic->finish();
    iterationResult.elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin)
            .count());
    const auto nativeCpuEnd = sampleProcessCpuNs();
    if (nativeCpuBegin && nativeCpuEnd && *nativeCpuEnd >= *nativeCpuBegin)
      iterationResult.nativeCpuNs = *nativeCpuEnd - *nativeCpuBegin;
    results.push_back(std::move(iterationResult));
    if (!results.back().execution.complete) break;
  }
  const bool complete = results.size() == options.repeat &&
      std::all_of(results.begin(), results.end(), [](const auto& value) {
        return value.execution.complete;
      });
  const bool deterministic = !results.empty() &&
      std::all_of(results.begin(), results.end(), [&](const auto& value) {
        return value.execution.digest == results.front().execution.digest &&
               value.execution.querySemanticDigest == results.front().execution.querySemanticDigest &&
               value.execution.draws == results.front().execution.draws &&
               value.execution.commands == results.front().execution.commands;
      });
  const bool nativeCpuAvailable =
      !results.empty() &&
      std::any_of(results.begin(), results.end(), [](const auto& value) {
        return value.nativeCpuNs.has_value();
      });
  std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1","grammar":"general-frame","semantic_projection":"final-slot-general-v2","backend":"cpu-metal-resources","resource_execution":"cpu-visible-metal-no-gpu-submit","attachment_contents":"backend-command-owned-v1","gpu_readback":false,"timing_backend":"encode-substituted","status":)"
            << std::quoted(complete ? "complete" : "unresolved");
  printCpuInputAdmissionSelection(options.inputAdmissionProfile,
      &executorOptions.inputCaps, executorOptions.sourceCapacity,
      executorOptions.maxQueryOraclePolls);
  std::cout << R"(,"run_mode":)" << std::quoted(options.diagnostic ? "diagnose" : options.measurement ? "measure" : "verify")
            << R"(,"phase_accounting":)"
            << std::quoted(options.diagnostic ? "exclusive-diagnostic-wall-process" :
                           options.measurement ? "iteration-only" : "scoped-observations")
            << R"(,"runtime_perf_counters":true,"cpu_policy":{"schema":)"
            << std::quoted(captureSourceRequested
                               ? "dxmt9.render_tape.cpu_policy.v2"
                               : "dxmt9.render_tape.cpu_policy.v1")
            << R"(,"replay_route":)"
            << std::quoted(options.compatibility ? "compatibility" : "production-slot")
            << R"(,"draw_command_limit":)" << options.policy.drawCommandLimit
            << R"(,"slot_headroom_bytes":)" << options.policy.slotHeadroomBytes
            << R"(,"aggregate_headroom_bytes":)" << options.policy.aggregateHeadroomBytes;
  if (captureSourceRequested) {
    std::cout << R"(,"source_boundary_mode":"capture-source","source_plan_sha256":)"
              << std::quoted(options.sourcePlanSha256);
  }
  std::cout << '}'
            << R"(,"replay_route":)" << std::quoted(options.compatibility ? "compatibility" : "production-slot")
            << R"(,"publication":"per-command","completion":)"
            << std::quoted(admission.counts.queryDefinitions ? "controlled-query-ready-obligation"
                : options.beforeNextWork ? "before-next-work" : "after-command")
            << R"(,"requested_completion":)"
            << std::quoted(options.beforeNextWork ? "before-next-work" : "after-command")
            << R"(,"repeat":)" << options.repeat
            << R"(,"fault":"none","max_pending":)" << options.maxPending
            << R"(,"source_projection_sha256":)";
  if (sourceOracleRequested)
    std::cout << std::quoted(options.sourceProjectionSha256);
  else
    std::cout << "null";
  std::cout << R"(,"source_oracle_sha256":)";
  if (sourceOracleRequested)
    std::cout << std::quoted(sourceOracleSha256);
  else
    std::cout << "null";
  const auto& runtimeEvidence = results.front().execution;
  std::cout << R"(,"window_runtime":{"page_size":)" << runtimeEvidence.windowPageSize
            << R"(,"offload":)" << (runtimeEvidence.windowOffload ? "true" : "false")
            << R"(,"cpu_ready_arena":)" << (runtimeEvidence.windowArena ? "true" : "false")
            << R"(,"workers_disabled":)" << (runtimeEvidence.windowWorkersDisabled ? "true" : "false")
            << R"(},"hold_safe_buffer_mutation":)"
            << (sourceOracleRequested ? "true" : "false")
            << R"(,"completion_plan_sha256":null,"immutable_input_owned":true,"deterministic":)"
            << (deterministic ? "true" : "false")
            << R"(,"native_cpu_clock":)" << std::quoted(
                nativeCpuAvailable ? "process" : "unavailable")
            << R"(,"reset_policy":"fresh-device","cache_policy":"fresh-runtime","shared_native_resource_device":true)"
            << R"(,"effective_cpu_policy":{"reset":"fresh-device","cpu_ready_arena":false,"draw_command_limit":)"
            << options.policy.drawCommandLimit
            << R"(,"draw_payload_limit":0,"early_prefix":false,"gpu_submit":false})"
            << R"(,"import_ns":)" << importNs
            << R"(,"input_counts":)";
  printGeneralCpuInputCounts(admission.counts);
  std::cout << R"(,"input_admission_counts":)";
  printGeneralCpuInputAdmissionCounts(admission.counts);
  std::cout << R"(,"iterations":[)";
  for (std::size_t index = 0u; index < results.size(); ++index) {
    if (index) std::cout << ',';
    const auto& iteration = results[index];
    const auto& result = iteration.execution;
    const auto bindingObservations = result.observedSnapshots + result.observedLiveBindings;
    std::cout << R"({"index":)" << index
              << R"(,"complete":)" << (result.complete ? "true" : "false")
              << R"(,"reset_ok":)" << (result.resetOk ? "true" : "false")
              << R"(,"failure":)" << std::quoted(result.failure)
              << R"(,"failed_event":)" << result.failedEvent
              << R"(,"input_admission_reason":)"
              << std::quoted(runtime::generalCpuAdmissionReasonName(result.inputAdmission.reason))
              << R"(,"input_admission_counts":)";
    printGeneralCpuInputAdmissionCounts(result.inputAdmission.counts);
    std::cout << R"(,"input_admission_caps":)";
    printGeneralCpuInputCaps(result.inputAdmission.inputCaps);
    std::cout << R"(,"source_demand":{"publications":)" << result.sourceDemand.publications
              << R"(,"progress_steps":)" << result.sourceDemand.progressSteps
              << R"(},"source_publications":)" << result.sourcePublications
              << R"(,"progress_steps":)" << result.progressSteps
              << R"(,"semantic_digest":")" << result.digest << '"'
              << R"(,"query_semantic_digest":")" << result.querySemanticDigest << '"'
              << R"(,"query_replay":)";
    printCpuQueryReplay(result.queryReplay, options.measurement, queryEventsSha256);
    std::cout
              << R"(,"command_count":)" << result.commands
              << R"(,"draw_count":)" << result.draws
              << R"(,"full_oracle_calls":)" << result.fullOracleCalls
              << R"(,"source_summary_commands":)" << result.sourceSummaryCommands
              << R"(,"source_summary_draws":)" << result.sourceSummaryDraws
              << R"(,"direct_draws":)" << iteration.direct
              << R"(,"mipmap_command_count":)" << result.mipmapCommands
              << R"(,"retained_mipmap_textures_after_destroy":)"
              << result.retainedMipmapTexturesAfterDestroy
              << R"(,"compatibility_draws":)" << iteration.compatibility
              << R"(,"capacity_rejected":)" << iteration.after.continuationCapacityRejected - iteration.before.continuationCapacityRejected
              << R"(,"populated_fallback":)" << iteration.after.continuationPopulatedFallback - iteration.before.continuationPopulatedFallback
              << R"(,"capacity_rotated":)" << iteration.after.continuationCapacityRotated - iteration.before.continuationCapacityRotated
              << R"(,"slot_provisioned":)" << iteration.after.slotProvisioned - iteration.before.slotProvisioned
              << R"(,"slot_provision_reused":)" << iteration.after.slotProvisionReused - iteration.before.slotProvisionReused
              << R"(,"slot_provision_failed":)" << iteration.after.slotProvisionFailed - iteration.before.slotProvisionFailed
              << R"(,"capacity_lease_denied":)" << iteration.after.capacityLeaseDenied - iteration.before.capacityLeaseDenied
              << R"(,"draw_limit_publishes":)" << iteration.after.drawLimitPublishes - iteration.before.drawLimitPublishes
              << R"(,"resources_created":)" << result.resourcesCreated
              << R"(,"resources_released":)" << result.resourcesReleased
              << R"(,"logical_resource_oracle_complete":)"
              << (result.logicalResourceOracleComplete ? "true" : "false")
              << R"(,"buffer_snapshot_observations":)" << result.observedSnapshots
              << R"(,"buffer_live_binding_observations":)" << result.observedLiveBindings
              << R"(,"buffer_binding_observations":)" << bindingObservations
              << R"(,"source_oracle_checked_sources":)"
              << result.sourceOracleCheckedSources
              << R"(,"deferred_buffer_mutations":)" << result.deferredBufferMutations
              << R"(,"deferred_discard_mutations":)" << result.deferredDiscardMutations
              << R"(,"retained_buffers_after_mutation":)" << result.retainedBuffersAfterMutation
              << R"(,"mutation_boundary_releases":)" << result.mutationBoundaryReleases
              << R"(,"buffer_backing_observed":)" << (bindingObservations ? "true" : "false")
              << R"(,"buffer_backing_oracle_complete":)"
              << (bindingObservations && result.logicalResourceOracleComplete ? "true" : "false")
              << R"(,"buffer_backing_oracle":)"
              << std::quoted(bindingObservations ? "real-metal-cpu-visible" : "not-applicable")
              << R"(,"submitted":)" << result.submitted
              << R"(,"completed":)" << result.completed
              << R"(,"reclaimed":)" << result.reclaimed
              << R"(,"pending_peak":)" << result.pendingPeak
              << R"(,"deferred_releases":)" << result.deferredReleases
              << R"(,"destroys_with_pending":)" << result.destroysWithPending
              << R"(,"retained_snapshots_after_destroy":)" << result.retainedSnapshotsAfterDestroy
              << R"(,"elapsed_ns":)" << iteration.elapsed
              << R"(,"phase_ns":{"setup":)" << result.setupNs
              << R"(,"replay":)" << result.replayNs
              << R"(,"observe":)" << result.observeNs
              << R"(,"completion":)" << result.completionNs
              << R"(,"cleanup":)" << result.cleanupNs
              << R"(},"native_cpu_ns":)";
    if (iteration.nativeCpuNs)
      std::cout << *iteration.nativeCpuNs;
    else
      std::cout << "null";
    if (iteration.diagnostic) {
      std::cout << R"(,"compat_append_observation":{"mode":)"
                << dxmt9::append_diag::mode() << R"(,"phases":{)";
      for (unsigned p = 0; p < dxmt9::append_diag::names.size(); ++p) {
        if (p) std::cout << ',';
        std::cout << std::quoted(dxmt9::append_diag::names[p])
                  << R"(:{"wall_ns":)"
                  << iteration.appendAfter[p].ns - iteration.appendBefore[p].ns
                  << R"(,"calls":)"
                  << iteration.appendAfter[p].calls - iteration.appendBefore[p].calls
                  << '}';
      }
      std::cout << "}}";
      const auto& ledger = *iteration.diagnostic;
      std::cout << R"(,"diagnostic_accounting":{"valid":)"
                << (ledger.valid() ? "true" : "false")
                << R"(,"wall_ns":)" << ledger.total().wall
                << R"(,"cpu_ns":)" << ledger.total().cpu
                << R"(,"phases":{)";
      for (std::size_t phase = 0u; phase < ledger.rows().size(); ++phase) {
        if (phase) std::cout << ',';
        const auto& row = ledger.rows()[phase];
        std::cout << std::quoted(dxmt9::test::kCpuDiagnosticPhaseNames[phase])
                  << R"(:{"wall_ns":)" << row.wall << R"(,"cpu_ns":)" << row.cpu
                  << R"(,"entries":)" << row.entries << '}';
      }
      std::cout << "}}";
    }
    if (captureSourceRequested) {
      const bool sourceComplete = result.complete &&
          !result.captureSourceReplayStorageOverflow &&
          result.captureSourcePlanRanges == result.captureSourceRows.size() &&
          std::all_of(result.captureSourceReplaySources.begin(),
                      result.captureSourceReplaySources.end(),
                      [](const auto& row) { return row.settled; });
      std::cout << ",\"source_replay\":{\"requested\":true,\"validated\":true,\"complete\":"
                << (sourceComplete ? "true" : "false")
                << ",\"plan_sha256\":"
                << std::quoted(renderTapeDigestHex(result.captureSourcePlanDigest))
                << ",\"source_count\":" << result.captureSourceRows.size()
                << ",\"completed_source_count\":"
                << result.captureSourcePlanRanges
                << ",\"publication_count\":"
                << result.captureSourcePlanPublications
                << ",\"empty_source_count\":"
                << result.captureSourcePlanEmptySources
                << ",\"present_source_count\":"
                << result.captureSourcePlanPresentSources
                << ",\"storage_overflow\":"
                << (result.captureSourceReplayStorageOverflow ? "true" : "false")
                << ",\"sources\":[";
      for (std::size_t row = 0u;
           row < result.captureSourceReplaySources.size(); ++row) {
        if (row) std::cout << ',';
        const auto& value = result.captureSourceReplaySources[row];
        const char* settlement = "empty";
        if (value.settlement ==
            dxmt9::d3d9::RenderTapeSourceReplaySettlement::Published)
          settlement = "published";
        else if (value.settlement ==
                 dxmt9::d3d9::RenderTapeSourceReplaySettlement::PresentPublished)
          settlement = "present-published";
        std::cout << "{\"event_ordinal\":" << value.eventOrdinal
                  << ",\"source_ordinal\":" << value.sourceOrdinal
                  << ",\"captured_seq_id\":" << value.capturedSeqId
                  << ",\"command_chunk_index\":" << value.commandChunkIndex
                  << ",\"first_record\":" << value.firstRecord
                  << ",\"record_count\":" << value.recordCount
                  << ",\"local_raw_ordinal\":" << value.actualRawOrdinal
                  << ",\"range_index\":" << value.rangeIndex
                  << ",\"settled\":" << (value.settled ? "true" : "false")
                  << ",\"settlement\":\"" << settlement
                  << "\",\"publications\":[";
        const auto first = static_cast<std::size_t>(value.publicationOffset);
        const auto last = first + value.publicationCount;
        for (std::size_t publication = first; publication < last; ++publication) {
          if (publication != first) std::cout << ',';
          const auto& receipt = result.captureSourceReplayPublications[publication];
          std::cout << "{\"source_index\":" << receipt.sourceIdIndex
                    << ",\"source_generation\":" << receipt.sourceIdGeneration
                    << ",\"storage_first_page\":" << receipt.storageFirstPage
                    << ",\"storage_page_count\":" << receipt.storagePageCount
                    << ",\"storage_generation\":" << receipt.storageGeneration
                    << ",\"raw_ordinal\":" << receipt.metadataRawOrdinal
                    << ",\"source_ordinal\":" << receipt.metadataSourceOrdinal
                    << ",\"seq_id\":" << receipt.metadataSeqId
                    << ",\"build_generation\":" << receipt.metadataBuildGeneration
                    << ",\"used_bytes\":" << receipt.metadataUsedBytes
                    << ",\"page_count\":" << receipt.metadataPageCount
                    << ",\"padding_pages_before\":"
                    << receipt.metadataPaddingPagesBefore
                    << ",\"strict_admission\":"
                    << (receipt.metadataStrictAdmission ? "true" : "false")
                    << ",\"control_index\":" << receipt.controlIndex
                    << ",\"has_present\":"
                    << (receipt.hasPresent ? "true" : "false") << '}';
        }
        std::cout << "]}";
      }
      std::cout << "]}";
    }
    std::cout << R"(,"fault_triggered":false,"physical_texture_oracle":false,"dispositions":[)";
    for (std::size_t reason = 0u; reason < iteration.after.dispositions.size(); ++reason) {
      if (reason) std::cout << ',';
      std::cout << iteration.after.dispositions[reason] - iteration.before.dispositions[reason];
    }
    std::cout << R"(],"outcomes":[)";
    for (std::size_t reason = 0u; reason < iteration.after.outcomes.size(); ++reason) {
      if (reason) std::cout << ',';
      std::cout << iteration.after.outcomes[reason] - iteration.before.outcomes[reason];
    }
    std::cout << R"(],"destroy_windows":[)";
    for (std::size_t i = 0u; i < result.destroyWindows.size(); ++i) {
      if (i) std::cout << ',';
      const auto& window = result.destroyWindows[i];
      std::cout << R"({"event":)" << window.event
                << R"(,"owners_checked":)" << window.ownersChecked
                << R"(,"retention_valid":)" << (window.retentionValid ? "true" : "false")
                << R"(,"pending_submissions":[)";
      for (std::size_t j = 0u; j < window.pendingSubmissions.size(); ++j) {
        if (j) std::cout << ',';
        std::cout << window.pendingSubmissions[j];
      }
      std::cout << "]}";
    }
    std::cout << R"(],"draw_observations":[)";
    for (std::size_t draw = 0u; draw < result.drawObservations.size(); ++draw) {
      if (draw) std::cout << ',';
      const auto& row = result.drawObservations[draw];
      std::cout << R"({"ordinal":)" << row.ordinal
                << R"(,"publication_event":)" << row.publicationEvent + 1u
                << R"(,"cumulative_semantic_digest":")" << row.cumulativeSemanticDigest << R"("})";
    }
    std::cout << R"(],"trace":[)";
    for (std::size_t trace = 0u; trace < result.trace.size(); ++trace) {
      if (trace) std::cout << ',';
      const auto& row = result.trace[trace];
      std::cout << R"({"iteration":)" << index + 1u
                << R"(,"submission":)" << row.submission
                << R"(,"source":)" << row.source
                << R"(,"source_generation":)" << row.sourceGeneration
                << R"(,"storage_first_page":)" << row.storageFirstPage
                << R"(,"storage_page_count":)" << row.storagePageCount
                << R"(,"storage_generation":)" << row.storageGeneration
                << R"(,"first_event":)" << row.event + 1u
                << R"(,"last_event":)" << row.event + 1u
                << R"(,"event":)" << row.event + 1u
                << R"(,"event_submission":)" << trace + 1u
                << R"(,"released_at_event":)" << row.releasedAtEvent + 1u
                << R"(,"held_mutation_event":)" << row.heldMutationEvent
                << R"(,"held_mutation_identity":)";
      if (row.heldMutationIdentity) {
        const auto& identity = *row.heldMutationIdentity;
        std::cout << R"({"kind":)" << identity.kind
                  << R"(,"generation":)" << identity.generation
                  << R"(,"object_id":")" << identity.objectId << R"("})";
      } else {
        std::cout << "null";
      }
      std::cout
                << R"(,"submitted":)" << (row.submitted ? "true" : "false")
                << R"(,"completed":)" << (row.completed ? "true" : "false")
                << R"(,"buffer_closure":[)";
      for (std::size_t resource = 0u; resource < row.bufferClosure.size(); ++resource) {
        if (resource) std::cout << ',';
        const auto& identity = row.bufferClosure[resource];
        std::cout << R"({"kind":)" << identity.kind
                  << R"(,"generation":)" << identity.generation
                  << R"(,"object_id":")" << identity.objectId << R"("})";
      }
      std::cout << "]}";
    }
    std::cout << "]}";
  }
  std::cout << R"(],"loaded_images":)";
  printCpuTapeLoadedImages();
  std::cout << "}\n";
  return complete && deterministic ? 0 : 4;
}

void controlledQueryFenceRefinementAndLifetime() {
  using namespace dxmt9::query;
  for (std::uint64_t owner : {1u, 2u})
    for (std::uint64_t sequence = 0u; sequence <= 2u; ++sequence)
      for (std::uint64_t published = 0u; published <= 2u; ++published)
        for (std::uint64_t completed = 0u; completed <= 3u; ++completed)
          for (bool failed : {false, true}) {
            const Fence fence{owner, sequence};
            const Progress progress{1u, published, completed, failed};
            const auto observed = classify(fence, progress);
            const bool valid = owner == progress.queue && completed <= published &&
                               sequence <= published;
            check((observed == Poll::Complete) ==
                      (valid && !failed && completed >= sequence),
                  "production query classify admits only queue-qualified completed prefixes");
            check((observed == Poll::Pending) ==
                      (valid && !failed && completed < sequence),
                  "publication and submission cannot manufacture query readiness");
          }
  const WriterIdentity writer{1u, 0u, 1u, 1u};
  auto marker = std::make_shared<EndMarker>(writer);
  EndReceipt deferred{1u, 0u};
  deferred.marker = marker;
  check(classifyEnd(deferred, {1u, 0u, 0u}) == Poll::Pending,
        "unbound END is not an already-completed empty prefix");
  check(!marker->bind({2u, 0u, 1u, 1u}, 1u) && marker->bind(writer, 1u) &&
            !marker->bind(writer, 2u),
        "production END binds its exact writer once, never foreign or duplicate publication");
  marker->cancel();
  check(classifyEnd(deferred, {1u, 1u, 1u}) == Poll::Failed,
        "cancelled result remains terminal above completion waterline");

  RuntimeFixture fixture(false, false, true, false);
  fixture.routing->controlledQueries = true;
  auto* wrapped = dxmt9c_device_create_query(fixture.cDevice.get(), 9u);
  check(wrapped != nullptr, "controlled production query creation");
  check(dxmt9c_query_issue(wrapped, 2u) == D3D_OK, "controlled BEGIN");
  fixture.routing->submitClear(ClearDesc{});
  check(dxmt9c_query_issue(wrapped, 1u) == D3D_OK, "controlled END publishes real work");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::publishWriting(fixture.routing->queue_),
        "controlled deferred END publishes through production queue");
  auto old = fixture.cDevice->dev().captureQueryPollSnapshot(wrapped->obj);
  const auto fence = old.backendFence.snapshot();
  check(old.eligible && fence.queue != 0u && fence.sequence != 0u,
        "controlled END receives an actual local publication receipt");
  std::uint32_t output = 0xdeadbeefu;
  check(dxmt9c_query_get_data(wrapped, &output, sizeof(output), 1u) == S_FALSE &&
            output == 0xdeadbeefu,
        "recorded FLUSH is nonblocking and preserves undefined pending bytes");
  dxmt9::d3d9::RenderTapeCompletionSchedule schedule;
  const auto ready = dxmt9::CommandQueueArenaLeaseTestAccess::peekReadyCompletion(fixture.routing->queue_);
  const dxmt9::d3d9::RenderTapeCompletionKey key{
      1u, ready.seqId, ready.source.id.index, 0u, ready.source.id.generation};
  check(schedule.append({key}).accepted() && schedule.beginIteration(1u).accepted(),
        "bounded query completion schedule initialized");
  bool prematureCallback = false;
  check(!schedule.release(key, [&] { prematureCallback = true; return true; }).accepted() &&
            !prematureCallback,
        "query completion schedule rejects acknowledgment before submission");
  check(schedule.submit(key).accepted(), "actual query submission admitted");
  const auto submitted = dxmt9::CommandQueueArenaLeaseTestAccess::submitOneWithheld(fixture.routing->queue_);
  check(submitted.submitted && fixture.cDevice->dev().pollQuerySnapshot(old,
            &output, sizeof(output), 0u) == S_FALSE,
        "submitted work remains Pending until explicit controlled completion");
  check(schedule.release(key, [&] {
          return dxmt9::CommandQueueArenaLeaseTestAccess::acknowledgeOne(fixture.routing->queue_);
        }).accepted() &&
        dxmt9::CommandQueueArenaLeaseTestAccess::finishQueue(fixture.routing->queue_),
        "query source completes and retires through the production lifecycle");
  check(!schedule.release(key).accepted(), "duplicate query acknowledgment rejects");
  check(fixture.cDevice->dev().pollQuerySnapshot(old, &output, sizeof(output), 0u) == D3D_OK,
        "completion makes the original query receipt ready");
  const auto resultOwner = old.occlusionResult;
  check(dxmt9c_query_issue(wrapped, 2u) == D3D_OK,
        "new BEGIN creates a replacement cycle");
  const auto replacement = fixture.cDevice->dev().captureQueryPollSnapshot(wrapped->obj);
  check(replacement.issueOrdinal > old.issueOrdinal && replacement.active &&
            replacement.occlusionResult != resultOwner,
        "reissue never relabels or clears old result storage");
  output = 0xdeadbeefu;
  check(dxmt9c_query_get_data(wrapped, &output, sizeof(output), 0u) == S_FALSE &&
            output == 0xdeadbeefu,
        "replacement cycle cannot expose the previous result");
  dxmt9c_query_release(wrapped);
  check(fixture.cDevice->dev().pollQuerySnapshot(old, &output, sizeof(output), 0u) == D3D_OK,
        "retained original snapshot survives public wrapper release");
  PresentParameters reset{};
  reset.backBufferWidth = reset.backBufferHeight = 16u;
  reset.backBufferFormat = Format::A8R8G8B8;
  reset.windowed = true;
  reset.deviceWindow = Handle{91u};
  reset.presentationInterval = PresentInterval::Immediate;
  check(fixture.cDevice->dev().reset(reset) == D3D_OK, "query reset advances device epoch");
  output = 0xdeadbeefu;
  check(fixture.cDevice->dev().pollQuerySnapshot(old, &output, sizeof(output), 0u) ==
            D3DERR_DEVICELOST && output == 0xdeadbeefu,
        "reset rejects stale epoch without writing result bytes");
}

void generalCpuQueryTranscriptUsesControlledQueue() {
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  using Fault = dxmt9::d3d9::test::CpuQueryFixtureFault;
  for (auto fault : {Fault::StaleCycle, Fault::MissingBegin,
                     Fault::UncapturedClosedResult, Fault::LegacyDescriptor}) {
    const auto tape = dxmt9::d3d9::test::makeGeneralCpuQueryFixture(fault);
    std::vector<dxmt9::d3d9::RenderTapeProviderBlob> blobs;
    for (const auto& bytes : tape.blobs)
      blobs.push_back({dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes), bytes});
    runtime::GeneralPreflightPlan plan;
    const auto preflight = runtime::buildGeneralReplayPlan(tape.tape, blobs, plan);
    check(!preflight.complete() && plan.events.empty(),
          "query negative fixture " + std::to_string(static_cast<unsigned>(fault)) +
              " rejects before effects: " +
              dxmt9::d3d9::frameTapeReplayStatusName(preflight.status));
  }
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE, "query tape needs actual CPU-visible resource backing");
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<
      RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
  for (bool reissue : {false, true}) {
    const auto tape = dxmt9::d3d9::test::makeGeneralCpuQueryFixture(Fault::None, reissue);
    std::vector<dxmt9::d3d9::RenderTapeProviderBlob> blobs;
    for (const auto& bytes : tape.blobs)
      blobs.push_back({dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes), bytes});
    runtime::GeneralPreflightPlan plan;
    const auto preflight = runtime::buildGeneralReplayPlan(tape.tape, blobs, plan);
    check(preflight.complete(), std::string("query preflight: ") +
          dxmt9::d3d9::frameTapeReplayStatusName(preflight.status));
    const auto admission = runtime::admitGeneralCpuPlan(plan);
    check(admission.accepted() && admission.counts.queryDefinitions == 1u &&
              admission.counts.queryIssues == (reissue ? 4u : 2u) &&
              admission.counts.queryPolls == (reissue ? 4u : 3u),
          "CPU query grammar preserves issue and original poll coverage");
    std::uint64_t semantic = 0u, querySemantic = 0u;
    for (unsigned scenario : {0u, 1u, 2u}) {
      for (bool compatibility : {true, false}) {
        const bool measured = scenario == 1u;
        const bool exhausted = scenario == 2u;
        RuntimeFixture fixture(false, false, true, false, WMT::Device{resourceDevice.handle});
        Executor executor(fixture, plan, blobs, {
            .route = compatibility ? dxmt9::d3d9::ProviderReplayPolicy::Compatibility
                                   : dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot,
            .measurement = measured, .maxQueryOraclePolls = exhausted ? 0u : 4u});
        const auto result = executor.run();
        if (exhausted) {
          check(!result.complete && result.resetOk &&
                    result.failure == "general-executor-query-progress-exhausted" &&
                    result.queryReplay.progressLimit ==
                        dxmt9::d3d9::RenderTapeQueryProgressLimit::PollBudget &&
                    result.queryReplay.supplementalPolls == 0u,
                "zero progress budget fails without hiding an oracle drain: " + result.failure);
          continue;
        }
        check(result.complete && result.resetOk, "controlled query replay: " + result.failure);
        const auto& queries = result.queryReplay;
        check(queries.present && queries.transcriptComplete && queries.oracleComplete &&
                  queries.progressLimit == dxmt9::d3d9::RenderTapeQueryProgressLimit::None &&
                  !queries.actualMetal && !queries.gpuResultOracle &&
                  queries.replayedIssueRecords == admission.counts.queryIssues &&
                  queries.replayedPolls == admission.counts.queryPolls &&
                  queries.readinessSatisfied == (reissue ? 2u : 1u) &&
                  queries.substitutedResults == (reissue ? 2u : 1u) &&
                  queries.matchedResults == 0u && queries.supplementalPolls == (reissue ? 2u : 1u),
              "CPU proves qualified readiness without claiming captured GPU samples");
        if (measured) {
          check(queries.rows.empty() && result.fullOracleCalls == 0u,
                "query measurement retains cheap obligations and omits semantic rows");
        } else {
          check(queries.rows.size() == admission.counts.queryPolls &&
                    queries.rows[0].capturedResult == S_FALSE &&
                    queries.rows[0].replayResult == S_FALSE &&
                    queries.rows[1].flags == 1u,
                "original Pending and FLUSH calls remain visible and ordered");
          for (const auto& row : queries.rows)
            if (row.resultRequired)
              check(row.readinessSatisfied && row.localSequence != 0u &&
                        row.completedSequence >= row.localSequence &&
                        row.resultSubstituted && !row.resultMatched,
                    "captured success binds the same local issue and completed END");
          if (semantic == 0u) { semantic = result.digest; querySemantic = result.querySemanticDigest; }
          check(result.digest == semantic && result.querySemanticDigest == querySemantic,
                "production route fallback preserves draw and query transcript semantics");
        }
      }
    }
  }
}

void generalCpuEventCheckpointUsesNewLocalEnd() {
  namespace tape = dxmt9::d3d9;
  namespace runtime = tape::render_tape_runtime;
  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<RuntimeFixture, Access>;
  using Fault = tape::test::CpuEventCheckpointFixtureFault;
  for (auto fault : {Fault::StaleCycle, Fault::StaleIdentity, Fault::MissingEpoch,
                     Fault::PendingCompletion, Fault::OpenBegin, Fault::Occlusion}) {
    const auto fixture = tape::test::makeGeneralCpuEventCheckpointFixture(fault);
    check(!fixture.tape.empty(), "EVENT checkpoint negative contains actual tape bytes");
    std::vector<tape::RenderTapeProviderBlob> blobs;
    for (const auto& bytes : fixture.blobs)
      blobs.push_back({tape::RenderTapeCaptureSession::sha256(bytes), bytes});
    runtime::GeneralPreflightPlan plan;
    check(!runtime::buildGeneralReplayPlan(fixture.tape, blobs, plan).complete() &&
              plan.events.empty(), "wrong captured checkpoint rejects transactionally");
  }
  // Concrete production binding distinguishes empty valid END from unissued
  // state, then exercises nonempty reissue, early completion and reset.
  {
    RuntimeFixture fixture(false, false, true, false);
    fixture.routing->controlledQueries = true;
    auto* query = dxmt9c_device_create_query(fixture.cDevice.get(), 8u);
    check(query && dxmt9c_query_issue(query, 1u) == D3D_OK,
          "EVENT checkpoint has a real local production END");
    const auto initial = fixture.cDevice->dev().captureQueryPollSnapshot(query->obj);
    const auto prefix = initial.backendFence.snapshot();
    std::uint32_t sentinel = 0xdeadbeefu;
    check(initial.issueOrdinal == 1u && initial.issueDeviceEpoch != 0u &&
              prefix.queue != 0u && prefix.sequence == 0u &&
              fixture.cDevice->dev().pollQuerySnapshot(initial, &sentinel, 4u, 0u) == D3D_OK &&
              sentinel == 0xdeadbeefu,
          "empty END is ready by real local authority and defines no EVENT result bytes");
    fixture.routing->submitClear(ClearDesc{});
    check(dxmt9c_query_issue(query, 1u) == D3D_OK && Access::publishWriting(fixture.routing->queue_),
          "recorded EVENT reissue publishes its actual nonempty prefix");
    const auto reissued = fixture.cDevice->dev().captureQueryPollSnapshot(query->obj);
    const auto ready = Access::peekReadyCompletion(fixture.routing->queue_);
    const tape::RenderTapeCompletionKey key{
        1u, ready.seqId, ready.source.id.index, 0u, ready.source.id.generation};
    tape::RenderTapeCompletionSchedule schedule;
    bool earlyAcknowledgment = false;
    check(schedule.append({key}).accepted() && schedule.beginIteration(1u).accepted(),
          "EVENT nonempty END binds a bounded production completion schedule");
    check(reissued.issueOrdinal == 2u && reissued.issueOrdinal != initial.issueOrdinal &&
              fixture.cDevice->dev().pollQuerySnapshot(reissued, &sentinel, 4u, 1u) == S_FALSE &&
              !schedule.release(key, [&] { earlyAcknowledgment = true; return true; }).accepted() &&
              !earlyAcknowledgment,
          "old completed checkpoint cannot satisfy reissued END or early acknowledgment");
    check(schedule.submit(key).accepted() &&
              Access::submitOneWithheld(fixture.routing->queue_).submitted &&
              fixture.cDevice->dev().pollQuerySnapshot(reissued, &sentinel, 4u, 0u) == S_FALSE &&
              schedule.release(key, [&] { return Access::acknowledgeOne(fixture.routing->queue_); }).accepted() &&
              Access::finishQueue(fixture.routing->queue_) &&
              fixture.cDevice->dev().pollQuerySnapshot(reissued, &sentinel, 4u, 0u) == D3D_OK &&
              sentinel == 0xdeadbeefu,
          "submission alone is Pending; controlled completion establishes EVENT readiness");
    dxmt9c_query_release(query);
    PresentParameters reset{};
    reset.backBufferWidth = reset.backBufferHeight = 16u;
    reset.backBufferFormat = Format::A8R8G8B8;
    reset.backBufferCount = 1u;
    reset.windowed = true;
    check(fixture.cDevice->dev().reset(reset) == D3D_OK &&
              fixture.cDevice->dev().pollQuerySnapshot(initial, &sentinel, 4u, 0u) == D3DERR_DEVICELOST &&
              sentinel == 0xdeadbeefu, "device reset invalidates retained checkpoint authority");
  }
  const auto captured = tape::test::makeGeneralCpuEventCheckpointFixture();
  std::vector<tape::RenderTapeProviderBlob> blobs;
  for (const auto& bytes : captured.blobs)
    blobs.push_back({tape::RenderTapeCaptureSession::sha256(bytes), bytes});
  runtime::GeneralPreflightPlan plan;
  check(runtime::buildGeneralReplayPlan(captured.tape, blobs, plan).complete(),
        "EVENT checkpoint fixture passes shared structural preflight");
  const auto admission = runtime::admitGeneralCpuPlan(plan);
  check(admission.accepted() && admission.counts.queryIssues == 1u &&
            admission.counts.queryPolls == 2u,
        "no fabricated captured END enters the original transcript: " +
            std::string(runtime::generalCpuAdmissionReasonName(admission.reason)));
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE, "EVENT tape needs CPU-visible draw resources only");
  std::uint64_t semantic = 0u, querySemantic = 0u;
  for (bool measured : {false, true})
    for (bool compatibility : {true, false}) {
      RuntimeFixture fixture(false, false, true, false, WMT::Device{resourceDevice.handle});
      Executor executor(fixture, plan, blobs, {
          .route = compatibility ? tape::ProviderReplayPolicy::Compatibility
                                 : tape::ProviderReplayPolicy::ProductionSlot,
          .measurement = measured});
      const auto result = executor.run();
      check(result.complete && result.resetOk, "EVENT checkpoint CPU replay: " + result.failure);
      const auto& evidence = result.queryReplay;
      check(evidence.checkpointDefinitions == 1u && evidence.checkpointSetupEnds == 1u &&
                evidence.checkpointSetupPublications == 0u && evidence.checkpointSetupPolls == 1u &&
                evidence.checkpointSetupWaits == 0u && evidence.checkpointCompleted == 1u &&
                evidence.checkpointActivated == 1u && evidence.replayedIssueRecords == 1u &&
                evidence.replayedPolls == 2u && evidence.readinessSatisfied == 2u &&
                evidence.requiredResults == 0u && evidence.matchedResults == 0u &&
                !evidence.actualMetal && !evidence.gpuResultOracle &&
                result.commands == admission.counts.records + admission.counts.bootstrapRecords &&
                result.resourcesCreated == result.resourcesReleased &&
                result.submitted == result.completed && result.completed == result.reclaimed,
            "setup conservation is separate from original command/source/query obligations");
      if (measured) {
        check(evidence.rows.empty() && evidence.checkpoints.empty() && result.fullOracleCalls == 0u &&
                  result.querySemanticDigest == 0u,
              "measurement retains cheap checkpoint counts without semantic scans");
      } else {
        check(evidence.checkpoints.size() == 1u && evidence.checkpoints[0].activated &&
                  evidence.checkpoints[0].capturedIssueCycle == 47u &&
                  evidence.checkpoints[0].capturedIssueOrdinal == 63u &&
                  evidence.checkpoints[0].localIssueOrdinal == 1u &&
                  evidence.rows.size() == 2u && evidence.rows[0].issueCycle == 47u &&
                  evidence.rows[0].localIssueOrdinal == 1u && evidence.rows[0].localSequence == 0u &&
                  evidence.rows[1].issueCycle == 48u && evidence.rows[1].localIssueOrdinal == 2u &&
                  evidence.rows[1].flags == 1u && evidence.rows[0].definedBytes == 0u &&
                  evidence.rows[1].definedBytes == 0u,
              "JIT activation preserves absolute captured cycles and new local reissue authority");
        if (!semantic) { semantic = result.digest; querySemantic = result.querySemanticDigest; }
        check(result.digest == semantic && result.querySemanticDigest == querySemantic,
              "both production routes preserve draw and original query transcript semantics");
      }
    }
  for (auto fault : {Executor::CheckpointFault::MissingEnd, Executor::CheckpointFault::StaleLocalIssue,
                     Executor::CheckpointFault::DoubleRestore}) {
    RuntimeFixture fixture(false, false, true, false, WMT::Device{resourceDevice.handle});
    Executor executor(fixture, plan, blobs, {.checkpointFault = fault});
    const auto result = executor.run();
    check(!result.complete && result.resetOk && result.commands == 0u &&
              result.queryReplay.checkpointActivated == 0u &&
              result.failure == "general-executor-checkpoint-local-authority",
          "missing/stale/double local END rejects before the transcript and cleans hidden owners");
  }
}

void generalCpuD24S8CheckpointRetainsBothLogicalPlanes() {
  namespace tape = dxmt9::d3d9;
  namespace runtime = tape::render_tape_runtime;
  using Fault = tape::test::CpuD24S8FixtureFault;
  using Variant = tape::test::CpuD24S8FixtureVariant;
  for (auto fault : {Fault::ShortSeed, Fault::ExtraSeed, Fault::PackedFourByteSeed,
                     Fault::WrongFormat, Fault::Multisampled, Fault::MissingSeed}) {
    const auto fixture = tape::test::makeGeneralCpuD24S8Fixture(fault);
    check(!fixture.tape.empty(), "D24S8 negative fixture has actual tape bytes");
    std::vector<tape::RenderTapeProviderBlob> blobs;
    for (const auto& bytes : fixture.blobs)
      blobs.push_back({tape::RenderTapeCaptureSession::sha256(bytes), bytes});
    runtime::GeneralPreflightPlan plan;
    const auto preflight = runtime::buildGeneralReplayPlan(fixture.tape, blobs, plan);
    check(!preflight.complete() && plan.events.empty() && plan.definitions.empty(),
          "D24S8 wrong descriptor/plane extent rejects transactionally before effects: " +
              std::to_string(static_cast<unsigned>(fault)) + " " +
              tape::frameTapeReplayStatusName(preflight.status));
  }
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE,
        "CPU D24S8 checkpoint needs CPU-visible resource backing only");
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<
      RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
  std::uint64_t originalDigest = 0u;
  std::vector<dxmt9::test::CpuGeneralTapeDrawObservation> originalDraws;
  for (auto variant : {Variant::Original, Variant::DepthOnly, Variant::StencilOnly}) {
    const auto fixture = tape::test::makeGeneralCpuD24S8Fixture(Fault::None, variant);
    std::vector<tape::RenderTapeProviderBlob> blobs;
    for (const auto& bytes : fixture.blobs)
      blobs.push_back({tape::RenderTapeCaptureSession::sha256(bytes), bytes});
    runtime::GeneralPreflightPlan plan;
    const auto preflight = runtime::buildGeneralReplayPlan(fixture.tape, blobs, plan);
    check(preflight.complete(), std::string("D24S8 shared CPU preflight: ") +
          tape::frameTapeReplayStatusName(preflight.status));
    const auto admission = runtime::admitGeneralCpuPlan(plan);
    check(admission.accepted() && admission.counts.queryDefinitions == 1u &&
              admission.counts.queryIssues == 4u && admission.counts.queryPolls == 4u &&
              admission.counts.bootstrapRecords == 1u,
          "D24S8 checkpoint is a complete initial seed, not source/query/bootstrap work");
    std::uint64_t semantic = 0u, querySemantic = 0u;
    for (bool measured : {false, true}) {
      for (bool compatibility : {true, false}) {
        RuntimeFixture runtimeFixture(false, false, true, false,
                                      WMT::Device{resourceDevice.handle});
        Executor executor(runtimeFixture, plan, blobs, {
            .route = compatibility ? tape::ProviderReplayPolicy::Compatibility
                                   : tape::ProviderReplayPolicy::ProductionSlot,
            .measurement = measured});
        const auto result = executor.run();
        check(result.complete && result.resetOk,
              "CPU D24S8 logical seed+draw/query replay: " + result.failure);
        check(result.draws == 2u && result.observedMutations == plan.mutations.size() &&
                  result.observedTextureUploads == 0u && result.submitted == 3u &&
                  result.submitted == result.completed && result.completed == result.reclaimed &&
                  result.resourcesCreated == result.resourcesReleased &&
                  result.commands == admission.counts.records +
                      admission.counts.bootstrapRecords &&
                  result.sourceSummaryCommands == admission.counts.records -
                      admission.counts.queryIssues &&
                  result.sourceSummaryDraws == 2u &&
                  result.queryReplay.readinessSatisfied == 2u &&
                  !result.queryReplay.actualMetal && !result.queryReplay.gpuResultOracle &&
                  result.queryReplay.replayedIssueRecords == 4u &&
                  result.queryReplay.replayedPolls == 4u,
              "both planes retain logical provenance, exact routes, query obligations and owner conservation");
        if (measured) {
          check(result.digest == 0u && result.fullOracleCalls == 0u &&
                    result.drawObservations.empty() && result.queryReplay.rows.empty(),
                "D24S8 measurement adds no physical seed, readback or semantic observations");
        } else {
          check(result.drawObservations.size() == 2u &&
                    runtimeFixture.routing->ordinaryDrawCalls ==
                        (compatibility ? 2u : 0u),
                "D24S8 checkpoint reaches both requested production draw routes");
          if (semantic == 0u) {
            semantic = result.digest;
            querySemantic = result.querySemanticDigest;
          }
          check(result.digest == semantic && result.querySemanticDigest == querySemantic,
                "exact canonical checkpoint preserves cross-route draw/query semantics");
          if (variant == Variant::Original && originalDraws.empty())
            originalDraws = result.drawObservations;
          if (variant != Variant::Original) {
            check(result.digest != originalDigest,
                  "depth-only or stencil-only changed canonical plane changes semantic digest");
            for (std::size_t draw = 0u; draw < originalDraws.size(); ++draw)
              check(result.drawObservations[draw].cumulativeSemanticDigest !=
                        originalDraws[draw].cumulativeSemanticDigest,
                    "every depth-bound draw consumes the full depth+stencil digest");
          }
        }
      }
    }
    if (variant == Variant::Original) originalDigest = semantic;
  }
}

void generalCpuVolumeWriterAndUploadShapeAreExact() {
  namespace tape = dxmt9::d3d9;
  namespace cpu = dxmt9::test::detail;
  for (auto format : {tape::render_tape_d3d_format::A8R8G8B8,
                      tape::render_tape_d3d_format::DXT1,
                      tape::render_tape_d3d_format::DXT5}) {
    const auto fixture = tape::test::makeGeneralCpuVolumeFixture(format);
    check(!fixture.tape.empty(), "volume writer fixture constructs complete bytes");
    for (std::uint32_t mip = 0u; mip < fixture.levels.size(); ++mip) {
      const auto& desc = fixture.levels[mip];
      tape::RenderTapeVolumeLockLayout canonical{};
      check(tape::renderTapeVolumeContentLayout(desc, canonical) ==
                tape::RenderTapeVolumeLayoutStatus::Accepted,
            "volume mip has a shared canonical layout");
      cpu::CpuGeneralTextureUploadShape shape{};
      check(cpu::cpuGeneralTextureUploadShape(desc, shape) &&
                shape.width == desc.width && shape.height == desc.height &&
                shape.depth == desc.depth && shape.pitch == canonical.fullRowBytes &&
                shape.slicePitch == canonical.fullSliceBytes &&
                shape.bytes == canonical.fullBytes,
            "upload shape binds mip extents and packed storage rows/slices");
      check(cpu::cpuGeneralTextureUploadShapeMatches(shape, shape.width,
                shape.height, shape.depth, shape.pitch, shape.slicePitch,
                static_cast<std::size_t>(shape.bytes)),
            "exact captured upload shape is accepted");
      for (unsigned field = 0u; field < 6u; ++field) {
        auto wrong = shape;
        switch (field) {
        case 0u: ++wrong.width; break;
        case 1u: ++wrong.height; break;
        case 2u: ++wrong.depth; break;
        case 3u: ++wrong.pitch; break;
        case 4u: ++wrong.slicePitch; break;
        case 5u: ++wrong.bytes; break;
        }
        check(!cpu::cpuGeneralTextureUploadShapeMatches(shape, wrong.width,
                  wrong.height, wrong.depth, wrong.pitch, wrong.slicePitch,
                  static_cast<std::size_t>(wrong.bytes)),
              "every upload dimension, stride, and byte extent is checked");
      }
      for (bool shadow : {false, true}) {
        const auto slicePitch = shadow
            ? static_cast<std::uint64_t>(canonical.pitch) * desc.height
            : canonical.fullSliceBytes;
        std::vector<std::byte> storage(
            static_cast<std::size_t>(slicePitch * desc.depth) + 16u, std::byte{0x5a});
        D9CTexture texture({}, nullptr);
        if (shadow) {
          auto& lockedShadow = texture.wow64Locks[mip];
          lockedShadow.active = true;
          lockedShadow.shadow.ptr = storage.data();
          lockedShadow.shadowSlicePitch = static_cast<std::size_t>(slicePitch);
        }
        const D9CLockedRect lock{
            .pitch = static_cast<std::int32_t>(canonical.pitch), .bits = storage.data()};
        check(cpu::cpuGeneralWriteTextureRows(desc, texture, mip, lock,
                  fixture.seeds[mip]),
              "CPU volume writer copies every packed or shadow-strided slice");
        tape::RenderTapeVolumeLockLayout locked{};
        std::vector<std::byte> roundtrip;
        check(tape::renderTapeVolumeLockLayout(desc, lock.pitch,
                  static_cast<std::int64_t>(slicePitch), nullptr, locked) ==
                  tape::RenderTapeVolumeLayoutStatus::Accepted &&
                  tape::copyRenderTapeVolumeRows(storage.data(), locked, roundtrip) &&
                  roundtrip == fixture.seeds[mip],
              "every mip/slice matches its exact canonical seed");
        for (std::uint32_t z = 0u; z < desc.depth; ++z)
          for (auto offset = canonical.fullSliceBytes; offset < slicePitch; ++offset)
            check(storage[static_cast<std::size_t>(z * slicePitch + offset)] ==
                      std::byte{0x5a}, "compressed shadow slice padding stays untouched");
        check(std::all_of(storage.end() - 16u, storage.end(), [](std::byte byte) {
                return byte == std::byte{0x5a}; }),
              "volume writer cannot write beyond its full depth extent");
        const auto before = storage;
        check(!cpu::cpuGeneralWriteTextureRows(desc, texture, mip, lock,
                  std::span(fixture.seeds[mip]).first(fixture.seeds[mip].size() - 1u)) &&
                  storage == before,
              "short canonical volume input rejects before copying any slice");
      }
    }
    tape::RenderTapeVolumeLockLayout level0{};
    check(tape::renderTapeVolumeContentLayout(fixture.levels[0], level0) ==
              tape::RenderTapeVolumeLayoutStatus::Accepted &&
              fixture.changedLevel0 != fixture.seeds[0] &&
              std::equal(fixture.seeds[0].begin(),
                         fixture.seeds[0].begin() + level0.fullSliceBytes,
                         fixture.changedLevel0.begin()),
          "nonzero-z captured mutation changes later slices and preserves slice zero");
  }
  D9CSurfaceDesc invalid{.format = tape::render_tape_d3d_format::A8R8G8B8,
      .resourceType = 4u, .width = 7u, .height = 5u, .depth = 3u};
  tape::RenderTapeVolumeLockLayout layout{};
  check(tape::renderTapeVolumeLockLayout(invalid, 28, 139, nullptr, layout) ==
            tape::RenderTapeVolumeLayoutStatus::InvalidPitch && layout.fullBytes == 0u,
        "short slice stride rejects before publishing a usable layout");
  check(tape::renderTapeVolumeLockLayout(invalid, 28,
            std::numeric_limits<std::int64_t>::max(), nullptr, layout) ==
            tape::RenderTapeVolumeLayoutStatus::Overflow && layout.fullBytes == 0u,
        "source slice offset overflow rejects before any CPU copy");
  invalid.width = std::numeric_limits<std::uint32_t>::max();
  cpu::CpuGeneralTextureUploadShape shape{};
  check(tape::renderTapeVolumeContentLayout(invalid, layout) ==
            tape::RenderTapeVolumeLayoutStatus::Overflow &&
            !cpu::cpuGeneralTextureUploadShape(invalid, shape) && shape.bytes == 0u,
        "unrepresentable Volume extent cannot authorize an upload shape");
}

void generalCpuBootstrapPagesUseProductionReplay() {
  namespace tape = dxmt9::d3d9;
  namespace runtime = tape::render_tape_runtime;
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE,
        "paged CPU bootstrap needs CPU-visible resources, not GPU work");
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<
      RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
  std::uint64_t originalDigest = 0u;
  for (bool alternateTail : {false, true}) {
    const auto fixture = tape::test::makeGeneralCpuBootstrapPagesFixture(alternateTail);
    std::vector<tape::RenderTapeProviderBlob> blobs;
    for (const auto& bytes : fixture.blobs)
      blobs.push_back({tape::RenderTapeCaptureSession::sha256(bytes), bytes});
    runtime::GeneralPreflightPlan plan;
    const auto preflight = runtime::buildGeneralReplayPlan(fixture.tape, blobs, plan);
    check(preflight.complete(), std::string("paged bootstrap shared preflight: ") +
          tape::frameTapeReplayStatusName(preflight.status));
    const auto admission = runtime::admitGeneralCpuPlan(plan);
    check(admission.accepted() && admission.counts.bootstrapChunks == 1u &&
              admission.counts.bootstrapRecords == 3u &&
              admission.counts.records == preflight.coverage.commandRecords &&
              admission.counts.queryIssues == 0u && admission.counts.queryPolls == 0u &&
              tape::pe::kRenderTapeBootstrapMaxRecords <=
                  runtime::GeneralCpuAdmissionCaps{}.maxRecordsPerChunk,
          "three real bootstrap records remain separate from commands/query/source counts");
    std::uint64_t semantic = 0u;
    for (bool measured : {false, true}) {
      for (bool compatibility : {true, false}) {
        RuntimeFixture runtimeFixture(false, false, true, false,
                                      WMT::Device{resourceDevice.handle});
        Executor executor(runtimeFixture, plan, blobs, {
            .route = compatibility ? tape::ProviderReplayPolicy::Compatibility
                                   : tape::ProviderReplayPolicy::ProductionSlot,
            .measurement = measured});
        const auto result = executor.run();
        check(result.complete && result.resetOk,
              "paged bootstrap production CPU replay: " + result.failure);
        check(result.commands == admission.counts.records +
                                     admission.counts.bootstrapRecords &&
                  result.draws == 2u &&
                  result.sourceSummaryCommands == admission.counts.records &&
                  result.sourceSummaryDraws == admission.counts.draws &&
                  result.resourcesCreated == result.resourcesReleased &&
                  result.submitted == result.completed &&
                  result.completed == result.reclaimed,
              "executor total equals separate ordinary/bootstrap counts and all owners settle");
        if (measured) {
          check(result.fullOracleCalls == 0u && result.drawObservations.empty(),
                "paged measurement keeps conservation without semantic rows");
        } else {
          if (semantic == 0u) semantic = result.digest;
          check(result.digest == semantic && result.drawObservations.size() == 2u,
                "paged state has identical verification semantics across actual routes");
        }
      }
    }
    if (!alternateTail) originalDigest = semantic;
    else check(semantic != originalDigest,
               "last sampler page contributes to the actual draw-state semantic oracle");
  }
}

void generalCpuVolumeTapeUsesProductionCreationAndMutation() {
  namespace tape = dxmt9::d3d9;
  namespace runtime = tape::render_tape_runtime;
  using Fault = tape::test::CpuVolumeFixtureFault;
  for (auto fault : {Fault::WrongMipDepth, Fault::WrongResourceType,
                     Fault::MissingSliceBytes}) {
    const auto fixture = tape::test::makeGeneralCpuVolumeFixture(
        tape::render_tape_d3d_format::A8R8G8B8, fault);
    std::vector<tape::RenderTapeProviderBlob> blobs;
    for (const auto& bytes : fixture.blobs)
      blobs.push_back({tape::RenderTapeCaptureSession::sha256(bytes), bytes});
    runtime::GeneralPreflightPlan plan;
    const auto preflight = runtime::buildGeneralReplayPlan(fixture.tape, blobs, plan);
    check(!preflight.complete() && plan.events.empty(),
          "volume negative fixture " + std::to_string(static_cast<unsigned>(fault)) +
              " rejects before effects: " + tape::frameTapeReplayStatusName(preflight.status));
  }
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE,
        "CPU volume tape needs actual resource backing, not a pixel oracle");
  {
    RuntimeFixture mismatch(false, false, true, false,
                            WMT::Device{resourceDevice.handle});
    auto volume = mismatch.cDevice->dev().createTexture(TextureDesc{
        .width = 7u, .height = 5u, .depth = 3u, .levels = 3u,
        .format = Format::A8R8G8B8, .type = TextureType::Volume,
        .pool = Pool::Default});
    check(volume && volume->handle(), "upload mismatch test has real Volume backing");
    const std::vector<std::uint8_t> bytes(7u * 5u * 3u * 4u, 0x5au);
    mismatch.routing->uploadTextureLevel(volume->handle(), 0u, 7u, 5u, 1u,
                                        28u, 140u, bytes);
    check(mismatch.routing->textureUploadFailed &&
              mismatch.routing->textureUploadObservations.empty(),
          "2D-shaped upload cannot authorize a three-slice Volume receipt");
  }
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<
      RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
  std::uint64_t originalDigest = 0u;
  for (bool alternateMutation : {false, true}) {
    const auto fixture = tape::test::makeGeneralCpuVolumeFixture(
        tape::render_tape_d3d_format::A8R8G8B8, Fault::None, alternateMutation);
    std::vector<tape::RenderTapeProviderBlob> blobs;
    for (const auto& bytes : fixture.blobs)
      blobs.push_back({tape::RenderTapeCaptureSession::sha256(bytes), bytes});
    runtime::GeneralPreflightPlan plan;
    const auto preflight = runtime::buildGeneralReplayPlan(fixture.tape, blobs, plan);
    check(preflight.complete(), std::string("volume shared preflight: ") +
          tape::frameTapeReplayStatusName(preflight.status));
    const auto admission = runtime::admitGeneralCpuPlan(plan);
    check(admission.accepted(), std::string("volume CPU grammar: ") +
          runtime::generalCpuAdmissionReasonName(admission.reason));
    std::uint64_t semantic = 0u;
    for (unsigned scenario : {0u, 1u, 2u}) {
      const bool delayed = scenario == 1u;
      const bool measured = scenario == 2u;
      for (bool compatibility : {true, false}) {
        RuntimeFixture runtimeFixture(false, false, true, false,
                                      WMT::Device{resourceDevice.handle});
        Executor executor(runtimeFixture, plan, blobs, {
            .route = compatibility ? tape::ProviderReplayPolicy::Compatibility
                                   : tape::ProviderReplayPolicy::ProductionSlot,
            .beforeNextWork = delayed, .measurement = measured});
        const auto result = executor.run();
        check(result.complete && result.resetOk,
              "production CPU volume replay: " + result.failure);
        check(result.draws == 2u && result.observedTextureUploads == 4u &&
                  result.observedMutations == plan.mutations.size() &&
                  !runtimeFixture.routing->textureUploadFailed &&
                  result.resourcesCreated == result.resourcesReleased &&
                  result.submitted == result.completed &&
                  result.completed == result.reclaimed,
              "all mip seeds and nonzero-z mutation reach exact production uploads and settle");
        if (measured) {
          check(result.fullOracleCalls == 0u && result.drawObservations.empty(),
                "volume measurement retains shape/conservation checks and omits semantic rows");
        } else {
          check(result.drawObservations.size() == 2u,
                "two actual draw observations consume the volume receipt closure");
          if (!semantic) semantic = result.digest;
          check(result.digest == semantic,
                "Volume draw semantics are invariant under route and completion timing");
        }
      }
    }
    if (!alternateMutation) originalDigest = semantic;
    else check(semantic != originalDigest,
               "later-slice volume content changes the actual draw semantic oracle");
  }
}

void generalCpuMipmapUsesProductionLifetime() {
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  const auto tape = dxmt9::d3d9::test::makeGeneralCpuMipmapFixture();
  std::vector<dxmt9::d3d9::RenderTapeProviderBlob> blobs;
  for (const auto& bytes : tape.blobs)
    blobs.push_back({dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes), bytes});
  runtime::GeneralPreflightPlan plan;
  const auto preflight = runtime::buildGeneralReplayPlan(tape.tape, blobs, plan);
  check(preflight.complete(), "mipmap fixture preflight: " +
      std::string(dxmt9::d3d9::frameTapeReplayStatusName(preflight.status)));
  check(runtime::admitGeneralCpuPlan(plan).accepted(), "mipmap CPU grammar admission");
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE, "mipmap fixture needs resource backing");
  std::uint64_t expectedDigest = 0u;
  for (bool delayed : {false, true}) {
    for (bool compatibility : {true, false}) {
      PresentParameters output{};
      output.backBufferWidth = plan.outputDesc.width;
      output.backBufferHeight = plan.outputDesc.height;
      output.backBufferFormat = dxmt9::d3d9::devicec::fmtFromD3D(plan.outputDesc.format);
      output.backBufferCount = 1u;
      output.windowed = true;
      output.deviceWindow = Handle{91};
      output.presentationInterval = PresentInterval::Immediate;
      RuntimeFixture fixture(false, false, true, false,
                             WMT::Device{resourceDevice.handle}, nullptr, &output);
      using Executor = dxmt9::test::CpuGeneralTapeExecutor<
          RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
      Executor executor(fixture, plan, blobs, {
          .route = compatibility ? dxmt9::d3d9::ProviderReplayPolicy::Compatibility
                                 : dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot,
          .beforeNextWork = delayed});
      const auto result = executor.run();
      check(result.complete && result.resetOk, "mipmap production replay: " + result.failure);
      check(result.mipmapCommands == tape.expectedMipmapCommandCount &&
                result.draws == tape.expectedDrawCount,
            "canonical mip command must reach the actual final Slot exactly once");
      check(result.submitted == 3u && result.completed == 3u && result.reclaimed == 3u,
            "draw, mipmap, and Present sources complete and reclaim exactly once");
      check(delayed ? result.retainedMipmapTexturesAfterDestroy > 0u
                    : result.retainedMipmapTexturesAfterDestroy == 0u,
            "delayed completion must retain the destroyed mip texture non-vacuously");
      if (!expectedDigest) expectedDigest = result.digest;
      check(result.digest == expectedDigest,
            "mipmap semantics must be invariant under route and completion timing");
    }
  }
}

void generalCpuDestroyWindowForBacking(bool dynamicVertex, bool dynamicIndex) {
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  const auto tape = dxmt9::d3d9::test::makeGeneralCpuDestroyFixture(dynamicVertex, dynamicIndex);
  std::vector<dxmt9::d3d9::RenderTapeProviderBlob> blobs;
  for (const auto& bytes : tape.blobs)
    blobs.push_back({dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes), bytes});
  runtime::GeneralPreflightPlan plan;
  const auto preflight = runtime::buildGeneralReplayPlan(tape.tape, blobs, plan);
  check(preflight.complete(), std::string("destroy window shared preflight: ") +
        dxmt9::d3d9::frameTapeReplayStatusName(preflight.status));
  const auto admission = runtime::admitGeneralCpuPlan(plan);
  check(admission.accepted(), std::string("destroy window CPU admission: ") +
        runtime::generalCpuAdmissionReasonName(admission.reason));
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE, "destroy window needs real buffer backing");
  std::uint64_t expectedDigest = 0u;
  for (unsigned scenario : {0u, 1u, 2u}) {
    const bool delayed = scenario != 0u;
    const bool corrupt = scenario == 2u;
    for (bool compatibility : {true, false}) {
      std::cerr << "general destroy window scenario=" << scenario
                << " compatibility=" << compatibility << '\n';
      PresentParameters output{};
      output.backBufferWidth = plan.outputDesc.width;
      output.backBufferHeight = plan.outputDesc.height;
      output.backBufferFormat = dxmt9::d3d9::devicec::fmtFromD3D(plan.outputDesc.format);
      output.backBufferCount = 1u;
      output.windowed = true;
      output.deviceWindow = Handle{91};
      output.presentationInterval = PresentInterval::Immediate;
      RuntimeFixture fixture(false, false, true, false,
                             WMT::Device{resourceDevice.handle}, nullptr, &output);
      using Executor = dxmt9::test::CpuGeneralTapeExecutor<
          RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
      Executor executor(fixture, plan, blobs, {
          .route = compatibility ? dxmt9::d3d9::ProviderReplayPolicy::Compatibility
                                 : dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot,
          .beforeNextWork = delayed,
          .corruptRetainedBytesForTest = corrupt});
      const auto result = executor.run();
      if (corrupt) {
        check(!result.complete && result.resetOk &&
                  result.failure == "general-executor-retained-buffer-bytes-changed",
              "actual backing corruption must fail the lifecycle oracle and clean up: " + result.failure);
        continue;
      }
      check(result.complete && result.resetOk, "general destroy window: " + result.failure);
      check(result.trace.size() == 2u && result.trace[0].bufferClosure.size() == 2u &&
                result.trace[1].bufferClosure.empty(),
            "production visitor projects VB/IB identities, not unrelated raw handles");
      const auto& vb = dxmt9::d3d9::test::general_cpu_destroy_fixture_detail::kVertexBuffer;
      const auto& ib = dxmt9::d3d9::test::general_cpu_destroy_fixture_detail::kIndexBuffer;
      for (const auto& identity : result.trace[0].bufferClosure)
        check(dxmt9::test::detail::cpuGeneralSameIdentity(identity, vb) ||
                  dxmt9::test::detail::cpuGeneralSameIdentity(identity, ib),
              "source buffer closure retains exact Tape object/generation identity");
      const auto captured = std::size_t(dynamicVertex) + std::size_t(dynamicIndex);
      check(result.observedSnapshots == captured && result.observedLiveBindings == 2u - captured,
            "each effective VB/IB must use its production captured or live backing");
      check(result.draws == 1u && result.submitted == 2u &&
                result.completed == 2u && result.reclaimed == 2u,
            "actual draw and Present sources must each complete exactly once");
      check(result.pendingPeak == 1u &&
                result.destroysWithPending == (delayed ? 2u : 0u) &&
                result.deferredReleases == (delayed ? 2u : 0u) &&
                result.retainedSnapshotsAfterDestroy == (delayed ? 4u : 0u),
            "destroy window must retain actual submitted snapshot bytes non-vacuously");
      if (!expectedDigest) expectedDigest = result.digest;
      check(result.digest == expectedDigest,
            "completion timing and replay route preserve final Slot semantics");
    }
  }
}

void generalCpuDestroyWindowUsesProductionLifetime() {
  for (bool dynamicVertex : {true, false})
    for (bool dynamicIndex : {true, false})
      generalCpuDestroyWindowForBacking(dynamicVertex, dynamicIndex);
}

void generalCpuPendingOwnerRejectsEveryStaleIdentityDimension() {
  namespace rt = dxmt9::d3d9;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<
      RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
  using Fault = Executor::PendingIdentityFault;
  const auto tape = rt::test::makeGeneralCpuDestroyFixture(true, true);
  std::vector<rt::RenderTapeProviderBlob> blobs;
  for (const auto& bytes : tape.blobs)
    blobs.push_back({rt::RenderTapeCaptureSession::sha256(bytes), bytes});
  rt::render_tape_runtime::GeneralPreflightPlan plan;
  check(rt::render_tape_runtime::buildGeneralReplayPlan(tape.tape, blobs, plan).complete() &&
            rt::render_tape_runtime::admitGeneralCpuPlan(plan).accepted(),
        "pending identity fixture uses authenticated general grammar");
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE, "pending identity fixture Metal device");
  for (const auto route : {rt::ProviderReplayPolicy::Compatibility,
                           rt::ProviderReplayPolicy::ProductionSlot}) {
    for (const bool delayed : {false, true}) {
      for (const auto fault : {Fault::None, Fault::Sequence, Fault::SourceIndex,
                               Fault::SourceGeneration, Fault::StorageFirstPage,
                               Fault::StoragePageCount, Fault::StorageGeneration}) {
        PresentParameters output{};
        output.backBufferWidth = plan.outputDesc.width;
        output.backBufferHeight = plan.outputDesc.height;
        output.backBufferFormat = rt::devicec::fmtFromD3D(plan.outputDesc.format);
        output.backBufferCount = 1u;
        output.windowed = true;
        output.deviceWindow = Handle{91};
        output.presentationInterval = PresentInterval::Immediate;
        RuntimeFixture fixture(false, false, true, false,
                               WMT::Device{resourceDevice.handle}, nullptr, &output);
        Executor executor(fixture, plan, blobs, {
            .route = route, .beforeNextWork = delayed, .pendingIdentityFault = fault});
        const auto result = executor.run();
        if (fault == Fault::None) {
          check(result.complete && result.resetOk && result.completed == 2u,
                "exact pending identity completes normally: " + result.failure);
        } else {
          check(!result.complete && result.resetOk && result.submitted == 1u &&
                    result.completed == 0u && result.reclaimed == 0u &&
                    result.trace.size() == 1u && !result.trace.front().completed &&
                    result.failure == "general-executor-release-identity-changed",
                "each stale owner dimension rejects before normal acknowledgment: " + result.failure);
        }
      }
    }
  }
}

void generalCpuWindowReplaysAuthenticatedSourcesInFifoOrder() {
  namespace rt = dxmt9::d3d9;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<
      RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
  using Mutation = rt::test::GeneralCpuMutationFixture;
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE, "general window needs real buffers");
  for (const auto& [mutation, clearBeforeExtraDraws] : std::array{
           std::pair{Mutation::None, false}, std::pair{Mutation::None, true},
           std::pair{Mutation::ReferencedPlain, false}, std::pair{Mutation::ReferencedPlain, true},
           std::pair{Mutation::ReferencedDiscard, false}, std::pair{Mutation::ReferencedDiscard, true}}) {
    const auto tape = rt::test::makeGeneralCpuDestroyFixture(
        true, true, mutation, rt::test::GeneralCpuResetFixture::A, 2u, clearBeforeExtraDraws);
    std::vector<rt::RenderTapeProviderBlob> blobs;
    for (const auto& bytes : tape.blobs)
      blobs.push_back({rt::RenderTapeCaptureSession::sha256(bytes), bytes});
    rt::render_tape_runtime::GeneralPreflightPlan plan;
    check(rt::render_tape_runtime::buildGeneralReplayPlan(tape.tape, blobs, plan).complete() &&
              rt::render_tape_runtime::admitGeneralCpuPlan(plan).accepted(),
          "window fixture is a complete authenticated Tape, not injected slots");
    std::optional<Executor::Result> reference;
    for (const auto route : {rt::ProviderReplayPolicy::Compatibility,
                             rt::ProviderReplayPolicy::ProductionSlot}) {
      for (const auto limit : {1u, 2u}) {
        PresentParameters output{};
        output.backBufferWidth = plan.outputDesc.width;
        output.backBufferHeight = plan.outputDesc.height;
        output.backBufferFormat = rt::devicec::fmtFromD3D(plan.outputDesc.format);
        output.backBufferCount = 1u;
        output.windowed = true;
        output.deviceWindow = Handle{91};
        output.presentationInterval = PresentInterval::Immediate;
        RuntimeFixture fixture(false, false, true, false,
                               WMT::Device{resourceDevice.handle}, nullptr, &output);
        Executor executor(fixture, plan, blobs, {
            .route = route, .beforeNextWork = true, .maxPendingSources = limit});
        const auto result = executor.run();
        check(result.complete && result.resetOk, "general window: " + result.failure);
        const auto draws = mutation == Mutation::None ? 3u : 4u;
        check(result.draws == draws && result.drawObservations.size() == draws &&
                  result.submitted == draws + 1u + std::size_t(clearBeforeExtraDraws) &&
                  result.completed == result.submitted &&
                  result.reclaimed == result.submitted && result.pendingPeak == limit,
              "actual general executor must reach the requested pending window");
        for (std::size_t i = 0u; i < draws; ++i)
          check(result.trace[i + std::size_t(clearBeforeExtraDraws && i > 0u)].bufferClosure.size() == 2u,
                "each pending source owns its own VB/IB closure");
        check(result.trace.back().bufferClosure.empty(), "Present owns no stale draw closure");
        if (limit == 2u) {
          if (clearBeforeExtraDraws) {
            check(result.trace[0].releasedAtEvent == result.trace[1].event &&
                      result.trace[1].releasedAtEvent == result.trace[2].event &&
                      result.trace[2].releasedAtEvent > result.trace[3].event,
                  "Draw A / Clear B / Draw C / Draw D drains B before C; only C/D overlap");
          } else {
            check(result.trace[0].releasedAtEvent == result.trace[2].event &&
                    result.trace[1].releasedAtEvent > result.trace[0].releasedAtEvent,
                "third draw source releases A only; B remains pending");
          }
          check(result.retainedSnapshotsAfterDestroy ==
                    (mutation == Mutation::None ? 8u : 4u),
                "destroy checks every still-pending source's actual captured bytes");
        }
        if (!reference) reference = result;
        check(result.digest == reference->digest, "window preserves final semantics");
        for (std::size_t i = 0u; i < draws; ++i)
          check(result.drawObservations[i].cumulativeSemanticDigest ==
                    reference->drawObservations[i].cumulativeSemanticDigest,
                "window preserves every draw across mutation and destruction");
      }
    }
  }
}

void cpuWindowPlanRejectsMissingShapeDimensions() {
  namespace rt = dxmt9::d3d9;
  const D9CWireObjectIdentity buffer{
      .kind = D9C_CHUNK_HANDLE_KIND_BUFFER, .generation = 1u, .objectId = 909u};
  const auto inspect = [&](const auto& records) {
    const auto wire = makeWireFixture(records);
    rt::ImportedChunkView imported;
    check(rt::importPrevalidatedCommandChunk(wire.bytes, wire.envelope, imported),
          "window plan fixture imports real wire");
    return rt::planRenderTapeCpuWindowSource(imported, 1u, rt::kRenderTapeCpuWindowPageSize);
  };
  const auto valid = inspect(std::array{applyRenderStateRecord(RS_ALPHABLEND_ENABLE, 1u),
                                       drawRecord(buffer)});
  check(valid.eligible(), "Draw/APPLY_STATE can use the one-source overlap plan");
  for (unsigned fault = 0u; fault < 11u; ++fault) {
    auto bad = valid;
    switch (fault) {
    case 0u: bad.recordCount = 65u; break;
    case 1u: --bad.allowedRecordCount; break;
    case 2u: bad.emission.coverageExact = false; break;
    case 3u: bad.emission.directDrawCount = 0u; break;
    case 4u: ++bad.emission.coordinatorCount; break;
    case 5u: ++bad.emission.orderedControlCount; break;
    case 6u: ++bad.emission.compatibilityRecordCount; break;
    case 7u: bad.emission.leaseSpans.front().ownsLease = false; break;
    case 8u: ++bad.emission.leaseSpans.front().firstRecordIndex; break;
    case 9u: --bad.emission.leaseSpans.front().recordCount; break;
    case 10u: bad.emission.leaseSpans.front().finalLeaseSpan = false; break;
    }
    check(!bad.eligible(), "each missing emission shape dimension rejects overlap");
  }
  check(!inspect(std::array{clearRecord(), drawRecord(buffer)}).eligible() &&
            !inspect(std::array{drawRecord(buffer), presentRecord()}).eligible(),
        "coordinator commands remain release boundaries even beside an eligible draw");
  auto fan = drawRecord(buffer);
  D9CCommandChunkWireDrawHeader header{};
  std::memcpy(&header, fan.payload.data(), sizeof(header));
  header.primitiveType = 6u;
  std::memcpy(fan.payload.data(), &header, sizeof(header));
  check(!inspect(std::array{fan}).eligible(), "TriangleFan compatibility conversion is not an overlap proof");
}

void generalCpuMutationWindowUsesProductionLifetime() {
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  using Mutation = dxmt9::d3d9::test::GeneralCpuMutationFixture;
  for (const auto mutation : {Mutation::UnrelatedPlain, Mutation::ReferencedPlain,
                              Mutation::ReferencedDiscard}) {
    // A referenced DISCARD is admitted only for a dynamic buffer. Plain
    // same-buffer writes exercise mandatory completion before the real map.
    for (bool dynamic : {false, true}) {
      if (mutation == Mutation::ReferencedDiscard && !dynamic) continue;
      const auto tape = dxmt9::d3d9::test::makeGeneralCpuDestroyFixture(dynamic, dynamic, mutation);
      std::vector<dxmt9::d3d9::RenderTapeProviderBlob> blobs;
      for (const auto& bytes : tape.blobs)
        blobs.push_back({dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes), bytes});
      runtime::GeneralPreflightPlan plan;
      check(runtime::buildGeneralReplayPlan(tape.tape, blobs, plan).complete(),
            "mutation fixture shared preflight");
      check(runtime::admitGeneralCpuPlan(plan).accepted(), "mutation fixture CPU admission");
      MetalDeviceLease resourceDevice;
      resourceDevice.handle = dxmt9_test_acquire_metal_device();
      check(resourceDevice.handle != NULL_OBJECT_HANDLE, "mutation fixture real backing");
      std::uint64_t expectedDigest = 0u;
      std::vector<dxmt9::test::CpuGeneralTapeDrawObservation> expectedDraws;
      for (unsigned schedule : {0u, 1u, 2u}) {
        const bool hold = schedule != 0u;
        const bool corrupt = schedule == 2u;
        if (corrupt && mutation == Mutation::ReferencedPlain) continue;
        for (bool compatibility : {true, false}) {
          std::cerr << "general mutation=" << unsigned(mutation) << " dynamic=" << dynamic
                    << " hold=" << hold << " compatibility=" << compatibility << '\n';
          PresentParameters output{};
          output.backBufferWidth = plan.outputDesc.width;
          output.backBufferHeight = plan.outputDesc.height;
          output.backBufferFormat = dxmt9::d3d9::devicec::fmtFromD3D(plan.outputDesc.format);
          output.backBufferCount = 1u;
          output.windowed = true;
          output.deviceWindow = Handle{91};
          output.presentationInterval = PresentInterval::Immediate;
          RuntimeFixture fixture(false, false, true, false,
                                 WMT::Device{resourceDevice.handle}, nullptr, &output);
          using Executor = dxmt9::test::CpuGeneralTapeExecutor<
              RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
          Executor executor(fixture, plan, blobs, {
              .route = compatibility ? dxmt9::d3d9::ProviderReplayPolicy::Compatibility
                                     : dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot,
              .beforeNextWork = true,
              .holdSafeBufferMutation = hold,
              .corruptRetainedBytesAfterMutationForTest = corrupt});
          const auto result = executor.run();
          if (corrupt) {
            check(!result.complete && result.resetOk &&
                      result.failure == "general-executor-mutation-changed-pending-bytes",
                  "mutation corruption must reject and clean up: " + result.failure);
            continue;
          }
          check(result.complete && result.resetOk, "mutation window: " + result.failure);
          const bool deferred = hold && mutation != Mutation::ReferencedPlain;
          check(result.deferredBufferMutations == std::size_t(deferred) &&
                    result.retainedBuffersAfterMutation == (deferred ? 2u : 0u) &&
                    result.deferredDiscardMutations ==
                        std::size_t(deferred && mutation == Mutation::ReferencedDiscard) &&
                    result.mutationBoundaryReleases ==
                        std::size_t(hold && mutation == Mutation::ReferencedPlain),
                "mutation admission must execute non-vacuously or release before blocking map");
          check(result.draws == 2u && result.submitted == 3u &&
                    result.completed == 3u && result.reclaimed == 3u && result.pendingPeak == 1u,
                "second draw and Present conserve actual source completion");
          if (!expectedDigest) {
            expectedDigest = result.digest;
            expectedDraws = result.drawObservations;
          }
          check(result.digest == expectedDigest && result.drawObservations.size() == expectedDraws.size(),
                "mutation schedule preserves final semantics");
          for (std::size_t i = 0u; i < expectedDraws.size(); ++i)
            check(result.drawObservations[i].cumulativeSemanticDigest == expectedDraws[i].cumulativeSemanticDigest,
                  "mutation schedule preserves each draw's bytes and generation");
        }
      }
    }
  }
}

std::vector<std::byte> encodeCpuSourceOracleForTest(
    std::span<const CpuSourceOracleRow> rows) {
  std::vector<std::byte> bytes{
      std::byte{'D'}, std::byte{'9'}, std::byte{'C'}, std::byte{'S'},
      std::byte{'R'}, std::byte{'C'}, std::byte{'0'}, std::byte{'1'},
      std::byte{}, std::byte{}, std::byte{}, std::byte{}};
  const auto append32 = [&](std::uint32_t value) {
    for (std::size_t shift = 0u; shift < 32u; shift += 8u)
      bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
  };
  const auto append64 = [&](std::uint64_t value) {
    for (std::size_t shift = 0u; shift < 64u; shift += 8u)
      bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
  };
  bytes[8u] = static_cast<std::byte>(rows.size() & 0xffu);
  bytes[9u] = static_cast<std::byte>((rows.size() >> 8u) & 0xffu);
  bytes[10u] = static_cast<std::byte>((rows.size() >> 16u) & 0xffu);
  bytes[11u] = static_cast<std::byte>((rows.size() >> 24u) & 0xffu);
  for (const auto& row : rows) {
    append32(row.event);
    append32(static_cast<std::uint32_t>(row.identities.size()));
    for (const auto& identity : row.identities) {
      append32(identity.kind);
      append32(identity.generation);
      append64(identity.objectId);
    }
  }
  return bytes;
}

void generalCpuSourceOracleUsesAuthenticatedClosures() {
  using Mutation = dxmt9::d3d9::test::GeneralCpuMutationFixture;
  const auto tape = dxmt9::d3d9::test::makeGeneralCpuDestroyFixture(
      true, true, Mutation::ReferencedDiscard);
  std::vector<dxmt9::d3d9::RenderTapeProviderBlob> blobs;
  for (const auto& bytes : tape.blobs)
    blobs.push_back({dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes), bytes});
  dxmt9::d3d9::render_tape_runtime::GeneralPreflightPlan plan;
  check(dxmt9::d3d9::render_tape_runtime::buildGeneralReplayPlan(
            tape.tape, blobs, plan).complete(),
        "source oracle fixture preflight");
  check(dxmt9::d3d9::render_tape_runtime::admitGeneralCpuPlan(plan).accepted(),
        "source oracle fixture admission");
  std::vector<CpuSourceOracleRow> expected;
  const auto& vertex = dxmt9::d3d9::test::general_cpu_destroy_fixture_detail::kVertexBuffer;
  const auto& index = dxmt9::d3d9::test::general_cpu_destroy_fixture_detail::kIndexBuffer;
  for (const auto& event : plan.events) {
    if (event.type != dxmt9::d3d9::RenderTapeEventType::CommandChunk) continue;
    CpuSourceOracleRow row{.event = event.eventIndex + 1u};
    if (expected.size() < 2u)
      row.identities = {{vertex.kind, vertex.generation, vertex.objectId},
                        {index.kind, index.generation, index.objectId}};
    expected.push_back(std::move(row));
  }
  check(expected.size() == 3u && expected.back().identities.empty(),
        "source oracle includes the empty Present source");
  const auto run = [&](dxmt9::d3d9::ProviderReplayPolicy route,
                       std::vector<CpuSourceOracleRow> rows) {
    MetalDeviceLease resourceDevice;
    resourceDevice.handle = dxmt9_test_acquire_metal_device();
    check(resourceDevice.handle != NULL_OBJECT_HANDLE,
          "source oracle fixture real backing");
    PresentParameters output{};
    output.backBufferWidth = plan.outputDesc.width;
    output.backBufferHeight = plan.outputDesc.height;
    output.backBufferFormat = dxmt9::d3d9::devicec::fmtFromD3D(plan.outputDesc.format);
    output.backBufferCount = 1u;
    output.windowed = true;
    output.deviceWindow = Handle{91};
    output.presentationInterval = PresentInterval::Immediate;
    RuntimeFixture fixture(false, false, true, false,
                           WMT::Device{resourceDevice.handle}, nullptr, &output);
    using Executor = dxmt9::test::CpuGeneralTapeExecutor<
        RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
    Executor executor(fixture, plan, blobs, {
        .route = route,
        .beforeNextWork = true,
        .holdSafeBufferMutation = true,
        .sourceOracle = rows});
    return executor.run();
  };
  for (const auto route : {dxmt9::d3d9::ProviderReplayPolicy::Compatibility,
                           dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot}) {
    const auto valid = run(route, expected);
    check(valid.complete && valid.trace.size() == 3u,
          "authenticated source closure completes on both routes");
    check(valid.trace[0].heldMutationEvent != 0u &&
              valid.trace[0].heldMutationIdentity.has_value() &&
              dxmt9::test::detail::cpuGeneralSameIdentity(*valid.trace[0].heldMutationIdentity,
                                             vertex) &&
              valid.trace[1].heldMutationEvent == 0u &&
              !valid.trace[1].heldMutationIdentity.has_value(),
          "held mutation metadata identifies the actual prior source");

    auto finalNonempty = expected;
    finalNonempty.back().identities = expected.front().identities;
    const auto finalMismatch = run(route, std::move(finalNonempty));
    check(!finalMismatch.complete && finalMismatch.resetOk &&
              finalMismatch.failure == "general-executor-source-oracle-mismatch",
          "empty final Present closure is authenticated");

    auto stale = expected;
    stale.front().identities.front().objectId++;
    const auto staleMismatch = run(route, std::move(stale));
    check(!staleMismatch.complete && staleMismatch.resetOk &&
              staleMismatch.failure == "general-executor-source-oracle-mismatch",
          "stale source identity is rejected");

    auto missing = expected;
    missing.pop_back();
    const auto missingRow = run(route, std::move(missing));
    check(!missingRow.complete &&
              missingRow.failure == "general-executor-source-oracle-command-events",
          "missing command row is rejected");

    auto extra = expected;
    extra.push_back({.event = expected.back().event + 1u});
    const auto extraRow = run(route, std::move(extra));
    check(!extraRow.complete &&
              extraRow.failure == "general-executor-source-oracle-extra-row",
          "extra command row is rejected");
  }

  const auto validBytes = encodeCpuSourceOracleForTest(expected);
  std::vector<CpuSourceOracleRow> decoded;
  check(decodeCpuSourceOracle(validBytes, decoded) && decoded.size() == expected.size(),
        "source oracle binary decoder accepts canonical rows");
  auto badMagic = validBytes;
  badMagic[0] = std::byte{'X'};
  check(!decodeCpuSourceOracle(badMagic, decoded),
        "source oracle decoder rejects bad magic");
  auto trailing = validBytes;
  trailing.push_back(std::byte{});
  check(!decodeCpuSourceOracle(trailing, decoded),
        "source oracle decoder rejects trailing bytes");
  auto unsortedRows = expected;
  std::swap(unsortedRows.front().identities[0],
            unsortedRows.front().identities[1]);
  check(!decodeCpuSourceOracle(encodeCpuSourceOracleForTest(unsortedRows),
                               decoded),
        "source oracle decoder rejects unsorted identities");
  CpuSourceOracleRow oversized{.event = expected.front().event};
  for (std::uint64_t objectId = 1u; objectId <= 513u; ++objectId)
    oversized.identities.push_back({D9C_CHUNK_HANDLE_KIND_BUFFER, 1u, objectId});
  check(!decodeCpuSourceOracle(encodeCpuSourceOracleForTest(
                                   std::span<const CpuSourceOracleRow>(&oversized, 1u)),
                               decoded),
        "source oracle decoder rejects more than 512 identities per row");
}

void generalCpuAttachmentContentsRefinement() {
  using Owner = dxmt9::AttachmentContentOwner;
  using Read = dxmt9::AttachmentReadRoute;
  // Exhaust all six-step traces over Seed A/B, Clear A/B, successful and
  // failed reads for all owner/attachment/readback combinations. The serial
  // reference always has the newest value; unavailable reads expose nothing.
  std::size_t staleFallbackCounterexamples = 0u;
  for (const auto owner : {Owner::CpuShadow, Owner::BackendCommands}) {
    for (const bool attachment : {false, true}) {
      for (const bool readback : {false, true}) {
        for (unsigned trace = 0u; trace < 46656u; ++trace) {
          unsigned remaining = trace, reference = 0u, shadow = 0u, backend = 0u;
          unsigned referenceCommands = 0u, commands = 0u;
          for (unsigned step = 0u; step < 6u; ++step) {
            const auto action = remaining % 6u;
            remaining /= 6u;
            if (action < 2u) {
              reference = shadow = backend = action + 1u;
            } else if (action < 4u) {
              reference = backend = action - 1u;
              ++referenceCommands;
              ++commands;
              if (!dxmt9::backendAttachmentContents(owner, attachment))
                shadow = reference;
            } else {
              const auto route = dxmt9::attachmentReadRoute(owner, attachment, readback);
              const bool backendOwned = attachment && owner == Owner::BackendCommands;
              const bool expectedReadable = !backendOwned || (readback && action == 4u);
              const bool readable = route == Read::CpuShadow ||
                  (route == Read::GpuReadback && action == 4u);
              check(readable == expectedReadable, "attachment read refinement availability");
              if (readable)
                check((route == Read::CpuShadow ? shadow : backend) == reference,
                      "attachment read refinement newest value");
              if (backendOwned && !readable && shadow != reference)
                ++staleFallbackCounterexamples;
            }
            check(commands == referenceCommands, "ownership never elides Clear commands");
          }
        }
      }
    }
  }
  check(staleFallbackCounterexamples != 0u,
        "bounded model distinguishes stale-seed read fallback mutant");

  using Access = dxmt9::CommandQueueArenaLeaseTestAccess;
  for (const auto owner : {Owner::CpuShadow, Owner::BackendCommands}) {
    RuntimeFixture fixture(false, false, true, false);
    fixture.routing->attachmentOwner = owner;
    check(!fixture.routing->supportsGpuReadback(), "CPU fixture never fabricates readback");
    auto& device = fixture.cDevice->dev();
    auto color = device.createSurface({2, 2, Format::A8R8G8B8, Pool::Default,
                                       UsageRenderTarget, true, false});
    auto depth = device.createSurface({2, 2, Format::D24S8, Pool::Default,
                                       UsageDepthStencil, false, true});
    auto texture = device.createTexture({2, 2, 1, 1, Format::A8R8G8B8,
        TextureType::TwoD, Pool::Default, UsageTexture | UsageRenderTarget});
    check(color && depth && texture, "attachment fixture resources");
    const auto seed = [](auto& resource) {
      auto region = resource->lockRect(nullptr, 0u);
      check(region.data != nullptr, "write-only attachment seed");
      std::memset(region.data, 0x5a, region.pitch * 2u);
      resource->unlockRect();
    };
    seed(color);
    seed(depth);
    auto alias = texture->surfaceLevel(0u);
    check(alias != nullptr, "texture attachment alias");
    seed(alias);
    ClearDesc clear{};
    clear.clearColor = clear.clearDepth = clear.clearStencil = true;
    clear.color = {1.0f, 0.0f, 0.0f, 1.0f};
    clear.depth = 1.0f;
    clear.stencil = 0xffu;
    clear.colorAttachments[0].handle = color->handle();
    clear.colorAttachments[1].handle = texture->handle();
    clear.depthStencil.handle = depth->handle();
    check(device.clear(clear) == D3D_OK && fixture.routing->clearCalls == 1u,
          "production Clear executes exactly once for both authorities");
    for (auto& surface : {color, depth, alias}) {
      auto region = surface->lockRect(nullptr, 0u);
      check(region.data != nullptr, "inspect backing seed, not rendered pixels");
      const auto* bytes = static_cast<const unsigned char*>(region.data);
      const bool unchanged = std::all_of(bytes, bytes + region.pitch * 2u,
                                         [](auto byte) { return byte == 0x5a; });
      surface->unlockRect();
      check(unchanged == (owner == Owner::BackendCommands),
            "production Clear shadow decision is bound to ownership, not readback");
    }
    if (owner == Owner::BackendCommands) {
      auto destination = device.createSurface({2, 2, Format::A8R8G8B8,
                                               Pool::SystemMem, 0u, false, false});
      check(device.getRenderTargetData(color, destination) == D3DERR_NOTAVAILABLE,
            "backend-command readback fails rather than copying the stale seed");
      check(fixture.routing->flushCalls == 0u,
            "unavailable readback rejects before submission/flush effects");
      fixture.routing->readbackAvailableForTest = true;
      check(device.getRenderTargetData(color, destination) == D3DERR_NOTAVAILABLE &&
                fixture.routing->flushCalls == 1u,
            "failed capable readback never falls back to the stale seed");
      fixture.routing->readbackAvailableForTest = false;
    }
    check(Access::publishWriting(fixture.routing->queue_), "publish preserved Clear");
    unsigned clearCount = 0u;
    const auto submitted = Access::submitOneWithheld(fixture.routing->queue_, [&](const auto& payload) {
      for (std::size_t i = 0; i < payload.commandCount(); ++i) {
        const auto command = payload.commandAt(i);
        if (command.kind() != MetalCommandKind::Clear) continue;
        ++clearCount;
        check(command.command.clear && command.command.clear->clearDepth &&
                  command.command.clear->colorAttachments[0].handle == color->handle() &&
                  command.command.clear->depthStencil.handle == depth->handle(),
              "Clear snapshot and attachment identities preserved");
      }
    });
    check(submitted.submitted && clearCount == 1u &&
              Access::acknowledgeOne(fixture.routing->queue_) &&
              Access::finishQueue(fixture.routing->queue_),
          "preserved Clear completes and reclaims normally");
  }
}

void generalCpuMeasurementOmitsOracleOnly() {
  using Ledger = dxmt9::test::CpuDiagnosticLedger;
  using Phase = dxmt9::test::CpuDiagnosticPhase;
  static dxmt9::test::CpuDiagnosticSample fake{};
  const auto fakeClock = +[]() noexcept { return fake; };
  {
    fake = {};
    Ledger ledger(fakeClock);
    fake = {5, 3};
    try {
      Ledger::Scope replay(&ledger, Phase::Replay);
      fake = {12, 7};
      Ledger::Scope completion(&ledger, Phase::Completion);
      fake = {15, 9};
      throw TestFailure("diagnostic-unwind");
    } catch (const TestFailure&) {}
    fake = {20, 11};
    ledger.finish();
    ledger.finish();
    check(ledger.valid() && ledger.total().wall == 20 && ledger.total().cpu == 11 &&
              ledger.rows()[static_cast<std::size_t>(Phase::Replay)].wall == 7 &&
              ledger.rows()[static_cast<std::size_t>(Phase::Replay)].cpu == 4 &&
              ledger.rows()[static_cast<std::size_t>(Phase::Completion)].wall == 3 &&
              ledger.rows()[0].wall == 10 && ledger.rows()[0].cpu == 5,
          "diagnostic scopes conserve both clocks through nested exception unwinding");
    fake = {0, 0};
    Ledger backwards(fakeClock);
    fake = {1, 0, false};
    backwards.finish();
    check(!backwards.valid(), "unavailable CPU clock is not zero-cost evidence");
    fake = {10, 10};
    Ledger regressed(fakeClock);
    fake = {9, 11};
    regressed.finish();
    check(!regressed.valid(), "non-monotonic diagnostic clock fails closed");
  }
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  const auto tape = dxmt9::d3d9::test::makeGeneralCpuDestroyFixture(true, true);
  std::vector<dxmt9::d3d9::RenderTapeProviderBlob> blobs;
  for (const auto& bytes : tape.blobs)
    blobs.push_back({dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes), bytes});
  runtime::GeneralPreflightPlan plan;
  check(runtime::buildGeneralReplayPlan(tape.tape, blobs, plan).complete() &&
            runtime::admitGeneralCpuPlan(plan).accepted(),
        "measurement fixture must pass unchanged general admission");
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE, "measurement CPU-visible device");
  PresentParameters output{};
  output.backBufferWidth = plan.outputDesc.width;
  output.backBufferHeight = plan.outputDesc.height;
  output.backBufferFormat = dxmt9::d3d9::devicec::fmtFromD3D(plan.outputDesc.format);
  output.backBufferCount = 1u;
  output.windowed = true;
  output.deviceWindow = Handle{91};
  output.presentationInterval = PresentInterval::Immediate;
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<
      RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
  for (const auto route : {dxmt9::d3d9::ProviderReplayPolicy::Compatibility,
                           dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot}) {
    std::optional<Executor::Result> reference;
    for (unsigned phase = 0u; phase < 4u; ++phase) {
      const bool measurement = phase == 1u || phase == 3u;
      std::optional<Ledger> diagnostic;
      if (phase == 3u) diagnostic.emplace(sampleCpuDiagnosticClocks);
      RuntimeFixture fixture(false, false, true, false,
                             WMT::Device{resourceDevice.handle}, nullptr, &output);
      Executor executor(fixture, plan, blobs, {.route = route,
          .measurement = measurement, .diagnostic = diagnostic ? &*diagnostic : nullptr});
      auto result = executor.run();
      if (diagnostic) {
        diagnostic->finish();
        std::uint64_t wall = 0u, cpu = 0u;
        for (const auto& row : diagnostic->rows()) { wall += row.wall; cpu += row.cpu; }
        check(diagnostic->valid() && wall == diagnostic->total().wall &&
                  cpu == diagnostic->total().cpu &&
                  diagnostic->rows()[static_cast<std::size_t>(Phase::Completion)].entries > 0u,
              "general diagnostic phases conserve wall/CPU without changing execution");
      }
      check(result.complete && result.resetOk, "measurement executes and resets: " + result.failure);
      check(result.draws > 0u && result.sourceSummaryDraws == result.draws &&
                result.resourcesCreated == result.resourcesReleased &&
                result.submitted == result.completed && result.completed == result.reclaimed,
            "both modes preserve actual draw, source and resource conservation");
      if (!reference) reference = result;
      check(result.commands == reference->commands && result.draws == reference->draws &&
                result.sourceSummaryCommands == reference->sourceSummaryCommands &&
                result.submitted == reference->submitted,
            "measurement must not replace semantic counts with physical batching counts");
      if (measurement) {
        check(result.fullOracleCalls == 0u && result.drawObservations.empty() &&
                  result.observedSnapshots == 0u && result.observedLiveBindings == 0u &&
                  result.sourceOracleCheckedSources == 0u && result.digest == 0u &&
                  !result.logicalResourceOracleComplete && result.observeNs == 0u &&
                  result.replayNs == 0u && result.setupNs == 0u &&
                  result.completionNs == 0u && result.cleanupNs == 0u,
              "measurement has no full oracle, per-draw proof or phase-clock result");
      } else {
        check(result.fullOracleCalls > 0u && result.logicalResourceOracleComplete &&
                  result.drawObservations.size() == result.draws && result.digest == reference->digest,
              "verification surrounding measurement retains the detailed semantic oracle");
      }
    }
    RuntimeFixture fixture(false, false, true, false,
                           WMT::Device{resourceDevice.handle}, nullptr, &output);
    Executor unsupported(fixture, plan, blobs, {.route = route,
        .beforeNextWork = true, .measurement = true});
    const auto rejected = unsupported.run();
    check(!rejected.complete && rejected.resourcesCreated == 0u && rejected.submitted == 0u &&
              rejected.failure == "general-executor-measurement-policy",
          "unqualified measurement schedule rejects before resource effects");
  }
}

void generalCpuResetQualificationUsesFreshFixtures() {
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  using Reset = dxmt9::d3d9::test::GeneralCpuResetFixture;
  const auto runOne = [](MetalDeviceLease& resourceDevice,
                         dxmt9::d3d9::ProviderReplayPolicy route,
                         bool beforeNextWork, Reset reset,
                         bool deferResetObject) {
    const auto tape = dxmt9::d3d9::test::makeGeneralCpuDestroyFixture(
        true, true, dxmt9::d3d9::test::GeneralCpuMutationFixture::None, reset);
    std::vector<dxmt9::d3d9::RenderTapeProviderBlob> blobs;
    for (const auto& bytes : tape.blobs)
      blobs.push_back({dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes), bytes});
    runtime::GeneralPreflightPlan plan;
    check(runtime::buildGeneralReplayPlan(tape.tape, blobs, plan).complete(),
          "reset fixture preflight");
    const auto admission = runtime::admitGeneralCpuPlan(plan);
    check(admission.accepted(), "reset fixture admission");
    PresentParameters output{};
    output.backBufferWidth = plan.outputDesc.width;
    output.backBufferHeight = plan.outputDesc.height;
    output.backBufferFormat = dxmt9::d3d9::devicec::fmtFromD3D(plan.outputDesc.format);
    output.backBufferCount = 1u;
    output.windowed = true;
    output.deviceWindow = Handle{91};
    output.presentationInterval = PresentInterval::Immediate;
    RuntimeFixture fixture(false, false, true, false,
                           WMT::Device{resourceDevice.handle}, nullptr, &output);
    using Executor = dxmt9::test::CpuGeneralTapeExecutor<
        RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
    Executor executor(fixture, plan, blobs, {
        .route = route,
        .beforeNextWork = beforeNextWork,
        .deferOneObjectReleaseForResetTest = deferResetObject});
    auto result = executor.run();
    if (!deferResetObject) {
      check(result.complete && result.resetOk, "reset fixture execution: " + result.failure);
      check(result.resourcesCreated == admission.counts.objects &&
                result.resourcesReleased == result.resourcesCreated &&
                result.submitted == result.completed &&
                result.completed == result.reclaimed && result.pendingPeak == 1u,
            "reset fixture conserves resources and completions");
    }
    return result;
  };

  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE,
        "reset qualification needs one shared real backing lease");
  for (const auto route : {dxmt9::d3d9::ProviderReplayPolicy::Compatibility,
                           dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot}) {
    for (const bool beforeNextWork : {false, true}) {
      const auto a0 = runOne(resourceDevice, route, beforeNextWork, Reset::A, false);
      const auto b = runOne(resourceDevice, route, beforeNextWork, Reset::B, false);
      const auto a1 = runOne(resourceDevice, route, beforeNextWork, Reset::A, false);
      check(a0.digest == a1.digest && a0.digest != b.digest,
            "A-B-A reset preserves and distinguishes semantic bytes");
      check(a0.drawObservations.size() == a1.drawObservations.size() &&
                a0.drawObservations.size() == b.drawObservations.size(),
            "A-B-A reset preserves per-draw observation count");
      for (std::size_t draw = 0u; draw < a0.drawObservations.size(); ++draw) {
        check(a0.drawObservations[draw].cumulativeSemanticDigest ==
                  a1.drawObservations[draw].cumulativeSemanticDigest &&
                  a0.drawObservations[draw].cumulativeSemanticDigest !=
                      b.drawObservations[draw].cumulativeSemanticDigest,
              "A-B-A reset preserves each A draw and changes B");
      }
      const auto skipped = runOne(resourceDevice, route, beforeNextWork, Reset::B, true);
      check(!skipped.complete && !skipped.resetOk &&
                skipped.failure == "general-executor-reset-object-release-incomplete" &&
                skipped.resourcesCreated == skipped.resourcesReleased,
            "reset qualification rejects an actual skipped object release");
      const auto recovered = runOne(resourceDevice, route, beforeNextWork, Reset::A, false);
      check(recovered.complete && recovered.resetOk && recovered.digest == a0.digest,
            "fresh A recovers after failed reset qualification");
      check(recovered.drawObservations.size() == a0.drawObservations.size(),
            "recovered A preserves per-draw observation count");
      for (std::size_t draw = 0u; draw < a0.drawObservations.size(); ++draw)
        check(recovered.drawObservations[draw].cumulativeSemanticDigest ==
                  a0.drawObservations[draw].cumulativeSemanticDigest,
              "recovered A preserves every per-draw semantic digest");
    }
  }
}

void generalCpuRgba16FloatCheckpointUsesQualifiedRows() {
  namespace tape = dxmt9::d3d9;
  namespace runtime = tape::render_tape_runtime;
  namespace cpu = dxmt9::test::detail;
  const D9CSurfaceDesc level{
      .format = 113u, .resourceType = 3u, .usage = 1u, .pool = 0u,
      .width = 7u, .height = 5u, .depth = 1u};
  const tape::RenderTapeTextureDescriptorV2 metadata{
      .dimension = static_cast<std::uint32_t>(tape::RenderTapeTextureDimension::Texture2D),
      .mipLevelCount = 1u, .subresourceCount = 1u,
      .initialContentDisposition = static_cast<std::uint32_t>(
          tape::RenderTapeInitialContentDisposition::CompleteSeed)};
  const auto descriptor = [](const auto& fixed, std::span<const D9CSurfaceDesc> levels) {
    std::vector<std::byte> bytes(sizeof(fixed) + levels.size_bytes());
    std::memcpy(bytes.data(), &fixed, sizeof(fixed));
    std::memcpy(bytes.data() + sizeof(fixed), levels.data(), levels.size_bytes());
    return bytes;
  };
  const auto exact = descriptor(metadata, std::span(&level, 1u));
  constexpr std::uint32_t rowBytes = 7u * 8u;
  constexpr std::uint32_t pitch = rowBytes + 8u;
  std::vector<std::byte> seed(rowBytes * level.height);
  for (std::size_t index = 0u; index < seed.size(); ++index)
    seed[index] = static_cast<std::byte>((index * 19u + 7u) & 0xffu);
  std::vector<std::byte> storage(pitch * level.height + 16u, std::byte{0xa5});
  const D9CLockedRect lock{
      .pitch = static_cast<std::int32_t>(pitch), .bits = storage.data() + 8u};
  D9CTexture texture({}, nullptr);
  check(tape::renderTapeLinearBytesPerPixel(113u) == 0u &&
            !cpu::cpuGeneralWriteTextureRows(level, texture, 0u, lock, seed),
        "RGBA16F snapshot support does not widen the general linear LockRect format table");
  check(cpu::cpuGeneralWriteTextureMutationRows(exact, texture, 0u, 0u, lock, seed),
        "exact descriptor-qualified RGBA16F checkpoint copies eight raw bytes per texel");
  for (std::uint32_t row = 0u; row < level.height; ++row) {
    check(std::equal(seed.begin() + row * rowBytes, seed.begin() + (row + 1u) * rowBytes,
                     storage.begin() + 8u + row * pitch),
          "every half-float lane and row is copied without numeric conversion");
    check(std::all_of(storage.begin() + 8u + row * pitch + rowBytes,
                      storage.begin() + 8u + (row + 1u) * pitch,
                      [](std::byte byte) { return byte == std::byte{0xa5}; }),
          "snapshot row padding remains untouched");
  }
  check(std::all_of(storage.begin(), storage.begin() + 8u,
                    [](std::byte byte) { return byte == std::byte{0xa5}; }) &&
            std::all_of(storage.end() - 8u, storage.end(),
                        [](std::byte byte) { return byte == std::byte{0xa5}; }),
        "snapshot writer stays within its complete row extent");
  const auto unchanged = storage;
  const auto reject = [&](std::span<const std::byte> desc, std::uint32_t subresource,
                          std::uint64_t offset, const D9CLockedRect& candidateLock,
                          std::span<const std::byte> bytes) {
    check(!cpu::cpuGeneralWriteTextureMutationRows(desc, texture, subresource,
              offset, candidateLock, bytes) && storage == unchanged,
          "invalid snapshot descriptor/pitch/extent/offset rejects before any write");
  };
  reject(exact, 1u, 0u, lock, seed);
  reject(exact, 0u, 1u, lock, seed);
  reject(exact, 0u, 0u, lock, std::span(seed).first(seed.size() - 1u));
  auto extraSeed = seed;
  extraSeed.push_back(std::byte{0xff});
  reject(exact, 0u, 0u, lock, extraSeed);
  for (auto invalidPitch : {0, -1, static_cast<std::int32_t>(rowBytes - 1u)})
    reject(exact, 0u, 0u, {.pitch = invalidPitch, .bits = lock.bits}, seed);
  reject(exact, 0u, 0u, {.pitch = lock.pitch, .bits = nullptr}, seed);
  for (unsigned fault = 0u; fault < 8u; ++fault) {
    auto wrongMetadata = metadata;
    std::vector<D9CSurfaceDesc> wrongLevels{level};
    switch (fault) {
    case 0u: wrongLevels[0].pool = 1u; break;
    case 1u: wrongLevels[0].usage = 0u; break;
    case 2u: wrongLevels[0].multiSampleType = 2u; break;
    case 3u: wrongMetadata.initialContentDisposition = static_cast<std::uint32_t>(
                 tape::RenderTapeInitialContentDisposition::Unavailable); break;
    case 4u:
      wrongMetadata.mipLevelCount = wrongMetadata.subresourceCount = 2u;
      wrongLevels.push_back(level);
      wrongLevels[1].width = 3u;
      wrongLevels[1].height = 2u;
      break;
    case 5u:
      wrongMetadata.dimension = static_cast<std::uint32_t>(tape::RenderTapeTextureDimension::Volume);
      wrongLevels[0].resourceType = 4u;
      wrongLevels[0].depth = 3u;
      break;
    case 6u:
      wrongMetadata.dimension = static_cast<std::uint32_t>(tape::RenderTapeTextureDimension::Cube);
      wrongMetadata.subresourceCount = 6u;
      wrongLevels.assign(6u, level);
      for (auto& face : wrongLevels) { face.resourceType = 5u; face.height = face.width; }
      break;
    case 7u: wrongLevels[0].resourceType = 1u; break;
    }
    const auto wrong = descriptor(wrongMetadata, wrongLevels);
    reject(wrong, 0u, 0u, lock, seed);
  }

  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE,
        "CPU RGBA16F checkpoint fixture needs resources, not GPU execution");
  using Executor = dxmt9::test::CpuGeneralTapeExecutor<
      RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
  std::uint64_t originalDigest = 0u;
  for (bool alternateSeed : {false, true}) {
    const auto captured = tape::test::makeGeneralCpuAliasFixture(true, alternateSeed, 113u);
    check(captured.blobs[0].size() == 16u * 16u * 8u,
          "complete HDR RT seed preserves all raw eight-byte texels");
    std::vector<tape::RenderTapeProviderBlob> blobs;
    for (const auto& bytes : captured.blobs)
      blobs.push_back({tape::RenderTapeCaptureSession::sha256(bytes), bytes});
    runtime::GeneralPreflightPlan plan;
    check(runtime::buildGeneralReplayPlan(captured.tape, blobs, plan).complete() &&
              runtime::admitGeneralCpuPlan(plan).accepted(),
          "single-level DEFAULT RGBA16F RT checkpoint passes shared and CPU preflight");
    std::uint64_t semantic = 0u;
    for (bool measured : {false, true})
      for (bool compatibility : {true, false}) {
        RuntimeFixture fixture(false, false, true, false, WMT::Device{resourceDevice.handle});
        Executor executor(fixture, plan, blobs, {
            .route = compatibility ? tape::ProviderReplayPolicy::Compatibility
                                   : tape::ProviderReplayPolicy::ProductionSlot,
            .measurement = measured});
        const auto result = executor.run();
        check(result.complete && result.resetOk, "CPU RGBA16F checkpoint replay: " + result.failure);
        check(result.draws == 2u && result.observedTextureUploads == 1u &&
                  result.observedMutations == 3u && !fixture.routing->textureUploadFailed &&
                  result.resourcesCreated == result.resourcesReleased &&
                  result.submitted == result.completed && result.completed == result.reclaimed,
              "actual CPU upload shape and alias lifetime conserve the complete HDR seed");
        if (measured) {
          check(result.fullOracleCalls == 0u && result.drawObservations.empty(),
                "HDR checkpoint measurement keeps shape checks and omits semantic scans");
        } else {
          check(result.drawObservations.size() == 2u,
                "both real draw observations consume the HDR parent/alias receipt");
          if (!semantic) semantic = result.digest;
          check(result.digest == semantic,
                "complete raw half-float seed has identical semantics across production routes");
        }
      }
    if (!alternateSeed) originalDigest = semantic;
    else check(semantic != originalDigest,
               "changed half-float raw bits change the observed command/resource semantics");
  }
}

void generalCpuAliasAfterParentDestroyUsesImmutableIdentity() {
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  using Fixture = dxmt9::d3d9::test::GeneralCpuAliasFixture;

  struct Prepared {
    Fixture fixture;
    std::vector<dxmt9::d3d9::RenderTapeProviderBlob> blobs;
    runtime::GeneralPreflightPlan plan;
    runtime::GeneralCpuAdmissionResult admission;
  };
  const auto prepare = [](bool destroyParentAfterFirstDraw,
                          bool alternateParentSeed = false) {
    Prepared prepared{.fixture = dxmt9::d3d9::test::makeGeneralCpuAliasFixture(
                          destroyParentAfterFirstDraw, alternateParentSeed)};
    dxmt9::d3d9::RenderTapeBlobCatalogue catalogue;
    for (const auto& bytes : prepared.fixture.blobs)
      prepared.blobs.push_back({dxmt9::d3d9::RenderTapeCaptureSession::sha256(bytes),
                                bytes});
    for (const auto& blob : prepared.blobs)
      catalogue.blobs.push_back({blob.digest, blob.bytes.size(), 1u});
    const auto validation = dxmt9::d3d9::validateRenderTape(
        prepared.fixture.tape, catalogue);
    check(validation.valid(), std::string("alias fixture validation: ") +
        dxmt9::d3d9::renderTapeValidationStatusName(validation.status) + " event=" +
        std::to_string(validation.failedEventIndex) + " reason=" +
        dxmt9::d3d9::renderTapeIncompleteFrameReasonName(validation.incompleteFrameReason));
    const auto replay = runtime::buildGeneralReplayPlan(
        prepared.fixture.tape, prepared.blobs, prepared.plan);
    check(replay.complete(), std::string("alias fixture preflight: ") +
                                dxmt9::d3d9::frameTapeReplayStatusName(replay.status));
    prepared.admission = runtime::admitGeneralCpuPlan(prepared.plan);
    check(prepared.admission.accepted(), "alias fixture CPU admission");
    return prepared;
  };

  // Both tapes contain the same texture mutation, two draws through S, and
  // one Present. Only the parent-wrapper release moves across the draw
  // boundary, so an equal observation digest proves the live S alias was
  // attributed to immutable T.g1 after T's wrapper was destroyed.
  const auto control = prepare(false);
  const auto alias = prepare(true);
  const auto changedAlias = prepare(true, true);
  check(control.plan.events.size() == alias.plan.events.size() &&
            control.admission.counts.draws == 2u &&
            alias.admission.counts.draws == 2u,
        "alias control and parent-destroy tapes have identical command shape");
  const auto& texture =
      dxmt9::d3d9::test::general_cpu_alias_fixture_detail::kTexture;
  const auto& surface =
      dxmt9::d3d9::test::general_cpu_alias_fixture_detail::kSurface;
  const auto aliasDestroy = std::find_if(
      alias.plan.events.begin(), alias.plan.events.end(), [&](const auto& event) {
        return event.type == dxmt9::d3d9::RenderTapeEventType::ObjectDestroy &&
               event.index < alias.plan.destroys.size() &&
               dxmt9::test::detail::cpuGeneralSameIdentity(
                   alias.plan.destroys[event.index].identity, texture);
      });
  const auto aliasSurfaceDefine = std::find_if(
      alias.plan.definitions.begin(), alias.plan.definitions.end(),
      [&](const auto& definition) {
        return dxmt9::test::detail::cpuGeneralSameIdentity(
            definition.fixed.identity, surface);
      });
  check(aliasDestroy != alias.plan.events.end() &&
            aliasSurfaceDefine != alias.plan.definitions.end(),
        "alias tape retains S while parent T is destroyed");

  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  check(resourceDevice.handle != NULL_OBJECT_HANDLE,
        "alias qualification needs one shared real backing lease");
  const auto run = [&](const Prepared& prepared,
                       dxmt9::d3d9::ProviderReplayPolicy route,
                       bool beforeNextWork, bool dropRetiredAliasReceipt = false) {
    PresentParameters output{};
    output.backBufferWidth = prepared.plan.outputDesc.width;
    output.backBufferHeight = prepared.plan.outputDesc.height;
    output.backBufferFormat =
        dxmt9::d3d9::devicec::fmtFromD3D(prepared.plan.outputDesc.format);
    output.backBufferCount = 1u;
    output.windowed = true;
    output.deviceWindow = Handle{91};
    output.presentationInterval = PresentInterval::Immediate;
    RuntimeFixture fixture(false, false, true, false,
                           WMT::Device{resourceDevice.handle}, nullptr, &output);
    using Executor = dxmt9::test::CpuGeneralTapeExecutor<
        RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
    Executor executor(fixture, prepared.plan, prepared.blobs, {
        .route = route,
        .beforeNextWork = beforeNextWork,
        .dropRetiredAliasReceiptForTest = dropRetiredAliasReceipt,
    });
    return executor.run();
  };

  for (const auto route : {dxmt9::d3d9::ProviderReplayPolicy::Compatibility,
                           dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot}) {
    for (const bool beforeNextWork : {false, true}) {
      const auto retained = run(control, route, beforeNextWork);
      const auto destroyed = run(alias, route, beforeNextWork);
      const auto changed = run(changedAlias, route, beforeNextWork);
      const auto dropped = run(alias, route, beforeNextWork, true);
      check(retained.complete && retained.resetOk,
            "alias control execution: " + retained.failure);
      check(destroyed.complete && destroyed.resetOk,
            "alias parent-destroy execution: " + destroyed.failure);
      check(changed.complete && changed.resetOk,
            "changed parent seed execution: " + changed.failure);
      check(dropped.complete && dropped.resetOk &&
                dropped.drawObservations.size() == 2u &&
                dropped.drawObservations.front().cumulativeSemanticDigest ==
                    destroyed.drawObservations.front().cumulativeSemanticDigest &&
                dropped.drawObservations.back().cumulativeSemanticDigest !=
                    destroyed.drawObservations.back().cumulativeSemanticDigest &&
                dropped.digest != destroyed.digest,
            "old live-only attribution fails equality exactly after parent destruction");
      check(retained.commands == destroyed.commands && retained.draws == 2u &&
                destroyed.draws == 2u && retained.submitted == 3u &&
                destroyed.submitted == 3u && retained.completed == 3u &&
                destroyed.completed == 3u && retained.reclaimed == 3u &&
                destroyed.reclaimed == 3u,
            "alias ordering admits the same draw and Present commands");
      check(retained.digest == destroyed.digest &&
                retained.drawObservations.size() == 2u &&
                destroyed.drawObservations.size() == 2u,
            "parent destruction does not change the full alias semantic digest");
      for (std::size_t draw = 0u; draw < retained.drawObservations.size(); ++draw)
        check(retained.drawObservations[draw].cumulativeSemanticDigest ==
                  destroyed.drawObservations[draw].cumulativeSemanticDigest,
              "parent destruction preserves each S-backed draw observation");
      check(changed.digest != destroyed.digest &&
                changed.drawObservations.size() == destroyed.drawObservations.size(),
            "changed parent seed changes the post-destroy alias digest");
      for (std::size_t draw = 0u; draw < destroyed.drawObservations.size(); ++draw)
        check(changed.drawObservations[draw].cumulativeSemanticDigest !=
                  destroyed.drawObservations[draw].cumulativeSemanticDigest,
              "changed parent seed changes every post-destroy draw observation");
      check(destroyed.destroysWithPending == std::size_t(beforeNextWork) &&
                destroyed.retainedSnapshotsAfterDestroy ==
                    (beforeNextWork ? 2u : 0u),
            "alias parent release observes the real pending boundary");
    }
  }
}

int runCpuTapeExperiment(CpuTapeExperimentOptions options) {
  if (options.inputAdmissionProfile != "default" &&
      options.inputAdmissionProfile != "extended")
    return rejectCpuInputAdmissionProfile(options,
        "unsupported-input-admission-profile", "input-admission-profile");
  if (!options.generalFrame && options.inputAdmissionProfile != "default")
    return rejectCpuInputAdmissionProfile(options, "unsupported-mode",
        "input-admission-profile-requires-general-frame");
  if (options.generalFrame) return runGeneralCpuTapeExperiment(options);
  if (options.beforeNextWork)
    throw TestFailure("before-next-work requires general-frame grammar");
  const bool metalResources =
      options.resourceBackend == CpuTapeResourceBackend::MetalResources;
  const bool managedAllocationFault = options.fault == "managed-allocation";
  const char* const backendName =
      metalResources ? "cpu-metal-resources" : "cpu-no-gpu";
  const auto importBegin = std::chrono::steady_clock::now();
  std::vector<std::byte> tapeBytes;
  if (!readBinaryFile(options.tapePath, tapeBytes)) {
    std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1",)"
              << R"("backend":")" << backendName
              << R"(","status":"input-read-failed"})" << '\n';
    return 2;
  }
  dxmt9::d3d9::RenderTapeBlobCatalogue catalogue;
  std::vector<std::vector<std::byte>> blobs;
  for (const auto& blobPath : options.blobPaths) {
    std::vector<std::byte> blobBytes;
    if (!readBinaryFile(blobPath, blobBytes)) {
      std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1",)"
                << R"("backend":")" << backendName
                << R"(","status":"blob-read-failed"})" << '\n';
      return 2;
    }
    const auto digest = dxmt9::d3d9::RenderTapeCaptureSession::sha256(
        std::span<const std::byte>(blobBytes));
    catalogue.blobs.push_back(
        {.digest = digest, .size = blobBytes.size(), .verified = 1u});
    blobs.push_back(std::move(blobBytes));
  }

  dxmt9::d3d9::ImportedRenderTapeView imported;
  const auto validation =
      dxmt9::d3d9::validateRenderTape(tapeBytes, catalogue, &imported);
  if (!validation.valid()) {
    std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1",)"
              << R"("backend":")" << backendName
              << R"(","status":"invalid-input",)"
              << R"("validation_status":")"
              << dxmt9::d3d9::renderTapeValidationStatusName(validation.status)
              << R"("})" << '\n';
    return 2;
  }
  bool hasManagedBuffer = false;
  for (std::size_t eventIndex = 0u; eventIndex < imported.events.size();
       ++eventIndex) {
    const auto event = imported.event(eventIndex);
    if (static_cast<dxmt9::d3d9::RenderTapeEventType>(event.header.type) !=
            dxmt9::d3d9::RenderTapeEventType::ObjectDefine ||
        event.payload.size() <
            sizeof(dxmt9::d3d9::RenderTapeObjectDefineHeader)) {
      continue;
    }
    dxmt9::d3d9::RenderTapeObjectDefineHeader fixed{};
    std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
    if (fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_BUFFER ||
        fixed.descriptorKind != static_cast<std::uint32_t>(
                                    dxmt9::d3d9::RenderTapeDescriptorKind::Buffer) ||
        fixed.descriptorBytes != sizeof(D9CBufferDesc) ||
        event.payload.size() < sizeof(fixed) + sizeof(D9CBufferDesc)) {
      continue;
    }
    D9CBufferDesc descriptor{};
    std::memcpy(&descriptor, event.payload.data() + sizeof(fixed),
                sizeof(descriptor));
    hasManagedBuffer = descriptor.pool == 1u;
    if (hasManagedBuffer) break;
  }
  if (hasManagedBuffer && !metalResources) {
    std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1",)"
              << R"("backend":")" << backendName
              << R"(","status":"managed-resource-backend-required"})" << '\n';
    return 3;
  }
  const auto grammar = dxmt9::d3d9::preflightRenderTapeCpuGrammar(
      imported, metalResources, managedAllocationFault,
      options.publication ==
          dxmt9::d3d9::RenderTapeCpuPublicationMode::DrawIsland);
  if (!grammar.accepted()) {
    std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1",)"
              << R"("backend":")" << backendName
              << R"(","status":"unsupported-grammar","reason":)"
              << std::quoted(dxmt9::d3d9::renderTapeCpuGrammarReasonName(grammar.reason))
              << R"(,"event":)" << grammar.eventIndex << "}\n";
    return 3;
  }
  const auto publicationPlan = dxmt9::d3d9::planRenderTapeCpuPublication(
      imported, options.publication);
  if (!publicationPlan.valid) {
    std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1",)"
              << R"("backend":")" << backendName
              << R"(","status":"invalid-publication-plan"})" << '\n';
    return 3;
  }
  if (options.publication ==
          dxmt9::d3d9::RenderTapeCpuPublicationMode::DrawIsland &&
      std::none_of(publicationPlan.ranges.begin(),
                   publicationPlan.ranges.begin() + publicationPlan.count,
                   [](const auto& range) { return range.commandEvents > 1u; })) {
    std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1",)"
              << R"("backend":")" << backendName
              << R"(","status":"vacuous-publication-plan"})" << '\n';
    return 3;
  }

  const auto tapeDigest = dxmt9::d3d9::RenderTapeCaptureSession::sha256(
      std::span<const std::byte>(tapeBytes));
  std::optional<dxmt9::d3d9::RenderTapeCompletionPlanFile> completionPlan;
  std::optional<dxmt9::d3d9::RenderTapeCompletionPlanController>
      completionController;
  std::string completionPlanSha256;
  if (!options.completionPlanPath.empty()) {
    std::vector<std::byte> planBytes;
    if (!readBinaryFile(options.completionPlanPath, planBytes)) {
      std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1",)"
                << R"("backend":")" << backendName
                << R"(","status":"completion-plan-read-failed"})"
                << '\n';
      return 2;
    }
    completionPlanSha256 = renderTapeDigestHex(
        dxmt9::d3d9::RenderTapeCaptureSession::sha256(
            std::span<const std::byte>(planBytes)));
    dxmt9::d3d9::RenderTapeCompletionPlanFile decodedPlan{};
    auto decoded = dxmt9::d3d9::decodeRenderTapeCompletionPlan(
        std::span<const std::byte>(planBytes), decodedPlan);
    if (!decoded.accepted()) {
      std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1",)"
                << R"("backend":")" << backendName
                << R"(","status":"invalid-completion-plan","reason":)"
                << std::quoted(completionPlanDispositionName(decoded.disposition))
                << "}\n";
      return 3;
    }
    decoded = dxmt9::d3d9::validateRenderTapeCompletionPlan(
        decodedPlan, tapeDigest);
    const auto expectedRoute =
        options.compatibility
            ? dxmt9::d3d9::RenderTapeCompletionPlanRoute::Compatibility
            : dxmt9::d3d9::RenderTapeCompletionPlanRoute::ProductionSlot;
    const bool eventCountMismatch =
        decodedPlan.header.tapeEventCount != imported.events.size();
    const bool routeMismatch =
        decodedPlan.header.route !=
            dxmt9::d3d9::RenderTapeCompletionPlanRoute::Any &&
        decodedPlan.header.route != expectedRoute;
    const bool repeatMismatch =
        options.repeatExplicit && decodedPlan.header.repeatCount != options.repeat;
    if (!decoded.accepted() || eventCountMismatch || routeMismatch ||
        repeatMismatch) {
      const auto reason = !decoded.accepted()
                              ? decoded.disposition
                              : eventCountMismatch
                                  ? dxmt9::d3d9::RenderTapeCompletionPlanDisposition::InvalidEvent
                                  : routeMismatch
                                      ? dxmt9::d3d9::RenderTapeCompletionPlanDisposition::RouteMismatch
                                      : dxmt9::d3d9::RenderTapeCompletionPlanDisposition::InvalidRepeat;
      std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1",)"
                << R"("backend":")" << backendName
                << R"(","status":"invalid-completion-plan","reason":)"
                << std::quoted(completionPlanDispositionName(reason)) << "}\n";
      return 3;
    }
    if (!options.repeatExplicit) options.repeat = decodedPlan.header.repeatCount;
    completionPlan = decodedPlan;
    completionController.emplace(*completionPlan,
                                 std::span<const std::byte>(tapeDigest));
  }

  const auto importNs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - importBegin)
          .count());
  const auto sharedDeviceSetupBegin = std::chrono::steady_clock::now();
  MetalDeviceLease resourceDeviceLease;
  if (metalResources) {
    resourceDeviceLease.handle = dxmt9_test_acquire_metal_device();
    if (resourceDeviceLease.handle == NULL_OBJECT_HANDLE) {
      std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1",)"
                << R"("backend":")" << backendName
                << R"(","status":"skip-no-metal-device"})" << '\n';
      return 77;
    }
  }
  const auto sharedDeviceSetupNs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - sharedDeviceSetupBegin)
          .count());
  const WMT::Device resourceDevice{resourceDeviceLease.handle};

  std::vector<CpuTapeExperimentSummary> results;
  results.reserve(options.repeat);
  for (std::size_t iteration = 0; iteration < options.repeat; ++iteration) {
    CpuTapeExperimentSummary summary;
    const auto nativeCpuBegin = sampleProcessCpuNs();
    CpuTapePhaseLedger<std::chrono::steady_clock> phaseLedger;
    phaseLedger.begin(CpuTapePhase::Setup);
    {
      dxmt9::d3d9::ReplayQueueAllocationFailure allocationFailure;
      RuntimeFixture fixture(/*rejectAfterClear=*/false,
                             /*segmentSerial=*/false,
                             /*directChunkSlot=*/true,
                             /*cpuReadyArena=*/false, resourceDevice,
                             managedAllocationFault ? &allocationFailure : nullptr);
      const auto replayCountersBefore = dxmt9::perf::snapshotRenderTapeReplay();
      CpuTapeExperimentSink sink(
          fixture, catalogue, blobs, options.compatibility, options.afterDestroy,
          metalResources, managedAllocationFault, iteration + 1u,
          options.fault, options.maxPending, summary, phaseLedger,
          &publicationPlan,
          completionController ? &*completionController : nullptr);
      phaseLedger.begin(CpuTapePhase::Replay);
      const auto replay = dxmt9::d3d9::replayPrevalidatedRenderTape(
          imported, catalogue, sink);
      phaseLedger.begin(CpuTapePhase::Cleanup);
      sink.finish();
      const auto replayCountersAfter = dxmt9::perf::snapshotRenderTapeReplay();
      summary.capacityRejected = static_cast<std::uint32_t>(
          replayCountersAfter.continuationCapacityRejected -
          replayCountersBefore.continuationCapacityRejected);
      summary.populatedFallback = static_cast<std::uint32_t>(
          replayCountersAfter.continuationPopulatedFallback -
          replayCountersBefore.continuationPopulatedFallback);
      summary.capacityRotated = static_cast<std::uint32_t>(
          replayCountersAfter.continuationCapacityRotated -
          replayCountersBefore.continuationCapacityRotated);
      summary.failure = sink.failure();
      if (summary.failure.empty() &&
          summary.drawObservations.size() != summary.draws) {
        summary.failure = "draw-observation-count-mismatch";
      }
      summary.bufferBackingOracleComplete =
          metalResources && summary.bufferBackingObserved &&
          summary.bufferBackingRevisionAdvanced &&
          (!summary.capturedARequiredRetention ||
           summary.distinctDynamicBackings) &&
          (!summary.capturedARequiredRetention ||
           summary.capturedABackingPreserved);
      const bool groupedPublicationComplete =
          options.publication ==
              dxmt9::d3d9::RenderTapeCpuPublicationMode::PerCommand ||
          (summary.groupedRanges != 0u &&
           summary.groupedEvents > summary.groupedRanges &&
           summary.groupedPublicationVerified &&
           summary.groupedObservedDraws <= summary.draws);
      summary.complete =
          replay.complete && summary.failure.empty() && summary.resetOk &&
          groupedPublicationComplete &&
          (!managedAllocationFault ||
           (summary.faultTriggered &&
            summary.faultPreEffectOwnershipUnchanged &&
            summary.faultRetrySucceeded && summary.faultMutationApplies == 1u));
    }  // fixture and sink destruction remain inside the cleanup phase.
    phaseLedger.finish();
    const auto nativeCpuEnd = sampleProcessCpuNs();
    if (nativeCpuBegin && nativeCpuEnd && *nativeCpuEnd >= *nativeCpuBegin)
      summary.nativeCpuNs = *nativeCpuEnd - *nativeCpuBegin;
    summary.phaseNs = phaseLedger.phaseNs();
    summary.elapsedNs = phaseLedger.elapsedNs();
    results.push_back(summary);
    if (!summary.complete) break;
  }
  const bool complete = results.size() == options.repeat && !results.empty() &&
                        std::all_of(results.begin(), results.end(),
                                    [](const auto& result) {
                                      return result.complete;
                                    });
  const bool deterministic =
      !results.empty() &&
      std::all_of(results.begin() + 1, results.end(),
                  [&](const auto& result) {
                    return result.digest == results.front().digest &&
                           result.commands == results.front().commands &&
                           result.draws == results.front().draws &&
                           result.submitted == results.front().submitted &&
                           result.completed == results.front().completed &&
                           result.reclaimed == results.front().reclaimed &&
                           result.groupedRanges ==
                               results.front().groupedRanges &&
                           result.groupedEvents ==
                               results.front().groupedEvents &&
                           result.groupedObservedDraws ==
                               results.front().groupedObservedDraws &&
                           result.capacityRejected ==
                               results.front().capacityRejected &&
                           result.populatedFallback ==
                               results.front().populatedFallback &&
                           result.capacityRotated ==
                               results.front().capacityRotated;
                  });
  const bool nativeCpuAvailable =
      !results.empty() &&
      std::any_of(results.begin(), results.end(), [](const auto& result) {
        return result.nativeCpuNs.has_value();
      });
  std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1",)"
            << R"("backend":")" << backendName
            << R"(","timing_backend":"encode-substituted","status":")"
            << (complete ? "complete" : "unresolved")
            << R"(","replay_route":")"
            << (options.compatibility ? "compatibility" : "production-slot")
            << R"(","publication":")"
            << (options.publication ==
                        dxmt9::d3d9::RenderTapeCpuPublicationMode::DrawIsland
                    ? "draw-island"
                    : "per-command")
            << R"(","completion":")"
            << (completionPlan ? "external"
                               : (options.afterDestroy ? "after-destroy"
                                                        : "after-command"))
            << R"(","repeat":)"
            << options.repeat
            << R"(,"fault":)" << std::quoted(options.fault)
            << R"(,"max_pending":)" << options.maxPending
            << R"(,"completion_plan_sha256":)"
            ;
  if (completionPlan) {
    std::cout << std::quoted(completionPlanSha256);
  } else {
    std::cout << "null";
  }
  std::cout << R"(,"immutable_input_owned":true,"deterministic":)"
            << (deterministic ? "true" : "false")
            << R"(,"phase_accounting":"exclusive-wall","clock":"steady_clock","native_cpu_clock":")"
            << (nativeCpuAvailable ? "process" : "unavailable")
            << R"(","native_cpu_ns":null,"import_ns":)"
            << importNs
            << R"(,"shared_device_setup_ns":)" << sharedDeviceSetupNs
            << R"(,"reset_policy":"fresh-runtime","cache_policy":"fresh-runtime","shared_native_resource_device":)"
            << (metalResources ? "true" : "false")
            << R"(,"resource_execution":")"
            << (metalResources ? "cpu-visible-metal-no-gpu-submit"
                               : "shadow-only-no-gpu")
            << R"(","iterations":[)";
  for (std::size_t i = 0; i < results.size(); ++i) {
    if (i != 0u) std::cout << ',';
    const auto& result = results[i];
    std::cout << R"({"index":)" << i
              << R"(,"draw_observations":[)";
    for (std::size_t draw = 0; draw < result.drawObservations.size(); ++draw) {
      if (draw != 0u) std::cout << ',';
      const auto& observation = result.drawObservations[draw];
      std::cout << R"({"ordinal":)" << observation.ordinal
                << R"(,"publication_event":)" << observation.publicationEvent
                << R"(,"cumulative_semantic_digest":")"
                << observation.cumulativeSemanticDigest << R"("})";
    }
    std::cout << ']'
              << R"(,"semantic_digest":")" << result.digest
              << R"(","command_count":)" << result.commands
              << R"(,"draw_count":)" << result.draws
              << R"(,"direct_draws":)" << result.directDraws
              << R"(,"compatibility_draws":)" << result.compatibilityDraws
              << R"(,"submitted":)" << result.submitted
              << R"(,"completed":)" << result.completed
              << R"(,"reclaimed":)" << result.reclaimed
              << R"(,"resources_created":)" << result.resourcesCreated
              << R"(,"resources_released":)" << result.resourcesReleased
              << R"(,"retained_before_completion":)"
              << (result.retainedBeforeCompletion ? "true" : "false")
              << R"(,"buffer_backing_observed":)"
              << (result.bufferBackingObserved ? "true" : "false")
              << R"(,"captured_A_required_retention":)"
              << (result.capturedARequiredRetention ? "true" : "false")
              << R"(,"captured_A_backing_preserved":)"
              << (result.capturedABackingPreserved ? "true" : "false")
              << R"(,"buffer_backing_revision_advanced":)"
              << (result.bufferBackingRevisionAdvanced ? "true" : "false")
              << R"(,"distinct_dynamic_backings":)"
              << (result.distinctDynamicBackings ? "true" : "false")
              << R"(,"buffer_backing_oracle":")"
              << (result.bufferBackingOracleComplete
                      ? "real-metal-cpu-visible"
                      : (result.bufferBackingObserved ? "unproven"
                                                       : "not-applicable"))
              << '"'
              << R"(,"reset_ok":)" << (result.resetOk ? "true" : "false")
              << R"(,"fault_triggered":)"
              << (result.faultTriggered ? "true" : "false")
              << R"(,"fault_receipt":)"
              << (result.faultReceipt.empty()
                      ? std::string("null")
                      : std::string("\"") + result.faultReceipt + "\"")
              << R"(,"fault_preeffect_ownership_unchanged":)"
              << (result.faultPreEffectOwnershipUnchanged ? "true" : "false")
              << R"(,"fault_retry_succeeded":)"
              << (result.faultRetrySucceeded ? "true" : "false")
              << R"(,"fault_mutation_applies":)"
              << result.faultMutationApplies
              << R"(,"grouped_ranges":)" << result.groupedRanges
              << R"(,"grouped_events":)" << result.groupedEvents
              << R"(,"grouped_observed_draws":)"
              << result.groupedObservedDraws
              << R"(,"grouped_publication_verified":)"
              << (result.groupedPublicationVerified ? "true" : "false")
              << R"(,"capacity_rejected":)" << result.capacityRejected
              << R"(,"populated_fallback":)" << result.populatedFallback
              << R"(,"capacity_rotated":)" << result.capacityRotated
              << R"(,"elapsed_ns":)" << result.elapsedNs
              << R"(,"phase_ns":{"setup":)"
              << result.phaseNs[cpuTapePhaseIndex(CpuTapePhase::Setup)]
              << R"(,"replay":)"
              << result.phaseNs[cpuTapePhaseIndex(CpuTapePhase::Replay)]
              << R"(,"observe":)"
              << result.phaseNs[cpuTapePhaseIndex(CpuTapePhase::Observe)]
              << R"(,"completion":)"
              << result.phaseNs[cpuTapePhaseIndex(CpuTapePhase::Completion)]
              << R"(,"cleanup":)"
              << result.phaseNs[cpuTapePhaseIndex(CpuTapePhase::Cleanup)]
              << '}'
              << R"(,"native_cpu_ns":)";
    if (result.nativeCpuNs)
      std::cout << *result.nativeCpuNs;
    else
      std::cout << "null";
    std::cout
              << R"(,"failure":)" << std::quoted(result.failure)
              << R"(,"trace":[)";
    for (std::size_t trace = 0; trace < result.trace.size(); ++trace) {
      if (trace != 0u) std::cout << ',';
      const auto& row = result.trace[trace];
      std::cout << R"({"iteration":)" << row.iteration
                << R"(,"submission":)" << row.submission
                << R"(,"source":)" << row.source
                << R"(,"source_generation":)" << row.sourceGeneration
                << R"(,"storage_first_page":)" << row.storageFirstPage
                << R"(,"storage_page_count":)" << row.storagePageCount
                << R"(,"storage_generation":)" << row.storageGeneration
                << R"(,"first_event":)" << row.firstEvent
                << R"(,"last_event":)" << row.lastEvent
                << R"(,"event":)" << row.event
                << R"(,"event_submission":)" << row.eventSubmission
                << R"(,"released_at_event":)" << row.releasedAtEvent
                << R"(,"submitted":)" << (row.submitted ? "true" : "false")
                << R"(,"completed":)" << (row.completed ? "true" : "false")
                << '}';
    }
    std::cout << "]}";
  }
  std::cout << R"(],"loaded_images":)";
  printCpuTapeLoadedImages();
  std::cout << "}\n";
  return complete ? 0 : 4;
}

#include "render_tape_cpu_v4_inventory_cli.inc"

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc >= 2 && std::string_view(argv[1]) == "--controlled-cpu-v4-grouping-spec") {
      check(argc == 3, "grouping spec requires one explicit fresh-process limit: 0, 3 or 64");
      const std::string_view argument = argv[2];
      check(argument == "0" || argument == "3" || argument == "64",
          "grouping spec rejects unsupported or malformed policy values before effects");
      const std::size_t limit = argument == "0" ? 0u : argument == "3" ? 3u : 64u;
      check(setenv("DXMT9_DISABLE_PRESENT_BOUNDARY", "1", 1) == 0 &&
                setenv("DXMT9_SYNC_PRESENT_FLUSH", "0", 1) == 0 &&
                setenv("DXMT9_OFFLOAD_COMMIT_REPLAY", "0", 1) == 0 &&
                setenv("DXMT9_DRAW_CHUNK_COMMAND_LIMIT", argv[2], 1) == 0 &&
                setenv("DXMT9_DIRECT_SLOT_HEADROOM_BYTES", "0", 1) == 0 &&
                setenv("DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES", "0", 1) == 0 &&
                setenv("DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES", "0", 1) == 0 &&
                setenv("DXMT9_CPU_READY_TAPE", "0", 1) == 0 &&
                setenv("DXMT9_CPU_READY_EARLY_PREFIX", "0", 1) == 0 &&
                setenv("DXMT9_SPLIT_STRETCH_CHUNK", "0", 1) == 0 &&
                setenv("DXMT9_ASYNC_QUERY_FENCES", "1", 1) == 0 &&
                setenv("DXMT9_COALESCE_QUERY_ENDS", "0", 1) == 0 &&
                setenv("DXMT9_METAL_OCCLUSION", "0", 1) == 0 &&
                setenv("DXMT9_QUERY_SCOPE_RANGES", "0", 1) == 0 &&
                setenv("DXMT_PERF_COUNTERS", "1", 1) == 0,
          "grouping spec pins isolated ordinary eager-END policy before cached initialization");
      controlledCpuV4GroupingPreservesQueryEnd(limit);
      return 0;
    }
    if (argc >= 2 && (std::string_view(argv[1]) == "--controlled-cpu-v4-inventory" ||
        std::string_view(argv[1]) == "--controlled-cpu-v4-semantics" ||
        std::string_view(argv[1]) == "--controlled-cpu-v4-execute")) {
      // Fresh-process CPU ownership pins, shared with the controlled fixtures.
      // They select the isolated harness domain, not a timed renderer policy.
      setenv("DXMT9_DISABLE_PRESENT_BOUNDARY", "1", 1);
      setenv("DXMT9_SYNC_PRESENT_FLUSH", "0", 1);
      setenv("DXMT9_OFFLOAD_COMMIT_REPLAY", "0", 1);
      // The cold CLI validates and pins its typed grouping policy before any
      // cached selector, resource setup or replay can observe it.
      setenv("DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES", "0", 1);
      setenv("DXMT9_CPU_READY_TAPE", "0", 1);
      setenv("DXMT9_CPU_READY_EARLY_PREFIX", "0", 1);
      setenv("DXMT9_SPLIT_STRETCH_CHUNK", "0", 1);
      return runControlledCpuV4InventoryCli(argc, argv);
    }
    if (argc == 2 && std::string_view(argv[1]) == "--qualified-ordinary-growth-spec") {
      qualifiedCapacitySurvivesOrdinaryGrowth();
      std::cout << "qualified ordinary growth spec passed\n";
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--headroom-ownership-spec") {
      setenv("DXMT9_DIRECT_SLOT_HEADROOM_BYTES", "65536", 1);
      setenv("DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES", "65536", 1);
      deniedHeadroomPreservesQualifiedCapacity();
      std::cout << "headroom ownership spec passed\n";
      return 0;
    }
    if (argc == 2 && (std::string_view(argv[1]) == "--general-cpu-destroy-spec" ||
                      std::string_view(argv[1]) == "--general-cpu-mipmap-spec" ||
                      std::string_view(argv[1]) == "--general-cpu-mutation-spec" ||
                      std::string_view(argv[1]) == "--general-cpu-source-oracle-spec" ||
                      std::string_view(argv[1]) == "--general-cpu-reset-spec" ||
                      std::string_view(argv[1]) == "--general-cpu-measurement-spec" ||
                      std::string_view(argv[1]) == "--general-cpu-attachment-spec" ||
                      std::string_view(argv[1]) == "--general-cpu-alias-spec" ||
                      std::string_view(argv[1]) == "--general-cpu-query-spec" ||
                      std::string_view(argv[1]) == "--general-cpu-event-checkpoint-spec" ||
                      std::string_view(argv[1]) == "--general-cpu-d24s8-spec" ||
                      std::string_view(argv[1]) == "--general-cpu-volume-spec" ||
                      std::string_view(argv[1]) == "--general-cpu-bootstrap-pages-spec" ||
                      std::string_view(argv[1]) == "--controlled-cpu-v4-consumer-spec" ||
                      std::string_view(argv[1]) == "--controlled-cpu-v4-integration-spec" ||
                      std::string_view(argv[1]) == "--controlled-cpu-v4-executor-spec" ||
                      std::string_view(argv[1]) == "--controlled-cpu-v4-route-spec" ||
                      std::string_view(argv[1]) == "--controlled-cpu-v4-execute-spec" ||
                      std::string_view(argv[1]) == "--controlled-cpu-v4-progress-spec" ||
                      std::string_view(argv[1]) ==
                          "--two-pending-production-sources-spec" ||
                      std::string_view(argv[1]) ==
                          "--cpu-tape-phase-ledger-spec" ||
                      std::string_view(argv[1]) ==
                          "--capture-source-policy-spec" ||
                      std::string_view(argv[1]) ==
                          "--capture-source-v3-policy-spec")) {
      // Isolate cached policies from the unrelated routing test matrix.
      // The general no-GPU executor requires the same admission/pacing pins
      // as its CLI, established before the first device is constructed.
      setenv("DXMT9_DISABLE_PRESENT_BOUNDARY", "1", 1);
      setenv("DXMT9_SYNC_PRESENT_FLUSH", "0", 1);
      setenv("DXMT9_OFFLOAD_COMMIT_REPLAY", "0", 1);
      // Only this fresh-process native stress flag requests the supported
      // draw-limit policy. No cached policy is mutated in an existing spec.
      setenv("DXMT9_DRAW_CHUNK_COMMAND_LIMIT",
          std::string_view(argv[1]) == "--controlled-cpu-v4-progress-spec" ? "3" : "0", 1);
      setenv("DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES", "0", 1);
      setenv("DXMT9_CPU_READY_TAPE", "0", 1);
      setenv("DXMT9_CPU_READY_EARLY_PREFIX", "0", 1);
      setenv("DXMT9_SPLIT_STRETCH_CHUNK", "0", 1);
      if (std::string_view(argv[1]) == "--controlled-cpu-v4-route-spec" ||
          std::string_view(argv[1]) == "--controlled-cpu-v4-execute-spec") {
        // The existing route delta counters are cached on first use. Pin the
        // same gate as the controlled semantics CLI before constructing any
        // fixture; this does not turn the test into a performance lane.
        check(setenv("DXMT_PERF_COUNTERS", "1", 1) == 0 && dxmt9::perf::enabled(),
            "controlled route/execution spec requires existing replay counter observation");
      }
      if (std::string_view(argv[1]) == "--controlled-cpu-v4-consumer-spec") {
        controlledCpuV4ConsumerUsesRealQueue();
      } else if (std::string_view(argv[1]) == "--controlled-cpu-v4-integration-spec") {
        controlledCpuV4IntegrationUsesObservedReplay();
      } else if (std::string_view(argv[1]) == "--controlled-cpu-v4-executor-spec") {
        controlledCpuV4ExecutorInventoriesOriginalTape();
        controlledCpuV4ExecutorPreservesPePresentContract();
      } else if (std::string_view(argv[1]) == "--controlled-cpu-v4-route-spec") {
        controlledCpuV4ExecutorHonorsRequestedRoute();
      } else if (std::string_view(argv[1]) == "--controlled-cpu-v4-execute-spec") {
        controlledCpuV4ExecutorOmitsObservationForExecution();
      } else if (std::string_view(argv[1]) == "--controlled-cpu-v4-progress-spec") {
        controlledCpuV4RootProgressUsesNaturalPublications();
      } else if (std::string_view(argv[1]) == "--capture-source-policy-spec") {
        providerSourcePolicyPreservesRawOrderAndSettlement();
        sourcePublicationCaptureOwnsEveryPublicationAndFailsClosed();
        generalCpuCaptureSourcePlanUsesAuthenticatedExecutor();
      } else if (std::string_view(argv[1]) == "--capture-source-v3-policy-spec") {
        generalCpuCaptureSourceV3PreservesCrossEventWriter();
        generalCpuCaptureSourceV3PreservesCrossEventWriter(true);
      } else if (std::string_view(argv[1]) == "--cpu-tape-phase-ledger-spec") {
        cpuTapePhaseLedgerNestedScopeSpec();
      } else if (std::string_view(argv[1]) == "--general-cpu-mipmap-spec") {
        cpuMipmapObserverPreservesOrderAndQualifiedTexture();
        generalCpuMipmapUsesProductionLifetime();
      } else if (std::string_view(argv[1]) == "--general-cpu-query-spec") {
        controlledQueryFenceRefinementAndLifetime();
        generalCpuQueryTranscriptUsesControlledQueue();
      } else if (std::string_view(argv[1]) == "--general-cpu-event-checkpoint-spec") {
        generalCpuEventCheckpointUsesNewLocalEnd();
      } else if (std::string_view(argv[1]) == "--general-cpu-d24s8-spec") {
        generalCpuD24S8CheckpointRetainsBothLogicalPlanes();
      } else if (std::string_view(argv[1]) == "--general-cpu-volume-spec") {
        generalCpuVolumeWriterAndUploadShapeAreExact();
        generalCpuVolumeTapeUsesProductionCreationAndMutation();
      } else if (std::string_view(argv[1]) == "--general-cpu-bootstrap-pages-spec") {
        generalCpuBootstrapPagesUseProductionReplay();
      } else if (std::string_view(argv[1]) == "--general-cpu-mutation-spec")
        generalCpuMutationWindowUsesProductionLifetime();
      else if (std::string_view(argv[1]) == "--general-cpu-source-oracle-spec")
        generalCpuSourceOracleUsesAuthenticatedClosures();
      else if (std::string_view(argv[1]) == "--general-cpu-reset-spec")
        generalCpuResetQualificationUsesFreshFixtures();
      else if (std::string_view(argv[1]) == "--general-cpu-measurement-spec")
        generalCpuMeasurementOmitsOracleOnly();
      else if (std::string_view(argv[1]) == "--general-cpu-attachment-spec")
        generalCpuAttachmentContentsRefinement();
      else if (std::string_view(argv[1]) == "--general-cpu-alias-spec") {
        generalCpuRgba16FloatCheckpointUsesQualifiedRows();
        generalCpuAliasAfterParentDestroyUsesImmutableIdentity();
      }
      else if (std::string_view(argv[1]) ==
               "--two-pending-production-sources-spec") {
        twoPendingProductionSourcesPreserveQualifiedFifoLifetime();
        pendingSourceCapacityRejectsBeforeWorkAndReopensAfterReclaim();
        generalCpuPendingOwnerRejectsEveryStaleIdentityDimension();
        generalCpuWindowReplaysAuthenticatedSourcesInFifoOrder();
        cpuWindowPlanRejectsMissingShapeDimensions();
      }
      else
        generalCpuDestroyWindowUsesProductionLifetime();
      return 0;
    }
    if (argc == 3 && (std::string_view(argv[1]) == "--write-general-cpu-fixture" ||
                     std::string_view(argv[1]) == "--write-general-cpu-mipmap-fixture" ||
                     std::string_view(argv[1]) == "--write-general-cpu-admission-profile-fixture" ||
                     std::string_view(argv[1]) == "--write-general-cpu-query-fixture" ||
                     std::string_view(argv[1]) == "--write-general-cpu-event-checkpoint-fixture" ||
                     std::string_view(argv[1]) == "--write-general-cpu-d24s8-fixture" ||
                     std::string_view(argv[1]) == "--write-general-cpu-volume-fixture" ||
                     std::string_view(argv[1]) == "--write-general-cpu-bootstrap-pages-fixture" ||
                     std::string_view(argv[1]) == "--write-general-cpu-window-fixture")) {
      const std::filesystem::path directory(argv[2]);
      if (!std::filesystem::create_directory(directory))
        throw TestFailure("general fixture output must be a new directory");
      const auto write = [&](const std::filesystem::path& path, const auto& bytes) {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) throw TestFailure("general fixture write failed");
      };
      const auto writeFixture = [&](const auto& fixture) {
        write(directory / "events.bin", fixture.tape);
        for (std::size_t i = 0u; i < fixture.blobs.size(); ++i)
          write(directory / ("blob-" + std::to_string(i) + ".bin"), fixture.blobs[i]);
      };
      if (std::string_view(argv[1]) == "--write-general-cpu-admission-profile-fixture") {
        namespace replay = dxmt9::d3d9;
        namespace runtime = replay::render_tape_runtime;
        auto fixture = replay::test::makeGeneralCpuMipmapFixture();
        std::vector<replay::RenderTapeProviderBlob> blobs;
        for (const auto& bytes : fixture.blobs)
          blobs.push_back({replay::RenderTapeCaptureSession::sha256(bytes), bytes});
        runtime::GeneralPreflightPlan plan;
        check(runtime::buildGeneralReplayPlan(fixture.tape, blobs, plan).complete(),
              "admission profile fixture base must preflight");
        // Fixed-size admission-only input: repeated identical uploads cross
        // the default input bounds without adding any source publications.
        constexpr std::size_t kAdditionalMutations = 2048u;
        replay::RenderTapeBuilder builder;
        bool expanded = false;
        for (std::size_t index = 0u; index < plan.tape.events.size(); ++index) {
          const auto event = plan.tape.event(index);
          const auto type = static_cast<replay::RenderTapeEventType>(event.header.type);
          if (type == replay::RenderTapeEventType::PresentComplete) {
            auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
            replay::RenderTapePresentCompleteHeader complete{};
            std::memcpy(&complete, payload.data(), sizeof(complete));
            complete.presentOrdinal += kAdditionalMutations;
            std::memcpy(payload.data(), &complete, sizeof(complete));
            builder.appendRawEvent(type, payload);
          } else {
            builder.appendRawEvent(type, event.payload);
          }
          if (!expanded && type == replay::RenderTapeEventType::ResourceMutation) {
            for (std::size_t extra = 0u; extra < kAdditionalMutations; ++extra)
              builder.appendRawEvent(type, event.payload);
            expanded = true;
          }
        }
        check(expanded, "admission profile fixture must contain a seed mutation");
        fixture.tape = builder.seal();
        runtime::GeneralPreflightPlan extendedPlan;
        check(runtime::buildGeneralReplayPlan(fixture.tape, blobs, extendedPlan).complete() &&
                  runtime::admitGeneralCpuPlan(extendedPlan).reason ==
                      runtime::GeneralCpuAdmissionReason::EventLimit &&
                  runtime::admitGeneralCpuPlan(
                      extendedPlan, runtime::generalCpuExtendedInputCaps()).accepted(),
              "admission-only fixture must reject default and admit extended input");
        writeFixture(fixture);
      } else if (std::string_view(argv[1]) == "--write-general-cpu-mipmap-fixture")
        writeFixture(dxmt9::d3d9::test::makeGeneralCpuMipmapFixture());
      else if (std::string_view(argv[1]) == "--write-general-cpu-query-fixture")
        writeFixture(dxmt9::d3d9::test::makeGeneralCpuQueryFixture());
      else if (std::string_view(argv[1]) == "--write-general-cpu-event-checkpoint-fixture")
        writeFixture(dxmt9::d3d9::test::makeGeneralCpuEventCheckpointFixture());
      else if (std::string_view(argv[1]) == "--write-general-cpu-d24s8-fixture")
        writeFixture(dxmt9::d3d9::test::makeGeneralCpuD24S8Fixture());
      else if (std::string_view(argv[1]) == "--write-general-cpu-volume-fixture")
        writeFixture(dxmt9::d3d9::test::makeGeneralCpuVolumeFixture());
      else if (std::string_view(argv[1]) == "--write-general-cpu-bootstrap-pages-fixture")
        writeFixture(dxmt9::d3d9::test::makeGeneralCpuBootstrapPagesFixture());
      else
        writeFixture(dxmt9::d3d9::test::makeGeneralCpuDestroyFixture(
            true, true, dxmt9::d3d9::test::GeneralCpuMutationFixture::ReferencedDiscard,
            dxmt9::d3d9::test::GeneralCpuResetFixture::A,
            std::string_view(argv[1]) == "--write-general-cpu-window-fixture" ? 2u : 0u));
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--write-completion-fixture") {
      dxmt9::d3d9::test::writeCompletionTapeFixture(argv[2]);
      return 0;
    }
    if (argc == 3 &&
        std::string_view(argv[1]) == "--write-dynamic-completion-fixture") {
      dxmt9::d3d9::test::writeDynamicCompletionTapeFixture(argv[2]);
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) ==
                         "--write-managed-allocation-failure-fixture") {
      dxmt9::d3d9::test::writeManagedAllocationFailureTapeFixture(argv[2]);
      return 0;
    }
    if (argc == 3 &&
        std::string_view(argv[1]) == "--write-capacity-continuation-fixture") {
      dxmt9::d3d9::test::writeCapacityContinuationTapeFixture(argv[2]);
      return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--tape-experiment") {
      setenv("DXMT9_OFFLOAD_COMMIT_REPLAY", "0", 1);
      setenv("DXMT_PERF_COUNTERS", "1", 1);
      // CPU execution has no presenter worker. Its explicit controller owns
      // acknowledgment; production frame-pacing waits are outside this mode.
      setenv("DXMT9_DISABLE_PRESENT_BOUNDARY", "1", 1);
      setenv("DXMT9_SYNC_PRESENT_FLUSH", "0", 1);
      CpuTapeExperimentOptions options;
      if (argc < 3) {
        throw TestFailure("--tape-experiment requires events.bin");
      }
      options.tapePath = argv[2];
      for (int index = 3; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--blob" && index + 1 < argc) {
          options.blobPaths.emplace_back(argv[++index]);
        } else if (argument == "--source-oracle" && index + 1 < argc) {
          options.sourceOraclePath = argv[++index];
        } else if (argument == "--source-projection-sha256" && index + 1 < argc) {
          options.sourceProjectionSha256 = argv[++index];
        } else if (argument == "--source-boundary-mode" && index + 1 < argc) {
          const std::string_view value(argv[++index]);
          if (value == "chunk") {
            options.sourceBoundaryMode =
                CpuTapeExperimentOptions::SourceBoundaryMode::Chunk;
          } else if (value == "capture-source") {
            options.sourceBoundaryMode =
                CpuTapeExperimentOptions::SourceBoundaryMode::CaptureSource;
          } else {
            throw TestFailure("invalid source boundary mode");
          }
        } else if (argument == "--identity" && index + 1 < argc) {
          options.identityPath = argv[++index];
        } else if (argument == "--source-plan-sha256" && index + 1 < argc) {
          options.sourcePlanSha256 = argv[++index];
        } else if (argument == "--grammar" && index + 1 < argc) {
          const std::string_view value(argv[++index]);
          if (value == "general-frame") options.generalFrame = true;
          else if (value != "bounded") throw TestFailure("invalid CPU grammar");
        } else if (argument == "--input-admission-profile" && index + 1 < argc) {
          options.inputAdmissionProfile = argv[++index];
        } else if (argument == "--route" && index + 1 < argc) {
          const std::string_view value(argv[++index]);
          if (value == "compatibility") options.compatibility = true;
          else if (value != "production-slot") throw TestFailure("invalid route");
        } else if (argument == "--run-mode" && index + 1 < argc) {
          const std::string_view value(argv[++index]);
          if (value != "measure" && value != "verify" && value != "diagnose")
            throw TestFailure("invalid CPU run mode");
          options.measurement = value != "verify";
          options.diagnostic = value == "diagnose";
        } else if ((argument == "--draw-command-limit" ||
                    argument == "--slot-headroom-bytes" ||
                    argument == "--aggregate-headroom-bytes") && index + 1 < argc) {
          const std::string_view value(argv[++index]);
          if (value.empty() || !std::all_of(value.begin(), value.end(),
                  [](char c) { return c >= '0' && c <= '9'; }))
            throw TestFailure("CPU policy requires unsigned decimal values");
          const auto parsed = std::stoull(std::string(value));
          if (parsed > std::numeric_limits<std::size_t>::max())
            throw TestFailure("CPU policy integer overflow");
          if (argument == "--draw-command-limit") options.policy.drawCommandLimit = parsed;
          else if (argument == "--slot-headroom-bytes") options.policy.slotHeadroomBytes = parsed;
          else options.policy.aggregateHeadroomBytes = parsed;
          options.policyExplicit = true;
        } else if (argument == "--completion" && index + 1 < argc) {
          const std::string_view value(argv[++index]);
          options.afterDestroy = false;
          options.beforeNextWork = false;
          if (value == "before-next-work") options.beforeNextWork = true;
          else if (value == "after-destroy") options.afterDestroy = true;
          else if (value != "after-command") throw TestFailure("invalid completion");
        } else if (argument == "--resource-backend" && index + 1 < argc) {
          const std::string_view value(argv[++index]);
          if (value == "cpu-no-gpu") {
            options.resourceBackend = CpuTapeResourceBackend::NoGpu;
          } else if (value == "cpu-metal-resources") {
            options.resourceBackend = CpuTapeResourceBackend::MetalResources;
          } else {
            throw TestFailure("invalid resource backend");
          }
        } else if (argument == "--fault" && index + 1 < argc) {
          options.fault = argv[++index];
          if (options.fault != "none" &&
              options.fault != "before-submit" &&
              options.fault != "before-release" &&
              options.fault != "managed-allocation") {
            throw TestFailure("invalid fault");
          }
        } else if (argument == "--max-pending" && index + 1 < argc) {
          options.maxPending = static_cast<std::size_t>(
              std::stoull(std::string(argv[++index])));
          if (options.maxPending >
              dxmt9::d3d9::kRenderTapeCompletionScheduleCapacity) {
            throw TestFailure("max-pending exceeds bounded capacity");
          }
        } else if (argument == "--repeat" && index + 1 < argc) {
          options.repeat = static_cast<std::size_t>(
              std::stoull(std::string(argv[++index])));
          options.repeatExplicit = true;
          if (options.repeat == 0u ||
              options.repeat > dxmt9::d3d9::kRenderTapeCompletionScheduleCapacity) {
            throw TestFailure("repeat is outside the bounded experiment capacity");
          }
        } else if (argument == "--completion-plan" && index + 1 < argc) {
          options.completionPlanPath = argv[++index];
        } else if (argument == "--publication" && index + 1 < argc) {
          const std::string_view value(argv[++index]);
          if (value == "per-command") {
            options.publication =
                dxmt9::d3d9::RenderTapeCpuPublicationMode::PerCommand;
          } else if (value == "draw-island") {
            options.publication =
                dxmt9::d3d9::RenderTapeCpuPublicationMode::DrawIsland;
          } else {
            throw TestFailure("invalid publication mode");
          }
        } else {
          throw TestFailure("unknown tape experiment option");
        }
      }
      if ((!options.sourceOraclePath.empty() ||
           !options.sourceProjectionSha256.empty()) && !options.generalFrame) {
        throw TestFailure("source oracle requires general-frame grammar");
      }
      const bool captureSourceRequested =
          options.sourceBoundaryMode ==
          CpuTapeExperimentOptions::SourceBoundaryMode::CaptureSource;
      if (!captureSourceRequested &&
          (!options.identityPath.empty() || !options.sourcePlanSha256.empty())) {
        throw TestFailure("identity and source plan require capture-source mode");
      }
      if (captureSourceRequested) {
        if (!options.generalFrame)
          throw TestFailure("capture-source requires general-frame grammar");
        if (options.identityPath.empty())
          throw TestFailure("capture-source requires --identity");
        if (!isNonzeroLowerSha256Hex(options.sourcePlanSha256))
          throw TestFailure("capture-source requires a nonzero lowercase source-plan SHA-256");
      }
      options.policy.compatibility = options.compatibility;
      if ((options.measurement || options.policyExplicit) && !options.generalFrame)
        throw TestFailure("CPU measurement/policy interface requires general-frame grammar");
      if (!dxmt9::test::cpuTapePolicyValid(options.policy))
        throw TestFailure("invalid CPU batch/capacity policy");
      setenv("DXMT9_OFFLOAD_COMMIT_REPLAY",
             options.fault == "managed-allocation" ? "1" : "0", 1);
      if (options.generalFrame) {
        // The checked compatibility limit bounds all internal publications
        // below the no-worker queue's inflight ceiling. Capacity options use
        // the real cached production provisioning policy, before first use.
        setenv("DXMT9_DRAW_CHUNK_COMMAND_LIMIT",
               std::to_string(options.policy.drawCommandLimit).c_str(), 1);
        setenv("DXMT9_DIRECT_SLOT_HEADROOM_BYTES",
               std::to_string(options.policy.slotHeadroomBytes).c_str(), 1);
        setenv("DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES",
               std::to_string(options.policy.aggregateHeadroomBytes).c_str(), 1);
        setenv("DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES", "0", 1);
        setenv("DXMT9_CPU_READY_TAPE", "0", 1);
        setenv("DXMT9_CPU_READY_EARLY_PREFIX", "0", 1);
        setenv("DXMT9_SPLIT_STRETCH_CHUNK", "0", 1);
      }
      if (options.publication ==
          dxmt9::d3d9::RenderTapeCpuPublicationMode::DrawIsland) {
        // Isolate the exact-fit production policy before its cached knobs
        // are first read. No manual reserve or product admission override.
        setenv("DXMT9_DIRECT_SLOT_HEADROOM_BYTES", "0", 1);
        setenv("DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES", "0", 1);
      }
      return runCpuTapeExperiment(options);
    }
    productionGateIsExplicitAndDefaultOff();
    directRawPublishesAndCompletesArenaSource();
    semanticTransferOwnsArenaTransactionUntilSettlement();
    semanticTransferPreEffectAbortRestoresOwner();
    semanticTransferBatchAbortRestoresOwner();
    semanticTransferPostEffectFailureIsTerminal();
    ordinarySegmentConfiguredRawKeepsOneSourceAt512Pages();
    capturedLargeRawPublishesTwoAuthenticatedSources();
    eventCompletionBridgeWaitsExactCapturedEvent();
    rejectedCaptureStillCompletesAcceptedWork();
    mixedSourceLeaseSelectionPreservesSourceOrder();
    providerResolvedEntryRoutesExistingClearPresent();
    oversizeSegmentedPresentTakesOneLegacyRollbackSource();
    resourceBearingDirectCapturesThenMarksExactTicketAndPublishes();
    ordinaryIngressDefersMarksUntilReplay();
    deferredIngressAdoptionAndFailureKeepRawOwnership();
    sameRawLegacyAndDirectProductionOracle();
    ordinaryChunkSlotDirectMatchesLegacyCadenceAndCompletion();
    ordinaryDrawApplyStateDrawUsesCarrierFreeDirectPath();
    compatibilityCutSplitsTheRawIntoTwoLeaseSpans();
    populatedIdentityMultiSpanFallsBackBeforeEffects();
    compatibilityPrefixDirectContinuationOwnsPhysicalCredit();
    compatibilityCutMatchesLegacyDrawSemantics();
    drawApplyStateDrawWithPresentUsesOneLeaseSpan();
    nonTerminalPresentFallsBackToCompatibilityInSourceOrder();
    duplicatePresentFallsBackToCompatibilityInSourceOrder();
    ordinaryUnsupportedClearRangeStaysCompatibilityOwned();
    ordinarySegmentedDrawApplyStateDrawMatchesLegacySemantics();
    directAdmissionRejectionPreservesLegacyDrawBatchGrouping();
    ordinarySourceBatchFoldsBindingAbaWithOneAcquisition();
    productionReplayAppliesSparseStreamFrequenciesTransactionally();
    cpuMipmapObserverPreservesOrderAndQualifiedTexture();
    populatedSlotDrawApplyDrawUsesCarrierFreeContinuation();
    populatedSlotInsufficientCapacityFallsBackBeforeEffects();
    populatedSlotProducerIdentityGapFallsBackBeforeEffects();
    populatedContinuationCommitFailureIsTerminalWithoutRetry();
    lateStateFailureDoesNotDuplicateDiscardedDirectProgress();
    populatedSlotPresentTailFallsBackBeforeEffects();
    directContinuationAdmissionMatchesShapeAndCapacityTruthTable();
    triangleFanNeverEntersDirectReplay();
    ordinaryChunkSlotSameRawCommitFailureDoesNotRetryLegacy();
    ordinaryOversizedDrawSourceUsesCarrierFreeLifecycle();
    captureSourceBoundaryReturnsExactQueuePublication();
    directChunkSlotRollbackAndPostEffectFailureAreFailSafe();
    directChunkSlotCapabilityLifetimeIsTerminalSafe();
    transactionalArenaFailureInjectionDisposition();
    stateOnlyRawMutatesWithoutTicket();
    postSemanticDirectFailureDoesNotFallback();
    batchBuilderFailureRetriesCompleteEventSerialExactlyOnce();
    captureIdentityBeginFailureRetriesCompleteEventSerialExactlyOnce();
    postSemanticBatchPublishFailureFailsStopsWithoutRetry();
    firstOrganicFailureIsRetainedWithCoordinates();
    organicBatchAppendFailureFromReplayFailsOnce();
    workerPressureWaitResumesAfterActiveLeasePublishes();
    boundedCpuGrammarRequiresUnbindBeforeDestroy();
    boundedCompletionReducerExhaustiveNegativeControls();
    controlledCompletionBindsProductionQueueAndPool();
    resourceBackedRoutingDeviceUsesPoolOwnership();
    textureUploadObservationTracksPlainABAndReadOnlyLock();
    providerRoutePinsDirectCompatibilityAndMalformedIdentity();
  } catch (const TestFailure& error) {
    std::cerr << "cpu_ready_production_routing_spec failed: "
              << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "cpu_ready_production_routing_spec unexpected error: "
              << error.what() << '\n';
    return 1;
  }
  std::cout << "cpu_ready_production_routing_spec passed\n";
  return 0;
}
