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
#include "../../../src/dxmt9/dxmt9_presenter.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"

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

D9CCommandRecordDrawIndexedPrimitive makeIndexedDrawRecord(
    D9CSurface* renderTarget,
    D9CBuffer* vertexBuffer,
    D9CBuffer* indexBuffer) {
  D9CCommandRecordDrawIndexedPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  draw.header.size = sizeof(draw);
  draw.packet.state.primitiveType = 4u;
  draw.packet.state.rtMask = 0x1u;
  draw.packet.state.rtHandles[0] = wireHandleFromPtr(renderTarget);
  draw.packet.state.streamSourceMask = 0x1u;
  draw.packet.state.streamSources[0].buffer = wireHandleFromPtr(vertexBuffer);
  draw.packet.state.streamSources[0].offset = 12u;
  draw.packet.state.streamSources[0].stride = 16u;
  draw.packet.baseVertex = -2;
  draw.packet.startIndex = 5u;
  draw.packet.primitiveCount = 2u;
  draw.packet.ibValid = 1u;
  draw.packet.ibHandle = wireHandleFromPtr(indexBuffer);
  return draw;
}

D9CCommandRecordDrawIndexedPrimitive makeIndexedParamOnlyDrawRecord(
    std::uint32_t primitiveType,
    std::uint32_t primitiveCount,
    int32_t baseVertex,
    std::uint32_t startIndex) {
  D9CCommandRecordDrawIndexedPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  draw.header.size = sizeof(draw);
  draw.packet.state.primitiveType = primitiveType;
  draw.packet.primitiveCount = primitiveCount;
  draw.packet.baseVertex = baseVertex;
  draw.packet.startIndex = startIndex;
  draw.packet.numVertices = primitiveCount * 3u;
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

void testImportedIndexedDrawPreservesBoundIndexPolicy() {
  auto upper = std::make_unique<RecordingDxmt9Device>();
  auto* recorder = upper.get();
  auto* d3d = dxmt9::com::Direct3DCreate9Ex(dxmt9::com::D3D_SDK_VERSION,
                                            std::move(upper));
  check(d3d != nullptr, "create indexed recording d3d factory");

  PresentParameters params{};
  params.backBufferWidth = 16u;
  params.backBufferHeight = 16u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{88};
  params.presentationInterval = PresentInterval::Immediate;

  auto* device = d3d->CreateDeviceEx(0u, params, nullptr);
  check(device != nullptr, "create indexed recording d3d device");
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
    auto vertexBuffer = device->CreateBuffer(BufferDesc{
        .size = 96u,
        .pool = Pool::Default,
        .usage = UsageVertexBuffer,
    });
    auto indexBuffer = device->CreateBuffer(BufferDesc{
        .size = 32u,
        .pool = Pool::Default,
        .usage = UsageIndexBuffer,
    });
    check(renderTarget != nullptr, "indexed test render target");
    check(vertexBuffer != nullptr, "indexed test vertex buffer");
    check(indexBuffer != nullptr, "indexed test index buffer");

    D9CSurface renderTargetWire(renderTarget);
    D9CBuffer vertexBufferWire(vertexBuffer);
    D9CBuffer indexBufferWire(indexBuffer);

    const auto draw = makeIndexedDrawRecord(
        &renderTargetWire, &vertexBufferWire, &indexBufferWire);

    std::vector<std::uint8_t> bytes;
    appendRecord(bytes, draw);

    const std::vector<D9CCommandChunkWireHandleEntry> handles{
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, &renderTargetWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, &vertexBufferWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, &indexBufferWire),
    };
    const std::vector<D9CCommandChunkWireRecordHeader> records{
        wireRecordHeader(D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE, 0u,
                         sizeof(draw), 0u, 3u),
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
            "commit imported indexed draw chunk");

    checkEq(recorder->events.size(), static_cast<std::size_t>(4),
            "imported indexed draw event count");
    checkEventKind(recorder->events, 0u, EventKind::MarkChunkResources,
                   "indexed bulk retention happens first");
    checkEventKind(recorder->events, 1u, EventKind::SetSkipDrawResourceMarking,
                   "indexed replay enables bulk-retention skip");
    check(recorder->events[1].skipDrawResourceMarking,
          "indexed replay bulk-retention skip enabled");
    checkEventKind(recorder->events, 2u, EventKind::SubmitDraw,
                   "indexed draw is submitted after retention");
    checkEventKind(recorder->events, 3u, EventKind::SetSkipDrawResourceMarking,
                   "indexed replay resets bulk-retention skip");
    check(!recorder->events[3].skipDrawResourceMarking,
          "indexed replay bulk-retention skip disabled");

    const auto& retained = recorder->events[0].chunkHandles;
    checkEq(retained.size(), static_cast<std::size_t>(3),
            "indexed bulk retention resolves every imported wrapper handle");
    check(containsChunkHandle(retained, ChunkHandleKind::Surface,
                              renderTarget->handle()),
          "indexed retention includes render target handle");
    check(containsChunkHandle(retained, ChunkHandleKind::Buffer,
                              vertexBuffer->handle()),
          "indexed retention includes vertex buffer handle");
    check(containsChunkHandle(retained, ChunkHandleKind::Buffer,
                              indexBuffer->handle()),
          "indexed retention includes index buffer handle");

    const auto& drawRun = recorder->events[2].drawRun;
    checkEq(drawRun.draws.size(), static_cast<std::size_t>(1),
            "indexed imported draw run contains one draw param");
    check(drawRun.hot == drawRun.state.hot,
          "indexed imported draw records canonical and flat hot state together");
    checkEq(drawRun.hot.colorAttachments[0].handle.value,
            renderTarget->handle().value, "indexed draw observes imported RT state");
    checkEq(drawRun.hot.streamBuffers[0].value, vertexBuffer->handle().value,
            "indexed draw observes imported stream state");
    checkEq(drawRun.hot.streamOffsets[0], 12u,
            "indexed draw observes imported stream offset");
    checkEq(drawRun.hot.streamStrides[0], 16u,
            "indexed draw observes imported stream stride");
    checkEq(drawRun.hot.indexBuffer.value, indexBuffer->handle().value,
            "indexed draw keeps imported index buffer in hot state");
    check(drawRun.draws[0].primitiveType == PrimitiveType::TriangleList,
          "indexed draw maps imported primitive type");
    checkEq(drawRun.draws[0].primitiveCount, 2u,
            "indexed draw keeps imported primitive count");
    check(drawRun.draws[0].indexed,
          "indexed draw param remains indexed");
    checkEq(drawRun.draws[0].baseVertexIndex, -2,
            "indexed draw keeps imported base vertex");
    checkEq(drawRun.draws[0].startIndex, 5u,
            "indexed draw keeps imported start index");
    check(drawRun.draws[0].userIndexRange.empty(),
          "indexed draw uses bound index buffer, not user index payload");
    check(drawRun.payloadArena.empty(),
          "indexed draw run has no UP payload arena");
  }

  checkEq(device->Release(), 0u, "release indexed recording d3d device");
  checkEq(d3d->Release(), 0u, "release indexed recording d3d factory");
}

void testDeviceCSetIndicesInfersIndex32Format() {
  auto upper = std::make_unique<RecordingDxmt9Device>();
  auto* d3d = dxmt9::com::Direct3DCreate9Ex(dxmt9::com::D3D_SDK_VERSION,
                                            std::move(upper));
  check(d3d != nullptr, "create set-indices recording d3d factory");

  PresentParameters params{};
  params.backBufferWidth = 16u;
  params.backBufferHeight = 16u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{91};
  params.presentationInterval = PresentInterval::Immediate;

  auto* device = d3d->CreateDeviceEx(0u, params, nullptr);
  check(device != nullptr, "create set-indices recording d3d device");
  device->AddRef();

  {
    D9CDevice cDevice(device);
    auto indexBuffer = device->CreateBuffer(BufferDesc{
        .size = 64u,
        .pool = Pool::Default,
        .usage = UsageIndexBuffer,
    });
    check(indexBuffer != nullptr, "set-indices index buffer");

    D9CBuffer indexBufferWire(indexBuffer);
    indexBufferWire.desc.format = 102u;
    checkEq(dxmt9c_device_set_indices(&cDevice, &indexBufferWire), D3D_OK,
            "set-indices accepts INDEX32 buffer");
    check(device->coreDevice().state().indexBuffer == indexBuffer,
          "set-indices binds INDEX32 buffer");
    check(device->coreDevice().state().indexType == IndexType::UInt32,
          "set-indices infers UInt32 from D3DFMT_INDEX32 wire desc");
  }

  checkEq(device->Release(), 0u, "release set-indices recording d3d device");
  checkEq(d3d->Release(), 0u, "release set-indices recording d3d factory");
}

void testImportedIndexedDrawRunCoalescesParamOnlyPackets() {
  auto upper = std::make_unique<RecordingDxmt9Device>();
  auto* recorder = upper.get();
  auto* d3d = dxmt9::com::Direct3DCreate9Ex(dxmt9::com::D3D_SDK_VERSION,
                                            std::move(upper));
  check(d3d != nullptr, "create indexed run recording d3d factory");

  PresentParameters params{};
  params.backBufferWidth = 16u;
  params.backBufferHeight = 16u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{89};
  params.presentationInterval = PresentInterval::Immediate;

  auto* device = d3d->CreateDeviceEx(0u, params, nullptr);
  check(device != nullptr, "create indexed run recording d3d device");
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
    auto vertexBuffer = device->CreateBuffer(BufferDesc{
        .size = 256u,
        .pool = Pool::Default,
        .usage = UsageVertexBuffer,
    });
    auto indexBuffer = device->CreateBuffer(BufferDesc{
        .size = 128u,
        .pool = Pool::Default,
        .usage = UsageIndexBuffer,
    });
    check(renderTarget != nullptr, "indexed run render target");
    check(vertexBuffer != nullptr, "indexed run vertex buffer");
    check(indexBuffer != nullptr, "indexed run index buffer");

    D9CSurface renderTargetWire(renderTarget);
    D9CBuffer vertexBufferWire(vertexBuffer);
    D9CBuffer indexBufferWire(indexBuffer);
    D9CVertexDecl vertexDeclWire;
    vertexDeclWire.elements = {
        VertexElement{0, 0, 2, 0, 0, 0},
        VertexElement{0, 12, 4, 0, 10, 0},
        VertexElement{0, 16, 3, 0, 5, 0},
    };

    auto statefulDraw = makeIndexedDrawRecord(
        &renderTargetWire, &vertexBufferWire, &indexBufferWire);
    statefulDraw.packet.state.streamSources[0].offset = 36u;
    statefulDraw.packet.state.streamSources[0].stride = 28u;
    statefulDraw.packet.state.fvfValid = 1u;
    statefulDraw.packet.state.fvf = 0x11223344u;
    statefulDraw.packet.state.vdeclValid = 1u;
    statefulDraw.packet.state.vdeclHandle = wireHandleFromPtr(&vertexDeclWire);
    statefulDraw.packet.baseVertex = -1;
    statefulDraw.packet.startIndex = 3u;
    statefulDraw.packet.primitiveCount = 1u;

    const auto runDraw0 =
        makeIndexedParamOnlyDrawRecord(4u, 2u, -4, 9u);
    const auto runDraw1 =
        makeIndexedParamOnlyDrawRecord(4u, 3u, 5, 15u);

    std::vector<std::uint8_t> bytes;
    appendRecord(bytes, statefulDraw);
    appendRecord(bytes, runDraw0);
    appendRecord(bytes, runDraw1);

    const std::vector<D9CCommandChunkWireHandleEntry> handles{
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, &renderTargetWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, &vertexBufferWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, &indexBufferWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_VERTEX_DECL, &vertexDeclWire),
    };
    const std::vector<D9CCommandChunkWireRecordHeader> records{
        wireRecordHeader(D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE, 0u,
                         sizeof(statefulDraw), 0u, 4u),
        wireRecordHeader(D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
                         sizeof(statefulDraw), sizeof(runDraw0), 0u, 0u),
        wireRecordHeader(D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
                         sizeof(statefulDraw) + sizeof(runDraw0),
                         sizeof(runDraw1), 0u, 0u),
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
            "commit imported indexed draw-run chunk");

    checkEq(recorder->events.size(), static_cast<std::size_t>(5),
            "indexed draw-run import event count");
    checkEventKind(recorder->events, 0u, EventKind::MarkChunkResources,
                   "indexed draw-run bulk retention happens first");
    checkEventKind(recorder->events, 1u, EventKind::SetSkipDrawResourceMarking,
                   "indexed draw-run replay enables bulk-retention skip");
    checkEventKind(recorder->events, 2u, EventKind::SubmitDraw,
                   "stateful indexed draw replays before param-only run");
    checkEventKind(recorder->events, 3u, EventKind::SubmitDraw,
                   "param-only indexed records coalesce into one draw run");
    checkEventKind(recorder->events, 4u, EventKind::SetSkipDrawResourceMarking,
                   "indexed draw-run replay resets bulk-retention skip");

    const auto& retained = recorder->events[0].chunkHandles;
    checkEq(retained.size(), static_cast<std::size_t>(3),
            "indexed draw-run bulk retention skips vertex-decl-only pool handles");
    check(containsChunkHandle(retained, ChunkHandleKind::Surface,
                              renderTarget->handle()),
          "indexed draw-run retention includes render target");
    check(containsChunkHandle(retained, ChunkHandleKind::Buffer,
                              vertexBuffer->handle()),
          "indexed draw-run retention includes vertex buffer");
    check(containsChunkHandle(retained, ChunkHandleKind::Buffer,
                              indexBuffer->handle()),
          "indexed draw-run retention includes index buffer");

    const auto& first = recorder->events[2].drawRun;
    checkEq(first.draws.size(), static_cast<std::size_t>(1),
            "stateful indexed import stays a single draw");
    checkEq(first.hot.streamBuffers[0].value, vertexBuffer->handle().value,
            "stateful indexed import binds stream buffer");
    checkEq(first.hot.streamOffsets[0], 36u,
            "stateful indexed import applies stream source offset");
    checkEq(first.hot.streamStrides[0], 28u,
            "stateful indexed import applies stream source stride");
    checkEq(first.hot.indexBuffer.value, indexBuffer->handle().value,
            "stateful indexed import binds index buffer");
    check(first.state.shaderLayout.vertexDecl.elements == vertexDeclWire.elements,
          "stateful indexed import snapshots vertex declaration elements");
    checkEq(first.state.shaderLayout.vertexDecl.streams[0].offset, 36u,
            "stateful indexed import snapshots vertex decl stream offset");
    checkEq(first.state.shaderLayout.vertexDecl.streams[0].stride, 28u,
            "stateful indexed import snapshots vertex decl stream stride");

    const auto& run = recorder->events[3].drawRun;
    checkEq(run.draws.size(), static_cast<std::size_t>(2),
            "param-only imported indexed records submit as one draw run");
    check(run.hot == run.state.hot,
          "coalesced indexed draw run records canonical and flat hot state together");
    checkEq(run.hot.streamBuffers[0].value, vertexBuffer->handle().value,
            "coalesced indexed run inherits stream buffer");
    checkEq(run.hot.streamOffsets[0], 36u,
            "coalesced indexed run inherits stream source offset");
    checkEq(run.hot.streamStrides[0], 28u,
            "coalesced indexed run inherits stream source stride");
    checkEq(run.hot.indexBuffer.value, indexBuffer->handle().value,
            "coalesced indexed run inherits index buffer");
    check(run.state.shaderLayout.vertexDecl.elements == vertexDeclWire.elements,
          "coalesced indexed run inherits vertex declaration snapshot");
    check(run.draws[0].indexed, "first coalesced draw remains indexed");
    check(run.draws[1].indexed, "second coalesced draw remains indexed");
    checkEq(run.draws[0].primitiveCount, 2u,
            "first coalesced draw keeps primitive count");
    checkEq(run.draws[0].baseVertexIndex, -4,
            "first coalesced draw keeps base vertex");
    checkEq(run.draws[0].startIndex, 9u,
            "first coalesced draw keeps start index");
    checkEq(run.draws[1].primitiveCount, 3u,
            "second coalesced draw keeps primitive count");
    checkEq(run.draws[1].baseVertexIndex, 5,
            "second coalesced draw keeps base vertex");
    checkEq(run.draws[1].startIndex, 15u,
            "second coalesced draw keeps start index");
    check(run.payloadArena.empty(),
          "coalesced indexed run uses bound buffers, not UP payloads");
  }

  checkEq(device->Release(), 0u, "release indexed run recording d3d device");
  checkEq(d3d->Release(), 0u, "release indexed run recording d3d factory");
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

void testPresentSourceSelectionPrefersExplicitSourceOverCurrentBackBuffer() {
  SwapDesc present{};
  present.sourceSurface = Handle{0x7000u};
  const Handle currentBackBuffer{0x6000u};

  checkEq(dxmt9::core::metalqueue::selectPresentSourceHandle(present, currentBackBuffer).value,
          present.sourceSurface.value,
          "explicit present source wins over current backbuffer fallback");

  present.sourceSurface = Handle{};
  checkEq(dxmt9::core::metalqueue::selectPresentSourceHandle(present, currentBackBuffer).value,
          currentBackBuffer.value,
          "missing present source falls back to current backbuffer");
}

void testEncodePresentRejectsMissingSourceWithoutStatusCallback() {
  dxmt9::resources::Pool resourcePool;
  WMT::CommandBuffer commandBuffer{NULL_OBJECT_HANDLE};

  bool statusNotified = false;
  SwapDesc present{};
  present.window = Handle{0x8000u};
  present.width = 32u;
  present.height = 32u;
  present.notifyPresentationStatus = [&](bool) { statusNotified = true; };

  const bool encoded =
      dxmt9::encodePresent(commandBuffer, resourcePool, /*presenter=*/nullptr,
                           /*drawableToken=*/nullptr, present,
                           SurfaceHandle{0x12345678u}, 9u);
  check(!encoded, "encodePresent rejects a missing source surface");
  check(!statusNotified,
        "missing source does not report presentation status without drawable work");
}

void testEncodePresentRejectsSourceWithoutTexture() {
  BackendLimits limits{};
  dxmt9::resources::Pool resourcePool;
  const auto source = resourcePool.createSurface(
      WMT::Device{NULL_OBJECT_HANDLE},
      limits,
      SurfaceDesc{
          .width = 32u,
          .height = 32u,
          .format = Format::A8R8G8B8,
          .pool = Pool::Default,
          .usage = UsageRenderTarget,
          .renderTarget = true,
      });
  check(static_cast<bool>(source), "textureless source surface allocated");
  auto* sourceRecord = resourcePool.findSurface(source.value);
  check(sourceRecord != nullptr, "textureless source surface record");
  check(!sourceRecord->texture, "null WMT device creates no source texture");

  WMT::CommandBuffer commandBuffer{NULL_OBJECT_HANDLE};
  bool statusNotified = false;
  SwapDesc present{};
  present.window = Handle{0x8001u};
  present.sourceSurface = source;
  present.width = 32u;
  present.height = 32u;
  present.notifyPresentationStatus = [&](bool) { statusNotified = true; };

  const bool encoded =
      dxmt9::encodePresent(commandBuffer, resourcePool, /*presenter=*/nullptr,
                           /*drawableToken=*/nullptr, present, source, 10u);
  check(!encoded, "encodePresent rejects a surface with no texture");
  check(!statusNotified,
        "textureless source does not report presentation status without drawable work");
}

// R-VERIF-3.4 SlotIdentityStable: HandleArena depends on std::deque's
// guarantee that push_back does not invalidate previously-handed-out
// element addresses. The static_assert in HandleArena pins the container
// type at compile time; this runtime test additionally proves that, for
// the std::deque the build actually links against, a Record* captured
// from find() survives many subsequent inserts (the regression a
// vector-backed slot store would exhibit on first reallocation).
void testHandleArenaSlotPointerStableAcrossInserts() {
  using BufferArena =
      dxmt9::resources::detail::HandleArena<ArenaTestRecord,
                                            dxmt9::resources::detail::ResourceHandleKind::Buffer>;
  BufferArena buffers;

  const auto first = buffers.insert(ArenaTestRecord{});
  check(static_cast<bool>(first), "arena allocates first slot");
  ArenaTestRecord* anchorPtr = buffers.find(first.value);
  check(anchorPtr != nullptr, "arena returns pointer to first slot");
  anchorPtr->lastUsedSeqId = 0xfeedfaceu;

  // Force growth well past any plausible inline / small-buffer storage
  // a slot container might reasonably ship with. std::vector would
  // reallocate within this range; std::deque must not.
  for (int i = 0; i < 256; ++i) {
    const auto h = buffers.insert(ArenaTestRecord{});
    check(static_cast<bool>(h), "arena allocates Nth slot during growth");
  }

  ArenaTestRecord* reloadedPtr = buffers.find(first.value);
  check(reloadedPtr == anchorPtr,
        "arena returns the same address for the first slot after growth "
        "(deque pointer-stability axiom — R-VERIF-3.4 SlotIdentityStable)");
  check(reloadedPtr->lastUsedSeqId == 0xfeedfaceu,
        "first slot's contents survive inserts unchanged");
}

}  // namespace

int main() {
  try {
    testImportedChunkBulkRetentionAndBarrierOrdering();
    testImportedIndexedDrawPreservesBoundIndexPolicy();
    testDeviceCSetIndicesInfersIndex32Format();
    testImportedIndexedDrawRunCoalescesParamOnlyPackets();
    testResourcePoolArenaRejectsStaleHandles();
    testHandleArenaSlotPointerStableAcrossInserts();
    testResourcePoolUsesArenaStorageOnly();
    testPresentSourceSelectionPrefersExplicitSourceOverCurrentBackBuffer();
    testEncodePresentRejectsMissingSourceWithoutStatusCallback();
    testEncodePresentRejectsSourceWithoutTexture();
  } catch (const TestFailure& e) {
    std::cerr << "resource_hazard_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "resource_hazard_spec unexpected exception: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
