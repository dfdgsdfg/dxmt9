#include "d3d9_pe_com_cache.hpp"

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
}  // namespace

int main() {
  try {
    testCanonicalDuplicateOwnership();
    testActualAllocatorFailureBalancesCandidate();
  } catch (const TestFailure& failure) {
    std::cerr << "pe_com_cache_spec failed: " << failure.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "pe_com_cache_spec passed\n";
  return EXIT_SUCCESS;
}
