#pragma once

#include <cstdint>
#include <new>

enum class D3D9PeComCacheInsertStatus : std::uint8_t {
  Inserted,
  Existing,
  OutOfMemory,
  Failed,
};

template <typename Cache, typename Key, typename Interface>
D3D9PeComCacheInsertStatus D3D9PeCanonicalizeComCacheInsertion(
    Cache& cache, const Key& key, Interface* candidate,
    Interface** canonical) noexcept {
  if (canonical) *canonical = nullptr;
  if (!candidate || !canonical) return D3D9PeComCacheInsertStatus::Failed;
  candidate->AddRef();
  try {
    const auto [it, inserted] = cache.emplace(key, candidate);
    if (inserted) {
      *canonical = candidate;
      return D3D9PeComCacheInsertStatus::Inserted;
    }
    candidate->Release();
    candidate->Release();
    it->second->AddRef();
    *canonical = it->second;
    return D3D9PeComCacheInsertStatus::Existing;
  } catch (const std::bad_alloc&) {
    candidate->Release();
    candidate->Release();
    return D3D9PeComCacheInsertStatus::OutOfMemory;
  } catch (...) {
    candidate->Release();
    candidate->Release();
    return D3D9PeComCacheInsertStatus::Failed;
  }
}
