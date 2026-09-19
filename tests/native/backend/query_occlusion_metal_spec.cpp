// R-CORE-8.2(a): production core -> queue -> encoder -> actual Metal bytes.
#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_device.hpp"
#include "../../../src/dxmt9/dxmt9_perf_counters.hpp"
#include "../../../src/dxmt9/render/session_source_policy.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace dxmt9::core;
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
struct Gate {
  WMT::Reference<WMT::SharedEvent> event;
  ~Gate() { if (event) event.signalValue(1); }
};
void run() {
  Factory factory(BackendLimits{});
  PresentParameters params{};
  params.backBufferWidth = params.backBufferHeight = 32;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.enableAutoDepthStencil = true;
  params.autoDepthStencilFormat = Format::D24S8;
  auto device = factory.createDevice(0, params);
  check(bool(device), "create real core device");
  check(device->upperDevice()->usesMetalOcclusion(), "actual GPU provider enabled");
  auto& queue = device->upperDevice()->queue();
  check(queue.valid() && queue.started(), "real Metal queue");
  const auto set = [&](u32 state, u32 value) {
    check(device->setRenderState(state, value) == D3D_OK, "set state");
  };
  check(device->setFVF(0x4002u | 0x040u) == D3D_OK, "XYZW diffuse");
  set(RS_CULL_MODE, u32(CullMode::None));
  set(RS_LIGHTING, 0);
  set(RS_Z_ENABLE, 1);
  set(RS_Z_WRITE_ENABLE, 0);
  set(RS_Z_FUNC, u32(CompareFunc::Less));
  check(device->setViewport({0, 0, 32, 32, 0, 1}) == D3D_OK, "viewport");
  struct Vertex { float x, y, z, w; u32 color; };
  // Extend beyond the viewport: D3D9's half-pixel translation must not put
  // viewport edge samples exactly on a triangle's rasterization boundary.
  const std::array<Vertex, 6> vertices{{
      {-2,-2,.5f,1,0xff00ff00}, {-2,2,.5f,1,0xff00ff00}, {2,-2,.5f,1,0xff00ff00},
      {2,-2,.5f,1,0xff00ff00}, {-2,2,.5f,1,0xff00ff00}, {2,2,.5f,1,0xff00ff00}}};
  auto vb = device->createBuffer({sizeof(vertices), Pool::Default, UsageVertexBuffer});
  check(bool(vb), "VB");
  auto mapping = vb->lock(0, sizeof(vertices), 0);
  check(mapping.data, "map VB");
  std::memcpy(mapping.data, vertices.data(), sizeof(vertices));
  vb->unlock();
  check(device->setStreamSource(0, vb, 0, sizeof(Vertex)) == D3D_OK, "stream");
  const auto draw = [&] {
    check(device->drawPrimitive(PrimitiveType::TriangleList, 2) == D3D_OK, "draw");
  };
  const auto clear = [&] {
    ClearDesc desc{};
    desc.clearColor = desc.clearDepth = true;
    desc.color = {1,0,0,1};
    desc.depth = .25f;
    check(device->clear(desc) == D3D_OK, "clear");
  };
  const auto issue = [&](const auto& q, bool begin) {
    check(device->issueQuery(q, begin) == D3D_OK, "issue query");
  };
  const auto result = [&](const auto& q) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    u32 samples = 0xdeadbeef;
    HResult status = S_FALSE;
    while (status == S_FALSE && std::chrono::steady_clock::now() < deadline) {
      status = device->getQueryData(q, &samples, sizeof(samples), QUERY_GETDATA_FLUSH);
      if (status == S_FALSE) {
        check(samples == 0xdeadbeef, "pending output unchanged");
        std::this_thread::yield();
      }
    }
    check(status == S_OK, "actual query completion without readback rescue");
    return samples;
  };
  auto a = device->createQuery(QueryType::Occlusion);
  auto b = device->createQuery(QueryType::Occlusion);
  const auto storesBefore = dxmt9::perf::test::snapshotRenderPassStoreAccounting();
  // A source boundary is not the death of an attachment. BEGIN publishes the
  // preceding unqueried writer; END publishes a query-only depth reader. Both
  // must preserve depth for later query windows even without a Present tail.
  ClearDesc initialDepth{};
  initialDepth.clearColor = initialDepth.clearDepth = true;
  initialDepth.depth = 1.0f;
  check(device->clear(initialDepth) == D3D_OK, "initial query depth");
  set(RS_Z_WRITE_ENABLE, 1);
  draw(); // writes .5 depth before BEGIN's source cut
  set(RS_Z_WRITE_ENABLE, 0);
  set(RS_Z_FUNC, u32(CompareFunc::LessEqual));
  set(RS_COLOR_WRITE_ENABLE, 0);
  for (unsigned i = 0; i != 3; ++i) {
    issue(a, true); draw(); issue(a, false);
    const auto retained = result(a);
    std::cout << "retained-depth samples=" << retained << '\n';
    check(retained == 1024, "query source boundaries preserve prior depth contents");
  }
  const auto storesAfter = dxmt9::perf::test::snapshotRenderPassStoreAccounting();
  // DontCare contents can survive by accident on a particular GPU. Check the
  // real descriptor-accounting seam too, not only fortunate retained bytes.
  check(storesAfter.depthStore >= storesBefore.depthStore + 4 &&
            storesAfter.depthDontCare == storesBefore.depthDontCare &&
            storesAfter.stencilDontCare == storesBefore.stencilDontCare &&
            storesAfter.colorDontCare == storesBefore.colorDontCare,
        "unqueried predecessor and queried readers use preserving Store actions");
  set(RS_COLOR_WRITE_ENABLE, 15);
  set(RS_Z_FUNC, u32(CompareFunc::Less));
  clear();
  issue(a, true); draw(); issue(a, false);
  check(result(a) == 0, "fully depth-occluded primitives return zero");
  set(RS_Z_ENABLE, 0);
  issue(a, true); draw(); issue(a, false);
  const auto full = result(a);
  std::cout << "full-screen samples=" << full << '\n';
  check(full == 1024, "visible query counts pixels, not two primitives");
  set(RS_COLOR_WRITE_ENABLE, 0);
  issue(a, true); draw(); issue(a, false);
  check(result(a) == full, "color/depth-write-disabled query still counts visibility");
  set(RS_COLOR_WRITE_ENABLE, 15);
  issue(a, true); draw(); draw(); issue(a, false);
  check(result(a) == 2 * full,
        "one visibility offset accumulates consecutive draws in one pass");
  issue(a, true);
  check(device->drawPrimitiveUP(PrimitiveType::TriangleList, 2,
      std::span(reinterpret_cast<const u8*>(vertices.data()), sizeof(vertices)),
      sizeof(Vertex)) == D3D_OK, "UP visibility draw");
  const std::array<u16, 6> indices{0,1,2,3,4,5};
  check(device->drawIndexedPrimitiveUP(PrimitiveType::TriangleList, 2,
      std::span(reinterpret_cast<const u8*>(vertices.data()), sizeof(vertices)),
      std::span(reinterpret_cast<const u8*>(indices.data()), sizeof(indices)),
      IndexType::UInt16, sizeof(Vertex)) == D3D_OK, "indexed UP visibility draw");
  issue(a, false);
  check(result(a) == 2 * full, "indexed and nonindexed UP contributions");
  check(device->setStreamSource(0, vb, 0, sizeof(Vertex)) == D3D_OK,
        "restore stream after UP draws");
  auto readback = device->createSurface(
      {32, 32, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
  check(device->getRenderTargetData(device->swapChain()->backBuffer(), readback) == D3D_OK,
        "readback only after completed result");
  const auto pixels = readback->lockRect(nullptr, 0);
  check(pixels.data && pixels.pitch >= 128, "readback bytes");
  for (unsigned y = 0; y != 32; ++y)
    for (unsigned x = 0; x != 32; ++x) {
      const auto* p = static_cast<const u8*>(pixels.data) + y * pixels.pitch + x * 4;
      check(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255,
            "query counting preserves exact green framebuffer");
    }
  readback->unlockRect();

  // Simultaneous scopes, separate pass contributions and publication cuts.
  Gate gate{queue.device().newSharedEvent()};
  auto blocker = queue.newCommandBuffer();
  blocker.encodeWaitForEvent(WMT::Event{gate.event.handle}, 1);
  blocker.commit();
  issue(a, true); draw();
  issue(b, true); draw(); clear(); draw();
  check(queue.publishQueryEnd().valid(), "source cut preserves active scope");
  draw(); issue(a, false); draw(); issue(b, false);
  u32 pending = 123;
  for (unsigned i = 0; i != 4; ++i)
    check(device->getQueryData(a, &pending, sizeof(pending), QUERY_GETDATA_FLUSH) == S_FALSE &&
              pending == 123, "GPU gate keeps real result pending");
  gate.event.signalValue(1);
  check(result(a) == 4 * full, "A scope sums four draws across passes/sources");
  check(result(b) == 4 * full, "B overlap receives same shared contributions");
  issue(a, false);
  check(result(a) == 4 * full, "repeated END preserves previous result");
  issue(a, true); issue(a, true); issue(a, false);
  check(result(a) == 0, "empty/repeated BEGIN replaces generation");

  set(RS_ALPHA_TEST_ENABLE, 1);
  set(RS_ALPHA_FUNC, u32(CompareFunc::Greater));
  set(RS_ALPHA_REF, 255);
  issue(a, true); draw(); issue(a, false);
  check(result(a) == 0, "alpha-rejected fragments excluded, including forced tile policy");
  set(RS_ALPHA_TEST_ENABLE, 0);
  auto released = device->createQuery(QueryType::Occlusion);
  issue(released, true); draw(); released.reset();
  issue(a, true); draw(); issue(a, false);
  check(result(a) == full, "release without END does not corrupt next scope");
  issue(b, true); draw();
  check(device->reset(params) == D3D_OK, "reset active query");
  check(device->getQueryData(b, &pending, sizeof(pending), 0) == D3DERR_DEVICELOST,
        "old query epoch fails after reset");

  // Admission isolation uses the same production predicates as sessions.
  ChunkSlot slot{};
  slot.appendClear(ClearDesc{});
  auto scope = std::make_shared<dxmt9::query::OcclusionScope>();
  scope->results[0] = std::make_shared<dxmt9::query::OcclusionResult>(1, 1, 1);
  scope->count = 1;
  slot.occlusionScope = scope;
  check(!dxmt9::render::sessionSourceCanBeHead(SourcePayloadView(slot)) &&
            !dxmt9::render::sessionSourceCanAppendToPending(SourcePayloadView(slot), true),
        "scoped source cannot reorder across an open session");
}

bool snapshotModeEnabled() {
  // The second Meson pair sets the candidate's existing opt-in gate. Keep the
  // oracle in this executable so tile-off and tile-force use identical bytes.
  const auto* value = std::getenv("DXMT9_QUERY_POLL_RECEIPTS");
  return value && value[0] && value[0] != '0';
}

void runSnapshotPollOracle() {
  Factory factory(BackendLimits{});
  PresentParameters params{};
  params.backBufferWidth = params.backBufferHeight = 32;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.enableAutoDepthStencil = true;
  params.autoDepthStencilFormat = Format::D24S8;
  auto device = factory.createDevice(0, params);
  check(bool(device), "create real core device for snapshot polling");
  check(device->upperDevice()->usesMetalOcclusion(),
        "snapshot polling requires actual GPU provider");
  auto& queue = device->upperDevice()->queue();
  check(queue.valid() && queue.started(), "snapshot polling Metal queue");

  const auto set = [&](u32 state, u32 value) {
    check(device->setRenderState(state, value) == D3D_OK,
          "snapshot polling render state");
  };
  check(device->setFVF(0x4002u | 0x040u) == D3D_OK,
        "snapshot polling XYZW diffuse");
  set(RS_CULL_MODE, u32(CullMode::None));
  set(RS_LIGHTING, 0);
  set(RS_Z_ENABLE, 1);
  set(RS_Z_WRITE_ENABLE, 0);
  set(RS_Z_FUNC, u32(CompareFunc::Less));
  check(device->setViewport({0, 0, 32, 32, 0, 1}) == D3D_OK,
        "snapshot polling viewport");

  struct Vertex { float x, y, z, w; u32 color; };
  const std::array<Vertex, 6> vertices{{
      {-2, -2, .5f, 1, 0xff00ff00}, {-2, 2, .5f, 1, 0xff00ff00},
      {2, -2, .5f, 1, 0xff00ff00},  {2, -2, .5f, 1, 0xff00ff00},
      {-2, 2, .5f, 1, 0xff00ff00},  {2, 2, .5f, 1, 0xff00ff00}}};
  auto vb = device->createBuffer({sizeof(vertices), Pool::Default,
                                  UsageVertexBuffer});
  check(bool(vb), "snapshot polling VB");
  auto mapping = vb->lock(0, sizeof(vertices), 0);
  check(mapping.data, "snapshot polling map VB");
  std::memcpy(mapping.data, vertices.data(), sizeof(vertices));
  vb->unlock();
  check(device->setStreamSource(0, vb, 0, sizeof(Vertex)) == D3D_OK,
        "snapshot polling stream");
  const auto draw = [&] {
    check(device->drawPrimitive(PrimitiveType::TriangleList, 2) == D3D_OK,
          "snapshot polling draw");
  };
  const auto clear = [&](bool color, f32 depth) {
    ClearDesc desc{};
    desc.clearColor = color;
    desc.clearDepth = true;
    desc.color = {1, 0, 0, 1};
    desc.depth = depth;
    check(device->clear(desc) == D3D_OK, "snapshot polling clear");
  };
  const auto issue = [&](const std::shared_ptr<Query>& query, bool begin) {
    check(device->issueQuery(query, begin) == D3D_OK,
          "snapshot polling issue");
  };
  const auto pollSnapshot = [&](const QueryPollSnapshot& snapshot,
                                u32 expected, u32 flags = QUERY_GETDATA_FLUSH) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(10);
    u32 value = 0xdeadbeefu;
    HResult status = S_FALSE;
    while (status == S_FALSE && std::chrono::steady_clock::now() < deadline) {
      status = device->pollQuerySnapshot(snapshot, &value, sizeof(value), flags);
      if (status == S_FALSE) {
        check(value == 0xdeadbeefu,
              "snapshot pending poll preserves output sentinel");
        std::this_thread::yield();
      }
    }
    check(status == S_OK, "snapshot poll completed without readback rescue");
    check(value == expected, "snapshot poll result");
  };
  const auto pollLegacy = [&](const std::shared_ptr<Query>& query,
                              u32 expected, u32 flags = QUERY_GETDATA_FLUSH) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(10);
    u32 value = 0xdeadbeefu;
    HResult status = S_FALSE;
    while (status == S_FALSE && std::chrono::steady_clock::now() < deadline) {
      status = device->getQueryData(query, &value, sizeof(value), flags);
      if (status == S_FALSE) {
        check(value == 0xdeadbeefu,
              "legacy pending poll preserves output sentinel");
        std::this_thread::yield();
      }
    }
    check(status == S_OK, "legacy poll completed without readback rescue");
    check(value == expected, "legacy poll result");
  };

  clear(true, 1.0f);

  // A real blocked command buffer proves both APIs observe the actual END
  // receipt. Neither poll may borrow a newer mutable Query generation or
  // overwrite caller bytes while that prefix is pending.
  Gate gate{queue.device().newSharedEvent()};
  auto blocker = queue.newCommandBuffer();
  blocker.encodeWaitForEvent(WMT::Event{gate.event.handle}, 1);
  blocker.commit();
  auto pendingQuery = device->createQuery(QueryType::Occlusion);
  issue(pendingQuery, true);
  draw();
  issue(pendingQuery, false);
  const auto pendingSnapshot = device->captureQueryPollSnapshot(pendingQuery);
  u32 snapshotPending = 0xdeadbeefu;
  u32 legacyPending = 0xdeadbeefu;
  check(device->pollQuerySnapshot(pendingSnapshot, &snapshotPending,
                                  sizeof(snapshotPending), 0) == S_FALSE &&
            snapshotPending == 0xdeadbeefu,
        "snapshot pending result preserves sentinel");
  check(device->getQueryData(pendingQuery, &legacyPending,
                             sizeof(legacyPending), 0) == S_FALSE &&
            legacyPending == 0xdeadbeefu,
        "legacy pending result preserves sentinel");
  gate.event.signalValue(1);
  pollSnapshot(pendingSnapshot, 1024);
  pollLegacy(pendingQuery, 1024);

  // Reissuing replaces the public Query's receipt, but the old snapshot owns
  // the old result and fence. Releasing the public object must not redirect
  // that immutable receipt to the new zero-sample generation.
  auto oldSnapshot = pendingSnapshot;
  issue(pendingQuery, true);
  clear(false, .25f);
  draw();
  issue(pendingQuery, false);
  const auto zeroSnapshot = device->captureQueryPollSnapshot(pendingQuery);
  pollSnapshot(zeroSnapshot, 0);
  pollLegacy(pendingQuery, 0);
  pendingQuery.reset();
  pollSnapshot(oldSnapshot, 1024);

  // The exact framebuffer remains the existing green fixture after all
  // snapshot and legacy polls; query result bytes must not imply a readback.
  auto readback = device->createSurface(
      {32, 32, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
  check(device->getRenderTargetData(device->swapChain()->backBuffer(), readback) ==
            D3D_OK,
        "snapshot polling framebuffer readback");
  const auto pixels = readback->lockRect(nullptr, 0);
  check(pixels.data && pixels.pitch >= 128,
        "snapshot polling framebuffer bytes");
  for (unsigned y = 0; y != 32; ++y)
    for (unsigned x = 0; x != 32; ++x) {
      const auto* p = static_cast<const u8*>(pixels.data) + y * pixels.pitch + x * 4;
      check(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255,
            "snapshot polling preserves exact green framebuffer");
    }
  readback->unlockRect();

  // Reset advances the query epoch. A captured receipt from the old epoch
  // must fail closed and preserve the caller's bytes, just like legacy data.
  auto resetQuery = device->createQuery(QueryType::Occlusion);
  issue(resetQuery, true);
  draw();
  issue(resetQuery, false);
  const auto resetSnapshot = device->captureQueryPollSnapshot(resetQuery);
  check(device->reset(params) == D3D_OK, "snapshot polling reset");
  u32 resetSnapshotValue = 0xdeadbeefu;
  u32 resetLegacyValue = 0xdeadbeefu;
  check(device->pollQuerySnapshot(resetSnapshot, &resetSnapshotValue,
                                  sizeof(resetSnapshotValue), 0) ==
            D3DERR_DEVICELOST &&
            resetSnapshotValue == 0xdeadbeefu,
        "reset invalidates immutable snapshot receipt");
  check(device->getQueryData(resetQuery, &resetLegacyValue,
                             sizeof(resetLegacyValue), 0) == D3DERR_DEVICELOST &&
            resetLegacyValue == 0xdeadbeefu,
        "reset invalidates legacy query receipt");
}
} // namespace

int main() {
  try {
    if (snapshotModeEnabled()) {
      runSnapshotPollOracle();
      std::cout << "query occlusion Metal snapshot oracle passed\n";
    } else {
      run();
      std::cout << "query occlusion Metal oracle passed\n";
    }
    return 0;
  }
  catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
