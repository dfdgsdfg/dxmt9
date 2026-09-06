#include "d3d9_pe_retainer.hpp"

#include <cstdint>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <utility>
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

// The cross-kind address test treats its pointer as opaque at the C stub
// boundary. Never dereference a Buffer object through a fabricated Query type.
static RefCounter* aliasedCounter = nullptr;

template<typename T>
RefCounter& counter(T* value) {
  if (static_cast<void*>(value) == static_cast<void*>(aliasedCounter))
    return *aliasedCounter;
  return *value;
}

template<typename T>
void addRef(T* value) {
  ++counter(value).refs;
}

template<typename T>
std::uint32_t release(T* value) {
  return --counter(value).refs;
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

struct RetainTraceState {
  D9CBuffer a{}, b{};
  D9CQuery query{};
  D3D9PePendingCommandRetainer retainer{3u};
  D3D9PePendingCommandRetainer::Acquired checkpoint{};
  bool active = false;

  // Alphabet: A, B, Query, null, EndEpoch, Clear, Rollback. Starting a new
  // record commits the prior record; no checkpoint survives epoch compaction.
  bool step(unsigned op, std::size_t capacity, bool fused) {
    if (op >= 4u) {
      if (op == 6u && active) retainer.rollback(checkpoint);
      active = false;
      if (op == 4u) retainer.endEpoch();
      if (op == 5u) retainer.clear();
      return true;
    }
    checkpoint = retainer.beginAcquire();
    active = true;
    const auto kind = op == 2u ? D9C_CHUNK_HANDLE_KIND_QUERY
                               : D9C_CHUNK_HANDLE_KIND_BUFFER;
    void* object = op == 0u ? static_cast<void*>(&a)
                   : op == 1u ? static_cast<void*>(&b)
                   : op == 2u ? static_cast<void*>(&query) : nullptr;
    if (fused)
      return retainer.retainWireObject(kind, object, checkpoint, capacity);
    // The old owner returns before its capacity probe for a null ref.
    if (!object) return true;
    const bool warm = op == 2u
        ? retainer.containsQuery(&query)
        : retainer.containsBuffer(static_cast<D9CBuffer*>(object));
    return (warm || retainer.size() < capacity) &&
           retainer.retainWireObject(kind, object, checkpoint);
  }
};

bool boundedTracesMatchReference(std::size_t capacity) {
  // All length-four strings also cover every shorter prefix: 4 * 7^4 traces.
  for (unsigned sequence = 0u; sequence < 7u * 7u * 7u * 7u; ++sequence) {
    RetainTraceState reference, fused;
    auto remaining = sequence;
    for (unsigned depth = 0u; depth < 4u; ++depth) {
      const auto op = remaining % 7u;
      remaining /= 7u;
      const bool expected = reference.step(op, capacity, false);
      const bool actual = fused.step(op, capacity, true);
      if (expected != actual || reference.retainer.size() != fused.retainer.size() ||
          reference.a.refs != fused.a.refs || reference.b.refs != fused.b.refs ||
          reference.query.refs != fused.query.refs) return false;
    }
    reference.step(5u, capacity, false);
    fused.step(5u, capacity, true);
    if (reference.a.refs != 1u || reference.b.refs != 1u ||
        reference.query.refs != 1u || fused.a.refs != 1u ||
        fused.b.refs != 1u || fused.query.refs != 1u) return false;
  }
  return true;
}

bool sameAddressDifferentKindsAreIndependent() {
  D9CBuffer object{};
  aliasedCounter = &object;
  D3D9PePendingCommandRetainer retainer(2u);
  auto acquired = retainer.beginAcquire();
  const bool okay = retainer.retainBuffer(&object, acquired, 2u) &&
      retainer.retainQuery(reinterpret_cast<D9CQuery*>(&object), acquired, 2u) &&
      object.refs == 3u && retainer.size() == 2u;
  retainer.clear();
  aliasedCounter = nullptr;
  return okay && object.refs == 1u;
}

bool warmBoundaryAndTombstoneTrace() {
  struct TinyReference {
    enum : std::size_t { kCapacity = 4u };
    std::array<D9CBuffer*, kCapacity> objects{};
    std::array<std::uint64_t, kCapacity> touched{};
    std::size_t size = 0u;
    std::uint64_t epoch = 0u;

    std::size_t beginAcquire() const noexcept { return size; }

    bool retain(D9CBuffer* object) noexcept {
      if (!object) return true;
      for (std::size_t i = 0u; i < size; ++i) {
        if (objects[i] == object) {
          touched[i] = epoch;
          return true;
        }
      }
      if (size == kCapacity) return false;
      objects[size] = object;
      touched[size] = epoch;
      ++size;
      ++object->refs;
      return true;
    }

    void rollback(std::size_t checkpoint) noexcept {
      while (size > checkpoint) {
        --objects[size - 1u]->refs;
        --size;
      }
    }

    void endEpoch() noexcept {
      std::size_t write = 0u;
      for (std::size_t read = 0u; read < size; ++read) {
        if (epoch - touched[read] <= D3D9PePendingCommandRetainer::kWarmEpochs) {
          objects[write] = objects[read];
          touched[write] = touched[read];
          ++write;
        } else {
          --objects[read]->refs;
        }
      }
      size = write;
      ++epoch;
    }

    void clear() noexcept {
      while (size != 0u) {
        --objects[size - 1u]->refs;
        --size;
      }
    }
  };

  D9CBuffer actualA{};
  D9CBuffer actualB{};
  D9CBuffer actualC{};
  D9CBuffer actualD{};
  D9CBuffer referenceA{};
  D9CBuffer referenceB{};
  D9CBuffer referenceC{};
  D9CBuffer referenceD{};
  D3D9PePendingCommandRetainer actual(4u);
  TinyReference reference;
  auto equivalent = [&]() noexcept {
    return actual.size() == reference.size &&
           actualA.refs == referenceA.refs && actualB.refs == referenceB.refs &&
           actualC.refs == referenceC.refs && actualD.refs == referenceD.refs;
  };
  auto acquire = [&]() noexcept {
    return std::pair{actual.beginAcquire(), reference.beginAcquire()};
  };
  auto retain = [&](D9CBuffer* actualObject, D9CBuffer* referenceObject,
                    auto& checkpoints) noexcept {
    return actual.retainBuffer(actualObject, checkpoints.first) &&
           reference.retain(referenceObject);
  };
  auto settle = [&]() noexcept {
    actual.endEpoch();
    reference.endEpoch();
    return equivalent();
  };

  auto checkpoints = acquire();
  if (!retain(&actualA, &referenceA, checkpoints) ||
      !retain(&actualB, &referenceB, checkpoints) ||
      !retain(&actualC, &referenceC, checkpoints) || !equivalent())
    return false;
  // The production condition structurally skips rebuild here: no entry
  // moved or expired and used == occupied. Allocation counting is separate
  // evidence that this clean-boundary trace also stays within reserved space.
  gAllocationCount.store(0u, std::memory_order_relaxed);
  gCountAllocations.store(true, std::memory_order_release);
  const bool firstSettled = settle();
  gCountAllocations.store(false, std::memory_order_release);
  if (!firstSettled ||
      gAllocationCount.load(std::memory_order_relaxed) != 0u)
    return false;
  checkpoints = acquire();
  if (!retain(&actualA, &referenceA, checkpoints) ||
      !retain(&actualC, &referenceC, checkpoints) || !settle())
    return false;
  // The middle entry expires after its second idle boundary and forces
  // survivor movement; <= 1 keeps it alive through the preceding boundary.
  checkpoints = acquire();
  if (!retain(&actualA, &referenceA, checkpoints) ||
      !retain(&actualC, &referenceC, checkpoints) || !settle() ||
      actualB.refs != 1u || actual.size() != 2u)
    return false;

  checkpoints = acquire();
  if (!retain(&actualD, &referenceD, checkpoints) || !settle()) return false;
  checkpoints = acquire();
  if (!retain(&actualA, &referenceA, checkpoints) ||
      !retain(&actualC, &referenceC, checkpoints) || !settle())
    return false;
  // D is a tail entry, so its expiry does not move a survivor.
  checkpoints = acquire();
  if (!retain(&actualA, &referenceA, checkpoints) ||
      !retain(&actualC, &referenceC, checkpoints) || !settle() ||
      actualD.refs != 1u || actual.size() != 2u)
    return false;

  checkpoints = acquire();
  if (!retain(&actualD, &referenceD, checkpoints) ||
      !retain(&actualD, &referenceD, checkpoints) ||
      !actual.retainBuffer(nullptr, checkpoints.first) ||
      !reference.retain(nullptr) || actual.size() != reference.size ||
      actualD.refs != referenceD.refs)
    return false;
  actual.rollback(checkpoints.first);
  reference.rollback(checkpoints.second);
  if (!equivalent() || actualD.refs != 1u) return false;
  checkpoints = acquire();
  if (!retain(&actualD, &referenceD, checkpoints) || !equivalent())
    return false;
  actual.clear();
  reference.clear();
  return equivalent() && actualA.refs == 1u && actualB.refs == 1u &&
         actualC.refs == 1u && actualD.refs == 1u;
}

}  // namespace

int main() {
  for (std::size_t capacity = 0u; capacity <= 3u; ++capacity) {
    if (!check(boundedTracesMatchReference(capacity),
               "bounded fused trace matches contains-then-retain reference")) {
      return 1;
    }
  }
  if (!check(sameAddressDifferentKindsAreIndependent(),
             "one opaque address retains two independent kind-qualified pins"))
    return 1;
  if (!check(warmBoundaryAndTombstoneTrace(),
             "warm/tombstone reference preserves release and allocation bounds"))
    return 1;

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

  // The bounded owner path is allocation-free through its reserved ceiling,
  // including a warm duplicate and a novel denial at that ceiling.
  {
    constexpr std::size_t kCapacity = 3u;
    D3D9PePendingCommandRetainer idx(kCapacity);
    D9CBuffer first{}, second{}, third{}, denied{};
    auto acquire = idx.beginAcquire();
    idx.retainBuffer(&first, acquire, kCapacity);
    idx.retainBuffer(&second, acquire, kCapacity);
    idx.retainBuffer(&third, acquire, kCapacity);
    idx.clear();
    gAllocationCount.store(0u, std::memory_order_relaxed);
    gCountAllocations.store(true, std::memory_order_release);
    auto retry = idx.beginAcquire();
    const bool firstResult = idx.retainBuffer(&first, retry, kCapacity);
    const bool duplicateResult = idx.retainBuffer(&first, retry, kCapacity);
    const bool secondResult = idx.retainBuffer(&second, retry, kCapacity);
    const bool thirdResult = idx.retainBuffer(&third, retry, kCapacity);
    const bool deniedResult = idx.retainBuffer(&denied, retry, kCapacity);
    gCountAllocations.store(false, std::memory_order_release);
    if (!check(firstResult && duplicateResult && secondResult && thirdResult &&
                   !deniedResult,
               "bounded reserve accepts warm/novel entries and denies over-capacity") ||
        !check(gAllocationCount.load(std::memory_order_relaxed) == 0u,
               "bounded warm-set retry and cap denial perform no allocations") ||
        !check(first.refs == 2u && second.refs == 2u && third.refs == 2u &&
                   denied.refs == 1u,
               "bounded cap denial preserves exact AddRef multiplicity")) {
      return 1;
    }
    idx.rollback(retry);
    idx.clear();
  }

  return 0;
}
