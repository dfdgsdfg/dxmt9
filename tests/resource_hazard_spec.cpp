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

struct RecordedEvent {
  EventKind kind = EventKind::Flush;
  bool skipDrawResourceMarking = false;
  std::vector<ChunkHandleEntry> chunkHandles;
  DrawDesc draw;
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

  void submitDraw(const DrawDesc& desc) override {
    RecordedEvent event;
    event.kind = EventKind::SubmitDraw;
    event.draw = desc;
    events.push_back(std::move(event));
  }

  void submitDrawRun(DrawRunDesc desc) override {
    for (const auto& param : desc.draws) {
      DrawDesc synthetic = desc.state.desc;
      synthetic.primitiveType = param.primitiveType;
      synthetic.primitiveCount = param.primitiveCount;
      synthetic.startVertex = param.startVertex;
      synthetic.baseVertexIndex = param.baseVertexIndex;
      synthetic.startIndex = param.startIndex;
      synthetic.indexType = param.indexType;
      synthetic.userVertexData = param.userVertexData;
      synthetic.userIndexData = param.userIndexData;
      submitDraw(synthetic);
    }
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

    std::vector<std::uint8_t> bytes;
    appendRecord(bytes, makeDrawRecord(&renderTargetWire, &vertexBufferWire, 0u));
    appendRecord(bytes, makeColorClearRecord());
    appendRecord(bytes, makeDrawRecord(&renderTargetWire, &vertexBufferWire, 3u));
    appendRecord(bytes, makeReadbackRecord(&renderTargetWire, &readbackTargetWire));

    const D9CChunkHandleEntry handles[] = {
        {D9C_CHUNK_HANDLE_KIND_SURFACE, 0u, wireValueFromPtr(&renderTargetWire)},
        {D9C_CHUNK_HANDLE_KIND_BUFFER, 0u, wireValueFromPtr(&vertexBufferWire)},
        {D9C_CHUNK_HANDLE_KIND_SURFACE, 0u, wireValueFromPtr(&readbackTargetWire)},
    };

    D9CCommandChunk chunk{};
    chunk.version = D9C_COMMAND_CHUNK_VERSION;
    chunk.recordCount = 4u;
    chunk.recordBytes = static_cast<std::uint32_t>(bytes.size());
    chunk.records = wireHandleFromPtr(bytes.data());
    chunk.handleCount = static_cast<std::uint32_t>(std::size(handles));
    chunk.handles = wireHandleFromPtr(handles);

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

    checkEq(recorder->events[2].draw.rts.color[0].handle.value,
            renderTarget->handle().value, "first draw observes imported RT state");
    check(recorder->events[2].draw.vertexDecl.streams[0].buffer == vertexBuffer,
          "first draw observes imported stream state");
    checkEq(recorder->events[4].draw.startVertex, 3u,
            "second draw payload remains in record order");
    checkEq(recorder->events[5].readback.source.value, renderTarget->handle().value,
            "readback source handle");
    checkEq(recorder->events[5].readback.destination.value,
            readbackTarget->handle().value, "readback destination handle");
  }

  checkEq(device->Release(), 0u, "release recording d3d device");
  checkEq(d3d->Release(), 0u, "release recording d3d factory");
}

}  // namespace

int main() {
  try {
    testImportedChunkBulkRetentionAndBarrierOrdering();
  } catch (const TestFailure& e) {
    std::cerr << "resource_hazard_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::cerr << "resource_hazard_spec unexpected exception: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
