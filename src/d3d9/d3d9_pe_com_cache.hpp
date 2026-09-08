#pragma once

#include <cstdint>
#include <new>

enum class D3D9PeComCacheInsertStatus : std::uint8_t {
  Inserted,
  Existing,
  OutOfMemory,
  Failed,
};

struct D3D9PePublicComCacheCredit {
  template <typename Interface>
  static void acquire(Interface* object) noexcept { object->AddRef(); }
  template <typename Interface>
  static void release(Interface* object) noexcept { object->Release(); }
};

template <typename Credit = D3D9PePublicComCacheCredit,
          typename Cache, typename Key, typename Interface>
D3D9PeComCacheInsertStatus D3D9PeCanonicalizeComCacheInsertion(
    Cache& cache, const Key& key, Interface* candidate,
    Interface** canonical) noexcept {
  if (canonical) *canonical = nullptr;
  if (!candidate || !canonical) return D3D9PeComCacheInsertStatus::Failed;
  Credit::acquire(candidate);
  try {
    const auto [it, inserted] = cache.emplace(key, candidate);
    if (inserted) {
      *canonical = candidate;
      return D3D9PeComCacheInsertStatus::Inserted;
    }
    Credit::release(candidate);
    candidate->Release();
    it->second->AddRef();
    *canonical = it->second;
    return D3D9PeComCacheInsertStatus::Existing;
  } catch (const std::bad_alloc&) {
    Credit::release(candidate);
    candidate->Release();
    return D3D9PeComCacheInsertStatus::OutOfMemory;
  } catch (...) {
    Credit::release(candidate);
    candidate->Release();
    return D3D9PeComCacheInsertStatus::Failed;
  }
}
