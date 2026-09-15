#include "device_c_common.hpp"
#include "device_c_cpu_ready_plan.hpp"
#include "device_c_chunk_replay.hpp"
#include "device_c_cpu_ready_transfer.hpp"
#include "device_c_render_tape_completion.hpp"
#include "device_c_render_tape_completion_plan.hpp"
#include "device_c_render_tape_publication.hpp"
#include "device_c_render_tape.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_provider.hpp"
#include "render_tape_completion_fixture.hpp"
#include "render_tape_cpu_grammar.hpp"
#include "render_tape_cpu_general_grammar.hpp"
#include "render_tape_cpu_general_executor.hpp"
#include "render_tape_cpu_policy.hpp"
#include "render_tape_cpu_source_admission.hpp"
#include "render_tape_cpu_destroy_fixture.hpp"
#include "render_tape_cpu_alias_fixture.hpp"
#include "render_tape_cpu_mipmap_fixture.hpp"
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
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
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
                         WMT::Device resourceDevice = {})
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
                : dxmt9::render::RenderPartitionConfig{}),
        rejectAfterClear_(rejectAfterClear),
        directChunkSlot_(directChunkSlot),
        cpuReadyArena_(cpuReadyArena),
        resourceDevice_(resourceDevice) {
    if (static_cast<bool>(resourceDevice_)) {
      dxmt9::CommandQueueArenaLeaseTestAccess::bindDevice(queue_,
                                                          resourceDevice_);
    }
  }

  WMT::Device wmtDevice() override { return resourceDevice_; }
  dxmt9::CommandQueue& queue() override { return queue_; }
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
  explicit RuntimeFixture(bool rejectAfterClear = false,
                          bool segmentSerial = false,
                          bool directChunkSlot = false,
                          bool cpuReadyArena = true,
                          WMT::Device resourceDevice = {},
                          dxmt9::d3d9::ReplayQueueAllocationFailure*
                              allocationFailure = nullptr,
                          const PresentParameters* outputParameters = nullptr) {
    allocationFailure_ = allocationFailure;
    auto upper = std::make_unique<RoutingDevice>(rejectAfterClear,
                                                  segmentSerial,
                                                  directChunkSlot,
                                                  cpuReadyArena,
                                                  resourceDevice);
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

  // Cold CPU iterations are fresh-device, not warm resets. Keep the real
  // backend alive while destroying every core/cache owner, so Pool and queue
  // reclamation can be checked afterwards without a dangling routing pointer.
  void releaseCoreForInspection() {
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
};

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
            directFixture.routing->combinedMarkCalls == 1u,
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
            compatibilityFixture.routing->combinedMarkCalls == 1u,
        "provider Compatibility must execute the historical combined mark path");

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
  check(!dxmt9::resolveCpuReadyTapeDirectReplayEnabled(nullptr, "1", nullptr) &&
            !dxmt9::resolveCpuReadyTapeDirectReplayEnabled(nullptr, "1", "") &&
            !dxmt9::resolveCpuReadyTapeDirectReplayEnabled(nullptr, "0", "Z:\\\\capture") &&
            dxmt9::resolveCpuReadyTapeDirectReplayEnabled(
                nullptr, "1", "Z:\\\\capture") &&
            dxmt9::resolveCpuReadyTapeDirectReplayEnabled(
                "1", nullptr, nullptr),
        "an installed Render Tape publisher selects the production source "
        "provider without changing capture-off or output-root-free runs");
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
  bool measurement = false;
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

int runGeneralCpuTapeExperiment(const CpuTapeExperimentOptions& options) {
  namespace runtime = dxmt9::d3d9::render_tape_runtime;
  const bool captureSourceRequested =
      options.sourceBoundaryMode ==
      CpuTapeExperimentOptions::SourceBoundaryMode::CaptureSource;
  const auto reject = [&](const char* status, const std::string& reason) {
    std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1","grammar":"general-frame","backend":"cpu-metal-resources","status":)"
              << std::quoted(status) << R"(,"reason":)" << std::quoted(reason)
              << "}\n";
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
  const auto admission = runtime::admitGeneralCpuPlan(plan);
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

  using Executor = dxmt9::test::CpuGeneralTapeExecutor<
      RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
  struct Iteration {
    Executor::Result execution;
    std::uint64_t direct = 0u, compatibility = 0u, elapsed = 0u;
    dxmt9::perf::RenderTapeReplaySnapshot before{}, after{};
    std::optional<std::uint64_t> nativeCpuNs;
  };
  std::vector<Iteration> results;
  results.reserve(options.repeat);
  for (std::size_t index = 0u; index < options.repeat; ++index) {
    const auto begin = std::chrono::steady_clock::now();
    const auto nativeCpuBegin = sampleProcessCpuNs();
    Iteration iterationResult;
    {
      RuntimeFixture fixture(false, false, true, false,
                             WMT::Device{resourceDevice.handle}, nullptr, &output);
      fixture.routing->recordBoundedCapturedBindings = false;
      iterationResult.before = dxmt9::perf::snapshotRenderTapeReplay();
      Executor executor(fixture, plan, blobs, {.route = options.compatibility
          ? dxmt9::d3d9::ProviderReplayPolicy::Compatibility
          : dxmt9::d3d9::ProviderReplayPolicy::ProductionSlot,
          .beforeNextWork = options.beforeNextWork,
          .maxPendingSources = options.maxPending,
          .holdSafeBufferMutation = sourceOracleRequested,
          .sourceOracle = sourceOracleRows,
          .measurement = options.measurement,
          .captureSourcePlan = captureSourceRequested ? &captureSourcePlan : nullptr,
          .captureSourceTape = captureSourceRequested ?
              std::span<const std::byte>(tape) : std::span<const std::byte>{},
          .captureSourceIdentitySidecar = captureSourceRequested ?
              std::span<const std::byte>(identitySidecar) :
              std::span<const std::byte>{}});
      iterationResult.execution = executor.run();
      iterationResult.after = dxmt9::perf::snapshotRenderTapeReplay();
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
    }
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
               value.execution.draws == results.front().execution.draws &&
               value.execution.commands == results.front().execution.commands;
      });
  const bool nativeCpuAvailable =
      !results.empty() &&
      std::any_of(results.begin(), results.end(), [](const auto& value) {
        return value.nativeCpuNs.has_value();
      });
  std::cout << R"({"schema":"dxmt9.render_tape.cpu_experiment.v1","grammar":"general-frame","semantic_projection":"final-slot-general-v2","backend":"cpu-metal-resources","resource_execution":"cpu-visible-metal-no-gpu-submit","timing_backend":"encode-substituted","status":)"
            << std::quoted(complete ? "complete" : "unresolved")
            << R"(,"run_mode":)" << std::quoted(options.measurement ? "measure" : "verify")
            << R"(,"phase_accounting":)"
            << std::quoted(options.measurement ? "iteration-only" : "scoped-observations")
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
            << R"(,"input_counts":{"events":)" << admission.counts.events
            << R"(,"objects":)" << admission.counts.objects
            << R"(,"mutations":)" << admission.counts.mutations
            << R"(,"chunks":)" << admission.counts.chunks
            << R"(,"records":)" << admission.counts.records
            << R"(,"draws":)" << admission.counts.draws << R"(},"iterations":[)";
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
              << R"(,"semantic_digest":")" << result.digest << '"'
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

void generalCpuMeasurementOmitsOracleOnly() {
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
    for (unsigned phase = 0u; phase < 3u; ++phase) {
      const bool measurement = phase == 1u;
      RuntimeFixture fixture(false, false, true, false,
                             WMT::Device{resourceDevice.handle}, nullptr, &output);
      Executor executor(fixture, plan, blobs, {.route = route,
                                             .measurement = measurement});
      auto result = executor.run();
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

}  // namespace

int main(int argc, char** argv) {
  try {
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
                      std::string_view(argv[1]) == "--general-cpu-alias-spec" ||
                      std::string_view(argv[1]) ==
                          "--two-pending-production-sources-spec" ||
                      std::string_view(argv[1]) ==
                          "--cpu-tape-phase-ledger-spec" ||
                      std::string_view(argv[1]) ==
                          "--capture-source-policy-spec")) {
      // Isolate cached policies from the unrelated routing test matrix.
      // The general no-GPU executor requires the same admission/pacing pins
      // as its CLI, established before the first device is constructed.
      setenv("DXMT9_DISABLE_PRESENT_BOUNDARY", "1", 1);
      setenv("DXMT9_SYNC_PRESENT_FLUSH", "0", 1);
      setenv("DXMT9_OFFLOAD_COMMIT_REPLAY", "0", 1);
      setenv("DXMT9_DRAW_CHUNK_COMMAND_LIMIT", "0", 1);
      setenv("DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES", "0", 1);
      setenv("DXMT9_CPU_READY_TAPE", "0", 1);
      setenv("DXMT9_CPU_READY_EARLY_PREFIX", "0", 1);
      setenv("DXMT9_SPLIT_STRETCH_CHUNK", "0", 1);
      if (std::string_view(argv[1]) == "--capture-source-policy-spec") {
        providerSourcePolicyPreservesRawOrderAndSettlement();
        sourcePublicationCaptureOwnsEveryPublicationAndFailsClosed();
        generalCpuCaptureSourcePlanUsesAuthenticatedExecutor();
      } else if (std::string_view(argv[1]) == "--cpu-tape-phase-ledger-spec") {
        cpuTapePhaseLedgerNestedScopeSpec();
      } else if (std::string_view(argv[1]) == "--general-cpu-mipmap-spec") {
        cpuMipmapObserverPreservesOrderAndQualifiedTexture();
        generalCpuMipmapUsesProductionLifetime();
      } else if (std::string_view(argv[1]) == "--general-cpu-mutation-spec")
        generalCpuMutationWindowUsesProductionLifetime();
      else if (std::string_view(argv[1]) == "--general-cpu-source-oracle-spec")
        generalCpuSourceOracleUsesAuthenticatedClosures();
      else if (std::string_view(argv[1]) == "--general-cpu-reset-spec")
        generalCpuResetQualificationUsesFreshFixtures();
      else if (std::string_view(argv[1]) == "--general-cpu-measurement-spec")
        generalCpuMeasurementOmitsOracleOnly();
      else if (std::string_view(argv[1]) == "--general-cpu-alias-spec")
        generalCpuAliasAfterParentDestroyUsesImmutableIdentity();
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
      if (std::string_view(argv[1]) == "--write-general-cpu-mipmap-fixture")
        writeFixture(dxmt9::d3d9::test::makeGeneralCpuMipmapFixture());
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
        } else if (argument == "--route" && index + 1 < argc) {
          const std::string_view value(argv[++index]);
          if (value == "compatibility") options.compatibility = true;
          else if (value != "production-slot") throw TestFailure("invalid route");
        } else if (argument == "--run-mode" && index + 1 < argc) {
          const std::string_view value(argv[++index]);
          if (value == "measure") options.measurement = true;
          else if (value != "verify") throw TestFailure("invalid CPU run mode");
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
