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
  SubmitStretchRect,
  Flush,
  SubmitColorFill,
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
  StretchRectDesc stretchRect;
  ColorFillDesc colorFill;
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
      param.bindingOverrideRange = appendPayload(payload.bindingOverrideData);
      param.bindingSnapshotRange = appendPayload(payload.bindingSnapshotData);
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

  void submitStretchRect(const StretchRectDesc& desc) override {
    RecordedEvent event;
    event.kind = EventKind::SubmitStretchRect;
    event.stretchRect = desc;
    events.push_back(std::move(event));
  }

  void flush() override {
    RecordedEvent event;
    event.kind = EventKind::Flush;
    events.push_back(std::move(event));
  }

  void submitColorFill(const ColorFillDesc& desc) override {
    RecordedEvent event;
    event.kind = EventKind::SubmitColorFill;
    event.colorFill = desc;
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

std::span<const u8> recordedPayloadBytes(const RecordedDrawRun& run,
                                         DrawPayloadRange range) {
  return drawRunPayloadBytes(range, std::span<const u8>(run.payloadArena));
}

DrawBindingOverride recordedBindingOverride(const RecordedDrawRun& run,
                                            DrawPayloadRange range,
                                            std::string_view message) {
  const auto bytes = recordedPayloadBytes(run, range);
  checkEq(bytes.size(), sizeof(DrawBindingOverride), message);
  DrawBindingOverride binding{};
  std::memcpy(&binding, bytes.data(), sizeof(binding));
  return binding;
}

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

D9CCommandRecordApplyState makeApplyStateRecord(D9CSurface* renderTarget,
                                                D9CBuffer* vertexBuffer) {
  D9CCommandRecordApplyState apply{};
  apply.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
  apply.header.size = sizeof(apply);
  apply.packet.rtMask = 0x1u;
  apply.packet.rtHandles[0] = wireHandleFromPtr(renderTarget);
  apply.packet.streamSourceMask = 0x1u;
  apply.packet.streamSources[0].buffer = wireHandleFromPtr(vertexBuffer);
  apply.packet.streamSources[0].offset = 8u;
  apply.packet.streamSources[0].stride = 24u;
  return apply;
}

D9CCommandRecordDrawPrimitive makeParamOnlyDrawRecord(
    std::uint32_t startVertex,
    std::uint32_t primitiveCount = 1u) {
  D9CCommandRecordDrawPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  draw.header.size = sizeof(draw);
  draw.packet.primitiveType = 4u;
  draw.packet.startVertex = startVertex;
  draw.packet.primitiveCount = primitiveCount;
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

D9CCommandRecordStretchRect makeStretchRectRecord(D9CSurface* source,
                                                  D9CSurface* destination) {
  D9CCommandRecordStretchRect stretch{};
  stretch.header.type = D9C_COMMAND_RECORD_STRETCH_RECT;
  stretch.header.size = sizeof(stretch);
  stretch.srcWire = wireValueFromPtr(source);
  stretch.dstWire = wireValueFromPtr(destination);
  stretch.hasSrcRect = 1u;
  stretch.hasDstRect = 1u;
  stretch.filter = 2u;
  stretch.srcRect = D9CRect{1, 2, 13, 18};
  stretch.dstRect = D9CRect{3, 5, 27, 37};
  return stretch;
}

D9CCommandRecordColorFill makeColorFillRecord(D9CSurface* destination) {
  D9CCommandRecordColorFill color{};
  color.header.type = D9C_COMMAND_RECORD_COLOR_FILL;
  color.header.size = sizeof(color);
  color.surfaceWire = wireValueFromPtr(destination);
  color.colorARGB = 0x8040a0ffu;
  color.hasRect = 1u;
  color.rect = D9CRect{4, 6, 14, 16};
  return color;
}

bool containsChunkHandle(const std::vector<ChunkHandleEntry>& entries,
                         ChunkHandleKind kind,
                         Handle handle) {
  return std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
           return entry.kind == kind && entry.handle == handle;
         }) != entries.end();
}

void checkNear(float left, float right, std::string_view message) {
  if (std::abs(left - right) > 0.0001f) {
    fail(std::string(message));
  }
}

void checkEventKind(const std::vector<RecordedEvent>& events,
                    std::size_t index,
                    EventKind expected,
                    std::string_view message) {
  check(index < events.size(), message);
  check(events[index].kind == expected, message);
}

// device_c_chunk_replay.cpp's offload branch always replays with
// skipDrawResourceMarking=false ("Deliberately NOT didBulkMarkResources" --
// the worker's per-draw markDrawResources must re-pin resources at the real
// append-time seqId instead of trusting the app-thread bulk mark's
// nextSeqId_ snapshot, R-BACK-2.51 hardening). That means the
// SetSkipDrawResourceMarking(true)/(false) bracket events the sync-path
// assertions below expect never fire under DXMT9_OFFLOAD_COMMIT_REPLAY=1.
// Delegate to the production resolver
// (dxmt9::d3d9::offloadCommitReplayEnabled(), device_c_replay_offload.cpp)
// so event-shape assertions can adapt instead of hard-coding a shape that
// only holds for the synchronous replay path. A local copy of the parse
// drifted once when the engine default flipped on — do not reintroduce it.
bool offloadReplayActive() {
  return dxmt9::d3d9::offloadCommitReplayEnabled();
}

// Sync-shaped event count, adjusted for the offload path's missing pair of
// SetSkipDrawResourceMarking bracket events (present in the sync shape,
// absent entirely when offload is active).
std::size_t expectedEventCount(std::size_t syncCount) {
  return offloadReplayActive() ? syncCount - 2u : syncCount;
}

// Sync-shaped event index for an event that sits strictly between the two
// SetSkipDrawResourceMarking brackets (i.e. not the leading
// MarkChunkResources at index 0, and not either bracket itself), adjusted
// for the offload path's missing leading bracket.
std::size_t midEventIndex(std::size_t syncIndex) {
  return offloadReplayActive() ? syncIndex - 1u : syncIndex;
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
    // See imported_apply_state_value_spec.cpp's commitWireChunk() comment:
    // this harness's D9C* wire objects are stack-scoped locals, so under
    // DXMT9_OFFLOAD_COMMIT_REPLAY=1 the caller must fence against the
    // offload worker before those locals (or the enclosing D9CDevice) start
    // unwinding. No-op when offload is disabled.
    dxmt9::d3d9::drainDeferredReplay(&cDevice);

    checkEq(recorder->events.size(), expectedEventCount(9u),
            "imported chunk event count");
    checkEventKind(recorder->events, 0u, EventKind::MarkChunkResources,
                   "bulk retention happens first");
    if (!offloadReplayActive()) {
      checkEventKind(recorder->events, 1u, EventKind::SetSkipDrawResourceMarking,
                     "per-draw retention skip is enabled before replay");
      check(recorder->events[1].skipDrawResourceMarking,
            "per-draw retention skip enabled");
    }
    checkEventKind(recorder->events, midEventIndex(2u), EventKind::SubmitDraw,
                   "first draw is replayed after retention");
    checkEventKind(recorder->events, midEventIndex(3u), EventKind::SubmitClear,
                   "clear barrier remains ordered after first draw");
    checkEventKind(recorder->events, midEventIndex(4u), EventKind::SubmitDraw,
                   "second draw remains ordered after clear");
    checkEventKind(recorder->events, midEventIndex(5u), EventKind::SubmitReadback,
                   "readback boundary is submitted after second draw");
    checkEventKind(recorder->events, midEventIndex(6u), EventKind::Flush,
                   "readback boundary flushes synchronously");
    checkEventKind(recorder->events, midEventIndex(7u), EventKind::SubmitSurfaceCopy,
                   "readback fallback copy remains after synchronous flush");
    if (!offloadReplayActive()) {
      checkEventKind(recorder->events, 8u, EventKind::SetSkipDrawResourceMarking,
                     "per-draw retention skip resets after replay");
      check(!recorder->events[8].skipDrawResourceMarking,
            "per-draw retention skip disabled");
    }

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

    const auto& firstDrawRun = recorder->events[midEventIndex(2u)].drawRun;
    checkEq(firstDrawRun.draws.size(), static_cast<std::size_t>(1),
            "first draw run contains one imported draw param");
    check(firstDrawRun.hot == firstDrawRun.state.hot,
          "first draw run records canonical and flat hot state together");
    checkEq(firstDrawRun.hot.colorAttachments[0].handle.value,
            renderTarget->handle().value, "first draw observes imported RT state");
    checkEq(firstDrawRun.hot.streamBuffers[0].value, std::uint64_t{0},
            "first draw base stream buffer is binding-agnostic");
    checkEq(firstDrawRun.hot.streamOffsets[0], 0u,
            "first draw base stream offset is binding-agnostic");
    checkEq(firstDrawRun.hot.streamStrides[0], 0u,
            "first draw base stream stride is binding-agnostic");
    checkEq(firstDrawRun.hot.streamMask, 0u,
            "first draw base stream mask is binding-agnostic");
    const auto firstBinding = recordedBindingOverride(
        firstDrawRun, firstDrawRun.draws[0].bindingOverrideRange,
        "first draw binding override payload size");
    checkEq(firstBinding.streamMask, 1u, "first draw binding stream mask");
    checkEq(firstBinding.streams[0].buffer.value, vertexBuffer->handle().value,
            "first draw binding stream buffer");
    checkEq(firstBinding.streams[0].offset, 0u,
            "first draw binding stream offset");
    checkEq(firstBinding.streams[0].stride, 16u,
            "first draw binding stream stride");
    checkEq(firstDrawRun.draws[0].startVertex, 0u,
            "first draw param keeps imported start vertex");
    check(!firstDrawRun.draws[0].indexed,
          "first draw param remains non-indexed");
    check(firstDrawRun.draws[0].userVertexRange.empty(),
          "first draw uses bound vertex buffer, not user vertex payload");
    check(firstDrawRun.draws[0].userIndexRange.empty(),
          "first draw has no user index payload");

    const auto& secondDrawRun = recorder->events[midEventIndex(4u)].drawRun;
    checkEq(secondDrawRun.draws.size(), static_cast<std::size_t>(1),
            "second draw run contains one imported draw param");
    check(secondDrawRun.hot == secondDrawRun.state.hot,
          "second draw run records canonical and flat hot state together");
    checkEq(secondDrawRun.draws[0].startVertex, 3u,
            "second draw param remains in record order");
    checkEq(secondDrawRun.hot.colorAttachments[0].handle.value,
            renderTarget->handle().value, "second draw observes imported RT state");
    checkEq(secondDrawRun.hot.streamBuffers[0].value, std::uint64_t{0},
            "second draw base stream buffer is binding-agnostic");
    const auto secondBinding = recordedBindingOverride(
        secondDrawRun, secondDrawRun.draws[0].bindingOverrideRange,
        "second draw binding override payload size");
    checkEq(secondBinding.streamMask, 1u, "second draw binding stream mask");
    checkEq(secondBinding.streams[0].buffer.value, vertexBuffer->handle().value,
            "second draw binding stream buffer");
    checkEq(secondBinding.streams[0].offset, 0u,
            "second draw binding stream offset");
    checkEq(secondBinding.streams[0].stride, 16u,
            "second draw binding stream stride");
    checkEq(recorder->events[midEventIndex(5u)].readback.source.value, renderTarget->handle().value,
            "readback source handle");
    checkEq(recorder->events[midEventIndex(5u)].readback.destination.value,
            readbackTarget->handle().value, "readback destination handle");
  }

  checkEq(device->Release(), 0u, "release recording d3d device");
  checkEq(d3d->Release(), 0u, "release recording d3d factory");
}

void testReadbackBoundarySplitsCoalescedImportedDrawRun() {
  auto upper = std::make_unique<RecordingDxmt9Device>();
  auto* recorder = upper.get();
  auto* d3d = dxmt9::com::Direct3DCreate9Ex(dxmt9::com::D3D_SDK_VERSION,
                                            std::move(upper));
  check(d3d != nullptr, "create readback-run recording d3d factory");

  PresentParameters params{};
  params.backBufferWidth = 16u;
  params.backBufferHeight = 16u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{78};
  params.presentationInterval = PresentInterval::Immediate;

  auto* device = d3d->CreateDeviceEx(0u, params, nullptr);
  check(device != nullptr, "create readback-run recording d3d device");
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
        .size = 128u,
        .pool = Pool::Default,
        .usage = UsageVertexBuffer,
    });
    check(renderTarget != nullptr, "readback-run render target");
    check(readbackTarget != nullptr, "readback-run readback target");
    check(vertexBuffer != nullptr, "readback-run vertex buffer");

    D9CSurface renderTargetWire(renderTarget);
    D9CSurface readbackTargetWire(readbackTarget);
    D9CBuffer vertexBufferWire(vertexBuffer);

    const auto apply = makeApplyStateRecord(&renderTargetWire, &vertexBufferWire);
    const auto draw0 = makeParamOnlyDrawRecord(2u, 1u);
    const auto draw1 = makeParamOnlyDrawRecord(5u, 2u);
    const auto readback =
        makeReadbackRecord(&renderTargetWire, &readbackTargetWire);
    const auto draw2 = makeParamOnlyDrawRecord(11u, 3u);

    std::vector<std::uint8_t> bytes;
    appendRecord(bytes, apply);
    appendRecord(bytes, draw0);
    appendRecord(bytes, draw1);
    appendRecord(bytes, readback);
    appendRecord(bytes, draw2);

    const std::vector<D9CCommandChunkWireHandleEntry> handles{
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, &renderTargetWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, &vertexBufferWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, &readbackTargetWire),
    };
    const std::vector<D9CCommandChunkWireRecordHeader> records{
        wireRecordHeader(D9C_COMMAND_RECORD_APPLY_STATE, 0u,
                         sizeof(apply), 0u, 2u),
        wireRecordHeader(D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                         sizeof(apply), sizeof(draw0), 0u, 0u),
        wireRecordHeader(D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                         sizeof(apply) + sizeof(draw0), sizeof(draw1), 0u, 0u),
        wireRecordHeader(D9C_COMMAND_RECORD_READBACK,
                         sizeof(apply) + sizeof(draw0) + sizeof(draw1),
                         sizeof(readback), 0u, 3u),
        wireRecordHeader(D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                         sizeof(apply) + sizeof(draw0) + sizeof(draw1) +
                             sizeof(readback),
                         sizeof(draw2), 0u, 0u),
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
            "commit readback-split imported draw-run chunk");
    // See imported_apply_state_value_spec.cpp's commitWireChunk() comment.
    dxmt9::d3d9::drainDeferredReplay(&cDevice);

    checkEq(recorder->events.size(), expectedEventCount(8u),
            "readback-split event count");
    checkEventKind(recorder->events, 0u, EventKind::MarkChunkResources,
                   "readback-split bulk retention happens first");
    if (!offloadReplayActive()) {
      checkEventKind(recorder->events, 1u, EventKind::SetSkipDrawResourceMarking,
                     "readback-split enables bulk-retention skip");
      check(recorder->events[1].skipDrawResourceMarking,
            "readback-split bulk-retention skip enabled");
    }
    checkEventKind(recorder->events, midEventIndex(2u), EventKind::SubmitDraw,
                   "draws before readback submit as one run");
    checkEventKind(recorder->events, midEventIndex(3u), EventKind::SubmitReadback,
                   "readback follows the coalesced draw run");
    checkEventKind(recorder->events, midEventIndex(4u), EventKind::Flush,
                   "readback forces a synchronous flush before fallback copy");
    checkEventKind(recorder->events, midEventIndex(5u), EventKind::SubmitSurfaceCopy,
                   "readback fallback copy stays before later draws");
    checkEventKind(recorder->events, midEventIndex(6u), EventKind::SubmitDraw,
                   "draw after readback starts a new run");
    if (!offloadReplayActive()) {
      checkEventKind(recorder->events, 7u, EventKind::SetSkipDrawResourceMarking,
                     "readback-split resets bulk-retention skip");
      check(!recorder->events[7].skipDrawResourceMarking,
            "readback-split bulk-retention skip disabled");
    }

    const auto& retained = recorder->events[0].chunkHandles;
    checkEq(retained.size(), static_cast<std::size_t>(3),
            "readback-split retention resolves every pool-backed handle");
    check(containsChunkHandle(retained, ChunkHandleKind::Surface,
                              renderTarget->handle()),
          "readback-split retention includes render target");
    check(containsChunkHandle(retained, ChunkHandleKind::Buffer,
                              vertexBuffer->handle()),
          "readback-split retention includes vertex buffer");
    check(containsChunkHandle(retained, ChunkHandleKind::Surface,
                              readbackTarget->handle()),
          "readback-split retention includes readback target");

    const auto& beforeReadback = recorder->events[midEventIndex(2u)].drawRun;
    checkEq(beforeReadback.draws.size(), static_cast<std::size_t>(2),
            "pre-readback draw run keeps both param-only draws");
    checkEq(beforeReadback.hot.colorAttachments[0].handle.value,
            renderTarget->handle().value,
            "pre-readback run inherits applied render target");
    checkEq(beforeReadback.hot.streamBuffers[0].value,
            vertexBuffer->handle().value,
            "pre-readback run inherits applied stream buffer");
    checkEq(beforeReadback.hot.streamOffsets[0], 8u,
            "pre-readback run inherits applied stream offset");
    checkEq(beforeReadback.hot.streamStrides[0], 24u,
            "pre-readback run inherits applied stream stride");
    checkEq(beforeReadback.draws[0].startVertex, 2u,
            "first pre-readback draw keeps start vertex");
    checkEq(beforeReadback.draws[0].primitiveCount, 1u,
            "first pre-readback draw keeps primitive count");
    checkEq(beforeReadback.draws[1].startVertex, 5u,
            "second pre-readback draw keeps start vertex");
    checkEq(beforeReadback.draws[1].primitiveCount, 2u,
            "second pre-readback draw keeps primitive count");
    check(beforeReadback.payloadArena.empty(),
          "pre-readback run has no UP payload arena");

    checkEq(recorder->events[midEventIndex(3u)].readback.source.value,
            renderTarget->handle().value, "readback-split source handle");
    checkEq(recorder->events[midEventIndex(3u)].readback.destination.value,
            readbackTarget->handle().value,
            "readback-split destination handle");

    const auto& afterReadback = recorder->events[midEventIndex(6u)].drawRun;
    checkEq(afterReadback.draws.size(), static_cast<std::size_t>(1),
            "post-readback draw run is not merged across readback");
    checkEq(afterReadback.hot.colorAttachments[0].handle.value,
            renderTarget->handle().value,
            "post-readback run keeps applied render target");
    checkEq(afterReadback.hot.streamBuffers[0].value, std::uint64_t{0},
            "post-readback run base stream buffer is binding-agnostic");
    checkEq(afterReadback.hot.streamOffsets[0], 0u,
            "post-readback run base stream offset is binding-agnostic");
    checkEq(afterReadback.hot.streamStrides[0], 0u,
            "post-readback run base stream stride is binding-agnostic");
    const auto afterBinding = recordedBindingOverride(
        afterReadback, afterReadback.draws[0].bindingOverrideRange,
        "post-readback draw binding override payload size");
    checkEq(afterBinding.streamMask, 1u,
            "post-readback draw binding stream mask");
    checkEq(afterBinding.streams[0].buffer.value, vertexBuffer->handle().value,
            "post-readback draw binding stream buffer");
    checkEq(afterBinding.streams[0].offset, 8u,
            "post-readback draw binding stream offset");
    checkEq(afterBinding.streams[0].stride, 24u,
            "post-readback draw binding stream stride");
    checkEq(afterReadback.draws[0].startVertex, 11u,
            "post-readback draw keeps start vertex");
    checkEq(afterReadback.draws[0].primitiveCount, 3u,
            "post-readback draw keeps primitive count");
    check(afterReadback.draws[0].userVertexRange.empty(),
          "post-readback draw uses bound vertex buffer, not user vertex payload");
    check(afterReadback.draws[0].userIndexRange.empty(),
          "post-readback draw has no user index payload");
  }

  checkEq(device->Release(), 0u, "release readback-run recording d3d device");
  checkEq(d3d->Release(), 0u, "release readback-run recording d3d factory");
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
    // See imported_apply_state_value_spec.cpp's commitWireChunk() comment.
    dxmt9::d3d9::drainDeferredReplay(&cDevice);

    checkEq(recorder->events.size(), expectedEventCount(4u),
            "imported indexed draw event count");
    checkEventKind(recorder->events, 0u, EventKind::MarkChunkResources,
                   "indexed bulk retention happens first");
    if (!offloadReplayActive()) {
      checkEventKind(recorder->events, 1u, EventKind::SetSkipDrawResourceMarking,
                     "indexed replay enables bulk-retention skip");
      check(recorder->events[1].skipDrawResourceMarking,
            "indexed replay bulk-retention skip enabled");
    }
    checkEventKind(recorder->events, midEventIndex(2u), EventKind::SubmitDraw,
                   "indexed draw is submitted after retention");
    if (!offloadReplayActive()) {
      checkEventKind(recorder->events, 3u, EventKind::SetSkipDrawResourceMarking,
                     "indexed replay resets bulk-retention skip");
      check(!recorder->events[3].skipDrawResourceMarking,
            "indexed replay bulk-retention skip disabled");
    }

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

    const auto& drawRun = recorder->events[midEventIndex(2u)].drawRun;
    checkEq(drawRun.draws.size(), static_cast<std::size_t>(1),
            "indexed imported draw run contains one draw param");
    check(drawRun.hot == drawRun.state.hot,
          "indexed imported draw records canonical and flat hot state together");
    checkEq(drawRun.hot.colorAttachments[0].handle.value,
            renderTarget->handle().value, "indexed draw observes imported RT state");
    checkEq(drawRun.hot.streamBuffers[0].value, std::uint64_t{0},
            "indexed draw base stream buffer is binding-agnostic");
    checkEq(drawRun.hot.streamOffsets[0], 0u,
            "indexed draw base stream offset is binding-agnostic");
    checkEq(drawRun.hot.streamStrides[0], 0u,
            "indexed draw base stream stride is binding-agnostic");
    checkEq(drawRun.hot.streamMask, 0u,
            "indexed draw base stream mask is binding-agnostic");
    checkEq(drawRun.hot.indexBuffer.value, std::uint64_t{0},
            "indexed draw base index buffer is binding-agnostic");
    const auto binding = recordedBindingOverride(
        drawRun, drawRun.draws[0].bindingOverrideRange,
        "indexed draw binding override payload size");
    checkEq(binding.streamMask, 1u, "indexed draw binding stream mask");
    checkEq(binding.streams[0].buffer.value, vertexBuffer->handle().value,
            "indexed draw binding stream buffer");
    checkEq(binding.streams[0].offset, 12u,
            "indexed draw binding stream offset");
    checkEq(binding.streams[0].stride, 16u,
            "indexed draw binding stream stride");
    check(binding.indexBufferValid, "indexed draw binding index valid");
    checkEq(binding.indexBuffer.value, indexBuffer->handle().value,
            "indexed draw binding index buffer");
    check(binding.indexType == IndexType::UInt16,
          "indexed draw binding index type");
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
    check(drawRun.draws[0].userVertexRange.empty(),
          "indexed draw uses bound vertex buffer, not user vertex payload");
  }

  checkEq(device->Release(), 0u, "release indexed recording d3d device");
  checkEq(d3d->Release(), 0u, "release indexed recording d3d factory");
}

void testImportedSurfaceOpsPreserveBoundaryPayloads() {
  auto upper = std::make_unique<RecordingDxmt9Device>();
  auto* recorder = upper.get();
  auto* d3d = dxmt9::com::Direct3DCreate9Ex(dxmt9::com::D3D_SDK_VERSION,
                                            std::move(upper));
  check(d3d != nullptr, "create surface-op recording d3d factory");

  PresentParameters params{};
  params.backBufferWidth = 32u;
  params.backBufferHeight = 32u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{87};
  params.presentationInterval = PresentInterval::Immediate;

  auto* device = d3d->CreateDeviceEx(0u, params, nullptr);
  check(device != nullptr, "create surface-op recording d3d device");
  device->AddRef();

  {
    D9CDevice cDevice(device);

    auto source = device->CreateSurface(SurfaceDesc{
        .width = 32u,
        .height = 32u,
        .format = Format::A8R8G8B8,
        .pool = Pool::Default,
        .usage = UsageRenderTarget,
        .renderTarget = true,
    });
    auto destination = device->CreateSurface(SurfaceDesc{
        .width = 48u,
        .height = 48u,
        .format = Format::A8R8G8B8,
        .pool = Pool::Default,
        .usage = UsageRenderTarget,
        .renderTarget = true,
    });
    auto fillTarget = device->CreateSurface(SurfaceDesc{
        .width = 24u,
        .height = 24u,
        .format = Format::A8R8G8B8,
        .pool = Pool::Default,
        .usage = UsageRenderTarget,
        .renderTarget = true,
    });
    check(source != nullptr, "surface-op source");
    check(destination != nullptr, "surface-op destination");
    check(fillTarget != nullptr, "surface-op fill target");

    D9CSurface sourceWire(source);
    D9CSurface destinationWire(destination);
    D9CSurface fillTargetWire(fillTarget);

    const auto stretch =
        makeStretchRectRecord(&sourceWire, &destinationWire);
    const auto color = makeColorFillRecord(&fillTargetWire);

    std::vector<std::uint8_t> bytes;
    appendRecord(bytes, stretch);
    appendRecord(bytes, color);

    const std::vector<D9CCommandChunkWireHandleEntry> handles{
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, &sourceWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, &destinationWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, &fillTargetWire),
    };
    const std::vector<D9CCommandChunkWireRecordHeader> records{
        wireRecordHeader(D9C_COMMAND_RECORD_STRETCH_RECT, 0u,
                         sizeof(stretch), 0u, 2u),
        wireRecordHeader(D9C_COMMAND_RECORD_COLOR_FILL, sizeof(stretch),
                         sizeof(color), 2u, 1u),
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
            "commit imported surface-op chunk");
    // See imported_apply_state_value_spec.cpp's commitWireChunk() comment.
    dxmt9::d3d9::drainDeferredReplay(&cDevice);

    checkEq(recorder->events.size(), expectedEventCount(5u),
            "surface-op import event count");
    checkEventKind(recorder->events, 0u, EventKind::MarkChunkResources,
                   "surface-op retention happens first");
    if (!offloadReplayActive()) {
      checkEventKind(recorder->events, 1u, EventKind::SetSkipDrawResourceMarking,
                     "surface-op replay enables bulk-retention skip");
      check(recorder->events[1].skipDrawResourceMarking,
            "surface-op bulk-retention skip enabled");
    }
    checkEventKind(recorder->events, midEventIndex(2u), EventKind::SubmitStretchRect,
                   "stretch rect submits after retention");
    checkEventKind(recorder->events, midEventIndex(3u), EventKind::SubmitColorFill,
                   "color fill remains ordered after stretch");
    if (!offloadReplayActive()) {
      checkEventKind(recorder->events, 4u, EventKind::SetSkipDrawResourceMarking,
                     "surface-op replay resets bulk-retention skip");
      check(!recorder->events[4].skipDrawResourceMarking,
            "surface-op bulk-retention skip disabled");
    }

    const auto& retained = recorder->events[0].chunkHandles;
    checkEq(retained.size(), static_cast<std::size_t>(3),
            "surface-op retention resolves every record-scoped surface");
    check(containsChunkHandle(retained, ChunkHandleKind::Surface,
                              source->handle()),
          "surface-op retention includes stretch source");
    check(containsChunkHandle(retained, ChunkHandleKind::Surface,
                              destination->handle()),
          "surface-op retention includes stretch destination");
    check(containsChunkHandle(retained, ChunkHandleKind::Surface,
                              fillTarget->handle()),
          "surface-op retention includes color-fill target");

    const auto& stretchDesc = recorder->events[midEventIndex(2u)].stretchRect;
    checkEq(stretchDesc.source.value, source->handle().value,
            "stretch boundary source handle");
    checkEq(stretchDesc.destination.value, destination->handle().value,
            "stretch boundary destination handle");
    check(stretchDesc.sourceRect == Rect{1, 2, 13, 18},
          "stretch boundary source rect");
    check(stretchDesc.destinationRect == Rect{3, 5, 27, 37},
          "stretch boundary destination rect");
    check(stretchDesc.linear,
          "stretch boundary preserves linear filter intent");
    checkEq(stretchDesc.sourceSampleCount, 1u,
            "stretch boundary source sample count default");
    checkEq(stretchDesc.destinationSampleCount, 1u,
            "stretch boundary destination sample count default");

    const auto& colorDesc = recorder->events[midEventIndex(3u)].colorFill;
    checkEq(colorDesc.destination.value, fillTarget->handle().value,
            "color-fill boundary destination handle");
    check(colorDesc.hasRect, "color-fill boundary preserves rect flag");
    check(colorDesc.rect == Rect{4, 6, 14, 16},
          "color-fill boundary rect");
    checkNear(colorDesc.color.r, 64.0f / 255.0f,
              "color-fill boundary red channel");
    checkNear(colorDesc.color.g, 160.0f / 255.0f,
              "color-fill boundary green channel");
    checkNear(colorDesc.color.b, 1.0f,
              "color-fill boundary blue channel");
    checkNear(colorDesc.color.a, 128.0f / 255.0f,
              "color-fill boundary alpha channel");
  }

  checkEq(device->Release(), 0u,
          "release surface-op recording d3d device");
  checkEq(d3d->Release(), 0u, "release surface-op recording d3d factory");
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
    // See imported_apply_state_value_spec.cpp's commitWireChunk() comment.
    dxmt9::d3d9::drainDeferredReplay(&cDevice);

    checkEq(recorder->events.size(), expectedEventCount(4u),
            "indexed draw-run import event count");
    checkEventKind(recorder->events, 0u, EventKind::MarkChunkResources,
                   "indexed draw-run bulk retention happens first");
    if (!offloadReplayActive()) {
      checkEventKind(recorder->events, 1u, EventKind::SetSkipDrawResourceMarking,
                     "indexed draw-run replay enables bulk-retention skip");
    }
    checkEventKind(recorder->events, midEventIndex(2u), EventKind::SubmitDraw,
                   "stateful and param-only indexed records coalesce");
    if (!offloadReplayActive()) {
      checkEventKind(recorder->events, 3u, EventKind::SetSkipDrawResourceMarking,
                     "indexed draw-run replay resets bulk-retention skip");
    }

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

    const auto& run = recorder->events[midEventIndex(2u)].drawRun;
    checkEq(run.draws.size(), static_cast<std::size_t>(3),
            "stateful and param-only imported indexed records submit as one draw run");
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
    checkEq(run.state.shaderLayout.vertexDecl.streams[0].offset, 36u,
            "coalesced indexed run snapshots vertex decl stream offset");
    checkEq(run.state.shaderLayout.vertexDecl.streams[0].stride, 28u,
            "coalesced indexed run snapshots vertex decl stream stride");
    check(run.draws[0].indexed, "stateful coalesced draw remains indexed");
    check(run.draws[1].indexed, "first param-only coalesced draw remains indexed");
    check(run.draws[2].indexed, "second param-only coalesced draw remains indexed");
    checkEq(run.draws[0].primitiveCount, 1u,
            "stateful coalesced draw keeps primitive count");
    checkEq(run.draws[0].baseVertexIndex, -1,
            "stateful coalesced draw keeps base vertex");
    checkEq(run.draws[0].startIndex, 3u,
            "stateful coalesced draw keeps start index");
    checkEq(run.draws[1].primitiveCount, 2u,
            "first coalesced draw keeps primitive count");
    checkEq(run.draws[1].baseVertexIndex, -4,
            "first coalesced draw keeps base vertex");
    checkEq(run.draws[1].startIndex, 9u,
            "first coalesced draw keeps start index");
    checkEq(run.draws[2].primitiveCount, 3u,
            "second coalesced draw keeps primitive count");
    checkEq(run.draws[2].baseVertexIndex, 5,
            "second coalesced draw keeps base vertex");
    checkEq(run.draws[2].startIndex, 15u,
            "second coalesced draw keeps start index");
    check(run.payloadArena.empty(),
          "coalesced indexed run uses bound buffers, not UP payloads");
  }

  checkEq(device->Release(), 0u, "release indexed run recording d3d device");
  checkEq(d3d->Release(), 0u, "release indexed run recording d3d factory");
}

void testImportedDrawRetainsOnlyRecordScopedHandles() {
  auto upper = std::make_unique<RecordingDxmt9Device>();
  auto* recorder = upper.get();
  auto* d3d = dxmt9::com::Direct3DCreate9Ex(dxmt9::com::D3D_SDK_VERSION,
                                            std::move(upper));
  check(d3d != nullptr, "create scoped-handle recording d3d factory");

  PresentParameters params{};
  params.backBufferWidth = 16u;
  params.backBufferHeight = 16u;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.deviceWindow = Handle{90};
  params.presentationInterval = PresentInterval::Immediate;

  auto* device = d3d->CreateDeviceEx(0u, params, nullptr);
  check(device != nullptr, "create scoped-handle recording d3d device");
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
    auto unusedSurface = device->CreateSurface(SurfaceDesc{
        .width = 32u,
        .height = 32u,
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
    auto unusedBuffer = device->CreateBuffer(BufferDesc{
        .size = 64u,
        .pool = Pool::Default,
        .usage = UsageVertexBuffer,
    });
    check(renderTarget != nullptr, "scoped-handle render target");
    check(unusedSurface != nullptr, "scoped-handle unused surface");
    check(vertexBuffer != nullptr, "scoped-handle vertex buffer");
    check(unusedBuffer != nullptr, "scoped-handle unused buffer");

    D9CSurface renderTargetWire(renderTarget);
    D9CSurface unusedSurfaceWire(unusedSurface);
    D9CBuffer vertexBufferWire(vertexBuffer);
    D9CBuffer unusedBufferWire(unusedBuffer);

    const auto draw = makeDrawRecord(&renderTargetWire, &vertexBufferWire, 4u);

    std::vector<std::uint8_t> bytes;
    appendRecord(bytes, draw);

    const std::vector<D9CCommandChunkWireHandleEntry> handles{
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, &renderTargetWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, &vertexBufferWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, &unusedSurfaceWire),
        wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, &unusedBufferWire),
    };
    const std::vector<D9CCommandChunkWireRecordHeader> records{
        wireRecordHeader(D9C_COMMAND_RECORD_DRAW_PRIMITIVE, 0u, sizeof(draw),
                         0u, 2u),
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
            "commit scoped-handle imported draw chunk");
    // See imported_apply_state_value_spec.cpp's commitWireChunk() comment.
    dxmt9::d3d9::drainDeferredReplay(&cDevice);

    checkEq(recorder->events.size(), expectedEventCount(4u),
            "scoped-handle import event count");
    checkEventKind(recorder->events, 0u, EventKind::MarkChunkResources,
                   "scoped-handle bulk retention happens first");
    if (!offloadReplayActive()) {
      checkEventKind(recorder->events, 1u, EventKind::SetSkipDrawResourceMarking,
                     "scoped-handle replay enables bulk-retention skip");
      check(recorder->events[1].skipDrawResourceMarking,
            "scoped-handle bulk-retention skip enabled");
    }
    checkEventKind(recorder->events, midEventIndex(2u), EventKind::SubmitDraw,
                   "scoped-handle draw submits after retention");
    if (!offloadReplayActive()) {
      checkEventKind(recorder->events, 3u, EventKind::SetSkipDrawResourceMarking,
                     "scoped-handle replay resets bulk-retention skip");
      check(!recorder->events[3].skipDrawResourceMarking,
            "scoped-handle bulk-retention skip disabled");
    }

    const auto& retained = recorder->events[0].chunkHandles;
    checkEq(retained.size(), static_cast<std::size_t>(2),
            "bulk retention keeps only record-scoped pool handles");
    check(containsChunkHandle(retained, ChunkHandleKind::Surface,
                              renderTarget->handle()),
          "scoped retention includes draw render target");
    check(containsChunkHandle(retained, ChunkHandleKind::Buffer,
                              vertexBuffer->handle()),
          "scoped retention includes draw vertex buffer");
    check(!containsChunkHandle(retained, ChunkHandleKind::Surface,
                               unusedSurface->handle()),
          "scoped retention excludes out-of-range surface handle");
    check(!containsChunkHandle(retained, ChunkHandleKind::Buffer,
                               unusedBuffer->handle()),
          "scoped retention excludes out-of-range buffer handle");

    const auto& drawRun = recorder->events[midEventIndex(2u)].drawRun;
    checkEq(drawRun.draws.size(), static_cast<std::size_t>(1),
            "scoped-handle draw run contains one draw");
    checkEq(drawRun.hot.colorAttachments[0].handle.value,
            renderTarget->handle().value,
            "scoped-handle draw uses the record-scoped RT");
    checkEq(drawRun.hot.streamBuffers[0].value, std::uint64_t{0},
            "scoped-handle draw base stream buffer is binding-agnostic");
    checkEq(drawRun.hot.streamOffsets[0], 0u,
            "scoped-handle draw base stream offset is binding-agnostic");
    checkEq(drawRun.hot.streamStrides[0], 0u,
            "scoped-handle draw base stream stride is binding-agnostic");
    checkEq(drawRun.hot.streamMask, 0u,
            "scoped-handle draw base stream mask is binding-agnostic");
    const auto binding = recordedBindingOverride(
        drawRun, drawRun.draws[0].bindingOverrideRange,
        "scoped-handle draw binding override payload size");
    checkEq(binding.streamMask, 1u, "scoped-handle draw binding stream mask");
    checkEq(binding.streams[0].buffer.value, vertexBuffer->handle().value,
            "scoped-handle draw binding stream buffer");
    checkEq(binding.streams[0].offset, 0u,
            "scoped-handle draw binding stream offset");
    checkEq(binding.streams[0].stride, 16u,
            "scoped-handle draw binding stream stride");
    checkEq(drawRun.draws[0].startVertex, 4u,
            "scoped-handle draw keeps start vertex");
    check(drawRun.draws[0].userVertexRange.empty(),
          "scoped-handle draw uses bound vertex buffer, not user vertex payload");
    check(drawRun.draws[0].userIndexRange.empty(),
          "scoped-handle draw has no user index payload");
  }

  checkEq(device->Release(), 0u, "release scoped-handle recording d3d device");
  checkEq(d3d->Release(), 0u, "release scoped-handle recording d3d factory");
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

void testResourcePoolTextureAndSurfaceDestroyWaitForUseSeq() {
  auto* resourcePool = new dxmt9::resources::Pool;
  BackendLimits limits{};

  const auto texture = resourcePool->createTexture(
      WMT::Device{NULL_OBJECT_HANDLE}, limits,
      TextureDesc{
          .width = 8u,
          .height = 4u,
          .depth = 1u,
          .levels = 2u,
          .format = Format::A8R8G8B8,
          .type = TextureType::TwoD,
          .pool = Pool::Managed,
          .usage = UsageTexture,
      });
  check(static_cast<bool>(texture), "resource pool allocates texture handle");
  check(resourcePool->findTexture(texture.value) != nullptr,
        "resource pool finds live texture before destroy");
  check(resourcePool->findBuffer(texture.value) == nullptr,
        "resource pool rejects texture handle as buffer");

  const auto surface = resourcePool->createSurface(
      WMT::Device{NULL_OBJECT_HANDLE}, limits,
      SurfaceDesc{
          .width = 8u,
          .height = 4u,
          .format = Format::A8R8G8B8,
          .pool = Pool::Default,
          .usage = UsageRenderTarget,
          .renderTarget = true,
      });
  check(static_cast<bool>(surface), "resource pool allocates surface handle");
  check(resourcePool->findSurface(surface.value) != nullptr,
        "resource pool finds live surface before destroy");
  check(resourcePool->findTexture(surface.value) == nullptr,
        "resource pool rejects surface handle as texture");

  resourcePool->markTextureUse(texture, 11u);
  resourcePool->markSurfaceUse(surface, 12u);

  check(resourcePool->markTextureDestroyAndGc(texture.value, 10u),
        "resource pool marks texture destroy-pending");
  check(resourcePool->findTexture(texture.value) != nullptr,
        "resource pool keeps pending texture until completed seq catches up");

  check(resourcePool->markSurfaceDestroyAndGc(surface.value, 11u),
        "resource pool marks surface destroy-pending");
  check(resourcePool->findTexture(texture.value) == nullptr,
        "resource pool reclaims texture once completed seq reaches last use");
  check(resourcePool->findSurface(surface.value) != nullptr,
        "resource pool keeps pending surface past lower completed seq");

  resourcePool->reclaimCompleted(12u);
  check(resourcePool->findSurface(surface.value) == nullptr,
        "resource pool reclaims surface once completed seq reaches last use");
}

void testReorderedIndexRejectedCacheTracksSourceRevision() {
  dxmt9::resources::Pool resourcePool;
  const auto source = resourcePool.createBuffer(
      WMT::Device{NULL_OBJECT_HANDLE},
      BufferDesc{
          .size = 64u,
          .pool = Pool::SystemMem,
          .usage = UsageIndexBuffer,
      });
  check(static_cast<bool>(source),
        "resource pool allocates source index buffer handle");

  dxmt9::resources::ReorderedIndexBufferCacheKey key{};
  key.startIndex = 3u;
  key.indexCount = 12u;
  key.indexType = IndexType::UInt16;
  key.order = dxmt9::resources::ReorderedIndexOrder::VertexCacheLru32;
  key.cacheSize = 32u;

  auto miss = resourcePool.findReorderedIndexBuffer(
      source.value, key, /*seqId=*/4u, /*completedSeqId=*/0u);
  check(!miss.hit, "reordered-index cache initially misses");

  check(resourcePool.rememberRejectedReorderedIndexBuffer(
            source.value, key, /*seqId=*/5u, /*completedSeqId=*/0u),
        "reordered-index cache records rejected gain-gate result");

  auto rejected = resourcePool.findReorderedIndexBuffer(
      source.value, key, /*seqId=*/6u, /*completedSeqId=*/0u);
  check(rejected.hit, "reordered-index cache hits rejected key");
  check(rejected.rejected, "reordered-index cache hit is marked rejected");
  check(!rejected.buffer, "rejected reordered-index entry has no Metal buffer");
  checkEq(rejected.byteCount, std::uint64_t{0},
          "rejected reordered-index entry has no byte payload");

  const std::uint8_t bytes[] = {0, 1, 2, 3};
  check(resourcePool.uploadBufferData(source.value, bytes, sizeof(bytes)),
        "source index buffer upload advances content revision");

  auto invalidated = resourcePool.findReorderedIndexBuffer(
      source.value, key, /*seqId=*/7u, /*completedSeqId=*/0u);
  check(!invalidated.hit,
        "source content revision invalidates rejected reordered-index key");

  check(resourcePool.rememberRejectedReorderedIndexBuffer(
            source.value, key, /*seqId=*/8u, /*completedSeqId=*/0u),
        "reordered-index cache can record rejection for new source revision");
  auto rejectedAfterUpload = resourcePool.findReorderedIndexBuffer(
      source.value, key, /*seqId=*/9u, /*completedSeqId=*/0u);
  check(rejectedAfterUpload.hit,
        "reordered-index cache hits rejected key after source revision refresh");
  check(rejectedAfterUpload.rejected,
        "refreshed reordered-index cache hit remains rejected-only");
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
    testReadbackBoundarySplitsCoalescedImportedDrawRun();
    testImportedIndexedDrawPreservesBoundIndexPolicy();
    testImportedSurfaceOpsPreserveBoundaryPayloads();
    testDeviceCSetIndicesInfersIndex32Format();
    testImportedIndexedDrawRunCoalescesParamOnlyPackets();
    testImportedDrawRetainsOnlyRecordScopedHandles();
    testResourcePoolArenaRejectsStaleHandles();
    testHandleArenaSlotPointerStableAcrossInserts();
    testResourcePoolUsesArenaStorageOnly();
    testResourcePoolTextureAndSurfaceDestroyWaitForUseSeq();
    testReorderedIndexRejectedCacheTracksSourceRevision();
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
