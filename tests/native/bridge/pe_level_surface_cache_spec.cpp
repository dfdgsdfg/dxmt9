// Host coverage for the pure part of the PE texture level-surface memo
// (`src/d3d9/d3d9_pe_level_surface_cache.hpp`). The COM wrappers that use it
// live in `d3d9_pe_device_child_surface.cpp`, which is PE-only (HDC/HBITMAP,
// IDirect3D*9), so only this policy core is host-testable; the wrapper wiring
// is compile-checked by the two PE cross lanes.

#include "d3d9_pe_level_surface_cache.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
  }
}

struct FakeEntry {
  int handle = 0;
};

using Cache = dxmt9::d3d9::pe::LevelSurfaceCache<FakeEntry>;

void testEmptyLookupsMiss() {
  Cache cache;
  check(cache.find(0u) == nullptr, "empty cache misses index 0");
  check(cache.find(7u) == nullptr, "empty cache misses a high index");
  check(cache.resolvedCount() == 0u, "empty cache resolved count is zero");
}

void testStoreThenFind() {
  Cache cache;
  const auto* stored = cache.store(3u, FakeEntry{42});
  check(stored != nullptr, "store returns the memoized entry");
  check(stored->handle == 42, "stored entry keeps its payload");
  const auto* found = cache.find(3u);
  check(found == stored, "find returns the same storage as store");
  check(cache.resolvedCount() == 1u, "one resolved entry after one store");
  // Sparse fill: storing index 3 must not fabricate entries 0..2.
  check(cache.find(0u) == nullptr, "index below a stored slot stays unresolved");
  check(cache.find(2u) == nullptr, "gap slot stays unresolved");
}

void testStoreDoesNotOverwrite() {
  Cache cache;
  const auto* first = cache.store(1u, FakeEntry{10});
  const auto* second = cache.store(1u, FakeEntry{20});
  check(first == second, "re-store returns the already-stored entry");
  check(second->handle == 10, "re-store does not overwrite the first entry");
  check(cache.resolvedCount() == 1u, "re-store does not double-count");
}

void testUncacheableIndexRefused() {
  Cache cache;
  check(!Cache::cacheable(Cache::kMaxCachedIndex),
        "the bound itself is not cacheable");
  check(Cache::cacheable(Cache::kMaxCachedIndex - 1u),
        "one below the bound is cacheable");
  check(cache.store(Cache::kMaxCachedIndex, FakeEntry{1}) == nullptr,
        "store refuses an index at the bound");
  check(cache.find(Cache::kMaxCachedIndex) == nullptr,
        "find refuses an index at the bound");
  check(cache.store(0xffffffffu, FakeEntry{1}) == nullptr,
        "store refuses a bogus index");
  check(cache.resolvedCount() == 0u, "refused stores resolve nothing");
}

void testReleaseAllVisitsEveryResolvedEntryOnce() {
  Cache cache;
  cache.store(0u, FakeEntry{100});
  cache.store(5u, FakeEntry{105});
  cache.store(2u, FakeEntry{102});
  check(cache.resolvedCount() == 3u, "three resolved entries");

  std::vector<int> released;
  cache.releaseAll([&](FakeEntry& entry) { released.push_back(entry.handle); });
  check(released.size() == 3u, "releaseAll visits every resolved entry once");
  bool sawAll = released.size() == 3u;
  for (int wanted : {100, 102, 105}) {
    bool found = false;
    for (int got : released) {
      found = found || got == wanted;
    }
    sawAll = sawAll && found;
  }
  check(sawAll, "releaseAll visits exactly the stored entries");
  check(cache.resolvedCount() == 0u, "releaseAll empties the cache");
  check(cache.find(0u) == nullptr, "releaseAll drops resolved slots");

  // The container destructor path calls releaseAll unconditionally; a second
  // call (or a call on an untouched cache) must not double-release.
  released.clear();
  cache.releaseAll([&](FakeEntry& entry) { released.push_back(entry.handle); });
  check(released.empty(), "releaseAll is idempotent");
}

void testReuseAfterRelease() {
  Cache cache;
  cache.store(4u, FakeEntry{7});
  cache.releaseAll([](FakeEntry&) {});
  const auto* again = cache.store(4u, FakeEntry{9});
  check(again != nullptr && again->handle == 9,
        "the cache is reusable after releaseAll");
  check(cache.resolvedCount() == 1u, "resolved count restarts after release");
}

}  // namespace

int main() {
  testEmptyLookupsMiss();
  testStoreThenFind();
  testStoreDoesNotOverwrite();
  testUncacheableIndexRefused();
  testReleaseAllVisitsEveryResolvedEntryOnce();
  testReuseAfterRelease();

  if (failures != 0) {
    std::cerr << failures << " level-surface-cache check(s) failed\n";
    return 1;
  }
  std::cout << "pe level surface cache spec ok\n";
  return 0;
}
