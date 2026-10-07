// R-CORE-8.2(a): production core -> queue -> encoder -> actual Metal bytes.
#include "dxmt9/core.hpp"
#include "../../../src/dxmt9/dxmt9_device.hpp"
#include "../../../src/dxmt9/dxmt9_perf_counters.hpp"
#include "../../../src/dxmt9/dxmt9_query_policy.hpp"
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
void run(bool benchmark = false, MultiSampleType samples = MultiSampleType::None) {
  Factory factory(BackendLimits{});
  PresentParameters params{};
  params.backBufferWidth = params.backBufferHeight = 32;
  params.backBufferFormat = Format::A8R8G8B8;
  params.windowed = true;
  params.enableAutoDepthStencil = true;
  params.autoDepthStencilFormat = Format::D24S8;
  params.multiSampleType = samples;
  auto device = factory.createDevice(0, params);
  check(bool(device), "create real core device");
  check(device->upperDevice()->usesMetalOcclusion(), "actual GPU provider enabled");
  auto& queue = device->upperDevice()->queue();
  check(queue.valid() && queue.started(), "real Metal queue");
  const auto sampleTotal = samples == MultiSampleType::None ? 1u : u32(samples);
  const auto expectedFull = 1024u * sampleTotal;
  if (sampleTotal > 1) {
    check(queue.device().supportsTextureSampleCount(sampleTotal), "requested MSAA supported");
    const auto* color = queue.pool().findSurface(device->swapChain()->backBuffer()->handle().value);
    const auto* depth = queue.pool().findSurface(device->swapChain()->depthStencilSurface()->handle().value);
    check(color && depth && color->desc.multiSampleType == samples &&
              depth->desc.multiSampleType == samples && color->resolveTexture &&
              WMT::Texture{color->texture.handle}.textureType() == WMTTextureType2DMultisample &&
              WMT::Texture{depth->texture.handle}.textureType() == WMTTextureType2DMultisample &&
              WMT::Texture{color->resolveTexture.handle}.textureType() == WMTTextureType2D,
          "oracle uses actual multisample color/depth and a separate resolve texture");
  }
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
    check(retained == expectedFull, "query source boundaries preserve prior depth samples");
  }
  const auto storesAfter = dxmt9::perf::test::snapshotRenderPassStoreAccounting();
  // DontCare contents can survive by accident on a particular GPU. Check the
  // real descriptor-accounting seam too, not only fortunate retained bytes.
  check(benchmark || (storesAfter.depthStore >= storesBefore.depthStore + 4 &&
            storesAfter.depthDontCare == storesBefore.depthDontCare &&
            storesAfter.stencilDontCare == storesBefore.stencilDontCare &&
            storesAfter.colorDontCare == storesBefore.colorDontCare),
        "unqueried predecessor and queried readers use preserving Store actions");
  set(RS_COLOR_WRITE_ENABLE, 15);
  set(RS_Z_FUNC, u32(CompareFunc::Less));
  clear();
  issue(a, true); draw(); issue(a, false);
  check(result(a) == 0, "fully depth-occluded primitives return zero");
  set(RS_Z_ENABLE, 0);
  issue(a, true); draw(); issue(a, false);
  const auto full = result(a);
  std::cout << "full-screen sample-count=" << sampleTotal << " samples=" << full << '\n';
  check(full == expectedFull, "visible query counts all samples, not primitives or resolved pixels");
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
  const auto readCompletedColor = [&] {
    // Core readback selects the pass's resolveTexture for MSAA. Do not insert
    // a separate StretchRect pass that could mask a missing query-pass resolve.
    return device->getRenderTargetData(device->swapChain()->backBuffer(), readback);
  };
  check(readCompletedColor() == D3D_OK,
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

  if (!benchmark) {
    // Keep this separate from the direct query-pass readback above: a stretch
    // pass must not repair or mask a missing resolve in that oracle.
    auto stretched = device->createSurface(
        {16, 16, Format::A8R8G8B8, Pool::Default, UsageRenderTarget, true, false});
    auto stretchReadback = device->createSurface(
        {16, 16, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
    check(bool(stretched) && bool(stretchReadback), "stretch oracle surfaces");
    for (const bool linear : {false, true}) {
      // Scaling excludes the raw texture-copy shortcut even at sample count 1.
      check(device->stretchRect(device->swapChain()->backBuffer(), nullptr,
                                stretched, nullptr, linear) == D3D_OK,
            "explicit filtered stretch after query completion");
      check(device->getRenderTargetData(stretched, stretchReadback) == D3D_OK,
            "read completed stretch destination");
      const auto stretchedPixels = stretchReadback->lockRect(nullptr, 0);
      check(stretchedPixels.data && stretchedPixels.pitch >= 64, "stretch readback bytes");
      for (unsigned y = 0; y != 16; ++y)
        for (unsigned x = 0; x != 16; ++x) {
          const auto* p = static_cast<const u8*>(stretchedPixels.data) +
                          y * stretchedPixels.pitch + x * 4;
          check(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255,
                "nearest/linear stretch preserves exact green samples");
        }
      stretchReadback->unlockRect();
      std::cout << "stretch sample-count=" << sampleTotal << " linear=" << linear
                << " exact-green passed\n";
    }
  }

  if (benchmark) {
    std::array<std::shared_ptr<Query>, 8> queries;
    for (auto& q : queries) q = device->createQuery(QueryType::Occlusion);
    using Clock = std::chrono::steady_clock;
    for (int iteration = -2; iteration != 16; ++iteration) {
      const auto before = queue.queryProgress();
      std::optional<dxmt9::perf::RenderTapeReplaySnapshot> countersBefore;
      if (dxmt9::perf::enabled()) countersBefore = dxmt9::perf::snapshotRenderTapeReplay();
      const auto start = Clock::now();
      for (const auto& q : queries) {
        draw(); // unqueried predecessor: same serial stream in both policies
        issue(q, true); draw(); issue(q, false);
      }
      const auto prepared = Clock::now();
      for (const auto& q : queries) check(result(q) == full, "burst query exact samples");
      const auto done = Clock::now();
      const auto after = queue.queryProgress();
      if (iteration >= 0)
        std::cout << "query-burst sample=" << iteration
                  << " ranges=" << dxmt9::query::scopeRangesEnabled()
                  << " prepare_us=" << std::chrono::duration<double, std::micro>(prepared-start).count()
                  << " completion_us=" << std::chrono::duration<double, std::micro>(done-prepared).count()
                  << " sources=" << after.published-before.published << '\n';
      if (iteration >= 0 && countersBefore) {
        const auto countersAfter = dxmt9::perf::snapshotRenderTapeReplay();
        std::cout << "query-burst-cpu sample=" << iteration
                  << " publication_us=" << double(countersAfter.prepareSlotWallNs -
                      countersBefore->prepareSlotWallNs) / 1000
                  << " encode_us=" << double(countersAfter.encodeChunkWallNs -
                      countersBefore->encodeChunkWallNs) / 1000 << '\n';
      }
      if (dxmt9::query::scopeRangesEnabled())
        check(after.published-before.published == 1, "range burst publishes one actual source");
      else
        check(after.published-before.published == 16, "reference burst publishes sixteen sources");
    }
    check(readCompletedColor() == D3D_OK,
          "burst final readback");
    const auto finalPixels = readback->lockRect(nullptr, 0);
    check(finalPixels.data && finalPixels.pitch >= 128, "burst final bytes");
    for (unsigned y = 0; y != 32; ++y)
      for (unsigned x = 0; x != 32; ++x) {
        const auto* p = static_cast<const u8*>(finalPixels.data) + y * finalPixels.pitch + x * 4;
        check(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255,
              "burst final exact green framebuffer");
      }
    readback->unlockRect();
    return;
  }

  // An unqueried stencil writer must survive BEGIN, and query-only readers
  // must preserve both stencil and the prior color through subsequent ENDs.
  // Vary the reference against one written value, including the original
  // clear value. A broken constant reference must not pass both writer/reader.
  const auto stencilStoresBefore = dxmt9::perf::test::snapshotRenderPassStoreAccounting();
  ClearDesc stencilInitial{};
  stencilInitial.clearColor = stencilInitial.clearStencil = true;
  stencilInitial.color = {1, 0, 1, 1};
  stencilInitial.stencil = 0x33;
  check(device->clear(stencilInitial) == D3D_OK, "initial stencil and magenta color");
  set(RS_Z_ENABLE, 0);
  set(RS_Z_WRITE_ENABLE, 0);
  set(RS_ALPHA_TEST_ENABLE, 0);
  set(RS_COLOR_WRITE_ENABLE, 0);
  set(RS_STENCIL_ENABLE, 1);
  set(RS_TWO_SIDED_STENCIL_MODE, 0);
  set(RS_STENCIL_MASK, 0xff);
  set(RS_STENCIL_WRITEMASK, 0xff);
  set(RS_STENCIL_REF, 0x5a);
  set(RS_STENCIL_FUNC, u32(CompareFunc::Always));
  set(RS_STENCIL_FAIL, u32(StencilOp::Keep));
  set(RS_STENCIL_ZFAIL, u32(StencilOp::Keep));
  set(RS_STENCIL_PASS, u32(StencilOp::Replace));
  draw();
  set(RS_STENCIL_PASS, u32(StencilOp::Keep));
  set(RS_STENCIL_WRITEMASK, 0);
  for (unsigned i = 0; i != 3; ++i) {
    const bool accept = i != 1;
    set(RS_STENCIL_FUNC, u32(CompareFunc::Equal));
    set(RS_STENCIL_REF, accept ? 0x5a : 0x33);
    issue(a, true); draw(); issue(a, false);
    const auto samples = result(a);
    std::cout << "retained-stencil reader=" << i << " samples=" << samples << '\n';
    check(samples == (accept ? full : 0),
          "query cuts preserve stencil accept/reject/accept without rewriting it");
  }
  const auto stencilStoresAfter = dxmt9::perf::test::snapshotRenderPassStoreAccounting();
  check(stencilStoresAfter.stencilStore >= stencilStoresBefore.stencilStore + 4 &&
            stencilStoresAfter.colorStore >= stencilStoresBefore.colorStore + 4 &&
            stencilStoresAfter.stencilDontCare == stencilStoresBefore.stencilDontCare &&
            stencilStoresAfter.colorDontCare == stencilStoresBefore.colorDontCare,
        "unqueried stencil writer and readers use preserving Store actions");
  check(readCompletedColor() == D3D_OK,
        "read prior color after completed stencil queries");
  const auto preservedPixels = readback->lockRect(nullptr, 0);
  check(preservedPixels.data && preservedPixels.pitch >= 128, "preserved color bytes");
  for (unsigned y = 0; y != 32; ++y)
    for (unsigned x = 0; x != 32; ++x) {
      const auto* p = static_cast<const u8*>(preservedPixels.data) +
          y * preservedPixels.pitch + x * 4;
      check(p[0] == 255 && p[1] == 0 && p[2] == 255 && p[3] == 255,
            "query-only stencil windows preserve exact prior magenta pixels");
    }
  readback->unlockRect();
  std::cout << "query-cut stencil/color oracle passed\n";
  set(RS_STENCIL_ENABLE, 0);
  set(RS_STENCIL_WRITEMASK, 0xff);
  set(RS_COLOR_WRITE_ENABLE, 15);

  // Simultaneous scopes, separate pass contributions and publication cuts.
  // Readback above has drained all prior sources. Every source published in
  // this interval is scoped, so an appended head proves queried CB sharing.
  const auto joinedBefore = dxmt9::perf::test::cpuReadySessionHeadAppended();
  const auto passesBefore = dxmt9::perf::snapshot().renderPassBegin;
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
  check(dxmt9::perf::snapshot().renderPassBegin - passesBefore == 5,
        "CB composition preserves all five independent visibility passes");
  if (dxmt9::query::scopeCbJoinEnabled()) {
    const auto joined = dxmt9::perf::test::cpuReadySessionHeadAppended() - joinedBefore;
    std::cout << "query-window appended sources=" << joined << '\n';
    check(joined != 0, "GPU oracle must exercise actual source CB composition");
  }
  check(readCompletedColor() == D3D_OK,
        "read back the actually composed query window");
  const auto joinedPixels = readback->lockRect(nullptr, 0);
  check(joinedPixels.data && joinedPixels.pitch >= 128, "composed readback bytes");
  for (unsigned y = 0; y != 32; ++y)
    for (unsigned x = 0; x != 32; ++x) {
      const auto* p = static_cast<const u8*>(joinedPixels.data) +
          y * joinedPixels.pitch + x * 4;
      check(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255,
            "actually composed query window preserves exact green pixels");
    }
  readback->unlockRect();
  issue(a, false);
  check(result(a) == 4 * full, "repeated END preserves previous result");
  if (dxmt9::query::scopeRangesEnabled()) {
    if (dxmt9::query::coalesceEndsEnabled()) {
      auto event = device->createQuery(QueryType::Event);
      draw(); issue(event, false);
      issue(a, true); draw(); issue(a, false);
      check(result(a) == full, "range owner after ordinary deferred END");
      (void)result(event); // Original marker must bind, not be overwritten.
    }
    // More issue generations than one owner can retain. Capacity must publish
    // a complete prefix and retry the next membership, not drop query results.
    std::vector<std::shared_ptr<Query>> many;
    const auto before = queue.queryProgress().published;
    for (unsigned i = 0; i != 70; ++i) {
      auto q = device->createQuery(QueryType::Occlusion);
      issue(q, true); draw(); issue(q, false);
      many.push_back(std::move(q));
    }
    for (const auto& q : many) check(result(q) == full, "range capacity exact query result");
    const auto sources = queue.queryProgress().published - before;
    check(sources >= 2 && sources < 70, "bounded scope table seals and continues");
  }
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
  const bool join = dxmt9::query::scopeCbJoinEnabled();
  check(dxmt9::render::sessionSourceCanBeHead(SourcePayloadView(slot)) == join &&
            dxmt9::render::sessionSourceCanAppendToPending(SourcePayloadView(slot), true) == join,
        "scoped FIFO CB admission follows the selected policy");
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

int main(int argc, char** argv) {
  try {
    // Test-only oracle selection, independent of production runtime policy.
    check(argc == 1 || (argc == 2 &&
          (std::strcmp(argv[1], "--snapshot-oracle") == 0 ||
           std::strcmp(argv[1], "--range-benchmark") == 0)),
          "unknown oracle argument");
    if (argc == 2 && std::strcmp(argv[1], "--range-benchmark") == 0) {
      run(true);
    } else if (argc == 2) {
      runSnapshotPollOracle();
      std::cout << "query occlusion Metal snapshot oracle passed\n";
    } else {
      run();
      run(false, MultiSampleType::Two);
      run(false, MultiSampleType::Four);
      std::cout << "query occlusion Metal oracle passed\n";
    }
    return 0;
  }
  catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
