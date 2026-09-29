// Production wire/import/replay oracle: do not replace these records with
// direct core Query calls, which bypass ordered-control session release.
#include "device_c_common.hpp"
#include "d3d9_pe_chunk_builder.hpp"
#include "dxmt9/dxmt9_device.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
using namespace dxmt9::core;
namespace pe = dxmt9::d3d9::pe;

void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

struct Fixture {
  dxmt9::com::IDirect3D9Ex* factory = nullptr;
  dxmt9::com::IDirect3DDevice9Ex* iface = nullptr;
  std::unique_ptr<D9CDevice> wire;
  D9CQuery* query = nullptr;

  Fixture() {
    auto devices = WMT::CopyAllDevices();
    check(devices && devices.count() != 0, "Metal device");
    auto upper = dxmt9::CreateDXMT9Device(
        dxmt9::DEVICE_DESC{.device = devices.object(0u)});
    check(bool(upper), "Metal upper runtime");
    factory = dxmt9::com::Direct3DCreate9Ex(
        dxmt9::com::D3D_SDK_VERSION, std::move(upper));
    check(factory, "factory");
    PresentParameters params{};
    params.backBufferWidth = params.backBufferHeight = 32;
    params.backBufferFormat = Format::A8R8G8B8;
    params.windowed = true;
    iface = factory->CreateDeviceEx(0, params, nullptr);
    check(iface, "device");
    iface->AddRef();
    wire = std::make_unique<D9CDevice>(iface);
    query = dxmt9c_device_create_query(wire.get(), 9u); // D3DQUERYTYPE_OCCLUSION
    check(query, "registered occlusion identity");
  }
  ~Fixture() {
    if (wire) (void)dxmt9::d3d9::drainDeferredReplay(wire.get(), "oracle teardown");
    if (query) dxmt9c_query_release(query);
    wire.reset();
    if (iface) iface->Release();
    if (factory) factory->Release();
  }
};

void run(bool requireRetainedWriter) {
  Fixture f;
  auto& device = f.wire->dev();
  auto& queue = device.upperDevice()->queue();
  check(device.upperDevice()->usesMetalOcclusion(), "real GPU occlusion required");
  check(device.setFVF(0x4002u | 0x040u) == D3D_OK, "XYZW diffuse");
  check(device.setRenderState(RS_CULL_MODE, u32(CullMode::None)) == D3D_OK &&
        device.setRenderState(RS_LIGHTING, 0) == D3D_OK &&
        device.setRenderState(RS_Z_ENABLE, 0) == D3D_OK, "fixed state");
  check(device.setViewport({0, 0, 32, 32, 0, 1}) == D3D_OK, "viewport");
  struct Vertex { float x, y, z, w; u32 color; };
  const std::array<Vertex, 6> vertices{{
      {-2,-2,.5f,1,0xff00ff00}, {-2,2,.5f,1,0xff00ff00}, {2,-2,.5f,1,0xff00ff00},
      {2,-2,.5f,1,0xff00ff00}, {-2,2,.5f,1,0xff00ff00}, {2,2,.5f,1,0xff00ff00}}};
  auto vb = device.createBuffer({sizeof(vertices), Pool::Default, UsageVertexBuffer});
  check(bool(vb), "vertex buffer");
  auto mapping = vb->lock(0, sizeof(vertices), 0);
  check(mapping.data, "vertex mapping");
  std::memcpy(mapping.data, vertices.data(), sizeof(vertices));
  vb->unlock();
  check(device.setStreamSource(0, vb, 0, sizeof(Vertex)) == D3D_OK, "stream");
  device.upperDevice()->flush();
  const auto before = queue.queryProgress();

  pe::CommandChunkBuilder builder;
  const auto draw = [&] {
    check(pe::appendSparseRecord(builder, D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
        {.primitiveType = 4u, .primitiveCount = 2}, // D3DPT_TRIANGLELIST
        {}), "serialized draw");
  };
  pe::QueryRef ref{};
  ref.identity = f.query->wireIdentity;
  ref.object = f.query;
  draw(); // Must not contribute to the following query.
  check(pe::appendQueryIssue(builder, 2u, ref), "serialized BEGIN");
  draw();
  check(pe::appendQueryIssue(builder, 1u, ref), "serialized END");
  const auto sealed = builder.seal();
  check(sealed.valid(), "canonical wire seals");
  const D9CCommandChunk chunk{
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = sealed.recordCount,
      .recordBytes = static_cast<u32>(sealed.blob.size()),
      .records = toWireHandle(sealed.blob.data()),
      .handleCount = sealed.handleCount,
  };
  check(dxmt9c_device_commit_chunk(f.wire.get(), &chunk) == D3D_OK, "wire import");
  check(dxmt9::d3d9::drainDeferredReplay(f.wire.get(), "serialized range oracle"),
        "actual replay completes");
  check(f.query->replayDrainTarget->lastQueuedSeq != 0 &&
        f.query->replayDrainTarget->lastQueuedSeq ==
            f.query->replayDrainTarget->lastReplayedSeq, "query raw identity settled");
  const auto beforePoll = queue.queryProgress();

  u32 samples = 0xdeadbeef;
  HResult hr = S_FALSE;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (hr == S_FALSE && std::chrono::steady_clock::now() < deadline) {
    hr = dxmt9c_query_get_data(f.query, &samples, sizeof(samples), QUERY_GETDATA_FLUSH);
    if (hr == S_FALSE) {
      check(samples == 0xdeadbeef, "pending poll leaves output untouched");
      std::this_thread::yield();
    }
  }
  check(hr == S_OK && samples == 32 * 32, "exact membership and FLUSH progress");
  const auto after = queue.queryProgress();
  std::cout << "serialized-query sources-before-poll="
            << beforePoll.published - before.published
            << " sources-after-poll=" << after.published - before.published
            << " samples=" << samples << '\n';

  auto readback = device.createSurface(
      {32, 32, Format::A8R8G8B8, Pool::Scratch, 0, false, false});
  check(device.getRenderTargetData(device.swapChain()->backBuffer(), readback) == D3D_OK,
        "readback after query completion, never a polling rescue");
  const auto pixels = readback->lockRect(nullptr, 0);
  check(pixels.data && pixels.pitch >= 128, "pixel bytes");
  for (u32 y = 0; y != 32; ++y)
    for (u32 x = 0; x != 32; ++x) {
      const auto* p = static_cast<const u8*>(pixels.data) + y * pixels.pitch + x * 4;
      check(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255,
            "exact green pixels");
    }
  readback->unlockRect();
  // Explicit activation gate, separate from correctness. Current serialized
  // ordered release is known to fail this; no expected-failure blanket masks
  // query, pixel, timeout, or validation failures in the normal test matrix.
  if (requireRetainedWriter)
    check(beforePoll.published == before.published &&
          after.published - before.published == 1,
          "serialized query ranges must retain writer until FLUSH");
}
} // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 1 || (argc == 2 &&
          std::strcmp(argv[1], "--require-retained-writer") == 0), "argument");
    run(argc == 2);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
