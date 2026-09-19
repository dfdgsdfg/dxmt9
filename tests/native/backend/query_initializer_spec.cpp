// Real initializer-only completion oracle: no ordinary draw/clear/Present
// or readback is allowed to supply the progress required by the query poll.
#include "../../../src/dxmt9/dxmt9_blit_encoders.hpp"
#include "../../../src/dxmt9/dxmt9_command_queue.hpp"
#include "../../../src/dxmt9/dxmt9_resource_pool.hpp"
#include "../../../src/winemetal/Metal.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <future>
#include <string_view>
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

  query::EndReceipt previous{};
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
    check(fence.valid() && fence.snapshot().sequence == 0 && fence.snapshot().initializer > 0,
          "END captures initializer-only actual submission");
    if (previous.valid()) {
      check(fence.snapshot().initializer > previous.snapshot().initializer, "reissue gets new upload prefix");
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

void testCoalescedOrdinary(dxmt9::CommandQueue& queue) {
  using namespace dxmt9;
  core::SurfaceDesc desc{};
  desc.width = desc.height = 8;
  desc.format = core::Format::A8R8G8B8;
  desc.usage = core::UsageRenderTarget;
  desc.renderTarget = true;
  const auto surface = queue.pool().createSurface(queue.device(), queue.limits(), desc);
  check(bool(surface), "coalesced render target");
  Gate gate{queue.device().newSharedEvent()};
  check(bool(gate.event), "ordinary independent GPU gate");
  auto blocker = queue.newCommandBuffer();
  blocker.encodeWaitForEvent(WMT::Event{gate.event.handle}, 1);
  blocker.commit();
  core::ClearDesc clear{};
  clear.colorAttachments[0].handle = surface;
  clear.clearColor = true;
  clear.color = {1, 0, 0, 1};
  const auto before = queue.queryProgress().published;
  queue.submitClear(clear);
  auto first = queue.publishQueryEnd();
  clear.color = {0, 1, 0, 1};
  queue.submitClear(clear);
  const auto second = queue.publishQueryEnd();
  check(first.marker && first.marker == second.marker &&
            queue.queryProgress().published == before,
        "two real clears share deferred writer, no END cut");
  check(queue.pollQueryFence(first, false) == query::Poll::Pending,
        "unbound ordinary marker cannot complete");
  check(queue.pollQueryFence(second, true) == query::Poll::Pending,
        "FLUSH publishes without waiting for blocked GPU");
  check(first.snapshot().sequence == second.snapshot().sequence &&
            first.snapshot().sequence > before,
        "publication binds both hardware receipts to one actual sequence");
  first = {};
  for (unsigned i = 0; i != 16; ++i)
    check(queue.pollQueryFence(second, false) == query::Poll::Pending,
          "blocked hardware remains pending after public receipt release");
  gate.release();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto status = query::Poll::Pending;
  while (status == query::Poll::Pending && std::chrono::steady_clock::now() < deadline) {
    status = queue.pollQueryFence(second, false);
    if (status == query::Poll::Pending) std::this_thread::yield();
  }
  check(status == query::Poll::Complete, "one FLUSH retains submission progress");
  // Readback is strictly after success and cannot rescue a parked encoder.
  core::ReadbackDesc readback{};
  readback.source = surface;
  readback.sourceRect = core::Rect{0, 0, 8, 8};
  core::ReadbackPixels actual{};
  check(encoders::readbackSurface(queue, queue.pool(), queue.device(), queue.limits(),
                                 readback, actual), "coalesced output readback");
  check(actual.pitch == 32 && actual.bytes.size() == 256, "exact output extent");
  for (std::size_t i = 0; i != actual.bytes.size(); i += 4)
    check(actual.bytes[i] == 0 && actual.bytes[i+1] == 255 &&
              actual.bytes[i+2] == 0 && actual.bytes[i+3] == 255,
          "ordered red then green clears produce exact green bytes");
}

void testCoveredPending(dxmt9::CommandQueue& queue) {
  using namespace dxmt9;
  Gate gate{queue.device().newSharedEvent()};
  check(bool(gate.event), "covered gate");
  auto blocker = queue.newCommandBuffer();
  check(bool(blocker), "covered blocker");
  blocker.encodeWaitForEvent(WMT::Event{gate.event.handle}, 1);
  blocker.commit();
  core::SurfaceDesc desc{};
  desc.width = desc.height = 8;
  desc.format = core::Format::A8R8G8B8;
  desc.usage = core::UsageRenderTarget;
  desc.renderTarget = true;
  const auto surface = queue.pool().createSurface(queue.device(), queue.limits(), desc);
  check(bool(surface), "covered surface");
  core::ClearDesc clear{};
  clear.colorAttachments[0].handle = surface;
  clear.clearColor = true;
  clear.color = {0, 1, 0, 1};
  queue.submitClear(clear);
  const auto receipt = queue.publishQueryEnd();
  check(receipt.valid() && !receipt.marker && receipt.prefix.sequence,
        "eager published receipt");
  const auto beforeCoverage = queue.queryPendingCovered_.load(std::memory_order_acquire);
  check(beforeCoverage < receipt.prefix.sequence, "new END has no borrowed coverage");
  check(queue.pollQueryFence(receipt, false) == query::Poll::Pending &&
        queue.queryPendingCovered_.load(std::memory_order_acquire) == beforeCoverage,
        "non-FLUSH poll cannot manufacture submission coverage");
  check(queue.pollQueryFence(receipt, true) == query::Poll::Pending,
        "first FLUSH installs durable coverage");
  // A real public poll must finish while another thread owns the large mutex.
  // Release it before checking the timeout so a failing fixture cannot deadlock.
  std::unique_lock lock(queue.mutex_);
  auto result = std::async(std::launch::async, [&] {
    for (unsigned i = 0; i != 64; ++i)
      if (queue.pollQueryFence(receipt, (i & 1) != 0) != query::Poll::Pending)
        return false;
    return true;
  });
  const bool bypassed = result.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  lock.unlock();
  check(result.get() && bypassed, "covered Pending bypasses queue mutex");
  auto invalid = receipt;
  ++invalid.prefix.queue;
  check(queue.pollQueryFence(invalid, true) == query::Poll::Invalid,
        "wrong queue cannot reuse coverage");
  invalid = receipt;
  invalid.prefix.initializer = UINT64_MAX;
  check(queue.pollQueryFence(invalid, true) == query::Poll::Invalid,
        "unvalidated initializer cannot reuse coverage");
  gate.release();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto status = query::Poll::Pending;
  while (status == query::Poll::Pending && std::chrono::steady_clock::now() < deadline) {
    status = queue.pollQueryFence(receipt, false);
    std::this_thread::yield();
  }
  check(status == query::Poll::Complete, "one FLUSH progresses to locked success");
  core::ReadbackDesc readback{};
  readback.source = surface;
  readback.sourceRect = core::Rect{0, 0, 8, 8};
  core::ReadbackPixels pixels{};
  check(encoders::readbackSurface(queue, queue.pool(), queue.device(), queue.limits(),
                                 readback, pixels), "covered readback");
  check(pixels.pitch == 32 && pixels.bytes.size() == 256, "covered extent");
  for (std::size_t i = 0; i != pixels.bytes.size(); i += 4)
    check(pixels.bytes[i] == 0 && pixels.bytes[i+1] == 255 &&
          pixels.bytes[i+2] == 0 && pixels.bytes[i+3] == 255, "covered exact green");
  Gate terminalGate{queue.device().newSharedEvent()};
  check(bool(terminalGate.event), "terminal gate");
  auto terminalBlocker = queue.newCommandBuffer();
  check(bool(terminalBlocker), "terminal blocker");
  terminalBlocker.encodeWaitForEvent(WMT::Event{terminalGate.event.handle}, 1);
  terminalBlocker.commit();
  queue.submitClear(clear);
  const auto terminalReceipt = queue.publishQueryEnd();
  check(queue.pollQueryFence(terminalReceipt, true) == query::Poll::Pending,
        "terminal case acquires coverage while GPU pending");
  queue.failStopReplaySourceExecution();
  check(queue.pollQueryFence(terminalReceipt, true) == query::Poll::Failed,
        "terminal defeats coverage even while GPU remains pending");
}
} // namespace

int main(int argc, char** argv) {
  try {
    auto devices = WMT::CopyAllDevices();
    if (!devices || !devices.count()) return 77;
    dxmt9::CommandQueue queue(devices.object(0), {}, false);
    check(queue.valid() && queue.started(), "production queue starts");
    testInitializerOnly(queue);
    const bool covered = argc > 1 && std::string_view(argv[1]) == "--covered-query";
    if (covered) testCoveredPending(queue);
    else if (argc > 1) testCoalescedOrdinary(queue);
    std::cout << "query_initializer_spec passed (3 gated uploads, 768 exact bytes)\n";
    if (covered) std::cout << "covered Pending lock bypass and exact readback passed\n";
    else if (argc > 1)
      std::cout << "coalesced ordinary oracle passed (2 ordered clears, 256 exact bytes)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
