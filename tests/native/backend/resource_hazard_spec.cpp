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
    testDeviceCSetIndicesInfersIndex32Format();
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
