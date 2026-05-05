#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "device_c_common.hpp"
#include "dxmt9/com.hpp"
#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"
#include "dxmt9/dxmt9_device.hpp"
#include "../src/dxmt9/dxmt9_resource_pool.hpp"

namespace {

using namespace dxmt9::core;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

enum class EventKind {
  MarkChunkResources,
  SetSkipDrawResourceMarking,
  SubmitDraw,
  SubmitClear,
  SubmitReadback,
  SubmitSurfaceCopy,
  Flush,
};

struct RecordedDrawRun {
  CanonicalDrawState state{};
  FlatDrawStateRecord hot{};
  std::vector<DrawParam> draws;
  std::vector<u8> payloadArena;
};

struct RecordedEvent {
  EventKind kind = EventKind::Flush;
  bool skipDrawResourceMarking = false;
  std::vector<ChunkHandleEntry> chunkHandles;
  RecordedDrawRun drawRun;
  ClearDesc clear;
  ReadbackDesc readback;
  SurfaceCopyDesc surfaceCopy;
};

struct RecordingDxmt9Device final : dxmt9::Device {
  RecordingDxmt9Device()
      : limits_{}, queue_(WMT::Device{NULL_OBJECT_HANDLE}, limits_) {}

  WMT::Device wmtDevice() override { return WMT::Device{NULL_OBJECT_HANDLE}; }
  dxmt9::CommandQueue& queue() override { return queue_; }
  const BackendLimits& limits() const override { return limits_; }
  std::shared_ptr<BackendDevice> backend() override { return {}; }

  void setDeviceLostObserver(BackendDevice::DeviceLostObserver observer) override {
    deviceLostObserver = std::move(observer);
  }

  void setPresentationStatusObserver(
      BackendDevice::PresentationStatusObserver observer) override {
    presentationStatusObserver = std::move(observer);
  }

  BufferHandle createBuffer(const BufferDesc&) override {
    return BufferHandle{nextHandle++};
  }

  TextureHandle createTexture(const TextureDesc&) override {
    return TextureHandle{nextHandle++};
  }

  SurfaceHandle createSurface(const SurfaceDesc&) override {
    return SurfaceHandle{nextHandle++};
  }

  SurfaceHandle createSurfaceForTexture(TextureHandle, std::uint32_t,
                                        const SurfaceDesc&) override {
    return SurfaceHandle{nextHandle++};
  }

  void markChunkResources(std::span<const ChunkHandleEntry> entries) override {
    RecordedEvent event;
    event.kind = EventKind::MarkChunkResources;
    event.chunkHandles.assign(entries.begin(), entries.end());
    events.push_back(std::move(event));
  }

  void setSkipDrawResourceMarking(bool skip) override {
    RecordedEvent event;
    event.kind = EventKind::SetSkipDrawResourceMarking;
    event.skipDrawResourceMarking = skip;
    events.push_back(std::move(event));
  }

  void submitDrawRun(CanonicalDrawState state, const DrawUniformPayload&,
                     std::span<const DrawParam> draws,
                     std::span<const DrawParamPayloadView> payloads) override {
    RecordedEvent event;
    event.kind = EventKind::SubmitDraw;
    event.drawRun.state = std::move(state);
    event.drawRun.hot = event.drawRun.state.hot;
    event.drawRun.draws.reserve(draws.size());
    auto appendPayload = [&](std::span<const u8> bytes) -> DrawPayloadRange {
      if (bytes.empty()) {
        return {};
      }
      const auto offset = static_cast<u32>(event.drawRun.payloadArena.size());
      event.drawRun.payloadArena.insert(event.drawRun.payloadArena.end(),
                                        bytes.begin(), bytes.end());
      return DrawPayloadRange{
          .offset = offset,
          .size = static_cast<u32>(bytes.size()),
      };
    };
    for (std::size_t i = 0; i < draws.size(); ++i) {
      DrawParam param = draws[i];
      const DrawParamPayloadView payload = i < payloads.size() ? payloads[i] : DrawParamPayloadView{};
      param.userVertexRange = appendPayload(payload.userVertexData);
      param.userIndexRange = appendPayload(payload.userIndexData);
      event.drawRun.draws.push_back(param);
    }
    events.push_back(std::move(event));
  }

  void submitClear(const ClearDesc& desc) override {
    RecordedEvent event;
    event.kind = EventKind::SubmitClear;
    event.clear = desc;
    events.push_back(std::move(event));
  }

  void submitReadback(const ReadbackDesc& desc) override {
    RecordedEvent event;
    event.kind = EventKind::SubmitReadback;
    event.readback = desc;
    events.push_back(std::move(event));
  }

  void submitSurfaceCopy(const SurfaceCopyDesc& desc) override {
    RecordedEvent event;
    event.kind = EventKind::SubmitSurfaceCopy;
    event.surfaceCopy = desc;
    events.push_back(std::move(event));
  }

  void flush() override {
    RecordedEvent event;
    event.kind = EventKind::Flush;
    events.push_back(std::move(event));
  }

  bool readbackSurface(const ReadbackDesc&, ReadbackPixels&) override {
    return false;
  }

  BackendLimits limits_{};
  dxmt9::CommandQueue queue_;
  std::uint64_t nextHandle = 1;
  std::vector<RecordedEvent> events;
  BackendDevice::DeviceLostObserver deviceLostObserver;
  BackendDevice::PresentationStatusObserver presentationStatusObserver;
};

D9CWireHandle wireHandleFromValue(std::uint64_t value) {
  D9CWireHandle handle{};
  handle.lo = static_cast<std::uint32_t>(value);
  handle.hi = static_cast<std::uint32_t>(value >> 32);
  return handle;
}

std::uint64_t wireValueFromPtr(const void* ptr) {
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ptr));
}

D9CWireHandle wireHandleFromPtr(const void* ptr) {
  return wireHandleFromValue(wireValueFromPtr(ptr));
}

template <typename T>
void appendRecord(std::vector<std::uint8_t>& bytes, const T& record) {
  const auto* begin = reinterpret_cast<const std::uint8_t*>(&record);
  bytes.insert(bytes.end(), begin, begin + sizeof(T));
}

template <typename T>
void appendPod(std::vector<std::uint8_t>& bytes, const T& value) {
  const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
  bytes.insert(bytes.end(), begin, begin + sizeof(T));
}

D9CCommandChunkWireHandleEntry wireHandleEntry(std::uint32_t kind,
                                               const void* ptr) {
  return D9CCommandChunkWireHandleEntry{
      .kind = kind,
      .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
      .opaqueHandle = wireValueFromPtr(ptr),
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
}

D9CCommandChunkWireRecordHeader wireRecordHeader(std::uint32_t type,
                                                 std::uint32_t payloadOffset,
                                                 std::uint32_t payloadSize,
                                                 std::uint32_t firstHandle,
                                                 std::uint32_t handleCount) {
  return D9CCommandChunkWireRecordHeader{
      .type = type,
      .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
      .payloadOffset = payloadOffset,
      .payloadSize = payloadSize,
      .firstHandle = firstHandle,
      .handleCount = handleCount,
      .reserved0 = 0u,
      .reserved1 = 0u,
  };
}

std::vector<std::uint8_t> makeWireChunkBlob(
    std::span<const std::uint8_t> payload,
    std::span<const D9CCommandChunkWireRecordHeader> records,
    std::span<const D9CCommandChunkWireHandleEntry> handles) {
  D9CCommandChunkWireHeader header{};
  header.version = D9C_COMMAND_CHUNK_WIRE_VERSION;
  header.headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  header.recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
  header.handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
  header.recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  header.recordCount = static_cast<std::uint32_t>(records.size());
  header.handleTableOffset = header.recordTableOffset +
      header.recordCount * D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
  header.handleCount = static_cast<std::uint32_t>(handles.size());
  header.payloadArenaOffset = header.handleTableOffset +
      header.handleCount * D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
  header.payloadArenaSize = static_cast<std::uint32_t>(payload.size());

  std::vector<std::uint8_t> blob;
  blob.reserve(header.payloadArenaOffset + header.payloadArenaSize);
  appendPod(blob, header);
  for (const auto& record : records) appendPod(blob, record);
  for (const auto& handle : handles) appendPod(blob, handle);
  blob.insert(blob.end(), payload.begin(), payload.end());
  return blob;
}

D9CCommandRecordDrawPrimitive makeDrawRecord(D9CSurface* renderTarget,
                                             D9CBuffer* vertexBuffer,
                                             std::uint32_t startVertex) {
  D9CCommandRecordDrawPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  draw.header.size = sizeof(draw);
  draw.packet.primitiveType = 4u;
  draw.packet.primitiveCount = 1u;
  draw.packet.startVertex = startVertex;
  draw.packet.rtMask = 0x1u;
  draw.packet.rtHandles[0] = wireHandleFromPtr(renderTarget);
  draw.packet.streamSourceMask = 0x1u;
  draw.packet.streamSources[0].buffer = wireHandleFromPtr(vertexBuffer);
  draw.packet.streamSources[0].offset = 0u;
  draw.packet.streamSources[0].stride = 16u;
  return draw;
}

D9CCommandRecordClear makeColorClearRecord() {
  D9CCommandRecordClear clear{};
  clear.header.type = D9C_COMMAND_RECORD_CLEAR;
  clear.header.size = sizeof(clear);
  clear.rectOffset = sizeof(D9CCommandRecordClear);
  clear.flags = 1u;
  clear.colorARGB = 0xff102030u;
  clear.z = 1.0f;
  return clear;
}

D9CCommandRecordReadback makeReadbackRecord(D9CSurface* source,
                                            D9CSurface* destination) {
  D9CCommandRecordReadback readback{};
  readback.header.type = D9C_COMMAND_RECORD_READBACK;
  readback.header.size = sizeof(readback);
  readback.srcWire = wireValueFromPtr(source);
  readback.dstWire = wireValueFromPtr(destination);
  return readback;
}

bool containsChunkHandle(const std::vector<ChunkHandleEntry>& entries,
                         ChunkHandleKind kind,
                         Handle handle) {
  return std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
           return entry.kind == kind && entry.handle == handle;
         }) != entries.end();
}

void checkEventKind(const std::vector<RecordedEvent>& events,
                    std::size_t index,
                    EventKind expected,
                    std::string_view message) {
  check(index < events.size(), message);
  check(events[index].kind == expected, message);
}

void testImportedChunkBulkRetentionAndBarrierOrdering() {
  auto upper = std::make_unique<RecordingDxmt9Device>();
  auto* recorder = upper.get();
  auto* d3d = dxmt9::com::Direct3DCreate9Ex(dxmt9::com::D3D_SDK_VERSION,
                                            std::move(upper));
  check(d3d != nullptr, "create recording d3d factory");

  PresentParameters params{};
  params.backBufferWidth = 16u;
  params.backBufferHeight = 16u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{77};
  params.presentationInterval = PresentInterval::Immediate;

  auto* device = d3d->CreateDeviceEx(0u, params, nullptr);
  check(device != nullptr, "create recording d3d device");
  device->AddRef();

  {
    D9CDevice cDevice(device);

    auto renderTarget = device->CreateSurface(SurfaceDesc{
        .width = 16u,
        .height = 16u,
        .format = Format::A8R8G8B8,
        .pool = Pool::Default,
        .usage = UsageRenderTarget,
        .renderTarget = true,
    });
    auto readbackTarget = device->CreateSurface(SurfaceDesc{
        .width = 16u,
        .height = 16u,
        .format = Format::A8R8G8B8,
        .pool = Pool::Scratch,
    });
    auto vertexBuffer = device->CreateBuffer(BufferDesc{
        .size = 64u,
        .pool = Pool::Default,
        .usage = UsageVertexBuffer,
    });
    check(renderTarget != nullptr, "test render target");
    check(readbackTarget != nullptr, "test readback target");
    check(vertexBuffer != nullptr, "test vertex buffer");

    D9CSurface renderTargetWire(renderTarget);
    D9CSurface readbackTargetWire(readbackTarget);
    D9CBuffer vertexBufferWire(vertexBuffer);

    const auto draw0 = makeDrawRecord(&renderTargetWire, &vertexBufferWire, 0u);
    const auto clear = makeColorClearRecord();
    const auto draw1 = makeDrawRecord(&renderTargetWire, &vertexBufferWire, 3u);
    const auto readback = makeReadbackRecord(&renderTargetWire, &readbackTargetWire);

    std::vector<std::uint8_t> bytes;
    appendRecord(bytes, draw0);
    appendRecord(bytes, clear);
    appendRecord(bytes, draw1);
    appendRecord(bytes, readback);

    const std::vector<D9CCommandChunkWireHandleEntry> handles{
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, &renderTargetWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, &vertexBufferWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, &readbackTargetWire),
    };
    const std::vector<D9CCommandChunkWireRecordHeader> records{
        wireRecordHeader(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, 0u, sizeof(draw0), 0u, 2u),
        wireRecordHeader(D9C_COMMAND_RECORD_CLEAR, sizeof(draw0), sizeof(clear), 0u, 1u),
        wireRecordHeader(D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                         sizeof(draw0) + sizeof(clear), sizeof(draw1), 0u, 2u),
        wireRecordHeader(D9C_COMMAND_RECORD_READBACK,
                         sizeof(draw0) + sizeof(clear) + sizeof(draw1),
                         sizeof(readback), 0u, 3u),
    };
    const auto wireBlob = makeWireChunkBlob(bytes, records, handles);

    D9CCommandChunk chunk{};
    chunk.version = D9C_COMMAND_CHUNK_VERSION;
    chunk.recordCount = static_cast<std::uint32_t>(records.size());
    chunk.recordBytes = static_cast<std::uint32_t>(wireBlob.size());
    chunk.records = wireHandleFromPtr(wireBlob.data());
    chunk.handleCount = static_cast<std::uint32_t>(std::size(handles));
    chunk.handles = {};

    recorder->events.clear();
    checkEq(dxmt9c_device_commit_chunk(&cDevice, &chunk), D3D_OK,
            "commit imported chunk");

    checkEq(recorder->events.size(), static_cast<std::size_t>(9),
            "imported chunk event count");
    checkEventKind(recorder->events, 0u, EventKind::MarkChunkResources,
                   "bulk retention happens first");
    checkEventKind(recorder->events, 1u, EventKind::SetSkipDrawResourceMarking,
                   "per-draw retention skip is enabled before replay");
    check(recorder->events[1].skipDrawResourceMarking,
          "per-draw retention skip enabled");
    checkEventKind(recorder->events, 2u, EventKind::SubmitDraw,
                   "first draw is replayed after retention");
    checkEventKind(recorder->events, 3u, EventKind::SubmitClear,
                   "clear barrier remains ordered after first draw");
    checkEventKind(recorder->events, 4u, EventKind::SubmitDraw,
                   "second draw remains ordered after clear");
    checkEventKind(recorder->events, 5u, EventKind::SubmitReadback,
                   "readback boundary is submitted after second draw");
    checkEventKind(recorder->events, 6u, EventKind::Flush,
                   "readback boundary flushes synchronously");
    checkEventKind(recorder->events, 7u, EventKind::SubmitSurfaceCopy,
                   "readback fallback copy remains after synchronous flush");
    checkEventKind(recorder->events, 8u, EventKind::SetSkipDrawResourceMarking,
                   "per-draw retention skip resets after replay");
    check(!recorder->events[8].skipDrawResourceMarking,
          "per-draw retention skip disabled");

    const auto& retained = recorder->events[0].chunkHandles;
    checkEq(retained.size(), static_cast<std::size_t>(3),
            "bulk retention resolves every imported wrapper handle");
    check(containsChunkHandle(retained, ChunkHandleKind::Surface,
                              renderTarget->handle()),
          "bulk retention includes render target handle");
    check(containsChunkHandle(retained, ChunkHandleKind::Buffer,
                              vertexBuffer->handle()),
          "bulk retention includes vertex buffer handle");
    check(containsChunkHandle(retained, ChunkHandleKind::Surface,
                              readbackTarget->handle()),
          "bulk retention includes readback target handle");

    const auto& firstDrawRun = recorder->events[2].drawRun;
    checkEq(firstDrawRun.draws.size(), static_cast<std::size_t>(1),
            "first draw run contains one imported draw param");
    check(firstDrawRun.hot == firstDrawRun.state.hot,
          "first draw run records canonical and flat hot state together");
    checkEq(firstDrawRun.hot.colorAttachments[0].handle.value,
            renderTarget->handle().value, "first draw observes imported RT state");
    checkEq(firstDrawRun.hot.streamBuffers[0].value, vertexBuffer->handle().value,
            "first draw observes imported stream state");
    checkEq(firstDrawRun.hot.streamOffsets[0], 0u,
            "first draw observes imported stream offset");
    checkEq(firstDrawRun.hot.streamStrides[0], 16u,
            "first draw observes imported stream stride");
    checkEq(firstDrawRun.draws[0].startVertex, 0u,
            "first draw param keeps imported start vertex");
    check(!firstDrawRun.draws[0].indexed,
          "first draw param remains non-indexed");
    check(firstDrawRun.payloadArena.empty(),
          "first draw run has no UP payload arena");

    const auto& secondDrawRun = recorder->events[4].drawRun;
    checkEq(secondDrawRun.draws.size(), static_cast<std::size_t>(1),
            "second draw run contains one imported draw param");
    check(secondDrawRun.hot == secondDrawRun.state.hot,
          "second draw run records canonical and flat hot state together");
    checkEq(secondDrawRun.draws[0].startVertex, 3u,
            "second draw param remains in record order");
    checkEq(secondDrawRun.hot.colorAttachments[0].handle.value,
            renderTarget->handle().value, "second draw observes imported RT state");
    checkEq(secondDrawRun.hot.streamBuffers[0].value, vertexBuffer->handle().value,
            "second draw observes imported stream state");
    checkEq(recorder->events[5].readback.source.value, renderTarget->handle().value,
            "readback source handle");
    checkEq(recorder->events[5].readback.destination.value,
            readbackTarget->handle().value, "readback destination handle");
  }

  checkEq(device->Release(), 0u, "release recording d3d device");
  checkEq(d3d->Release(), 0u, "release recording d3d factory");
}

struct ArenaTestRecord {
  bool destroyPending = false;
  std::uint64_t lastUsedSeqId = 0;
};

void testResourcePoolArenaRejectsStaleHandles() {
  using BufferArena =
      dxmt9::resources::detail::HandleArena<ArenaTestRecord,
                                            dxmt9::resources::detail::ResourceHandleKind::Buffer>;
  using TextureArena =
      dxmt9::resources::detail::HandleArena<ArenaTestRecord,
                                            dxmt9::resources::detail::ResourceHandleKind::Texture>;

  BufferArena buffers;
  TextureArena textures;

  const auto first = buffers.insert(ArenaTestRecord{});
  check(static_cast<bool>(first), "resource arena allocates first buffer handle");
  check(buffers.find(first.value) != nullptr, "resource arena finds live buffer");
  check(textures.find(first.value) == nullptr,
        "resource arena kind tag prevents cross-kind lookup");

  auto* firstRecord = buffers.find(first.value);
  check(firstRecord != nullptr, "resource arena returns first record");
  firstRecord->destroyPending = true;
  buffers.reclaimCompleted(0u, [](const ArenaTestRecord& record) {
    check(record.destroyPending, "resource arena visits pending record before reclaim");
  });
  check(buffers.find(first.value) == nullptr,
        "resource arena rejects reclaimed stale buffer handle");

  const auto second = buffers.insert(ArenaTestRecord{});
  check(static_cast<bool>(second), "resource arena allocates recycled buffer handle");
  check(first.value != second.value,
        "resource arena bumps generation when reusing a handle index");
  check(buffers.find(first.value) == nullptr,
        "resource arena keeps stale generation invalid after reuse");
  check(buffers.find(second.value) != nullptr,
        "resource arena finds current generation buffer");
}

void testResourcePoolUsesArenaStorageOnly() {
  auto* resourcePool = new dxmt9::resources::Pool;

  const auto first = resourcePool->createBuffer(
      WMT::Device{NULL_OBJECT_HANDLE},
      BufferDesc{
          .size = 16u,
          .pool = Pool::SystemMem,
      });
  check(static_cast<bool>(first), "resource pool allocates arena buffer handle");
  check(resourcePool->findBuffer(first.value) != nullptr,
        "resource pool finds arena buffer");
  check(resourcePool->findTexture(first.value) == nullptr,
        "resource pool rejects buffer handle as texture");
  check(resourcePool->findSurface(first.value) == nullptr,
        "resource pool rejects buffer handle as surface");

  resourcePool->markBufferUse(first, 7u);
  check(resourcePool->markBufferDestroyAndGc(first.value, 6u),
        "resource pool marks arena buffer destroy-pending");
  check(resourcePool->findBuffer(first.value) != nullptr,
        "resource pool keeps pending arena buffer until completed seq catches up");

  resourcePool->reclaimCompleted(7u);
  check(resourcePool->findBuffer(first.value) == nullptr,
        "resource pool rejects stale arena buffer after reclaim");

  const auto second = resourcePool->createBuffer(
      WMT::Device{NULL_OBJECT_HANDLE},
      BufferDesc{
          .size = 16u,
          .pool = Pool::SystemMem,
      });
  check(static_cast<bool>(second), "resource pool allocates recycled arena buffer");
  check(first.value != second.value,
        "resource pool bumps generation for recycled buffer slot");
  check(resourcePool->findBuffer(first.value) == nullptr,
        "resource pool stale generation remains invalid after slot reuse");
  check(resourcePool->findBuffer(second.value) != nullptr,
        "resource pool finds current recycled buffer handle");
}

}  // namespace

int main() {
  try {
    testImportedChunkBulkRetentionAndBarrierOrdering();
    testResourcePoolArenaRejectsStaleHandles();
    testResourcePoolUsesArenaStorageOnly();
  } catch (const TestFailure& e) {
    std::cerr << "resource_hazard_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "resource_hazard_spec unexpected exception: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
