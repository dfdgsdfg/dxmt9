#include "d3d9_pe_com_cache.hpp"
#include "d3d9_pe_cached_com_lifetime.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {
struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

struct FakeCom {
  unsigned refs = 1u;
  unsigned AddRef() noexcept { return ++refs; }
  unsigned Release() noexcept { return --refs; }
};

template <typename T>
struct FailingAllocator {
  using value_type = T;
  bool* fail = nullptr;

  FailingAllocator() = default;
  explicit FailingAllocator(bool* value) : fail(value) {}
  template <typename U>
  FailingAllocator(const FailingAllocator<U>& other) : fail(other.fail) {}

  T* allocate(std::size_t count) {
    if (fail && *fail) throw std::bad_alloc();
    return std::allocator<T>{}.allocate(count);
  }
  void deallocate(T* value, std::size_t count) noexcept {
    std::allocator<T>{}.deallocate(value, count);
  }
  template <typename U>
  bool operator==(const FailingAllocator<U>& other) const noexcept {
    return fail == other.fail;
  }
};

using CacheValue = std::pair<const unsigned, FakeCom*>;
using Cache = std::unordered_map<
    unsigned, FakeCom*, std::hash<unsigned>, std::equal_to<unsigned>,
    FailingAllocator<CacheValue>>;

using dxmt9::d3d9::pe::CachedComAction;
using dxmt9::d3d9::pe::CachedComLifetime;
using dxmt9::d3d9::pe::CachedComTransition;
using dxmt9::d3d9::pe::kCachedComCacheCredit;
using dxmt9::d3d9::pe::kCachedComExternalMask;
using dxmt9::d3d9::pe::planCachedComTransition;

struct PrivateFakeCom {
  CachedComLifetime lifetime{};
  std::uint32_t cacheAcquires = 0u;
  std::uint32_t cacheReleases = 0u;
  bool destroyed = false;

  unsigned AddRef() noexcept {
    return lifetime.transition(CachedComAction::AcquireExternal)
        .externalRefs;
  }
  unsigned Release() noexcept {
    const auto transition =
        lifetime.transition(CachedComAction::ReleaseExternal);
    destroyed = destroyed || transition.destroy;
    return transition.externalRefs;
  }
};

struct PrivateCacheCredit {
  template <typename Interface>
  static void acquire(Interface* object) noexcept {
    const auto transition =
        object->lifetime.transition(CachedComAction::AcquireCache);
    if (transition.valid)
      ++object->cacheAcquires;
  }

  template <typename Interface>
  static void release(Interface* object) noexcept {
    const auto transition =
        object->lifetime.transition(CachedComAction::ReleaseCache);
    if (transition.valid)
      ++object->cacheReleases;
    object->destroyed = object->destroyed || transition.destroy;
  }
};

using PrivateCacheValue = std::pair<const unsigned, PrivateFakeCom*>;
using PrivateCache = std::unordered_map<
    unsigned, PrivateFakeCom*, std::hash<unsigned>, std::equal_to<unsigned>,
    FailingAllocator<PrivateCacheValue>>;

void testCanonicalDuplicateOwnership() {
  bool fail = false;
  Cache cache(0u, std::hash<unsigned>{}, std::equal_to<unsigned>{},
              FailingAllocator<CacheValue>(&fail));
  FakeCom first;
  FakeCom* canonical = nullptr;
  check(D3D9PeCanonicalizeComCacheInsertion(cache, 3u, &first, &canonical) ==
            D3D9PeComCacheInsertStatus::Inserted &&
            canonical == &first && first.refs == 2u,
        "first insertion leaves one caller and one cache reference");

  FakeCom loser;
  canonical = nullptr;
  check(D3D9PeCanonicalizeComCacheInsertion(cache, 3u, &loser, &canonical) ==
            D3D9PeComCacheInsertStatus::Existing &&
            canonical == &first && loser.refs == 0u && first.refs == 3u,
        "duplicate insertion destroys the loser and returns canonical ref");
  canonical->Release();
  cache.begin()->second->Release();
  cache.clear();
  first.Release();
  check(first.refs == 0u, "canonical caller/cache ownership balances once");
}

void testActualAllocatorFailureBalancesCandidate() {
  bool fail = true;
  Cache cache(0u, std::hash<unsigned>{}, std::equal_to<unsigned>{},
              FailingAllocator<CacheValue>(&fail));
  FakeCom candidate;
  FakeCom* canonical = reinterpret_cast<FakeCom*>(0x1u);
  check(D3D9PeCanonicalizeComCacheInsertion(
            cache, 7u, &candidate, &canonical) ==
            D3D9PeComCacheInsertStatus::OutOfMemory &&
            canonical == nullptr && candidate.refs == 0u && cache.empty(),
        "real unordered_map allocation failure consumes both candidate refs");
}

void testPrivateCacheCreditCanonicalOwnership() {
  bool fail = false;
  PrivateCache cache(0u, std::hash<unsigned>{}, std::equal_to<unsigned>{},
                     FailingAllocator<PrivateCacheValue>(&fail));
  PrivateFakeCom first;
  PrivateFakeCom* canonical = nullptr;
  check(D3D9PeCanonicalizeComCacheInsertion<PrivateCacheCredit>(
            cache, 3u, &first, &canonical) ==
                D3D9PeComCacheInsertStatus::Inserted &&
            canonical == &first && first.cacheAcquires == 1u &&
            first.cacheReleases == 0u &&
            first.lifetime.packed() == (1u | kCachedComCacheCredit),
        "private cache credit inserts with public one plus cache one");

  PrivateFakeCom loser;
  canonical = nullptr;
  check(D3D9PeCanonicalizeComCacheInsertion<PrivateCacheCredit>(
            cache, 3u, &loser, &canonical) ==
                D3D9PeComCacheInsertStatus::Existing &&
            canonical == &first && loser.cacheAcquires == 1u &&
            loser.cacheReleases == 1u && loser.destroyed &&
            loser.lifetime.packed() == 0u &&
            first.lifetime.packed() == (2u | kCachedComCacheCredit),
        "private duplicate releases loser cache credit before public ref");

  canonical->Release();
  PrivateCacheCredit::release(cache.begin()->second);
  check(first.lifetime.packed() == 1u && first.cacheReleases == 1u,
        "private canonical caller/cache ownership separates cleanly");
  first.Release();
  cache.clear();
  check(first.destroyed && first.lifetime.packed() == 0u,
        "private canonical object reaches terminal state once");
}

void testPrivateCacheCreditAllocatorFailure() {
  bool fail = true;
  PrivateCache cache(0u, std::hash<unsigned>{}, std::equal_to<unsigned>{},
                     FailingAllocator<PrivateCacheValue>(&fail));
  PrivateFakeCom candidate;
  PrivateFakeCom* canonical = reinterpret_cast<PrivateFakeCom*>(0x1u);
  check(D3D9PeCanonicalizeComCacheInsertion<PrivateCacheCredit>(
            cache, 7u, &candidate, &canonical) ==
                D3D9PeComCacheInsertStatus::OutOfMemory &&
            canonical == nullptr && candidate.cacheAcquires == 1u &&
            candidate.cacheReleases == 1u && candidate.destroyed &&
            candidate.lifetime.packed() == 0u && cache.empty(),
        "private allocator failure releases cache then public ownership");
}

void checkTransitionShape(std::uint32_t packed, CachedComAction action,
                          const CachedComTransition& transition) {
  check(transition.externalRefs ==
            (transition.next & kCachedComExternalMask),
        "transition exposes the next public external count");
  if (transition.valid) {
    check(transition.next != packed || action == CachedComAction::AcquireExternal,
          "valid transition changes the packed lifetime state");
    check(!(transition.acquireParents && transition.releaseParents),
          "one transition cannot acquire and release parent holds");
  } else {
    check(transition.next == packed && !transition.destroy &&
              !transition.acquireParents && !transition.releaseParents,
          "invalid transition is side-effect free");
  }
}

void testCachedLifetimeReducerSmallDomain() {
  constexpr CachedComAction actions[] = {
      CachedComAction::AcquireExternal,
      CachedComAction::ReleaseExternal,
      CachedComAction::AcquireCache,
      CachedComAction::ReleaseCache,
  };
  for (std::uint32_t external = 0u; external < 8u; ++external) {
    for (const bool cached : {false, true}) {
      const auto packed = external | (cached ? kCachedComCacheCredit : 0u);
      for (const auto action : actions) {
        const auto transition = planCachedComTransition(packed, action);
        checkTransitionShape(packed, action, transition);
        const bool expected = [&] {
          switch (action) {
          case CachedComAction::AcquireExternal:
            return packed != 0u;
          case CachedComAction::ReleaseExternal:
            return external != 0u;
          case CachedComAction::AcquireCache:
            return packed != 0u && !cached;
          case CachedComAction::ReleaseCache:
            return cached;
          }
          return false;
        }();
        check(transition.valid == expected,
              "small-domain action validity follows terminal/cache state");
        if (!transition.valid)
          continue;
        switch (action) {
        case CachedComAction::AcquireExternal:
          check(transition.externalRefs == external + 1u &&
                    transition.acquireParents == (external == 0u) &&
                    !transition.destroy &&
                    !transition.releaseParents,
                "external acquire updates parent hold only on 0-to-1");
          break;
        case CachedComAction::ReleaseExternal:
          check(transition.externalRefs == external - 1u &&
                    transition.releaseParents == (external == 1u) &&
                    transition.destroy == (external == 1u && !cached),
                "external release updates parent hold only on 1-to-0");
          break;
        case CachedComAction::AcquireCache:
          check(transition.next == (packed | kCachedComCacheCredit) &&
                    !transition.acquireParents && !transition.releaseParents &&
                    !transition.destroy,
                "first cache acquire consumes only one private credit");
          break;
        case CachedComAction::ReleaseCache:
          check(transition.next == external && !transition.acquireParents &&
                    !transition.releaseParents &&
                    transition.destroy == (external == 0u),
                "cache release drops only the private credit");
          break;
        }
      }
    }
  }
}

void testCachedLifetimeExtremaAndTerminalRules() {
  const auto maxExternal = kCachedComExternalMask;
  check(!planCachedComTransition(maxExternal, CachedComAction::AcquireExternal)
             .valid,
        "public reference overflow is rejected");
  check(!planCachedComTransition(maxExternal | kCachedComCacheCredit,
                                 CachedComAction::AcquireExternal)
             .valid,
        "cached public reference overflow is rejected");
  check(!planCachedComTransition(0u, CachedComAction::AcquireExternal).valid,
        "dead object cannot be resurrected");
  check(!planCachedComTransition(0u, CachedComAction::ReleaseExternal).valid,
        "dead object cannot release an external reference");
  check(!planCachedComTransition(0u, CachedComAction::ReleaseCache).valid,
        "dead object cannot release a duplicate cache credit");
  check(!planCachedComTransition(kCachedComCacheCredit,
                                 CachedComAction::ReleaseExternal)
             .valid,
        "cache-only object cannot release a missing external reference");
  check(!planCachedComTransition(kCachedComCacheCredit,
                                 CachedComAction::AcquireCache)
             .valid,
        "duplicate cache acquisition is rejected");

  const auto lastExternal = planCachedComTransition(
      1u, CachedComAction::ReleaseExternal);
  check(lastExternal.valid && lastExternal.next == 0u &&
            lastExternal.externalRefs == 0u && lastExternal.destroy,
        "uncached final external release reaches terminal state");
  const auto lastCachedExternal = planCachedComTransition(
      1u | kCachedComCacheCredit, CachedComAction::ReleaseExternal);
  check(lastCachedExternal.valid &&
            lastCachedExternal.next == kCachedComCacheCredit &&
            lastCachedExternal.releaseParents && !lastCachedExternal.destroy,
        "cache credit keeps the object alive after final external release");
  const auto cacheOnlyReacquire = planCachedComTransition(
      kCachedComCacheCredit, CachedComAction::AcquireExternal);
  check(cacheOnlyReacquire.valid && cacheOnlyReacquire.externalRefs == 1u &&
            cacheOnlyReacquire.next == kCachedComCacheCredit + 1u &&
            cacheOnlyReacquire.acquireParents,
        "live cache permits a guarded external reacquire");

  const auto invalidAction = planCachedComTransition(
      1u, static_cast<CachedComAction>(0xffu));
  check(!invalidAction.valid, "unknown action is rejected");
}

void testCachedLifetimeReleaseOrders() {
  CachedComLifetime first;
  const auto cache = first.transition(CachedComAction::AcquireCache);
  check(!cache.acquireParents && !cache.releaseParents && first.packed() ==
                                      (1u | kCachedComCacheCredit),
        "cache acquisition records one private credit without parent effects");
  const auto external = first.transition(CachedComAction::ReleaseExternal);
  check(external.externalRefs == 0u && !external.destroy &&
            external.releaseParents &&
            first.packed() == kCachedComCacheCredit,
        "last external release leaves a cache-only object alive");
  const auto destroy = first.transition(CachedComAction::ReleaseCache);
  check(!destroy.releaseParents && destroy.destroy && first.packed() == 0u,
        "cache eviction destroys after the external count is empty");

  CachedComLifetime second;
  second.transition(CachedComAction::AcquireCache);
  const auto evict = second.transition(CachedComAction::ReleaseCache);
  check(!evict.releaseParents && !evict.destroy && second.packed() == 1u,
        "cache eviction preserves a live external caller");
  const auto final = second.transition(CachedComAction::ReleaseExternal);
  check(final.destroy && second.packed() == 0u,
        "last external release destroys after cache eviction");

  CachedComLifetime third;
  third.transition(CachedComAction::AcquireCache);
  third.transition(CachedComAction::AcquireExternal);
  third.transition(CachedComAction::ReleaseExternal);
  third.transition(CachedComAction::ReleaseExternal);
  const auto finalCached = third.transition(CachedComAction::ReleaseCache);
  check(finalCached.destroy && third.packed() == 0u,
        "extra external acquire/release does not duplicate cache ownership");
}

// R-CORE-4.9.1: real recursive fake ownership using the production reducer.
// ASan can detect an accidental access after child/parent deletion here.
struct NodeReceipt {
  bool alive = true;
  unsigned destroyed = 0u;
};

struct LifetimeNode {
  CachedComLifetime lifetime{};
  NodeReceipt* receipt;
  LifetimeNode* device = nullptr;
  LifetimeNode* container = nullptr;
  LifetimeNode* child = nullptr;
  bool strongCache = false;

  LifetimeNode(NodeReceipt& value, LifetimeNode* d = nullptr,
               LifetimeNode* c = nullptr)
      : receipt(&value), device(d), container(c) {
    if (device) device->AddRef();
    if (container) container->AddRef();
  }
  ~LifetimeNode() noexcept(false) {
    check(!device || device->receipt->alive, "device survives child destructor");
    check(!container || container->receipt->alive,
          "container survives child destructor");
    clearCache();
    check(receipt->alive && receipt->destroyed == 0u,
          "wrapper is physically destroyed exactly once");
    receipt->alive = false;
    ++receipt->destroyed;
  }
  unsigned AddRef() {
    auto* const d = device;
    auto* const c = container;
    const auto step = lifetime.transition(CachedComAction::AcquireExternal);
    if (step.acquireParents) {
      if (d) d->AddRef();
      if (c) c->AddRef();
    }
    return step.externalRefs;
  }
  unsigned Release() {
    auto* const d = device;
    auto* const c = container;
    const auto step = lifetime.transition(CachedComAction::ReleaseExternal);
    if (step.destroy) delete this;
    if (step.releaseParents) {
      if (c) c->Release();
      if (d) d->Release();
    }
    return step.externalRefs;
  }
  void releaseCache() {
    const auto step = lifetime.transition(CachedComAction::ReleaseCache);
    if (step.destroy) delete this;
  }
  void setCache(LifetimeNode* value, bool strong = false) {
    check(child == nullptr, "one canonical child per bounded parent");
    child = value;
    strongCache = strong;
    if (strong) child->AddRef();
    else child->lifetime.transition(CachedComAction::AcquireCache);
  }
  void clearCache() {
    auto* const saved = child;
    const bool strong = strongCache;
    child = nullptr; // Detach before a callback can reentrantly delete this.
    if (saved) {
      if (strong) saved->Release();
      else saved->releaseCache();
    }
  }
};

void testReentrantSurfaceSwapchainCacheComposition() {
  // Exhaust all six caller-release orders for implicit and additional chains.
  // Each operation includes recursive cache eviction and parent Release calls,
  // not a separate test-only counter adjustment after the operation.
  constexpr unsigned orders[][3] = {
      {0, 1, 2}, {0, 2, 1}, {1, 0, 2},
      {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  for (const bool implicit : {false, true}) {
    for (const auto& order : orders) {
      NodeReceipt receipts[3]{};
      LifetimeNode* nodes[3]{};
      nodes[0] = new LifetimeNode(receipts[0]);
      nodes[1] = new LifetimeNode(receipts[1], nodes[0]);
      nodes[2] = new LifetimeNode(receipts[2], nodes[0], nodes[1]);
      if (implicit) nodes[0]->setCache(nodes[1]);
      nodes[1]->setCache(nodes[2]);
      for (const auto index : order) nodes[index]->Release();
      for (const auto& receipt : receipts)
        check(!receipt.alive && receipt.destroyed == 1u,
              "all release orders retire device, swapchain and surface once");
    }
  }

  NodeReceipt receipts[3]{};
  auto* device = new LifetimeNode(receipts[0]);
  auto* swapchain = new LifetimeNode(receipts[1], device);
  auto* surface = new LifetimeNode(receipts[2], device, swapchain);
  device->setCache(swapchain);
  swapchain->setCache(surface);
  surface->Release();
  swapchain->Release();
  check(device->lifetime.packed() == 1u, "cache-only children do not retain device");
  // The live root and unmodified caches dominate this simulated getter.
  surface->AddRef();
  check(device->lifetime.packed() == 3u,
        "cache-only surface reacquires both device and container parent holds");
  device->clearCache(); // Reset evicts cache, but external surface stays valid.
  check(receipts[1].alive && receipts[2].alive, "external child survives eviction");
  surface->Release();
  check(!receipts[1].alive && !receipts[2].alive,
        "last surface release recursively deletes its additional-style parent");
  device->Release();
  check(!receipts[0].alive, "device retires after remaining caller releases");
}

void testStrongCacheParentCycleCounterexample() {
  NodeReceipt receipts[3]{};
  auto* device = new LifetimeNode(receipts[0]);
  auto* swapchain = new LifetimeNode(receipts[1], device);
  auto* surface = new LifetimeNode(receipts[2], device, swapchain);
  device->setCache(swapchain, true);
  swapchain->setCache(surface, true);
  surface->Release();
  swapchain->Release();
  check(device->Release() == 2u, "public cache mutant reproduces final Release 2");
  for (const auto& receipt : receipts)
    check(receipt.alive && receipt.destroyed == 0u, "strong cache graph is cyclic");
  // Test-only cleanup of the intentional mutant, not a production workaround.
  device->clearCache();
  swapchain->clearCache();
  for (const auto& receipt : receipts)
    check(!receipt.alive && receipt.destroyed == 1u, "mutant test does not leak");
}
}  // namespace

int main() {
  try {
    testCanonicalDuplicateOwnership();
    testActualAllocatorFailureBalancesCandidate();
    testPrivateCacheCreditCanonicalOwnership();
    testPrivateCacheCreditAllocatorFailure();
    testCachedLifetimeReducerSmallDomain();
    testCachedLifetimeExtremaAndTerminalRules();
    testCachedLifetimeReleaseOrders();
    testReentrantSurfaceSwapchainCacheComposition();
    testStrongCacheParentCycleCounterexample();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_com_cache_spec failed: " << failure.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "pe_com_cache_spec passed\n";
  return EXIT_SUCCESS;
}
