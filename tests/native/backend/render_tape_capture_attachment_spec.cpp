#include "dxmt9/dxmt9_render_tape_runtime_observer.hpp"
#include "dxmt9/dxmt9_render_tape_topology_observer.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace dxmt9::render {
struct TapeRuntimeEdgeClockTestAccess {
  static void exhaustAfterOne(TapeRuntimeEdgeClock& clock) noexcept {
    clock.next_.store(UINT64_MAX - 1u, std::memory_order_release);
  }
};
struct TapeRuntimeObserverEndpointTestAccess {
  static std::array<std::size_t, 2> storage(TapeRuntimeObserverEndpoint& endpoint) {
    std::lock_guard lock(endpoint.mutex_);
    return {endpoint.states_.size(), endpoint.states_.capacity()};
  }
};
} // namespace dxmt9::render

namespace {
using namespace dxmt9::render;
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

struct Probe {
  std::atomic<std::uint32_t> calls{0u};
  std::atomic<std::uint32_t> destroyed{0u};
  std::atomic<std::uint64_t> callbackEdge{0u};
  std::mutex mutex{};
  std::condition_variable cv{};
  bool entered = false;
  bool release = false;
  bool blocking = false;
  bool selfRetire = false;
  bool reenterDestructor = false;
  bool selfDetach = true;
  bool selfRevoke = true;
  bool remainedEnabled = false;
  bool destructorAttached = false;
  TapeRuntimeObserverEndpoint* endpoint = nullptr;
  TapeRuntimeObserverSink rootSink{};
  TapeRuntimeObserverSink staleProxy{};

  static void legacy(void* context, const TapeRuntimeObservation&) noexcept {
    ++static_cast<Probe*>(context)->calls;
  }
  TapeRuntimeObserverSink legacySink() noexcept { return {this, legacy}; }
};

class Root final : public TapeRuntimeCaptureAttachment {
public:
  explicit Root(Probe& probe) noexcept : probe_(probe) {}
  ~Root() override {
    ++probe_.destroyed;
    if (probe_.reenterDestructor) {
      // Dispatch locks the stale State, and attach locks the endpoint. Either
      // destructor running under those locks would deadlock this regression.
      if (probe_.staleProxy) probe_.staleProxy.fn(probe_.staleProxy.context, {});
      probe_.destructorAttached = probe_.endpoint->attach(probe_.legacySink());
    }
  }
  TapeRuntimeObserverSink sink() noexcept override { return {this, observe}; }
  TapeTopologyCapture& topology() noexcept override { return topology_; }
private:
  static void observe(void* context, const TapeRuntimeObservation&) noexcept {
    auto& root = *static_cast<Root*>(context);
    auto& probe = root.probe_;
    ++probe.calls;
    probe.callbackEdge.store(root.edges().next(), std::memory_order_release);
    if (probe.selfRetire) {
      probe.selfDetach = probe.endpoint->detach(probe.rootSink);
      probe.selfRevoke = probe.endpoint->revoke(probe.rootSink);
      probe.remainedEnabled = probe.endpoint->enabled();
    }
    if (probe.blocking) {
      std::unique_lock lock(probe.mutex);
      probe.entered = true;
      probe.cv.notify_all();
      probe.cv.wait(lock, [&] { return probe.release; });
    }
  }
  Probe& probe_;
  TapeTopologyCapture topology_{};
};

void checkedSharedClockIsMonotonicAndNonResettable() {
  TapeRuntimeEdgeClock clock;
  check(clock.baseline() == 1u && clock.next() == 2u && clock.next() == 3u && clock.active(),
      "baseline one is reserved and normal edges start at two");
  clock.revoke();
  check(!clock.active() && !clock.next() && !clock.next(), "revocation is permanent, never a counter reset");
  TapeRuntimeEdgeClock exhausted;
  TapeRuntimeEdgeClockTestAccess::exhaustAfterOne(exhausted);
  check(exhausted.next() == UINT64_MAX - 1u && !exhausted.active() && !exhausted.next() &&
      !exhausted.next(), "checked overflow never allocates max, wraps or reuses baseline");

  TapeRuntimeObserverEndpoint endpoint;
  Probe probe;
  auto root = std::make_shared<Root>(probe);
  check(endpoint.attachRetained(root), "shared-clock typed root attaches");
  const auto lease = endpoint.acquireCapture();
  constexpr std::size_t threadCount = 4u, count = 128u;
  std::array<std::array<std::uint64_t, count>, threadCount> rows{};
  std::array<std::thread, threadCount> threads;
  for (std::size_t thread = 0u; thread < threadCount; ++thread) {
    threads[thread] = std::thread([&, thread] {
      for (std::size_t index = 0u; index < count; ++index)
        rows[thread][index] = thread % 2u ? lease.nextEdge() : root->edges().next();
    });
  }
  for (auto& thread : threads) thread.join();
  std::array<std::uint64_t, threadCount * count> edges{};
  for (std::size_t thread = 0u; thread < threadCount; ++thread)
    std::copy(rows[thread].begin(), rows[thread].end(), edges.begin() + thread * count);
  std::sort(edges.begin(), edges.end());
  for (std::size_t index = 0u; index < edges.size(); ++index)
    check(edges[index] == index + 2u, "provenance and topology lease share exactly one concurrent edge source");
  check(endpoint.revoke(root->sink()) && !lease.nextEdge() && !root->edges().next(),
      "retirement permanently closes the common edge source");
}

void retainedLeaseOutlivesRetirementButCannotDispatch() {
  TapeRuntimeObserverEndpoint endpoint;
  Probe first, second;
  auto root = std::make_shared<Root>(first);
  const auto sink = root->sink();
  std::weak_ptr<Root> weak = root;
  check(endpoint.attachRetained(root), "retained capture attaches");
  const auto staleProxy = endpoint.proxy();
  auto lease = endpoint.acquireCapture();
  check(lease && lease.nextEdge() == 2u && lease.dispatch({}) && first.callbackEdge == 3u,
      "lease and provenance callback allocate from the root's common clock");
  bool topologyVisited = false;
  check(lease.visit([&](TapeRuntimeCaptureAttachment& owner) noexcept {
    topologyVisited = &owner.topology() == &root->topology();
  }) && topologyVisited, "gated typed visit selects only the acquired root's topology");
  root.reset();
  check(!weak.expired() && endpoint.detach(sink) && !endpoint.enabled() &&
      !lease.active() && !weak.expired() && first.destroyed == 0u,
      "an idle retained lease outlives the join without delaying retirement");
  check(!lease.dispatch({}) && !lease.nextEdge() &&
      !lease.visit([](TapeRuntimeCaptureAttachment&) noexcept {}),
      "revoked leases expose no unguarded dispatch or owner accessor");
  auto replacement = std::make_shared<Root>(second);
  const auto replacementSink = replacement->sink();
  check(endpoint.attachRetained(replacement), "new capture arm attaches while old lease remains retained");
  auto fresh = endpoint.acquireCapture();
  staleProxy.fn(staleProxy.context, {});
  check(fresh.nextEdge() == 2u && !lease.dispatch({}) && first.calls == 1u && !second.calls &&
      endpoint.revoke(sink) && fresh.active(), "stale arm, proxy and revoke cannot target the replacement sink");
  lease = {};
  check(weak.expired() && first.destroyed == 1u, "last old lease releases the old owner without a cycle");
  replacement.reset();
  check(endpoint.revoke(replacementSink) && second.destroyed == 0u,
      "replacement scope lifetime also remains independent of endpoint ownership");
  fresh = {};
  check(second.destroyed == 1u, "new arm has no retained owner cycle");
}

void retirementJoinsAdmittedCallbackAndAllowsRearm() {
  TapeRuntimeObserverEndpoint endpoint;
  Probe first, second;
  first.blocking = true;
  auto root = std::make_shared<Root>(first);
  const auto sink = root->sink();
  std::weak_ptr<Root> weak = root;
  check(endpoint.attachRetained(root), "blocking capture attaches");
  const auto staleProxy = endpoint.proxy();
  auto lease = endpoint.acquireCapture();
  root.reset();
  std::thread callback([&] { staleProxy.fn(staleProxy.context, {}); });
  {
    std::unique_lock lock(first.mutex);
    first.cv.wait(lock, [&] { return first.entered; });
  }
  std::atomic<bool> joined{false};
  bool retired = false;
  std::thread retirement([&] {
    retired = endpoint.revoke(sink);
    joined.store(true, std::memory_order_release);
  });
  while (endpoint.enabled()) std::this_thread::yield();
  check(!joined.load(std::memory_order_acquire) && !weak.expired() && !lease.active() &&
      !endpoint.acquireCapture(), "closing admission does not bypass the already-admitted callback join");
  auto replacement = std::make_shared<Root>(second);
  check(endpoint.attachRetained(replacement), "replacement arm need not wait for old callback completion");
  check(!lease.dispatch({}) && !lease.nextEdge(), "late old-lease operations reject during retirement");
  {
    std::lock_guard lock(first.mutex);
    first.release = true;
  }
  first.cv.notify_all();
  callback.join();
  retirement.join();
  staleProxy.fn(staleProxy.context, {});
  check(retired && joined && first.calls == 1u && !second.calls && endpoint.enabled(),
      "admitted work finishes only on its old root and retirement cannot clear the new arm");
  lease = {};
  check(weak.expired() && first.destroyed == 1u, "old root survives exactly until the last scope releases");
  check(endpoint.revoke(replacement->sink()), "replacement capture retires independently");
}

void retirementJoinsShortVisitNotLeaseLifetime() {
  TapeRuntimeObserverEndpoint endpoint;
  Probe probe;
  auto root = std::make_shared<Root>(probe);
  const auto sink = root->sink();
  check(endpoint.attachRetained(root), "short-visit capture attaches");
  auto lease = endpoint.acquireCapture();
  root.reset();
  bool visited = false, retired = false;
  std::atomic<bool> joined{false};
  std::thread visit([&] {
    visited = lease.visit([&](TapeRuntimeCaptureAttachment&) noexcept {
      std::unique_lock lock(probe.mutex);
      probe.entered = true;
      probe.cv.notify_all();
      probe.cv.wait(lock, [&] { return probe.release; });
    });
  });
  {
    std::unique_lock lock(probe.mutex);
    probe.cv.wait(lock, [&] { return probe.entered; });
  }
  std::thread retirement([&] {
    retired = endpoint.detach(sink);
    joined.store(true, std::memory_order_release);
  });
  while (endpoint.enabled()) std::this_thread::yield();
  check(!joined && !lease.visit([](TapeRuntimeCaptureAttachment&) noexcept {}),
      "retirement blocks later visits and waits for the admitted short operation");
  {
    std::lock_guard lock(probe.mutex);
    probe.release = true;
  }
  probe.cv.notify_all();
  visit.join();
  retirement.join();
  check(visited && retired && joined && !probe.destroyed,
      "join finishes even though the backend's retained lease still pins the root");
  lease = {};
  check(probe.destroyed == 1u, "lease lifetime is released outside in-flight synchronization");
}

void selfRetirementRejectsAcrossNestedActivities() {
  TapeRuntimeObserverEndpoint endpoint, innerEndpoint;
  Probe outer, inner;
  outer.endpoint = &endpoint;
  outer.selfRetire = true;
  auto root = std::make_shared<Root>(outer);
  outer.rootSink = root->sink();
  check(endpoint.attachRetained(root), "self-retirement typed root attaches");
  auto lease = endpoint.acquireCapture();
  check(lease.dispatch({}) && !outer.selfDetach && !outer.selfRevoke && outer.remainedEnabled,
      "callback self-detach and self-revoke reject without joining or changing admission");
  auto innerRoot = std::make_shared<Root>(inner);
  check(innerEndpoint.attachRetained(innerRoot), "nested other arm attaches");
  const auto innerLease = innerEndpoint.acquireCapture();
  bool nested = false, rejected = false;
  check(lease.visit([&](TapeRuntimeCaptureAttachment&) noexcept {
    nested = innerLease.visit([&](TapeRuntimeCaptureAttachment&) noexcept {
      rejected = !endpoint.revoke(outer.rootSink);
    });
  }) && nested && rejected && endpoint.enabled(),
      "nested admitted-operation chain detects outer self-join, not merely the top activity");
  check(endpoint.revoke(outer.rootSink) && innerEndpoint.revoke(innerRoot->sink()),
      "external retirement remains admitted after all nested operations finish");
}

void rootDestructionIsOutsideBothEndpointLocks() {
  TapeRuntimeObserverEndpoint endpoint;
  Probe probe;
  probe.endpoint = &endpoint;
  probe.reenterDestructor = true;
  auto root = std::make_shared<Root>(probe);
  const auto sink = root->sink();
  check(endpoint.attachRetained(root), "destructor-reentry capture attaches");
  probe.staleProxy = endpoint.proxy();
  root.reset();
  check(endpoint.detach(sink) && probe.destroyed == 1u && probe.destructorAttached &&
      endpoint.enabled() && !probe.calls, "final destructor can lock stale State and attach a new arm after retirement");
  check(endpoint.revoke(probe.legacySink()), "reentrantly attached raw arm retires");

  Probe active, failed;
  failed.endpoint = &endpoint;
  failed.reenterDestructor = true;
  check(endpoint.attach(active.legacySink()), "existing raw arm blocks another attachment");
  failed.staleProxy = endpoint.proxy();
  auto rejected = std::make_shared<Root>(failed);
  check(!endpoint.attachRetained(std::move(rejected)) && failed.destroyed == 1u &&
      !failed.destructorAttached && active.calls == 1u,
      "failed attachment also destroys its candidate outside both locks");
  check(endpoint.revoke(active.legacySink()), "blocking raw arm retires");
}

void legacyRawSinkHasNoTypedLease() {
  TapeRuntimeObserverEndpoint endpoint;
  Probe probe;
  check(endpoint.attach(probe.legacySink()) && !endpoint.acquireCapture(),
      "legacy V3 raw attach remains available without inventing a typed lifetime");
  const auto proxy = endpoint.proxy();
  proxy.fn(proxy.context, {});
  check(probe.calls == 1u && endpoint.detach(probe.legacySink()), "legacy proxy dispatch and detach semantics remain");
  proxy.fn(proxy.context, {});
  check(probe.calls == 1u, "legacy stale proxy remains inert");
}

void armTombstoneBudgetFailsWithoutPersistentGrowth() {
  TapeRuntimeObserverEndpoint endpoint;
  Probe probe;
  for (std::uint32_t arm = 0u; arm < 4096u; ++arm)
    check(endpoint.attach(probe.legacySink()) && endpoint.detach(probe.legacySink()),
        "each bounded raw arm retires without reusing stale-proxy State");
  const auto before = TapeRuntimeObserverEndpointTestAccess::storage(endpoint);
  check(before[0u] == 4096u && !endpoint.attach(probe.legacySink()) &&
      TapeRuntimeObserverEndpointTestAccess::storage(endpoint) == before && !endpoint.enabled(),
      "arm budget failure neither publishes another State nor grows persistent storage");
  auto root = std::make_shared<Root>(probe);
  check(!endpoint.attachRetained(root) && root->edges().active() &&
      TapeRuntimeObserverEndpointTestAccess::storage(endpoint) == before,
      "retained-arm budget rejection does not revoke an unattached owner's shared clock");
}

void exhaustiveBoundedLeaseLifecycle() {
  constexpr std::uint32_t actionCount = 5u;
  constexpr std::uint32_t sequenceCount = actionCount * actionCount * actionCount * actionCount;
  for (std::uint32_t sequence = 0u; sequence < sequenceCount; ++sequence) {
    TapeRuntimeObserverEndpoint endpoint;
    Probe probe;
    std::vector<std::shared_ptr<Root>> roots;
    TapeRuntimeCaptureLease lease;
    TapeRuntimeObserverSink sink{};
    bool active = false;
    std::uint32_t generation = 0u, leaseGeneration = 0u, remaining = sequence;
    for (std::uint32_t step = 0u; step < 4u; ++step) {
      const auto action = remaining % actionCount;
      remaining /= actionCount;
      if (action == 0u) {
        auto root = std::make_shared<Root>(probe);
        const bool actual = endpoint.attachRetained(root);
        check(actual == !active, "finite attachment cannot replace an active arm");
        if (actual) { active = true; ++generation; sink = root->sink(); }
        roots.push_back(std::move(root));
      } else if (action == 1u) {
        lease = endpoint.acquireCapture();
        check(static_cast<bool>(lease) == active, "finite acquire binds only the currently admitted arm");
        leaseGeneration = active ? generation : 0u;
      } else if (action == 2u) {
        const bool actual = endpoint.detach(sink);
        check(actual == active, "finite detach retires the selected active arm exactly once");
        if (actual) active = false;
      } else if (action == 3u) {
        const auto before = probe.calls.load();
        const bool permitted = active && leaseGeneration == generation;
        check(lease.dispatch({}) == permitted && probe.calls == before + (permitted ? 1u : 0u),
            "finite dispatch cannot use an unacquired or retired generation");
      } else {
        const bool permitted = active && leaseGeneration == generation;
        check((lease.nextEdge() != 0u) == permitted,
            "finite edge allocation shares the same exact arm admission as dispatch");
      }
      check(endpoint.enabled() == active && lease.active() == (active && leaseGeneration == generation),
          "finite arm state never authenticates an old retained owner after rearm");
    }
    if (active) check(endpoint.revoke(sink), "finite capture cleanup retires the final arm");
  }
}
} // namespace

int main() {
  try {
    checkedSharedClockIsMonotonicAndNonResettable();
    retainedLeaseOutlivesRetirementButCannotDispatch();
    retirementJoinsAdmittedCallbackAndAllowsRearm();
    retirementJoinsShortVisitNotLeaseLifetime();
    selfRetirementRejectsAcrossNestedActivities();
    rootDestructionIsOutsideBothEndpointLocks();
    legacyRawSinkHasNoTypedLease();
    armTombstoneBudgetFailsWithoutPersistentGrowth();
    exhaustiveBoundedLeaseLifecycle();
    std::cout << "render-tape-capture-attachment checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render-tape-capture-attachment: " << error.what() << '\n';
    return 1;
  }
}
