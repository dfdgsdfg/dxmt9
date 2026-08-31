#include "device_c_common.hpp"
#include "device_c_cpu_ready_plan.hpp"
#include "device_c_chunk_replay.hpp"
#include "device_c_replay_offload.hpp"
#include "dxmt9/com.hpp"
#include "dxmt9/copy_materialization_ledger.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "d3d9_pe_wire_handle.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void digestBytes(std::uint64_t& hash, const void* data,
                 std::size_t size) noexcept {
  constexpr std::uint64_t kPrime = 1099511628211ull;
  const auto* bytes = static_cast<const std::byte*>(data);
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<std::uint8_t>(bytes[i]);
    hash *= kPrime;
  }
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

std::uint64_t effectivePayloadDigest(
    const dxmt9::core::SourcePayloadView& payload) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  digestValue(hash, payload.commandCount());
  for (std::size_t index = 0; index < payload.commandCount(); ++index) {
    const auto source = payload.commandAt(index);
    const auto& command = source.command;
    digestValue(hash, command.kind);
    switch (command.kind) {
    case dxmt9::core::MetalCommandKind::DrawRun:
      if (command.drawRunRecord) {
        const auto& record = *command.drawRunRecord;
        digestValue(hash, record.stateIndex);
        digestValue(hash, record.firstParam);
        digestValue(hash, record.paramCount);
        digestValue(hash, record.payloadOffset);
        digestValue(hash, record.payloadSize);
        digestValue(hash, record.uniformHandle.index);
        digestValue(hash, record.uniformHandle.generation);
        digestValue(hash, record.uniformHandle.hash);
        digestValue(hash, record.invariant.viewportScissorHash);
        digestValue(hash, record.invariant.runStableBindingHash);
        digestValue(hash, record.invariant.streamMask);
        digestValue(hash, record.invariant.textureMask);
        digestValue(hash, record.invariant.samplerStateMask);
      }
      if (command.drawPsoSubview) {
        const auto& pso = *command.drawPsoSubview;
        digestValue(hash, pso.hasShaderContext);
        digestValue(hash, pso.vertexShaderHash);
        digestValue(hash, pso.pixelShaderHash);
        digestValue(hash, pso.vertexDeclHash);
        digestValue(hash, pso.renderStateHash);
        digestValue(hash, pso.textureMask);
        digestValue(hash, pso.samplerStateMask);
        digestValue(hash, pso.renderTargetMask);
        for (const auto handle : pso.colorAttachmentHandles) digestHandle(hash, handle);
        digestHandle(hash, pso.depthStencilHandle);
      }
      if (command.drawState.hot) {
        const auto& hot = *command.drawState.hot;
        digestValue(hash, hot.key.renderStateHash);
        digestValue(hash, hot.streamMask);
        digestValue(hash, hot.textureMask);
        digestValue(hash, hot.renderTargetMask);
        digestHandle(hash, hot.indexBuffer);
        for (const auto handle : hot.streamBuffers) digestHandle(hash, handle);
        for (const auto handle : hot.textures) digestHandle(hash, handle);
      }
      for (const auto& param : command.drawParams) {
        digestDrawParam(hash, param);
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
    std::size_t commandCount = 0;
    std::size_t arenaSegmentCount = 0;
    core::Handle presentSource{};
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

  static std::size_t readyCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.readyCount();
  }

  static std::size_t residentCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.cpuReadyTape_.residentCount();
  }

  static std::size_t writingCommandCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    if (!queue.writingSlot_ || *queue.writingSlot_ >= queue.slots_.size() ||
        !queue.slots_[*queue.writingSlot_].payload) {
      return 0;
    }
    return queue.slots_[*queue.writingSlot_].payload->commandCount();
  }

  static bool stopped(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.stop_;
  }

  static DrawDigest writingDrawDigest(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    DrawDigest result{};
    if (!queue.writingSlot_) return result;
    const core::SourcePayloadView view(
        *queue.slots_[*queue.writingSlot_].payload);
    if (view.commandCount() == 0u) return result;
    const auto& command = view.commandAt(0u).command;
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

RecordSpec drawRecord(const D9CWireObjectIdentity& bufferIdentity,
                      std::uint32_t streamHandleIndex = 0u) {
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
      .slot = 0u,
      .valid = 1u,
      .handleIndex = streamHandleIndex,
      .offset = 0u,
      .stride = 16u,
      .frequency = 1u,
  };
  std::vector<std::byte> payload(
      draw.sectionPayloadOffset + sizeof(stream));
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + draw.sectionTableOffset,
              &section, sizeof(section));
  std::memcpy(payload.data() + draw.sectionPayloadOffset,
              &stream, sizeof(stream));
  return {
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      .payload = std::move(payload),
      .handles = {dxmt9::d3d9::wireHandleEntry(bufferIdentity)},
  };
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
  explicit RoutingDevice(bool rejectAfterClear = false,
                         bool segmentSerial = false,
                         bool directChunkSlot = false)
      : queue_(
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
        directChunkSlot_(directChunkSlot) {}

  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  dxmt9::CommandQueue& queue() override { return queue_; }
  const BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<BackendDevice> backend() override { return {}; }
  bool supportsCpuReadyArenaReplay() const noexcept override { return true; }
  bool supportsDirectChunkSlotReplay() const noexcept override {
    return directChunkSlot_;
  }

  BufferHandle createBuffer(const BufferDesc& desc) override {
    return queue_.pool().createBuffer(WMT::Device{NULL_OBJECT_HANDLE}, desc);
  }

  SurfaceHandle createSurface(const SurfaceDesc&) override {
    return SurfaceHandle{nextHandle_++};
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
    return queue_.captureChunkBufferBindings(entries, snapshots);
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
    ++drawCalls;
    queue_.submitDrawRun(std::move(state), uniforms, draws, payloads);
  }

  void submitDrawRunBatch(
      std::span<DrawRunSubmission> submissions) override {
    ++drawBatchCalls;
    lastDrawBatchSize = static_cast<std::uint32_t>(submissions.size());
    drawCalls += static_cast<std::uint32_t>(submissions.size());
    queue_.submitDrawRunBatch(submissions);
  }

  DirectReplayDrawDisposition submitDirectReplayDraw(
      const DirectReplayDrawInput& input) noexcept override {
    ++drawCalls;
    return queue_.submitDirectReplayDraw(input);
  }

  void present(const SwapDesc& desc) override {
    ++presentCalls;
    lastPresentSeqId = queue_.submitPresent(desc);
  }

  BackendLimits limits_{};
  dxmt9::CommandQueue queue_;
  bool rejectAfterClear_ = false;
  bool rejectAfterFinalClear_ = false;
  bool directChunkSlot_ = false;
  std::uint64_t nextHandle_ = 1;
  std::atomic<std::uint32_t> clearCalls{0};
  std::atomic<std::uint32_t> drawCalls{0};
  std::atomic<std::uint32_t> drawBatchCalls{0};
  std::atomic<std::uint32_t> presentCalls{0};
  std::uint32_t lastDrawBatchSize = 0;
  std::uint64_t lastPresentSeqId = 0;
  std::uint32_t captureCalls = 0;
  std::uint32_t legacyMarkCalls = 0;
  std::uint64_t capturedBufferLastUsedSeq = UINT64_MAX;
  std::thread::id captureThread{};
  std::vector<ChunkHandleEntry> capturedResources;
  dxmt9::CommandQueue::CpuReadyArenaFailureSnapshot organicFailure{};
  bool organicFailureObserved = false;
};

struct RuntimeFixture {
  explicit RuntimeFixture(bool rejectAfterClear = false,
                          bool segmentSerial = false,
                          bool directChunkSlot = false) {
    auto upper = std::make_unique<RoutingDevice>(rejectAfterClear,
                                                  segmentSerial,
                                                  directChunkSlot);
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
    device = factory->CreateDeviceEx(0, params, nullptr);
    check(device != nullptr, "routing core device must construct");
    device->AddRef();
    cDevice = std::make_unique<D9CDevice>(device);
  }

  ~RuntimeFixture() {
    cDevice.reset();
    if (device) {
      (void)device->Release();
    }
    if (factory) {
      (void)factory->Release();
    }
  }

  RoutingDevice* routing = nullptr;
  dxmt9::com::IDirect3D9Ex* factory = nullptr;
  dxmt9::com::IDirect3DDevice9Ex* device = nullptr;
  std::unique_ptr<D9CDevice> cDevice;
};

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
      drawRecord(identity), clearRecord(),
      stretchRectRecord(sourceIdentity, destinationIdentity, 1u)};
  const auto wire = makeWireFixture(records);
  auto directRaw = makeRaw(wire, 71u, false, &registry);
  auto legacyRaw = makeRaw(wire, 71u, false, &registry);
  const std::array resources{
      ChunkHandleEntry{.kind = ChunkHandleKind::Buffer,
                       .handle = buffer->obj->handle()},
      ChunkHandleEntry{.kind = ChunkHandleKind::Surface,
                       .handle = source->obj->handle()},
      ChunkHandleEntry{.kind = ChunkHandleKind::Surface,
                       .handle = destination->obj->handle()},
  };
  directRaw.resourceEntries.assign(resources.begin(), resources.end());
  legacyRaw.resourceEntries.assign(resources.begin(), resources.end());
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
            fixture.routing->drawCalls == 1u,
        "direct mixed Draw/Clear/copy construction preserves the ordinary "
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
  check(fixture.device->SetStreamSource(0u, nullptr, 0u, 0u) == D3D_OK,
        "ordinary differential restores its initial stream state");
  fixture.routing->directChunkSlot_ = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), legacyRaw) == D3D_OK &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                fixture.routing->queue_) == directWritingCommandCount &&
            fixture.routing->drawCalls == 2u &&
            fixture.routing->legacyMarkCalls == 1u,
        "the same raw takes the typed Legacy lane when direct support is absent");
  const auto legacyDrawDigest =
      dxmt9::CommandQueueArenaLeaseTestAccess::writingDrawDigest(
          fixture.routing->queue_);
  fixture.routing->present(SwapDesc{});
  const auto legacyCompletion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(directCompletion.dequeued && !directCompletion.arena &&
            directCompletion.submitted && directCompletion.completed &&
            directCompletion.reclaimed &&
            legacyCompletion.dequeued && !legacyCompletion.arena &&
            legacyCompletion.submitted && legacyCompletion.completed &&
            legacyCompletion.reclaimed &&
            directCompletion.commandCount == directWritingCommandCount + 1u &&
            directCompletion.commandCount == legacyCompletion.commandCount &&
            directCompletion.effectiveDigest == legacyCompletion.effectiveDigest &&
            directDrawDigest.record == legacyDrawDigest.record &&
            directDrawDigest.pso == legacyDrawDigest.pso &&
            directDrawDigest.hot == legacyDrawDigest.hot &&
            directDrawDigest.params == legacyDrawDigest.params &&
            directDrawDigest.payload == legacyDrawDigest.payload &&
            directDrawDigest.payloadBytes == legacyDrawDigest.payloadBytes,
        "ordinary direct and Legacy final ChunkSlots are completion- and semantic-neutral: "
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
  dxmt9c_surface_release(source);
  dxmt9c_surface_release(destination);
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
            fixture.routing->drawCalls == 2u &&
            fixture.routing->drawBatchCalls == 0u &&
            fixture.routing->legacyMarkCalls == 0u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                fixture.routing->queue_) == 2u,
        "Direct Draw/ApplyState/Draw must bypass DrawRunSubmission carrier");

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

void drawApplyStateDrawWithPresentRetainsLegacyCarrierBoundary() {
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
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(imported, 92u);
  check(plan.directArenaCandidate() &&
            dxmt9::d3d9::classifyDirectChunkSlotReplay(
                imported, plan, /*captureOrTrace=*/false) ==
                dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyPresent,
        "Present-bearing raw remains fail-closed LegacyPresent");

  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
            fixture.routing->drawCalls == 2u &&
            fixture.routing->drawBatchCalls == 2u &&
            fixture.routing->lastDrawBatchSize == 1u &&
            fixture.routing->presentCalls == 1u,
        "Present-bearing Draw/ApplyState/Draw must retain Legacy carriers");
  const auto completion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(completion.dequeued && !completion.arena && completion.hasPresent &&
            completion.submitted && completion.completed &&
            completion.reclaimed,
        "Legacy carrier boundary preserves Present completion");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
}

void ordinarySingleSourceSegmentedAggregateUsesDirectConstruction() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  const auto rects = clearRectCountForPlannerPages(40);
  const std::array records{clearRecord(rects), clearRecord(rects)};
  const auto wire = makeWireFixture(records);
  dxmt9::d3d9::ImportedChunkView imported;
  check(validateCommandChunk(wire.bytes, wire.envelope, &imported).valid(),
        "single-source segmented aggregate fixture validates");
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(
      imported, 93u,
      {.pageSize = 4096u,
       .maxOrdinaryPagesPerSegment = 64u,
       .maxSegmentsPerSource = 2u,
       .maxPagesPerSource = 128u,
       .maxPages = 128u,
       .maxSourcesPerChunk = 1u});
  check(plan.directArenaCandidate() && plan.sourceCount == 1u &&
            plan.segmentCount == 2u && plan.arenaLayout.has_value() &&
            !plan.layout.has_value() &&
            dxmt9::d3d9::classifyDirectChunkSlotReplay(
                imported, plan, /*captureOrTrace=*/false) ==
                dxmt9::d3d9::DirectChunkSlotReplayDisposition::Direct,
        "one logical source may aggregate multiple checked physical segments");

  auto raw = makeRaw(wire, 93u);
  raw.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) == D3D_OK &&
            fixture.routing->clearCalls == 2u &&
            fixture.routing->drawBatchCalls == 0u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                fixture.routing->queue_) == 2u,
        "segmented single-source aggregate must construct Direct ChunkSlot");
  fixture.routing->present(SwapDesc{});
  const auto completion =
      dxmt9::CommandQueueArenaLeaseTestAccess::consumeOne(
          fixture.routing->queue_);
  check(completion.dequeued && !completion.arena && completion.commandCount == 3u &&
            completion.submitted && completion.completed &&
            completion.reclaimed,
        "segmented aggregate preserves one ordinary completion identity");
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
  check(plan.directArenaCandidate() && plan.sourceCount == 1u &&
            plan.segmentCount == 2u && plan.arenaLayout.has_value() &&
            !plan.layout.has_value() &&
            dxmt9::d3d9::classifyDirectChunkSlotReplay(
                imported, plan, /*captureOrTrace=*/false) ==
                dxmt9::d3d9::DirectChunkSlotReplayDisposition::Direct,
        "segmented Draw/ApplyState/Draw is one validated Direct aggregate");

  auto direct = makeRaw(wire, 94u, false, &registry);
  auto legacy = makeRaw(wire, 94u, false, &registry);
  direct.cpuReadyTapePlanningEnabled = false;
  legacy.cpuReadyTapePlanningEnabled = false;
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), direct) == D3D_OK &&
            fixture.routing->clearCalls == 2u &&
            fixture.routing->drawCalls == 2u &&
            fixture.routing->drawBatchCalls == 0u,
        "segmented Draw/ApplyState/Draw uses carrier-free Direct replay");
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
  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), legacy) == D3D_OK &&
            fixture.routing->clearCalls == 4u &&
            fixture.routing->drawCalls == 4u &&
            fixture.routing->drawBatchCalls == 2u,
        "the same segmented Draw/ApplyState/Draw takes the Legacy carrier lane");
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
  check(directCompletion.dequeued && directCompletion.submitted &&
            directCompletion.completed && directCompletion.reclaimed &&
            legacyCompletion.dequeued && legacyCompletion.submitted &&
            legacyCompletion.completed && legacyCompletion.reclaimed &&
            directWritingCommandCount == legacyWritingCommandCount &&
            directCompletion.commandCount == legacyCompletion.commandCount &&
            // The carrier and Direct forms intentionally have different
            // internal state/PSO identities. Compare the stable draw
            // parameters and payload bytes, which are the semantic ABI.
            directDrawDigest.params == legacyDrawDigest.params &&
            directDrawDigest.payload == legacyDrawDigest.payload &&
            directDrawDigest.payloadBytes == legacyDrawDigest.payloadBytes,
        "segmented Direct and Legacy preserve exact command and draw semantics");
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
  check(fixture.routing->drawCalls == 2u &&
            fixture.routing->drawBatchCalls == 1u &&
            fixture.routing->lastDrawBatchSize == 2u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                fixture.routing->queue_) == 2u,
        "Legacy fallback must preserve one submitDrawRunBatch group for the raw draws");

  dxmt9::d3d9::releaseRetainedWrappers(raw);
  dxmt9c_buffer_release(buffer);
  dxmt9c_buffer_release(secondBuffer);
}

void ordinaryChunkSlotSameRawCommitFailureDoesNotRetryLegacy() {
  RuntimeFixture fixture(/*rejectAfterClear=*/false,
                         /*segmentSerial=*/false,
                         /*directChunkSlot=*/true);
  const std::array records{clearRecord()};
  auto raw = makeRaw(makeWireFixture(records), 79u);
  raw.cpuReadyTapePlanningEnabled = false;
  dxmt9::CommandQueueArenaLeaseTestAccess::
      forceNextDirectChunkSlotCommitFailure(fixture.routing->queue_);

  check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw) != D3D_OK &&
            fixture.routing->clearCalls == 1u &&
            fixture.routing->legacyMarkCalls == 0u,
        "same-raw ordinary Direct commit failure applies semantics once and "
        "never retries Legacy");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::stopped(
            fixture.routing->queue_) &&
            dxmt9::CommandQueueArenaLeaseTestAccess::readyCount(
                fixture.routing->queue_) == 0u &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                fixture.routing->queue_) == 1u,
        "same-raw post-effect failure fail-stops before publication without "
        "rolling back the applied prefix");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
}

void directChunkSlotRollbackAndPostEffectFailureAreFailSafe() {
  SourcePayloadCapacity capacity{};
  capacity.commandHeaders = 1;
  capacity.clearRecords = 1;

  dxmt9::CommandQueue rollbackQueue(
      dxmt9::CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  auto rollback = rollbackQueue.beginDirectChunkSlotReplay(
      81u, capacity, sizeof(ClearDesc));
  check(rollback.status ==
                dxmt9::CommandQueue::DirectChunkSlotReplayStatus::Ready &&
            rollback.lease.has_value(),
        "pre-effect rollback fixture acquires a generation-qualified borrow");
  rollbackQueue.submitClear(ClearDesc{});
  check(rollback.lease->rollbackPreEffect() &&
            dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
                rollbackQueue) == 0u &&
            !dxmt9::CommandQueueArenaLeaseTestAccess::stopped(rollbackQueue),
        "pre-effect rollback restores the exact writing-slot checkpoint");

  dxmt9::CommandQueue unsettledQueue(
      dxmt9::CommandQueue::ArenaLeaseTestQueueTag{}, BackendLimits{});
  {
    auto unsettled = unsettledQueue.beginDirectChunkSlotReplay(
        83u, capacity, sizeof(ClearDesc));
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
      82u, capacity, sizeof(ClearDesc));
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
      0u, capacity, sizeof(ClearDesc));
  check(unsupported.status ==
            dxmt9::CommandQueue::DirectChunkSlotReplayStatus::LegacyUnsupported,
        "invalid pre-effect admission retains a typed Legacy disposition");
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

}  // namespace

int main() {
  try {
    productionGateIsExplicitAndDefaultOff();
    directRawPublishesAndCompletesArenaSource();
    ordinarySegmentConfiguredRawKeepsOneSourceAt512Pages();
    capturedLargeRawPublishesTwoAuthenticatedSources();
    mixedSourceLeaseSelectionPreservesSourceOrder();
    providerResolvedEntryRoutesExistingClearPresent();
    oversizeSegmentedPresentTakesOneLegacyRollbackSource();
    resourceBearingDirectCapturesThenMarksExactTicketAndPublishes();
    sameRawLegacyAndDirectProductionOracle();
    ordinaryChunkSlotDirectMatchesLegacyCadenceAndCompletion();
    ordinaryDrawApplyStateDrawUsesCarrierFreeDirectPath();
    drawApplyStateDrawWithPresentRetainsLegacyCarrierBoundary();
    ordinarySingleSourceSegmentedAggregateUsesDirectConstruction();
    ordinarySegmentedDrawApplyStateDrawMatchesLegacySemantics();
    directAdmissionRejectionPreservesLegacyDrawBatchGrouping();
    ordinaryChunkSlotSameRawCommitFailureDoesNotRetryLegacy();
    directChunkSlotRollbackAndPostEffectFailureAreFailSafe();
    transactionalArenaFailureInjectionDisposition();
    stateOnlyRawMutatesWithoutTicket();
    postSemanticDirectFailureDoesNotFallback();
    batchBuilderFailureRetriesCompleteEventSerialExactlyOnce();
    captureIdentityBeginFailureRetriesCompleteEventSerialExactlyOnce();
    postSemanticBatchPublishFailureFailsStopsWithoutRetry();
    firstOrganicFailureIsRetainedWithCoordinates();
    organicBatchAppendFailureFromReplayFailsOnce();
    workerPressureWaitResumesAfterActiveLeasePublishes();
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
