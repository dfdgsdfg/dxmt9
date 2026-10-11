#include "dxmt9/dxmt9_command_queue.hpp"
#include "dxmt9/dxmt9_render_tape_attachment_restore.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace { std::atomic<std::ptrdiff_t> failAllocationAfter{-1}; }
void* operator new(std::size_t size) {
  auto left = failAllocationAfter.load();
  while (left >= 0) {
    if (failAllocationAfter.compare_exchange_weak(left, left - 1)) {
      if (!left) throw std::bad_alloc();
      break;
    }
  }
  if (auto* value = std::malloc(size ? size : 1u)) return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }

namespace dxmt9 {
struct CommandQueueArenaLeaseTestAccess {
  static core::Handle surface(CommandQueue& queue) {
    return queue.pool_.createSurface(WMT::Device{NULL_OBJECT_HANDLE}, {},
        core::SurfaceDesc{.format = core::Format::NullRt});
  }
  static void current(CommandQueue& queue, core::Handle value) {
    queue.currentBackBuffer_ = value;
    queue.backBufferDiscardAfterPresent_ = false;
  }
  static bool discard(CommandQueue& queue) { return queue.backBufferDiscardAfterPresent_; }
  static void inflight(CommandQueue& queue, std::size_t value) { queue.inflightCount_ = value; }
  static void terminal(CommandQueue& queue, bool value) { queue.stop_ = value; }
  static void builder(CommandQueue& queue, bool value) { queue.arenaAdmissionActive_.store(value); }
  static void destroy(CommandQueue& queue, core::Handle value) {
    queue.pool_.markSurfaceDestroyAndGc(value.value, 0u);
  }
};
}

namespace {
using namespace dxmt9;
using namespace dxmt9::d3d9;
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
struct Fixture {
  core::BackendLimits limits{};
  CommandQueue queue{CommandQueue::ArenaLeaseTestQueueTag{}, limits};
  core::Handle current = CommandQueueArenaLeaseTestAccess::surface(queue);
  core::Handle target = CommandQueueArenaLeaseTestAccess::surface(queue);
  core::Handle unrelated = CommandQueueArenaLeaseTestAccess::surface(queue);
  RenderTapeAttachmentHistoryHeader header{.token = 11u, .epoch = 12u, .queue = 13u,
      .completedSequence = 99u, .completedInitializer = 88u, .count = 3u, .flags = 3u};
  const D9CWireObjectIdentity a{D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 100u};
  const D9CWireObjectIdentity b{D9C_CHUNK_HANDLE_KIND_SURFACE, 4u, 101u};
  const D9CWireObjectIdentity c{D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 102u};
  std::array<RenderTapeAttachmentHistoryRow, 3> history{{
      {.surface = a, .storage = a, .currentBackbuffer = 1u},
      {.surface = b, .storage = b, .currentBackbuffer = 1u},
      {.surface = c, .storage = c, .touched = 1u}}};
  std::array<D9CWireObjectIdentity, 3> required{a, b, c};
  std::array<TapeAttachmentRestoreBinding, 3> bindings{{
      {a, current.value}, {b, current.value}, {c, target.value}}};
  TapeAttachmentRestoreBoundary boundary{};
  Fixture() {
    CommandQueueArenaLeaseTestAccess::current(queue, current);
    queue.markColorHandleTouched(current);
    queue.markColorHandleTouched(unrelated);
    const auto progress = queue.queryProgress();
    boundary = {progress.queue, progress.published, progress.initializerPublished};
  }
  auto plan() { return TapeAttachmentRestorePlan::prepare(header, history, required, bindings, boundary); }
  void unchanged() {
    check(queue.isColorHandleTouched(current) && !queue.isColorHandleTouched(target) &&
        queue.isColorHandleTouched(unrelated) && !CommandQueueArenaLeaseTestAccess::discard(queue),
        "failed restore leaves all original live touch/discard state unchanged");
  }
};

void planRejections() {
  for (unsigned fault = 0; fault < 8u; ++fault) {
    Fixture f;
    if (fault == 0) ++f.bindings[0].original.generation;
    if (fault == 1) f.bindings[0].localHandle = 0u;
    if (fault == 2) f.history[1].touched = 1u;
    if (fault == 3) f.bindings[1].localHandle = f.target.value;
    if (fault == 4) f.bindings[2].localHandle = f.current.value;
    if (fault == 5) f.required[1] = f.required[0];
    if (fault == 6) f.header.count = 2u;
    if (fault == 7) f.boundary.queue = 0u;
    check(!f.plan(), "malformed, stale or inconsistent alias plan rejected");
    f.unchanged();
  }
  Fixture f;
  check(!TapeAttachmentRestorePlan::prepare(f.header, f.history, f.required,
      std::span(f.bindings).first(2), f.boundary), "missing required binding rejected");
  for (std::ptrdiff_t count = 0; count < 2; ++count) {
    failAllocationAfter = count;
    const auto plan = f.plan();
    failAllocationAfter = -1;
    check(!plan, "plan allocation failures return no partial plan");
  }
}

void runtimeRejections() {
  for (unsigned fault = 0; fault < 11u; ++fault) {
    Fixture f;
    auto plan = f.plan();
    check(bool(plan), "valid original history prepares");
    if (fault == 0) { ++f.boundary.queue; plan.reset(); plan.emplace(std::move(*f.plan())); }
    if (fault == 1) { ++f.boundary.published; plan.reset(); plan.emplace(std::move(*f.plan())); }
    if (fault == 2) { ++f.boundary.initializerPublished; plan.reset(); plan.emplace(std::move(*f.plan())); }
    if (fault == 3) CommandQueueArenaLeaseTestAccess::inflight(f.queue, 1u);
    if (fault == 4) CommandQueueArenaLeaseTestAccess::terminal(f.queue, true);
    if (fault == 5) CommandQueueArenaLeaseTestAccess::builder(f.queue, true);
    if (fault == 6) f.queue.setRenderTapeExactAttachmentPreservation(true);
    if (fault == 7) CommandQueueArenaLeaseTestAccess::current(f.queue, f.target);
    if (fault == 8) CommandQueueArenaLeaseTestAccess::destroy(f.queue, f.target);
    const render::TapeRuntimeObserverSink observer{nullptr,
        [](void*, const render::TapeRuntimeObservation&) noexcept {}};
    if (fault == 9) {
      render::TapeRuntimeBaseline baseline;
      check(f.queue.installTapeRuntimeObserver(observer, baseline), "capture observer installs");
    }
    if (fault == 10) {
      f.queue.setRenderTapeExactAttachmentPreservation(true);
      f.queue.setRenderTapeExactAttachmentPreservation(false);
    }
    check(!f.queue.restoreRenderTapeNormalAttachmentHistory(std::move(*plan)),
        "local boundary, health, policy and live identity independently gate restoration");
    check(!plan->valid(), "rejected plan is consumed");
    f.unchanged();
    CommandQueueArenaLeaseTestAccess::inflight(f.queue, 0u);
    CommandQueueArenaLeaseTestAccess::terminal(f.queue, false);
    CommandQueueArenaLeaseTestAccess::builder(f.queue, false);
    if (fault == 9) f.queue.revokeTapeRuntimeObserver(observer);
  }
}

void originalHistoryTruthTable() {
  for (unsigned bits = 0u; bits < 8u; ++bits) {
    Fixture f;
    f.history[0].touched = f.history[1].touched = bool(bits & 1u);
    f.history[2].touched = bool(bits & 2u);
    f.header.flags = 2u | bool(bits & 4u);
    auto plan = f.plan();
    check(plan && f.queue.restoreRenderTapeNormalAttachmentHistory(std::move(*plan)),
        "every original touch/discard combination restores");
    check(f.queue.isColorHandleTouched(f.current) == bool(bits & 1u) &&
        f.queue.isColorHandleTouched(f.target) == bool(bits & 2u) &&
        CommandQueueArenaLeaseTestAccess::discard(f.queue) == bool(bits & 4u),
        "touch and discard bits are independent of each other and setup writes");
  }
  Fixture f;
  const D9CWireObjectIdentity parent{D9C_CHUNK_HANDLE_KIND_TEXTURE, 2u, 500u};
  f.header.flags = 0u;
  f.header.count = 2u;
  f.history[0].storage = f.history[1].storage = parent;
  f.history[0].currentBackbuffer = f.history[1].currentBackbuffer = 0u;
  const auto plan = [&] {
    return TapeAttachmentRestorePlan::prepare(f.header, std::span(f.history).first(2),
        std::span(f.required).first(2), std::span(f.bindings).first(2), f.boundary);
  };
  check(bool(plan()), "matching texture-level aliases join one local handle");
  ++f.history[1].subresource;
  check(!plan(), "different original subresources cannot collapse to one local handle");
  --f.history[1].subresource;
  f.bindings[1].localHandle = f.target.value;
  check(!plan(), "one original subresource cannot split into unrelated local handles");
}

void transactionalRestore() {
  unsigned rejected = 0u, succeeded = 0u;
  for (std::ptrdiff_t count = 0; count < 8; ++count) {
    Fixture f;
    auto plan = f.plan();
    check(bool(plan), "complete history mapping prepares");
    failAllocationAfter = count;
    const bool restored = f.queue.restoreRenderTapeNormalAttachmentHistory(std::move(*plan));
    failAllocationAfter = -1;
    check(!plan->valid(), "restoration consumes the plan exactly once");
    if (!restored) { ++rejected; f.unchanged(); continue; }
    ++succeeded;
    check(!f.queue.isColorHandleTouched(f.current) && f.queue.isColorHandleTouched(f.target) &&
        f.queue.isColorHandleTouched(f.unrelated) && CommandQueueArenaLeaseTestAccess::discard(f.queue),
        "original untouched/touched/discard overrides seed history without touching unrelated resources");
    check(!f.queue.restoreRenderTapeNormalAttachmentHistory(std::move(*plan)),
        "consumed plan cannot restore twice");
    const auto progress = f.queue.queryProgress();
    check(progress.published == f.boundary.published && progress.completed == f.boundary.published &&
        progress.initializerPublished == f.boundary.initializerPublished,
        "restore publishes no work and imports no captured completion labels");
  }
  check(rejected && succeeded, "fault enumeration covers rollback and commit");
  std::cout << "restore allocation outcomes: rejected=" << rejected
            << " committed=" << succeeded << '\n';
}
}

int main() {
  try {
    planRejections(); runtimeRejections(); originalHistoryTruthTable(); transactionalRestore();
    std::cout << "Normal attachment restore: alias, generation, local boundary, one-use and allocation checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    failAllocationAfter = -1;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
