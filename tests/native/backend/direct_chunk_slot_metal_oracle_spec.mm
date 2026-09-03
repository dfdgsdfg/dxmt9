// Concrete production oracle for the ordinary Direct ChunkSlot route.
//
// Each child process creates the real dxmt9::Device after selecting one
// DXMT9_DIRECT_CHUNK_SLOT_REPLAY value, replays a noncommutative
// Draw/flush/Clear/flush/Draw/Present sequence, and reads the resulting
// offscreen texture back from Metal at every cut. The Direct child also
// runs a separate forced-commit probe: the existing production test seam can
// fire only from commitDirectChunkSlotReplay, so a fail-stop result proves the
// ordinary route was entered before the uninstrumented pixel run.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "device_c_common.hpp"
#include "device_c_chunk_replay.hpp"
#include "device_c_replay_offload.hpp"
#include "dxmt9/com.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/dxmt9_command_queue.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace dxmt9 {

// This access type is already a declared friend of CommandQueue. Keep this
// test-local and value-only: it reads the existing native commit-failure seam
// and the queue watermarks needed to compare command/resource/completion
// identity; it does not add a production observer or alter the hot path.
struct CommandQueueArenaLeaseTestAccess {
  static bool beginDirectLifecycleObservation(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    if (!queue.pipelineLifecycleObserver_) return false;
    queue.pipelineLifecycleObserver_->resetObservationWindow();
    return true;
  }

  static void forceNextDirectChunkSlotCommitFailure(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    queue.testOnlyForceNextDirectChunkSlotCommitFailure_ = true;
  }

  static bool stopped(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.stop_;
  }

  static bool directLifecycleMissingTerminalMatchFailsClosed(
      CommandQueue& queue) {
    std::uint64_t sourceGeneration = 0u;
    std::uint64_t storageGeneration = 0u;
    std::uint64_t seqId = 0u;
    std::uint32_t sourceIndex = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t firstPage = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t pageCount = 0u;
    std::size_t controlIndex = std::numeric_limits<std::size_t>::max();
    {
      std::lock_guard lock(queue.mutex_);
      if (!queue.pipelineLifecycleObserver_ ||
          queue.pipelineLifecycleObserver_->directState().recordCount == 0u) {
        return false;
      }
      const auto& record = queue.pipelineLifecycleObserver_->directState()
                               .records[0];
      sourceGeneration = record.identity.sourceGeneration;
      storageGeneration = record.identity.storageGeneration;
      seqId = record.identity.seqId;
      sourceIndex = record.identity.sourceIndex;
      firstPage = record.identity.firstPage;
      pageCount = record.identity.pageCount;
      controlIndex = record.identity.destinationSlot;
    }
    if (sourceGeneration == 0u || storageGeneration == 0u || seqId == 0u) {
      return false;
    }
    // Keep the source generations valid but change the sequence locator. This
    // exercises the production terminal adapter's admitted-source
    // missing-match path, rather than calling the observer callback directly.
    const auto wrongSeq = seqId == std::numeric_limits<std::uint64_t>::max()
        ? seqId - 1u : seqId + 1u;
    std::lock_guard lock(queue.mutex_);
    queue.queueLifecycle_.observeDirectLifecycleForSourceLocked(
        core::CpuReadyTape::SourceRef{
            .id = {.index = sourceIndex, .generation = sourceGeneration},
            .storage = {.firstPage = firstPage, .pageCount = pageCount,
                        .generation = storageGeneration}},
        controlIndex, wrongSeq, queue::DirectSourceAction::Encode,
        /*requireMatch=*/false);
    const auto error = queue.pipelineLifecycleObserver_
        ? queue.pipelineLifecycleObserver_->directError()
        : queue::DirectSourceLifecycleError::None;
    return queue.pipelineLifecycleObserver_ &&
        error != queue::DirectSourceLifecycleError::None;
  }

  static std::uint64_t lastCommittedSeqId(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    return queue.lastCommittedSeqId_;
  }

  static std::uint64_t completedSeqId(CommandQueue& queue) {
    return queue.completedSeqIdAcquire();
  }

  static std::size_t writingCommandCount(CommandQueue& queue) {
    std::lock_guard lock(queue.mutex_);
    if (!queue.writingSlot_ || *queue.writingSlot_ >= queue.slots_.size()) {
      return 0u;
    }
    return queue.slots_[*queue.writingSlot_].commandCount();
  }

  static bool directLifecycleComplete(CommandQueue& queue,
                                      bool expectPresent) {
    std::lock_guard lock(queue.mutex_);
    if (!queue.pipelineLifecycleObserver_ ||
        queue.pipelineLifecycleObserver_->directError() !=
            queue::DirectSourceLifecycleError::None) {
      return false;
    }
    const auto& state = queue.pipelineLifecycleObserver_->directState();
    if (state.recordCount != 2u) {
      return false;
    }
    for (std::size_t i = 0; i < state.recordCount; ++i) {
      const auto& record = state.records[i];
      if (record.phase != queue::DirectSourcePhase::Reclaimed ||
          !record.completed || record.poisoned || record.detachedCredit != 0u ||
          record.publicationCount != 1u) {
        return false;
      }
    }
    const auto& first = state.records[0];
    const auto& last = state.records[1];
    return (first.identity.rawOrdinal < last.identity.rawOrdinal ||
            (first.identity.rawOrdinal == last.identity.rawOrdinal &&
             first.identity.spanOrdinal < last.identity.spanOrdinal)) &&
           first.identity.sourceOrdinal <= last.identity.sourceOrdinal &&
           first.physicalCreditOwner && last.physicalCreditOwner &&
           !first.hasPresent && last.hasPresent == expectPresent &&
           state.lastReclaimedRawOrdinal == last.identity.rawOrdinal &&
           state.lastReclaimedSpanOrdinal == last.identity.spanOrdinal &&
           state.lastReclaimedSourceOrdinal == last.identity.sourceOrdinal;
  }

  static bool waitForDirectLifecycleReclaim(CommandQueue& queue) {
    std::unique_lock lock(queue.mutex_);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      if (queue.pipelineLifecycleObserver_ &&
          queue.pipelineLifecycleObserver_->directError() ==
              queue::DirectSourceLifecycleError::None) {
        const auto& state = queue.pipelineLifecycleObserver_->directState();
        if (state.recordCount == 2u &&
            state.records[0].phase == queue::DirectSourcePhase::Reclaimed &&
            state.records[1].phase == queue::DirectSourcePhase::Reclaimed) {
          return true;
        }
      }
      queue.writeCv_.wait_for(lock, std::chrono::milliseconds(1));
    }
    return false;
  }
};

}  // namespace dxmt9

namespace {

constexpr std::uint32_t kWidth = 16u;
constexpr std::uint32_t kHeight = 16u;
constexpr std::uint32_t kTriangleFvf = 0x0044u; // D3DFVF_XYZRHW | DIFFUSE.
// A8R8G8B8 is read back from the BGRA8Unorm Metal backing in byte order
// B,G,R,A. The triangle covers every pixel center in the 16x16 viewport, so
// this is a full-image oracle rather than a non-zero smoke check.
constexpr std::array<std::uint8_t, 4> kFirstDrawPixelBytes{
    0x56u, 0x34u, 0x12u, 0xffu};
constexpr std::array<std::uint8_t, 4> kClearPixelBytes{
    0xc3u, 0xb2u, 0xa1u, 0xffu};
constexpr std::array<std::uint8_t, 4> kFinalDrawPixelBytes{
    0xefu, 0xcdu, 0xabu, 0xffu};

struct TriangleVertex {
  float x;
  float y;
  float z;
  float rhw;
  std::uint32_t color;
};

static_assert(sizeof(TriangleVertex) == 20u);

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

struct WireFixture {
  std::vector<std::byte> bytes;
  dxmt9::d3d9::CommandChunkEnvelope envelope{};
};

void retainOracleObject(std::uint32_t kind, void* object) noexcept {
  if (kind == D9C_CHUNK_HANDLE_KIND_BUFFER && object) {
    dxmt9c_buffer_addref(static_cast<D9CBuffer*>(object));
  } else if (kind == D9C_CHUNK_HANDLE_KIND_SURFACE && object) {
    dxmt9c_surface_addref(static_cast<D9CSurface*>(object));
  }
}

WireFixture makeTriangleDrawChunk(
    const D9CWireObjectIdentity& bufferIdentity,
    const D9CWireObjectIdentity& surfaceIdentity,
    bool appendPresent = false) {
  D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4u, // D3DPT_TRIANGLELIST.
      .primitiveCount = 1u,
      .sectionCount = 4u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
  };
  const auto tableEnd = sizeof(draw) +
                        4u * sizeof(D9CCommandChunkWireSectionDesc);
  const auto streamOffset = alignUp(
      tableEnd, alignof(D9CCommandChunkWireStreamBinding));
  const auto inputOffset = alignUp(
      streamOffset + sizeof(D9CCommandChunkWireStreamBinding),
      alignof(D9CCommandChunkWireVertexInput));
  const auto targetOffset = alignUp(
      inputOffset + sizeof(D9CCommandChunkWireVertexInput),
      alignof(D9CCommandChunkWireRenderTargetBinding));
  const auto viewportOffset = alignUp(
      targetOffset + sizeof(D9CCommandChunkWireRenderTargetBinding),
      alignof(D9CViewport));
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(streamOffset);
  const std::array sections{
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
          .elementSize = sizeof(D9CCommandChunkWireStreamBinding),
          .count = 1u,
          .payloadOffset = static_cast<std::uint32_t>(streamOffset),
          .byteSize = sizeof(D9CCommandChunkWireStreamBinding),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_VERTEX_INPUT,
          .elementSize = sizeof(D9CCommandChunkWireVertexInput),
          .count = 1u,
          .payloadOffset = static_cast<std::uint32_t>(inputOffset),
          .byteSize = sizeof(D9CCommandChunkWireVertexInput),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_RENDER_TARGET,
          .elementSize = sizeof(D9CCommandChunkWireRenderTargetBinding),
          .count = 1u,
          .payloadOffset = static_cast<std::uint32_t>(targetOffset),
          .byteSize = sizeof(D9CCommandChunkWireRenderTargetBinding),
      },
      D9CCommandChunkWireSectionDesc{
          .kind = D9C_COMMAND_CHUNK_SECTION_VIEWPORT,
          .elementSize = sizeof(D9CViewport),
          .count = 1u,
          .payloadOffset = static_cast<std::uint32_t>(viewportOffset),
          .byteSize = sizeof(D9CViewport),
      },
  };
  const D9CCommandChunkWireVertexInput input{
      .valid = 1u,
      .kind = D9C_COMMAND_CHUNK_VERTEX_INPUT_FVF,
      .value = kTriangleFvf,
      .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX,
  };
  const D9CCommandChunkWireStreamBinding stream{
      .slot = 0u,
      .valid = 1u,
      .handleIndex = 0u,
      .offset = 0u,
      .stride = sizeof(TriangleVertex),
      .frequency = 1u,
  };
  const D9CCommandChunkWireRenderTargetBinding target{
      .slot = 0u,
      .valid = 1u,
      .handleIndex = 1u,
  };
  const D9CViewport viewport{
      .x = 0u, .y = 0u, .width = kWidth, .height = kHeight,
      .minZ = 0.0f, .maxZ = 1.0f};
  std::vector<std::byte> payload(viewportOffset + sizeof(viewport));
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + draw.sectionTableOffset, sections.data(),
              sizeof(sections));
  std::memcpy(payload.data() + streamOffset, &stream,
              sizeof(stream));
  std::memcpy(payload.data() + inputOffset, &input, sizeof(input));
  std::memcpy(payload.data() + targetOffset, &target, sizeof(target));
  std::memcpy(payload.data() + viewportOffset, &viewport, sizeof(viewport));
  std::vector<D9CCommandChunkWireRecordHeader> records{
      D9CCommandChunkWireRecordHeader{
          .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
          .flags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
          .payloadOffset = 0u,
          .payloadSize = static_cast<std::uint32_t>(payload.size()),
          .firstHandle = 0u,
          .handleCount = 2u,
      }};
  if (appendPresent) {
    const auto presentOffset = alignUp(
        payload.size(), alignof(D9CCommandChunkWirePresent));
    payload.resize(presentOffset + sizeof(D9CCommandChunkWirePresent));
    const D9CCommandChunkWirePresent present{};
    std::memcpy(payload.data() + presentOffset, &present, sizeof(present));
    records.push_back({
        .type = D9C_COMMAND_RECORD_PRESENT,
        .flags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
        .payloadOffset = static_cast<std::uint32_t>(presentOffset),
        .payloadSize = sizeof(present),
        .firstHandle = 2u,
    });
  }
  const std::array handles{
      dxmt9::d3d9::wireHandleEntry(bufferIdentity),
      dxmt9::d3d9::wireHandleEntry(surfaceIdentity),
  };
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

  WireFixture result;
  result.bytes.resize(header.payloadArenaOffset + payload.size());
  std::memcpy(result.bytes.data(), &header, sizeof(header));
  std::memcpy(result.bytes.data() + header.recordTableOffset, records.data(),
              records.size() * sizeof(records[0]));
  std::memcpy(result.bytes.data() + header.handleTableOffset, handles.data(),
              sizeof(handles));
  std::memcpy(result.bytes.data() + header.payloadArenaOffset, payload.data(),
              payload.size());
  result.envelope = {
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
  };
  return result;
}

dxmt9::d3d9::RawCommandChunk makeRaw(const WireFixture& fixture,
                                     dxmt9::d3d9::WireObjectRegistry& registry,
                                     D9CBuffer& buffer, D9CSurface& surface,
                                     std::uint64_t rawOrdinal = 1u) {
  dxmt9::d3d9::ImportedChunkView imported;
  const auto validation = dxmt9::d3d9::validateCommandChunk(
      fixture.bytes, fixture.envelope, &imported);
  check(validation.valid(),
        "production Draw raw chunk validates before owned preflight: status=" +
            std::to_string(static_cast<unsigned>(validation.status)) +
            " record=" + std::to_string(validation.failedRecordIndex));
  dxmt9::d3d9::RawCommandChunk raw;
  check(dxmt9::d3d9::prepareOffloadChunk(
            fixture.bytes, fixture.envelope, registry, retainOracleObject, raw),
        "production Draw raw chunk passes owned preflight");
  raw.replaySeq = rawOrdinal;
  raw.producerIdentity = {
      .firstEventOrdinal = rawOrdinal,
      .lastEventOrdinal = rawOrdinal,
      .firstSourceOrdinal = rawOrdinal,
      .lastSourceOrdinal = rawOrdinal,
  };
  raw.cpuReadyTapePlanningEnabled = false;
  raw.resourceEntries.push_back({
      .kind = dxmt9::core::ChunkHandleKind::Buffer,
      .handle = buffer.obj->handle(),
  });
  raw.resourceEntries.push_back({
      .kind = dxmt9::core::ChunkHandleKind::Surface,
      .handle = surface.obj->handle(),
  });
  return raw;
}

struct ProductionFixture {
  explicit ProductionFixture(WMT::Device metalDevice) {
    auto upper = dxmt9::CreateDXMT9Device(
        dxmt9::DEVICE_DESC{.device = metalDevice});
    check(upper != nullptr, "production Metal device constructs");
    upperRaw = upper.get();
    factory = dxmt9::com::Direct3DCreate9Ex(
        dxmt9::com::D3D_SDK_VERSION, std::move(upper));
    check(factory != nullptr, "production core factory constructs");

    dxmt9::core::PresentParameters params{};
    params.backBufferWidth = kWidth;
    params.backBufferHeight = kHeight;
    params.backBufferFormat = dxmt9::core::Format::A8R8G8B8;
    params.windowed = true;
    params.presentationInterval = dxmt9::core::PresentInterval::Immediate;
    device = factory->CreateDeviceEx(0u, params, nullptr);
    check(device != nullptr, "production core device constructs");
    device->AddRef();
    cDevice = std::make_unique<D9CDevice>(device);
    surface = dxmt9c_device_create_render_target(
        cDevice.get(), kWidth, kHeight, 21u, 0u, 0u, 0u, nullptr);
    check(surface != nullptr && surface->obj,
          "production offscreen render target constructs");
    buffer = dxmt9c_device_create_vertex_buffer(
        cDevice.get(), 3u * sizeof(TriangleVertex), 0u, kTriangleFvf, 0u);
    check(buffer != nullptr && buffer->obj,
          "production triangle vertex buffer constructs");
    setVertices(0xff123456u);
    const D9CViewport viewport{
        .x = 0u, .y = 0u, .width = kWidth, .height = kHeight,
        .minZ = 0.0f, .maxZ = 1.0f};
    check(dxmt9c_device_set_viewport(cDevice.get(), &viewport) ==
              dxmt9::core::D3D_OK &&
              dxmt9c_device_set_fvf(cDevice.get(), kTriangleFvf) ==
              dxmt9::core::D3D_OK &&
              dxmt9c_device_set_render_target(cDevice.get(), 0u, surface) ==
              dxmt9::core::D3D_OK,
          "production triangle state binds viewport, FVF, and target");
    check(dxmt9c_device_clear(cDevice.get(), 0u, nullptr, 1u, 0u, 1.0f,
                              0u) == dxmt9::core::D3D_OK,
          "production triangle target clears to a deterministic black seed");
    upperRaw->flush();
  }

  void setVertices(std::uint32_t color) {
    void* vertexBytes = nullptr;
    check(dxmt9c_buffer_lock(buffer, 0u, 3u * sizeof(TriangleVertex),
                             &vertexBytes, 0u) == dxmt9::core::D3D_OK &&
              vertexBytes != nullptr,
          "production triangle vertex buffer lock succeeds");
    const std::array vertices{
        // This right triangle covers every 16x16 pixel center. Keeping the
        // diffuse value constant makes the readback an exact fixture oracle,
        // rather than comparing interpolation at a partially covered edge.
        TriangleVertex{0.0f, 0.0f, 0.5f, 1.0f, color},
        TriangleVertex{32.0f, 0.0f, 0.5f, 1.0f, color},
        TriangleVertex{0.0f, 32.0f, 0.5f, 1.0f, color},
    };
    std::memcpy(vertexBytes, vertices.data(), sizeof(vertices));
    check(dxmt9c_buffer_unlock(buffer) == dxmt9::core::D3D_OK,
          "production triangle vertex buffer unlock succeeds");
  }

  ~ProductionFixture() {
    if (buffer) {
      dxmt9c_buffer_release(buffer);
      buffer = nullptr;
    }
    if (surface) {
      dxmt9c_surface_release(surface);
      surface = nullptr;
    }
    cDevice.reset();
    if (device) {
      (void)device->Release();
    }
    if (factory) {
      (void)factory->Release();
    }
  }

  dxmt9::Device* upperRaw = nullptr;
  dxmt9::com::IDirect3D9Ex* factory = nullptr;
  dxmt9::com::IDirect3DDevice9Ex* device = nullptr;
  std::unique_ptr<D9CDevice> cDevice;
  D9CBuffer* buffer = nullptr;
  D9CSurface* surface = nullptr;
};

bool directSelection() {
  return dxmt9::resolveDirectChunkSlotReplayEnabled(
      std::getenv("DXMT9_DIRECT_CHUNK_SLOT_REPLAY"),
      /*traceRender=*/false);
}

void directCommitProbe(WMT::Device metalDevice) {
  ProductionFixture fixture(metalDevice);
  check(fixture.upperRaw->supportsDirectChunkSlotReplay(),
        "env=1 production device advertises Direct ChunkSlot replay");
  dxmt9::d3d9::WireObjectRegistry registry;
  const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER,
                                        fixture.buffer);
  const auto surfaceIdentity = registry.insert(
      D9C_CHUNK_HANDLE_KIND_SURFACE, fixture.surface);
  const auto wire = makeTriangleDrawChunk(identity, surfaceIdentity);
  auto raw = makeRaw(wire, registry, *fixture.buffer, *fixture.surface);
  check(dxmt9::CommandQueueArenaLeaseTestAccess::beginDirectLifecycleObservation(
            fixture.upperRaw->queue()),
        "production lifecycle observer is bound for commit-failure negative");
  dxmt9::CommandQueueArenaLeaseTestAccess::forceNextDirectChunkSlotCommitFailure(
      fixture.upperRaw->queue());
  const auto hr = dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), raw);
  check(hr != dxmt9::core::D3D_OK &&
            dxmt9::CommandQueueArenaLeaseTestAccess::stopped(
                fixture.upperRaw->queue()),
        "forced commit seam proves the production Direct route reached its commit");
  check(dxmt9::CommandQueueArenaLeaseTestAccess::
            directLifecycleMissingTerminalMatchFailsClosed(
                fixture.upperRaw->queue()),
        "production Direct replay-receipt adapter fails closed on a missing admitted match");
  dxmt9::d3d9::releaseRetainedWrappers(raw);
}

struct ChildResult {
  bool direct = false;
  bool directCommitProved = false;
  std::uint64_t commandCount = 0;
  std::uint64_t resourceHandle = 0;
  std::uint64_t resourceSeq = 0;
  std::uint64_t committedSeq = 0;
  std::uint64_t completedSeq = 0;
  std::vector<std::byte> pixels;
};

std::uint64_t pixelDigest(std::span<const std::byte> pixels) noexcept {
  // FNV-1a is deliberately tiny and stable across child processes; this is
  // diagnostics only, while the oracle below still requires exact bytes.
  std::uint64_t digest = 1469598103934665603ull;
  for (const auto byte : pixels) {
    digest ^= static_cast<std::uint8_t>(byte);
    digest *= 1099511628211ull;
  }
  return digest;
}

std::vector<std::byte> readbackPixels(
    ProductionFixture& fixture,
    const std::array<std::uint8_t, 4>& expected,
    std::string_view label) {
  dxmt9::core::ReadbackPixels readback;
  check(fixture.upperRaw->readbackSurface(
            dxmt9::core::ReadbackDesc{
                .source = fixture.surface->obj->handle()},
            readback),
        std::string(label) + " Metal offscreen readback succeeds");
  const std::size_t expectedBytes =
      static_cast<std::size_t>(kWidth) * kHeight * 4u;
  check(readback.pitch >= kWidth * 4u &&
            readback.bytes.size() >=
                static_cast<std::size_t>(readback.pitch) * kHeight,
        std::string(label) + " readback has a bounded row span");
  std::vector<std::byte> pixels(expectedBytes);
  for (std::uint32_t row = 0u; row < kHeight; ++row) {
    std::memcpy(pixels.data() + static_cast<std::size_t>(row) * kWidth * 4u,
                readback.bytes.data() +
                    static_cast<std::size_t>(row) * readback.pitch,
                kWidth * 4u);
  }
  for (std::size_t pixel = 0u; pixel < kWidth * kHeight; ++pixel) {
    const auto offset = pixel * expected.size();
    for (std::size_t component = 0u; component < expected.size(); ++component) {
      check(static_cast<std::uint8_t>(pixels[offset + component]) ==
                expected[component],
            std::string(label) + " every pixel matches its exact BGRA value");
    }
  }
  return pixels;
}

void printPixelQuad(std::ostream& out, std::span<const std::byte> pixels,
                    std::size_t offset) {
  out << '[';
  for (std::size_t i = 0u; i < 4u; ++i) {
    if (i != 0u) out << ',';
    out << static_cast<unsigned>(static_cast<std::uint8_t>(pixels[offset + i]));
  }
  out << ']';
}

ChildResult runChild(bool expectedDirect) {
  setenv("DXMT9_DIRECT_CHUNK_SLOT_REPLAY", expectedDirect ? "1" : "0", 1);
  setenv("DXMT9_PERF_PIPELINE_LIFECYCLE_OBSERVER", "1", 1);
  unsetenv("DXMT_TRACE_RENDER");
  check(directSelection() == expectedDirect,
        "production resolver selects the requested explicit env mode");

  @autoreleasepool {
    auto devices = WMT::CopyAllDevices();
    if (!devices || devices.count() == 0u) {
      std::cout << "SKIP\n";
      std::exit(77);
    }
    WMT::Device metalDevice = devices.object(0u);
    if (expectedDirect) {
      directCommitProbe(metalDevice);
    }

    ProductionFixture fixture(metalDevice);
    check(fixture.upperRaw->supportsDirectChunkSlotReplay() == expectedDirect,
          "real production device capability matches selected env mode");
    if (expectedDirect) {
      check(dxmt9::CommandQueueArenaLeaseTestAccess::
                beginDirectLifecycleObservation(fixture.upperRaw->queue()),
            "production lifecycle observer is bound before queue startup");
    }
    dxmt9::d3d9::WireObjectRegistry registry;
    const auto identity = registry.insert(D9C_CHUNK_HANDLE_KIND_BUFFER,
                                          fixture.buffer);
    const auto surfaceIdentity = registry.insert(
        D9C_CHUNK_HANDLE_KIND_SURFACE, fixture.surface);
    const auto wire = makeTriangleDrawChunk(identity, surfaceIdentity);
    auto firstRaw = makeRaw(wire, registry, *fixture.buffer, *fixture.surface,
                            1u);
    check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), firstRaw) ==
              dxmt9::core::D3D_OK,
          "first production Draw replay succeeds");
    fixture.upperRaw->flush();
    (void)readbackPixels(fixture, kFirstDrawPixelBytes, "first Draw cut");

    check(dxmt9c_device_clear(fixture.cDevice.get(), 0u, nullptr, 1u,
                              0xffa1b2c3u, 1.0f, 0u) ==
              dxmt9::core::D3D_OK,
          "middle noncommutative Clear succeeds");
    fixture.upperRaw->flush();
    (void)readbackPixels(fixture, kClearPixelBytes, "middle Clear cut");

    fixture.setVertices(0xffabcdefu);
    const auto finalWire = makeTriangleDrawChunk(
        identity, surfaceIdentity, /*appendPresent=*/true);
    auto finalRaw = makeRaw(finalWire, registry, *fixture.buffer,
                            *fixture.surface, 2u);
    check(dxmt9::d3d9::replayRawChunk(fixture.cDevice.get(), finalRaw) ==
              dxmt9::core::D3D_OK,
          "final production Draw replay succeeds after rotation/reuse cut");
    const auto& queue = fixture.upperRaw->queue();
    const auto commandCount =
        dxmt9::CommandQueueArenaLeaseTestAccess::writingCommandCount(
            const_cast<dxmt9::CommandQueue&>(queue));
    // The trailing Present publishes the slot immediately, so the encoder
    // may consume it before this sample. Compare the normalized count across
    // child modes below; exact Draw+Present ordering is pinned by the wire
    // fixture and the committed/completed sequence.
    fixture.upperRaw->flush();
    if (expectedDirect) {
      check(dxmt9::CommandQueueArenaLeaseTestAccess::waitForDirectLifecycleReclaim(
                const_cast<dxmt9::CommandQueue&>(queue)),
            "production Direct lifecycle drains through finishReclaim");
      check(dxmt9::CommandQueueArenaLeaseTestAccess::directLifecycleComplete(
                const_cast<dxmt9::CommandQueue&>(queue), true),
            "production observer reduces both Direct sources through Present and encoded completion");
    }
    ChildResult result;
    result.direct = expectedDirect;
    result.directCommitProved = expectedDirect;
    result.commandCount = commandCount;
    result.resourceHandle = fixture.surface->obj->handle().value;
    const auto* surfaceRecord = fixture.upperRaw->pool()->findSurface(
        fixture.surface->obj->handle().value);
    check(surfaceRecord != nullptr, "production surface resource remains present");
    result.resourceSeq = surfaceRecord->lastUsedSeqId;
    result.committedSeq =
        dxmt9::CommandQueueArenaLeaseTestAccess::lastCommittedSeqId(
            const_cast<dxmt9::CommandQueue&>(queue));
    result.completedSeq =
        dxmt9::CommandQueueArenaLeaseTestAccess::completedSeqId(
            const_cast<dxmt9::CommandQueue&>(queue));
    result.pixels = readbackPixels(
        fixture, kFinalDrawPixelBytes, "final Draw/Present cut");
    check(result.pixels.size() ==
              static_cast<std::size_t>(kWidth) * kHeight * 4u,
          "Metal readback contains exactly one 16x16 RGBA image");
    dxmt9::d3d9::releaseRetainedWrappers(finalRaw);
    dxmt9::d3d9::releaseRetainedWrappers(firstRaw);
    return result;
  }
}

std::string serialize(const ChildResult& result) {
  std::ostringstream out;
  out << (result.direct ? 1 : 0) << ' ' << (result.directCommitProved ? 1 : 0)
      << ' ' << result.commandCount << ' ' << result.resourceHandle << ' '
      << result.resourceSeq << ' ' << result.committedSeq << ' '
      << result.completedSeq << ' ';
  for (const auto byte : result.pixels) {
    out << static_cast<unsigned>(static_cast<std::uint8_t>(byte)) << ',';
  }
  return out.str();
}

ChildResult parseResult(std::string_view text) {
  check(text.rfind("SKIP\n", 0) != 0u,
        "child should not be parsed as a Metal skip result");
  std::istringstream in{std::string(text)};
  ChildResult result;
  int direct = 0;
  int proved = 0;
  check(static_cast<bool>(in >> direct >> proved >> result.commandCount >>
                          result.resourceHandle >> result.resourceSeq >>
                          result.committedSeq >> result.completedSeq),
        "child emits command/resource/completion identity");
  result.direct = direct != 0;
  result.directCommitProved = proved != 0;
  std::string pixelText;
  in >> pixelText;
  std::size_t begin = 0u;
  while (begin < pixelText.size()) {
    const auto end = pixelText.find(',', begin);
    check(end != std::string::npos, "child pixel serialization is bounded");
    result.pixels.push_back(static_cast<std::byte>(std::stoul(
        pixelText.substr(begin, end - begin))));
    begin = end + 1u;
  }
  return result;
}

std::string runMode(const char* self, const char* mode) {
  int pipefd[2] = {-1, -1};
  check(pipe(pipefd) == 0, "create child result pipe");
  const pid_t child = fork();
  check(child >= 0, "fork child mode");
  if (child == 0) {
    close(pipefd[0]);
    check(dup2(pipefd[1], STDOUT_FILENO) >= 0,
          "redirect child result stdout");
    close(pipefd[1]);
    execl(self, self, "--child", mode, static_cast<char*>(nullptr));
    _exit(127);
  }
  close(pipefd[1]);
  std::string output;
  std::array<char, 4096> buffer{};
  for (;;) {
    const ssize_t count = read(pipefd[0], buffer.data(), buffer.size());
    if (count > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    check(count == 0 || errno == EINTR, "read child result");
    if (count == 0) break;
  }
  close(pipefd[0]);
  int status = 0;
  check(waitpid(child, &status, 0) == child, "wait for child mode");
  check(WIFEXITED(status), "child mode exits normally");
  const int exitCode = WEXITSTATUS(status);
  if (exitCode == 77) {
    return "SKIP\n";
  }
  check(exitCode == 0, "child mode succeeds");
  return output;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--child") {
      const bool direct = std::string_view(argv[2]) == "1";
      std::cout << serialize(runChild(direct)) << '\n';
      return 0;
    }
    check(argc >= 1, "test executable has a path");
    check(dxmt9::resolveDirectChunkSlotReplayEnabled("0", false) == false &&
              dxmt9::resolveDirectChunkSlotReplayEnabled("1", false) == true,
          "explicit Direct ChunkSlot resolver values remain distinct");
    const auto legacyText = runMode(argv[0], "0");
    if (legacyText == "SKIP\n") return 77;
    const auto directText = runMode(argv[0], "1");
    if (directText == "SKIP\n") return 77;
    const auto legacy = parseResult(legacyText);
    const auto direct = parseResult(directText);
    check(!legacy.direct && !legacy.directCommitProved,
          "env=0 child remains on the Legacy route");
    check(direct.direct && direct.directCommitProved,
          "env=1 child proves the ordinary Direct route before readback");
    // Resource handles are process-local opaque ordinals: each lane runs in a
    // fresh child, so compare their normalized presence and queue sequence,
    // not the raw handle value. The source wire identity is otherwise exactly
    // the same one-buffer Draw fixture in both children.
    check(legacy.commandCount == direct.commandCount &&
              legacy.resourceHandle != 0u && direct.resourceHandle != 0u &&
              legacy.resourceSeq == direct.resourceSeq &&
              legacy.committedSeq == direct.committedSeq &&
              legacy.completedSeq == direct.completedSeq,
          "Direct/Legacy preserve command, normalized resource, and completion identity");
    if (legacy.pixels != direct.pixels) {
      std::size_t firstDifference = 0u;
      while (firstDifference < legacy.pixels.size() &&
             firstDifference < direct.pixels.size() &&
             legacy.pixels[firstDifference] == direct.pixels[firstDifference]) {
        ++firstDifference;
      }
      std::cerr << "Draw oracle first pixel difference="
                << firstDifference / 4u << " byte=" << firstDifference % 4u
                << " legacy=";
      if (firstDifference + 4u <= legacy.pixels.size()) {
        printPixelQuad(std::cerr, legacy.pixels,
                       firstDifference & ~std::size_t{3u});
      }
      std::cerr << " direct=";
      if (firstDifference + 4u <= direct.pixels.size()) {
        printPixelQuad(std::cerr, direct.pixels,
                       firstDifference & ~std::size_t{3u});
      }
      std::cerr << " legacyDigest=0x" << std::hex
                << pixelDigest(legacy.pixels) << " directDigest=0x"
                << pixelDigest(direct.pixels) << std::dec << '\n';
    }
    check(legacy.pixels == direct.pixels,
          "Direct/Legacy exact offscreen Metal readback pixels match");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
