#include "d3d9_pe_retainer.hpp"

#include <cstdint>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <vector>

namespace {
std::atomic<bool> gCountAllocations{false};
std::atomic<std::size_t> gAllocationCount{0u};
}

void* operator new(std::size_t size) {
  if (void* value = std::malloc(size ? size : 1u)) {
    if (gCountAllocations.load(std::memory_order_relaxed)) {
      gAllocationCount.fetch_add(1u, std::memory_order_relaxed);
    }
    return value;
  }
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

struct RefCounter {
  std::uint32_t refs = 1;
};

struct D9CSurface : RefCounter {};
struct D9CTexture : RefCounter {};
struct D9CBuffer : RefCounter {};
struct D9CShader : RefCounter {};
struct D9CVertexDecl : RefCounter {};
struct D9CQuery : RefCounter {};

template<typename T>
void addRef(T* value) {
  ++value->refs;
}

template<typename T>
std::uint32_t release(T* value) {
  return --value->refs;
}

extern "C" void dxmt9c_surface_addref(D9CSurface* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_surface_release(D9CSurface* value) {
  return release(value);
}
extern "C" void dxmt9c_texture_addref(D9CTexture* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_texture_release(D9CTexture* value) {
  return release(value);
}
extern "C" void dxmt9c_buffer_addref(D9CBuffer* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_buffer_release(D9CBuffer* value) {
  return release(value);
}
extern "C" void dxmt9c_shader_addref(D9CShader* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_shader_release(D9CShader* value) {
  return release(value);
}
extern "C" void dxmt9c_vdecl_addref(D9CVertexDecl* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_vdecl_release(D9CVertexDecl* value) {
  return release(value);
}
extern "C" void dxmt9c_query_addref(D9CQuery* value) { addRef(value); }
extern "C" std::uint32_t dxmt9c_query_release(D9CQuery* value) {
  return release(value);
}

namespace {

bool check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "pe_retainer_spec failed: " << message << '\n';
  }
  return condition;
}

}  // namespace

int main() {
  D3D9PePendingCommandRetainer retainer;
  D9CQuery first{};
  D9CQuery second{};

  auto firstAcquire = retainer.beginAcquire();
  retainer.retainQuery(&first, firstAcquire);
  retainer.retainQuery(&first, firstAcquire);
  if (!check(first.refs == 2u, "query is addref'd exactly once") ||
      !check(retainer.size() == 1u, "query occupies one flat-set entry")) {
    return 1;
  }

  auto rollbackAcquire = retainer.beginAcquire();
  retainer.retainQuery(&first, rollbackAcquire);
  retainer.retainQuery(&second, rollbackAcquire);
  if (!check(first.refs == 2u, "existing query is not re-retained") ||
      !check(second.refs == 2u, "new query is retained")) {
    return 1;
  }
  retainer.rollback(rollbackAcquire);
  if (!check(first.refs == 2u, "rollback preserves pre-checkpoint query") ||
      !check(second.refs == 1u, "rollback releases new query") ||
      !check(retainer.size() == 1u, "rollback restores flat arena checkpoint")) {
    return 1;
  }

  auto objectAcquire = retainer.beginAcquire();
  retainer.retainWireObject(D9C_CHUNK_HANDLE_KIND_QUERY, &first,
                            objectAcquire);
  if (!check(first.refs == 2u, "typed canonical retain de-duplicates query")) {
    return 1;
  }

  retainer.clear();
  if (!check(first.refs == 1u, "clear releases retained query") ||
      !check(retainer.size() == 0u, "clear preserves an empty flat set")) {
    return 1;
  }

  // --- Cross-epoch pin aggregation -------------------------------------
  // A chunk boundary is endEpoch(); a discard is clear(). The point of the
  // former is that an object named by consecutive chunks is addref'd once, not
  // once per chunk, so the wire pair never happens.
  {
    D3D9PePendingCommandRetainer warm;
    D9CBuffer hot{};
    D9CBuffer cold{};

    for (int chunk = 0; chunk < 5; ++chunk) {
      auto acquire = warm.beginAcquire();
      warm.retainBuffer(&hot, acquire);
      if (chunk == 0) {
        warm.retainBuffer(&cold, acquire);
      }
      warm.endEpoch();
    }
    if (!check(hot.refs == 2u,
               "an object named every chunk is addref'd exactly once") ||
        !check(warm.size() == 1u,
               "the cold entry is evicted and the hot one is kept")) {
      return 1;
    }
    // cold was named only in epoch 0; kWarmEpochs == 1 keeps it through the
    // end of epoch 1 and releases it when epoch 2 closes.
    if (!check(cold.refs == 1u, "a cold entry releases its pin on eviction")) {
      return 1;
    }

    // Naming it again after eviction takes a fresh pin.
    auto reacquire = warm.beginAcquire();
    warm.retainBuffer(&cold, reacquire);
    if (!check(cold.refs == 2u, "an evicted object is re-pinned when named") ||
        !check(warm.size() == 2u, "re-pinning appends a new arena entry")) {
      return 1;
    }

    // A record that only dedupe-hits a warm entry must not release it on
    // rollback: the entry belongs to an earlier epoch, below the checkpoint.
    auto rollbackWarm = warm.beginAcquire();
    warm.retainBuffer(&hot, rollbackWarm);
    warm.rollback(rollbackWarm);
    if (!check(hot.refs == 2u,
               "rollback does not release a warm entry it only re-touched") ||
        !check(warm.size() == 2u, "rollback leaves the warm arena intact")) {
      return 1;
    }

    // Exact eviction boundary: touching an entry inside the warm window keeps
    // it, and one epoch past the window drops it.
    D9CBuffer boundary{};
    auto boundaryAcquire = warm.beginAcquire();
    warm.retainBuffer(&boundary, boundaryAcquire);
    warm.endEpoch();  // closes the epoch it was named in
    if (!check(boundary.refs == 2u, "an entry survives its own epoch close")) {
      return 1;
    }
    warm.endEpoch();  // one fully idle epoch, still inside kWarmEpochs
    if (!check(boundary.refs == 2u, "an entry survives one idle epoch")) {
      return 1;
    }
    warm.endEpoch();  // second idle epoch, now cold
    if (!check(boundary.refs == 1u,
               "an entry is released after kWarmEpochs + 1 idle epochs")) {
      return 1;
    }

    // Interleaved kinds share one arena and one epoch clock.
    D9CTexture texture{};
    D9CShader shader{};
    auto mixedAcquire = warm.beginAcquire();
    warm.retainWireObject(D9C_CHUNK_HANDLE_KIND_TEXTURE, &texture,
                          mixedAcquire);
    warm.retainWireObject(D9C_CHUNK_HANDLE_KIND_SHADER, &shader, mixedAcquire);
    warm.endEpoch();
    auto mixedAgain = warm.beginAcquire();
    warm.retainWireObject(D9C_CHUNK_HANDLE_KIND_TEXTURE, &texture, mixedAgain);
    warm.retainWireObject(D9C_CHUNK_HANDLE_KIND_SHADER, &shader, mixedAgain);
    warm.endEpoch();
    if (!check(texture.refs == 2u, "texture pin survives a chunk boundary") ||
        !check(shader.refs == 2u, "shader pin survives a chunk boundary")) {
      return 1;
    }

    // The three idle epochs above evicted `hot` and `cold` as well; re-pin
    // them so the discard assertions below are not vacuous.
    auto finalAcquire = warm.beginAcquire();
    warm.retainBuffer(&hot, finalAcquire);
    warm.retainBuffer(&cold, finalAcquire);
    if (!check(hot.refs == 2u && cold.refs == 2u,
               "both buffers are pinned again before the discard check")) {
      return 1;
    }

    // The discard path must release every warm pin, whatever its epoch — this
    // is what device teardown / Reset / ResetEx rely on so nothing is still
    // pinned when dxmt9c_device_reset* runs.
    warm.clear();
    if (!check(hot.refs == 1u, "clear releases a warm pin") ||
        !check(cold.refs == 1u, "clear releases a re-pinned entry") ||
        !check(texture.refs == 1u, "clear releases a warm texture pin") ||
        !check(shader.refs == 1u, "clear releases a warm shader pin") ||
        !check(warm.size() == 0u, "clear empties the warm arena")) {
      return 1;
    }
  }

  // --- R-BACK-43.7 RetentionIndex consistency pins ---------------------
  // retain()'s duplicate check moved from an O(n) std::find_if scan to a
  // (kind, ptr) -> index accelerator that must stay in lockstep with
  // entries_ across rollback, endEpoch compaction, and growth.

  // retain-after-rollback: the index must not resolve a tombstoned/removed
  // slot to a stale entries_ index, and a fresh retain of the same pointer
  // must dedupe correctly afterward.
  {
    D3D9PePendingCommandRetainer idx;
    D9CBuffer obj{};

    auto acquire = idx.beginAcquire();
    idx.retainBuffer(&obj, acquire);
    if (!check(obj.refs == 2u,
               "retain-after-rollback: initial retain addrefs once")) {
      return 1;
    }
    idx.rollback(acquire);
    if (!check(obj.refs == 1u,
               "retain-after-rollback: rollback releases the pin") ||
        !check(idx.size() == 0u,
               "retain-after-rollback: rollback empties the arena")) {
      return 1;
    }
    auto reacquire = idx.beginAcquire();
    idx.retainBuffer(&obj, reacquire);
    if (!check(obj.refs == 2u,
               "retain-after-rollback: re-retain addrefs fresh") ||
        !check(idx.size() == 1u,
               "retain-after-rollback: re-retain occupies one slot")) {
      return 1;
    }
    // A second retain in the same acquire must dedupe through the rebuilt
    // index, not double-addref via a stale erased entry.
    idx.retainBuffer(&obj, reacquire);
    if (!check(obj.refs == 2u,
               "retain-after-rollback: repeat retain dedupes through the "
               "reused slot") ||
        !check(idx.size() == 1u,
               "retain-after-rollback: dedupe does not grow the arena")) {
      return 1;
    }
    idx.clear();
    if (!check(obj.refs == 1u, "retain-after-rollback: clear releases the pin")) {
      return 1;
    }
  }

  // endEpoch compaction rebuild: when a middle entry is evicted, every
  // surviving entry's index shifts. The accelerator must be rebuilt so a
  // dedupe after compaction resolves to the correct (shifted) entry and
  // never falls back to a stale index into entries_.
  {
    D3D9PePendingCommandRetainer idx;
    D9CBuffer keepFirst{};
    D9CBuffer dropMiddle{};
    D9CBuffer keepLast{};

    auto a = idx.beginAcquire();
    idx.retainBuffer(&keepFirst, a);
    idx.retainBuffer(&dropMiddle, a);
    idx.retainBuffer(&keepLast, a);
    idx.endEpoch();  // closes the epoch all three were named in: all survive.

    auto b = idx.beginAcquire();
    idx.retainBuffer(&keepFirst, b);
    idx.retainBuffer(&keepLast, b);
    idx.endEpoch();  // dropMiddle idle 1 epoch: still inside kWarmEpochs.

    auto c = idx.beginAcquire();
    idx.retainBuffer(&keepFirst, c);
    idx.retainBuffer(&keepLast, c);
    idx.endEpoch();  // dropMiddle idle 2 epochs: evicted. keepLast's
                      // position in entries_ shifts down by one.

    if (!check(dropMiddle.refs == 1u,
               "endEpoch compaction: the cold middle entry is evicted") ||
        !check(keepFirst.refs == 2u && keepLast.refs == 2u,
               "endEpoch compaction: the warm entries around it survive") ||
        !check(idx.size() == 2u,
               "endEpoch compaction: the arena shrinks to the survivors")) {
      return 1;
    }

    // Dedupe against the shifted entry must hit the rebuilt index, not a
    // stale position (which after compaction could alias dropMiddle's old
    // slot or an out-of-range index).
    auto d = idx.beginAcquire();
    idx.retainBuffer(&keepLast, d);
    if (!check(keepLast.refs == 2u,
               "endEpoch compaction: dedupe after rebuild does not "
               "re-addref the shifted entry") ||
        !check(idx.size() == 2u,
               "endEpoch compaction: dedupe after rebuild does not grow "
               "the arena")) {
      return 1;
    }

    // A brand-new object retained right after the rebuild must still index
    // correctly (appended at the next free position, not colliding with a
    // stale slot the rebuild left behind).
    D9CBuffer freshAfterCompaction{};
    idx.retainBuffer(&freshAfterCompaction, d);
    if (!check(freshAfterCompaction.refs == 2u,
               "endEpoch compaction: a fresh retain after rebuild addrefs "
               "once") ||
        !check(idx.size() == 3u,
               "endEpoch compaction: a fresh retain after rebuild appends "
               "one entry")) {
      return 1;
    }

    idx.clear();
    if (!check(keepFirst.refs == 1u && keepLast.refs == 1u &&
                  freshAfterCompaction.refs == 1u,
               "endEpoch compaction: clear releases every surviving pin")) {
      return 1;
    }
  }

  // Overflow/growth: the accelerator grows (never falls back to a scan) once
  // the working set exceeds its initial capacity; dedupe must stay correct
  // at scale, both for the initial retain pass and for a second pass that
  // re-retains every object.
  {
    D3D9PePendingCommandRetainer idx;
    constexpr int kCount = 200;
    std::vector<std::unique_ptr<D9CBuffer>> buffers;
    buffers.reserve(kCount);

    auto acquire = idx.beginAcquire();
    for (int i = 0; i < kCount; ++i) {
      buffers.push_back(std::make_unique<D9CBuffer>());
      idx.retainBuffer(buffers.back().get(), acquire);
    }
    bool allAddreffedOnce = true;
    for (auto& b : buffers) {
      allAddreffedOnce = allAddreffedOnce && (b->refs == 2u);
    }
    if (!check(allAddreffedOnce,
               "index growth: every distinct buffer is addref'd exactly "
               "once") ||
        !check(idx.size() == static_cast<std::size_t>(kCount),
               "index growth: the arena holds every distinct entry")) {
      return 1;
    }

    // Re-retaining the whole set must dedupe every one of them through the
    // grown table, not just the entries present before the last grow.
    for (auto& b : buffers) {
      idx.retainBuffer(b.get(), acquire);
    }
    allAddreffedOnce = true;
    for (auto& b : buffers) {
      allAddreffedOnce = allAddreffedOnce && (b->refs == 2u);
    }
    if (!check(allAddreffedOnce,
               "index growth: re-retaining after growth still dedupes "
               "every entry") ||
        !check(idx.size() == static_cast<std::size_t>(kCount),
               "index growth: re-retaining after growth does not grow the "
               "arena")) {
      return 1;
    }

    idx.clear();
  }

  // A builder-admitted 256-handle working set must remain allocation-free on
  // the retry after Reset/discard.  In particular, RetentionIndex::clear()
  // must preserve its preallocated slots rather than rebuilding from zero.
  {
    constexpr std::size_t kCapacity = 256u;
    D3D9PePendingCommandRetainer idx(kCapacity);
    std::vector<std::unique_ptr<D9CBuffer>> buffers;
    buffers.reserve(kCapacity);
    for (std::size_t i = 0; i < kCapacity; ++i) {
      buffers.push_back(std::make_unique<D9CBuffer>());
    }
    auto first = idx.beginAcquire();
    for (auto& buffer : buffers) idx.retainBuffer(buffer.get(), first);
    idx.clear();

    gAllocationCount.store(0u, std::memory_order_relaxed);
    gCountAllocations.store(true, std::memory_order_release);
    auto retry = idx.beginAcquire();
    for (auto& buffer : buffers) idx.retainBuffer(buffer.get(), retry);
    gCountAllocations.store(false, std::memory_order_release);
    if (!check(gAllocationCount.load(std::memory_order_relaxed) == 0u,
               "max-capacity retry after clear performs no allocations") ||
        !check(idx.size() == kCapacity,
               "max-capacity retry retains every admitted handle")) {
      return 1;
    }
    idx.clear();
  }

  return 0;
}
