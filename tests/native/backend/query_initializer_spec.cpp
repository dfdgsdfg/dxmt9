// Real initializer-only completion oracle: no ordinary draw/clear/Present
// or readback is allowed to supply the progress required by the query poll.
#include "../../../src/dxmt9/dxmt9_blit_encoders.hpp"
#include "../../../src/dxmt9/dxmt9_command_queue.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/winemetal/Metal.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

// Never leave the hardware parked if an assertion unwinds the fixture.
struct Gate {
  WMT::Reference<WMT::SharedEvent> event;
  ~Gate() { event.signalValue(1); }
  void release() { event.signalValue(1); }
};

void testInitializerOnly(dxmt9::CommandQueue& queue) {
  using namespace dxmt9;
  constexpr unsigned size = 8;
  core::TextureDesc textureDesc{};
  textureDesc.width = size;
  textureDesc.height = size;
  textureDesc.levels = 1;
  textureDesc.format = core::Format::A8R8G8B8;
  textureDesc.usage = core::UsageRenderTarget; // forces private-mode staging
  const auto texture = queue.pool().createTexture(queue.device(), queue.limits(), textureDesc);
  check(bool(texture), "create private upload destination");
  core::SurfaceDesc surfaceDesc{};
  surfaceDesc.width = size;
  surfaceDesc.height = size;
  surfaceDesc.format = textureDesc.format;
  surfaceDesc.usage = core::UsageRenderTarget;
  surfaceDesc.renderTarget = true;
  const auto surface = queue.pool().createSurfaceForTexture(texture, 0, surfaceDesc);
  check(bool(surface), "create readback alias");

  query::Fence previous{};
  for (unsigned issue = 0; issue != 3; ++issue) {
    Gate gate{queue.device().newSharedEvent()};
    check(bool(gate.event), "create independent GPU gate");
    auto blocker = queue.newCommandBuffer();
    check(bool(blocker), "create GPU gate command buffer");
    blocker.encodeWaitForEvent(WMT::Event{gate.event.handle}, 1);
    blocker.commit();
    std::vector<std::uint8_t> expected(size * size * 4);
    for (std::size_t i = 0; i != expected.size(); ++i)
      expected[i] = static_cast<std::uint8_t>((i * 17 + issue * 53) & 255);
    queue.uploadTextureLevel(texture, 0, size, size, 1, size * 4,
                             size * size * 4, expected);
    const auto fence = queue.publishQueryEnd();
    check(fence.valid() && fence.sequence == 0 && fence.initializer > 0,
          "END captures initializer-only actual submission");
    if (previous.valid()) {
      check(fence.initializer > previous.initializer, "reissue gets new upload prefix");
      check(queue.pollQueryFence(previous, true) == query::Poll::Complete,
            "later blocked upload does not extend old receipt");
    }
    for (unsigned poll = 0; poll != 16; ++poll)
      check(queue.pollQueryFence(fence, (poll & 1) != 0) == query::Poll::Pending,
            "unreleased GPU gate keeps FLUSH/non-FLUSH pending without waiting");
    gate.release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto result = query::Poll::Pending;
    while (result == query::Poll::Pending && std::chrono::steady_clock::now() < deadline) {
      result = queue.pollQueryFence(fence, true);
      if (result == query::Poll::Pending) std::this_thread::yield();
    }
    check(result == query::Poll::Complete, "actual initializer completion resolves query");
    // Deliberately AFTER completion: this readback cannot rescue the poll.
    core::ReadbackDesc readback{};
    readback.source = surface;
    readback.sourceRect = core::Rect{0, 0, size, size};
    core::ReadbackPixels actual{};
    check(encoders::readbackSurface(queue, queue.pool(), queue.device(), queue.limits(),
                                   readback, actual), "initializer bytes read back");
    check(actual.pitch == size * 4 && actual.bytes == expected,
          "all initializer pixels match, including reissue overwrite");
    previous = fence;
  }
}
} // namespace

int main() {
  try {
    auto devices = WMT::CopyAllDevices();
    if (!devices || !devices.count()) return 77;
    dxmt9::CommandQueue queue(devices.object(0), {}, false);
    check(queue.valid() && queue.started(), "production queue starts");
    testInitializerOnly(queue);
    std::cout << "query_initializer_spec passed (3 gated uploads, 768 exact bytes)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
